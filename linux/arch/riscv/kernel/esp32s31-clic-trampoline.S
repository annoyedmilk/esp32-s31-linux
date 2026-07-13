/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Author: Marco Müller <hello@annoyedmilk.ch>
 *
 * ESP32-S31 supervisor CLIC interrupt trampoline
 *
 * All supervisor CLIC interrupts enter here through stvec in non-vectored
 * CLIC mode. By the time we arrive:
 *
 *   Hardware has saved:
 *     sepc         = PC of interrupted code
 *     scause       = interrupt ID and CLIC state
 *     sstatus.SPIE = previous SIE
 *     sstatus.SIE  = 0
 *
 * We must:
 *   1. Save caller-saved registers → pt_regs on stack
 *   2. Call esp32s31_clic_handle_irq(regs) with SIE disabled
 *   3. Restore saved registers and execute sret
 *
 * Linux's generic IRQ core expects hard IRQ handlers to run with CPU
 * interrupts disabled. The ESP32-S31 CLIC can do hardware nesting, but this
 * trampoline presents a conventional non-nested IRQ entry to Linux.
 */

#include <linux/linkage.h>
#include <asm/asm.h>
#include <asm/csr.h>
#include <asm/asm-offsets.h>

	.text
	/*
	 * stvec.BASE is rounded down to a wider boundary in CLIC mode. Keep the
	 * non-vectored entry 64-byte aligned so interrupts cannot land in the
	 * preceding function.
	 */
	.balign 64

/*
 * Common trampoline for supervisor interrupts.
 *
 * Stack layout (struct pt_regs) - each field is 4 bytes on RV32:
 *   sp + 0x00:  epc
 *   sp + 0x04:  ra
 *   sp + 0x08:  sp (saved)
 *   sp + 0x0C:  gp
 *   sp + 0x10:  tp
 *   sp + 0x14:  t0
 *   sp + 0x18:  t1
 *   sp + 0x1C:  t2
 *   sp + 0x20:  s0/fp
 *   sp + 0x24:  s1
 *   sp + 0x28:  a0
 *   sp + 0x2C:  a1
 *   sp + 0x30:  a2
 *   sp + 0x34:  a3
 *   sp + 0x38:  a4
 *   sp + 0x3C:  a5
 *   sp + 0x40:  a6
 *   sp + 0x44:  a7
 *   sp + 0x48:  s2
 *   sp + 0x4C:  s3
 *   sp + 0x50:  s4
 *   sp + 0x54:  s5
 *   sp + 0x58:  s6
 *   sp + 0x5C:  s7
 *   sp + 0x60:  s8
 *   sp + 0x64:  s9
 *   sp + 0x68:  s10
 *   sp + 0x6C:  s11
 *   sp + 0x70:  t3
 *   sp + 0x74:  t4
 *   sp + 0x78:  t5
 *   sp + 0x7C:  t6
 *   sp + 0x80:  status
 *   sp + 0x84:  badaddr
 *   sp + 0x88:  cause
 *   sp + 0x8C:  orig_a0
 *
 * Must match struct pt_regs in arch/riscv/include/asm/ptrace.h.
 */

/* Offsets for 32-bit pt_regs - must match struct pt_regs in
 * arch/riscv/include/asm/ptrace.h exactly (4-byte fields for RV32).
 *
 * Layout (offset, size = 4 each):
 *   epc, ra, sp, gp, tp, t0, t1, t2, s0, s1,
 *   a0, a1, a2, a3, a4, a5, a6, a7,
 *   s2, s3, s4, s5, s6, s7, s8, s9, s10, s11,
 *   t3, t4, t5, t6,
 *   status, badaddr, cause, orig_a0
 */
