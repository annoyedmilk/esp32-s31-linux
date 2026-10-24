// SPDX-License-Identifier: BSD-2-Clause
// Author: Marco Müller <hello@annoyedmilk.ch>

#include <sbi/riscv_asm.h>
#include <sbi/riscv_atomic.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_ecall_interface.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_ipi.h>
#include <sbi/sbi_platform.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_system.h>
#include <sbi/sbi_timer.h>
#include "esp32s31-hosted-shm.h"

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

#define ESP32S31_HP_MEM_APM_ATTR0	0x2050480cUL
#define ESP32S31_APM_REE_TEE_RWX	0x7777UL
#define ESP32S31_APM_TEE_RWX		0x7000UL
#define ESP32S31_APM_LOCK		(1UL << 16)

/* HP_APM region 1 gives the OpenSBI area to TEE (M-mode) only, not DMA. */
#define ESP32S31_HP_APM_BASE		0x20504400UL
#define ESP32S31_HP_APM_FILTER_EN	ESP32S31_HP_APM_BASE
#define ESP32S31_HP_APM_START(n)	(ESP32S31_HP_APM_BASE + 0x04UL + 0x0cUL * (n))
#define ESP32S31_HP_APM_END(n)		(ESP32S31_HP_APM_BASE + 0x08UL + 0x0cUL * (n))
#define ESP32S31_HP_APM_ATTR(n)		(ESP32S31_HP_APM_BASE + 0x0cUL + 0x0cUL * (n))
#define ESP32S31_FW_BASE		0x50f00000UL
/* The two PMP regions of the root domain: 128 KiB code, 64 KiB data. */
#define ESP32S31_FW_SIZE		0x00030000UL

/* The TEE mode and APM registers: a domain region keeps them M-mode only. */
#define ESP32S31_TEE_APM_BASE		0x20504000UL
#define ESP32S31_TEE_APM_SIZE		0x1000UL

/* CPU_APM checks the CPU-local bus: open it for the CLIC and the timer. */
#define ESP32S31_CPU_APM_BASE		0x20504c00UL
#define ESP32S31_CPU_APM_FILTER_EN	(ESP32S31_CPU_APM_BASE + 0x00UL)
#define ESP32S31_CPU_APM_REGION0_START	(ESP32S31_CPU_APM_BASE + 0x04UL)
#define ESP32S31_CPU_APM_REGION0_END	(ESP32S31_CPU_APM_BASE + 0x08UL)
#define ESP32S31_CPU_APM_ATTR0		(ESP32S31_CPU_APM_BASE + 0x0cUL)
#define ESP32S31_CPU_APM_FUNC_CTRL	(ESP32S31_CPU_APM_BASE + 0xc4UL)

/* PMA cfg bit 27 makes the frame buffer uncached for the LCD DMA. */
#define ESP32S31_PMAADDR0		0xbd0
#define ESP32S31_PMACFG0		0xbc0
#define ESP32S31_PMA_NAPOT(base, size)	(((base) | (((size) >> 1) - 1)) >> 2)
#define ESP32S31_PMA_RWX		0xc000001dUL
#define ESP32S31_PMA_RW_UNCACHED	0xc8000019UL
#define ESP32S31_PSRAM_BASE		0x50000000UL
#define ESP32S31_PSRAM_SIZE		0x01000000UL

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
	unsigned long fw_end = ESP32S31_FW_BASE + ESP32S31_FW_SIZE;

	/* Regions 2 and 1 first, then shrink region 0: no gap in between. */
	reg_write(ESP32S31_HP_APM_START(2), fw_end);
	reg_write(ESP32S31_HP_APM_END(2), 0xffffffffUL);
	reg_write(ESP32S31_HP_APM_ATTR(2), ESP32S31_APM_REE_TEE_RWX | ESP32S31_APM_LOCK);
	reg_write(ESP32S31_HP_APM_START(1), ESP32S31_FW_BASE);
	reg_write(ESP32S31_HP_APM_END(1), fw_end - 1);
	reg_write(ESP32S31_HP_APM_ATTR(1), ESP32S31_APM_TEE_RWX | ESP32S31_APM_LOCK);
	reg_write(ESP32S31_HP_APM_FILTER_EN, 0x7);
	reg_write(ESP32S31_HP_APM_START(0), 0);
	reg_write(ESP32S31_HP_APM_END(0), ESP32S31_FW_BASE - 1);
	reg_write(ESP32S31_HP_APM_ATTR(0), ESP32S31_APM_REE_TEE_RWX | ESP32S31_APM_LOCK);

	reg_write(ESP32S31_HP_MEM_APM_ATTR0, ESP32S31_APM_REE_TEE_RWX);
	reg_write(ESP32S31_CPU_APM_REGION0_START, 0x00000000UL);
	reg_write(ESP32S31_CPU_APM_REGION0_END, 0xffffffffUL);
	reg_write(ESP32S31_CPU_APM_ATTR0, ESP32S31_APM_REE_TEE_RWX);
	reg_write(ESP32S31_CPU_APM_FILTER_EN, 1);
	reg_write(ESP32S31_CPU_APM_FUNC_CTRL, 0x0fUL);
}

