// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Marco Mueller <hello@annoyedmilk.ch>
 *
 * ESP32-S31 GPIO controller
 *
 * Two blocks connect a pin to its pad.  IO_MUX selects the peripheral that
 * uses the pin.  The GPIO matrix connects a signal to it.  A usual GPIO is
 * IO_MUX function 1, with the matrix set to the constant GPIO output.
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>

#define ESP32S31_GPIO_OUT		0x04
#define ESP32S31_GPIO_OUT_W1TS		0x08
#define ESP32S31_GPIO_OUT_W1TC		0x0c
#define ESP32S31_GPIO_OUT1		0x10
#define ESP32S31_GPIO_OUT1_W1TS		0x14
#define ESP32S31_GPIO_OUT1_W1TC		0x18
#define ESP32S31_GPIO_ENABLE		0x34
#define ESP32S31_GPIO_ENABLE_W1TS	0x38
#define ESP32S31_GPIO_ENABLE_W1TC	0x3c
#define ESP32S31_GPIO_ENABLE1		0x40
#define ESP32S31_GPIO_ENABLE1_W1TS	0x44
#define ESP32S31_GPIO_ENABLE1_W1TC	0x48
#define ESP32S31_GPIO_IN		0x64
#define ESP32S31_GPIO_IN1		0x68
#define ESP32S31_GPIO_FUNC_OUT_SEL	0xaf4

/* The matrix sends this constant, not a peripheral signal. */
#define ESP32S31_SIG_GPIO_OUT		256

#define ESP32S31_IOMUX_FUN_IE		BIT(9)
#define ESP32S31_IOMUX_MCU_SEL		GENMASK(14, 12)
#define ESP32S31_IOMUX_FUNC_GPIO	1

#define ESP32S31_GPIO_COUNT		62
/* These pins have no package pin, so never give them to a consumer. */
#define ESP32S31_GPIO_RESERVED		(BIT_ULL(29) | BIT_ULL(41))

struct esp32s31_gpio {
	struct gpio_chip chip;
	void __iomem *base;
	void __iomem *iomux;
};

/* Bank 1 has pins 32 and higher, at a fixed offset from bank 0. */
static unsigned int esp32s31_bank(unsigned int offset, unsigned int reg0,
				  unsigned int reg1)
{
	return offset < 32 ? reg0 : reg1;
}

static int esp32s31_gpio_set(struct gpio_chip *chip, unsigned int offset,
			     int value)
{
	struct esp32s31_gpio *priv = gpiochip_get_data(chip);
	unsigned int reg;

	if (value)
		reg = esp32s31_bank(offset, ESP32S31_GPIO_OUT_W1TS,
				    ESP32S31_GPIO_OUT1_W1TS);
	else
		reg = esp32s31_bank(offset, ESP32S31_GPIO_OUT_W1TC,
				    ESP32S31_GPIO_OUT1_W1TC);

	/* Set and clear are different registers, so no lock is necessary. */
	writel(BIT(offset % 32), priv->base + reg);

	return 0;
}

static int esp32s31_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct esp32s31_gpio *priv = gpiochip_get_data(chip);
	unsigned int reg = esp32s31_bank(offset, ESP32S31_GPIO_IN,
					 ESP32S31_GPIO_IN1);

	return !!(readl(priv->base + reg) & BIT(offset % 32));
}

static int esp32s31_gpio_get_direction(struct gpio_chip *chip,
				       unsigned int offset)
{
	struct esp32s31_gpio *priv = gpiochip_get_data(chip);
	unsigned int reg = esp32s31_bank(offset, ESP32S31_GPIO_ENABLE,
					 ESP32S31_GPIO_ENABLE1);

	if (readl(priv->base + reg) & BIT(offset % 32))
		return GPIO_LINE_DIRECTION_OUT;

	return GPIO_LINE_DIRECTION_IN;
}

/* Take the pad from its peripheral and read its level. */
static void esp32s31_gpio_claim(struct esp32s31_gpio *priv, unsigned int offset)
{
	void __iomem *iomux = priv->iomux + offset * 4;
	u32 val = readl(iomux);

	val &= ~ESP32S31_IOMUX_MCU_SEL;
	val |= FIELD_PREP(ESP32S31_IOMUX_MCU_SEL, ESP32S31_IOMUX_FUNC_GPIO);
	val |= ESP32S31_IOMUX_FUN_IE;
	writel(val, iomux);
}

static int esp32s31_gpio_direction_input(struct gpio_chip *chip,
					 unsigned int offset)
{
	struct esp32s31_gpio *priv = gpiochip_get_data(chip);
	unsigned int reg = esp32s31_bank(offset, ESP32S31_GPIO_ENABLE_W1TC,
					 ESP32S31_GPIO_ENABLE1_W1TC);

	esp32s31_gpio_claim(priv, offset);
	writel(BIT(offset % 32), priv->base + reg);

	return 0;
}

static int esp32s31_gpio_direction_output(struct gpio_chip *chip,
					  unsigned int offset, int value)
{
	struct esp32s31_gpio *priv = gpiochip_get_data(chip);
	unsigned int reg = esp32s31_bank(offset, ESP32S31_GPIO_ENABLE_W1TS,
					 ESP32S31_GPIO_ENABLE1_W1TS);

	esp32s31_gpio_claim(priv, offset);
	/* Set the level before the output driver starts. */
	esp32s31_gpio_set(chip, offset, value);
	writel(ESP32S31_SIG_GPIO_OUT,
	       priv->base + ESP32S31_GPIO_FUNC_OUT_SEL + offset * 4);
	writel(BIT(offset % 32), priv->base + reg);

	return 0;
}

static int esp32s31_gpio_init_valid_mask(struct gpio_chip *chip,
					 unsigned long *valid_mask,
					 unsigned int ngpios)
{
	unsigned int offset;

	for (offset = 0; offset < ngpios; offset++)
		if (ESP32S31_GPIO_RESERVED & BIT_ULL(offset))
			clear_bit(offset, valid_mask);

	return 0;
}

static int esp32s31_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct esp32s31_gpio *priv;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->base = devm_platform_ioremap_resource_byname(pdev, "gpio");
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	priv->iomux = devm_platform_ioremap_resource_byname(pdev, "iomux");
	if (IS_ERR(priv->iomux))
		return PTR_ERR(priv->iomux);

	priv->chip.label = dev_name(dev);
	priv->chip.parent = dev;
	priv->chip.owner = THIS_MODULE;
	priv->chip.base = -1;
	priv->chip.ngpio = ESP32S31_GPIO_COUNT;
	priv->chip.get = esp32s31_gpio_get;
	priv->chip.set = esp32s31_gpio_set;
	priv->chip.get_direction = esp32s31_gpio_get_direction;
	priv->chip.direction_input = esp32s31_gpio_direction_input;
	priv->chip.direction_output = esp32s31_gpio_direction_output;
	priv->chip.init_valid_mask = esp32s31_gpio_init_valid_mask;

	return devm_gpiochip_add_data(dev, &priv->chip, priv);
}

static const struct of_device_id esp32s31_gpio_of_match[] = {
	{ .compatible = "esp,esp32s31-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_gpio_of_match);

static struct platform_driver esp32s31_gpio_driver = {
	.driver = {
		.name = "esp32s31-gpio",
		.of_match_table = esp32s31_gpio_of_match,
	},
	.probe = esp32s31_gpio_probe,
};
builtin_platform_driver(esp32s31_gpio_driver);
