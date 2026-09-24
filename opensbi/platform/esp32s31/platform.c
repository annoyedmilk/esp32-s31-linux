// SPDX-License-Identifier: BSD-2-Clause
// Author: Marco Müller <hello@annoyedmilk.ch>

#include <sbi/riscv_asm.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_ecall_interface.h>
#include <sbi/sbi_ipi.h>
#include <sbi/sbi_platform.h>
#include <sbi/sbi_system.h>
#include <sbi/sbi_timer.h>

#define ESP32S31_UART0_BASE		0x2038a000UL
#define ESP32S31_UART_FIFO		0x00UL
#define ESP32S31_UART_STATUS		0x1cUL
#define ESP32S31_UART_RXFIFO_CNT_MASK	0xffUL
#define ESP32S31_UART_TXFIFO_CNT_SHIFT	16
#define ESP32S31_UART_TXFIFO_CNT_MASK	0xffUL
#define ESP32S31_UART_TXFIFO_LIMIT	120UL

/* Copy of the console on the USB Serial/JTAG CDC endpoint. */
#if ESP32S31_USB_SERIAL_JTAG_CONSOLE
#define ESP32S31_USB_SERIAL_JTAG_BASE		0x20391000UL
#define ESP32S31_USB_SERIAL_JTAG_EP1		0x00UL
#define ESP32S31_USB_SERIAL_JTAG_EP1_CONF	0x04UL
#define ESP32S31_USB_SERIAL_JTAG_WR_DONE	(1UL << 0)
#define ESP32S31_USB_SERIAL_JTAG_TX_FREE	(1UL << 1)
#endif

#define ESP32S31_HP_APM_ATTR0		0x2050440cUL
#define ESP32S31_HP_MEM_APM_ATTR0	0x2050480cUL
#define ESP32S31_APM_REE_TEE_RWX	0x7777UL

/*
 * CPU_APM controls the CPU-local bus.  Open one region for all addresses, so
 * S-mode can access the CLIC and machine-timer windows.
 */
#define ESP32S31_CPU_APM_BASE		0x20504c00UL
#define ESP32S31_CPU_APM_FILTER_EN	(ESP32S31_CPU_APM_BASE + 0x00UL)
#define ESP32S31_CPU_APM_REGION0_START	(ESP32S31_CPU_APM_BASE + 0x04UL)
#define ESP32S31_CPU_APM_REGION0_END	(ESP32S31_CPU_APM_BASE + 0x08UL)
#define ESP32S31_CPU_APM_ATTR0		(ESP32S31_CPU_APM_BASE + 0x0cUL)
#define ESP32S31_CPU_APM_FUNC_CTRL	(ESP32S31_CPU_APM_BASE + 0xc4UL)

/*
 * PSRAM must be write-through.  The LCD DMA engine reads the frame buffer
 * directly from PSRAM, without the CPU cache or the DMA API.  With
 * write-back, dirty lines stay in L1, the panel does not see them, and the
 * text is corrupted.  SD and USB use the DMA API, and the esp32s31-cache
 * driver does their cache maintenance, so write-through does not affect them.
 * cfg bit [10] = 1 selects write-through.  The other bits set RWX and cached.
 */
#define ESP32S31_PMAADDR0		0xbd0
#define ESP32S31_PMACFG0		0xbc0
#define ESP32S31_SRAM_PMA_NAPOT	0x0bc03fffUL
#define ESP32S31_PSRAM_PMA_NAPOT	0x141fffffUL
#define ESP32S31_SRAM_PMA_RWX		0xc000001dUL
#define ESP32S31_PSRAM_PMA_RWX_WT	0xc400001dUL

static inline void reg_write(unsigned long addr, unsigned long val)
{
	*(volatile unsigned int *)addr = (unsigned int)val;
	__asm__ volatile("fence iorw, iorw" ::: "memory");
}

static inline unsigned long reg_read(unsigned long addr)
{
	unsigned long val = *(volatile unsigned int *)addr;

	__asm__ volatile("fence iorw, iorw" ::: "memory");
	return val;
}

static inline void reg_write8(unsigned long addr, unsigned char val)
{
	*(volatile unsigned char *)addr = val;
	__asm__ volatile("fence iorw, iorw" ::: "memory");
}

