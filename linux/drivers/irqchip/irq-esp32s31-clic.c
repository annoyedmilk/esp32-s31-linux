// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Marco Müller <hello@annoyedmilk.ch>
 *
 * ESP32-S31 Core-Local Interrupt Controller (CLIC) driver
 *
 * Linux runs in S-mode and receives non-vectored interrupts through the
 * supervisor CLIC window. OpenSBI owns the machine interrupt path.
 *
 * Key ESP32-S31 CLIC characteristics:
 *   - S-mode register base at 0x10a0_0000
 *   - S-mode per-interrupt control at 0x10a0_1000
 *   - CLICINTCTLBITS = 3, giving 8 interrupt levels
 *   - 32 external interrupts (CLIC IDs 16-47) routed via Interrupt Matrix
 *   - Per-core address virtualization: each core accesses its OWN
 *     registers at the *same* physical address.  The hardware remaps
 *     based on which core performs the access.  The +0x10000 offset
 *     accesses the OTHER core's registers (for cross-core IPI).
 *   - CLICINTCTL byte (offset 3 of per-int word):
 *       bits [7:5] = interrupt level (0-7, writable)
 *       bits [4:0] = not used for priority with CLICINTCTLBITS=3
 */

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/percpu.h>
#include <linux/hardirq.h>
#include <linux/smp.h>
#include <asm/csr.h>
#include <asm/esp32s31-clic.h>
#include <asm/irq.h>

int esp32s31_clic_set_priority(unsigned int irq, unsigned int level,
				      unsigned int prio);

/* CLIC register map */

#define ESP32S31_CLIC_BASE 0x10a00000	/* S-mode sclicbase window */
#define ESP32S31_CLIC_DUALCORE_OFF 0x10000
#define ESP32S31_CLIC_SIZE (ESP32S31_CLIC_DUALCORE_OFF * 2)

/*
 * Per-interrupt control registers start at 0x10a0_1000.
 * Each interrupt gets a 32-bit word with four byte-accessible sub-registers.
 * The same address is used by ALL cores - the hardware virtualises per core.
 * To access the OTHER core's registers, add ESP32S31_CLIC_DUALCORE_OFF.
 */
#define ESP32S31_CLIC_CTRL_BASE 0x10a01000	/* S-mode per-interrupt window */
#define ESP32S31_CLIC_INT_STRIDE 4 /* 4 bytes per interrupt */
#define ESP32S31_CLIC_INT_IP 0x0 /* Interrupt pending (byte 0, bit 0) */
#define ESP32S31_CLIC_INT_IE 0x1 /* Interrupt enable  (byte 1, bit 0) */
#define ESP32S31_CLIC_INT_ATTR 0x2 /* Interrupt attributes (byte 2) */
#define ESP32S31_CLIC_INT_CTL 0x3 /* Interrupt level (byte 3, bits [7:5]) */

/* Interrupt numbering */

#define CLIC_EXT_MIN_ID 16 /* First external IRQ        */
#define CLIC_EXT_MAX_ID 47 /* Last external IRQ         */
#define CLIC_MAX_ID 47 /* Max interrupt ID          */

/* CLIC configuration constants */

#define ESP32S31_CLICINTCTLBITS 3 /* Hardwired on S31 */
#define ESP32S31_NR_LEVELS (1 << ESP32S31_CLICINTCTLBITS) /* 8 */
#define ESP32S31_MAX_PRIORITY \
	31 /* Max sub-priority (unused at CLICINTCTLBITS=3) */

/* CLIC CSRs */
#ifndef CSR_SINTTHRESH
#define CSR_SINTTHRESH 0x147
#endif

/* clicintctl[i] level encoding with CLICINTCTLBITS=3 */
/*
 * clicintctl[i] byte (byte 3 of the per-interrupt 32-bit word):
 *   bits [7:5] = interrupt level  (0-7, writable per ESP-IDF)
 *   bits [4:0] = not used for priority discrimination at CLICINTCTLBITS=3
 *
 * Higher level = higher priority.  With CLICINTCTLBITS=3 there are
 * 8 distinct levels and sub-priority bits [4:0] do not affect ordering.
 */
