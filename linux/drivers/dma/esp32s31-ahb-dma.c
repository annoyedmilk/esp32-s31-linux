// SPDX-License-Identifier: GPL-2.0-only
/* ESP32-S31 AHB DMA cyclic transfers for streaming peripherals. */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#include "virt-dma.h"

#define AHB_CHANNELS		10
#define AHB_CHANNEL_PAIRS	5
#define AHB_INT_STATUS		0x04
#define AHB_INT_ENABLE		0x08
#define AHB_INT_CLEAR		0x0c
#define AHB_CONF0		0x00
#define AHB_CONF1		0x04
#define AHB_LINK		0x10
#define AHB_LINK_ADDR		0x14
#define AHB_EOF_ADDR		0x1c
#define AHB_PERIPH		0x38
#define AHB_RESET		BIT(0)
#define AHB_RX_DESC_BURST	BIT(2)
#define AHB_TX_EOF_MODE		BIT(3)
#define AHB_TX_DESC_BURST	BIT(4)
#define AHB_RX_EOF		BIT(1)
#define AHB_TX_EOF		BIT(1)
#define AHB_RX_ERRORS		GENMASK(7, 2)
#define AHB_TX_ERRORS		(BIT(2) | GENMASK(6, 4))
#define AHB_DESC_OWNER		BIT(31)
#define AHB_DESC_EOF		BIT(30)
#define AHB_DESC_LENGTH		GENMASK(23, 12)
#define AHB_DESC_SIZE		GENMASK(11, 0)
#define AHB_RX_STOP		BIT(1)
#define AHB_RX_START		BIT(2)
#define AHB_RX_PARK		BIT(4)
#define AHB_TX_STOP		BIT(0)
#define AHB_TX_START		BIT(1)
#define AHB_TX_PARK		BIT(3)
#define AHB_MEMORY_START	0x2f000000U
#define AHB_MEMORY_END		0x54000000U
#define AHB_MAX_PERIOD		4032
#define AHB_MAX_PERIODS		32

struct esp32s31_ahb_link {
	__le32 control;
	__le32 buffer;
	__le32 next;
};

struct esp32s31_ahb_desc {
	struct virt_dma_desc vd;
	struct esp32s31_ahb_link *links;
	dma_addr_t dma;
	size_t length;
	size_t period;
	unsigned int count;
	unsigned int completed;
};

struct esp32s31_ahb_chan {
	struct virt_dma_chan vc;
	void __iomem *regs;
	void __iomem *intr;
	struct esp32s31_ahb_desc *active;
	unsigned int request;
	int irq;
	bool tx;
	bool failed;
};

struct esp32s31_ahb_dma {
	struct dma_device dma;
	struct esp32s31_ahb_chan channels[AHB_CHANNELS];
};

static struct esp32s31_ahb_chan *to_ahb_chan(struct dma_chan *chan)
{
	return container_of(chan, struct esp32s31_ahb_chan, vc.chan);
}

static struct esp32s31_ahb_desc *to_ahb_desc(struct virt_dma_desc *vd)
{
	return container_of(vd, struct esp32s31_ahb_desc, vd);
}

static void esp32s31_ahb_free_desc(struct virt_dma_desc *vd)
{
	struct esp32s31_ahb_desc *desc = to_ahb_desc(vd);

	dma_free_coherent(vd->tx.chan->device->dev,
			  array_size(desc->count, sizeof(*desc->links)),
			  desc->links, desc->dma);
	kfree(desc);
}

static struct dma_async_tx_descriptor *
esp32s31_ahb_prep_cyclic(struct dma_chan *chan, dma_addr_t buffer,
			 size_t length, size_t period,
			 enum dma_transfer_direction dir, unsigned long flags)
{
	struct esp32s31_ahb_chan *ch = to_ahb_chan(chan);
	struct esp32s31_ahb_desc *desc;
	unsigned int i, count;
	u32 control;

	if (dir != (ch->tx ? DMA_MEM_TO_DEV : DMA_DEV_TO_MEM) ||
	    !period || period > AHB_MAX_PERIOD || length % period ||
	    !IS_ALIGNED(buffer, 4) || !IS_ALIGNED(period, 4) ||
	    buffer < AHB_MEMORY_START || buffer >= AHB_MEMORY_END ||
	    length > AHB_MEMORY_END - buffer)
		return NULL;
	count = length / period;
	if (count < 2 || count > AHB_MAX_PERIODS)
		return NULL;

