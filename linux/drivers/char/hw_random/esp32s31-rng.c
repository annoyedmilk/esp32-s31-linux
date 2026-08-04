// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Marco Müller <hello@annoyedmilk.ch>
 *
 * ESP32-S31 true random number generator.  The block samples a noise source
 * and mixes it through a CRC, so unlike the generator on earlier Espressif
 * parts it needs neither the RF subsystem nor the SAR ADC to be running.
 * Its clock and reset live in the LP peripheral clock controller.
 */

#include <linux/delay.h>
#include <linux/hw_random.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define ESP32S31_TRNG_CONF		0x00
#define ESP32S31_TRNG_NOISE_CRC_EN	BIT(30)
#define ESP32S31_TRNG_SAMPLE_ENABLE	BIT(31)
#define ESP32S31_TRNG_DATA		0x48
#define ESP32S31_TRNG_DATE		0xfc
#define ESP32S31_TRNG_CLK_EN		BIT(28)

#define ESP32S31_RNG_CTRL_CLK_EN	BIT(30)
#define ESP32S31_RNG_CTRL_RST_EN	BIT(31)

/* Sampling interval the noise source needs to decorrelate consecutive words. */
#define ESP32S31_TRNG_READ_DELAY_US	1

struct esp32s31_rng {
	struct hwrng rng;
	void __iomem *trng;
	void __iomem *clkrst;
};

static void esp32s31_rng_update(void __iomem *reg, u32 mask, bool set)
{
	u32 val = readl(reg);

	if (set)
		val |= mask;
	else
		val &= ~mask;
	writel(val, reg);
}

static void esp32s31_rng_enable(struct esp32s31_rng *priv)
{
	esp32s31_rng_update(priv->clkrst, ESP32S31_RNG_CTRL_CLK_EN, true);
	esp32s31_rng_update(priv->clkrst, ESP32S31_RNG_CTRL_RST_EN, true);
	esp32s31_rng_update(priv->clkrst, ESP32S31_RNG_CTRL_RST_EN, false);

	esp32s31_rng_update(priv->trng + ESP32S31_TRNG_DATE,
			    ESP32S31_TRNG_CLK_EN, true);
	esp32s31_rng_update(priv->trng + ESP32S31_TRNG_CONF,
			    ESP32S31_TRNG_SAMPLE_ENABLE |
			    ESP32S31_TRNG_NOISE_CRC_EN, true);
}

static void esp32s31_rng_disable(void *data)
{
	struct esp32s31_rng *priv = data;

	esp32s31_rng_update(priv->trng + ESP32S31_TRNG_CONF,
			    ESP32S31_TRNG_SAMPLE_ENABLE |
			    ESP32S31_TRNG_NOISE_CRC_EN, false);
	esp32s31_rng_update(priv->trng + ESP32S31_TRNG_DATE,
			    ESP32S31_TRNG_CLK_EN, false);
	esp32s31_rng_update(priv->clkrst, ESP32S31_RNG_CTRL_CLK_EN, false);
}

static int esp32s31_rng_read(struct hwrng *rng, void *buf, size_t max,
			     bool wait)
{
	struct esp32s31_rng *priv = container_of(rng, struct esp32s31_rng, rng);
	size_t done = 0;

	while (done + sizeof(u32) <= max) {
		u32 val = readl(priv->trng + ESP32S31_TRNG_DATA);

		memcpy(buf + done, &val, sizeof(val));
		done += sizeof(val);

		if (done + sizeof(u32) <= max)
			udelay(ESP32S31_TRNG_READ_DELAY_US);
	}

	return done;
}

static int esp32s31_rng_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct esp32s31_rng *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->trng = devm_platform_ioremap_resource_byname(pdev, "trng");
	if (IS_ERR(priv->trng))
		return PTR_ERR(priv->trng);

	priv->clkrst = devm_platform_ioremap_resource_byname(pdev, "clkrst");
	if (IS_ERR(priv->clkrst))
		return PTR_ERR(priv->clkrst);

	esp32s31_rng_enable(priv);

	ret = devm_add_action_or_reset(dev, esp32s31_rng_disable, priv);
	if (ret)
		return ret;

	priv->rng.name = pdev->name;
	priv->rng.read = esp32s31_rng_read;
	priv->rng.quality = 1000;

	return devm_hwrng_register(dev, &priv->rng);
}

static const struct of_device_id esp32s31_rng_of_match[] = {
	{ .compatible = "espressif,esp32s31-trng" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_rng_of_match);

static struct platform_driver esp32s31_rng_driver = {
	.probe = esp32s31_rng_probe,
	.driver = {
		.name = "esp32s31-rng",
		.of_match_table = esp32s31_rng_of_match,
	},
};
module_platform_driver(esp32s31_rng_driver);

MODULE_DESCRIPTION("ESP32-S31 true random number generator");
MODULE_AUTHOR("Marco Müller <hello@annoyedmilk.ch>");
MODULE_LICENSE("GPL");