#define CLICCTL_MAKE(level, prio) \
	(((level) << (8 - ESP32S31_CLICINTCTLBITS)) | ((prio) & 0x1F))

/* Interrupt attribute bits (byte 2 of per-interrupt word) */
/*
 *   bit 0:      SHV, left clear for non-vectored delivery
 *   bits [2:1]: TRIG, trigger type
 *     X0 = level, 01 = rising edge, 11 = falling edge
 *   bits [7:6]: MODE, set to supervisor mode for Linux-owned inputs
 */
#define CLIC_ATTR_TRIG_LEVEL 0x00 /* level-triggered           */
#define CLIC_ATTR_TRIG_EDGE_RISE 0x02 /* rising-edge triggered     */
#define CLIC_ATTR_TRIG_EDGE_FALL 0x06 /* falling-edge triggered    */
#define CLIC_ATTR_TRIG_EDGE 0x02 /* bit 1: 1 = edge, 0 = level */
#define CLIC_ATTR_MODE_S 0x40 /* S-mode on ESP32-S31 */

#define ESP32S31_INTMATRIX_BASE 0x20585000
#define ESP32S31_INTMATRIX_CORE_STRIDE 0x800
#define ESP32S31_INTMATRIX_SIZE (ESP32S31_INTMATRIX_CORE_STRIDE * 2)
#define ESP32S31_INTMATRIX_MAP_MASK 0x3f
#define ESP32S31_INTMATRIX_PASS_LEVEL_SHIFT 8
#define ESP32S31_INTMATRIX_PASS_LEVEL_MASK (0x3 << ESP32S31_INTMATRIX_PASS_LEVEL_SHIFT)
#define ESP32S31_INTMATRIX_PASS_LEVEL_S (1 << ESP32S31_INTMATRIX_PASS_LEVEL_SHIFT)

/* Per-CPU CLIC structure */

struct esp32s31_clic {
	void __iomem *regs;
	void __iomem *intmatrix_regs;
	struct irq_domain *domain;
	u32 num_interrupts;
};

static DEFINE_PER_CPU(struct esp32s31_clic *, clic_per_cpu);
static DEFINE_PER_CPU(raw_spinlock_t, clic_lock);

/* Register access helpers */
/*
 * The ESP32-S31 CLIC uses per-core address virtualisation.
 * Every core accesses its OWN per-interrupt registers at the same
 * physical address (ESP32S31_CLIC_CTRL_BASE + irq_id*4 + byte_offset).
 * The +0x10000 dual-core offset is ONLY used to access the OTHER core's
 * registers from a different core - never for the current core's own
 * registers.
 */
static inline u8 clic_readb(struct esp32s31_clic *clic, unsigned int irq_id,
			    unsigned int byte_off)
{
	return readb(clic->regs + ESP32S31_CLIC_CTRL_BASE - ESP32S31_CLIC_BASE +
		     (irq_id * ESP32S31_CLIC_INT_STRIDE) + byte_off);
}

static inline void clic_writeb(struct esp32s31_clic *clic, unsigned int irq_id,
			       unsigned int byte_off, u8 val)
{
	writeb(val, clic->regs + ESP32S31_CLIC_CTRL_BASE - ESP32S31_CLIC_BASE +
			    (irq_id * ESP32S31_CLIC_INT_STRIDE) + byte_off);
}

/* irq_chip callbacks */

static void esp32s31_clic_irq_mask(struct irq_data *d)
{
	struct esp32s31_clic *clic = irq_data_get_irq_chip_data(d);
	raw_spinlock_t *lock = this_cpu_ptr(&clic_lock);
	unsigned long flags;

	raw_spin_lock_irqsave(lock, flags);
	clic_writeb(clic, d->hwirq, ESP32S31_CLIC_INT_IE, 0);
	raw_spin_unlock_irqrestore(lock, flags);
}

static void esp32s31_clic_irq_unmask(struct irq_data *d)
{
	struct esp32s31_clic *clic = irq_data_get_irq_chip_data(d);
	raw_spinlock_t *lock = this_cpu_ptr(&clic_lock);
	unsigned long flags;

	raw_spin_lock_irqsave(lock, flags);
	clic_writeb(clic, d->hwirq, ESP32S31_CLIC_INT_IE, 1);
	raw_spin_unlock_irqrestore(lock, flags);
}

