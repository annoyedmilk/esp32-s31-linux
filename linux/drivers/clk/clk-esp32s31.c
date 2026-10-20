// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Marco Mueller <hello@annoyedmilk.ch>
 *
 * ESP32-S31 clock and reset controllers
 *
 * Three blocks have clocks for Linux.  LP_AONCLKRST has the MPLL, which the
 * firmware sets for the PSRAM, so Linux only reads its rate.  HP_SYS_CLKRST
 * has the CPU, SYS and APB clocks, which the firmware sets, and a gate for
 * each peripheral.  CNNT_SYS has the EMAC TX clock and reset, and the EMAC
 * interface registers, which the EMAC glue gets as a syscon.
 *
 * The firmware on hart 0 does not write these registers after it starts
 * Linux.  The USB PHY maps single registers of HP_SYS_CLKRST and CNNT_SYS,
 * so this driver does not request the regions.
 */

#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/math.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>
#include <linux/spinlock.h>

#include <dt-bindings/clock/esp32s31-clk.h>

#define LP_MSPI_DIV			0x54
#define LP_MSPI_REF_DIV			GENMASK(2, 0)
#define LP_MSPI_FB_DIV			GENMASK(7, 3)

#define HP_SOC_CLK_SEL			0x00
#define HP_CPU_FREQ_CTRL0		0x04
#define HP_SYS_FREQ_CTRL0		0x0c
#define HP_APB_FREQ_CTRL0		0x10
#define HP_EMAC_CTRL0			0xc8
#define HP_FREQ_DIV_NUM			GENMASK(7, 0)
#define HP_FREQ_NUMERATOR		GENMASK(10, 8)
#define HP_FREQ_DENOMINATOR		GENMASK(13, 11)

#define CNNT_EMAC_CTRL			0x3c
#define CNNT_EMAC_RST_EN		BIT(1)
#define CNNT_EMAC_REF_CTRL		0x40
#define CNNT_SIZE			0x1000

struct esp32s31_clk {
	struct device *dev;
	void __iomem *base;
	struct clk_hw_onecell_data *hws;
	/* For the read-modify-write of the registers */
	spinlock_t lock;
	struct reset_controller_dev rcdev;
};

/* The MPLL and the root dividers are set by the firmware: read only. */
struct esp32s31_ro_clk {
	struct clk_hw hw;
	void __iomem *reg;
};

#define to_ro_clk(_hw) container_of(_hw, struct esp32s31_ro_clk, hw)

static unsigned long esp32s31_mpll_recalc_rate(struct clk_hw *hw,
					       unsigned long parent_rate)
{
	u32 val = readl(to_ro_clk(hw)->reg);

	return parent_rate * (FIELD_GET(LP_MSPI_FB_DIV, val) + 1) /
	       (FIELD_GET(LP_MSPI_REF_DIV, val) + 1);
}

static const struct clk_ops esp32s31_mpll_ops = {
	.recalc_rate = esp32s31_mpll_recalc_rate,
};

/* rate = parent / (div + numerator / denominator), div = DIV_NUM + 1 */
static unsigned long esp32s31_frac_recalc_rate(struct clk_hw *hw,
					       unsigned long parent_rate)
{
	u32 val = readl(to_ro_clk(hw)->reg);
	u32 div = FIELD_GET(HP_FREQ_DIV_NUM, val) + 1;
	u32 num = FIELD_GET(HP_FREQ_NUMERATOR, val);
	u32 den = FIELD_GET(HP_FREQ_DENOMINATOR, val);

	if (!den)
		return parent_rate / div;

	return mult_frac(parent_rate, den, div * den + num);
}

static const struct clk_ops esp32s31_frac_ops = {
	.recalc_rate = esp32s31_frac_recalc_rate,
};

static struct clk_hw *esp32s31_ro_register(struct esp32s31_clk *ec,
					   const char *name,
					   const struct clk_parent_data *parent,
					   const struct clk_ops *ops,
					   unsigned int offset)
{
	struct clk_init_data init = {
		.name = name,
		.ops = ops,
		.parent_data = parent,
		.num_parents = 1,
	};
	struct esp32s31_ro_clk *clk;
	int ret;

	clk = devm_kzalloc(ec->dev, sizeof(*clk), GFP_KERNEL);
	if (!clk)
		return ERR_PTR(-ENOMEM);

	clk->reg = ec->base + offset;
	clk->hw.init = &init;
	ret = devm_clk_hw_register(ec->dev, &clk->hw);
	if (ret)
		return ERR_PTR(ret);

	return &clk->hw;
}