#define PT_EPC_OFFSET		0x00
#define PT_RA_OFFSET		0x04
#define PT_SP_OFFSET		0x08
#define PT_GP_OFFSET		0x0C
#define PT_TP_OFFSET		0x10
#define PT_T0_OFFSET		0x14
#define PT_T1_OFFSET		0x18
#define PT_T2_OFFSET		0x1C
#define PT_S0_OFFSET		0x20
#define PT_S1_OFFSET		0x24
#define PT_A0_OFFSET		0x28
#define PT_A1_OFFSET		0x2C
#define PT_A2_OFFSET		0x30
#define PT_A3_OFFSET		0x34
#define PT_A4_OFFSET		0x38
#define PT_A5_OFFSET		0x3C
#define PT_A6_OFFSET		0x40
#define PT_A7_OFFSET		0x44
#define PT_S2_OFFSET		0x48
#define PT_S3_OFFSET		0x4C
#define PT_S4_OFFSET		0x50
#define PT_S5_OFFSET		0x54
#define PT_S6_OFFSET		0x58
#define PT_S7_OFFSET		0x5C
#define PT_S8_OFFSET		0x60
#define PT_S9_OFFSET		0x64
#define PT_S10_OFFSET		0x68
#define PT_S11_OFFSET		0x6C
#define PT_T3_OFFSET		0x70
#define PT_T4_OFFSET		0x74
#define PT_T5_OFFSET		0x78
#define PT_T6_OFFSET		0x7C
#define PT_STATUS_OFFSET	0x80
#define PT_BADADDR_OFFSET	0x84
#define PT_CAUSE_OFFSET		0x88
#define PT_ORIG_A0_OFFSET	0x8C

#ifndef CONFIG_MMU
#define do_page_fault do_trap_unknown
#endif

#define CLIC_STATUS_PP SR_PP

SYM_CODE_START(esp32s31_clic_trampoline)

	/*
	 * Match handle_exception's tp/scratch contract before we touch C code.
	 * User-mode interrupts arrive with CSR_SCRATCH = kernel tp and tp = user
	 * TLS. Kernel-mode interrupts arrive with CSR_SCRATCH = 0.
	 */
	csrrw	tp, CSR_SCRATCH, tp
	bnez	tp, .Lclic_from_user

.Lclic_from_kernel:
	csrr	tp, CSR_SCRATCH
	j	.Lclic_have_kernel_tp

.Lclic_from_user:
	REG_S	sp, TASK_TI_USER_SP(tp)
	REG_L	sp, TASK_TI_KERNEL_SP(tp)

.Lclic_have_kernel_tp:

	/*
	 * Save interrupted context
	 *
	 * We save the complete register set.  The caller-saved registers
	 * are mandatory; callee-saved are saved so the C handler can
	 * call schedule() and not lose state.
	 */
	addi	sp, sp, -PT_SIZE_ON_STACK

	/* Save GPRs */
	REG_S	ra,  PT_RA_OFFSET(sp)
	REG_S	gp,  PT_GP_OFFSET(sp)
	REG_S	t0,  PT_T0_OFFSET(sp)
	REG_S	t1,  PT_T1_OFFSET(sp)
	REG_S	t2,  PT_T2_OFFSET(sp)
	REG_S	s0,  PT_S0_OFFSET(sp)
	REG_S	s1,  PT_S1_OFFSET(sp)
	REG_S	a0,  PT_A0_OFFSET(sp)
	REG_S	a1,  PT_A1_OFFSET(sp)
	REG_S	a2,  PT_A2_OFFSET(sp)
	REG_S	a3,  PT_A3_OFFSET(sp)
	REG_S	a4,  PT_A4_OFFSET(sp)
	REG_S	a5,  PT_A5_OFFSET(sp)
	REG_S	a6,  PT_A6_OFFSET(sp)
	REG_S	a7,  PT_A7_OFFSET(sp)
	REG_S	s2,  PT_S2_OFFSET(sp)
	REG_S	s3,  PT_S3_OFFSET(sp)
	REG_S	s4,  PT_S4_OFFSET(sp)
	REG_S	s5,  PT_S5_OFFSET(sp)
	REG_S	s6,  PT_S6_OFFSET(sp)
	REG_S	s7,  PT_S7_OFFSET(sp)
	REG_S	s8,  PT_S8_OFFSET(sp)
	REG_S	s9,  PT_S9_OFFSET(sp)
	REG_S	s10, PT_S10_OFFSET(sp)
	REG_S	s11, PT_S11_OFFSET(sp)
	REG_S	t3,  PT_T3_OFFSET(sp)
	REG_S	t4,  PT_T4_OFFSET(sp)
	REG_S	t5,  PT_T5_OFFSET(sp)
	REG_S	t6,  PT_T6_OFFSET(sp)

	/* Save CSRs */
	csrr	t0, CSR_STATUS
	REG_S	t0,  PT_STATUS_OFFSET(sp)
	csrr	t1, CSR_EPC
	REG_S	t1,  PT_EPC_OFFSET(sp)
	csrr	t1, CSR_TVAL
	REG_S	t1,  PT_BADADDR_OFFSET(sp)
	csrr	t1, CSR_CAUSE
	REG_S	t1,  PT_CAUSE_OFFSET(sp)
	csrr	t1, CSR_SCRATCH
	REG_S	t1,  PT_TP_OFFSET(sp)

	/* Save the interrupted stack pointer, not the current kernel IRQ stack. */
	li	t1, CLIC_STATUS_PP
	and	t1, t0, t1
	bnez	t1, .Lclic_save_kernel_sp
	REG_L	t1, TASK_TI_USER_SP(tp)
	j	.Lclic_save_sp_done

