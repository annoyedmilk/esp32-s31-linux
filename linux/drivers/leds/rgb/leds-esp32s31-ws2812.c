// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Marco Müller <hello@annoyedmilk.ch>
 *
 * WS2812 RGB LED on RMT channel 0 of the ESP32-S31.
 *
 * The clock driver gives the RMT clock and reset, and pinctrl routes the
 * channel 0 output to the LED pin.  This driver powers the RMT RAM, gives the
 * CPU direct access to it, sets a 50 ns channel tick and uses channel 0.
 *
 * One RMT word holds two pulses: duration and level for each.  A WS2812 bit
 * is one high pulse and one low pulse, so one word sends one bit.  24 words
 * send one LED in GRB order, and a word with duration 0 ends the sequence.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/led-class-multicolor.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#define RMT_CH0_CONF0			0x20
#define RMT_CONF0_TX_START		BIT(0)
#define RMT_CONF0_MEM_RD_RST		BIT(1)
#define RMT_CONF0_APB_MEM_RST		BIT(2)
#define RMT_CONF0_IDLE_OUT_EN		BIT(6)
#define RMT_CONF0_DIV_CNT		GENMASK(15, 8)
#define RMT_CONF0_MEM_SIZE		GENMASK(19, 16)
#define RMT_CONF0_CONF_UPDATE		BIT(24)
#define RMT_INT_RAW			0x70
#define RMT_INT_CLR			0x7c
#define RMT_INT_CH0_TX_END		BIT(0)
#define RMT_SYS_CONF			0xc0
#define RMT_SYS_APB_FIFO_MASK		BIT(0)
#define RMT_CH0_RAM			0x800

/* HP_SYSTEM_RMT_MEM_LP_CTRL */
#define RMT_MEM_LP_EN			BIT(2)
#define RMT_MEM_LP_FORCE_CTRL		BIT(3)

/* One channel tick is 50 ns. */
#define RMT_TICK_HZ			20000000

/* Pulse lengths in 50 ns ticks, from the WS2812B data sheet. */
#define WS2812_T0H			8	/* 0.40 us */
#define WS2812_T0L			17	/* 0.85 us */
#define WS2812_T1H			16	/* 0.80 us */
#define WS2812_T1L			9	/* 0.45 us */
/* Newer WS2812B parts need more than 280 us low to latch. */
#define WS2812_RESET_US			300

#define RMT_WORD(d0, l0, d1, l1) \
	((u32)(d0) | ((u32)(l0) << 15) | ((u32)(d1) << 16) | ((u32)(l1) << 31))

struct esp32s31_ws2812 {
	struct led_classdev_mc mc;
	struct mc_subled subled[3];
	void __iomem *base;
	struct mutex lock;
};

static void esp32s31_ws2812_conf0(struct esp32s31_ws2812 *priv, u32 set)
{
	writel(readl(priv->base + RMT_CH0_CONF0) | set,
	       priv->base + RMT_CH0_CONF0);
}

static int esp32s31_ws2812_send(struct esp32s31_ws2812 *priv, u32 grb)
{
	u32 conf0, raw;
	int i, ret;

	for (i = 0; i < 24; i++) {
		bool one = grb & BIT(23 - i);

		writel(one ? RMT_WORD(WS2812_T1H, 1, WS2812_T1L, 0) :
			     RMT_WORD(WS2812_T0H, 1, WS2812_T0L, 0),
		       priv->base + RMT_CH0_RAM + 4 * i);
	}
	writel(0, priv->base + RMT_CH0_RAM + 4 * 24);

	/* Set the read pointer back to the start of the channel RAM. */
	conf0 = readl(priv->base + RMT_CH0_CONF0);
	writel(conf0 | RMT_CONF0_MEM_RD_RST | RMT_CONF0_APB_MEM_RST,
	       priv->base + RMT_CH0_CONF0);
	writel(conf0 & ~(RMT_CONF0_MEM_RD_RST | RMT_CONF0_APB_MEM_RST),
	       priv->base + RMT_CH0_CONF0);

	writel(RMT_INT_CH0_TX_END, priv->base + RMT_INT_CLR);
	esp32s31_ws2812_conf0(priv, RMT_CONF0_CONF_UPDATE);
	esp32s31_ws2812_conf0(priv, RMT_CONF0_TX_START);

	/* 24 bits take 30 us. */
	ret = readl_poll_timeout(priv->base + RMT_INT_RAW, raw,
				 raw & RMT_INT_CH0_TX_END, 5, 1000);
	writel(RMT_INT_CH0_TX_END, priv->base + RMT_INT_CLR);
	usleep_range(WS2812_RESET_US, WS2812_RESET_US + 50);

	return ret;
}

