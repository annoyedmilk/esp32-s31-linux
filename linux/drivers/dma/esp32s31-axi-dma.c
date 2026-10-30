// SPDX-License-Identifier: GPL-2.0-only
/* ESP32-S31 AXI peripheral DMA receive channels. */

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

#define AXI_RX_CHANNELS		3
#define AXI_CH_STRIDE		0x80
#define AXI_INT_STATUS		0x04
#define AXI_INT_ENABLE		0x08
#define AXI_INT_CLEAR		0x0c
#define AXI_CONF0		0x10
#define AXI_CONF1		0x14
#define AXI_LINK1		0x20
#define AXI_LINK2		0x24
#define AXI_PERIPH		0x44
#define AXI_INT_EOF		BIT(1)
#define AXI_INT_ERRORS		(BIT(2) | BIT(3))
#define AXI_CONF_RESET		BIT(0)
#define AXI_CONF_BURST		GENMASK(6, 4)
#define AXI_CONF_ABORT		BIT(7)
#define AXI_CONF_DESC_BURST	BIT(9)
#define AXI_OWNER_CHECK		BIT(12)
#define AXI_LINK_STOP		BIT(1)
#define AXI_LINK_START		BIT(2)
#define AXI_LINK_PARK		BIT(4)
#define AXI_RESET_AVAILABLE(n)	(0x28c + (n) * 4)
#define AXI_DESC_SIZE		GENMASK(11, 0)
#define AXI_DESC_LENGTH		GENMASK(23, 12)
#define AXI_DESC_OWNER		BIT(31)
#define AXI_BUFFER_SIZE		4032

/* Hardware requires eight-byte alignment; each link occupies 16 bytes. */
struct esp32s31_axi_link {
	__le32 control;
	__le32 buffer;
	__le32 next;
	__le32 reserved;
};

struct esp32s31_axi_desc {
	struct virt_dma_desc vd;
	struct esp32s31_axi_link *links;
	dma_addr_t dma;
	size_t size;
	size_t length;
	unsigned int count;
};

struct esp32s31_axi_chan {
	struct virt_dma_chan vc;
	void __iomem *regs;
	struct esp32s31_axi_desc *active;
	unsigned int id;
	unsigned int request;
	bool faulted;
};

struct esp32s31_axi_dma {
	struct dma_device dma;
	void __iomem *regs;
	struct clk *clk;
	struct esp32s31_axi_chan channels[AXI_RX_CHANNELS];
};

static struct esp32s31_axi_chan *to_axi_chan(struct dma_chan *chan)
{
	return container_of(chan, struct esp32s31_axi_chan, vc.chan);
}

static struct esp32s31_axi_desc *to_axi_desc(struct virt_dma_desc *vd)
{
	return container_of(vd, struct esp32s31_axi_desc, vd);
}

static void esp32s31_axi_free_desc(struct virt_dma_desc *vd)
{
	struct esp32s31_axi_desc *desc = to_axi_desc(vd);
	struct device *dev = vd->tx.chan->device->dev;

	dma_free_coherent(dev, desc->size, desc->links, desc->dma);
	kfree(desc);
}

static struct dma_async_tx_descriptor *
esp32s31_axi_prep_sg(struct dma_chan *chan, struct scatterlist *sgl,
		     unsigned int sg_len, enum dma_transfer_direction dir,
		     unsigned long flags, void *context)
{
	struct esp32s31_axi_desc *desc;
	struct scatterlist *sg;
	unsigned int i, count = 0, index = 0;
	dma_addr_t addr;
	size_t left, chunk;

	if (dir != DMA_DEV_TO_MEM || !sg_len ||
	    READ_ONCE(to_axi_chan(chan)->faulted))
		return NULL;

	for_each_sg(sgl, sg, sg_len, i) {
		if (!IS_ALIGNED(sg_dma_address(sg), 64) ||
		    !IS_ALIGNED(sg_dma_len(sg), 64))
			return NULL;
		count += DIV_ROUND_UP(sg_dma_len(sg), AXI_BUFFER_SIZE);
	}
	if (!count || count > 4096)
		return NULL;

	desc = kzalloc_obj(*desc, GFP_ATOMIC);
	if (!desc)
		return NULL;
	desc->count = count;
	desc->size = array_size(count, sizeof(*desc->links));
	desc->links = dma_alloc_coherent(chan->device->dev, desc->size,
					&desc->dma, GFP_ATOMIC);
	if (!desc->links) {
		kfree(desc);
		return NULL;
	}

	for_each_sg(sgl, sg, sg_len, i) {
		addr = sg_dma_address(sg);
		left = sg_dma_len(sg);
		desc->length += left;
		while (left) {
			struct esp32s31_axi_link *link = &desc->links[index];

			chunk = min_t(size_t, left, AXI_BUFFER_SIZE);
			link->control = cpu_to_le32(AXI_DESC_OWNER | chunk);
			link->buffer = cpu_to_le32(addr);
			if (index + 1 < count)
				link->next = cpu_to_le32(desc->dma +
						(index + 1) * sizeof(*link));
			addr += chunk;
			left -= chunk;
			index++;
		}
	}
	return vchan_tx_prep(&to_axi_chan(chan)->vc, &desc->vd, flags);
}

