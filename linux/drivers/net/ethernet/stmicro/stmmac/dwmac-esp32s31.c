// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Roan Brand <brandroan@gmail.com>
 *
 * DWMAC glue of the ESP32-S31: a DesignWare GMAC 3.70a with RGMII.  The
 * interface mode and the RGMII clock paths are in CNNT_SYS, a syscon.  The
 * GMAC takes the mode at its reset, so the glue sets it in the init callback,
 * which stmmac calls before it resets the GMAC and at resume.  The TX clock
 * comes from the clock framework.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include "stmmac.h"
#include "stmmac_platform.h"

#define CNNT_EMAC_RMII_PAD_CTRL		0x44
#define CNNT_EMAC_RMII_CTRL		0x48
#define CNNT_EMAC_RX_CTRL		0x4c
#define CNNT_EMAC_TX_CTRL		0x50
#define CNNT_GMAC_CTRL0			0x60

#define RMII_PAD_CLK_EN			BIT(1)
#define RMII_PAD_CLK_INV_EN		BIT(2)
#define RMII_CLK_SEL			BIT(0)
#define RMII_CLK_EN			BIT(1)
#define RMII_PAD_OUT_CLK_EN		BIT(2)
/* RX_CTRL and TX_CTRL */
#define RXTX_PAD_CLK_EN			BIT(0)
#define RXTX_PAD_CLK_INV_EN		BIT(1)
#define RXTX_CLK_SEL			BIT(2)
#define RXTX_180_CLK_EN			BIT(3)
#define GMAC_PHY_INTF_SEL		GENMASK(4, 2)
#define GMAC_PHY_INTF_RGMII		1

struct esp32s31_reg_update {
	u32 reg;
	u32 mask;
	u32 val;
};

/* RX clock from the RX_CLK pad, TX clock out on the RMII clock pad */
static const struct esp32s31_reg_update esp32s31_rgmii[] = {
	{ CNNT_GMAC_CTRL0, GMAC_PHY_INTF_SEL,
	  FIELD_PREP_CONST(GMAC_PHY_INTF_SEL, GMAC_PHY_INTF_RGMII) },
	{ CNNT_EMAC_RMII_PAD_CTRL, RMII_PAD_CLK_EN | RMII_PAD_CLK_INV_EN, 0 },
	{ CNNT_EMAC_RMII_CTRL, RMII_CLK_SEL | RMII_CLK_EN | RMII_PAD_OUT_CLK_EN,
	  RMII_PAD_OUT_CLK_EN },
	{ CNNT_EMAC_RX_CTRL, RXTX_PAD_CLK_EN | RXTX_PAD_CLK_INV_EN |
	  RXTX_CLK_SEL | RXTX_180_CLK_EN,
	  RXTX_PAD_CLK_EN | RXTX_CLK_SEL | RXTX_180_CLK_EN },
	{ CNNT_EMAC_TX_CTRL, RXTX_PAD_CLK_EN | RXTX_CLK_SEL | RXTX_180_CLK_EN,
	  RXTX_180_CLK_EN },
};

static int esp32s31_dwmac_init(struct device *dev, void *bsp_priv)
{
	struct regmap *regmap = bsp_priv;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(esp32s31_rgmii); i++) {
		const struct esp32s31_reg_update *u = &esp32s31_rgmii[i];

		ret = regmap_update_bits(regmap, u->reg, u->mask, u->val);
		if (ret)
			return ret;
	}

	return 0;
}

static int esp32s31_dwmac_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	struct device *dev = &pdev->dev;
	struct regmap *regmap;
	int ret;

	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return ret;

	plat_dat = devm_stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat))
		return dev_err_probe(dev, PTR_ERR(plat_dat),
				     "failed to parse DT parameters\n");

	if (!phy_interface_mode_is_rgmii(plat_dat->phy_interface))
		return dev_err_probe(dev, -EINVAL, "only RGMII is supported\n");

	regmap = syscon_regmap_lookup_by_phandle(dev->of_node, "esp,cnnt-sys");
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "failed to get CNNT_SYS\n");

	plat_dat->clk_tx_i = devm_clk_get_enabled(dev, "ref");
	if (IS_ERR(plat_dat->clk_tx_i))
		return dev_err_probe(dev, PTR_ERR(plat_dat->clk_tx_i),
				     "failed to get the TX clock\n");
	plat_dat->set_clk_tx_rate = stmmac_set_clk_tx_rate;

	/*
	 * stmmac selects the descriptor ops before it reads the DMA
	 * capabilities, so it must know the enhanced descriptors here.
	 */
	plat_dat->enh_desc = true;
	plat_dat->bsp_priv = regmap;
	plat_dat->init = esp32s31_dwmac_init;

	return devm_stmmac_pltfr_probe(pdev, plat_dat, &stmmac_res);
}

static const struct of_device_id esp32s31_dwmac_match[] = {
	{ .compatible = "esp,esp32s31-dwmac" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_dwmac_match);

static struct platform_driver esp32s31_dwmac_driver = {
	.probe = esp32s31_dwmac_probe,
	.driver = {
		.name = "esp32s31-dwmac",
		.pm = &stmmac_pltfr_pm_ops,
		.of_match_table = esp32s31_dwmac_match,
	},
};
module_platform_driver(esp32s31_dwmac_driver);

MODULE_DESCRIPTION("ESP32-S31 DWMAC glue driver");
MODULE_AUTHOR("Roan Brand <brandroan@gmail.com>");
MODULE_LICENSE("GPL");