static void esp32s31_pma_init(void)
{
	csr_write_num(ESP32S31_PMAADDR0 + 2,
		      ESP32S31_PMA_NAPOT(ESP32S31_PSRAM_BASE, ESP32S31_PSRAM_SIZE));
	csr_write_num(ESP32S31_PMACFG0 + 2, ESP32S31_PMA_RWX);
#if ESP32S31_FB_UNCACHED
	csr_write_num(ESP32S31_PMAADDR0,
		      ESP32S31_PMA_NAPOT(0x50f40000UL, 0x00040000UL));
	csr_write_num(ESP32S31_PMACFG0, ESP32S31_PMA_RW_UNCACHED);
	csr_write_num(ESP32S31_PMAADDR0 + 1,
		      ESP32S31_PMA_NAPOT(0x50f80000UL, 0x00080000UL));
	csr_write_num(ESP32S31_PMACFG0 + 1, ESP32S31_PMA_RW_UNCACHED);
#endif
}

#if ESP32S31_USB_SERIAL_JTAG_CONSOLE
static void esp32s31_usb_serial_jtag_putc(char ch)
{
	/* No host: TX_FREE stays clear.  After a timeout, poll once and drop. */
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

/* The doorbell of the firmware on hart 0: FROM_CPU_1, as Linux uses it. */
#define ESP32S31_DOORBELL_TO_FW		0x20586014UL

static int esp32s31_system_reset_check(u32 type, u32 reason)
{
	return type == SBI_SRST_RESET_TYPE_SHUTDOWN ||
	       type == SBI_SRST_RESET_TYPE_COLD_REBOOT ||
	       type == SBI_SRST_RESET_TYPE_WARM_REBOOT;
}

/* No power switch: ask the firmware on hart 0 for deep sleep. */
static void esp32s31_request_poweroff(void)
{
	atomic_raw_set_bit(__builtin_ctz(ESP_SHM_REQ_POWEROFF),
			   (volatile unsigned long *)(ESP_SHM_ADDR + ESP_SHM_CTRL +
						      ESP_SHM_HOST_REQ_OFF));
	reg_write(ESP32S31_DOORBELL_TO_FW, 1);
}

static void esp32s31_system_reset(u32 type, u32 reason)
{
	if (type == SBI_SRST_RESET_TYPE_SHUTDOWN)
		esp32s31_request_poweroff();
	else
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

	if (!cold_boot)
		return 0;

	sbi_system_reset_add_device(&esp32s31_reset);
	sbi_console_set_device(&esp32s31_console);

	/* HP_APM puts the last word below region 2 into region 2: DMA can write it. */
	if (sbi_scratch_thishart_ptr()->fw_size > ESP32S31_FW_SIZE - 4) {
		sbi_printf("esp32s31: firmware is larger than 0x%lx bytes\n",
			   ESP32S31_FW_SIZE - 4);
		return SBI_ENOMEM;
	}

	return sbi_domain_root_add_memrange(ESP32S31_TEE_APM_BASE,
					    ESP32S31_TEE_APM_SIZE,
					    ESP32S31_TEE_APM_SIZE,
					    SBI_DOMAIN_MEMREGION_MMIO |
					    SBI_DOMAIN_MEMREGION_M_READABLE |
					    SBI_DOMAIN_MEMREGION_M_WRITABLE);
}

static int esp32s31_final_init(bool cold_boot)
{
	if (!cold_boot)
		return 0;

	/* sbi_hart_switch_mode() keeps SPIE: set it, so Linux gets SIE = 1. */
	csr_set(CSR_MSTATUS, MSTATUS_SPIE);

	return 0;
}

/* The machine timer (docs/internals.md, "Timer and IPI"). */
#define ESP32S31_MTIMER_BASE		0x10000000UL
#define ESP32S31_MTIMECMP_LO		(ESP32S31_MTIMER_BASE + 0x4000UL)
#define ESP32S31_MTIMECMP_HI		(ESP32S31_MTIMER_BASE + 0x4004UL)
#define ESP32S31_MTIMECTL		(ESP32S31_MTIMER_BASE + 0x4010UL)
#define ESP32S31_MTIME_LO		(ESP32S31_MTIMER_BASE + 0xbff8UL)
#define ESP32S31_MTIME_HI		(ESP32S31_MTIMER_BASE + 0xbffcUL)
#define ESP32S31_MTIMER_FREQ		320000000UL

/* The measured layout (docs/internals.md, "Interrupts: the CLIC"). */
#define ESP32S31_MCLICCFG		0x10800000UL
#define ESP32S31_MCLICCFG_MNLBITS_3	(3UL << 0)
#define ESP32S31_MCLICCFG_NMBITS_2	(2UL << 4)
#define ESP32S31_MCLICCFG_VALUE \
	(ESP32S31_MCLICCFG_MNLBITS_3 | ESP32S31_MCLICCFG_NMBITS_2)

#define ESP32S31_CLIC_CTRL_BASE	0x10801000UL
#define ESP32S31_CLIC_IP(id)		(ESP32S31_CLIC_CTRL_BASE + 4UL * (id) + 0)
#define ESP32S31_CLIC_IE(id)		(ESP32S31_CLIC_CTRL_BASE + 4UL * (id) + 1)
#define ESP32S31_CLIC_ATTR(id)		(ESP32S31_CLIC_CTRL_BASE + 4UL * (id) + 2)
#define ESP32S31_CLIC_CTL(id)		(ESP32S31_CLIC_CTRL_BASE + 4UL * (id) + 3)

/* clicintattr: SHV bit 0, TRIG [2:1], MODE [7:6] (the mode that sees it). */
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
	/* Clear an old edge latch.  esp32s31_timer_init() set ATTR and IE. */
	reg_write8(ESP32S31_CLIC_IP(ESP32S31_CLIC_MTIMER_ID), 0);

	/* High word first, at its maximum: no match on a half-written value. */
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

/* MIP.STIP is not writable: the S-mode timer interrupt is CLIC input 5. */
void sbi_timer_plat_sirq_set(void)
{
	/* ATTR and CTL are configuration.  An interrupt only sets IP. */
	reg_write8(ESP32S31_CLIC_IP(ESP32S31_CLIC_STIMER_ID), 1);
}

void sbi_timer_plat_sirq_clear(void)
{
	reg_write8(ESP32S31_CLIC_IP(ESP32S31_CLIC_STIMER_ID), 0);
}

/* MIP.SSIP is not writable: SBI IPIs use CLIC input 1 (Linux does not). */
void sbi_ipi_plat_sirq_set(void)
{
	reg_write8(ESP32S31_CLIC_IP(ESP32S31_CLIC_SSOFT_ID), 1);
}

void sbi_ipi_plat_sirq_clear(void)
{
	reg_write8(ESP32S31_CLIC_IP(ESP32S31_CLIC_SSOFT_ID), 0);
}

/* Set up one local input at the maximum level, and enable it. */
static void esp32s31_clic_setup(int id, unsigned char attr)
{
	reg_write8(ESP32S31_CLIC_IP(id), 0);
	reg_write8(ESP32S31_CLIC_ATTR(id), attr);
	reg_write8(ESP32S31_CLIC_CTL(id), ESP32S31_CLIC_CTL_MAX);
	reg_write8(ESP32S31_CLIC_IE(id), 1);
}

static int esp32s31_timer_init(void)
{
	int i;

	reg_write(ESP32S31_MCLICCFG, ESP32S31_MCLICCFG_VALUE);
	csr_write(ESP32S31_CSR_MINTTHRESH, 0);
	csr_write(ESP32S31_CSR_SINTTHRESH, 0);

	/* All matrix inputs go to S-mode, disabled; the kernel sets the rest. */
	for (i = ESP32S31_CLIC_EXT_MIN_ID; i < ESP32S31_CLIC_NUM_INT; i++) {
		reg_write8(ESP32S31_CLIC_IE(i), 0);
		reg_write8(ESP32S31_CLIC_ATTR(i), ESP32S31_CLIC_ATTR_S_LEVEL);
	}

	/* Enable the S-mode timer: a payload without a CLIC driver gets it. */
	esp32s31_clic_setup(ESP32S31_CLIC_MTIMER_ID, ESP32S31_CLIC_ATTR_M_EDGE);
	esp32s31_clic_setup(ESP32S31_CLIC_STIMER_ID, ESP32S31_CLIC_ATTR_S_EDGE);
	esp32s31_clic_setup(ESP32S31_CLIC_SSOFT_ID, ESP32S31_CLIC_ATTR_S_EDGE);

	reg_write(ESP32S31_MTIMECTL, 1);
	esp32s31_timer_event_stop();
	sbi_timer_set_device(&esp32s31_timer);

	return 0;
}

/* The cache sync engine for Linux (docs/internals.md, "Cache and DMA"). */
#define ESP32S31_CACHE_BASE		0x2c000000UL
#define ESP32S31_CACHE_SYNC_CTRL	(ESP32S31_CACHE_BASE + 0x9cUL)
#define ESP32S31_CACHE_SYNC_MAP		(ESP32S31_CACHE_BASE + 0xa0UL)
#define ESP32S31_CACHE_SYNC_ADDR	(ESP32S31_CACHE_BASE + 0xa4UL)
#define ESP32S31_CACHE_SYNC_SIZE	(ESP32S31_CACHE_BASE + 0xa8UL)
#define ESP32S31_CACHE_INVALIDATE	(1UL << 0)
#define ESP32S31_CACHE_WRITEBACK	(1UL << 2)
#define ESP32S31_CACHE_WRITEBACK_INV	(1UL << 3)
#define ESP32S31_CACHE_SYNC_DONE	(1UL << 4)
#define ESP32S31_CACHE_MAP_L1_ICACHE(hart)	(1UL << (hart))
#define ESP32S31_CACHE_MAP_L1_DCACHE	(1UL << 4)

/* Vendor SBI function IDs.  Keep them the same as in the Linux driver. */
#define ESP32S31_SBI_CACHE_WBACK_ALL	0
#define ESP32S31_SBI_CACHE_INV		1
#define ESP32S31_SBI_CACHE_WBACK	2
#define ESP32S31_SBI_CACHE_WBACK_INV	3
#define ESP32S31_SBI_ICACHE_SYNC	4

#define ESP32S31_CACHE_LOCK \
	((volatile unsigned long *)(ESP_SHM_ADDR + ESP_SHM_CTRL + ESP_SHM_CACHE_LOCK_OFF))

/* Erratum: a single write-back can lose part of the range, so run it twice. */
static void esp32s31_cache_sync(unsigned long map, unsigned long op,
				unsigned long addr, unsigned long size)
{
	int pass, passes = op == ESP32S31_CACHE_INVALIDATE ? 1 : 2;

	while (atomic_raw_xchg_ulong(ESP32S31_CACHE_LOCK, 1))
		;
	reg_write(ESP32S31_CACHE_SYNC_MAP, map);
	reg_write(ESP32S31_CACHE_SYNC_ADDR, addr);
	reg_write(ESP32S31_CACHE_SYNC_SIZE, size);
	for (pass = 0; pass < passes; pass++) {
		reg_write(ESP32S31_CACHE_SYNC_CTRL, op);
		while (!(reg_read(ESP32S31_CACHE_SYNC_CTRL) &
			 ESP32S31_CACHE_SYNC_DONE))
			;
	}
	atomic_raw_xchg_ulong(ESP32S31_CACHE_LOCK, 0);
}

/* D-cache op, then I-cache op of this hart (fence.i does not invalidate). */
static const struct {
	unsigned long dcache_op;
	unsigned long icache_op;
} esp32s31_cache_ops[] = {
	[ESP32S31_SBI_CACHE_WBACK_ALL] = { ESP32S31_CACHE_WRITEBACK, 0 },
	[ESP32S31_SBI_CACHE_INV] = { ESP32S31_CACHE_INVALIDATE, 0 },
	[ESP32S31_SBI_CACHE_WBACK] = { ESP32S31_CACHE_WRITEBACK, 0 },
	[ESP32S31_SBI_CACHE_WBACK_INV] = { ESP32S31_CACHE_WRITEBACK_INV, 0 },
	[ESP32S31_SBI_ICACHE_SYNC] = { ESP32S31_CACHE_WRITEBACK,
				       ESP32S31_CACHE_INVALIDATE },
};

/* A size of 0 selects the full cache, as the ROM Cache_WriteBack_All() does. */
static int esp32s31_vendor_ext(long funcid, struct sbi_trap_regs *regs,
			       struct sbi_ecall_return *out)
{
	bool all = funcid == ESP32S31_SBI_CACHE_WBACK_ALL;
	unsigned long addr = all ? 0 : regs->a0;
	unsigned long size = all ? 0 : regs->a1;

	if (funcid < 0 || funcid >= array_size(esp32s31_cache_ops))
		return SBI_ENOTSUPP;

	esp32s31_cache_sync(ESP32S31_CACHE_MAP_L1_DCACHE,
			    esp32s31_cache_ops[funcid].dcache_op, addr, size);
	if (esp32s31_cache_ops[funcid].icache_op)
		esp32s31_cache_sync(ESP32S31_CACHE_MAP_L1_ICACHE(current_hartid()),
				    esp32s31_cache_ops[funcid].icache_op,
				    addr, size);
	return 0;
}

const struct sbi_platform_operations platform_ops = {
	.early_init = esp32s31_early_init,
	.final_init = esp32s31_final_init,
	.timer_init = esp32s31_timer_init,
	.vendor_ext_provider = esp32s31_vendor_ext,
};

/* Hart 0 runs the ESP-IDF firmware in M-mode; OpenSBI controls hart 1 only. */
static const u32 esp32s31_hart_index2id[] = { 1 };

const struct sbi_platform platform = {
	.opensbi_version = OPENSBI_VERSION,
	.platform_version = SBI_PLATFORM_VERSION(0x0, 0x01),
	.name = "ESP32-S31",
	.features = SBI_PLATFORM_DEFAULT_FEATURES,
	.hart_count = 1,
	.hart_index2id = esp32s31_hart_index2id,
	.hart_stack_size = SBI_PLATFORM_DEFAULT_HART_STACK_SIZE,
	.heap_size = SBI_PLATFORM_DEFAULT_HEAP_SIZE(1),
	.platform_ops_addr = (unsigned long)&platform_ops,
};
