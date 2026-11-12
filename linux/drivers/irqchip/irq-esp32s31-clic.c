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
 *   - CLICINTCTLBITS = 3, with CLICCFG.snlbits set to 0
 *   - 32 external interrupts (CLIC IDs 16-47) from the Interrupt Matrix
 */

#define pr_fmt(fmt) "CLIC: " fmt

#include <linux/bitfield.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/irq_work.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <asm/csr.h>
#include <asm/irq.h>
#include <asm/smp.h>

/* CLIC register map */

/* A word of four byte registers for each input; the other core at +0x10000. */
#define ESP32S31_CLIC_CTRL_OFF 0x1000	/* per-interrupt window in sclicbase */
#define ESP32S31_CLIC_INT_STRIDE 4		/* Bytes per interrupt */
#define ESP32S31_CLIC_INT_IP 0x0		/* Pending (byte 0, bit 0) */
#define ESP32S31_CLIC_INT_IE 0x1		/* Enable (byte 1, bit 0) */
#define ESP32S31_CLIC_INT_ATTR 0x2		/* Attributes (byte 2) */
#define ESP32S31_CLIC_INT_CTL 0x3		/* Level, priority (byte 3, [7:5]) */

/* Interrupt numbering */

#define CLIC_EXT_MIN_ID 16			/* First external IRQ */
#define CLIC_NR_IRQS 48				/* 16 local + 32 external */
#define ESP32S31_CLIC_IRQ_WORK_ID CLIC_EXT_MIN_ID

/* CLIC mode keeps the level in the high cause bits: mask the exception code. */
#define CLIC_CAUSE_EXCCODE_MASK 0xfff

/* CLIC configuration constants */

#define ESP32S31_CLICINTCTLBITS 3		/* Implemented control bits */
#define ESP32S31_CLICNLBITS 1			/* Level bits of CLICCTL_MAKE() */
#define ESP32S31_MAX_PRIORITY 3			/* Bits [6:5] of CLICCTL_MAKE() */

/* CLIC CSRs */
#define CSR_SINTTHRESH 0x147

/* cliccfg 0x23 (snlbits 0): the bits are priority; 0xe0 keeps level 255. */
#define CLICCTL_MAKE(level, prio) \
	(((level) << (8 - ESP32S31_CLICNLBITS)) | \
	 ((prio) << (8 - ESP32S31_CLICINTCTLBITS)))

/* Byte 2: SHV bit 0, TRIG [2:1] (X0 level, 01 rise, 11 fall), MODE [7:6]. */
#define CLIC_ATTR_TRIG_LEVEL 0x00
#define CLIC_ATTR_TRIG_EDGE 0x02 /* rising edge; bit 1 tells edge from level */
#define CLIC_ATTR_MODE_S 0x40

/* The Interrupt Matrix block of this hart; the other is for the firmware. */
#define ESP32S31_INTMATRIX_CORE_STRIDE 0x800
#define ESP32S31_INTMATRIX_MAP_MASK 0x3f
#define ESP32S31_INTMATRIX_PASS_LEVEL GENMASK(9, 8)
#define ESP32S31_INTMATRIX_PASS_LEVEL_S 1

/* Linux runs on one hart, so there is one CLIC. */

struct esp32s31_clic {
	void __iomem *regs;
	void __iomem *intmatrix_regs;
	struct irq_domain *domain;
};

static struct esp32s31_clic *clic;

/* The per-interrupt registers of this core; the +0x10000 alias is not used. */
static inline u8 clic_readb(struct esp32s31_clic *clic, unsigned int irq_id,
			    unsigned int byte_off)
{
	return readb(clic->regs + ESP32S31_CLIC_CTRL_OFF +
		     (irq_id * ESP32S31_CLIC_INT_STRIDE) + byte_off);
}

static inline void clic_writeb(struct esp32s31_clic *clic, unsigned int irq_id,
			       unsigned int byte_off, u8 val)
{
	writeb(val, clic->regs + ESP32S31_CLIC_CTRL_OFF +
	       (irq_id * ESP32S31_CLIC_INT_STRIDE) + byte_off);
}

/* irq_chip callbacks */

static void esp32s31_clic_irq_enable(struct irq_data *d, u8 enable)
{
	clic_writeb(clic, d->hwirq, ESP32S31_CLIC_INT_IE, enable);
}

static void esp32s31_clic_irq_mask(struct irq_data *d)
{
	esp32s31_clic_irq_enable(d, 0);
}

static void esp32s31_clic_irq_unmask(struct irq_data *d)
{
	esp32s31_clic_irq_enable(d, 1);
}

/* Empty, as riscv_intc_irq_eoi(): chained_irq_enter() then skips the mask. */
static void esp32s31_clic_irq_eoi(struct irq_data *d)
{
}