static int esp32s31_lp_init(struct esp32s31_clk *ec)
{
	static const struct clk_parent_data xtal = { .fw_name = "xtal" };

	ec->hws->hws[ESP32S31_LP_CLK_MPLL] =
		esp32s31_ro_register(ec, "mpll", &xtal, &esp32s31_mpll_ops,
				     LP_MSPI_DIV);
	return 0;
}

static int esp32s31_hp_init(struct esp32s31_clk *ec)
{
	static const struct clk_parent_data root[] = {
		{ .fw_name = "xtal" },
		{ .fw_name = "cpll" },
		{ .fw_name = "rc-fast" },
		{ .fw_name = "pll-f240m" },
	};
	/* Each one is the parent of the next. */
	static const struct {
		const char *name;
		unsigned int id;
		unsigned int offset;
	} chain[] = {
		{ "cpu", ESP32S31_CLK_CPU, HP_CPU_FREQ_CTRL0 },
		{ "sys", ESP32S31_CLK_SYS, HP_SYS_FREQ_CTRL0 },
		{ "apb", ESP32S31_CLK_APB, HP_APB_FREQ_CTRL0 },
	};
	struct clk_hw **hws = ec->hws->hws;
	struct clk_parent_data parent = {};
	struct clk_hw *hw;
	unsigned int i;

	hw = devm_clk_hw_register_mux_parent_data_table(ec->dev, "soc-root",
							root, ARRAY_SIZE(root),
							0, ec->base + HP_SOC_CLK_SEL,
							0, 2, CLK_MUX_READ_ONLY,
							NULL, &ec->lock);
	for (i = 0; i < ARRAY_SIZE(chain) && !IS_ERR(hw); i++) {
		parent.hw = hw;
		hw = esp32s31_ro_register(ec, chain[i].name, &parent,
					  &esp32s31_frac_ops, chain[i].offset);
		hws[chain[i].id] = hw;
	}
	if (IS_ERR(hw))
		return PTR_ERR(hw);

	hws[ESP32S31_CLK_EMAC_SYS] =
		devm_clk_hw_register_gate_parent_hw(ec->dev, "emac-sys",
						    hws[ESP32S31_CLK_SYS], 0,
						    ec->base + HP_EMAC_CTRL0,
						    0, 0, &ec->lock);
	return 0;
}

static struct esp32s31_clk *to_esp32s31_clk(struct reset_controller_dev *rcdev)
{
	return container_of(rcdev, struct esp32s31_clk, rcdev);
}

static int esp32s31_cnnt_reset_update(struct reset_controller_dev *rcdev,
				      unsigned long id, bool assert)
{
	struct esp32s31_clk *ec = to_esp32s31_clk(rcdev);
	void __iomem *reg = ec->base + CNNT_EMAC_CTRL;
	u32 val;

	guard(spinlock_irqsave)(&ec->lock);
	val = readl(reg) & ~CNNT_EMAC_RST_EN;
	writel(val | (assert ? CNNT_EMAC_RST_EN : 0), reg);
	return 0;
}

static int esp32s31_cnnt_reset_assert(struct reset_controller_dev *rcdev,
				      unsigned long id)
{
	return esp32s31_cnnt_reset_update(rcdev, id, true);
}

static int esp32s31_cnnt_reset_deassert(struct reset_controller_dev *rcdev,
					unsigned long id)
{
	return esp32s31_cnnt_reset_update(rcdev, id, false);
}

static const struct reset_control_ops esp32s31_cnnt_reset_ops = {
	.assert = esp32s31_cnnt_reset_assert,
	.deassert = esp32s31_cnnt_reset_deassert,
};

static const struct regmap_config esp32s31_cnnt_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = CNNT_SIZE - 4,
};

