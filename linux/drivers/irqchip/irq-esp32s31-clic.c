// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Marco Müller <hello@annoyedmilk.ch>
 *
 * ESP32-S31 Core-Local Interrupt Controller (CLIC) driver
 *
 * Linux runs in S-mode and gets non-vectored interrupts through the
 * supervisor CLIC window.  OpenSBI controls the machine interrupts.
 *
 * Main ESP32-S31 CLIC data:
 *   - S-mode register base at 0x10a0_0000
 *   - S-mode per-interrupt control at 0x10a0_1000
 *   - CLICINTCTLBITS = 3, with CLICCFG.nlbits set to 1
 *   - 32 external interrupts (CLIC IDs 16-47) from the Interrupt Matrix
 */

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/irq_work.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/percpu.h>
#include <asm/csr.h>
#include <asm/esp32s31-clic.h>
#include <asm/irq.h>
#include <asm/smp.h>

/* CLIC register map */

#define ESP32S31_CLIC_BASE 0x10a00000	/* S-mode sclicbase window */

/*
 * The per-interrupt control registers start at 0x10a0_1000.  Each interrupt
 * has a 32-bit word with four byte registers.  All cores use the same
 * address, and each core gets its own registers.  The registers of the other
 * core are at +0x10000.
 */
#define ESP32S31_CLIC_CTRL_BASE 0x10a01000	/* S-mode per-interrupt window */
#define ESP32S31_CLIC_INT_STRIDE 4		/* Bytes per interrupt */
#define ESP32S31_CLIC_INT_IP 0x0		/* Pending (byte 0, bit 0) */
#define ESP32S31_CLIC_INT_IE 0x1		/* Enable (byte 1, bit 0) */
#define ESP32S31_CLIC_INT_ATTR 0x2		/* Attributes (byte 2) */
#define ESP32S31_CLIC_INT_CTL 0x3		/* Level (byte 3, bits [7:5]) */

/* Interrupt numbering */

#define CLIC_EXT_MIN_ID 16			/* First external IRQ */
#define CLIC_NR_IRQS 48				/* 16 local + 32 external */
#define ESP32S31_CLIC_IRQ_WORK_ID CLIC_EXT_MIN_ID

/*
 * Slot 16 is a software interrupt only when this driver reserves it for
 * irq_work.  Otherwise it is a usual Interrupt Matrix input with a usual ack.
 */
#if defined(CONFIG_IRQ_WORK) && !defined(CONFIG_SMP)
#define esp32s31_clic_is_irq_work(hwirq) ((hwirq) == ESP32S31_CLIC_IRQ_WORK_ID)
#else
#define esp32s31_clic_is_irq_work(hwirq) ((void)(hwirq), false)
#endif

/*
 * In CLIC mode, the low cause bits hold the interrupt ID.  The high bits hold
 * CLIC data, for example the previous interrupt level.  CAUSE_IRQ_FLAG does
 * not remove them, so use a mask to get the exception code.
 */
#define CLIC_CAUSE_EXCCODE_MASK 0xfff

/* CLIC configuration constants */

#define ESP32S31_CLICINTCTLBITS 3		/* Implemented control bits */
#define ESP32S31_CLICNLBITS 1			/* The firmware sets CLICCFG.nlbits */
#define ESP32S31_MAX_PRIORITY 3			/* The other two bits are priority */

/* CLIC CSRs */
#ifndef CSR_SINTTHRESH
#define CSR_SINTTHRESH 0x147
#endif

/*
 * clicintctl[i], byte 3 of the per-interrupt word: bit 7 is the level, bits
 * [6:5] are the priority.  CLICINFO shows three control bits, but the
 * firmware sets CLICCFG 0x23 (nmbits=1, nlbits=1).  Thus the level must be in
 * bit 7.  If the level uses all three bits, level 1 becomes 0x3f.  Then bit 7
 * is clear, and the input never passes a zero threshold.
 */
#define CLICCTL_MAKE(level, prio) \
	(((level) << (8 - ESP32S31_CLICNLBITS)) | \
	 ((prio) << (8 - ESP32S31_CLICINTCTLBITS)))