static void esp32s31_clic_irq_eoi(struct irq_data *d)
{
	struct esp32s31_clic *clic = irq_data_get_irq_chip_data(d);
	raw_spinlock_t *lock = this_cpu_ptr(&clic_lock);
	u32 hwirq = d->hwirq;
	u8 attr;
	unsigned long flags;

	/*
	 * For edge-triggered interrupts the ESP32-S31 CLIC uses
	 * write-1-to-clear semantics on the IP (Interrupt Pending)
	 * bit.  Writing 1 clears the edge-triggered pending latch;
	 * writing 0 has no effect.  Confirmed against ESP-IDF
	 * rv_utils_intr_edge_ack() which does:
	 *   REG_SET_BIT(CLIC_INT_CTRL_REG(irq), CLIC_INT_IP);
	 *
	 * Level-triggered interrupts are cleared by the device
	 * de-asserting its interrupt line; no software action needed.
	 *
	 * ATTR byte bit 1 (CLIC_ATTR_TRIG_EDGE) distinguishes
	 * edge (1) from level (0).
	 */
	raw_spin_lock_irqsave(lock, flags);
	attr = clic_readb(clic, hwirq, ESP32S31_CLIC_INT_ATTR);
	if (attr & CLIC_ATTR_TRIG_EDGE)
		clic_writeb(clic, hwirq, ESP32S31_CLIC_INT_IP, 0);
	raw_spin_unlock_irqrestore(lock, flags);
}

static int esp32s31_clic_set_type(struct irq_data *d, unsigned int flow_type)
{
	struct esp32s31_clic *clic = irq_data_get_irq_chip_data(d);
	u32 hwirq = d->hwirq;
	unsigned long flags;
	u8 attr;

	/*
	 * ESP32-S31 CLIC trigger encoding (ATTR byte bits [2:1]):
	 *   0bX0 = level-triggered
	 *   0b01 = rising-edge
	 *   0b11 = falling-edge
	 *
	 * The CLIC does not distinguish level-high from level-low.
	 * We map both Linux LEVEL_HIGH and LEVEL_LOW to level mode.
	 */
	switch (flow_type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_LEVEL_HIGH:
	case IRQ_TYPE_LEVEL_LOW:
		attr = CLIC_ATTR_TRIG_LEVEL;
		break;
	case IRQ_TYPE_EDGE_RISING:
		attr = CLIC_ATTR_TRIG_EDGE_RISE;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		attr = CLIC_ATTR_TRIG_EDGE_FALL;
		break;
	default:
		return -EINVAL;
	}

	/* Linux owns only S-mode, non-vectored CLIC inputs. */
	attr |= CLIC_ATTR_MODE_S;

	raw_spin_lock_irqsave(this_cpu_ptr(&clic_lock), flags);
	clic_writeb(clic, hwirq, ESP32S31_CLIC_INT_ATTR, attr);
	raw_spin_unlock_irqrestore(this_cpu_ptr(&clic_lock), flags);

	return 0;
}

static int esp32s31_clic_set_affinity(struct irq_data *d,
				     const struct cpumask *mask_val, bool force)
{
	/*
	 * CLIC is per-hart with address virtualisation.  Each hart
	 * manages its own interrupt enables/pending via the same
	 * register addresses.  Affinity changes require reprogramming
	 * the Interrupt Matrix to steer the source to a different core's
	 * CLIC input, then configuring the target core's CLIC registers.
	 * Not currently implemented.
	 */
	return -EINVAL;
}

/*
 * Set interrupt priority.  Higher level = higher priority.
 * Level range: 0 (lowest) to 7 (highest).
 * Priority sub-level: 0-31 within same level.
 *
 * Note: with CLICINTCTLBITS=3, only bits [7:5] (the level) affect
 * interrupt prioritisation.  The prio parameter is accepted but has
 * no hardware effect at this configuration (8 discrete levels).
 */
