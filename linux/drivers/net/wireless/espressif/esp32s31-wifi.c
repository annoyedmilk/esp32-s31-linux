// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Marco Müller <hello@annoyedmilk.ch>
 *
 * Full-MAC cfg80211 device for the ESP32-S31 WLAN modem, which is owned by
 * ESP-IDF firmware resident on hart 0.  The firmware runs 802.11 and its own
 * supplicant; Linux exchanges 802.3 frames, scan requests and association
 * with it through fixed-size slot rings in internal SRAM and a pair of
 * cross-core doorbell interrupts.
 *
 * The rings are reached without the data cache on either hart, so the io
 * accessors here are for their ordering barriers rather than for a device.
 */

#include <linux/etherdevice.h>
#include <linux/hex.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <net/cfg80211.h>

#include "esp32s31-wifi-ipc.h"

#define ESP32S31_WIFI_NAPI_BUDGET	16
#define ESP32S31_WIFI_DOORBELL_RX	0x0
#define ESP32S31_WIFI_DOORBELL_TX	0x4

struct esp32s31_wifi {
	struct net_device *ndev;
	struct napi_struct napi;
	struct esp32s31_ipc __iomem *ipc;
	void __iomem *doorbell;

	struct wiphy *wiphy;
	struct wireless_dev wdev;
	/* Scan results arrive from an interrupt; cfg80211 wants process
	 * context, so the doorbell only schedules this.
	 */
	struct work_struct scan_work;
	struct cfg80211_scan_request *scan_req;
	struct mutex scan_lock;
	u32 scan_seq;
	bool connected;
	u8 bssid[ETH_ALEN];
};

/* 2.4 GHz only: the modem has no 5 GHz radio. */
static struct ieee80211_channel esp32s31_wifi_channels[] = {
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2412, .hw_value = 1 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2417, .hw_value = 2 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2422, .hw_value = 3 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2427, .hw_value = 4 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2432, .hw_value = 5 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2437, .hw_value = 6 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2442, .hw_value = 7 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2447, .hw_value = 8 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2452, .hw_value = 9 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2457, .hw_value = 10 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2462, .hw_value = 11 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2467, .hw_value = 12 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2472, .hw_value = 13 },
};

static struct ieee80211_rate esp32s31_wifi_rates[] = {
	{ .bitrate = 10, .hw_value = 0 },
	{ .bitrate = 20, .hw_value = 1 },
	{ .bitrate = 55, .hw_value = 2 },
	{ .bitrate = 110, .hw_value = 3 },
	{ .bitrate = 60, .hw_value = 4 },
	{ .bitrate = 120, .hw_value = 5 },
	{ .bitrate = 240, .hw_value = 6 },
	{ .bitrate = 540, .hw_value = 7 },
};

/* What the firmware's supplicant negotiates. */
static const u32 esp32s31_wifi_ciphers[] = {
	WLAN_CIPHER_SUITE_CCMP,
	WLAN_CIPHER_SUITE_TKIP,
	WLAN_CIPHER_SUITE_AES_CMAC,
};

static struct ieee80211_supported_band esp32s31_wifi_band_2ghz = {
	.band = NL80211_BAND_2GHZ,
	.channels = esp32s31_wifi_channels,
	.n_channels = ARRAY_SIZE(esp32s31_wifi_channels),
	.bitrates = esp32s31_wifi_rates,
	.n_bitrates = ARRAY_SIZE(esp32s31_wifi_rates),
};

static struct esp32s31_wifi *esp32s31_wifi_priv(struct net_device *ndev)
{
	return *(struct esp32s31_wifi **)netdev_priv(ndev);
}

static struct esp32s31_ipc_slot __iomem *
esp32s31_wifi_slot(struct esp32s31_ipc_ring __iomem *ring, u32 index)
{
	return &ring->slot[index % ESP32S31_IPC_SLOTS];
}

static bool esp32s31_wifi_tx_full(struct esp32s31_ipc_ring __iomem *ring)
{
	return ioread32(&ring->head) - ioread32(&ring->tail) >=
	       ESP32S31_IPC_SLOTS;
}