/*
 * Interrupt attribute bits (byte 2 of the per-interrupt word):
 *   bit 0:      SHV, 0 for non-vectored interrupts
 *   bits [2:1]: TRIG, trigger type
 *     X0 = level, 01 = rising edge, 11 = falling edge
 *   bits [7:6]: MODE, supervisor mode for the Linux inputs
 */
#define CLIC_ATTR_TRIG_LEVEL 0x00
#define CLIC_ATTR_TRIG_EDGE_RISE 0x02
#define CLIC_ATTR_TRIG_EDGE_FALL 0x06
#define CLIC_ATTR_TRIG_EDGE 0x02 /* bit 1: 1 = edge, 0 = level */
#define CLIC_ATTR_MODE_S 0x40

/*
 * The Interrupt Matrix connects SoC interrupt sources to CLIC inputs.  Each
 * core has its own block.  Linux maps only the block of its hart.  The other
 * block is for the M-mode firmware.
 */
#define ESP32S31_INTMATRIX_CORE_STRIDE 0x800
#define ESP32S31_INTMATRIX_MAP_MASK 0x3f
#define ESP32S31_INTMATRIX_PASS_LEVEL_SHIFT 8
#define ESP32S31_INTMATRIX_PASS_LEVEL_MASK (0x3 << ESP32S31_INTMATRIX_PASS_LEVEL_SHIFT)
#define ESP32S31_INTMATRIX_PASS_LEVEL_S (1 << ESP32S31_INTMATRIX_PASS_LEVEL_SHIFT)

/* Per-CPU CLIC structure */

struct esp32s31_clic {
	void __iomem *regs;
	void __iomem *intmatrix_regs;
	struct irq_domain *domain;
};

static DEFINE_PER_CPU(struct esp32s31_clic *, clic_per_cpu);
static DEFINE_PER_CPU(raw_spinlock_t, clic_lock);

/*
 * Each core finds its own per-interrupt registers at
 * ESP32S31_CLIC_CTRL_BASE + irq_id * 4 + byte_offset.  This driver does not
 * use the +0x10000 alias of the other core.
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

static void esp32s31_clic_irq_enable(struct irq_data *d, u8 enable)
{
	struct esp32s31_clic *clic = irq_data_get_irq_chip_data(d);
	raw_spinlock_t *lock = this_cpu_ptr(&clic_lock);
	unsigned long flags;

	raw_spin_lock_irqsave(lock, flags);
	clic_writeb(clic, d->hwirq, ESP32S31_CLIC_INT_IE, enable);
	raw_spin_unlock_irqrestore(lock, flags);
}

static void esp32s31_clic_irq_mask(struct irq_data *d)
{
	esp32s31_clic_irq_enable(d, 0);
}

static void esp32s31_clic_irq_unmask(struct irq_data *d)
{
	esp32s31_clic_irq_enable(d, 1);
}

static void esp32s31_clic_irq_eoi(struct irq_data *d)
{
	struct esp32s31_clic *clic = irq_data_get_irq_chip_data(d);
	raw_spinlock_t *lock = this_cpu_ptr(&clic_lock);
	u32 hwirq = d->hwirq;
	unsigned long flags;
	u8 attr;

	/*
	 * Write one to IP to ack an edge input, as ESP-IDF
	 * rv_utils_intr_edge_ack() does.  A level input clears when the device
	 * releases it.  Skip the irq_work slot: its handler clears IP before
	 * it runs the work.  An ack here would lose an item that set IP again
	 * in irq_work_run().
	 */
	if (esp32s31_clic_is_irq_work(hwirq))
		return;

	raw_spin_lock_irqsave(lock, flags);
	attr = clic_readb(clic, hwirq, ESP32S31_CLIC_INT_ATTR);
	if (attr & CLIC_ATTR_TRIG_EDGE)
		clic_writeb(clic, hwirq, ESP32S31_CLIC_INT_IP, 1);
	raw_spin_unlock_irqrestore(lock, flags);
}