int esp32s31_clic_set_priority(unsigned int irq, unsigned int level,
			      unsigned int prio)
{
	struct irq_data *d = irq_get_irq_data(irq);
	struct esp32s31_clic *clic;
	unsigned long flags;

	if (!d || !d->chip_data)
		return -EINVAL;

	clic = irq_data_get_irq_chip_data(d);

	if (level >= ESP32S31_NR_LEVELS || prio > ESP32S31_MAX_PRIORITY)
		return -EINVAL;

	raw_spin_lock_irqsave(this_cpu_ptr(&clic_lock), flags);
	clic_writeb(clic, d->hwirq, ESP32S31_CLIC_INT_CTL,
		    CLICCTL_MAKE(level, prio));
	raw_spin_unlock_irqrestore(this_cpu_ptr(&clic_lock), flags);

	return 0;
}
EXPORT_SYMBOL_GPL(esp32s31_clic_set_priority);

static struct irq_chip esp32s31_clic_chip = {
	.name = "ESP32S31-CLIC",
	.irq_mask = esp32s31_clic_irq_mask,
	.irq_unmask = esp32s31_clic_irq_unmask,
	.irq_eoi = esp32s31_clic_irq_eoi,
	.irq_set_type = esp32s31_clic_set_type,
	.irq_set_affinity = esp32s31_clic_set_affinity,
};

/* S-mode CLIC chip for the riscv,cpu-intc domain */
/*
 * On ESP32-S31 in CLIC mode the standard S-mode interrupt-enable CSRs
 * (sie/sieh) are illegal.  Local riscv,cpu-intc interrupts (timer, IPI)
 * must be masked/unmasked through the S-mode CLIC MMIO window instead.
 * This chip replaces the upstream riscv_intc_chip on the INTC domain.
 */

static void __iomem *esp32s31_sclic_regs __ro_after_init;

#define ESP32S31_SCLIC_CTRL_OFF	0x1000	/* per-interrupt control offset from base */

static void esp32s31_intc_clic_irq_mask(struct irq_data *d)
{
	if (WARN_ON_ONCE(!esp32s31_sclic_regs))
		return;

	writeb(0, esp32s31_sclic_regs + ESP32S31_SCLIC_CTRL_OFF +
		  (d->hwirq * ESP32S31_CLIC_INT_STRIDE) +
		  ESP32S31_CLIC_INT_IE);
}

static void esp32s31_intc_clic_irq_unmask(struct irq_data *d)
{
	if (WARN_ON_ONCE(!esp32s31_sclic_regs))
		return;

	writeb(1, esp32s31_sclic_regs + ESP32S31_SCLIC_CTRL_OFF +
		  (d->hwirq * ESP32S31_CLIC_INT_STRIDE) +
		  ESP32S31_CLIC_INT_IE);
}

static void esp32s31_intc_clic_irq_eoi(struct irq_data *d)
{
	/*
	 * Empty EOI - matches riscv_intc_irq_eoi().  Required so that
	 * chained_irq_enter()/chained_irq_exit() in child irqchip drivers
	 * do not trigger unnecessary mask/unmask cycles.
	 */
}

static struct irq_chip esp32s31_intc_chip = {
	.name		= "ESP32-S31 CLIC local INTC",
	.irq_mask	= esp32s31_intc_clic_irq_mask,
	.irq_unmask	= esp32s31_intc_clic_irq_unmask,
	.irq_eoi	= esp32s31_intc_clic_irq_eoi,
};

/* IRQ domain operations */

static void esp32s31_intmatrix_route(struct esp32s31_clic *clic,
				     unsigned int source,
				     irq_hw_number_t hwirq)
{
	void __iomem *reg;
	u32 val;

	if (!clic->intmatrix_regs)
		return;

	if (source * 4 >= ESP32S31_INTMATRIX_CORE_STRIDE) {
		pr_warn("CLIC: S31 interrupt source %u out of matrix range\n",
			source);
		return;
	}

	reg = clic->intmatrix_regs + source * 4;
	val = readl(reg);
	val &= ~(ESP32S31_INTMATRIX_MAP_MASK |
		 ESP32S31_INTMATRIX_PASS_LEVEL_MASK);
	val |= (hwirq & ESP32S31_INTMATRIX_MAP_MASK) |
	       ESP32S31_INTMATRIX_PASS_LEVEL_S;
	writel(val, reg);
}

