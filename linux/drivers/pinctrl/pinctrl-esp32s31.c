// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Marco Mueller <hello@annoyedmilk.ch>
 *
 * ESP32-S31 pin controller and GPIO controller
 *
 * Two blocks connect a pin to its pad.  The IO MUX selects the function of
 * the pin: a peripheral directly, or function 1, the GPIO matrix.  The GPIO
 * matrix connects a peripheral signal or the GPIO registers to the pin.  The
 * GPIO registers are in the block of the matrix, so one driver has both.
 * GPIO13-25 are CNNT pads: the IO MUX functions of the GMAC and the SD host
 * on them also need a select bit in CNNT_IO_MUX_CTRL.
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>

#include "core.h"
#include "pinconf.h"
#include "pinctrl-utils.h"
#include "pinmux.h"

#define ESP32S31_GPIO_OUT_W1TS		0x08
#define ESP32S31_GPIO_OUT_W1TC		0x0c
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
#define ESP32S31_GPIO_PIN(n)		(0xf4 + (n) * 4)
#define ESP32S31_GPIO_FUNC_IN_SEL(s)	(0x2f4 + (s) * 4)
#define ESP32S31_GPIO_FUNC_OUT_SEL(n)	(0xaf4 + (n) * 4)

#define ESP32S31_PIN_PAD_DRIVER		BIT(2)
#define ESP32S31_IN_SEL_MATRIX		BIT(9)
/* The reset value: the signal comes from the IO MUX. */
#define ESP32S31_IN_SEL_IOMUX		0x80
/* The matrix sends the GPIO registers, not a peripheral signal. */
#define ESP32S31_SIG_GPIO_OUT		256

#define ESP32S31_IOMUX_FUN_WPD		BIT(7)
#define ESP32S31_IOMUX_FUN_WPU		BIT(8)
#define ESP32S31_IOMUX_FUN_IE		BIT(9)
#define ESP32S31_IOMUX_FUN_DRV		GENMASK(11, 10)
#define ESP32S31_IOMUX_MCU_SEL		GENMASK(14, 12)
#define ESP32S31_FUNC_GPIO		1

/* The low 16 bits of a "pinmux" cell, see dt-bindings/pinctrl/esp32s31-pinctrl.h. */
#define ESP32S31_MUX_SIG		GENMASK(8, 0)
#define ESP32S31_MUX_IN			BIT(9)
#define ESP32S31_MUX_OUT		BIT(10)
#define ESP32S31_MUX_FUNC		GENMASK(13, 11)

#define ESP32S31_NPINS			62
#define ESP32S31_NSIGS			256

/* FUN_DRV 0 to 3 */
static const u32 esp32s31_drive_ma[] = { 5, 10, 20, 40 };

/* The CNNT pads, and the function that needs their select bit */
static const struct {
	u8 first, last, func;
	u32 sel;
} esp32s31_cnnt_pads[] = {
	{ 13, 19, 2, BIT(1) },	/* GMAC */
	{ 20, 25, 4, BIT(0) },	/* SD host */
};

struct esp32s31_pinctrl {
	struct pinctrl_desc desc;
	struct pinctrl_dev *pctl;
	struct gpio_chip chip;
	void __iomem *iomux;
	void __iomem *gpio;
	void __iomem *cnnt;
	raw_spinlock_t lock;
};

static void esp32s31_iomux_rmw(struct esp32s31_pinctrl *pctrl,
			       unsigned int pin, u32 clr, u32 set)
{
	void __iomem *reg = pctrl->iomux + pin * 4;

	guard(raw_spinlock_irqsave)(&pctrl->lock);
	writel((readl(reg) & ~clr) | set, reg);
}

/* Bank 1 has pins 32 and higher, at a fixed offset from bank 0. */
static unsigned int esp32s31_bank(unsigned int pin, unsigned int reg0,
				  unsigned int reg1)
{
	return pin < 32 ? reg0 : reg1;
}