static int esp32s31_clic_set_type(struct irq_data *d, unsigned int flow_type)
{
	/* The CLIC has no level-low: the two Linux levels become level. */
	if (!(flow_type & IRQ_TYPE_LEVEL_MASK))
		return -EINVAL;

	clic_writeb(clic, d->hwirq, ESP32S31_CLIC_INT_ATTR,
		    CLIC_ATTR_MODE_S | CLIC_ATTR_TRIG_LEVEL);
	return 0;
}

static struct irq_chip esp32s31_clic_chip = {
	.name = "ESP32S31-CLIC",
	.irq_mask = esp32s31_clic_irq_mask,
	.irq_unmask = esp32s31_clic_irq_unmask,
	.irq_eoi = esp32s31_clic_irq_eoi,
	.irq_set_type = esp32s31_clic_set_type,
};

#if defined(CONFIG_IRQ_WORK) && !defined(CONFIG_SMP)
/* UP RISC-V has no self-interrupt; input 1 does not arbitrate: use 16. */
void arch_irq_work_raise(void)
{
	/* Before the probe: esp32s31_irq_work_init() raises it once. */
	if (!clic)
		return;

	clic_writeb(clic, ESP32S31_CLIC_IRQ_WORK_ID, ESP32S31_CLIC_INT_IP, 1);
}

static irqreturn_t esp32s31_irq_work_interrupt(int irq, void *dev_id)
{
	/* Clear IP before the work runs: an item that adds work sets it. */
	clic_writeb(clic, ESP32S31_CLIC_IRQ_WORK_ID, ESP32S31_CLIC_INT_IP, 0);

	irq_work_run();

	return IRQ_HANDLED;
}

static int __init esp32s31_irq_work_init(void)
{
	int virq;
	int ret;

	/* Slot 16 has no Interrupt Matrix source.  Software sets its IP bit. */
	clic_writeb(clic, ESP32S31_CLIC_IRQ_WORK_ID, ESP32S31_CLIC_INT_IP, 0);
	clic_writeb(clic, ESP32S31_CLIC_IRQ_WORK_ID, ESP32S31_CLIC_INT_ATTR,
		    CLIC_ATTR_MODE_S | CLIC_ATTR_TRIG_EDGE);

	virq = irq_create_mapping(clic->domain, ESP32S31_CLIC_IRQ_WORK_ID);
	if (!virq)
		return -ENOMEM;

	ret = request_irq(virq, esp32s31_irq_work_interrupt, 0,
			  "esp32s31-irq-work", NULL);
	if (ret) {
		irq_dispose_mapping(virq);
		return ret;
	}

	/* The tick does not run raised work here: run what came before. */
	clic_writeb(clic, ESP32S31_CLIC_IRQ_WORK_ID, ESP32S31_CLIC_INT_IP, 1);
	return 0;
}
#else
static int __init esp32s31_irq_work_init(void)
{
	return 0;
}
#endif

/* sie/sieh are illegal in CLIC mode: mask the local causes in the CLIC. */
static struct irq_chip esp32s31_intc_chip = {
	.name		= "ESP32-S31 CLIC local INTC",
	.irq_mask	= esp32s31_clic_irq_mask,
	.irq_unmask	= esp32s31_clic_irq_unmask,
	.irq_eoi	= esp32s31_clic_irq_eoi,
};

/* IRQ domain operations */

static void esp32s31_intmatrix_route(unsigned int source, irq_hw_number_t hwirq)
{
	void __iomem *reg;
	u32 val;

	if (source >= ESP32S31_INTMATRIX_CORE_STRIDE / sizeof(u32)) {
		pr_warn("S31 interrupt source %u out of matrix range\n", source);
		return;
	}

	reg = clic->intmatrix_regs + source * sizeof(u32);
	val = readl(reg);
	val &= ~(ESP32S31_INTMATRIX_MAP_MASK | ESP32S31_INTMATRIX_PASS_LEVEL);
	val |= (hwirq & ESP32S31_INTMATRIX_MAP_MASK) |
	       FIELD_PREP(ESP32S31_INTMATRIX_PASS_LEVEL, ESP32S31_INTMATRIX_PASS_LEVEL_S);
	writel(val, reg);
}

/* <CLIC ID, SoC source, type>: UART0 is <32 9 IRQ_TYPE_LEVEL_HIGH>. */
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
	irq_domain_set_info(d, irq, hwirq, &esp32s31_clic_chip, NULL,
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
	struct irq_fwspec *fwspec = data;
	irq_hw_number_t hwirq;
	unsigned int source, type;

	if (esp32s31_clic_parse_fwspec(fwspec, &hwirq, &source, &type))
		return -EINVAL;

	if (hwirq >= CLIC_NR_IRQS)
		return -EINVAL;

	/* The probe set the control and the level attribute of each ID. */
	esp32s31_intmatrix_route(source, hwirq);
	return esp32s31_clic_domain_map(domain, virq, hwirq);
}