static void esp32s31_apm_init(void)
{
	reg_write(ESP32S31_HP_APM_ATTR0, ESP32S31_APM_REE_TEE_RWX);
	reg_write(ESP32S31_HP_MEM_APM_ATTR0, ESP32S31_APM_REE_TEE_RWX);
	reg_write(ESP32S31_CPU_APM_REGION0_START, 0x00000000UL);
	reg_write(ESP32S31_CPU_APM_REGION0_END, 0xffffffffUL);
	reg_write(ESP32S31_CPU_APM_ATTR0, ESP32S31_APM_REE_TEE_RWX);
	reg_write(ESP32S31_CPU_APM_FILTER_EN, 1);
	reg_write(ESP32S31_CPU_APM_FUNC_CTRL, 0x0fUL);
}

static void esp32s31_pma_init(void)
{
	csr_write_num(ESP32S31_PMAADDR0, ESP32S31_SRAM_PMA_NAPOT);
	csr_write_num(ESP32S31_PMACFG0, ESP32S31_SRAM_PMA_RWX);
	csr_write_num(ESP32S31_PMAADDR0 + 1, ESP32S31_PSRAM_PMA_NAPOT);
	csr_write_num(ESP32S31_PMACFG0 + 1, ESP32S31_PSRAM_PMA_RWX_WT);
}

#if ESP32S31_USB_SERIAL_JTAG_CONSOLE
static void esp32s31_usb_serial_jtag_putc(char ch)
{
	/*
	 * When no host reads the endpoint, TX_FREE stays clear.  A full poll
	 * then takes milliseconds for each character, and the full console
	 * (also the UART) slows to approximately 130 B/s.  After one full
	 * timeout, the host is absent: check TX_FREE one time for each
	 * character and discard the output.  When a host connects later, it
	 * reads the endpoint, TX_FREE is set again, and the copy starts again.
	 */
	static bool host_absent;
	u32 limit = host_absent ? 1 : 100000;
	u32 i;

	for (i = 0; i < limit; i++) {
		if (reg_read(ESP32S31_USB_SERIAL_JTAG_BASE +
		     ESP32S31_USB_SERIAL_JTAG_EP1_CONF) &
		    ESP32S31_USB_SERIAL_JTAG_TX_FREE) {
			reg_write(ESP32S31_USB_SERIAL_JTAG_BASE +
				  ESP32S31_USB_SERIAL_JTAG_EP1,
				  (unsigned char)ch);
			reg_write(ESP32S31_USB_SERIAL_JTAG_BASE +
				  ESP32S31_USB_SERIAL_JTAG_EP1_CONF,
				  ESP32S31_USB_SERIAL_JTAG_WR_DONE);
			host_absent = false;
			return;
		}
	}
	host_absent = true;
}
#endif

static void esp32s31_console_putc(char ch)
{
	unsigned long status;
	unsigned long count;

	do {
		status = reg_read(ESP32S31_UART0_BASE + ESP32S31_UART_STATUS);
		count = (status >> ESP32S31_UART_TXFIFO_CNT_SHIFT) &
			ESP32S31_UART_TXFIFO_CNT_MASK;
	} while (count >= ESP32S31_UART_TXFIFO_LIMIT);

	reg_write(ESP32S31_UART0_BASE + ESP32S31_UART_FIFO, (unsigned char)ch);
#if ESP32S31_USB_SERIAL_JTAG_CONSOLE
	esp32s31_usb_serial_jtag_putc(ch);
#endif
}

static int esp32s31_console_getc(void)
{
	unsigned long status = reg_read(ESP32S31_UART0_BASE + ESP32S31_UART_STATUS);

	if (!(status & ESP32S31_UART_RXFIFO_CNT_MASK))
		return -1;

	return reg_read(ESP32S31_UART0_BASE + ESP32S31_UART_FIFO) & 0xff;
}

static struct sbi_console_device esp32s31_console = {
	.name = "esp32s31-uart0",
	.console_putc = esp32s31_console_putc,
	.console_getc = esp32s31_console_getc,
};

/* Resets the full digital system, the same as the reset button. */
#define ESP32S31_LP_SYS_CTRL		0x20700008UL
#define ESP32S31_LP_SYS_SW_RST		(1UL << 1)

static int esp32s31_system_reset_check(u32 type, u32 reason)
{
	return type == SBI_SRST_RESET_TYPE_SHUTDOWN ||
	       type == SBI_SRST_RESET_TYPE_COLD_REBOOT ||
	       type == SBI_SRST_RESET_TYPE_WARM_REBOOT;
}

static void esp32s31_system_reset(u32 type, u32 reason)
{
	/* The chip has no power switch, so shutdown stops the hart. */
	if (type != SBI_SRST_RESET_TYPE_SHUTDOWN)
		reg_write(ESP32S31_LP_SYS_CTRL,
			  reg_read(ESP32S31_LP_SYS_CTRL) |
			  ESP32S31_LP_SYS_SW_RST);

	csr_write(CSR_MIE, 0);
	while (1)
		wfi();
}