static void esp32s31_wifi_rx_one(struct esp32s31_wifi *priv,
				 struct esp32s31_ipc_slot __iomem *slot,
				 u32 len)
{
	struct net_device *ndev = priv->ndev;
	struct sk_buff *skb;

	if (len < ETH_HLEN || len > ESP32S31_IPC_SLOT_DATA) {
		ndev->stats.rx_length_errors++;
		return;
	}

	skb = napi_alloc_skb(&priv->napi, len);
	if (!skb) {
		ndev->stats.rx_dropped++;
		return;
	}

	memcpy_fromio(skb_put(skb, len), slot->data, len);
	skb->protocol = eth_type_trans(skb, ndev);
	ndev->stats.rx_packets++;
	ndev->stats.rx_bytes += len;
	napi_gro_receive(&priv->napi, skb);
}

/*
 * The firmware associates by itself, so the BSS it chose may never have been
 * scanned.  cfg80211 refuses a connection it cannot tie to a BSS, so publish
 * the one the firmware reports before claiming success.
 */
static void esp32s31_wifi_inform_link(struct esp32s31_wifi *priv)
{
	struct ieee80211_channel *chan;
	struct cfg80211_bss *bss;
	u8 ie[2 + ESP32S31_IPC_SSID_MAX];
	u8 ssid[ESP32S31_IPC_SSID_MAX];
	size_t ssid_len;
	int freq;

	memcpy_fromio(priv->bssid, priv->ipc->bssid, ETH_ALEN);
	freq = ieee80211_channel_to_frequency(ioread8(&priv->ipc->channel),
					      NL80211_BAND_2GHZ);
	chan = ieee80211_get_channel(priv->wiphy, freq);
	if (!chan)
		return;

	memcpy_fromio(ssid, priv->ipc->cmd.ssid, sizeof(ssid));
	ssid_len = strnlen((const char *)ssid, sizeof(ssid));

	ie[0] = WLAN_EID_SSID;
	ie[1] = ssid_len;
	memcpy(&ie[2], ssid, ssid_len);

	bss = cfg80211_inform_bss(priv->wiphy, chan, CFG80211_BSS_FTYPE_UNKNOWN,
				  priv->bssid, 0, WLAN_CAPABILITY_ESS, 100,
				  ie, 2 + ssid_len, 0, GFP_ATOMIC);
	if (bss)
		cfg80211_put_bss(priv->wiphy, bss);
}

static void esp32s31_wifi_sync_carrier(struct esp32s31_wifi *priv)
{
	bool up = ioread32(&priv->ipc->link_up);

	if (up == netif_carrier_ok(priv->ndev))
		return;

	if (up) {
		netif_carrier_on(priv->ndev);
		if (!priv->connected) {
			esp32s31_wifi_inform_link(priv);
			cfg80211_connect_result(priv->ndev, priv->bssid, NULL,
						0, NULL, 0,
						WLAN_STATUS_SUCCESS,
						GFP_ATOMIC);
			priv->connected = true;
		}
	} else {
		netif_carrier_off(priv->ndev);
		if (priv->connected) {
			cfg80211_disconnected(priv->ndev, 0, NULL, 0, true,
					      GFP_ATOMIC);
			priv->connected = false;
		}
	}
}

/* The firmware publishes seq after the table, so a change means it is whole. */
static void esp32s31_wifi_check_scan(struct esp32s31_wifi *priv)
{
	if (!priv->scan_req)
		return;

	if (ioread32(&priv->ipc->scan.seq) == priv->scan_seq)
		return;

	schedule_work(&priv->scan_work);
}

static int esp32s31_wifi_poll(struct napi_struct *napi, int budget)
{
	struct esp32s31_wifi *priv = container_of(napi, struct esp32s31_wifi,
						  napi);
	struct esp32s31_ipc_ring __iomem *ring = &priv->ipc->to_linux;
	int done = 0;

	esp32s31_wifi_sync_carrier(priv);

	while (done < budget) {
		u32 tail = ioread32(&ring->tail);

		if (tail == ioread32(&ring->head))
			break;

		esp32s31_wifi_rx_one(priv, esp32s31_wifi_slot(ring, tail),
				     ioread32(&esp32s31_wifi_slot(ring, tail)->len));
		iowrite32(tail + 1, &ring->tail);
		done++;
	}

	if (netif_queue_stopped(priv->ndev) &&
	    !esp32s31_wifi_tx_full(&priv->ipc->to_firmware))
		netif_wake_queue(priv->ndev);

	if (done < budget)
		napi_complete_done(napi, done);

	return done;
}

