// SPDX-License-Identifier: GPL-2.0-only
/*
 * ESP32-S31 integrated USB 2.0 UTMI PHY
 *
 * The register sequence mirrors ESP-IDF's usb_utmi_hal_init() and
 * usb_phy_otg_set_mode(..., USB_OTG_MODE_HOST).  The PHY and the DWC2 core
 * share clock/reset controls, so they are kept together in this provider.
 */

#include <linux/bitops.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>

/* HP_SYS_CLKRST_USB_OTGHS_CTRL0 */
#define ESP32S31_USB_APB_CLK_EN		BIT(0)
#define ESP32S31_USB_SYS_CLK_EN		BIT(1)

/* CNNT_SYS_USB_OTG20_CTRL */
#define ESP32S31_USB_UTMIFS_CLK_EN	BIT(23)
#define ESP32S31_USB_PHYREF_CLK_EN	BIT(27)
#define ESP32S31_USB_PHY_RST_EN		BIT(29)
#define ESP32S31_USB_AHB_RST_EN		BIT(30)
#define ESP32S31_USB_APB_RST_EN		BIT(31)
#define ESP32S31_USB_ALL_RST_EN		(ESP32S31_USB_PHY_RST_EN | \
					 ESP32S31_USB_AHB_RST_EN | \
					 ESP32S31_USB_APB_RST_EN)

/* HP_ALIVE_SYS_USB_CTRL */
#define ESP32S31_USB_DM_PULLDOWN		BIT(2)
#define ESP32S31_USB_DP_PULLDOWN		BIT(3)

/* HP_ALIVE_SYS_USB_OTGHS_CTRL */
#define ESP32S31_USB_PHY_PLL_FORCE_EN	BIT(0)
#define ESP32S31_USB_PHY_SUSPEND_FORCE_EN BIT(2)
#define ESP32S31_USB_PHY_OTG_SUSPENDM	BIT(7)

/* USB_UTMI_FC_06 */
#define ESP32S31_USB_LS_PARALLEL_EN	BIT(0)
#define ESP32S31_USB_LS_KEEPALIVE_EN	BIT(3)

struct esp32s31_usb_phy {
	void __iomem *clkrst;
	void __iomem *cnnt;
	void __iomem *usb_ctrl;
	void __iomem *otghs_ctrl;
	void __iomem *utmi_fc06;
};

static void esp32s31_usb_update_bits(void __iomem *reg, u32 mask, u32 val)
{
	u32 tmp = readl(reg);

	tmp &= ~mask;
	tmp |= val & mask;
	writel(tmp, reg);
}

static int esp32s31_usb_phy_init(struct phy *phy)
{
	struct esp32s31_usb_phy *priv = phy_get_drvdata(phy);
	u32 val;

	/* Enable the DWC2 APB/system clocks and the UTMI/reference clocks. */
	esp32s31_usb_update_bits(priv->clkrst,
				 ESP32S31_USB_APB_CLK_EN |
				 ESP32S31_USB_SYS_CLK_EN,
				 ESP32S31_USB_APB_CLK_EN |
				 ESP32S31_USB_SYS_CLK_EN);
	esp32s31_usb_update_bits(priv->cnnt,
				 ESP32S31_USB_UTMIFS_CLK_EN |
				 ESP32S31_USB_PHYREF_CLK_EN,
				 ESP32S31_USB_UTMIFS_CLK_EN |
				 ESP32S31_USB_PHYREF_CLK_EN);

	/* Let DWC2 control PHY suspend and PLL state automatically. */
	esp32s31_usb_update_bits(priv->otghs_ctrl,
				 ESP32S31_USB_PHY_PLL_FORCE_EN |
				 ESP32S31_USB_PHY_SUSPEND_FORCE_EN, 0);

	/* Assert all resets, then release the PHY before the AHB/APB core. */
	val = readl(priv->cnnt) | ESP32S31_USB_ALL_RST_EN;
	writel(val, priv->cnnt);
	val &= ~ESP32S31_USB_PHY_RST_EN;
	writel(val, priv->cnnt);
	val &= ~(ESP32S31_USB_AHB_RST_EN | ESP32S31_USB_APB_RST_EN);
	writel(val, priv->cnnt);

	/* Match the UTMI setup used by ESP-IDF on ESP32-S31. */
	esp32s31_usb_update_bits(priv->otghs_ctrl,
				 ESP32S31_USB_PHY_OTG_SUSPENDM,
				 ESP32S31_USB_PHY_OTG_SUSPENDM);
	esp32s31_usb_update_bits(priv->utmi_fc06,
				 ESP32S31_USB_LS_PARALLEL_EN |
				 ESP32S31_USB_LS_KEEPALIVE_EN,
				 ESP32S31_USB_LS_PARALLEL_EN |
				 ESP32S31_USB_LS_KEEPALIVE_EN);

	/* Host mode requires 15 kohm pull-downs on both data lines. */
	esp32s31_usb_update_bits(priv->usb_ctrl,
				 ESP32S31_USB_DM_PULLDOWN |
				 ESP32S31_USB_DP_PULLDOWN,
				 ESP32S31_USB_DM_PULLDOWN |
				 ESP32S31_USB_DP_PULLDOWN);

	return 0;
}