static void esp32s31_cnnt_select(struct esp32s31_pinctrl *pctrl,
				 unsigned int pin, unsigned int func)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(esp32s31_cnnt_pads); i++) {
		if (pin < esp32s31_cnnt_pads[i].first ||
		    pin > esp32s31_cnnt_pads[i].last ||
		    func != esp32s31_cnnt_pads[i].func)
			continue;

		guard(raw_spinlock_irqsave)(&pctrl->lock);
		writel(readl(pctrl->cnnt) | esp32s31_cnnt_pads[i].sel, pctrl->cnnt);
	}
}

static int esp32s31_mux_one(struct esp32s31_pinctrl *pctrl, unsigned int pin,
			    u32 mux)
{
	unsigned int func = FIELD_GET(ESP32S31_MUX_FUNC, mux);
	unsigned int sig = FIELD_GET(ESP32S31_MUX_SIG, mux);
	u32 set = FIELD_PREP(ESP32S31_IOMUX_MCU_SEL, func);
	bool matrix = func == ESP32S31_FUNC_GPIO;

	if (pin >= ESP32S31_NPINS || sig >= ESP32S31_NSIGS)
		return -EINVAL;

	if (mux & ESP32S31_MUX_IN) {
		writel(matrix ? ESP32S31_IN_SEL_MATRIX | pin : ESP32S31_IN_SEL_IOMUX,
		       pctrl->gpio + ESP32S31_GPIO_FUNC_IN_SEL(sig));
		set |= ESP32S31_IOMUX_FUN_IE;
	}
	/* The peripheral drives the output enable of the pad. */
	if (mux & ESP32S31_MUX_OUT)
		writel(sig, pctrl->gpio + ESP32S31_GPIO_FUNC_OUT_SEL(pin));

	esp32s31_iomux_rmw(pctrl, pin, ESP32S31_IOMUX_MCU_SEL, set);
	esp32s31_cnnt_select(pctrl, pin, func);
	return 0;
}

static int esp32s31_set_mux(struct pinctrl_dev *pctldev, unsigned int fsel,
			    unsigned int gsel)
{
	struct esp32s31_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);
	const struct group_desc *group;
	const unsigned int *muxes;
	unsigned int i;
	int ret;

	group = pinctrl_generic_get_group(pctldev, gsel);
	if (!group)
		return -EINVAL;

	muxes = group->data;
	for (i = 0; i < group->grp.npins; i++) {
		ret = esp32s31_mux_one(pctrl, group->grp.pins[i], muxes[i]);
		if (ret)
			return ret;
	}

	return 0;
}

/* Start as an input, with the GPIO registers on the matrix output. */
static int esp32s31_gpio_request_enable(struct pinctrl_dev *pctldev,
					struct pinctrl_gpio_range *range,
					unsigned int pin)
{
	struct esp32s31_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);

	writel(BIT(pin % 32), pctrl->gpio +
	       esp32s31_bank(pin, ESP32S31_GPIO_ENABLE_W1TC,
			     ESP32S31_GPIO_ENABLE1_W1TC));
	writel(ESP32S31_SIG_GPIO_OUT,
	       pctrl->gpio + ESP32S31_GPIO_FUNC_OUT_SEL(pin));
	esp32s31_iomux_rmw(pctrl, pin, ESP32S31_IOMUX_MCU_SEL,
			   FIELD_PREP(ESP32S31_IOMUX_MCU_SEL, ESP32S31_FUNC_GPIO) |
			   ESP32S31_IOMUX_FUN_IE);
	return 0;
}