	desc = kzalloc_obj(*desc, GFP_ATOMIC);
	if (!desc)
		return NULL;
	desc->count = count;
	desc->length = length;
	desc->period = period;
	desc->links = dma_alloc_coherent(chan->device->dev,
					 array_size(count, sizeof(*desc->links)),
					&desc->dma, GFP_ATOMIC);
	if (!desc->links) {
		kfree(desc);
		return NULL;
	}

	control = AHB_DESC_OWNER | FIELD_PREP(AHB_DESC_SIZE, period);
	if (ch->tx)
		control |= AHB_DESC_EOF | FIELD_PREP(AHB_DESC_LENGTH, period);
	for (i = 0; i < count; i++) {
		desc->links[i].control = cpu_to_le32(control);
		desc->links[i].buffer = cpu_to_le32(buffer + i * period);
		desc->links[i].next = cpu_to_le32(desc->dma +
				 ((i + 1) % count) * sizeof(*desc->links));
	}
	return vchan_tx_prep(&ch->vc, &desc->vd, flags);
}

static void esp32s31_ahb_start(struct esp32s31_ahb_chan *ch)
{
	struct virt_dma_desc *vd = vchan_next_desc(&ch->vc);
	u32 config = ch->tx ? AHB_TX_EOF_MODE | AHB_TX_DESC_BURST :
			     AHB_RX_DESC_BURST;

	if (!vd)
		return;
	list_del(&vd->node);
	ch->active = to_ahb_desc(vd);
	ch->failed = false;
	writel(config | AHB_RESET, ch->regs + AHB_CONF0);
	writel(config, ch->regs + AHB_CONF0);
	/* Cyclic RX writes owner back; the ring is owned until terminate_all. */
	writel(0, ch->regs + AHB_CONF1);
	writel(ch->request, ch->regs + AHB_PERIPH);
	writel(~0U, ch->intr + AHB_INT_CLEAR);
	writel(ch->tx ? AHB_TX_EOF | AHB_TX_ERRORS : AHB_RX_EOF | AHB_RX_ERRORS,
	       ch->intr + AHB_INT_ENABLE);
	dma_wmb();
	writel(ch->active->dma, ch->regs + AHB_LINK_ADDR);
	writel(ch->tx ? AHB_TX_START : AHB_RX_START, ch->regs + AHB_LINK);
}

static void esp32s31_ahb_issue_pending(struct dma_chan *chan)
{
	struct esp32s31_ahb_chan *ch = to_ahb_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&ch->vc.lock, flags);
	if (vchan_issue_pending(&ch->vc) && !ch->active)
		esp32s31_ahb_start(ch);
	spin_unlock_irqrestore(&ch->vc.lock, flags);
}

static irqreturn_t esp32s31_ahb_interrupt(int irq, void *data)
{
	struct esp32s31_ahb_chan *ch = data;
	struct esp32s31_ahb_desc *desc;
	unsigned long flags;
	u32 status, eof;
	unsigned int next;

	status = readl(ch->intr + AHB_INT_STATUS);
	if (!status)
		return IRQ_NONE;
	writel(status, ch->intr + AHB_INT_CLEAR);

	spin_lock_irqsave(&ch->vc.lock, flags);
	desc = ch->active;
	if (!desc)
		goto out;
	if (status & (ch->tx ? AHB_TX_ERRORS : AHB_RX_ERRORS)) {
		ch->failed = true;
		writel(0, ch->intr + AHB_INT_ENABLE);
		writel(ch->tx ? AHB_TX_STOP : AHB_RX_STOP, ch->regs + AHB_LINK);
		dev_err_ratelimited(ch->vc.chan.device->dev,
				    "DMA %s error: %#x\n", ch->tx ? "TX" : "RX", status);
		goto out;
	}
	if (status & (ch->tx ? AHB_TX_EOF : AHB_RX_EOF)) {
		eof = readl(ch->regs + AHB_EOF_ADDR);
		if (eof < desc->dma || eof >= desc->dma +
		    desc->count * sizeof(*desc->links) ||
		    (eof - desc->dma) % sizeof(*desc->links))
			goto out;
		next = ((eof - desc->dma) / sizeof(*desc->links) + 1) % desc->count;
		desc->completed = next;
		vchan_cyclic_callback(&desc->vd);
	}
out:
	spin_unlock_irqrestore(&ch->vc.lock, flags);
	return IRQ_HANDLED;
}