static int esp32s31_clic_set_type(struct irq_data *d, unsigned int flow_type)
{
	struct esp32s31_clic *clic = irq_data_get_irq_chip_data(d);
	u32 hwirq = d->hwirq;
	unsigned long flags;
	u8 attr;

	/* The CLIC has no level-low mode, so the two Linux levels become level. */
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

	/* Linux uses only S-mode, non-vectored CLIC inputs. */
	attr |= CLIC_ATTR_MODE_S;

	raw_spin_lock_irqsave(this_cpu_ptr(&clic_lock), flags);
	clic_writeb(clic, hwirq, ESP32S31_CLIC_INT_ATTR, attr);
	raw_spin_unlock_irqrestore(this_cpu_ptr(&clic_lock), flags);

	return 0;
}

static struct irq_chip esp32s31_clic_chip = {
	.name = "ESP32S31-CLIC",
	.irq_mask = esp32s31_clic_irq_mask,
	.irq_unmask = esp32s31_clic_irq_unmask,
	.irq_eoi = esp32s31_clic_irq_eoi,
	.irq_set_type = esp32s31_clic_set_type,
};

/*
 * S-mode CLIC chip for the riscv,cpu-intc domain.
 *
 * On the ESP32-S31 in CLIC mode, the standard S-mode interrupt-enable CSRs
 * (sie/sieh) are illegal.  Thus mask and unmask the local riscv,cpu-intc
 * interrupts (timer, IPI) through the S-mode CLIC MMIO window.  This chip
 * replaces the upstream riscv_intc_chip on the INTC domain.
 */

#if defined(CONFIG_IRQ_WORK) && !defined(CONFIG_SMP)
/*
 * Upstream RISC-V has arch_irq_work_raise() in smp.c.  Thus a UP kernel
 * waits for the next timer tick.  With NO_HZ_IDLE, this is a deadlock when
 * the only task that can run waits for irq_work.  Tiny SRCU does this when it
 * replaces the boot console.  CLIC local cause 1 accepts IP writes on S31
 * v0.0, but the CLIC does not arbitrate it.  Thus use the first unused
 * external CLIC input as a software interrupt.
 */
void arch_irq_work_raise(void)
{
	struct esp32s31_clic *clic = this_cpu_read(clic_per_cpu);

	/* The first timer tick after the probe runs the queued work. */
	if (!clic)
		return;

	clic_writeb(clic, ESP32S31_CLIC_IRQ_WORK_ID, ESP32S31_CLIC_INT_IP, 1);
}

static irqreturn_t esp32s31_irq_work_interrupt(int irq, void *dev_id)
{
	struct esp32s31_clic *clic = dev_id;
	raw_spinlock_t *lock = this_cpu_ptr(&clic_lock);
	unsigned long flags;

	/*
	 * Clear the software pending bit before the work runs.  An item that
	 * adds more work sets IP again in irq_work_run(), and that IP must
	 * stay set.  For the same reason, the eoi callback skips this slot.
	 */
	raw_spin_lock_irqsave(lock, flags);
	clic_writeb(clic, ESP32S31_CLIC_IRQ_WORK_ID, ESP32S31_CLIC_INT_IP, 0);
	raw_spin_unlock_irqrestore(lock, flags);

	irq_work_run();

	return IRQ_HANDLED;
}

static int __init esp32s31_irq_work_init(struct esp32s31_clic *clic)
{
	int virq;
	int ret;

	/* Slot 16 has no Interrupt Matrix source.  Software sets its IP bit. */
	clic_writeb(clic, ESP32S31_CLIC_IRQ_WORK_ID, ESP32S31_CLIC_INT_IP, 0);
	clic_writeb(clic, ESP32S31_CLIC_IRQ_WORK_ID, ESP32S31_CLIC_INT_ATTR,
		    CLIC_ATTR_MODE_S | CLIC_ATTR_TRIG_EDGE_RISE);
	clic_writeb(clic, ESP32S31_CLIC_IRQ_WORK_ID, ESP32S31_CLIC_INT_CTL,
		    CLICCTL_MAKE(1, ESP32S31_MAX_PRIORITY));

	virq = irq_create_mapping(clic->domain, ESP32S31_CLIC_IRQ_WORK_ID);
	if (!virq)
		return -ENOMEM;

	ret = request_irq(virq, esp32s31_irq_work_interrupt, 0,
			  "esp32s31-irq-work", clic);
	if (ret) {
		irq_dispose_mapping(virq);
		return ret;
	}

	return 0;
}
#else
static int __init esp32s31_irq_work_init(struct esp32s31_clic *clic)
{
	return 0;
}
#endif

