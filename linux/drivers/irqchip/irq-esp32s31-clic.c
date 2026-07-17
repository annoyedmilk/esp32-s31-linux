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
 *   - CLICINTCTLBITS = 3, with CLICCFG.nlbits configured to 1
 *   - 32 external interrupts (CLIC IDs 16-47) routed via Interrupt Matrix
 *   - Per-core address virtualization: each core accesses its OWN
 *     registers at the *same* physical address.  The hardware remaps
 *     based on which core performs the access.  The +0x10000 offset
 *     accesses the OTHER core's registers (for cross-core IPI).
 *   - CLICINTCTL byte (offset 3 of per-int word):
 *       bit  [7]   = interrupt level (0-1)
 *       bits [6:5] = priority (three implemented control bits total)
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

/* CLIC register map */

#define ESP32S31_CLIC_BASE 0x10a00000	/* S-mode sclicbase window */

/*
 * Per-interrupt control registers start at 0x10a0_1000.
 * Each interrupt gets a 32-bit word with four byte-accessible sub-registers.
 * The same address is used by ALL cores - the hardware virtualises per core.
 * To access the OTHER core's registers, add 0x10000.
 */
#define ESP32S31_CLIC_CTRL_BASE 0x10a01000	/* S-mode per-interrupt window */
#define ESP32S31_CLIC_INT_STRIDE 4 /* 4 bytes per interrupt */
#define ESP32S31_CLIC_INT_IP 0x0 /* Interrupt pending (byte 0, bit 0) */
#define ESP32S31_CLIC_INT_IE 0x1 /* Interrupt enable  (byte 1, bit 0) */
#define ESP32S31_CLIC_INT_ATTR 0x2 /* Interrupt attributes (byte 2) */
#define ESP32S31_CLIC_INT_CTL 0x3 /* Interrupt level (byte 3, bits [7:5]) */

/* Interrupt numbering */

#define CLIC_EXT_MIN_ID 16 /* First external IRQ        */
#define CLIC_NR_IRQS 48 /* 16 local + 32 external    */
#define ESP32S31_CLIC_IRQ_WORK_ID CLIC_EXT_MIN_ID

/* CLIC configuration constants */

#define ESP32S31_CLICINTCTLBITS 3 /* Hardwired implemented control bits */
#define ESP32S31_CLICNLBITS 1 /* CLICCFG.nlbits established by firmware */
#define ESP32S31_MAX_PRIORITY 3 /* Two remaining implemented priority bits */

/* CLIC CSRs */
#ifndef CSR_SINTTHRESH
#define CSR_SINTTHRESH 0x147
#endif

/* clicintctl[i] encoding with CLICINTCTLBITS=3 and CLICCFG.nlbits=1 */
/*
 * clicintctl[i] byte (byte 3 of the per-interrupt 32-bit word):
 *   bit  [7]   = interrupt level (0-1)
 *   bits [6:5] = priority within that level (0-3)
 *
 * CLICINFO advertises three implemented control bits, but the live CLICCFG
 * value installed by firmware is 0x23: nmbits=1 and nlbits=1.  Treating all
 * three implemented bits as level bits encoded level 1 as 0x3f, whose bit 7
 * is clear.  Such interrupts remained at effective level 0 and could never
 * pass a zero threshold.  Put the configured level in bit 7 and priority in
 * the following two implemented bits.
 */
#define CLICCTL_MAKE(level, prio) \
	(((level) << (8 - ESP32S31_CLICNLBITS)) | \
	 ((prio) << (8 - ESP32S31_CLICINTCTLBITS)))

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
	 * Hardware-routed edge interrupts use write-one acknowledgement,
	 * matching ESP-IDF rv_utils_intr_edge_ack():
	 *   REG_SET_BIT(CLIC_INT_CTRL_REG(irq), CLIC_INT_IP);
	 *
	 * Slot 16 is not matrix-routed; Linux uses its IP bit as writable
	 * software-pending state for irq_work, so it is cleared with zero.
	 * Level-triggered inputs clear when the device deasserts its line.
	 */
	raw_spin_lock_irqsave(lock, flags);
	attr = clic_readb(clic, hwirq, ESP32S31_CLIC_INT_ATTR);
	if (attr & CLIC_ATTR_TRIG_EDGE) {
		/* The software-owned irq_work slot has a regular writable IP bit. */
		clic_writeb(clic, hwirq, ESP32S31_CLIC_INT_IP,
			    hwirq == ESP32S31_CLIC_IRQ_WORK_ID ? 0 : 1);
	}
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