static struct sbi_system_reset_device esp32s31_reset = {
	.name = "esp32s31-sys-rst",
	.system_reset_check = esp32s31_system_reset_check,
	.system_reset = esp32s31_system_reset,
};

static int esp32s31_early_init(bool cold_boot)
{
	esp32s31_apm_init();
	esp32s31_pma_init();

	if (cold_boot) {
		sbi_system_reset_add_device(&esp32s31_reset);
		sbi_console_set_device(&esp32s31_console);
	}

	return 0;
}

static int esp32s31_final_init(bool cold_boot)
{
	if (!cold_boot)
		return 0;

	/*
	 * sbi_hart_switch_mode() keeps MSTATUS_SPIE.  Set it, so that the mret
	 * into S-mode sets SIE to 1.  Without it, the kernel starts with
	 * interrupts disabled and never gets a CLIC S-mode interrupt.
	 */
	csr_set(CSR_MSTATUS, MSTATUS_SPIE);

	return 0;
}

/*
 * The machine-timer window is at 0x10000000, as on a CLINT: mtime at
 * +0xbff8, mtimecmp at +0x4000 and a control register at +0x4010.  An
 * mtimecmp match goes to CLIC input 7, the standard machine-timer interrupt
 * ID.  The ESP-IDF loader sets the CPU and machine-timer clock to 320 MHz
 * before it starts OpenSBI.
 *
 * mcliccfg.NMBITS is writable.  NMBITS=1 enables clicintattr[i].MODE, so a
 * CLIC input can go directly to S-mode.  CLIC input 5 is also the S-mode
 * timer interrupt: in CLIC mode the interrupt ID goes into the scause
 * exception-code field, and 5 == IRQ_S_TIMER.
 *
 * Sequence: mtimecmp match -> CLIC ID7 -> M-mode trap (standard
 * IRQ_M_TIMER handling, no platform hook) -> sbi_timer_process() -> the
 * S-mode event callback sets CLIC ID5 pending -> the hardware jumps to the
 * S-mode stvec when sstatus.SIE is set.
 */
#define ESP32S31_MTIMER_BASE		0x10000000UL
#define ESP32S31_MTIMECMP_LO		(ESP32S31_MTIMER_BASE + 0x4000UL)
#define ESP32S31_MTIMECMP_HI		(ESP32S31_MTIMER_BASE + 0x4004UL)
#define ESP32S31_MTIMECTL		(ESP32S31_MTIMER_BASE + 0x4010UL)
#define ESP32S31_MTIME_LO		(ESP32S31_MTIMER_BASE + 0xbff8UL)
#define ESP32S31_MTIME_HI		(ESP32S31_MTIMER_BASE + 0xbffcUL)
#define ESP32S31_MTIMER_FREQ		320000000UL

/*
 * cliccfg is per hart: nvbits at bit 0, nlbits at [4:1], nmbits at [6:5].
 * Write all fields.  The Linux hart does not run the ESP-IDF startup, which
 * sets nlbits.  With an incorrect nlbits, the level bits of clicintctl are in
 * the wrong position.  Then all inputs have level 0 and never pass a zero
 * threshold.  nmbits = 1 enables clicintattr[i].MODE, so an input can go
 * directly to S-mode.
 */
#define ESP32S31_MCLICCFG		0x10800000UL
#define ESP32S31_MCLICCFG_NVBITS	(1UL << 0)
#define ESP32S31_MCLICCFG_NLBITS_1	(1UL << 1)
#define ESP32S31_MCLICCFG_NMBITS_M_S	(1UL << 5)
#define ESP32S31_MCLICCFG_VALUE \
	(ESP32S31_MCLICCFG_NVBITS | ESP32S31_MCLICCFG_NLBITS_1 | \
	 ESP32S31_MCLICCFG_NMBITS_M_S)

#define ESP32S31_CLIC_CTRL_BASE	0x10801000UL
#define ESP32S31_CLIC_IP(id)		(ESP32S31_CLIC_CTRL_BASE + 4UL * (id) + 0)
#define ESP32S31_CLIC_IE(id)		(ESP32S31_CLIC_CTRL_BASE + 4UL * (id) + 1)
#define ESP32S31_CLIC_ATTR(id)		(ESP32S31_CLIC_CTRL_BASE + 4UL * (id) + 2)
#define ESP32S31_CLIC_CTL(id)		(ESP32S31_CLIC_CTRL_BASE + 4UL * (id) + 3)