.Lclic_save_kernel_sp:
	addi	t1, sp, PT_SIZE_ON_STACK

.Lclic_save_sp_done:
	REG_S	t1,  PT_SP_OFFSET(sp)

	/* Nested interrupts should reuse the live kernel tp/stack state. */
	csrw	CSR_SCRATCH, zero
	load_global_pointer

	/* orig_a0 (unused for IRQs, set to 0) */
	REG_S	zero, PT_ORIG_A0_OFFSET(sp)

	/*
	 * stvec.BASE also receives synchronous exceptions in CLIC mode. Route those
	 * through the normal trap handlers instead of the IRQ dispatcher. CLIC keeps
	 * extra state in scause, so low cause IDs are not enough: exception 7 and the
	 * machine-timer interrupt both have low bits of 7. The architectural interrupt
	 * bit is the discriminator.
	 */
	REG_L	t1, PT_CAUSE_OFFSET(sp)
	li	t0, CAUSE_IRQ_FLAG
	and	t0, t1, t0
	beqz	t0, .Lclic_exception

.Lclic_interrupt:

	/*
	 * Call C handler
	 *
	 * void esp32s31_clic_handle_irq(struct pt_regs *regs)
	 *
	 * Pass pt_regs pointer in a0. Keep SIE disabled while Linux's generic IRQ
	 * core runs; it will account and dispatch as a normal hardirq context.
	 *
	 * Load a preempt-safe stack pointer if we're in the kernel
	 * (avoid using the interrupted user stack).
	 */
	mv	a0, sp
	call	esp32s31_clic_handle_irq
	j	.Lclic_irq_return

.Lclic_exception:
	li	t2, 0xfff
	and	t0, t1, t2
	REG_S	t0, PT_CAUSE_OFFSET(sp)   /* Store masked cause so handlers see correct code */
	slli	t0, t0, RISCV_LGPTR
	la	t1, esp32s31_clic_excp_vect_table
	la	t2, esp32s31_clic_excp_vect_table_end
	add	t0, t1, t0
	bgeu	t0, t2, .Lclic_exception_unknown
	REG_L	t0, 0(t0)
	mv	a0, sp
	la	ra, ret_from_exception
	jr	t0

.Lclic_exception_unknown:
	tail	do_trap_unknown

.Lclic_irq_return:

	/*
	 * Disable interrupts during cleanup.
	 *
	 * The CLIC tracks nested interrupts via the level stack.
	 * Clearing xIE prevents higher-level interrupts from
	 * corrupting our restore sequence.
	 */
	csrci	CSR_STATUS, SR_IE

	/*
	 * If we are returning to userspace, publish the unwound kernel stack and
	 * kernel tp back to CSR_SCRATCH before we restore the user register state.
	 */
	REG_L	t0, PT_STATUS_OFFSET(sp)
	li	t1, CLIC_STATUS_PP
	and	t1, t0, t1
	bnez	t1, .Lclic_restore_csrs
	addi	t1, sp, PT_SIZE_ON_STACK
	REG_S	t1, TASK_TI_KERNEL_SP(tp)
	csrw	CSR_SCRATCH, tp

