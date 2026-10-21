// SPDX-License-Identifier: GPL-2.0-only
/*
 * ESP-Hosted transport for the ESP32-S31 when this Linux runs on hart 1 and
 * the ESP-Hosted firmware runs on hart 0 of the same chip.  The frames go
 * through two byte rings in internal SRAM, and the cross-core interrupts are
 * the doorbells.  The frames are the frames of the USB transport, so the
 * driver core sees a USB transport.  esp32s31-hosted-shm.h has the layout.
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/skbuff.h>
#include <linux/workqueue.h>

#include "utils.h"
#include "esp_if.h"
#include "esp_api.h"
#include "esp_utils.h"
#include "esp32s31-hosted-shm.h"
#include "esp_shm_flash.h"

/* Frames that wait for ring space: stop the net queues, wake them, drop. */
#define ESP_SHM_TX_PAUSE	64
#define ESP_SHM_TX_RESUME	16
#define ESP_SHM_TX_QUEUE_MAX	256

/* Doorbell registers: FROM_CPU_1 to the firmware, FROM_CPU_2 to Linux. */
#define ESP_SHM_DOORBELL_TO_FW	0x0
#define ESP_SHM_DOORBELL_TO_HOST	0x4

struct esp_shm_ring {
	u32 ctrl;
	u32 data;
	u32 size;
};

static const struct esp_shm_ring to_host = {
	ESP_SHM_TO_HOST_CTRL, ESP_SHM_TO_HOST_DATA, ESP_SHM_TO_HOST_SIZE
};
static const struct esp_shm_ring to_fw = {
	ESP_SHM_TO_FW_CTRL, ESP_SHM_TO_FW_DATA, ESP_SHM_TO_FW_SIZE
};

struct esp_shm_context {
	struct esp_adapter *adapter;
	struct device *dev;
	void __iomem *shm;
	void __iomem *doorbell;
	struct sk_buff_head tx_queue;
	struct work_struct tx_work;
	spinlock_t tx_lock;		/* running, tx_quiesced, tx_paused, tx_queue */
	struct esp_shm_flash *flash;
	bool running;
	bool tx_quiesced;
	bool tx_paused;
};

static struct esp_adapter *s_adapter;

/* Each transport defines it.  This one never sleeps, as on USB. */
volatile u8 host_sleep;

static u32 shm_read32(struct esp_shm_context *ctx, u32 off)
{
	return ioread32(ctx->shm + off);
}

static void shm_write32(struct esp_shm_context *ctx, u32 off, u32 val)
{
	iowrite32(val, ctx->shm + off);
}

/*
 * dst is 4-byte aligned.  memcpy_toio() reads src byte by byte on RV32, so
 * copy whole words when src is aligned too.
 */
static void shm_copy_to(void __iomem *dst, const void *src, u32 len)
{
	const u32 *s = src;

	if (IS_ALIGNED((unsigned long)src, 4))
		for (; len >= 4; len -= 4, dst += 4)
			__raw_writel(*s++, dst);
	memcpy_toio(dst, s, len);
}

static void ring_copy_in(struct esp_shm_context *ctx,
			 const struct esp_shm_ring *r, u32 pos,
			 const void *src, u32 len)
{
	u32 off = pos & (r->size - 1);
	u32 first = min(len, r->size - off);

	shm_copy_to(ctx->shm + r->data + off, src, first);
	shm_copy_to(ctx->shm + r->data, src + first, len - first);
}

/*
 * dst and src are 4-byte aligned.  memcpy_fromio() cannot know that for dst,
 * and on RV32 it then stores each word as four bytes.
 */
static void shm_copy_from(void *dst, const void __iomem *src, u32 len)
{
	u32 *d = dst;

	for (; len >= 4; len -= 4, src += 4)
		*d++ = __raw_readl(src);
	memcpy_fromio(d, src, len);
}

static void ring_copy_out(struct esp_shm_context *ctx,
			  const struct esp_shm_ring *r, u32 pos,
			  void *dst, u32 len)
{
	u32 off = pos & (r->size - 1);
	u32 first = min(len, r->size - off);

	shm_copy_from(dst, ctx->shm + r->data + off, first);
	shm_copy_from(dst + first, ctx->shm + r->data, len - first);
}