static int esp32s31_ahb_terminate_all(struct dma_chan *chan)
{
	struct esp32s31_ahb_chan *ch = to_ahb_chan(chan);
	unsigned long flags;
	u32 value;
	int ret;
	LIST_HEAD(head);

	spin_lock_irqsave(&ch->vc.lock, flags);
	writel(0, ch->intr + AHB_INT_ENABLE);
	writel(ch->tx ? AHB_TX_STOP : AHB_RX_STOP, ch->regs + AHB_LINK);
	ret = readl_poll_timeout_atomic(ch->regs + AHB_LINK, value,
					value & (ch->tx ? AHB_TX_PARK : AHB_RX_PARK), 1, 1000);
	/* Reset the local channel even when its peripheral has stopped clocks. */
	value = readl(ch->regs + AHB_CONF0);
	writel(value | AHB_RESET, ch->regs + AHB_CONF0);
	writel(value & ~AHB_RESET, ch->regs + AHB_CONF0);
	if (ret)
		dev_warn_ratelimited(chan->device->dev,
				     "DMA %s stop timed out; reset channel\n",
				     ch->tx ? "TX" : "RX");
	writel(~0U, ch->intr + AHB_INT_CLEAR);
	/* A running cyclic callback can still hold its descriptor until sync. */
	vchan_get_all_descriptors(&ch->vc, &head);
	list_splice_tail_init(&head, &ch->vc.desc_terminated);
	if (ch->active) {
		vchan_terminate_vdesc(&ch->active->vd);
		ch->active = NULL;
	}
	spin_unlock_irqrestore(&ch->vc.lock, flags);
	return 0;
}

static void esp32s31_ahb_synchronize(struct dma_chan *chan)
{
	struct esp32s31_ahb_chan *ch = to_ahb_chan(chan);

	synchronize_irq(ch->irq);
	vchan_synchronize(&ch->vc);
}

static void esp32s31_ahb_free_resources(struct dma_chan *chan)
{
	esp32s31_ahb_terminate_all(chan);
	esp32s31_ahb_synchronize(chan);
	vchan_free_chan_resources(&to_ahb_chan(chan)->vc);
}

static int esp32s31_ahb_config(struct dma_chan *chan,
			       struct dma_slave_config *config)
{
	struct esp32s31_ahb_chan *ch = to_ahb_chan(chan);
	enum dma_slave_buswidth width = ch->tx ? config->dst_addr_width :
						config->src_addr_width;

	if (width != DMA_SLAVE_BUSWIDTH_2_BYTES &&
	    width != DMA_SLAVE_BUSWIDTH_4_BYTES)
		return -EINVAL;
	return 0;
}

static enum dma_status esp32s31_ahb_status(struct dma_chan *chan,
					   dma_cookie_t cookie,
					 struct dma_tx_state *state)
{
	struct esp32s31_ahb_chan *ch = to_ahb_chan(chan);
	enum dma_status status;
	unsigned long flags;

	spin_lock_irqsave(&ch->vc.lock, flags);
	status = dma_cookie_status(chan, cookie, state);
	if (status == DMA_IN_PROGRESS && ch->active &&
	    ch->active->vd.tx.cookie == cookie) {
		if (ch->failed)
			status = DMA_ERROR;
		else if (state)
			dma_set_residue(state, ch->active->length -
					ch->active->completed * ch->active->period);
	}
	spin_unlock_irqrestore(&ch->vc.lock, flags);
	return status;
}

/* Cells: channel (RX 0..4, TX 5..9), peripheral request. */
static struct dma_chan *esp32s31_ahb_xlate(struct of_phandle_args *args,
					   struct of_dma *ofdma)
{
	struct esp32s31_ahb_dma *dma = ofdma->of_dma_data;
	struct esp32s31_ahb_chan *ch;
	struct dma_chan *chan;

	if (args->args_count != 2 || args->args[0] >= AHB_CHANNELS ||
	    args->args[1] > 14 || args->args[1] == 9)
		return NULL;
	ch = &dma->channels[args->args[0]];
	chan = dma_get_slave_channel(&ch->vc.chan);
	if (chan)
		ch->request = args->args[1];
	return chan;
}

