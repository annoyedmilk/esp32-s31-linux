// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Marco Müller <hello@annoyedmilk.ch>
 *
 * Full-MAC cfg80211 device for the ESP32-S31 WLAN modem.  The ESP-IDF
 * firmware on hart 0 controls the modem.  It runs 802.11 and its own
 * supplicant.  Linux sends 802.3 frames, scan requests and association
 * requests through rings of fixed-size slots in internal SRAM, with two
 * cross-core doorbell interrupts.  A supplicant on Linux gives the key
 * through the 4-way handshake offload (a PMK) or the SAE offload (the
 * password).
 *
 * The two harts access the rings without the data cache.  The io accessors
 * are here for their ordering barriers, not for a device.
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
/* Offsets from FROM_CPU_1: Linux sets TX, the firmware sets RX. */
#define ESP32S31_WIFI_DOORBELL_TX	0x0
#define ESP32S31_WIFI_DOORBELL_RX	0x4

struct esp32s31_wifi {
	struct net_device *ndev;
	struct napi_struct napi;
	struct esp32s31_ipc __iomem *ipc;
	void __iomem *doorbell;

	struct wiphy *wiphy;
	struct wireless_dev wdev;
	/* Scan results come with an interrupt, but cfg80211 needs process
	 * context.  Thus the doorbell only schedules this work.
	 */
	struct work_struct scan_work;
	struct cfg80211_scan_request *scan_req;
	struct mutex scan_lock;
	u32 scan_seq;
	bool connected;
	/* A connect request that has no result yet. */
	bool connecting;
	/* The firmware does the 4-way handshake with a key from cfg80211. */
	bool key_offload;
	u32 fail_seq;
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

/* The ciphers that the firmware supplicant uses. */
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
 * The firmware selects the BSS itself, so Linux possibly did not scan it.
 * cfg80211 does not accept a connection without a BSS.  Thus publish the BSS
 * from the firmware before the connection result.
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
			/*
			 * The firmware did the handshake, so the supplicant
			 * must not wait for EAPOL frames.
			 */
			if (priv->key_offload)
				cfg80211_port_authorized(priv->ndev,
							 priv->bssid, NULL, 0,
							 GFP_ATOMIC);
			priv->connected = true;
			priv->connecting = false;
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

/*
 * The firmware gives up after some attempts that did not associate.  Give
 * the result to cfg80211, so that the supplicant does not wait.
 */
static void esp32s31_wifi_check_fail(struct esp32s31_wifi *priv)
{
	u32 seq = ioread32(&priv->ipc->fail_seq);
	u16 reason;

	if (seq == priv->fail_seq)
		return;
	priv->fail_seq = seq;
	if (!priv->connecting || priv->connected)
		return;
	priv->connecting = false;

	reason = ioread16(&priv->ipc->fail_reason);
	netdev_info(priv->ndev, "connection failed, reason %u\n", reason);
	/* ESP-IDF reasons 201 and 211: no AP with this SSID was found. */
	if (reason == 201 || reason == 211)
		cfg80211_connect_timeout(priv->ndev, NULL, NULL, 0, GFP_ATOMIC,
					 NL80211_TIMEOUT_SCAN);
	else
		cfg80211_connect_result(priv->ndev, NULL, NULL, 0, NULL, 0,
					WLAN_STATUS_UNSPECIFIED_FAILURE,
					GFP_ATOMIC);
}

/* The firmware writes seq after the table.  A new seq means a full table. */
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
	esp32s31_wifi_check_fail(priv);

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
	 * The firmware rings the doorbell when it empties slots, and the poll
	 * loop then starts the queue again.  Check again after the stop: the
	 * firmware can empty the ring first, and then no doorbell comes.
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

/*
 * cfg80211 frees a pending scan request when the interface goes down, so
 * finish it here first.
 */
static void esp32s31_wifi_abort_scan(struct esp32s31_wifi *priv)
{
	struct cfg80211_scan_info info = { .aborted = true };
	struct cfg80211_scan_request *req;

	cancel_work_sync(&priv->scan_work);

	mutex_lock(&priv->scan_lock);
	req = priv->scan_req;
	priv->scan_req = NULL;
	mutex_unlock(&priv->scan_lock);

	if (req)
		cfg80211_scan_done(req, &info);
}

static int esp32s31_wifi_stop(struct net_device *ndev)
{
	struct esp32s31_wifi *priv = esp32s31_wifi_priv(ndev);

	netif_stop_queue(ndev);
	napi_disable(&priv->napi);
	netif_carrier_off(ndev);
	esp32s31_wifi_abort_scan(priv);

	/*
	 * The link-down doorbell has no poll to run while NAPI is off.  Tell
	 * cfg80211 now, or the next association never reports a result.
	 */
	if (priv->connected) {
		cfg80211_disconnected(ndev, 0, NULL, 0, true, GFP_KERNEL);
		priv->connected = false;
	}

	return 0;
}

static void esp32s31_wifi_send_cmd(struct esp32s31_wifi *priv, u32 code)
{
	/* Write the code last: the firmware reads it to find a command. */
	wmb();
	iowrite32(code, &priv->ipc->cmd.code);
	iowrite32(1, priv->doorbell + ESP32S31_WIFI_DOORBELL_TX);
}

/*
 * The firmware gives parsed scan data, not the raw elements.  Build the
 * elements that a supplicant reads: SSID, rates, DS parameter set, and the
 * RSN or WPA element.
 */
#define ESP32S31_WIFI_IE_MAX		160

/* A count (le16) and one suite for each cipher bit. */
static u8 *esp32s31_wifi_put_ciphers(u8 *p, const u8 *oui, u8 ciphers)
{
	static const struct {
		u8 bit;
		u8 type;
	} map[] = {
		{ ESP32S31_IPC_CIPHER_CCMP, 4 },
		{ ESP32S31_IPC_CIPHER_TKIP, 2 },
		{ ESP32S31_IPC_CIPHER_GCMP, 8 },
		{ ESP32S31_IPC_CIPHER_GCMP256, 9 },
	};
	u8 *count = p;
	unsigned int i;

	p += 2;
	count[0] = 0;
	count[1] = 0;
	for (i = 0; i < ARRAY_SIZE(map); i++) {
		if (!(ciphers & map[i].bit))
			continue;
		memcpy(p, oui, 3);
		p[3] = map[i].type;
		p += 4;
		count[0]++;
	}
	return p;
}

static u8 *esp32s31_wifi_put_suite(u8 *p, const u8 *oui, u8 type)
{
	memcpy(p, oui, 3);
	p[3] = type;
	return p + 4;
}

static u8 esp32s31_wifi_cipher_type(u8 ciphers)
{
	if (ciphers & ESP32S31_IPC_CIPHER_TKIP)
		return 2;
	if (ciphers & ESP32S31_IPC_CIPHER_GCMP)
		return 8;
	if (ciphers & ESP32S31_IPC_CIPHER_GCMP256)
		return 9;
	return 4;
}

static size_t esp32s31_wifi_build_ies(const struct esp32s31_ipc_bss *bss,
				      u8 *ie)
{
	static const u8 rsn_oui[3] = { 0x00, 0x0f, 0xac };
	static const u8 wpa_oui[3] = { 0x00, 0x50, 0xf2 };
	/* 1, 2, 5.5 and 11 Mbit/s as basic rates, then the OFDM rates. */
	static const u8 cck[] = { 0x82, 0x84, 0x8b, 0x96 };
	static const u8 ofdm[] = { 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60,
				   0x6c };
	u8 pairwise = bss->pairwise ?: ESP32S31_IPC_CIPHER_CCMP;
	u8 group = bss->group ?: pairwise;
	u8 rates[sizeof(cck) + sizeof(ofdm)];
	unsigned int n = 0, first;
	u8 *p = ie, *len, *count;

	*p++ = WLAN_EID_SSID;
	*p++ = bss->ssid_len;
	memcpy(p, bss->ssid, bss->ssid_len);
	p += bss->ssid_len;

	if ((bss->phy & ESP32S31_IPC_PHY_11B) || !bss->phy) {
		memcpy(rates, cck, sizeof(cck));
		n = sizeof(cck);
	}
	if (bss->phy & ~ESP32S31_IPC_PHY_11B) {
		memcpy(rates + n, ofdm, sizeof(ofdm));
		n += sizeof(ofdm);
	}
	/* Up to 8 supported rates, the others in the extended rates. */
	first = min(n, 8U);
	*p++ = WLAN_EID_SUPP_RATES;
	*p++ = first;
	memcpy(p, rates, first);
	p += first;
	if (n > first) {
		*p++ = WLAN_EID_EXT_SUPP_RATES;
		*p++ = n - first;
		memcpy(p, rates + first, n - first);
		p += n - first;
	}

	*p++ = WLAN_EID_DS_PARAMS;
	*p++ = 1;
	*p++ = bss->channel;

	if (bss->akm & ESP32S31_IPC_AKM_WPA1) {
		*p++ = WLAN_EID_VENDOR_SPECIFIC;
		len = p++;
		p = esp32s31_wifi_put_suite(p, wpa_oui, 1);	/* WPA element */
		*p++ = 1;					/* version 1 */
		*p++ = 0;
		p = esp32s31_wifi_put_suite(p, wpa_oui,
					    esp32s31_wifi_cipher_type(group));
		p = esp32s31_wifi_put_ciphers(p, wpa_oui, pairwise &
					      (ESP32S31_IPC_CIPHER_TKIP |
					       ESP32S31_IPC_CIPHER_CCMP));
		*p++ = 1;					/* one AKM */
		*p++ = 0;
		p = esp32s31_wifi_put_suite(p, wpa_oui,
					    bss->akm & ESP32S31_IPC_AKM_EAP ?
					    1 : 2);
		*len = p - len - 1;
	} else if (bss->akm & (ESP32S31_IPC_AKM_PSK | ESP32S31_IPC_AKM_SAE |
			       ESP32S31_IPC_AKM_EAP | ESP32S31_IPC_AKM_OWE)) {
		/* SAE and OWE need MFP.  Only SAE or OWE alone requires it. */
		bool mfpc = bss->akm & (ESP32S31_IPC_AKM_SAE |
					ESP32S31_IPC_AKM_OWE);
		bool mfpr = mfpc && !(bss->akm & (ESP32S31_IPC_AKM_PSK |
						  ESP32S31_IPC_AKM_EAP));

		*p++ = WLAN_EID_RSN;
		len = p++;
		*p++ = 1;					/* version 1 */
		*p++ = 0;
		p = esp32s31_wifi_put_suite(p, rsn_oui,
					    esp32s31_wifi_cipher_type(group));
		p = esp32s31_wifi_put_ciphers(p, rsn_oui, pairwise);
		count = p;
		p += 2;
		count[0] = 0;
		count[1] = 0;
		if (bss->akm & ESP32S31_IPC_AKM_EAP) {
			p = esp32s31_wifi_put_suite(p, rsn_oui, 1);
			count[0]++;
		}
		if (bss->akm & ESP32S31_IPC_AKM_PSK) {
			p = esp32s31_wifi_put_suite(p, rsn_oui, 2);
			count[0]++;
		}
		if (bss->akm & ESP32S31_IPC_AKM_SAE) {
			p = esp32s31_wifi_put_suite(p, rsn_oui, 8);
			count[0]++;
		}
		if (bss->akm & ESP32S31_IPC_AKM_OWE) {
			p = esp32s31_wifi_put_suite(p, rsn_oui, 18);
			count[0]++;
		}
		/* RSN capabilities: bit 6 MFP required, bit 7 MFP capable. */
		*p++ = (mfpc ? BIT(7) : 0) | (mfpr ? BIT(6) : 0);
		*p++ = 0;
		*len = p - len - 1;
	}

	return p - ie;
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
		u8 ie[ESP32S31_WIFI_IE_MAX];
		u16 caps = WLAN_CAPABILITY_ESS;
		size_t ie_len;
		int freq;

		memcpy_fromio(&bss, &priv->ipc->scan.bss[i], sizeof(bss));
		if (bss.ssid_len > ESP32S31_IPC_SSID_MAX)
			continue;

		freq = ieee80211_channel_to_frequency(bss.channel,
						      NL80211_BAND_2GHZ);
		chan = ieee80211_get_channel(priv->wiphy, freq);
		if (!chan)
			continue;

		if (bss.akm)
			caps |= WLAN_CAPABILITY_PRIVACY;
		ie_len = esp32s31_wifi_build_ies(&bss, ie);

		found = cfg80211_inform_bss(priv->wiphy, chan,
					    CFG80211_BSS_FTYPE_UNKNOWN,
					    bss.bssid, 0, caps, 100,
					    ie, ie_len,
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
 * The firmware supplicant takes one string for all key types: a passphrase,
 * or 64 hex characters that it uses as the PMK.  The 4-way handshake offload
 * gives a PMK.  The SAE offload gives the password.  A supplicant gives the
 * password only for a network with SAE in key_mgmt.
 */
static int esp32s31_wifi_set_key(struct esp32s31_wifi *priv,
				 struct cfg80211_connect_params *sme)
{
	char hex[2 * WLAN_PMK_LEN + 1];

	/*
	 * The password first: the firmware can use it for SAE and for
	 * WPA2-PSK.  A PMK does not work for SAE, and a supplicant can give
	 * both for a WPA2/WPA3 transition network.
	 */
	if (sme->crypto.sae_pwd) {
		if (sme->crypto.sae_pwd_len > ESP32S31_IPC_PSK_MAX)
			return -EINVAL;
		memset_io(priv->ipc->cmd.psk, 0, ESP32S31_IPC_PSK_MAX);
		memcpy_toio(priv->ipc->cmd.psk, sme->crypto.sae_pwd,
			    sme->crypto.sae_pwd_len);
		return 0;
	}

	if (sme->crypto.psk) {
		memset_io(priv->ipc->cmd.psk, 0, ESP32S31_IPC_PSK_MAX);
		bin2hex(hex, sme->crypto.psk, WLAN_PMK_LEN);
		memcpy_toio(priv->ipc->cmd.psk, hex, 2 * WLAN_PMK_LEN);
		return 0;
	}

	/* No key: an open network. */
	memset_io(priv->ipc->cmd.psk, 0, ESP32S31_IPC_PSK_MAX);
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

	priv->key_offload = sme->crypto.psk || sme->crypto.sae_pwd;
	priv->fail_seq = ioread32(&priv->ipc->fail_seq);
	priv->connecting = true;
	esp32s31_wifi_send_cmd(priv, ESP32S31_IPC_CMD_CONNECT);

	return 0;
}

static int esp32s31_wifi_disconnect(struct wiphy *wiphy,
				    struct net_device *ndev, u16 reason_code)
{
	struct esp32s31_wifi *priv = wiphy_priv(wiphy);

	priv->connecting = false;
	esp32s31_wifi_send_cmd(priv, ESP32S31_IPC_CMD_DISCONNECT);

	return 0;
}

/* Without this, "iw link" shows the association and also an error. */
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
	/* The firmware keeps the keys and does the handshakes. */
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
	},
};
module_platform_driver(esp32s31_wifi_driver);

MODULE_DESCRIPTION("ESP32-S31 WLAN modem shared-memory network device");
MODULE_AUTHOR("Marco Müller <hello@annoyedmilk.ch>");
MODULE_LICENSE("GPL");