static int esp32s31_cnnt_init(struct esp32s31_clk *ec)
{
	static const struct clk_parent_data ref[] = {
		{ .fw_name = "mpll" },
		{ .fw_name = "cpll" },
		{ .fw_name = "apll" },
	};
	void __iomem *reg = ec->base + CNNT_EMAC_REF_CTRL;
	struct device *dev = ec->dev;
	struct regmap *regmap;
	struct clk_hw *hw;
	int ret;

	/* EMAC_REF, the RGMII TX clock: source, divider (DIV_NUM + 1), gate */
	hw = devm_clk_hw_register_mux_parent_data_table(dev, "emac-ref-mux", ref,
							ARRAY_SIZE(ref),
							CLK_SET_RATE_NO_REPARENT,
							reg, 0, 2, 0, NULL,
							&ec->lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);

	hw = devm_clk_hw_register_divider_parent_hw(dev, "emac-ref-div", hw, 0,
						    reg, 8, 8, 0, &ec->lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);

	ec->hws->hws[ESP32S31_CNNT_CLK_EMAC_REF] =
		devm_clk_hw_register_gate_parent_hw(dev, "emac-ref", hw,
						    CLK_SET_RATE_PARENT, reg,
						    2, 0, &ec->lock);

	ec->rcdev.ops = &esp32s31_cnnt_reset_ops;
	ec->rcdev.owner = THIS_MODULE;
	ec->rcdev.of_node = dev->of_node;
	ec->rcdev.nr_resets = 1;
	ret = devm_reset_controller_register(dev, &ec->rcdev);
	if (ret)
		return ret;

	regmap = devm_regmap_init_mmio(dev, ec->base,
				       &esp32s31_cnnt_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	return of_syscon_register_regmap(dev->of_node, regmap);
}

struct esp32s31_clk_data {
	unsigned int nclks;
	int (*init)(struct esp32s31_clk *ec);
};

static int esp32s31_clk_probe(struct platform_device *pdev)
{
	const struct esp32s31_clk_data *data = device_get_match_data(&pdev->dev);
	struct device *dev = &pdev->dev;
	struct esp32s31_clk *ec;
	struct resource *res;
	unsigned int i;
	int ret;

	ec = devm_kzalloc(dev, sizeof(*ec), GFP_KERNEL);
	if (!ec)
		return -ENOMEM;

	ec->hws = devm_kzalloc(dev, struct_size(ec->hws, hws, data->nclks),
			       GFP_KERNEL);
	if (!ec->hws)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	ec->base = devm_ioremap(dev, res->start, resource_size(res));
	if (!ec->base)
		return -ENOMEM;

	ec->dev = dev;
	ec->hws->num = data->nclks;
	spin_lock_init(&ec->lock);

	ret = data->init(ec);
	if (ret)
		return ret;

	for (i = 0; i < data->nclks; i++)
		if (IS_ERR(ec->hws->hws[i]))
			return dev_err_probe(dev, PTR_ERR(ec->hws->hws[i]),
					     "failed to register clock %u\n", i);

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, ec->hws);
}

static const struct esp32s31_clk_data esp32s31_lp_data = {
	.nclks = ESP32S31_LP_CLK_MPLL + 1,
	.init = esp32s31_lp_init,
};

static const struct esp32s31_clk_data esp32s31_hp_data = {
	.nclks = ESP32S31_CLK_EMAC_SYS + 1,
	.init = esp32s31_hp_init,
};

static const struct esp32s31_clk_data esp32s31_cnnt_data = {
	.nclks = ESP32S31_CNNT_CLK_EMAC_REF + 1,
	.init = esp32s31_cnnt_init,
};

static const struct of_device_id esp32s31_clk_of_match[] = {
	{ .compatible = "esp,esp32s31-lp-clkrst", .data = &esp32s31_lp_data },
	{ .compatible = "esp,esp32s31-hp-clkrst", .data = &esp32s31_hp_data },
	{ .compatible = "esp,esp32s31-cnnt-sys", .data = &esp32s31_cnnt_data },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_clk_of_match);

static struct platform_driver esp32s31_clk_driver = {
	.driver = {
		.name = "esp32s31-clk",
		.of_match_table = esp32s31_clk_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = esp32s31_clk_probe,
};
builtin_platform_driver(esp32s31_clk_driver);