static int esp32s31_ahb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct esp32s31_ahb_dma *dma;
	struct reset_control *rst;
	void __iomem *regs;
	struct clk *clk;
	unsigned int i;
	int irq, ret;

	dma = devm_kzalloc(dev, sizeof(*dma), GFP_KERNEL);
	if (!dma)
		return -ENOMEM;
	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs))
		return PTR_ERR(regs);
	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return PTR_ERR(clk);
	rst = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(rst))
		return PTR_ERR(rst);
	ret = reset_control_assert(rst);
	if (!ret)
		ret = reset_control_deassert(rst);
	if (ret)
		return ret;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	writel(AHB_MEMORY_START, regs + 0x600);
	writel(AHB_MEMORY_END - 1, regs + 0x604);
	INIT_LIST_HEAD(&dma->dma.channels);
	dma_cap_set(DMA_SLAVE, dma->dma.cap_mask);
	dma_cap_set(DMA_CYCLIC, dma->dma.cap_mask);
	dma->dma.dev = dev;
	dma->dma.directions = BIT(DMA_DEV_TO_MEM) | BIT(DMA_MEM_TO_DEV);
	dma->dma.src_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_2_BYTES) |
				   BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	dma->dma.dst_addr_widths = dma->dma.src_addr_widths;
	dma->dma.residue_granularity = DMA_RESIDUE_GRANULARITY_SEGMENT;
	dma->dma.device_config = esp32s31_ahb_config;
	dma->dma.device_free_chan_resources = esp32s31_ahb_free_resources;
	dma->dma.device_prep_dma_cyclic = esp32s31_ahb_prep_cyclic;
	dma->dma.device_issue_pending = esp32s31_ahb_issue_pending;
	dma->dma.device_terminate_all = esp32s31_ahb_terminate_all;
	dma->dma.device_synchronize = esp32s31_ahb_synchronize;
	dma->dma.device_tx_status = esp32s31_ahb_status;
	for (i = 0; i < AHB_CHANNELS; i++) {
		struct esp32s31_ahb_chan *ch = &dma->channels[i];
		unsigned int id = i % AHB_CHANNEL_PAIRS;

		ch->tx = i >= AHB_CHANNEL_PAIRS;
		ch->regs = regs + 0x100 + id * 0x100 + (ch->tx ? 0x80 : 0);
		ch->intr = regs + id * 0x10 + (ch->tx ? 0x50 : 0);
		writel(0, ch->intr + AHB_INT_ENABLE);
		ch->vc.desc_free = esp32s31_ahb_free_desc;
		vchan_init(&ch->vc, &dma->dma);
		irq = platform_get_irq(pdev, i);
		if (irq < 0) {
			ret = irq;
			goto free_channels;
		}
		ret = devm_request_irq(dev, irq, esp32s31_ahb_interrupt, 0,
				       dev_name(dev), ch);
		if (ret)
			goto free_channels;
		ch->irq = irq;
	}
	ret = dma_async_device_register(&dma->dma);
	if (ret)
		goto free_channels;
	ret = of_dma_controller_register(dev->of_node, esp32s31_ahb_xlate, dma);
	if (ret) {
		dma_async_device_unregister(&dma->dma);
		goto free_channels;
	}
	platform_set_drvdata(pdev, dma);
	return 0;

free_channels:
	for (i = 0; i < AHB_CHANNELS; i++)
		if (dma->channels[i].vc.chan.device) {
			struct esp32s31_ahb_chan *ch = &dma->channels[i];

			if (ch->irq)
				devm_free_irq(dev, ch->irq, ch);
			tasklet_kill(&ch->vc.task);
		}
	return ret;
}

static void esp32s31_ahb_remove(struct platform_device *pdev)
{
	struct esp32s31_ahb_dma *dma = platform_get_drvdata(pdev);
	unsigned int i;

	of_dma_controller_free(pdev->dev.of_node);
	dma_async_device_unregister(&dma->dma);
	for (i = 0; i < AHB_CHANNELS; i++) {
		struct esp32s31_ahb_chan *ch = &dma->channels[i];

		esp32s31_ahb_free_resources(&ch->vc.chan);
		devm_free_irq(&pdev->dev, ch->irq, ch);
	}
}

static const struct of_device_id esp32s31_ahb_ids[] = {
	{ .compatible = "esp,esp32s31-ahb-dma" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_ahb_ids);

static struct platform_driver esp32s31_ahb_driver = {
	.probe = esp32s31_ahb_probe,
	.remove = esp32s31_ahb_remove,
	.driver = {
		.name = "esp32s31-ahb-dma",
		.of_match_table = esp32s31_ahb_ids,
	},
};
module_platform_driver(esp32s31_ahb_driver);

MODULE_DESCRIPTION("ESP32-S31 AHB cyclic peripheral DMA");
MODULE_LICENSE("GPL");