/*
 * clicintattr byte: SHV at bit 0 (0 for non-vectored), TRIG at [2:1], MODE
 * at [7:6].  MODE selects the privilege mode that gets the input.  It also
 * controls if the supervisor register window can see the input.
 */
#define ESP32S31_CLIC_ATTR_TRIG_EDGE	(1UL << 1)
#define ESP32S31_CLIC_ATTR_MODE_S	(1UL << 6)
#define ESP32S31_CLIC_ATTR_MODE_M	(3UL << 6)

#define ESP32S31_CLIC_ATTR_M_EDGE \
	(ESP32S31_CLIC_ATTR_MODE_M | ESP32S31_CLIC_ATTR_TRIG_EDGE)
#define ESP32S31_CLIC_ATTR_S_EDGE \
	(ESP32S31_CLIC_ATTR_MODE_S | ESP32S31_CLIC_ATTR_TRIG_EDGE)
#define ESP32S31_CLIC_ATTR_S_LEVEL	ESP32S31_CLIC_ATTR_MODE_S
#define ESP32S31_CLIC_CTL_MAX		0xff

#define ESP32S31_CLIC_EXT_MIN_ID	16
#define ESP32S31_CLIC_NUM_INT		48

#define ESP32S31_CLIC_MTIMER_ID	7	/* machine timer input */
#define ESP32S31_CLIC_SSOFT_ID	1	/* S-mode software interrupt */
#define ESP32S31_CLIC_STIMER_ID	5	/* S-mode timer input */

/* CLIC level-threshold CSRs.  Set the two to zero, or all inputs are masked. */
#define ESP32S31_CSR_MINTTHRESH	0x347
#define ESP32S31_CSR_SINTTHRESH	0x147

static u64 esp32s31_timer_value(void)
{
	u32 lo, hi, tmp;

	do {
		hi = reg_read(ESP32S31_MTIME_HI);
		lo = reg_read(ESP32S31_MTIME_LO);
		tmp = reg_read(ESP32S31_MTIME_HI);
	} while (hi != tmp);

	return ((u64)hi << 32) | lo;
}

static void esp32s31_timer_event_start(u64 next_event)
{
	/* Clear an old edge latch and make sure that the input is enabled. */
	reg_write8(ESP32S31_CLIC_IP(ESP32S31_CLIC_MTIMER_ID), 0);
	reg_write8(ESP32S31_CLIC_ATTR(ESP32S31_CLIC_MTIMER_ID),
		   ESP32S31_CLIC_ATTR_M_EDGE);
	reg_write8(ESP32S31_CLIC_IE(ESP32S31_CLIC_MTIMER_ID), 1);

	/*
	 * Set the high compare word to its maximum first, so the 64-bit update
	 * cannot match a half-written value.
	 */
	reg_write(ESP32S31_MTIMECMP_HI, 0xffffffffUL);
	reg_write(ESP32S31_MTIMECMP_LO, (u32)next_event);
	reg_write(ESP32S31_MTIMECMP_HI, (u32)(next_event >> 32));
}

static void esp32s31_timer_event_stop(void)
{
	reg_write(ESP32S31_MTIMECMP_HI, 0xffffffffUL);
	reg_write(ESP32S31_MTIMECMP_LO, 0xffffffffUL);
	reg_write8(ESP32S31_CLIC_IP(ESP32S31_CLIC_MTIMER_ID), 0);
}

static struct sbi_timer_device esp32s31_timer = {
	.name = "esp32s31-mtimer",
	.timer_freq = ESP32S31_MTIMER_FREQ,
	.timer_value = esp32s31_timer_value,
	.timer_event_start = esp32s31_timer_event_start,
	.timer_event_stop = esp32s31_timer_event_stop,
};

/*
 * Replace the weak MIP.STIP hooks from sbi_timer.c.  On this core MIP is not
 * writable, and the S-mode timer interrupt is CLIC input 5.
 */
void sbi_timer_plat_sirq_set(void)
{
	/* ATTR and CTL are configuration.  An interrupt only sets IP. */
	reg_write8(ESP32S31_CLIC_IP(ESP32S31_CLIC_STIMER_ID), 1);
}

void sbi_timer_plat_sirq_clear(void)
{
	reg_write8(ESP32S31_CLIC_IP(ESP32S31_CLIC_STIMER_ID), 0);
}

/*
 * MIP.SSIP is not writable on this CLIC-only hart.  SBI self-IPIs go through
 * CLIC input 1, which has the cause code of the standard supervisor software
 * interrupt.  Linux irq_work uses them, also on a uniprocessor system.
 */