static irqreturn_t esp32s31_wifi_irq(int irq, void *dev_id)
{
	struct esp32s31_wifi *priv = dev_id;

	iowrite32(0, priv->doorbell + ESP32S31_WIFI_DOORBELL_RX);
	esp32s31_wifi_check_scan(priv);
	napi_schedule(&priv->napi);

	return IRQ_HANDLED;
}

static netdev_tx_t esp32s31_wifi_xmit(struct sk_buff *skb,
				      struct net_device *ndev)
{
	struct esp32s31_wifi *priv = esp32s31_wifi_priv(ndev);
	struct esp32s31_ipc_ring __iomem *ring = &priv->ipc->to_firmware;
	struct esp32s31_ipc_slot __iomem *slot;
	u32 head = ioread32(&ring->head);
	unsigned int len = skb->len;

	if (len > ESP32S31_IPC_SLOT_DATA) {
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	if (esp32s31_wifi_tx_full(ring)) {
		netif_stop_queue(ndev);
		return NETDEV_TX_BUSY;
	}

	slot = esp32s31_wifi_slot(ring, head);
	memcpy_toio(slot->data, skb->data, len);
	iowrite32(len, &slot->len);
	iowrite32(head + 1, &ring->head);
	iowrite32(1, priv->doorbell + ESP32S31_WIFI_DOORBELL_TX);

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += len;
	dev_kfree_skb_any(skb);

	/*
	 * The firmware rings the doorbell back as it drains, which reopens
	 * the queue from the poll loop.  Re-check after stopping in case that
	 * drain won the race, since an empty ring produces no more doorbells.
	 */
	if (esp32s31_wifi_tx_full(ring)) {
		netif_stop_queue(ndev);
		if (!esp32s31_wifi_tx_full(ring))
			netif_wake_queue(ndev);
	}

	return NETDEV_TX_OK;
}

static int esp32s31_wifi_open(struct net_device *ndev)
{
	struct esp32s31_wifi *priv = esp32s31_wifi_priv(ndev);

	napi_enable(&priv->napi);
	esp32s31_wifi_sync_carrier(priv);
	netif_start_queue(ndev);

	return 0;
}

static int esp32s31_wifi_stop(struct net_device *ndev)
{
	struct esp32s31_wifi *priv = esp32s31_wifi_priv(ndev);

	netif_stop_queue(ndev);
	netif_carrier_off(ndev);
	napi_disable(&priv->napi);

	return 0;
}

/*
 * The firmware runs the supplicant, so association is driven by handing it
 * credentials rather than through cfg80211: there is no 802.11 state here for
 * nl80211 to describe.  Both credential attributes are write-only.
 */
static ssize_t esp32s31_wifi_store_text(void __iomem *dst, size_t dst_len,
					const char *buf, size_t count)
{
	size_t len = strnlen(buf, count);

	while (len && buf[len - 1] == '\n')
		len--;

	if (len >= dst_len)
		return -EINVAL;

	memset_io(dst, 0, dst_len);
	memcpy_toio(dst, buf, len);

	return count;
}

/*
 * The one key the offloads cannot carry: a passphrase, for a supplicant that
 * wants one rather than the PMK nl80211 hands over.  Association itself goes
 * through cfg80211.
 */
static ssize_t psk_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct esp32s31_wifi *priv = dev_get_drvdata(dev);

	return esp32s31_wifi_store_text(priv->ipc->cmd.psk,
					ESP32S31_IPC_PSK_MAX, buf, count);
}
static DEVICE_ATTR_WO(psk);

static struct attribute *esp32s31_wifi_attrs[] = {
	&dev_attr_psk.attr,
	NULL,
};
ATTRIBUTE_GROUPS(esp32s31_wifi);

static void esp32s31_wifi_send_cmd(struct esp32s31_wifi *priv, u32 code)
{
	/* The code is written last: it is what the firmware polls on. */
	wmb();
	iowrite32(code, &priv->ipc->cmd.code);
	iowrite32(1, priv->doorbell + ESP32S31_WIFI_DOORBELL_TX);
}

