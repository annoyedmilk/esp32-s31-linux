// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Roan Brand <brandroan@gmail.com>
 *
 * DWMAC glue of the ESP32-S31: a DesignWare GMAC 3.70a with RGMII.  The
 * loader sets the pins, clocks, reset and RGMII mode.  This driver only sets
 * the divider of the TX clock (the MPLL) for the link speed.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/platform_device.h>

#include "stmmac.h"
#include "stmmac_platform.h"

#define ESP32S31_REF_CLK_DIV		GENMASK(15, 8)

/* The default RX ring of 512 pages would take 2 MiB of the 16 MiB. */
#define ESP32S31_DMA_RING_SIZE		64

struct esp32s31_dwmac {
	void __iomem *ref_ctrl;
	unsigned long mpll_rate;
};

static int esp32s31_set_clk_tx_rate(void *bsp_priv, struct clk *clk_tx_i,
				    phy_interface_t interface, int speed)
{
	struct esp32s31_dwmac *dwmac = bsp_priv;
	long rate = rgmii_clock(speed);
	u32 val;

	if (rate < 0 || dwmac->mpll_rate % rate ||
	    dwmac->mpll_rate / rate > FIELD_MAX(ESP32S31_REF_CLK_DIV) + 1)
		return -EINVAL;

	val = readl(dwmac->ref_ctrl);
	val &= ~ESP32S31_REF_CLK_DIV;
	val |= FIELD_PREP(ESP32S31_REF_CLK_DIV, dwmac->mpll_rate / rate - 1);
	writel(val, dwmac->ref_ctrl);
	return 0;
}

static int esp32s31_dwmac_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	struct device *dev = &pdev->dev;
	struct esp32s31_dwmac *dwmac;
	struct stmmac_priv *priv;
	struct clk *mpll;
	int ret;

	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return ret;

	plat_dat = devm_stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat))
		return dev_err_probe(dev, PTR_ERR(plat_dat),
				     "failed to parse DT parameters\n");

	dwmac = devm_kzalloc(dev, sizeof(*dwmac), GFP_KERNEL);
	if (!dwmac)
		return -ENOMEM;

	dwmac->ref_ctrl = devm_platform_ioremap_resource_byname(pdev, "tx-clk");
	if (IS_ERR(dwmac->ref_ctrl))
		return PTR_ERR(dwmac->ref_ctrl);

	mpll = devm_clk_get_enabled(dev, "mpll");
	if (IS_ERR(mpll))
		return dev_err_probe(dev, PTR_ERR(mpll), "failed to get MPLL\n");
	dwmac->mpll_rate = clk_get_rate(mpll);

	plat_dat->bsp_priv = dwmac;
	plat_dat->set_clk_tx_rate = esp32s31_set_clk_tx_rate;

	ret = stmmac_dvr_probe(dev, plat_dat, &stmmac_res);
	if (ret)
		return ret;

	/* The first open allocates the rings, after the probe. */
	priv = netdev_priv(dev_get_drvdata(dev));
	priv->dma_conf.dma_rx_size = ESP32S31_DMA_RING_SIZE;
	priv->dma_conf.dma_tx_size = ESP32S31_DMA_RING_SIZE;
	return 0;
}

static const struct of_device_id esp32s31_dwmac_match[] = {
	{ .compatible = "esp,esp32s31-gmac" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_dwmac_match);

static struct platform_driver esp32s31_dwmac_driver = {
	.probe = esp32s31_dwmac_probe,
	.remove = stmmac_pltfr_remove,
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