/* vc.lock serializes the active descriptor, interrupt and submit paths. */
static void esp32s31_axi_start(struct esp32s31_axi_chan *ch)
{
	struct virt_dma_desc *vd = vchan_next_desc(&ch->vc);
	u32 config = FIELD_PREP(AXI_CONF_BURST, 2) | AXI_CONF_DESC_BURST;

	if (!vd || ch->faulted)
		return;
	list_del(&vd->node);
	ch->active = to_axi_desc(vd);
	writel(config | AXI_CONF_RESET, ch->regs + AXI_CONF0);
	writel(config, ch->regs + AXI_CONF0);
	writel(AXI_OWNER_CHECK, ch->regs + AXI_CONF1);
	writel(ch->request, ch->regs + AXI_PERIPH);
	writel(~0U, ch->regs + AXI_INT_CLEAR);
	writel(AXI_INT_EOF | AXI_INT_ERRORS, ch->regs + AXI_INT_ENABLE);
	/* Publish the coherent SRAM links before the DMA reads them. */
	dma_wmb();
	writel(ch->active->dma, ch->regs + AXI_LINK2);
	writel(AXI_LINK_START, ch->regs + AXI_LINK1);
}

static void esp32s31_axi_issue_pending(struct dma_chan *chan)
{
	struct esp32s31_axi_chan *ch = to_axi_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&ch->vc.lock, flags);
	if (vchan_issue_pending(&ch->vc) && !ch->active)
		esp32s31_axi_start(ch);
	spin_unlock_irqrestore(&ch->vc.lock, flags);
}

static irqreturn_t esp32s31_axi_interrupt(int irq, void *data)
{
	struct esp32s31_axi_chan *ch = data;
	struct esp32s31_axi_desc *desc;
	unsigned long flags;
	u32 status;
	size_t received = 0;
	unsigned int i;

	status = readl(ch->regs + AXI_INT_STATUS);
	if (!status)
		return IRQ_NONE;
	writel(status, ch->regs + AXI_INT_CLEAR);

	spin_lock_irqsave(&ch->vc.lock, flags);
	desc = ch->active;
	if (desc && !ch->faulted) {
		writel(0, ch->regs + AXI_INT_ENABLE);
		writel(AXI_LINK_STOP, ch->regs + AXI_LINK1);
		dma_rmb();
		for (i = 0; i < desc->count; i++)
			received += FIELD_GET(AXI_DESC_LENGTH,
					      le32_to_cpu(desc->links[i].control));
		desc->vd.tx_result.residue = desc->length -
			min(received, desc->length);
		if (status & AXI_INT_ERRORS)
			desc->vd.tx_result.result = DMA_TRANS_READ_FAILED;
		ch->active = NULL;
		vchan_cookie_complete(&desc->vd);
		esp32s31_axi_start(ch);
	}
	spin_unlock_irqrestore(&ch->vc.lock, flags);
	return IRQ_HANDLED;
}

static int esp32s31_axi_terminate_all(struct dma_chan *chan)
{
	struct esp32s31_axi_chan *ch = to_axi_chan(chan);
	struct esp32s31_axi_dma *dma = container_of(chan->device,
						 struct esp32s31_axi_dma, dma);
	unsigned long flags;
	u32 value;
	int ret;
	LIST_HEAD(head);

	spin_lock_irqsave(&ch->vc.lock, flags);
	if (ch->faulted) {
		ret = -EIO;
		goto collect;
	}
	writel(0, ch->regs + AXI_INT_ENABLE);
	value = readl(ch->regs + AXI_CONF0);
	writel(value | AXI_CONF_ABORT, ch->regs + AXI_CONF0);
	writel(AXI_LINK_STOP, ch->regs + AXI_LINK1);
	ret = readl_poll_timeout_atomic(dma->regs + AXI_RESET_AVAILABLE(ch->id),
					value, value & 1, 1, 1000);
	if (ret) {
		/* Hardware may still follow these links. Keep them allocated. */
		ch->faulted = true;
		__module_get(THIS_MODULE);
		dev_err(chan->device->dev,
			"channel %u cannot stop; DMA quarantined until reboot\n",
			ch->id);
		goto collect;
	}
	value = readl(ch->regs + AXI_CONF0);
	writel(value | AXI_CONF_RESET, ch->regs + AXI_CONF0);
	writel(value & ~(AXI_CONF_RESET | AXI_CONF_ABORT),
	       ch->regs + AXI_CONF0);
	writel(~0U, ch->regs + AXI_INT_CLEAR);
	if (ch->active) {
		vchan_terminate_vdesc(&ch->active->vd);
		ch->active = NULL;
	}
collect:
	/* The active descriptor is outside these lists when quarantined. */
	vchan_get_all_descriptors(&ch->vc, &head);
	spin_unlock_irqrestore(&ch->vc.lock, flags);
	vchan_dma_desc_free_list(&ch->vc, &head);
	return ret;
}

