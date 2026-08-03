/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_RISCV_ESP32S31_CACHE_H
#define _ASM_RISCV_ESP32S31_CACHE_H

/* Both L1 caches use 64-byte lines, which matches RISC-V L1_CACHE_BYTES. */
#define ESP32S31_CACHE_LINE_SIZE	64

/*
 * Only the external memory aperture is served by the cache controller.
 * Internal SRAM and MMIO are reached without the data cache, so ranges
 * outside this window neither need nor tolerate a sync operation.
 */
#define ESP32S31_CACHE_EXTRAM_BASE	0x50000000UL
#define ESP32S31_CACHE_EXTRAM_SIZE	0x04000000UL

#endif /* _ASM_RISCV_ESP32S31_CACHE_H */