static int esp32s31_clic_parse_fwspec(struct irq_fwspec *fwspec,
				     irq_hw_number_t *hwirq,
				     unsigned int *source,
				     unsigned int *level,
				     unsigned int *type)
{
	if (fwspec->param_count != 1 && fwspec->param_count != 2 &&
	    fwspec->param_count != 3)
		return -EINVAL;

	*hwirq = fwspec->param[0];
	*source = UINT_MAX;
	*level = 1;
	*type = IRQ_TYPE_NONE;

	if (fwspec->param_count == 2)
		*type = fwspec->param[1] & IRQ_TYPE_SENSE_MASK;
	else if (fwspec->param_count == 3) {
		/*
		 * ESP32-S31 uses <raw CLIC ID, IDF interrupt source, type>.
		 * For example UART0 is <32 9 IRQ_TYPE_LEVEL_HIGH>, where
		 * source 9 is ETS_UART0_INTR_SOURCE in the S31 ESP-IDF table.
		 */
		*source = fwspec->param[1];
		*type = fwspec->param[2] & IRQ_TYPE_SENSE_MASK;
	}

	return 0;
}

static int esp32s31_clic_domain_map(struct irq_domain *d, unsigned int irq,
				   irq_hw_number_t hwirq)
{
	struct esp32s31_clic *clic = d->host_data;

	irq_domain_set_info(d, irq, hwirq, &esp32s31_clic_chip, clic,
			    handle_fasteoi_irq, NULL, NULL);

	return 0;
}

static int esp32s31_clic_domain_translate(struct irq_domain *domain,
					 struct irq_fwspec *fwspec,
					 irq_hw_number_t *hwirq,
					 unsigned int *type)
{
	unsigned int source, level;

	return esp32s31_clic_parse_fwspec(fwspec, hwirq, &source, &level, type);
}

static int esp32s31_clic_domain_alloc(struct irq_domain *domain,
				     unsigned int virq, unsigned int nr_irqs,
				     void *data)
{
	struct esp32s31_clic *clic = domain->host_data;
	struct irq_fwspec *fwspec = data;
	irq_hw_number_t hwirq;
	unsigned int source, level, type;
	unsigned long flags;

	/*
	 * The current ESP32-S31 DTS uses one-cell interrupt specifiers
	 * (<irq>). ESP32-S31 bring-up DTS uses three-cell specifiers:
	 * (<raw CLIC ID>, <IDF interrupt source>, <IRQ_TYPE_*>).
	 */
	if (esp32s31_clic_parse_fwspec(fwspec, &hwirq, &source, &level, &type))
		return -EINVAL;

	if (hwirq >= clic->num_interrupts)
		return -EINVAL;

	if (source != UINT_MAX)
		esp32s31_intmatrix_route(clic, source, hwirq);

	irq_domain_set_info(domain, virq, hwirq, &esp32s31_clic_chip, clic,
			    handle_fasteoi_irq, NULL, NULL);

	raw_spin_lock_irqsave(this_cpu_ptr(&clic_lock), flags);
	clic_writeb(clic, hwirq, ESP32S31_CLIC_INT_CTL,
		    CLICCTL_MAKE(level, ESP32S31_MAX_PRIORITY));
	raw_spin_unlock_irqrestore(this_cpu_ptr(&clic_lock), flags);

	if (type != IRQ_TYPE_NONE)
		esp32s31_clic_set_type(irq_get_irq_data(virq), type);

	return 0;
}

static void esp32s31_clic_domain_free(struct irq_domain *domain,
				     unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *data = irq_domain_get_irq_data(domain, virq);

	irq_domain_reset_irq_data(data);
}

static const struct irq_domain_ops esp32s31_clic_domain_ops = {
	.map = esp32s31_clic_domain_map,
	.alloc = esp32s31_clic_domain_alloc,
	.free = esp32s31_clic_domain_free,
	.translate = esp32s31_clic_domain_translate,
};