.Lclic_restore_csrs:

	/*
	 * Restore context and return
	 *
	 * xIE was cleared above. Restore xepc first (no side effects),
	 * then xstatus with xIE forcibly masked off so that restoring
	 * the saved status does not accidentally re-enable interrupts
	 * mid-way through the GPR restore loop. xret at the end atomically
	 * restores the correct interrupt-enable state.
	 */

	/* Restore xepc first - no interrupt-state side effects */
	REG_L	t0, PT_EPC_OFFSET(sp)
	csrw	CSR_EPC, t0

	/* Restore xstatus with xIE forcibly cleared */
	REG_L	t0, PT_STATUS_OFFSET(sp)
	li	t1, SR_IE
	not	t1, t1
	and	t0, t0, t1
	csrw	CSR_STATUS, t0

	/*
	 * Restore xcause before xret. The CLIC stores the previous interrupt
	 * level in xcause; xret copies it back to xintstatus. In S-mode this
	 * must be scause+sret. Using mret leaves mintstatus.SIL at the taken
	 * interrupt's level and permanently masks all later S interrupts.
	 */
	/* Now safe to restore GPRs - xIE is off, no preemption */
	/* (Restore all GPRs except sp, restored last) */
	REG_L	ra,  PT_RA_OFFSET(sp)
	REG_L	gp,  PT_GP_OFFSET(sp)
	REG_L	tp,  PT_TP_OFFSET(sp)
	REG_L	t1,  PT_T1_OFFSET(sp)
	REG_L	t2,  PT_T2_OFFSET(sp)
	REG_L	s0,  PT_S0_OFFSET(sp)
	REG_L	s1,  PT_S1_OFFSET(sp)
	REG_L	a0,  PT_A0_OFFSET(sp)
	REG_L	a1,  PT_A1_OFFSET(sp)
	REG_L	a2,  PT_A2_OFFSET(sp)
	REG_L	a3,  PT_A3_OFFSET(sp)
	REG_L	a4,  PT_A4_OFFSET(sp)
	REG_L	a5,  PT_A5_OFFSET(sp)
	REG_L	a6,  PT_A6_OFFSET(sp)
	REG_L	a7,  PT_A7_OFFSET(sp)
	REG_L	s2,  PT_S2_OFFSET(sp)
	REG_L	s3,  PT_S3_OFFSET(sp)
	REG_L	s4,  PT_S4_OFFSET(sp)
	REG_L	s5,  PT_S5_OFFSET(sp)
	REG_L	s6,  PT_S6_OFFSET(sp)
	REG_L	s7,  PT_S7_OFFSET(sp)
	REG_L	s8,  PT_S8_OFFSET(sp)
	REG_L	s9,  PT_S9_OFFSET(sp)
	REG_L	s10, PT_S10_OFFSET(sp)
	REG_L	s11, PT_S11_OFFSET(sp)
	REG_L	t3,  PT_T3_OFFSET(sp)
	REG_L	t4,  PT_T4_OFFSET(sp)
	REG_L	t5,  PT_T5_OFFSET(sp)
	REG_L	t6,  PT_T6_OFFSET(sp)

	/* scause.spil must be live at sret; restore it after all other state. */
	REG_L	t0, PT_CAUSE_OFFSET(sp)
	csrw	CSR_CAUSE, t0
	REG_L	t0, PT_T0_OFFSET(sp)

	addi	sp, sp, PT_SIZE_ON_STACK

	/*
	 * sret returns to sepc, restores SIE from SPIE, and informs the
	 * CLIC that this interrupt level is done.  The CLIC then
	 * delivers any pending interrupts at the restored level.
	 */
	sret

SYM_CODE_END(esp32s31_clic_trampoline)

SYM_DATA_START_LOCAL(esp32s31_clic_excp_vect_table)
	RISCV_PTR do_trap_insn_misaligned
	RISCV_PTR do_trap_insn_fault
	RISCV_PTR do_trap_insn_illegal
	RISCV_PTR do_trap_break
	RISCV_PTR do_trap_load_misaligned
	RISCV_PTR do_trap_load_fault
	RISCV_PTR do_trap_store_misaligned
	RISCV_PTR do_trap_store_fault
	RISCV_PTR do_trap_ecall_u
	RISCV_PTR do_trap_ecall_s
	RISCV_PTR do_trap_unknown
	RISCV_PTR do_trap_ecall_m
	RISCV_PTR do_page_fault
	RISCV_PTR do_page_fault
	RISCV_PTR do_trap_unknown
	RISCV_PTR do_page_fault
SYM_DATA_END_LABEL(esp32s31_clic_excp_vect_table, SYM_L_LOCAL, esp32s31_clic_excp_vect_table_end)