static void esp32s31_wifi_scan_work(struct work_struct *work)
{
	struct esp32s31_wifi *priv = container_of(work, struct esp32s31_wifi,
						  scan_work);
	struct cfg80211_scan_info info = { };
	struct cfg80211_scan_request *req;
	struct esp32s31_ipc_bss bss;
	u32 count, i;

	mutex_lock(&priv->scan_lock);
	req = priv->scan_req;
	priv->scan_req = NULL;
	mutex_unlock(&priv->scan_lock);

	if (!req)
		return;

	count = ioread32(&priv->ipc->scan.count);
	if (count > ESP32S31_IPC_SCAN_MAX)
		count = ESP32S31_IPC_SCAN_MAX;

	for (i = 0; i < count; i++) {
		struct ieee80211_channel *chan;
		struct cfg80211_bss *found;
		u8 ie[2 + ESP32S31_IPC_SSID_MAX];
		u16 caps = WLAN_CAPABILITY_ESS;
		int freq;

		memcpy_fromio(&bss, &priv->ipc->scan.bss[i], sizeof(bss));
		if (bss.ssid_len > ESP32S31_IPC_SSID_MAX)
			continue;

		freq = ieee80211_channel_to_frequency(bss.channel,
						      NL80211_BAND_2GHZ);
		chan = ieee80211_get_channel(priv->wiphy, freq);
		if (!chan)
			continue;

		if (bss.authmode != ESP32S31_IPC_AUTH_OPEN)
			caps |= WLAN_CAPABILITY_PRIVACY;

		/* cfg80211 builds the BSS from the one element we can
		 * honestly supply; the firmware keeps the rest to itself.
		 */
		ie[0] = WLAN_EID_SSID;
		ie[1] = bss.ssid_len;
		memcpy(&ie[2], bss.ssid, bss.ssid_len);

		found = cfg80211_inform_bss(priv->wiphy, chan,
					    CFG80211_BSS_FTYPE_UNKNOWN,
					    bss.bssid, 0, caps, 100,
					    ie, 2 + bss.ssid_len,
					    DBM_TO_MBM(bss.rssi), GFP_KERNEL);
		if (found)
			cfg80211_put_bss(priv->wiphy, found);
	}

	cfg80211_scan_done(req, &info);
}

static int esp32s31_wifi_scan(struct wiphy *wiphy,
			      struct cfg80211_scan_request *request)
{
	struct esp32s31_wifi *priv = wiphy_priv(wiphy);

	mutex_lock(&priv->scan_lock);
	if (priv->scan_req) {
		mutex_unlock(&priv->scan_lock);
		return -EBUSY;
	}
	priv->scan_req = request;
	priv->scan_seq = ioread32(&priv->ipc->scan.seq);
	mutex_unlock(&priv->scan_lock);

	esp32s31_wifi_send_cmd(priv, ESP32S31_IPC_CMD_SCAN);

	return 0;
}

/*
 * The firmware's supplicant takes one string for every key type: a
 * passphrase, or 64 hex characters it reads as the PMK itself, which is what
 * the 4-way handshake offload hands us.  SAE needs the password, which the
 * SAE offload carries.
 */
static int esp32s31_wifi_set_key(struct esp32s31_wifi *priv,
				 struct cfg80211_connect_params *sme)
{
	char hex[2 * WLAN_PMK_LEN + 1];

	if (sme->crypto.psk) {
		memset_io(priv->ipc->cmd.psk, 0, ESP32S31_IPC_PSK_MAX);
		bin2hex(hex, sme->crypto.psk, WLAN_PMK_LEN);
		memcpy_toio(priv->ipc->cmd.psk, hex, 2 * WLAN_PMK_LEN);
		return 0;
	}

	if (sme->crypto.sae_pwd) {
		if (sme->crypto.sae_pwd_len > ESP32S31_IPC_PSK_MAX)
			return -EINVAL;
		memset_io(priv->ipc->cmd.psk, 0, ESP32S31_IPC_PSK_MAX);
		memcpy_toio(priv->ipc->cmd.psk, sme->crypto.sae_pwd,
			    sme->crypto.sae_pwd_len);
		return 0;
	}

