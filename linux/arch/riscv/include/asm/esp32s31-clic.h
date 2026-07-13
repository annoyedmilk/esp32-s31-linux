/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_RISCV_ESP32S31_CLIC_H
#define _ASM_RISCV_ESP32S31_CLIC_H

struct pt_regs;

void esp32s31_clic_handle_irq(struct pt_regs *regs);

#endif /* _ASM_RISCV_ESP32S31_CLIC_H */
