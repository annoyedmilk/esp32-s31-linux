/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Author: Marco Müller <hello@annoyedmilk.ch>
 *
 * ESP32-S31 supervisor CLIC interrupt trampoline
 *
 * All supervisor CLIC interrupts come here through stvec, in non-vectored
 * CLIC mode.  At entry, the hardware has set:
 *
 *     sepc         = PC of the interrupted code
 *     scause       = interrupt ID and CLIC state
 *     sstatus.SPIE = previous SIE
 *     sstatus.SIE  = 0
 *
 * The trampoline:
 *   1. Saves all registers in pt_regs on the stack
 *   2. Sends a synchronous exception to its trap handler, or calls
 *      do_irq(regs) with SIE disabled
 *   3. Restores the registers and executes sret
 *
 * The Linux IRQ core runs hard IRQ handlers with CPU interrupts disabled.
 * The ESP32-S31 CLIC can nest interrupts in hardware, but this trampoline
 * gives Linux a usual IRQ entry without nesting.
 */

#include <linux/linkage.h>
#include <asm/asm.h>
#include <asm/csr.h>
#include <asm/asm-offsets.h>

	.text
	/*
	 * In CLIC mode, stvec.BASE is rounded down to a larger boundary.  Align
	 * the entry to 64 bytes, so an interrupt cannot go into the function
	 * before it.
	 */
	.balign 64

/*
 * Trampoline for all supervisor interrupts.  The register save area is a
 * struct pt_regs, with the generated PT_* asm-offsets, as in entry.S.
 */

#ifndef CONFIG_MMU
#define do_page_fault do_trap_unknown
#endif

/* scause[23:16] is the CLIC previous-interrupt-level (spil) field. */
#define CLIC_CAUSE_SPIL_MASK (0xff << 16)

SYM_CODE_START(esp32s31_clic_trampoline)

	/*
	 * Use the same tp and scratch rules as handle_exception, before C code.
	 * From user mode: CSR_SCRATCH = kernel tp and tp = user TLS.  From
	 * kernel mode: CSR_SCRATCH = 0.
	 */
	csrrw	tp, CSR_SCRATCH, tp
	bnez	tp, .Lclic_from_user

.Lclic_from_kernel:
	csrr	tp, CSR_SCRATCH
	/* As handle_exception: first save the interrupted kernel stack. */
	REG_S	sp, TASK_TI_KERNEL_SP(tp)