static void esp32s31_intc_clic_irq_mask(struct irq_data *d)
{
	clic_writeb(this_cpu_read(clic_per_cpu), d->hwirq,
		    ESP32S31_CLIC_INT_IE, 0);
}

static void esp32s31_intc_clic_irq_unmask(struct irq_data *d)
{
	clic_writeb(this_cpu_read(clic_per_cpu), d->hwirq,
		    ESP32S31_CLIC_INT_IE, 1);
}

static void esp32s31_intc_clic_irq_eoi(struct irq_data *d)
{
	/*
	 * Empty EOI, as riscv_intc_irq_eoi().  Without it,
	 * chained_irq_enter() and chained_irq_exit() in child irqchip drivers
	 * do mask and unmask cycles that are not necessary.
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

	if (source >= ESP32S31_INTMATRIX_CORE_STRIDE / sizeof(u32)) {
		pr_warn("CLIC: S31 interrupt source %u out of matrix range\n",
			source);
		return;
	}

	reg = clic->intmatrix_regs + source * sizeof(u32);
	val = readl(reg);
	val &= ~(ESP32S31_INTMATRIX_MAP_MASK |
		 ESP32S31_INTMATRIX_PASS_LEVEL_MASK);
	val |= (hwirq & ESP32S31_INTMATRIX_MAP_MASK) |
	       ESP32S31_INTMATRIX_PASS_LEVEL_S;
	writel(val, reg);
}

/*
 * ESP32-S31 interrupt specifiers are <CLIC ID, SoC interrupt source, type>.
 * For example, UART0 is <32 9 IRQ_TYPE_LEVEL_HIGH>.  Source 9 is
 * ETS_UART0_INTR_SOURCE in the S31 ESP-IDF table.
 */
static int esp32s31_clic_parse_fwspec(struct irq_fwspec *fwspec,
				     irq_hw_number_t *hwirq,
				     unsigned int *source,
				     unsigned int *type)
{
	if (fwspec->param_count != 3)
		return -EINVAL;

	*hwirq = fwspec->param[0];
	*source = fwspec->param[1];
	*type = fwspec->param[2] & IRQ_TYPE_SENSE_MASK;

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
	unsigned int source;

	return esp32s31_clic_parse_fwspec(fwspec, hwirq, &source, type);
}

static int esp32s31_clic_domain_alloc(struct irq_domain *domain,
				     unsigned int virq, unsigned int nr_irqs,
				     void *data)
{
	struct esp32s31_clic *clic = domain->host_data;
	struct irq_fwspec *fwspec = data;
	irq_hw_number_t hwirq;
	unsigned int source, type;
	unsigned long flags;

	if (esp32s31_clic_parse_fwspec(fwspec, &hwirq, &source, &type))
		return -EINVAL;

	if (hwirq >= CLIC_NR_IRQS)
		return -EINVAL;

	esp32s31_intmatrix_route(clic, source, hwirq);

	irq_domain_set_info(domain, virq, hwirq, &esp32s31_clic_chip, clic,
			    handle_fasteoi_irq, NULL, NULL);

	raw_spin_lock_irqsave(this_cpu_ptr(&clic_lock), flags);
	clic_writeb(clic, hwirq, ESP32S31_CLIC_INT_CTL,
		    CLICCTL_MAKE(1, ESP32S31_MAX_PRIORITY));
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

/*
 * The assembly trampoline calls this.  It already removed the synchronous
 * exceptions.  regs->cause has the unchanged CLIC cause.
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
	irq_id = raw_cause & CLIC_CAUSE_EXCCODE_MASK;

	/*
	 * Interrupt IDs below the first CLIC external source are standard
	 * local RISC-V interrupts for the riscv,cpu-intc domain.
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
		return;
	}

	if (irq_id >= CLIC_NR_IRQS) {
		pr_warn_ratelimited("CLIC: spurious interrupt %lu\n", irq_id);
		return;
	}

	generic_handle_domain_irq(clic->domain, irq_id);
}

/* Initialization */

static void __init esp32s31_clic_init_cpu(struct esp32s31_clic *clic, int cpu)
{
	int i;

	csr_write(CSR_SINTTHRESH, 0);

	/*
	 * OpenSBI controls the local CLIC slots.  Linux sets up only the
	 * external inputs.  It does not change the machine and supervisor
	 * timer inputs.
	 */
	for (i = CLIC_EXT_MIN_ID; i < CLIC_NR_IRQS; i++) {
		clic_writeb(clic, i, ESP32S31_CLIC_INT_IP, 0);
		clic_writeb(clic, i, ESP32S31_CLIC_INT_IE, 0);
		clic_writeb(clic, i, ESP32S31_CLIC_INT_ATTR,
			    CLIC_ATTR_MODE_S | CLIC_ATTR_TRIG_LEVEL);
		clic_writeb(clic, i, ESP32S31_CLIC_INT_CTL,
			    CLICCTL_MAKE(1, ESP32S31_MAX_PRIORITY));
	}

	per_cpu(clic_per_cpu, cpu) = clic;
}

static int __init esp32s31_clic_probe(struct device_node *node,
				     struct device_node *parent)
{
	struct fwnode_handle *intc_fwnode;
	struct irq_domain *intc_domain;
	struct esp32s31_clic *clic;
	struct resource res;
	unsigned long hart;
	int ret;

	clic = kzalloc_obj(*clic, GFP_KERNEL);
	if (!clic)
		return -ENOMEM;

	/*
	 * Map the full S-mode window.  It includes the threshold register and
	 * the registers of the other core at +0x10000.
	 */
	ret = of_address_to_resource(node, 0, &res);
	if (ret)
		goto err_free;

	clic->regs = ioremap(res.start, resource_size(&res));
	if (!clic->regs) {
		pr_err("CLIC: Failed to ioremap 0x%llx\n",
		       (unsigned long long)res.start);
		ret = -ENOMEM;
		goto err_free;
	}

	ret = of_address_to_resource(node, 1, &res);
	if (ret) {
		pr_err("CLIC: missing interrupt matrix resource\n");
		goto err_unmap;
	}

	hart = cpuid_to_hartid_map(0);
	if ((hart + 1) * ESP32S31_INTMATRIX_CORE_STRIDE > resource_size(&res)) {
		pr_err("CLIC: hart %lu has no interrupt matrix block\n", hart);
		ret = -EINVAL;
		goto err_unmap;
	}

	clic->intmatrix_regs = ioremap(res.start +
				       hart * ESP32S31_INTMATRIX_CORE_STRIDE,
				       ESP32S31_INTMATRIX_CORE_STRIDE);
	if (!clic->intmatrix_regs) {
		pr_err("CLIC: Failed to ioremap S31 interrupt matrix\n");
		ret = -ENOMEM;
		goto err_unmap;
	}

	clic->domain = irq_domain_add_linear(node, CLIC_NR_IRQS,
					     &esp32s31_clic_domain_ops, clic);
	if (!clic->domain) {
		pr_err("CLIC: Failed to create IRQ domain\n");
		ret = -ENOMEM;
		goto err_unmap;
	}

	esp32s31_clic_init_cpu(clic, 0);

	/* Use riscv_intc_irq() for the local causes (IDs < 16). */
	fallback_handle_irq = handle_arch_irq;
	handle_arch_irq = esp32s31_clic_handle_irq;

	intc_fwnode = riscv_get_intc_hwnode();
	intc_domain = intc_fwnode ?
		irq_find_matching_fwnode(intc_fwnode, DOMAIN_BUS_ANY) : NULL;
	if (intc_domain) {
		intc_domain->host_data = &esp32s31_intc_chip;
		ret = esp32s31_irq_work_init(clic);
		if (ret)
			pr_warn("CLIC: failed to initialize UP irq_work: %d\n",
				ret);
	} else {
		pr_warn("CLIC: no INTC domain - local IRQ masking unavailable\n");
	}

	pr_info("CLIC: initialized, %u interrupts, S-mode non-vectored\n",
		CLIC_NR_IRQS);

	return 0;

err_unmap:
	if (clic->intmatrix_regs)
		iounmap(clic->intmatrix_regs);
	iounmap(clic->regs);
err_free:
	kfree(clic);
	return ret;
}

IRQCHIP_DECLARE(esp32s31_clic, "esp,esp32s31-clic", esp32s31_clic_probe);
