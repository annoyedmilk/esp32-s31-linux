// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Marco Müller <hello@annoyedmilk.ch>
 *
 * ESP32-S31 cache controller: non-standard cache maintenance for DMA
 *
 * The hart has no Zicbom.  Thus the only way to make DMA buffers in cached
 * memory coherent is the sync engine of the cache controller.  The uncached
 * alias of PSRAM at 0xc0000000 is not used.  One set of global SYNC_*
 * registers holds one operation at a time.  OpenSBI does all sync operations
 * in M-mode under one lock, and this driver asks for them through the vendor
 * SBI extension.  The SBI call also works early in the boot, before an
 * ioremap() is possible.
 *
 * The I-cache of each core refills from PSRAM, not from the shared D-cache,
 * and fence.i does not invalidate it.  With write-back PSRAM, new code can
 * also stay in the D-cache.  Thus local_flush_icache_all() and
 * flush_icache_folio() call esp32s31_icache_sync().
 */

#include <linux/align.h>
#include <linux/dma-mapping.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <asm/cacheflush.h>
#include <asm/dma-noncoherent.h>
#include <asm/esp32s31-cache.h>
#include <asm/sbi.h>

/* 0x09000000 + mvendorid.  Keep the IDs the same as in OpenSBI. */
#define ESP32S31_SBI_EXT		0x09000612
#define ESP32S31_SBI_CACHE_WBACK_ALL	0
#define ESP32S31_SBI_CACHE_INV		1
#define ESP32S31_SBI_CACHE_WBACK	2
#define ESP32S31_SBI_CACHE_WBACK_INV	3
#define ESP32S31_SBI_ICACHE_SYNC	4

/* For new code: D-cache write-back, then I-cache invalidate.  Size 0: all. */
void esp32s31_icache_sync(phys_addr_t paddr, size_t size)
{
	sbi_ecall(ESP32S31_SBI_EXT, ESP32S31_SBI_ICACHE_SYNC,
		  paddr, size, 0, 0, 0, 0);
}

/* Ranges fully in PSRAM only: SRAM is uncached, and size 0 means all. */
static void esp32s31_cache_sync(phys_addr_t paddr, size_t size,
				unsigned long funcid)
{
	phys_addr_t end = paddr + size;

	paddr = ALIGN_DOWN(paddr, ESP32S31_CACHE_LINE_SIZE);
	size = ALIGN(end - paddr, ESP32S31_CACHE_LINE_SIZE);

	if (!size || paddr < ESP32S31_CACHE_EXTRAM_BASE ||
	    paddr + size > ESP32S31_CACHE_EXTRAM_BASE + ESP32S31_CACHE_EXTRAM_SIZE)
		return;

	sbi_ecall(ESP32S31_SBI_EXT, funcid, paddr, size, 0, 0, 0, 0);
}

static void esp32s31_cache_wback(phys_addr_t paddr, size_t size)
{
	esp32s31_cache_sync(paddr, size, ESP32S31_SBI_CACHE_WBACK);
}

static void esp32s31_cache_inv(phys_addr_t paddr, size_t size)
{
	esp32s31_cache_sync(paddr, size, ESP32S31_SBI_CACHE_INV);
}

static void esp32s31_cache_wback_inv(phys_addr_t paddr, size_t size)
{
	esp32s31_cache_sync(paddr, size, ESP32S31_SBI_CACHE_WBACK_INV);
}

static const struct riscv_nonstd_cache_ops esp32s31_cache_ops __initconst = {
	.wback = &esp32s31_cache_wback,
	.inv = &esp32s31_cache_inv,
	.wback_inv = &esp32s31_cache_wback_inv,
};

static const struct of_device_id esp32s31_cache_ids[] __initconst = {
	{ .compatible = "esp,esp32s31-cache" },
	{ /* sentinel */ }
};

static int __init esp32s31_cache_init(void)
{
	struct device_node *np;
	bool available;

	np = of_find_matching_node(NULL, esp32s31_cache_ids);
	available = of_device_is_available(np);
	of_node_put(np);
	if (!available)
		return -ENODEV;

	if (sbi_probe_extension(ESP32S31_SBI_EXT) <= 0) {
		pr_err("esp32s31-cache: no cache SBI extension in the firmware\n");
		return -ENODEV;
	}

	/* setup_arch() set this up before slab took its DMA alignment. */
	riscv_noncoherent_register_cache_ops(&esp32s31_cache_ops);

	/* An alignment below the line size: setup_arch() did not enable it. */
	pr_info("esp32s31-cache: non-coherent DMA cache ops registered (%u-byte lines, DMA alignment %d)\n",
		ESP32S31_CACHE_LINE_SIZE, dma_get_cache_alignment());

	return 0;
}
early_initcall(esp32s31_cache_init);