static const struct irq_domain_ops esp32s31_clic_domain_ops = {
	.map = esp32s31_clic_domain_map,
	.alloc = esp32s31_clic_domain_alloc,
	.free = irq_domain_free_irqs_top,
	.translate = esp32s31_clic_domain_translate,
};

/* do_irq() calls this with the unchanged CLIC cause in regs->cause. */
static void (*fallback_handle_irq)(struct pt_regs *);

static void esp32s31_clic_handle_irq(struct pt_regs *regs)
{
	unsigned long raw_cause;
	unsigned long irq_id;

	raw_cause = regs->cause;
	irq_id = raw_cause & CLIC_CAUSE_EXCCODE_MASK;

	/* IDs below 16 are the local interrupts of the riscv,cpu-intc domain. */
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
		pr_warn_ratelimited("spurious interrupt %lu\n", irq_id);
		return;
	}

	generic_handle_domain_irq(clic->domain, irq_id);
}

/* Initialization */

static void __init esp32s31_clic_init(struct esp32s31_clic *c)
{
	int i;

	csr_write(CSR_SINTTHRESH, 0);

	/* OpenSBI controls the local inputs; set up only the external ones. */
	for (i = CLIC_EXT_MIN_ID; i < CLIC_NR_IRQS; i++) {
		clic_writeb(c, i, ESP32S31_CLIC_INT_IP, 0);
		clic_writeb(c, i, ESP32S31_CLIC_INT_IE, 0);
		clic_writeb(c, i, ESP32S31_CLIC_INT_ATTR,
			    CLIC_ATTR_MODE_S | CLIC_ATTR_TRIG_LEVEL);
		clic_writeb(c, i, ESP32S31_CLIC_INT_CTL,
			    CLICCTL_MAKE(1, ESP32S31_MAX_PRIORITY));
	}
}

static int __init esp32s31_clic_probe(struct device_node *node,
				     struct device_node *parent)
{
	struct fwnode_handle *intc_fwnode;
	struct irq_domain *intc_domain;
	struct esp32s31_clic *c;
	struct resource res;
	unsigned long hart;
	int ret;

	c = kzalloc_obj(*c, GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	/* The full S-mode window: threshold register and the other core too. */
	c->regs = of_iomap(node, 0);
	if (!c->regs) {
		pr_err("cannot map the CLIC\n");
		ret = -ENOMEM;
		goto err_free;
	}

	ret = of_address_to_resource(node, 1, &res);
	if (ret) {
		pr_err("missing interrupt matrix resource\n");
		goto err_unmap;
	}

	hart = cpuid_to_hartid_map(0);
	if ((hart + 1) * ESP32S31_INTMATRIX_CORE_STRIDE > resource_size(&res)) {
		pr_err("hart %lu has no interrupt matrix block\n", hart);
		ret = -EINVAL;
		goto err_unmap;
	}

	c->intmatrix_regs = ioremap(res.start +
				    hart * ESP32S31_INTMATRIX_CORE_STRIDE,
				    ESP32S31_INTMATRIX_CORE_STRIDE);
	if (!c->intmatrix_regs) {
		pr_err("Failed to ioremap S31 interrupt matrix\n");
		ret = -ENOMEM;
		goto err_unmap;
	}

	c->domain = irq_domain_create_linear(of_fwnode_handle(node),
					     CLIC_NR_IRQS,
					     &esp32s31_clic_domain_ops, NULL);
	if (!c->domain) {
		pr_err("Failed to create IRQ domain\n");
		ret = -ENOMEM;
		goto err_unmap;
	}

	/* Set the pointer after the init: arch_irq_work_raise() reads it. */
	esp32s31_clic_init(c);
	clic = c;

	/* Use riscv_intc_irq() for the local causes (IDs < 16). */
	fallback_handle_irq = handle_arch_irq;
	handle_arch_irq = esp32s31_clic_handle_irq;

	intc_fwnode = riscv_get_intc_hwnode();
	intc_domain = intc_fwnode ?
		irq_find_matching_fwnode(intc_fwnode, DOMAIN_BUS_ANY) : NULL;
	if (intc_domain) {
		/* riscv-intc masks with sie/sieh, illegal in CLIC mode: use our chip. */
		intc_domain->host_data = &esp32s31_intc_chip;
		ret = esp32s31_irq_work_init();
		if (ret)
			pr_warn("failed to initialize UP irq_work: %d\n", ret);
	} else {
		pr_warn("no INTC domain - local IRQ masking unavailable\n");
	}

	pr_info("initialized, %u interrupts, S-mode non-vectored\n",
		CLIC_NR_IRQS);

	return 0;

err_unmap:
	if (c->intmatrix_regs)
		iounmap(c->intmatrix_regs);
	iounmap(c->regs);
err_free:
	kfree(c);
	return ret;
}

IRQCHIP_DECLARE(esp32s31_clic, "esp,esp32s31-clic", esp32s31_clic_probe);