	/*
	 * Neither offload carried a key, so this association is driven through
	 * the psk attribute, the only way to reach a supplicant that wants a
	 * passphrase rather than a PMK.  Whatever was written there stands:
	 * nl80211 never says a network is open, only that it has no key for
	 * us, and clearing on that wipes the passphrase a moment before the
	 * firmware needs it.
	 */
	return 0;
}

static int esp32s31_wifi_connect(struct wiphy *wiphy, struct net_device *ndev,
				 struct cfg80211_connect_params *sme)
{
	struct esp32s31_wifi *priv = wiphy_priv(wiphy);
	int ret;

	if (!sme->ssid_len || sme->ssid_len > ESP32S31_IPC_SSID_MAX)
		return -EINVAL;

	ret = esp32s31_wifi_set_key(priv, sme);
	if (ret && ret != -ENOKEY)
		return ret;

	memset_io(priv->ipc->cmd.ssid, 0, ESP32S31_IPC_SSID_MAX);
	memcpy_toio(priv->ipc->cmd.ssid, sme->ssid, sme->ssid_len);

	esp32s31_wifi_send_cmd(priv, ESP32S31_IPC_CMD_CONNECT);

	return 0;
}

static int esp32s31_wifi_disconnect(struct wiphy *wiphy,
				    struct net_device *ndev, u16 reason_code)
{
	struct esp32s31_wifi *priv = wiphy_priv(wiphy);

	esp32s31_wifi_send_cmd(priv, ESP32S31_IPC_CMD_DISCONNECT);

	return 0;
}

/* Without this "iw link" reports the association and an error beside it. */
static int esp32s31_wifi_get_station(struct wiphy *wiphy,
				     struct wireless_dev *wdev, const u8 *mac,
				     struct station_info *sinfo)
{
	struct esp32s31_wifi *priv = wiphy_priv(wiphy);
	struct net_device *ndev = priv->ndev;

	if (!priv->connected || !ether_addr_equal(mac, priv->bssid))
		return -ENOENT;

	sinfo->filled = BIT_ULL(NL80211_STA_INFO_TX_BYTES) |
			BIT_ULL(NL80211_STA_INFO_RX_BYTES) |
			BIT_ULL(NL80211_STA_INFO_TX_PACKETS) |
			BIT_ULL(NL80211_STA_INFO_RX_PACKETS);
	sinfo->tx_bytes = ndev->stats.tx_bytes;
	sinfo->rx_bytes = ndev->stats.rx_bytes;
	sinfo->tx_packets = ndev->stats.tx_packets;
	sinfo->rx_packets = ndev->stats.rx_packets;

	return 0;
}

static const struct cfg80211_ops esp32s31_wifi_cfg80211_ops = {
	.scan = esp32s31_wifi_scan,
	.get_station = esp32s31_wifi_get_station,
	.connect = esp32s31_wifi_connect,
	.disconnect = esp32s31_wifi_disconnect,
};

static const struct net_device_ops esp32s31_wifi_netdev_ops = {
	.ndo_open = esp32s31_wifi_open,
	.ndo_stop = esp32s31_wifi_stop,
	.ndo_start_xmit = esp32s31_wifi_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};