static struct irq_chip esp32s31_clic_chip = {
	.name = "ESP32S31-CLIC",
	.irq_mask = esp32s31_clic_irq_mask,
	.irq_unmask = esp32s31_clic_irq_unmask,
	.irq_eoi = esp32s31_clic_irq_eoi,
	.irq_set_type = esp32s31_clic_set_type,
};

/* S-mode CLIC chip for the riscv,cpu-intc domain */
/*
 * On ESP32-S31 in CLIC mode the standard S-mode interrupt-enable CSRs
 * (sie/sieh) are illegal.  Local riscv,cpu-intc interrupts (timer, IPI)
 * must be masked/unmasked through the S-mode CLIC MMIO window instead.
 * This chip replaces the upstream riscv_intc_chip on the INTC domain.
 */

#if defined(CONFIG_IRQ_WORK) && !defined(CONFIG_SMP)
/*
 * Upstream RISC-V supplies arch_irq_work_raise() from smp.c, so a UP kernel
 * otherwise falls back to waiting for the next timer tick. That deadlocks
 * with NO_HZ_IDLE when the only runnable task is waiting for irq_work (tiny
 * SRCU does exactly this while replacing the boot console). CLIC local cause
 * 1 accepts IP writes on S31 v0.0 but is not arbitrated, so reserve the first
 * otherwise-unused external CLIC input as a software-owned self-interrupt.
 */
void arch_irq_work_raise(void)
{
	struct esp32s31_clic *clic = this_cpu_read(clic_per_cpu);

	/* Queued work is picked up by the first timer tick after probe. */
	if (!clic)
		return;

	clic_writeb(clic, ESP32S31_CLIC_IRQ_WORK_ID, ESP32S31_CLIC_INT_IP, 1);
}

static irqreturn_t esp32s31_irq_work_interrupt(int irq, void *dev_id)
{
	irq_work_run();
	return IRQ_HANDLED;
}

static int __init esp32s31_irq_work_init(struct esp32s31_clic *clic)
{
	int virq;
	int ret;

	/* Slot 16 has no interrupt-matrix source; its IP bit is software-owned. */
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

	if (source >= ESP32S31_INTMATRIX_CORE_STRIDE / sizeof(u32)) {
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

/*
 * ESP32-S31 interrupt specifiers are <raw CLIC ID, SoC interrupt source,
 * type>.  For example UART0 is <32 9 IRQ_TYPE_LEVEL_HIGH>, where source 9
 * is ETS_UART0_INTR_SOURCE in the S31 ESP-IDF table.
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
		return;
	}

	if (irq_id >= CLIC_NR_IRQS) {
		pr_warn_ratelimited("CLIC: spurious interrupt %lu\n", irq_id);
		return;
	}

	generic_handle_domain_irq(clic->domain, irq_id);
}

/* Initialization */

static void __init esp32s31_clic_init_hart(struct esp32s31_clic *clic, int hart)
{
	int i;

	csr_write(CSR_SINTTHRESH, 0);

	/*
	 * OpenSBI owns the local CLIC slots. Linux initializes only external
	 * inputs and leaves the machine and supervisor timer paths untouched.
	 */
	for (i = CLIC_EXT_MIN_ID; i < CLIC_NR_IRQS; i++) {
		clic_writeb(clic, i, ESP32S31_CLIC_INT_IP, 0);
		clic_writeb(clic, i, ESP32S31_CLIC_INT_IE, 0);
		clic_writeb(clic, i, ESP32S31_CLIC_INT_ATTR,
			    CLIC_ATTR_MODE_S | CLIC_ATTR_TRIG_LEVEL);
		clic_writeb(clic, i, ESP32S31_CLIC_INT_CTL,
			    CLICCTL_MAKE(1, ESP32S31_MAX_PRIORITY));
	}

	per_cpu(clic_per_cpu, hart) = clic;
}

static int __init esp32s31_clic_probe(struct device_node *node,
				     struct device_node *parent)
{
	struct fwnode_handle *intc_fwnode;
	struct irq_domain *intc_domain;
	struct esp32s31_clic *clic;
	struct resource res;
	int ret;

	clic = kzalloc_obj(*clic, GFP_KERNEL);
	if (!clic)
		return -ENOMEM;

	/*
	 * Map the full S-mode window even though the hardware virtualises
	 * per-core access: it covers the threshold register and allows
	 * future cross-core register access at +0x10000.
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

	clic->intmatrix_regs = ioremap(ESP32S31_INTMATRIX_BASE,
				       ESP32S31_INTMATRIX_SIZE);
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

IRQCHIP_DECLARE(esp32s31_clic, "espressif,esp32s31-clic", esp32s31_clic_probe);