/* Interrupt handler */

/*
 * This is called from the assembly trampoline with:
 *   a0 = struct pt_regs * (saved context)
 *
 * The trampoline has already filtered synchronous exceptions. Interrupts
 * arrive here with raw mcause in regs->cause.
 */
static void (*fallback_handle_irq)(struct pt_regs *);

void esp32s31_clic_handle_irq(struct pt_regs *regs)
{
	struct esp32s31_clic *clic = this_cpu_read(clic_per_cpu);
	unsigned long raw_cause;
	unsigned long irq_id;

	if (WARN_ON(!clic))
		return;

	raw_cause = regs->cause;

	/*
	 * In CLIC mode the low cause bits hold the interrupt ID while the
	 * upper bits carry interrupt metadata such as the current level. The
	 * standard CAUSE_IRQ_FLAG mask does not strip those CLIC-specific bits,
	 * so decode only the cause code itself here.
	 */
	irq_id = regs->cause & 0xfff;

	/*
	 * Interrupt IDs below the first CLIC external source are standard local
	 * RISC-V interrupts routed through the riscv,cpu-intc domain.
	 */
	if (irq_id < CLIC_EXT_MIN_ID) {
		/* Clear pending state for S-mode edge-triggered local interrupts. */
		u8 attr = clic_readb(clic, irq_id, ESP32S31_CLIC_INT_ATTR);

		if (attr & CLIC_ATTR_TRIG_EDGE)
			clic_writeb(clic, irq_id, ESP32S31_CLIC_INT_IP, 0);
		regs->cause = CAUSE_IRQ_FLAG | irq_id;
		if (fallback_handle_irq)
			fallback_handle_irq(regs);
		regs->cause = raw_cause;
		goto out;
	}

	if (irq_id >= clic->num_interrupts) {
		pr_warn_ratelimited("CLIC: spurious interrupt %lu\n", irq_id);
		goto out;
	}

	generic_handle_domain_irq(clic->domain, irq_id);

	/*
	 * No claim/completion is needed. The hardware tracks the current
	 * interrupt level via sintstatus. When the trampoline executes sret,
	 * the CLIC restores the previous level and delivers eligible pending
	 * interrupts.
	 */

out:
	/*
	 * RISC-V enters this handler through do_irq()/handle_riscv_irq(),
	 * which already performs irqentry state management, irq_enter_rcu(),
	 * irq_exit_rcu(), and set_irq_regs() around handle_arch_irq().
	 * Repeating that work here corrupts nested IRQ accounting and can
	 * trip scheduler invariants during timer-driven reschedule.
	 */
}

/* Initialization */

static void __init esp32s31_clic_init_hart(struct esp32s31_clic *clic, int hart)
{
	const u8 mode_attr = CLIC_ATTR_MODE_S;
	int i;

	csr_write(CSR_SINTTHRESH, 0);

	/*
	 * OpenSBI owns the local CLIC slots. Linux initializes only external
	 * inputs and leaves the machine and supervisor timer paths untouched.
	 */
	for (i = CLIC_EXT_MIN_ID; i <= CLIC_EXT_MAX_ID; i++) {
		clic_writeb(clic, i, ESP32S31_CLIC_INT_IP, 0);
		clic_writeb(clic, i, ESP32S31_CLIC_INT_IE, 0);
		clic_writeb(clic, i, ESP32S31_CLIC_INT_ATTR,
			    mode_attr | CLIC_ATTR_TRIG_LEVEL);
		clic_writeb(clic, i, ESP32S31_CLIC_INT_CTL,
			    CLICCTL_MAKE(1, ESP32S31_MAX_PRIORITY));
	}

	per_cpu(clic_per_cpu, hart) = clic;

	pr_debug("CLIC: S31 hart %d using S-mode sclicbase window\n", hart);
}

static int __init esp32s31_clic_probe(struct device_node *node,
				     struct device_node *parent)
{
	struct esp32s31_clic *clic;
	struct resource res;
	u32 num_interrupts = CLIC_MAX_ID + 1;
	int ret;

