// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Marco Müller <hello@annoyedmilk.ch>
 *
 * Network device for the ESP32-S31 WLAN modem, which is owned by ESP-IDF
 * firmware resident on hart 0.  Linux exchanges 802.3 frames with it through
 * fixed-size slot rings in internal SRAM and a pair of cross-core doorbell
 * interrupts; the firmware runs the 802.11 side itself.
 *
 * The rings are reached without the data cache on either hart, so the io
 * accessors here are for their ordering barriers rather than for a device.
 */

#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "esp32s31-wifi-ipc.h"

#define ESP32S31_WIFI_NAPI_BUDGET	16
#define ESP32S31_WIFI_DOORBELL_RX	0x0
#define ESP32S31_WIFI_DOORBELL_TX	0x4

struct esp32s31_wifi {
	struct net_device *ndev;
	struct napi_struct napi;
	struct esp32s31_ipc __iomem *ipc;
	void __iomem *doorbell;
};

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

static void esp32s31_wifi_sync_carrier(struct esp32s31_wifi *priv)
{
	bool up = ioread32(&priv->ipc->link_up);

	if (up == netif_carrier_ok(priv->ndev))
		return;

	if (up)
		netif_carrier_on(priv->ndev);
	else
		netif_carrier_off(priv->ndev);
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
	napi_schedule(&priv->napi);

	return IRQ_HANDLED;
}

static netdev_tx_t esp32s31_wifi_xmit(struct sk_buff *skb,
				      struct net_device *ndev)
{
	struct esp32s31_wifi *priv = netdev_priv(ndev);
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
	struct esp32s31_wifi *priv = netdev_priv(ndev);

	napi_enable(&priv->napi);
	esp32s31_wifi_sync_carrier(priv);
	netif_start_queue(ndev);

	return 0;
}

static int esp32s31_wifi_stop(struct net_device *ndev)
{
	struct esp32s31_wifi *priv = netdev_priv(ndev);

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

static ssize_t ssid_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct esp32s31_wifi *priv = dev_get_drvdata(dev);

	return esp32s31_wifi_store_text(priv->ipc->cmd.ssid,
					ESP32S31_IPC_SSID_MAX, buf, count);
}
static DEVICE_ATTR_WO(ssid);

static ssize_t psk_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct esp32s31_wifi *priv = dev_get_drvdata(dev);

	return esp32s31_wifi_store_text(priv->ipc->cmd.psk,
					ESP32S31_IPC_PSK_MAX, buf, count);
}
static DEVICE_ATTR_WO(psk);

static ssize_t connect_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct esp32s31_wifi *priv = dev_get_drvdata(dev);
	bool connect;
	int ret;

	ret = kstrtobool(buf, &connect);
	if (ret)
		return ret;

	iowrite32(connect ? ESP32S31_IPC_CMD_CONNECT :
			    ESP32S31_IPC_CMD_DISCONNECT,
		  &priv->ipc->cmd.code);
	iowrite32(1, priv->doorbell + ESP32S31_WIFI_DOORBELL_TX);

	return count;
}
static DEVICE_ATTR_WO(connect);

static struct attribute *esp32s31_wifi_attrs[] = {
	&dev_attr_ssid.attr,
	&dev_attr_psk.attr,
	&dev_attr_connect.attr,
	NULL,
};
ATTRIBUTE_GROUPS(esp32s31_wifi);

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
	u8 mac[ETH_ALEN];
	int irq, ret;

	ndev = devm_alloc_etherdev(dev, sizeof(*priv));
	if (!ndev)
		return -ENOMEM;

	SET_NETDEV_DEV(ndev, dev);
	priv = netdev_priv(ndev);
	priv->ndev = ndev;
	platform_set_drvdata(pdev, priv);

	priv->ipc = devm_platform_ioremap_resource_byname(pdev, "ipc");
	if (IS_ERR(priv->ipc))
		return PTR_ERR(priv->ipc);

	priv->doorbell = devm_platform_ioremap_resource_byname(pdev, "doorbell");
	if (IS_ERR(priv->doorbell))
		return PTR_ERR(priv->doorbell);

	if (ioread32(&priv->ipc->magic) != ESP32S31_IPC_MAGIC)
		return dev_err_probe(dev, -ENODEV, "no firmware ring at %p\n",
				     priv->ipc);

	if (ioread32(&priv->ipc->version) != ESP32S31_IPC_VERSION)
		return dev_err_probe(dev, -EPROTO, "ring version %u != %u\n",
				     ioread32(&priv->ipc->version),
				     ESP32S31_IPC_VERSION);

	memcpy_fromio(mac, priv->ipc->mac, ETH_ALEN);
	eth_hw_addr_set(ndev, mac);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ndev->netdev_ops = &esp32s31_wifi_netdev_ops;
	ndev->max_mtu = ESP32S31_IPC_SLOT_DATA - ETH_HLEN;
	netif_napi_add(ndev, &priv->napi, esp32s31_wifi_poll);
	netif_carrier_off(ndev);

	ret = devm_request_irq(dev, irq, esp32s31_wifi_irq, 0,
			       dev_name(dev), priv);
	if (ret)
		return ret;

	ret = devm_register_netdev(dev, ndev);
	if (ret)
		return ret;

	dev_info(dev, "%s: firmware ring v%u, %pM\n", ndev->name,
		 ESP32S31_IPC_VERSION, ndev->dev_addr);

	return 0;
}

static const struct of_device_id esp32s31_wifi_of_match[] = {
	{ .compatible = "espressif,esp32s31-wifi" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_wifi_of_match);

static struct platform_driver esp32s31_wifi_driver = {
	.probe = esp32s31_wifi_probe,
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