static int esp32s31_wifi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct esp32s31_wifi *priv;
	struct net_device *ndev;
	struct wiphy *wiphy;
	u8 mac[ETH_ALEN];
	int irq, ret;

	wiphy = wiphy_new(&esp32s31_wifi_cfg80211_ops, sizeof(*priv));
	if (!wiphy)
		return -ENOMEM;

	set_wiphy_dev(wiphy, dev);
	wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	wiphy->bands[NL80211_BAND_2GHZ] = &esp32s31_wifi_band_2ghz;
	wiphy->max_scan_ssids = 1;
	wiphy->max_scan_ie_len = 0;
	wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	/* The firmware holds the keys and runs the handshakes. */
	wiphy_ext_feature_set(wiphy,
			      NL80211_EXT_FEATURE_4WAY_HANDSHAKE_STA_PSK);
	wiphy_ext_feature_set(wiphy, NL80211_EXT_FEATURE_SAE_OFFLOAD);
	wiphy->cipher_suites = esp32s31_wifi_ciphers;
	wiphy->n_cipher_suites = ARRAY_SIZE(esp32s31_wifi_ciphers);

	priv = wiphy_priv(wiphy);
	priv->wiphy = wiphy;
	mutex_init(&priv->scan_lock);
	INIT_WORK(&priv->scan_work, esp32s31_wifi_scan_work);
	platform_set_drvdata(pdev, priv);

	ndev = alloc_netdev(sizeof(struct esp32s31_wifi *), "wlan%d",
			    NET_NAME_ENUM, ether_setup);
	if (!ndev) {
		ret = -ENOMEM;
		goto err_wiphy;
	}

	SET_NETDEV_DEV(ndev, dev);
	*(struct esp32s31_wifi **)netdev_priv(ndev) = priv;
	priv->ndev = ndev;

	priv->wdev.wiphy = wiphy;
	priv->wdev.iftype = NL80211_IFTYPE_STATION;
	priv->wdev.netdev = ndev;
	ndev->ieee80211_ptr = &priv->wdev;

	priv->ipc = devm_platform_ioremap_resource_byname(pdev, "ipc");
	if (IS_ERR(priv->ipc)) {
		ret = PTR_ERR(priv->ipc);
		goto err_netdev;
	}

	priv->doorbell = devm_platform_ioremap_resource_byname(pdev, "doorbell");
	if (IS_ERR(priv->doorbell)) {
		ret = PTR_ERR(priv->doorbell);
		goto err_netdev;
	}

	if (ioread32(&priv->ipc->magic) != ESP32S31_IPC_MAGIC) {
		ret = dev_err_probe(dev, -ENODEV, "no firmware ring at %p\n",
				    priv->ipc);
		goto err_netdev;
	}

	if (ioread32(&priv->ipc->version) != ESP32S31_IPC_VERSION) {
		ret = dev_err_probe(dev, -EPROTO, "ring version %u != %u\n",
				    ioread32(&priv->ipc->version),
				    ESP32S31_IPC_VERSION);
		goto err_netdev;
	}

	memcpy_fromio(mac, priv->ipc->mac, ETH_ALEN);
	eth_hw_addr_set(ndev, mac);
	memcpy(wiphy->perm_addr, mac, ETH_ALEN);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto err_netdev;
	}

	ndev->netdev_ops = &esp32s31_wifi_netdev_ops;
	ndev->max_mtu = ESP32S31_IPC_SLOT_DATA - ETH_HLEN;
	netif_napi_add(ndev, &priv->napi, esp32s31_wifi_poll);
	netif_carrier_off(ndev);

	ret = wiphy_register(wiphy);
	if (ret)
		goto err_netdev;

	ret = devm_request_irq(dev, irq, esp32s31_wifi_irq, 0,
			       dev_name(dev), priv);
	if (ret)
		goto err_register;

	ret = register_netdev(ndev);
	if (ret)
		goto err_register;

	dev_info(dev, "%s: firmware ring v%u, %pM\n", ndev->name,
		 ESP32S31_IPC_VERSION, ndev->dev_addr);

	return 0;

err_register:
	wiphy_unregister(wiphy);
err_netdev:
	free_netdev(ndev);
err_wiphy:
	wiphy_free(wiphy);

	return ret;
}

static void esp32s31_wifi_remove(struct platform_device *pdev)
{
	struct esp32s31_wifi *priv = platform_get_drvdata(pdev);

	cancel_work_sync(&priv->scan_work);
	unregister_netdev(priv->ndev);
	wiphy_unregister(priv->wiphy);
	free_netdev(priv->ndev);
	wiphy_free(priv->wiphy);
}

static const struct of_device_id esp32s31_wifi_of_match[] = {
	{ .compatible = "esp,esp32s31-wifi" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_wifi_of_match);

static struct platform_driver esp32s31_wifi_driver = {
	.probe = esp32s31_wifi_probe,
	.remove = esp32s31_wifi_remove,
	.driver = {
		.name = "esp32s31-wifi",
		.of_match_table = esp32s31_wifi_of_match,
		.dev_groups = esp32s31_wifi_groups,
	},
};
module_platform_driver(esp32s31_wifi_driver);

MODULE_DESCRIPTION("ESP32-S31 WLAN modem shared-memory network device");
MODULE_AUTHOR("Marco Müller <hello@annoyedmilk.ch>");
MODULE_LICENSE("GPL");