static void esp_shm_ring_firmware(struct esp_shm_context *ctx)
{
	iowrite32(1, ctx->doorbell + ESP_SHM_DOORBELL_TO_FW);
}

/* Ask the firmware for a new boot event, as the USB READY replay does. */
static void esp_shm_request_ready_replay(struct esp_shm_context *ctx)
{
	u32 req = shm_read32(ctx, ESP_SHM_CTRL + ESP_SHM_HOST_REQ_OFF);

	shm_write32(ctx, ESP_SHM_CTRL + ESP_SHM_HOST_REQ_OFF,
		    req | ESP_SHM_REQ_READY_REPLAY);
	esp_shm_ring_firmware(ctx);
}

/*
 * Add one frame.  Returns false when the ring is full.  Sets *kick when the
 * firmware had read all frames: then it can wait for the doorbell.  Else it
 * reads this frame before it waits.
 */
static bool esp_shm_push(struct esp_shm_context *ctx, const struct sk_buff *skb,
			 bool *kick)
{
	u32 head = shm_read32(ctx, to_fw.ctrl + ESP_SHM_HEAD_OFF);
	u32 tail = shm_read32(ctx, to_fw.ctrl + ESP_SHM_TAIL_OFF);
	u32 need = ESP_SHM_RECORD_LEN(skb->len);
	u32 len = skb->len;

	if (need > to_fw.size - (head - tail))
		return false;

	ring_copy_in(ctx, &to_fw, head + 4, skb->data, len);
	ring_copy_in(ctx, &to_fw, head, &len, 4);
	/* The firmware reads head, then the frame. */
	wmb();
	shm_write32(ctx, to_fw.ctrl + ESP_SHM_HEAD_OFF, head + need);
	/*
	 * Store head before the load of tail.  The firmware stores tail before
	 * it loads head, so one of the two sides sees the other.
	 */
	mb();
	*kick |= shm_read32(ctx, to_fw.ctrl + ESP_SHM_TAIL_OFF) == head;
	return true;
}

/* Stop or wake the net queues.  Called with tx_lock held. */
static void esp_shm_tx_flow(struct esp_shm_context *ctx, bool pause)
{
	int i;

	if (ctx->tx_paused == pause)
		return;
	ctx->tx_paused = pause;
	for (i = 0; i < ESP_MAX_INTERFACE; i++) {
		if (pause)
			esp_tx_pause(ctx->adapter->priv[i]);
		else
			esp_tx_resume(ctx->adapter->priv[i]);
	}
}

/* Move the queued frames into the ring.  Called with tx_lock held. */
static void esp_shm_tx_flush(struct esp_shm_context *ctx)
{
	struct sk_buff *skb;
	bool kick = false;

	if (!ctx->running || ctx->tx_quiesced)
		return;

	while ((skb = skb_peek(&ctx->tx_queue))) {
		/*
		 * Ask for a doorbell before the second try, so that space which
		 * the firmware frees in between is not missed.
		 */
		if (!esp_shm_push(ctx, skb, &kick)) {
			shm_write32(ctx, to_fw.ctrl + ESP_SHM_WAIT_OFF, 1);
			/* The flag before the second read of the ring index. */
			mb();
			if (!esp_shm_push(ctx, skb, &kick))
				break;
		}
		__skb_unlink(skb, &ctx->tx_queue);
		dev_kfree_skb_any(skb);
	}

	if (skb_queue_len(&ctx->tx_queue) <= ESP_SHM_TX_RESUME)
		esp_shm_tx_flow(ctx, false);
	if (kick)
		esp_shm_ring_firmware(ctx);
}

/* The firmware freed space in the ring. */
static void esp_shm_tx_work(struct work_struct *work)
{
	struct esp_shm_context *ctx =
		container_of(work, struct esp_shm_context, tx_work);

	spin_lock_bh(&ctx->tx_lock);
	esp_shm_tx_flush(ctx);
	spin_unlock_bh(&ctx->tx_lock);
}

static void esp_shm_tx_purge(struct esp_shm_context *ctx)
{
	spin_lock_bh(&ctx->tx_lock);
	__skb_queue_purge(&ctx->tx_queue);
	spin_unlock_bh(&ctx->tx_lock);
}