.Lclic_from_user:
	/*
	 * Use thread_info.user_sp for the interrupted stack in the two cases.
	 * For a kernel trap, TASK_TI_KERNEL_SP is now the current stack.  For a
	 * user trap, it is the top of the kernel stack of the task.
	 */
	REG_S	sp, TASK_TI_USER_SP(tp)
	REG_L	sp, TASK_TI_KERNEL_SP(tp)

	/*
	 * Save the interrupted context.
	 *
	 * Save all registers.  The caller-saved registers are necessary.  Save
	 * the callee-saved registers too, so the C handler can call
	 * schedule().
	 */
	addi	sp, sp, -PT_SIZE_ON_STACK

	/* Save GPRs */
	REG_S	ra,  PT_RA(sp)
	REG_S	gp,  PT_GP(sp)
	REG_S	t0,  PT_T0(sp)
	REG_S	t1,  PT_T1(sp)
	REG_S	t2,  PT_T2(sp)
	REG_S	s0,  PT_S0(sp)
	REG_S	s1,  PT_S1(sp)
	REG_S	a0,  PT_A0(sp)
	REG_S	a1,  PT_A1(sp)
	REG_S	a2,  PT_A2(sp)
	REG_S	a3,  PT_A3(sp)
	REG_S	a4,  PT_A4(sp)
	REG_S	a5,  PT_A5(sp)
	REG_S	a6,  PT_A6(sp)
	REG_S	a7,  PT_A7(sp)
	REG_S	s2,  PT_S2(sp)
	REG_S	s3,  PT_S3(sp)
	REG_S	s4,  PT_S4(sp)
	REG_S	s5,  PT_S5(sp)
	REG_S	s6,  PT_S6(sp)
	REG_S	s7,  PT_S7(sp)
	REG_S	s8,  PT_S8(sp)
	REG_S	s9,  PT_S9(sp)
	REG_S	s10, PT_S10(sp)
	REG_S	s11, PT_S11(sp)
	REG_S	t3,  PT_T3(sp)
	REG_S	t4,  PT_T4(sp)
	REG_S	t5,  PT_T5(sp)
	REG_S	t6,  PT_T6(sp)

	/* Save CSRs */
	csrr	t0, CSR_STATUS
	REG_S	t0,  PT_STATUS(sp)
	csrr	t1, CSR_EPC
	REG_S	t1,  PT_EPC(sp)
	csrr	t1, CSR_TVAL
	REG_S	t1,  PT_BADADDR(sp)
	csrr	t1, CSR_CAUSE
	REG_S	t1,  PT_CAUSE(sp)
	csrr	t1, CSR_SCRATCH
	REG_S	t1,  PT_TP(sp)

	/* Save the interrupted stack pointer, not the kernel IRQ stack. */
	REG_L	t1, TASK_TI_USER_SP(tp)
	REG_S	t1,  PT_SP(sp)

	/* Nested interrupts use the current kernel tp and stack. */
	csrw	CSR_SCRATCH, zero
	load_global_pointer

	/* orig_a0 is not used for IRQs.  Set it to 0. */
	REG_S	zero, PT_ORIG_A0(sp)

	/*
	 * In CLIC mode, synchronous exceptions also come to stvec.BASE.  Send
	 * them to the usual trap handlers, not to the IRQ dispatcher.  The CLIC
	 * keeps more state in scause, so the low cause bits are not sufficient:
	 * exception 7 and the machine-timer interrupt both have 7.  Use the
	 * interrupt bit to identify them.
	 */
	REG_L	t1, PT_CAUSE(sp)
	li	t0, CAUSE_IRQ_FLAG
	and	t0, t1, t0
	beqz	t0, .Lclic_exception

.Lclic_interrupt:

	/*
	 * Remove the level from the CLIC supervisor level stack before C code.
	 *
	 * On this CLIC, only sret lowers the supervisor interrupt level
	 * (sintstatus.SIL <- scause.spil).  do_irq() can switch context
	 * through irqentry_exit() or preempt_schedule_irq().  If the level is
	 * still high then, other tasks run at handler level, with all S-mode
	 * interrupts masked.  The handler frames of preempted tasks then do
	 * their sret in scheduling order, not in nesting order.  A frame with
	 * an old level can keep SIL high: the timer tick stops and the system
	 * hangs without a message.
	 *
	 * Thus do an sret to the next instruction with scause.spil set to zero,
	 * so the handler and all that it schedules run at base level.  Clear
	 * SPIE first, so interrupts stay disabled.  Set SPP, so the CPU stays in
	 * S-mode.  pt_regs has the real sstatus, sepc and scause of the
	 * interrupted context, and the exit path restores them.
	 */
	REG_L	t0, PT_CAUSE(sp)
	li	t1, CLIC_CAUSE_SPIL_MASK
	not	t1, t1
	and	t0, t0, t1
	csrw	CSR_CAUSE, t0
	li	t0, SR_SPIE
	csrc	CSR_STATUS, t0
	li	t0, SR_SPP
	csrs	CSR_STATUS, t0
	la	t0, .Lclic_level_popped
	csrw	CSR_EPC, t0
	sret

.Lclic_level_popped:

	/*
	 * Call do_irq(regs) with SIE disabled.  The Linux IRQ core handles it as
	 * a usual hardirq.  The standard RISC-V IRQ wrapper also sets irq_regs
	 * and does the hardirq and RCU accounting that the timer and the
	 * scheduler need.
	 */
	mv	a0, sp
	call	do_irq
	j	.Lclic_irq_return