static void esp32s31_axi_synchronize(struct dma_chan *chan)
{
	vchan_synchronize(&to_axi_chan(chan)->vc);
}

static void esp32s31_axi_free_resources(struct dma_chan *chan)
{
	esp32s31_axi_terminate_all(chan);
	vchan_free_chan_resources(&to_axi_chan(chan)->vc);
}

static enum dma_status esp32s31_axi_status(struct dma_chan *chan,
					 dma_cookie_t cookie,
					 struct dma_tx_state *state)
{
	return dma_cookie_status(chan, cookie, state);
}

static struct dma_chan *esp32s31_axi_xlate(struct of_phandle_args *args,
					 struct of_dma *ofdma)
{
	struct esp32s31_axi_dma *dma = ofdma->of_dma_data;
	struct esp32s31_axi_chan *ch;
	struct dma_chan *chan;

	if (args->args_count != 2 || args->args[0] >= AXI_RX_CHANNELS ||
	    args->args[1] > 5)
		return NULL;
	ch = &dma->channels[args->args[0]];
	chan = dma_get_slave_channel(&ch->vc.chan);
	if (chan)
		ch->request = args->args[1];
	return chan;
}

static int esp32s31_axi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct esp32s31_axi_dma *dma;
	struct reset_control *rst;
	struct dma_chan *chan;
	unsigned int i;
	int irq, ret;

	dma = devm_kzalloc(dev, sizeof(*dma), GFP_KERNEL);
	if (!dma)
		return -ENOMEM;
	dma->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dma->regs))
		return PTR_ERR(dma->regs);
	dma->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(dma->clk))
		return PTR_ERR(dma->clk);
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

	/* Allow descriptor SRAM and cached PSRAM bus addresses, as in ESP-IDF. */
	writel(0x2f000000, dma->regs + 0x27c);
	writel(0x2f07ffff, dma->regs + 0x280);
	writel(0x40000000, dma->regs + 0x284);
	writel(0x53ffffff, dma->regs + 0x288);
	INIT_LIST_HEAD(&dma->dma.channels);
	dma_cap_set(DMA_SLAVE, dma->dma.cap_mask);
	dma->dma.dev = dev;
	dma->dma.directions = BIT(DMA_DEV_TO_MEM);
	dma->dma.residue_granularity = DMA_RESIDUE_GRANULARITY_DESCRIPTOR;
	dma->dma.device_free_chan_resources = esp32s31_axi_free_resources;
	dma->dma.device_prep_slave_sg = esp32s31_axi_prep_sg;
	dma->dma.device_issue_pending = esp32s31_axi_issue_pending;
	dma->dma.device_terminate_all = esp32s31_axi_terminate_all;
	dma->dma.device_synchronize = esp32s31_axi_synchronize;
	dma->dma.device_tx_status = esp32s31_axi_status;
	for (i = 0; i < AXI_RX_CHANNELS; i++) {
		struct esp32s31_axi_chan *ch = &dma->channels[i];

		ch->id = i;
		ch->regs = dma->regs + i * AXI_CH_STRIDE;
		writel(0, ch->regs + AXI_INT_ENABLE);
		ch->vc.desc_free = esp32s31_axi_free_desc;
		vchan_init(&ch->vc, &dma->dma);
		irq = platform_get_irq(pdev, i);
		if (irq < 0) {
			ret = irq;
			goto free_channels;
		}
		ret = devm_request_irq(dev, irq, esp32s31_axi_interrupt, 0,
				       dev_name(dev), ch);
		if (ret)
			goto free_channels;
	}
	ret = dma_async_device_register(&dma->dma);
	if (ret)
		goto free_channels;
	ret = of_dma_controller_register(dev->of_node, esp32s31_axi_xlate, dma);
	if (ret) {
		dma_async_device_unregister(&dma->dma);
		goto free_channels;
	}
	platform_set_drvdata(pdev, dma);
	return 0;

free_channels:
	list_for_each_entry(chan, &dma->dma.channels, device_node)
		tasklet_kill(&to_axi_chan(chan)->vc.task);
	return ret;
}

static void esp32s31_axi_remove(struct platform_device *pdev)
{
	struct esp32s31_axi_dma *dma = platform_get_drvdata(pdev);
	unsigned int i;

	of_dma_controller_free(pdev->dev.of_node);
	dma_async_device_unregister(&dma->dma);
	for (i = 0; i < AXI_RX_CHANNELS; i++) {
		esp32s31_axi_free_resources(&dma->channels[i].vc.chan);
		tasklet_kill(&dma->channels[i].vc.task);
	}
}

static const struct of_device_id esp32s31_axi_ids[] = {
	{ .compatible = "esp,esp32s31-axi-dma" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_axi_ids);

static struct platform_driver esp32s31_axi_driver = {
	.probe = esp32s31_axi_probe,
	.remove = esp32s31_axi_remove,
	.driver = {
		.name = "esp32s31-axi-dma",
		.of_match_table = esp32s31_axi_ids,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(esp32s31_axi_driver);

MODULE_DESCRIPTION("ESP32-S31 AXI peripheral receive DMA");
MODULE_LICENSE("GPL");