static irqreturn_t esp_shm_irq(int irq, void *data)
{
	struct esp_shm_context *ctx = data;

	iowrite32(0, ctx->doorbell + ESP_SHM_DOORBELL_TO_HOST);
	esp_process_new_packet_intr(ctx->adapter);
	esp_shm_flash_wake(ctx->flash);
	if (!skb_queue_empty(&ctx->tx_queue))
		schedule_work(&ctx->tx_work);
	return IRQ_HANDLED;
}

static struct sk_buff *esp_shm_alloc_skb(u32 len)
{
	struct sk_buff *skb;
	u32 offset;

	/* As the USB transport: keep the headroom and align the data. */
	skb = netdev_alloc_skb(NULL, len + INTERFACE_HEADER_PADDING +
			       SKB_DATA_ADDR_ALIGNMENT);
	if (!skb)
		return NULL;

	skb_reserve(skb, INTERFACE_HEADER_PADDING);
	offset = ((unsigned long)skb->data) & (SKB_DATA_ADDR_ALIGNMENT - 1);
	if (offset)
		skb_reserve(skb, SKB_DATA_ADDR_ALIGNMENT - offset);
	return skb;
}

/* Give the space of the read frames back to the firmware. */
static void esp_shm_rx_done(struct esp_shm_context *ctx, u32 tail)
{
	/* Read the frames before the firmware can write over them. */
	mb();
	shm_write32(ctx, to_host.ctrl + ESP_SHM_TAIL_OFF, tail);
	/* Store tail before the loads of wait and head.  See esp_shm_push(). */
	mb();
	if (shm_read32(ctx, to_host.ctrl + ESP_SHM_WAIT_OFF)) {
		shm_write32(ctx, to_host.ctrl + ESP_SHM_WAIT_OFF, 0);
		esp_shm_ring_firmware(ctx);
	}
}

static struct sk_buff *esp_shm_read(struct esp_adapter *adapter)
{
	struct esp_shm_context *ctx = adapter ? adapter->if_context : NULL;
	struct sk_buff *skb = NULL;
	u32 head, tail, len;

	if (!ctx || !ctx->running)
		return NULL;

	tail = shm_read32(ctx, to_host.ctrl + ESP_SHM_TAIL_OFF);
	while (!skb) {
		head = shm_read32(ctx, to_host.ctrl + ESP_SHM_HEAD_OFF);
		if (head == tail)
			return NULL;
		/* The firmware wrote the frame before head. */
		rmb();

		ring_copy_out(ctx, &to_host, tail, &len, 4);
		if (len < sizeof(struct esp_payload_header) ||
		    ESP_SHM_RECORD_LEN(len) > head - tail) {
			dev_err_ratelimited(ctx->dev, "bad frame length %u\n", len);
			shm_write32(ctx, to_host.ctrl + ESP_SHM_TAIL_OFF, head);
			esp_schedule_transport_recovery(adapter);
			return NULL;
		}

		/* Without memory, drop the frame: no doorbell comes for it. */
		skb = esp_shm_alloc_skb(len);
		if (skb)
			ring_copy_out(ctx, &to_host, tail + 4, skb_put(skb, len), len);
		tail += ESP_SHM_RECORD_LEN(len);
		esp_shm_rx_done(ctx, tail);
	}
	return skb;
}

static int esp_shm_write(struct esp_adapter *adapter, struct sk_buff *skb)
{
	struct esp_shm_context *ctx = adapter ? adapter->if_context : NULL;
	int ret = 0;

	if (!skb)
		return -EINVAL;
	if (!ctx || skb->len > adapter->tx_aggr_size ||
	    (skb_is_nonlinear(skb) && skb_linearize(skb))) {
		dev_kfree_skb_any(skb);
		return ctx ? -EMSGSIZE : -ENODEV;
	}

	spin_lock_bh(&ctx->tx_lock);
	if (!ctx->running || ctx->tx_quiesced) {
		ret = -ENODEV;
	} else if (skb_queue_len(&ctx->tx_queue) >= ESP_SHM_TX_QUEUE_MAX) {
		ret = -EBUSY;
	} else {
		__skb_queue_tail(&ctx->tx_queue, skb);
		esp_shm_tx_flush(ctx);
		if (skb_queue_len(&ctx->tx_queue) >= ESP_SHM_TX_PAUSE)
			esp_shm_tx_flow(ctx, true);
	}
	spin_unlock_bh(&ctx->tx_lock);

	if (ret)
		dev_kfree_skb_any(skb);
	return ret;
}