static int esp32s31_pin_config_get(struct pinctrl_dev *pctldev,
				   unsigned int pin, unsigned long *config)
{
	struct esp32s31_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);
	enum pin_config_param param = pinconf_to_config_param(*config);
	u32 iomux = readl(pctrl->iomux + pin * 4);
	u32 pad = readl(pctrl->gpio + ESP32S31_GPIO_PIN(pin));
	u32 arg = 1;
	bool on;

	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
		on = !(iomux & (ESP32S31_IOMUX_FUN_WPU | ESP32S31_IOMUX_FUN_WPD));
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		on = iomux & ESP32S31_IOMUX_FUN_WPU;
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		on = iomux & ESP32S31_IOMUX_FUN_WPD;
		break;
	case PIN_CONFIG_INPUT_ENABLE:
		on = iomux & ESP32S31_IOMUX_FUN_IE;
		break;
	case PIN_CONFIG_DRIVE_OPEN_DRAIN:
		on = pad & ESP32S31_PIN_PAD_DRIVER;
		break;
	case PIN_CONFIG_DRIVE_PUSH_PULL:
		on = !(pad & ESP32S31_PIN_PAD_DRIVER);
		break;
	case PIN_CONFIG_DRIVE_STRENGTH:
		arg = esp32s31_drive_ma[FIELD_GET(ESP32S31_IOMUX_FUN_DRV, iomux)];
		on = true;
		break;
	default:
		return -ENOTSUPP;
	}

	if (!on)
		return -EINVAL;

	*config = pinconf_to_config_packed(param, arg);
	return 0;
}

static int esp32s31_drive_field(u32 ma)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(esp32s31_drive_ma); i++)
		if (esp32s31_drive_ma[i] == ma)
			return FIELD_PREP(ESP32S31_IOMUX_FUN_DRV, i);

	return -EINVAL;
}

static int esp32s31_config_one(struct esp32s31_pinctrl *pctrl, unsigned int pin,
			       unsigned long config)
{
	u32 arg = pinconf_to_config_argument(config);
	void __iomem *pad = pctrl->gpio + ESP32S31_GPIO_PIN(pin);
	u32 pull = ESP32S31_IOMUX_FUN_WPU | ESP32S31_IOMUX_FUN_WPD;
	int drv;

	switch (pinconf_to_config_param(config)) {
	case PIN_CONFIG_BIAS_DISABLE:
		esp32s31_iomux_rmw(pctrl, pin, pull, 0);
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		esp32s31_iomux_rmw(pctrl, pin, pull, ESP32S31_IOMUX_FUN_WPU);
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		esp32s31_iomux_rmw(pctrl, pin, pull, ESP32S31_IOMUX_FUN_WPD);
		break;
	case PIN_CONFIG_INPUT_ENABLE:
		esp32s31_iomux_rmw(pctrl, pin, ESP32S31_IOMUX_FUN_IE,
				   arg ? ESP32S31_IOMUX_FUN_IE : 0);
		break;
	case PIN_CONFIG_DRIVE_STRENGTH:
		drv = esp32s31_drive_field(arg);
		if (drv < 0)
			return drv;
		esp32s31_iomux_rmw(pctrl, pin, ESP32S31_IOMUX_FUN_DRV, drv);
		break;
	case PIN_CONFIG_DRIVE_OPEN_DRAIN:
	case PIN_CONFIG_DRIVE_PUSH_PULL: {
		bool od = pinconf_to_config_param(config) == PIN_CONFIG_DRIVE_OPEN_DRAIN;

		guard(raw_spinlock_irqsave)(&pctrl->lock);
		writel((readl(pad) & ~ESP32S31_PIN_PAD_DRIVER) |
		       (od ? ESP32S31_PIN_PAD_DRIVER : 0), pad);
		break;
	}
	default:
		return -ENOTSUPP;
	}

	return 0;
}

