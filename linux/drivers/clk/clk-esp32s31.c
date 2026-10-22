// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Marco Mueller <hello@annoyedmilk.ch>
 *
 * ESP32-S31 clock and reset controllers
 *
 * Three blocks have clocks for Linux.  LP_AONCLKRST has the MPLL, which the
 * firmware sets for the PSRAM, so Linux only reads its rate.  HP_SYS_CLKRST
 * has the CPU, SYS and APB clocks, which the firmware sets, and the gates,
 * dividers and resets of the peripherals that Linux owns.  CNNT_SYS has the
 * EMAC TX clock, the EMAC and SD host resets, and the EMAC interface
 * registers, which the EMAC glue gets as a syscon.
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
#define HP_RMT_CTRL0			0xbc
#define HP_SDIO_HOST_CTRL0		0xc0
#define HP_SDIO_LS_CLK_EN		3
#define HP_SDIO_HOST_FUNC_CTRL0		0xc4
#define HP_SDIO_EDGE_UPDATE		BIT(0)
#define HP_SDIO_EDGE_L			GENMASK(4, 1)
#define HP_SDIO_EDGE_H			GENMASK(8, 5)
#define HP_SDIO_EDGE_N			GENMASK(12, 9)
#define HP_SDIO_DRV_EDGE_SEL		GENMASK(18, 16)
#define HP_SDIO_PHASE_CLK_EN		GENMASK(24, 22)
#define HP_SDIO_DIV_MAX			16UL
#define HP_EMAC_CTRL0			0xc8
#define HP_I2C_CTRL0(n)			(0xdc + (n) * 4)
/* The bus gate and the reset of a peripheral clock register */
#define HP_PERIPH_BUS_CLK_EN		0
#define HP_PERIPH_RST_EN		BIT(1)
#define HP_FREQ_DIV_NUM			GENMASK(7, 0)
#define HP_FREQ_NUMERATOR		GENMASK(10, 8)
#define HP_FREQ_DENOMINATOR		GENMASK(13, 11)

#define CNNT_SDMMC_CTRL			0x38
#define CNNT_SDMMC_RST_EN		BIT(30)
#define CNNT_EMAC_CTRL			0x3c
#define CNNT_EMAC_RST_EN		BIT(1)
#define CNNT_EMAC_REF_CTRL		0x40
#define CNNT_SIZE			0x1000

/* A reset bit: 1 holds the peripheral in reset. */
struct esp32s31_reset {
	u16 offset;
	u32 mask;
};

struct esp32s31_clk {
	struct device *dev;
	void __iomem *base;
	struct clk_hw_onecell_data *hws;
	/* For the read-modify-write of the registers */
	spinlock_t lock;
	const struct esp32s31_reset *resets;
	struct reset_controller_dev rcdev;
};

/* A clock in one register: the MPLL, a root divider or the SDIO LS divider */
struct esp32s31_reg_clk {
	struct clk_hw hw;
	void __iomem *reg;
};

#define to_reg_clk(_hw) container_of(_hw, struct esp32s31_reg_clk, hw)