static void esp_shm_set_tx_quiesced(struct esp_shm_context *ctx, bool quiesced)
{
	spin_lock_bh(&ctx->tx_lock);
	ctx->tx_quiesced = quiesced;
	spin_unlock_bh(&ctx->tx_lock);
}

/*
 * The firmware never restarts without Linux, so a "firmware reset" here is a
 * new boot event.  Drop the frames of the old session in the two directions.
 */
static int esp_shm_quiesce_for_fw_reset(struct esp_adapter *adapter)
{
	struct esp_shm_context *ctx = adapter ? adapter->if_context : NULL;

	if (!ctx)
		return -ENODEV;
	esp_shm_set_tx_quiesced(ctx, true);
	cancel_work_sync(&ctx->tx_work);
	esp_shm_tx_purge(ctx);
	return 0;
}

static int esp_shm_reinit_after_fw_reset(struct esp_adapter *adapter)
{
	struct esp_shm_context *ctx = adapter ? adapter->if_context : NULL;

	if (!ctx || !ctx->running)
		return -ENODEV;
	esp_shm_set_tx_quiesced(ctx, false);
	schedule_work(&ctx->tx_work);
	return 0;
}

static int esp_shm_recover_transport(struct esp_adapter *adapter)
{
	struct esp_shm_context *ctx = adapter ? adapter->if_context : NULL;

	if (!ctx || !ctx->running)
		return -ENODEV;
	/* A new boot event completes the recovery, as on USB. */
	esp_shm_request_ready_replay(ctx);
	return 0;
}

static struct esp_if_ops esp_shm_if_ops = {
	.read = esp_shm_read,
	.write = esp_shm_write,
	.alloc_skb = esp_shm_alloc_skb,
	.quiesce_for_fw_reset = esp_shm_quiesce_for_fw_reset,
	.reinit_after_fw_reset = esp_shm_reinit_after_fw_reset,
	.recover_transport = esp_shm_recover_transport,
};

static int esp_shm_probe(struct platform_device *pdev)
{
	struct esp_adapter *adapter = s_adapter;
	struct device *dev = &pdev->dev;
	struct esp_shm_context *ctx;
	int irq, ret;

	if (!adapter)
		return -ENODEV;
	if (adapter->if_context)
		return -EBUSY;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->shm = devm_platform_ioremap_resource_byname(pdev, "shm");
	if (IS_ERR(ctx->shm))
		return PTR_ERR(ctx->shm);
	ctx->doorbell = devm_platform_ioremap_resource_byname(pdev, "doorbell");
	if (IS_ERR(ctx->doorbell))
		return PTR_ERR(ctx->doorbell);
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	if (shm_read32(ctx, ESP_SHM_CTRL + ESP_SHM_MAGIC_OFF) != ESP_SHM_MAGIC ||
	    shm_read32(ctx, ESP_SHM_CTRL + ESP_SHM_VERSION_OFF) != ESP_SHM_VERSION) {
		dev_err(dev, "no ESP-Hosted firmware with shared-memory transport\n");
		return -ENODEV;
	}

	ctx->adapter = adapter;
	ctx->dev = dev;
	skb_queue_head_init(&ctx->tx_queue);
	spin_lock_init(&ctx->tx_lock);
	INIT_WORK(&ctx->tx_work, esp_shm_tx_work);

	adapter->if_context = ctx;
	adapter->if_ops = &esp_shm_if_ops;
	adapter->if_type = ESP_IF_TYPE_USB;
	adapter->dev = dev;
	esp_reset_boot_recovery_for_binding(adapter, true);
	clear_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags);
	clear_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags);
	atomic_set(&adapter->state, ESP_CONTEXT_RX_READY);
	ctx->running = true;
	platform_set_drvdata(pdev, ctx);

	ret = devm_request_irq(dev, irq, esp_shm_irq, 0, dev_name(dev), ctx);
	if (ret) {
		ctx->running = false;
		adapter->if_context = NULL;
		adapter->dev = NULL;
		atomic_set(&adapter->state, ESP_CONTEXT_DISABLED);
		return ret;
	}

	/*
	 * The firmware put its boot event into the ring before Linux started.
	 * Read it now.  If it is missing, the recovery watchdog asks for a new
	 * one through recover_transport().
	 */
	esp_process_new_packet_intr(adapter);
	ctx->flash = esp_shm_flash_probe(dev, ctx->shm, ctx->doorbell);
	schedule_delayed_work(&adapter->fw_recovery_work,
			      msecs_to_jiffies(ESP_FW_RECOVERY_WATCHDOG_MS));
	dev_info(dev, "ESP32-S31 shared-memory transport bound\n");
	return 0;
}