static int esp32s31_pin_config_set(struct pinctrl_dev *pctldev,
				   unsigned int pin, unsigned long *configs,
				   unsigned int num_configs)
{
	struct esp32s31_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);
	unsigned int i;
	int ret;

	for (i = 0; i < num_configs; i++) {
		ret = esp32s31_config_one(pctrl, pin, configs[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int esp32s31_pin_config_group_set(struct pinctrl_dev *pctldev,
					 unsigned int gsel,
					 unsigned long *configs,
					 unsigned int num_configs)
{
	const struct group_desc *group;
	unsigned int i;
	int ret;

	group = pinctrl_generic_get_group(pctldev, gsel);
	if (!group)
		return -EINVAL;

	for (i = 0; i < group->grp.npins; i++) {
		ret = esp32s31_pin_config_set(pctldev, group->grp.pins[i],
					      configs, num_configs);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct pinctrl_ops esp32s31_pctrl_ops = {
	.get_groups_count	= pinctrl_generic_get_group_count,
	.get_group_name		= pinctrl_generic_get_group_name,
	.get_group_pins		= pinctrl_generic_get_group_pins,
	.dt_node_to_map		= pinctrl_generic_pinmux_dt_node_to_map,
	.dt_free_map		= pinctrl_utils_free_map,
};

static const struct pinmux_ops esp32s31_pmx_ops = {
	.get_functions_count	= pinmux_generic_get_function_count,
	.get_function_name	= pinmux_generic_get_function_name,
	.get_function_groups	= pinmux_generic_get_function_groups,
	.set_mux		= esp32s31_set_mux,
	.gpio_request_enable	= esp32s31_gpio_request_enable,
	.strict			= true,
};

static const struct pinconf_ops esp32s31_pinconf_ops = {
	.pin_config_get		= esp32s31_pin_config_get,
	.pin_config_set		= esp32s31_pin_config_set,
	.pin_config_group_set	= esp32s31_pin_config_group_set,
	.is_generic		= true,
};

static int esp32s31_gpio_set(struct gpio_chip *chip, unsigned int offset,
			     int value)
{
	struct esp32s31_pinctrl *pctrl = gpiochip_get_data(chip);
	unsigned int reg;

	if (value)
		reg = esp32s31_bank(offset, ESP32S31_GPIO_OUT_W1TS,
				    ESP32S31_GPIO_OUT1_W1TS);
	else
		reg = esp32s31_bank(offset, ESP32S31_GPIO_OUT_W1TC,
				    ESP32S31_GPIO_OUT1_W1TC);

	/* Set and clear are different registers, so no lock is necessary. */
	writel(BIT(offset % 32), pctrl->gpio + reg);
	return 0;
}

static int esp32s31_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct esp32s31_pinctrl *pctrl = gpiochip_get_data(chip);
	unsigned int reg = esp32s31_bank(offset, ESP32S31_GPIO_IN,
					 ESP32S31_GPIO_IN1);

	return !!(readl(pctrl->gpio + reg) & BIT(offset % 32));
}

static int esp32s31_gpio_get_direction(struct gpio_chip *chip,
				       unsigned int offset)
{
	struct esp32s31_pinctrl *pctrl = gpiochip_get_data(chip);
	unsigned int reg = esp32s31_bank(offset, ESP32S31_GPIO_ENABLE,
					 ESP32S31_GPIO_ENABLE1);

	if (readl(pctrl->gpio + reg) & BIT(offset % 32))
		return GPIO_LINE_DIRECTION_OUT;

	return GPIO_LINE_DIRECTION_IN;
}

static int esp32s31_gpio_direction_input(struct gpio_chip *chip,
					 unsigned int offset)
{
	struct esp32s31_pinctrl *pctrl = gpiochip_get_data(chip);

	writel(BIT(offset % 32), pctrl->gpio +
	       esp32s31_bank(offset, ESP32S31_GPIO_ENABLE_W1TC,
			     ESP32S31_GPIO_ENABLE1_W1TC));
	return 0;
}

static int esp32s31_gpio_direction_output(struct gpio_chip *chip,
					  unsigned int offset, int value)
{
	struct esp32s31_pinctrl *pctrl = gpiochip_get_data(chip);

	/* Set the level before the output driver starts. */
	esp32s31_gpio_set(chip, offset, value);
	writel(BIT(offset % 32), pctrl->gpio +
	       esp32s31_bank(offset, ESP32S31_GPIO_ENABLE_W1TS,
			     ESP32S31_GPIO_ENABLE1_W1TS));
	return 0;
}

static int esp32s31_gpio_set_config(struct gpio_chip *chip, unsigned int offset,
				    unsigned long config)
{
	struct esp32s31_pinctrl *pctrl = gpiochip_get_data(chip);

	return esp32s31_config_one(pctrl, offset, config);
}

static int esp32s31_pins_init(struct device *dev, struct pinctrl_desc *desc)
{
	struct pinctrl_pin_desc *pins;
	unsigned int i;

	pins = devm_kcalloc(dev, ESP32S31_NPINS, sizeof(*pins), GFP_KERNEL);
	if (!pins)
		return -ENOMEM;

	for (i = 0; i < ESP32S31_NPINS; i++) {
		pins[i].number = i;
		pins[i].name = devm_kasprintf(dev, GFP_KERNEL, "GPIO%u", i);
		if (!pins[i].name)
			return -ENOMEM;
	}

	desc->pins = pins;
	desc->npins = ESP32S31_NPINS;
	return 0;
}

static int esp32s31_pinctrl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct esp32s31_pinctrl *pctrl;
	struct gpio_chip *chip;
	int ret;

	pctrl = devm_kzalloc(dev, sizeof(*pctrl), GFP_KERNEL);
	if (!pctrl)
		return -ENOMEM;

	pctrl->iomux = devm_platform_ioremap_resource_byname(pdev, "iomux");
	if (IS_ERR(pctrl->iomux))
		return PTR_ERR(pctrl->iomux);

	pctrl->gpio = devm_platform_ioremap_resource_byname(pdev, "gpio");
	if (IS_ERR(pctrl->gpio))
		return PTR_ERR(pctrl->gpio);

	pctrl->cnnt = devm_platform_ioremap_resource_byname(pdev, "cnnt");
	if (IS_ERR(pctrl->cnnt))
		return PTR_ERR(pctrl->cnnt);

	raw_spin_lock_init(&pctrl->lock);

	ret = esp32s31_pins_init(dev, &pctrl->desc);
	if (ret)
		return ret;

	pctrl->desc.name = dev_name(dev);
	pctrl->desc.pctlops = &esp32s31_pctrl_ops;
	pctrl->desc.pmxops = &esp32s31_pmx_ops;
	pctrl->desc.confops = &esp32s31_pinconf_ops;
	pctrl->desc.owner = THIS_MODULE;

	ret = devm_pinctrl_register_and_init(dev, &pctrl->desc, pctrl,
					     &pctrl->pctl);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register pinctrl\n");

	ret = pinctrl_enable(pctrl->pctl);
	if (ret)
		return ret;

	chip = &pctrl->chip;
	chip->label = dev_name(dev);
	chip->parent = dev;
	chip->owner = THIS_MODULE;
	chip->base = -1;
	chip->ngpio = ESP32S31_NPINS;
	chip->request = gpiochip_generic_request;
	chip->free = gpiochip_generic_free;
	chip->get = esp32s31_gpio_get;
	chip->set = esp32s31_gpio_set;
	chip->get_direction = esp32s31_gpio_get_direction;
	chip->direction_input = esp32s31_gpio_direction_input;
	chip->direction_output = esp32s31_gpio_direction_output;
	chip->set_config = esp32s31_gpio_set_config;

	return devm_gpiochip_add_data(dev, chip, pctrl);
}

static const struct of_device_id esp32s31_pinctrl_of_match[] = {
	{ .compatible = "esp,esp32s31-pinctrl" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_pinctrl_of_match);

static struct platform_driver esp32s31_pinctrl_driver = {
	.driver = {
		.name = "esp32s31-pinctrl",
		.of_match_table = esp32s31_pinctrl_of_match,
	},
	.probe = esp32s31_pinctrl_probe,
};
builtin_platform_driver(esp32s31_pinctrl_driver);
