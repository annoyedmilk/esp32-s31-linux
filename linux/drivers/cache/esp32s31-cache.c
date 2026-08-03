// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Marco Müller <hello@annoyedmilk.ch>
 *
 * ESP32-S31 cache controller: non-standard cache maintenance for DMA
 *
 * The hart implements no Zicbom, and the SoC exposes no uncached alias of
 * the PSRAM aperture, so the only way to make DMA buffers coherent is to
 * drive the cache controller's sync engine over MMIO.  A single set of
 * global SYNC_* registers describes one operation at a time, so every
 * request is serialised and polled to completion.
 */

#include <linux/align.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <asm/cacheflush.h>
#include <asm/dma-noncoherent.h>
#include <asm/esp32s31-cache.h>

#define ESP32S31_CACHE_SYNC_CTRL		0x9c
#define ESP32S31_CACHE_INVALIDATE_ENA		BIT(0)
#define ESP32S31_CACHE_WRITEBACK_ENA		BIT(2)
#define ESP32S31_CACHE_WRITEBACK_INVALIDATE_ENA	BIT(3)
#define ESP32S31_CACHE_SYNC_DONE		BIT(4)
#define ESP32S31_CACHE_SYNC_MAP			0xa0
#define ESP32S31_CACHE_SYNC_ADDR		0xa4
#define ESP32S31_CACHE_SYNC_SIZE		0xa8

/* Sync map selects which cache the operation applies to. */
#define ESP32S31_CACHE_MAP_L1_DCACHE		BIT(4)

#define ESP32S31_CACHE_SYNC_TIMEOUT_US		100000

static void __iomem *esp32s31_cache_base;
static DEFINE_RAW_SPINLOCK(esp32s31_cache_lock);

/*
 * Issue one sync operation over the requested range.  The controller latches
 * ADDR/SIZE and self-clears the start bit, so a repeat only has to rewrite
 * CTRL.  Errata: on this SoC a writeback can lose part of the range unless
 * the operation runs twice, which is what ESP-IDF's ROM patch does for
 * ESP_ROM_CACHE_WRITEBACK_NEEDS_SYNC_TWICE_MAP.  A plain invalidate is not
 * affected and runs once.
 */
static void esp32s31_cache_sync(phys_addr_t paddr, size_t size, u32 op,
				unsigned int passes)
{
	phys_addr_t end = paddr + size;
	unsigned long flags;
	unsigned int i;

	paddr = ALIGN_DOWN(paddr, ESP32S31_CACHE_LINE_SIZE);
	size = ALIGN(end - paddr, ESP32S31_CACHE_LINE_SIZE);

	/*
	 * Internal SRAM, which the coherent DMA pool is carved from, is
	 * reached without the data cache and is not a valid target for the
	 * sync engine.  A single buffer never straddles the two, so a range
	 * that is not entirely external needs no maintenance at all.
	 */
	if (paddr < ESP32S31_CACHE_EXTRAM_BASE ||
	    paddr + size > ESP32S31_CACHE_EXTRAM_BASE + ESP32S31_CACHE_EXTRAM_SIZE)
		return;

	raw_spin_lock_irqsave(&esp32s31_cache_lock, flags);

	writel(ESP32S31_CACHE_MAP_L1_DCACHE,
	       esp32s31_cache_base + ESP32S31_CACHE_SYNC_MAP);
	writel(paddr, esp32s31_cache_base + ESP32S31_CACHE_SYNC_ADDR);
	writel(size, esp32s31_cache_base + ESP32S31_CACHE_SYNC_SIZE);

	for (i = 0; i < passes; i++) {
		void __iomem *ctrl = esp32s31_cache_base + ESP32S31_CACHE_SYNC_CTRL;
		int timeout = ESP32S31_CACHE_SYNC_TIMEOUT_US;

		writel(op, ctrl);
		while (!(readl(ctrl) & ESP32S31_CACHE_SYNC_DONE)) {
			if (--timeout < 0) {
				raw_spin_unlock_irqrestore(&esp32s31_cache_lock,
							   flags);
				pr_err_ratelimited("esp32s31-cache: sync timeout op=%#x pa=%pa size=%zu\n",
						   op, &paddr, size);
				return;
			}
			udelay(1);
		}
	}

	raw_spin_unlock_irqrestore(&esp32s31_cache_lock, flags);
}

static void esp32s31_cache_wback(phys_addr_t paddr, size_t size)
{
	esp32s31_cache_sync(paddr, size, ESP32S31_CACHE_WRITEBACK_ENA, 2);
}

static void esp32s31_cache_inv(phys_addr_t paddr, size_t size)
{
	esp32s31_cache_sync(paddr, size, ESP32S31_CACHE_INVALIDATE_ENA, 1);
}

static void esp32s31_cache_wback_inv(phys_addr_t paddr, size_t size)
{
	esp32s31_cache_sync(paddr, size,
			    ESP32S31_CACHE_WRITEBACK_INVALIDATE_ENA, 2);
}

static const struct riscv_nonstd_cache_ops esp32s31_cache_ops __initconst = {
	.wback = &esp32s31_cache_wback,
	.inv = &esp32s31_cache_inv,
	.wback_inv = &esp32s31_cache_wback_inv,
};

static const struct of_device_id esp32s31_cache_ids[] __initconst = {
	{ .compatible = "espressif,esp32s31-cache" },
	{ /* sentinel */ }
};

static int __init esp32s31_cache_init(void)
{
	struct device_node *np;
	struct resource res;
	int ret;

	np = of_find_matching_node(NULL, esp32s31_cache_ids);
	if (!of_device_is_available(np)) {
		of_node_put(np);
		return -ENODEV;
	}

	ret = of_address_to_resource(np, 0, &res);
	of_node_put(np);
	if (ret)
		return ret;

	esp32s31_cache_base = ioremap(res.start, resource_size(&res));
	if (!esp32s31_cache_base)
		return -ENOMEM;

	/*
	 * setup_arch() has already published the line size and declared
	 * non-coherent DMA supported, because the slab allocator sizes its
	 * DMA alignment long before initcalls run.  Only the operations
	 * themselves have to wait for ioremap().
	 */
	riscv_noncoherent_register_cache_ops(&esp32s31_cache_ops);

	/*
	 * The reported alignment is what keeps a kmalloc'd DMA buffer from
	 * sharing a cache line with another allocation; anything below the
	 * line size means the setup_arch() declaration was missed.
	 */
	pr_info("esp32s31-cache: non-coherent DMA cache ops registered (%u-byte lines, DMA alignment %d)\n",
		ESP32S31_CACHE_LINE_SIZE, dma_get_cache_alignment());

	return 0;
}
early_initcall(esp32s31_cache_init);
