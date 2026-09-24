// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Marco Müller <hello@annoyedmilk.ch>
 *
 * ESP32-S31 true random number generator.  The block samples a noise source
 * and mixes it with a CRC.  Thus, different from earlier Espressif chips, it
 * does not need the RF subsystem or the SAR ADC.  Its clock and reset are in
 * the LP peripheral clock controller.
 *
 * The ESP-IDF firmware on hart 0 also reads the TRNG, and it starts the block
 * with noise source, health test and output mode settings.  A reset or a new
 * setup from Linux would erase them for the two harts.  Thus Linux only makes
 * sure that the block is on, and reads it.
 */

#include <linux/delay.h>
#include <linux/hw_random.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define ESP32S31_TRNG_CONF		0x00
#define ESP32S31_TRNG_SAMPLE_ENABLE	BIT(31)
#define ESP32S31_TRNG_DATA		0x48

#define ESP32S31_RNG_CTRL_CLK_EN	BIT(30)
#define ESP32S31_RNG_CTRL_RST_EN	BIT(31)

/* Time between two reads, so that two words from the noise source are not related. */
#define ESP32S31_TRNG_READ_DELAY_US	1

struct esp32s31_rng {
	struct hwrng rng;
	void __iomem *trng;
	void __iomem *clkrst;
};

/*
 * The same test as ESP-IDF rng_ll_is_enabled(), plus the sample bit.  Do not
 * test DATE.clk_en: it only forces the register clock on, and the firmware
 * block works with it at 0.
 */
static bool esp32s31_rng_is_on(struct esp32s31_rng *priv)
{
	u32 ctrl = readl(priv->clkrst);
	u32 conf = readl(priv->trng + ESP32S31_TRNG_CONF);

	return (ctrl & ESP32S31_RNG_CTRL_CLK_EN) &&
	       !(ctrl & ESP32S31_RNG_CTRL_RST_EN) &&
	       (conf & ESP32S31_TRNG_SAMPLE_ENABLE);
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

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->trng = devm_platform_ioremap_resource_byname(pdev, "trng");
	if (IS_ERR(priv->trng))
		return PTR_ERR(priv->trng);

	priv->clkrst = devm_platform_ioremap_resource_byname(pdev, "clkrst");
	if (IS_ERR(priv->clkrst))
		return PTR_ERR(priv->clkrst);

	if (!esp32s31_rng_is_on(priv))
		return dev_err_probe(dev, -ENODEV,
				     "the firmware did not start the TRNG\n");

	priv->rng.name = pdev->name;
	priv->rng.read = esp32s31_rng_read;
	priv->rng.quality = 1000;

	return devm_hwrng_register(dev, &priv->rng);
}

static const struct of_device_id esp32s31_rng_of_match[] = {
	{ .compatible = "esp,esp32s31-trng" },
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
