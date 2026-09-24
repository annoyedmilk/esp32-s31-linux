/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_RISCV_ESP32S31_CACHE_H
#define _ASM_RISCV_ESP32S31_CACHE_H

/* The two L1 caches have 64-byte lines, the same as RISC-V L1_CACHE_BYTES. */
#define ESP32S31_CACHE_LINE_SIZE	64

/*
 * The cache controller handles only the external memory aperture.  The CPU
 * accesses internal SRAM and MMIO without the data cache.  A range outside
 * this window does not need a sync operation, and must not get one.
 */
#define ESP32S31_CACHE_EXTRAM_BASE	0x50000000UL
#define ESP32S31_CACHE_EXTRAM_SIZE	0x04000000UL

#endif /* _ASM_RISCV_ESP32S31_CACHE_H */