void sbi_ipi_plat_sirq_set(void)
{
	reg_write8(ESP32S31_CLIC_IP(ESP32S31_CLIC_SSOFT_ID), 1);
}

void sbi_ipi_plat_sirq_clear(void)
{
	reg_write8(ESP32S31_CLIC_IP(ESP32S31_CLIC_SSOFT_ID), 0);
}

static int esp32s31_timer_init(void)
{
	int i;

	reg_write(ESP32S31_MCLICCFG, ESP32S31_MCLICCFG_VALUE);
	csr_write(ESP32S31_CSR_MINTTHRESH, 0);
	csr_write(ESP32S31_CSR_SINTTHRESH, 0);

	/*
	 * M-mode owns the CLIC.  The supervisor register window can only access
	 * an input when its MODE is S.  From S-mode, the other inputs read as
	 * zero and ignore writes.  Give all Interrupt Matrix inputs to the
	 * kernel, disabled.  The kernel sets the trigger and the level.
	 */
	for (i = ESP32S31_CLIC_EXT_MIN_ID; i < ESP32S31_CLIC_NUM_INT; i++) {
		reg_write8(ESP32S31_CLIC_IE(i), 0);
		reg_write8(ESP32S31_CLIC_ATTR(i), ESP32S31_CLIC_ATTR_S_LEVEL);
	}

	/* Machine timer input: M-mode, edge, max level, enabled. */
	reg_write8(ESP32S31_CLIC_IP(ESP32S31_CLIC_MTIMER_ID), 0);
	reg_write8(ESP32S31_CLIC_ATTR(ESP32S31_CLIC_MTIMER_ID),
		   ESP32S31_CLIC_ATTR_M_EDGE);
	reg_write8(ESP32S31_CLIC_CTL(ESP32S31_CLIC_MTIMER_ID),
		   ESP32S31_CLIC_CTL_MAX);
	reg_write8(ESP32S31_CLIC_IE(ESP32S31_CLIC_MTIMER_ID), 1);

	/*
	 * S-mode timer input: enable it here, so a payload without a CLIC driver
	 * gets it.  An S-mode kernel can control IE itself.
	 */
	reg_write8(ESP32S31_CLIC_IP(ESP32S31_CLIC_STIMER_ID), 0);
	reg_write8(ESP32S31_CLIC_ATTR(ESP32S31_CLIC_STIMER_ID),
		   ESP32S31_CLIC_ATTR_S_EDGE);
	reg_write8(ESP32S31_CLIC_CTL(ESP32S31_CLIC_STIMER_ID),
		   ESP32S31_CLIC_CTL_MAX);
	reg_write8(ESP32S31_CLIC_IE(ESP32S31_CLIC_STIMER_ID), 1);

	/* Supervisor software interrupt for the SBI IPI extension. */
	reg_write8(ESP32S31_CLIC_IP(ESP32S31_CLIC_SSOFT_ID), 0);
	reg_write8(ESP32S31_CLIC_ATTR(ESP32S31_CLIC_SSOFT_ID),
		   ESP32S31_CLIC_ATTR_S_EDGE);
	reg_write8(ESP32S31_CLIC_CTL(ESP32S31_CLIC_SSOFT_ID),
		   ESP32S31_CLIC_CTL_MAX);
	reg_write8(ESP32S31_CLIC_IE(ESP32S31_CLIC_SSOFT_ID), 1);

	reg_write(ESP32S31_MTIMECTL, 1);
	esp32s31_timer_event_stop();
	sbi_timer_set_device(&esp32s31_timer);

	return 0;
}

const struct sbi_platform_operations platform_ops = {
	.early_init = esp32s31_early_init,
	.final_init = esp32s31_final_init,
	.timer_init = esp32s31_timer_init,
};

/*
 * Hart 0 stays in M-mode and runs the ESP-IDF firmware for the WLAN modem.
 * OpenSBI controls only the Linux hart: hart 1.
 */
static const u32 esp32s31_hart_index2id[] = { 1 };

const struct sbi_platform platform = {
	.opensbi_version = OPENSBI_VERSION,
	.platform_version = SBI_PLATFORM_VERSION(0x0, 0x01),
	.name = "ESP32-S31 Korvo-1",
	.features = SBI_PLATFORM_DEFAULT_FEATURES,
	.hart_count = 1,
	.hart_index2id = esp32s31_hart_index2id,
	.hart_stack_size = SBI_PLATFORM_DEFAULT_HART_STACK_SIZE,
	.heap_size = SBI_PLATFORM_DEFAULT_HEAP_SIZE(1),
	.platform_ops_addr = (unsigned long)&platform_ops,
};