.Lclic_exception:
	li	t2, 0xfff
	and	t0, t1, t2
	REG_S	t0, PT_CAUSE(sp)   /* Store the masked cause for the handlers */
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
	 * Disable interrupts during the cleanup.
	 *
	 * The CLIC uses the level stack for nested interrupts.  With xIE
	 * clear, a higher-level interrupt cannot corrupt the restore sequence.
	 */
	csrci	CSR_STATUS, SR_IE

	/*
	 * For a return to userspace, save the kernel stack and the kernel tp in
	 * CSR_SCRATCH before the user registers are restored.
	 */
	REG_L	t0, PT_STATUS(sp)
	li	t1, SR_PP
	and	t1, t0, t1
	bnez	t1, .Lclic_restore_csrs
	addi	t1, sp, PT_SIZE_ON_STACK
	REG_S	t1, TASK_TI_KERNEL_SP(tp)
	csrw	CSR_SCRATCH, tp

.Lclic_restore_csrs:

	/*
	 * Restore the context and return.  xIE is clear.  Restore xepc first,
	 * because it does not change the interrupt state.  Then restore
	 * xstatus with xIE clear, so interrupts stay disabled while the GPRs
	 * are restored.  sret at the end sets the interrupt-enable state.
	 */
	REG_L	t0, PT_EPC(sp)
	csrw	CSR_EPC, t0

	REG_L	t0, PT_STATUS(sp)
	li	t1, SR_IE
	not	t1, t1
	and	t0, t0, t1
	csrw	CSR_STATUS, t0

	/* Restore the GPRs.  Restore sp last. */
	REG_L	ra,  PT_RA(sp)
	REG_L	gp,  PT_GP(sp)
	REG_L	tp,  PT_TP(sp)
	REG_L	t2,  PT_T2(sp)
	REG_L	s0,  PT_S0(sp)
	REG_L	s1,  PT_S1(sp)
	REG_L	a0,  PT_A0(sp)
	REG_L	a1,  PT_A1(sp)
	REG_L	a2,  PT_A2(sp)
	REG_L	a3,  PT_A3(sp)
	REG_L	a4,  PT_A4(sp)
	REG_L	a5,  PT_A5(sp)
	REG_L	a6,  PT_A6(sp)
	REG_L	a7,  PT_A7(sp)
	REG_L	s2,  PT_S2(sp)
	REG_L	s3,  PT_S3(sp)
	REG_L	s4,  PT_S4(sp)
	REG_L	s5,  PT_S5(sp)
	REG_L	s6,  PT_S6(sp)
	REG_L	s7,  PT_S7(sp)
	REG_L	s8,  PT_S8(sp)
	REG_L	s9,  PT_S9(sp)
	REG_L	s10, PT_S10(sp)
	REG_L	s11, PT_S11(sp)
	REG_L	t3,  PT_T3(sp)
	REG_L	t4,  PT_T4(sp)
	REG_L	t5,  PT_T5(sp)
	REG_L	t6,  PT_T6(sp)

	/*
	 * sret uses scause, so restore it after all other state.  The CLIC
	 * keeps the previous interrupt level in scause, and sret copies it to
	 * sintstatus.  Thus use scause and sret.  With mret, mintstatus.SIL
	 * stays at the level of the interrupt, and all later supervisor
	 * interrupts stay masked.
	 *
	 * The entry already removed the level, so set spil to zero here too.
	 * The interrupted context always continues at base level.  The saved
	 * spil can be old if the scheduler mixed the order of handler frames.
	 */
	REG_L	t0, PT_CAUSE(sp)
	li	t1, CLIC_CAUSE_SPIL_MASK
	not	t1, t1
	and	t0, t0, t1
	csrw	CSR_CAUSE, t0
	REG_L	t1, PT_T1(sp)
	REG_L	t0, PT_T0(sp)

	/* Return on the interrupted stack, not on the kernel IRQ stack. */
	REG_L	sp, PT_SP(sp)

	/*
	 * sret returns to sepc, sets SIE from SPIE, and tells the CLIC that
	 * this interrupt level is complete.  Then the CLIC sends the pending
	 * interrupts at the restored level.
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