static unsigned long esp32s31_mpll_recalc_rate(struct clk_hw *hw,
					       unsigned long parent_rate)
{
	u32 val = readl(to_reg_clk(hw)->reg);

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
	u32 val = readl(to_reg_clk(hw)->reg);
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

static struct clk_hw *esp32s31_reg_register(struct esp32s31_clk *ec,
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
	struct esp32s31_reg_clk *clk;
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
		esp32s31_reg_register(ec, "mpll", &xtal, &esp32s31_mpll_ops,
				      LP_MSPI_DIV);
	return 0;
}

/*
 * The SDIO LS clock of the SD host: MPLL divided by 2 to 16 with edge
 * counters.  Its source select keeps the reset value 0, MPLL.  The phase
 * clocks get the ESP-IDF defaults: drive on edge 1, sample on edge 0.
 */
static unsigned long esp32s31_sdio_div(unsigned long rate,
				       unsigned long parent_rate)
{
	if (!rate)
		return HP_SDIO_DIV_MAX;

	return clamp(DIV_ROUND_UP(parent_rate, rate), 2UL, HP_SDIO_DIV_MAX);
}

static unsigned long esp32s31_sdio_recalc_rate(struct clk_hw *hw,
					       unsigned long parent_rate)
{
	u32 val = readl(to_reg_clk(hw)->reg);

	return parent_rate / (FIELD_GET(HP_SDIO_EDGE_N, val) + 1);
}

static int esp32s31_sdio_determine_rate(struct clk_hw *hw,
					struct clk_rate_request *req)
{
	req->rate = req->best_parent_rate /
		    esp32s31_sdio_div(req->rate, req->best_parent_rate);
	return 0;
}

static int esp32s31_sdio_set_rate(struct clk_hw *hw, unsigned long rate,
				  unsigned long parent_rate)
{
	unsigned long div = esp32s31_sdio_div(rate, parent_rate);
	void __iomem *reg = to_reg_clk(hw)->reg;
	u32 val = FIELD_PREP(HP_SDIO_EDGE_L, div - 1) |
		  FIELD_PREP(HP_SDIO_EDGE_H, div / 2 - 1) |
		  FIELD_PREP(HP_SDIO_EDGE_N, div - 1) |
		  FIELD_PREP(HP_SDIO_DRV_EDGE_SEL, 1) | HP_SDIO_PHASE_CLK_EN;

	writel(val | HP_SDIO_EDGE_UPDATE, reg);
	writel(val, reg);
	return 0;
}

static const struct clk_ops esp32s31_sdio_ops = {
	.recalc_rate = esp32s31_sdio_recalc_rate,
	.determine_rate = esp32s31_sdio_determine_rate,
	.set_rate = esp32s31_sdio_set_rate,
};

/*
 * A peripheral clock register: the bus gate in bit 0, the reset in bit 1,
 * and the function clock: source, divider (DIV_NUM + 1), gate.  The bus gate
 * has the ID id, the function clock id + 1.
 */
struct esp32s31_periph {
	const char *names[4];
	u16 offset;
	u8 bus, nsrc, sel_shift, sel_width, en_shift, div_shift, id;
};

static const struct esp32s31_periph esp32s31_periphs[] = {
	{ { "i2c0-apb", "i2c0-sel", "i2c0-div", "i2c0" }, HP_I2C_CTRL0(0),
	  ESP32S31_CLK_APB, 2, 3, 1, 4, 5, ESP32S31_CLK_I2C0_APB },
	{ { "i2c1-apb", "i2c1-sel", "i2c1-div", "i2c1" }, HP_I2C_CTRL0(1),
	  ESP32S31_CLK_APB, 2, 3, 1, 4, 5, ESP32S31_CLK_I2C1_APB },
	{ { "rmt-sys", "rmt-sel", "rmt-div", "rmt" }, HP_RMT_CTRL0,
	  ESP32S31_CLK_SYS, 3, 3, 2, 5, 6, ESP32S31_CLK_RMT_SYS },
};

static struct clk_hw *esp32s31_periph_register(struct esp32s31_clk *ec,
					       const struct esp32s31_periph *p)
{
	static const struct clk_parent_data src[] = {
		{ .fw_name = "xtal" },
		{ .fw_name = "rc-fast" },
		{ .fw_name = "pll-f80m" },
	};
	void __iomem *reg = ec->base + p->offset;
	struct clk_hw **hws = ec->hws->hws;
	struct clk_hw *hw;

	hws[p->id] = devm_clk_hw_register_gate_parent_hw(ec->dev, p->names[0],
							 hws[p->bus], 0, reg,
							 HP_PERIPH_BUS_CLK_EN,
							 0, &ec->lock);

	hw = devm_clk_hw_register_mux_parent_data_table(ec->dev, p->names[1],
							src, p->nsrc,
							CLK_SET_RATE_NO_REPARENT,
							reg, p->sel_shift,
							p->sel_width, 0, NULL,
							&ec->lock);
	if (IS_ERR(hw))
		return hw;

	hw = devm_clk_hw_register_divider_parent_hw(ec->dev, p->names[2], hw,
						    0, reg, p->div_shift,
						    8, 0, &ec->lock);
	if (IS_ERR(hw))
		return hw;

	hw = devm_clk_hw_register_gate_parent_hw(ec->dev, p->names[3], hw,
						 CLK_SET_RATE_PARENT, reg,
						 p->en_shift, 0, &ec->lock);
	hws[p->id + 1] = hw;
	return hw;
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
	static const struct clk_parent_data mpll = { .fw_name = "mpll" };
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
		hw = esp32s31_reg_register(ec, chain[i].name, &parent,
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

	for (i = 0; i < ARRAY_SIZE(esp32s31_periphs); i++) {
		hw = esp32s31_periph_register(ec, &esp32s31_periphs[i]);
		if (IS_ERR(hw))
			return PTR_ERR(hw);
	}

	hws[ESP32S31_CLK_SDMMC_SYS] =
		devm_clk_hw_register_gate_parent_hw(ec->dev, "sdmmc-sys",
						    hws[ESP32S31_CLK_SYS], 0,
						    ec->base + HP_SDIO_HOST_CTRL0,
						    HP_PERIPH_BUS_CLK_EN, 0,
						    &ec->lock);

	hw = esp32s31_reg_register(ec, "sdio-ls-div", &mpll,
				   &esp32s31_sdio_ops, HP_SDIO_HOST_FUNC_CTRL0);
	if (IS_ERR(hw))
		return PTR_ERR(hw);

	hws[ESP32S31_CLK_SDIO_LS] =
		devm_clk_hw_register_gate_parent_hw(ec->dev, "sdio-ls", hw,
						    CLK_SET_RATE_PARENT,
						    ec->base + HP_SDIO_HOST_CTRL0,
						    HP_SDIO_LS_CLK_EN, 0,
						    &ec->lock);
	return 0;
}

static struct esp32s31_clk *to_esp32s31_clk(struct reset_controller_dev *rcdev)
{
	return container_of(rcdev, struct esp32s31_clk, rcdev);
}

static int esp32s31_reset_update(struct reset_controller_dev *rcdev,
				 unsigned long id, bool assert)
{
	struct esp32s31_clk *ec = to_esp32s31_clk(rcdev);
	const struct esp32s31_reset *rst = &ec->resets[id];
	void __iomem *reg = ec->base + rst->offset;
	u32 val;

	guard(spinlock_irqsave)(&ec->lock);
	val = readl(reg) & ~rst->mask;
	writel(val | (assert ? rst->mask : 0), reg);
	return 0;
}

static int esp32s31_reset_assert(struct reset_controller_dev *rcdev,
				 unsigned long id)
{
	return esp32s31_reset_update(rcdev, id, true);
}

static int esp32s31_reset_deassert(struct reset_controller_dev *rcdev,
				   unsigned long id)
{
	return esp32s31_reset_update(rcdev, id, false);
}

static const struct reset_control_ops esp32s31_reset_ops = {
	.assert = esp32s31_reset_assert,
	.deassert = esp32s31_reset_deassert,
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

	regmap = devm_regmap_init_mmio(dev, ec->base,
				       &esp32s31_cnnt_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	return of_syscon_register_regmap(dev->of_node, regmap);
}

struct esp32s31_clk_data {
	unsigned int nclks;
	int (*init)(struct esp32s31_clk *ec);
	const struct esp32s31_reset *resets;
	unsigned int nresets;
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

	if (data->nresets) {
		ec->resets = data->resets;
		ec->rcdev.ops = &esp32s31_reset_ops;
		ec->rcdev.owner = THIS_MODULE;
		ec->rcdev.of_node = dev->of_node;
		ec->rcdev.nr_resets = data->nresets;
		ret = devm_reset_controller_register(dev, &ec->rcdev);
		if (ret)
			return ret;
	}

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, ec->hws);
}

static const struct esp32s31_clk_data esp32s31_lp_data = {
	.nclks = ESP32S31_LP_CLK_MPLL + 1,
	.init = esp32s31_lp_init,
};

static const struct esp32s31_reset esp32s31_hp_resets[] = {
	[ESP32S31_RST_I2C0] = { HP_I2C_CTRL0(0), HP_PERIPH_RST_EN },
	[ESP32S31_RST_I2C1] = { HP_I2C_CTRL0(1), HP_PERIPH_RST_EN },
	[ESP32S31_RST_RMT] = { HP_RMT_CTRL0, HP_PERIPH_RST_EN },
};

static const struct esp32s31_clk_data esp32s31_hp_data = {
	.nclks = ESP32S31_CLK_SDIO_LS + 1,
	.init = esp32s31_hp_init,
	.resets = esp32s31_hp_resets,
	.nresets = ARRAY_SIZE(esp32s31_hp_resets),
};

static const struct esp32s31_reset esp32s31_cnnt_resets[] = {
	[ESP32S31_CNNT_RST_EMAC] = { CNNT_EMAC_CTRL, CNNT_EMAC_RST_EN },
	[ESP32S31_CNNT_RST_SDMMC] = { CNNT_SDMMC_CTRL, CNNT_SDMMC_RST_EN },
};

static const struct esp32s31_clk_data esp32s31_cnnt_data = {
	.nclks = ESP32S31_CNNT_CLK_EMAC_REF + 1,
	.init = esp32s31_cnnt_init,
	.resets = esp32s31_cnnt_resets,
	.nresets = ARRAY_SIZE(esp32s31_cnnt_resets),
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