static void esp_shm_remove(struct platform_device *pdev)
{
	struct esp_shm_context *ctx = platform_get_drvdata(pdev);
	struct esp_adapter *adapter = ctx->adapter;

	set_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags);
	set_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags);
	clear_bit(ESP_INIT_DONE, &adapter->state_flags);
	atomic_set(&adapter->state, ESP_CONTEXT_DISABLED);
	cancel_delayed_work_sync(&adapter->fw_recovery_work);
	if (adapter->events_wq)
		cancel_work_sync(&adapter->events_work);
	skb_queue_purge(&adapter->events_skb_q);
	if (adapter->if_rx_workqueue)
		cancel_work_sync(&adapter->if_rx_work);
	esp_remove_card(adapter, false);
	esp_shm_flash_remove(ctx->flash);

	esp_shm_set_tx_quiesced(ctx, true);
	ctx->running = false;
	devm_free_irq(&pdev->dev, platform_get_irq(pdev, 0), ctx);
	cancel_work_sync(&ctx->tx_work);
	esp_shm_tx_purge(ctx);
	adapter->if_context = NULL;
	adapter->dev = NULL;
}

static const struct of_device_id esp_shm_of_match[] = {
	{ .compatible = "esp,esp32s31-hosted-shm" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp_shm_of_match);

static struct platform_driver esp_shm_driver = {
	.probe = esp_shm_probe,
	.remove = esp_shm_remove,
	.driver = {
		.name = "esp32s31-hosted-shm",
		.of_match_table = esp_shm_of_match,
	},
};

/*
 * On USB, close is a soft reset of the firmware.  Here a firmware restart
 * also restarts Linux, so open and close only ask for a new boot event.  The
 * driver core starts a new session, but the firmware keeps its Wi-Fi state.
 */
int generate_slave_intr(void *context, u8 data)
{
	struct esp_shm_context *ctx = context;

	if (!ctx)
		return -ENODEV;
	if (data & (BIT(ESP_OPEN_DATA_PATH) | BIT(ESP_CLOSE_DATA_PATH)))
		esp_shm_request_ready_replay(ctx);
	return 0;
}

int esp_init_interface_layer(struct esp_adapter *adapter, u32 speed)
{
	int ret;

	if (!adapter)
		return -EINVAL;

	s_adapter = adapter;
	adapter->if_type = ESP_IF_TYPE_USB;
	adapter->if_ops = &esp_shm_if_ops;
	ret = platform_driver_register(&esp_shm_driver);
	if (ret) {
		adapter->if_ops = NULL;
		s_adapter = NULL;
	}
	return ret;
}

void esp_deinit_interface_layer(void)
{
	platform_driver_unregister(&esp_shm_driver);
	s_adapter = NULL;
}

int esp_validate_chipset(struct esp_adapter *adapter, u8 chipset)
{
	if (!adapter)
		return -EINVAL;

	if (chipset != ESP_FIRMWARE_CHIP_ESP32S31) {
		esp_err("Chipset=%s ID=%02x not supported for shared memory\n",
			esp_chipname_from_id(chipset), chipset);
		adapter->chipset = ESP_FIRMWARE_CHIP_UNRECOGNIZED;
		return -ENODEV;
	}

	adapter->chipset = chipset;
	esp_info("Chipset=%s ID=%02x detected over shared memory\n",
		 esp_chipname_from_id(chipset), chipset);
	return 0;
}

int esp_adjust_spi_clock(struct esp_adapter *adapter, u8 spi_clk_mhz)
{
	return 0;
}