static int esp32s31_ws2812_set(struct led_classdev *cdev,
			       enum led_brightness brightness)
{
	struct led_classdev_mc *mc = lcdev_to_mccdev(cdev);
	struct esp32s31_ws2812 *priv =
		container_of(mc, struct esp32s31_ws2812, mc);
	u32 grb;
	int ret;

	led_mc_calc_color_components(mc, brightness);

	/* subled 0 is red, 1 is green, 2 is blue; the wire order is GRB. */
	grb = (mc->subled_info[1].brightness << 16) |
	      (mc->subled_info[0].brightness << 8) |
	      mc->subled_info[2].brightness;

	mutex_lock(&priv->lock);
	ret = esp32s31_ws2812_send(priv, grb);
	mutex_unlock(&priv->lock);

	return ret;
}

static int esp32s31_ws2812_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct led_init_data init_data = { };
	struct esp32s31_ws2812 *priv;
	struct device_node *child;
	struct reset_control *rst;
	struct clk *clk;
	void __iomem *mem_lp;
	unsigned long div;
	static const int colors[] = {
		LED_COLOR_ID_RED, LED_COLOR_ID_GREEN, LED_COLOR_ID_BLUE,
	};
	int i, ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	mem_lp = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(mem_lp))
		return PTR_ERR(mem_lp);

	ret = devm_mutex_init(dev, &priv->lock);
	if (ret)
		return ret;

	clk = devm_clk_get_enabled(dev, "sys");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "no RMT bus clock\n");

	clk = devm_clk_get_enabled(dev, "core");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "no RMT clock\n");

	rst = devm_reset_control_get_exclusive_deasserted(dev, NULL);
	if (IS_ERR(rst))
		return dev_err_probe(dev, PTR_ERR(rst), "no RMT reset\n");

	div = DIV_ROUND_CLOSEST(clk_get_rate(clk), RMT_TICK_HZ);
	if (!div || div > FIELD_MAX(RMT_CONF0_DIV_CNT))
		return dev_err_probe(dev, -EINVAL, "RMT clock rate not usable\n");

	/* The RMT RAM is off after reset: let the PMU power it, as ESP-IDF does. */
	writel(readl(mem_lp) & ~(RMT_MEM_LP_EN | RMT_MEM_LP_FORCE_CTRL), mem_lp);

	/* The CPU writes the channel RAM directly, not through the FIFO. */
	writel(readl(priv->base + RMT_SYS_CONF) | RMT_SYS_APB_FIFO_MASK,
	       priv->base + RMT_SYS_CONF);

	/* Channel 0: one RAM block, no carrier, output low when idle. */
	writel(FIELD_PREP(RMT_CONF0_DIV_CNT, div) |
	       FIELD_PREP(RMT_CONF0_MEM_SIZE, 1) | RMT_CONF0_IDLE_OUT_EN,
	       priv->base + RMT_CH0_CONF0);

	child = of_get_next_available_child(dev->of_node, NULL);
	if (!child)
		return dev_err_probe(dev, -EINVAL, "no LED node\n");

	for (i = 0; i < ARRAY_SIZE(colors); i++) {
		priv->subled[i].color_index = colors[i];
		priv->subled[i].intensity = LED_FULL;
	}

	priv->mc.subled_info = priv->subled;
	priv->mc.num_colors = ARRAY_SIZE(colors);
	priv->mc.led_cdev.max_brightness = LED_FULL;
	priv->mc.led_cdev.brightness_set_blocking = esp32s31_ws2812_set;

	init_data.fwnode = of_fwnode_handle(child);
	ret = devm_led_classdev_multicolor_register_ext(dev, &priv->mc,
							&init_data);
	of_node_put(child);
	if (ret)
		return dev_err_probe(dev, ret, "cannot register the LED\n");

	/* The LED can keep a color from before a reset. */
	return esp32s31_ws2812_send(priv, 0);
}

static const struct of_device_id esp32s31_ws2812_of_match[] = {
	{ .compatible = "esp,esp32s31-rmt-ws2812" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_ws2812_of_match);

static struct platform_driver esp32s31_ws2812_driver = {
	.probe = esp32s31_ws2812_probe,
	.driver = {
		.name = "esp32s31-ws2812",
		.of_match_table = esp32s31_ws2812_of_match,
	},
};
module_platform_driver(esp32s31_ws2812_driver);

MODULE_DESCRIPTION("ESP32-S31 RMT WS2812 RGB LED driver");
MODULE_AUTHOR("Marco Müller <hello@annoyedmilk.ch>");
MODULE_LICENSE("GPL");