	clic = kzalloc_obj(*clic, GFP_KERNEL);
	if (!clic)
		return -ENOMEM;

	/*
	 * Map CLIC registers. The S-mode window occupies 0x10a0_0000-0x10a1_ffff
	 * (two 64 KB windows for dual-core).  Falls back to the known
	 * base if no DT reg property.
	 *
	 * We map the full region even though the hardware virtualises
	 * per-core access. This covers the threshold register and
	 * allows future cross-core register access at +0x10000.
	 */
	ret = of_address_to_resource(node, 0, &res);
	if (ret) {
		res.start = ESP32S31_CLIC_BASE;
		res.end = ESP32S31_CLIC_BASE + ESP32S31_CLIC_SIZE - 1;
	}

	clic->regs = ioremap(res.start, resource_size(&res));
	if (!clic->regs) {
		pr_err("CLIC: Failed to ioremap 0x%llx\n",
		       (unsigned long long)res.start);
		ret = -ENOMEM;
		goto err_free;
	}

	clic->intmatrix_regs = ioremap(ESP32S31_INTMATRIX_BASE,
				       ESP32S31_INTMATRIX_SIZE);
	if (!clic->intmatrix_regs) {
		pr_err("CLIC: Failed to ioremap S31 interrupt matrix\n");
		ret = -ENOMEM;
		goto err_unmap;
	}

	/*
	 * The ESP32-S31 CLIC is hardwired to 48 interrupts (IDs 0-47)
	 * with two HP cores.
	 */

	clic->num_interrupts = 48;

	/* Create IRQ domain */
	clic->domain = irq_domain_add_linear(node, num_interrupts,
					     &esp32s31_clic_domain_ops, clic);
	if (!clic->domain) {
		pr_err("CLIC: Failed to create IRQ domain\n");
		ret = -ENOMEM;
		goto err_unmap;
	}

	/* Initialize hart 0 (boot CPU) */
	esp32s31_clic_init_hart(clic, 0);

	/*
	 * Take over as the top-level IRQ handler.  The riscv-intc has
	 * already registered riscv_intc_irq() via set_handle_irq().
	 * We capture it as fallback for local interrupts (IDs < 16)
	 * and install our CLIC handler which dispatches both local
	 * and external CLIC interrupts.
	 */
	fallback_handle_irq = handle_arch_irq;
	handle_arch_irq = esp32s31_clic_handle_irq;

	/*
	 * Map the S-mode CLIC window for local interrupt masking and
	 * swap the riscv,cpu-intc domain's irq_chip so that mask/unmask
	 * of local interrupts (timer, IPI) goes through the CLIC MMIO
	 * registers instead of the illegal sie/sieh CSRs.
	 */
	esp32s31_sclic_regs = ioremap(ESP32S31_CLIC_BASE, SZ_128K);
	if (!esp32s31_sclic_regs) {
		pr_err("CLIC: failed to ioremap S-mode CLIC window\n");
		ret = -ENOMEM;
		goto err_restore_handler;
	}
	{
		struct fwnode_handle *intc_fwnode = riscv_get_intc_hwnode();
		struct irq_domain *intc_domain;

		if (intc_fwnode) {
			intc_domain = irq_find_matching_fwnode(intc_fwnode,
							      DOMAIN_BUS_ANY);
			if (intc_domain)
				intc_domain->host_data = &esp32s31_intc_chip;
			else
				pr_warn("CLIC: could not find INTC domain to swap chip\n");
		} else {
			pr_warn("CLIC: no INTC hwnode - local IRQ masking may fail\n");
		}
	}

	pr_info("CLIC: initialized, %u interrupts, S-mode non-vectored\n",
		num_interrupts);

	return 0;

err_restore_handler:
	handle_arch_irq = fallback_handle_irq;
	fallback_handle_irq = NULL;
err_unmap:
	if (clic->intmatrix_regs)
		iounmap(clic->intmatrix_regs);
	iounmap(clic->regs);
err_free:
	kfree(clic);
	return ret;
}

IRQCHIP_DECLARE(esp32s31_clic, "espressif,esp32s31-clic", esp32s31_clic_probe);
