/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_RISCV_ESP32S31_CACHE_H
#define _ASM_RISCV_ESP32S31_CACHE_H

/* The two L1 caches have 64-byte lines, the same as RISC-V L1_CACHE_BYTES. */
#define ESP32S31_CACHE_LINE_SIZE	64

/* Only the external memory aperture is cached; SRAM and MMIO need no sync. */
#define ESP32S31_CACHE_EXTRAM_BASE	0x50000000UL
#define ESP32S31_CACHE_EXTRAM_SIZE	0x04000000UL

#endif /* _ASM_RISCV_ESP32S31_CACHE_H */