static int esp32s31_usb_phy_exit(struct phy *phy)
{
	struct esp32s31_usb_phy *priv = phy_get_drvdata(phy);

	esp32s31_usb_update_bits(priv->usb_ctrl,
				 ESP32S31_USB_DM_PULLDOWN |
				 ESP32S31_USB_DP_PULLDOWN, 0);
	esp32s31_usb_update_bits(priv->cnnt,
				 ESP32S31_USB_UTMIFS_CLK_EN |
				 ESP32S31_USB_PHYREF_CLK_EN, 0);
	esp32s31_usb_update_bits(priv->clkrst,
				 ESP32S31_USB_APB_CLK_EN |
				 ESP32S31_USB_SYS_CLK_EN, 0);

	return 0;
}

static const struct phy_ops esp32s31_usb_phy_ops = {
	.init = esp32s31_usb_phy_init,
	.exit = esp32s31_usb_phy_exit,
	.owner = THIS_MODULE,
};

static int esp32s31_usb_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct phy_provider *provider;
	struct esp32s31_usb_phy *priv;
	struct phy *phy;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->clkrst = devm_platform_ioremap_resource_byname(pdev, "clkrst");
	if (IS_ERR(priv->clkrst))
		return PTR_ERR(priv->clkrst);
	priv->cnnt = devm_platform_ioremap_resource_byname(pdev, "cnnt");
	if (IS_ERR(priv->cnnt))
		return PTR_ERR(priv->cnnt);
	priv->usb_ctrl = devm_platform_ioremap_resource_byname(pdev, "usb-ctrl");
	if (IS_ERR(priv->usb_ctrl))
		return PTR_ERR(priv->usb_ctrl);
	priv->otghs_ctrl = devm_platform_ioremap_resource_byname(pdev,
								 "otghs-ctrl");
	if (IS_ERR(priv->otghs_ctrl))
		return PTR_ERR(priv->otghs_ctrl);
	priv->utmi_fc06 = devm_platform_ioremap_resource_byname(pdev,
								"utmi-fc06");
	if (IS_ERR(priv->utmi_fc06))
		return PTR_ERR(priv->utmi_fc06);

	phy = devm_phy_create(dev, NULL, &esp32s31_usb_phy_ops);
	if (IS_ERR(phy))
		return PTR_ERR(phy);

	phy_set_drvdata(phy, priv);
	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id esp32s31_usb_phy_of_match[] = {
	{ .compatible = "espressif,esp32s31-usb-phy" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_usb_phy_of_match);

static struct platform_driver esp32s31_usb_phy_driver = {
	.probe = esp32s31_usb_phy_probe,
	.driver = {
		.name = "esp32s31-usb-phy",
		.of_match_table = esp32s31_usb_phy_of_match,
	},
};
module_platform_driver(esp32s31_usb_phy_driver);

MODULE_DESCRIPTION("Espressif ESP32-S31 USB 2.0 UTMI PHY driver");
MODULE_LICENSE("GPL");
