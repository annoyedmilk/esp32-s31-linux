// SPDX-License-Identifier: GPL-2.0-only
/* ESP32-S31 standard I2S, with cyclic AHB DMA and a shared pad clock. */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/reset.h>
#include <sound/dmaengine_pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#define I2S_RX_CONF		0x20
#define I2S_TX_CONF		0x24
#define I2S_RX_CONF1		0x28
#define I2S_TX_CONF1		0x2c
#define I2S_RX_RECOMB		0x30
#define I2S_TX_PCM2PDM		0x44
#define I2S_RX_PDM2PCM		0x4c
#define I2S_RX_TDM		0x50
#define I2S_TX_TDM		0x54
#define I2S_RX_EOF		0x64
#define I2S_DESTINATION		0xf4
#define I2S_RESET		BIT(0)
#define I2S_FIFO_RESET		BIT(1)
#define I2S_START		BIT(2)
#define I2S_UPDATE		BIT(8)
#define I2S_PCM_BYPASS		BIT(12)
#define I2S_LEFT_ALIGN		BIT(15)
#define I2S_MSB_SHIFT		BIT(13)
#define I2S_TDM_ENABLE		BIT(19)
#define I2S_BCLK_DIV		GENMASK(26, 21)
#define I2S_WS_WIDTH		GENMASK(8, 0)
#define I2S_DATA_BITS		GENMASK(18, 14)
#define I2S_HALF_SAMPLE		GENMASK(26, 19)
#define I2S_SLOT_BITS		GENMASK(31, 27)
#define I2S_TDM_SLOTS		GENMASK(19, 16)
#define I2S_PCM_BUFFER_SIZE	8192

struct esp32s31_i2s {
	void __iomem *base;
	struct clk *mclk;
	/* Serialize playback and capture, which share the clock pads. */
	struct mutex lock;
	bool opened;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins[2];
	struct snd_dmaengine_dai_dma_data playback;
	struct snd_dmaengine_dai_dma_data capture;
};

/* Both directions drive the same BCLK/WS pads. Serialize stream opens. */
static int esp32s31_i2s_startup(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct esp32s31_i2s *i2s = snd_soc_dai_get_drvdata(dai);
	int ret;

	guard(mutex)(&i2s->lock);
	if (i2s->opened)
		return -EBUSY;
	ret = pinctrl_select_state(i2s->pinctrl, i2s->pins[substream->stream]);
	if (ret)
		return ret;
	ret = clk_prepare_enable(i2s->mclk);
	if (ret)
		return ret;
	i2s->opened = true;
	return 0;
}

static void esp32s31_i2s_shutdown(struct snd_pcm_substream *substream,
				  struct snd_soc_dai *dai)
{
	struct esp32s31_i2s *i2s = snd_soc_dai_get_drvdata(dai);

	guard(mutex)(&i2s->lock);
	clk_disable_unprepare(i2s->mclk);
	i2s->opened = false;
}

static int esp32s31_i2s_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S ||
	    (fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF ||
	    (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_BP_FP)
		return -EINVAL;
	return 0;
}

static int esp32s31_i2s_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	struct esp32s31_i2s *i2s = snd_soc_dai_get_drvdata(dai);
	bool playback = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	unsigned int reg = playback ? I2S_TX_CONF : I2S_RX_CONF;
	unsigned int mclk = params_rate(params) * 256;
	u32 conf, slots;
	int ret;

	if (params_format(params) != SNDRV_PCM_FORMAT_S16_LE ||
	    params_channels(params) != 2)
		return -EINVAL;
	ret = clk_set_rate(i2s->mclk, mclk);
	if (ret)
		return ret;
	/* A fractional crystal divider must stay within 100 ppm. */
	if (abs((long)clk_get_rate(i2s->mclk) - mclk) > mclk / 10000)
		return -EINVAL;

	conf = I2S_TDM_ENABLE | I2S_MSB_SHIFT | I2S_PCM_BYPASS |
	       I2S_LEFT_ALIGN | FIELD_PREP(I2S_BCLK_DIV, 7);
	writel(conf | I2S_RESET | I2S_FIFO_RESET, i2s->base + reg);
	writel(conf, i2s->base + reg);
	slots = FIELD_PREP(I2S_WS_WIDTH, 15) |
		FIELD_PREP(I2S_DATA_BITS, 15) |
		FIELD_PREP(I2S_HALF_SAMPLE, 15) |
		FIELD_PREP(I2S_SLOT_BITS, 15);
	writel(slots, i2s->base + (playback ? I2S_TX_CONF1 : I2S_RX_CONF1));
	writel(FIELD_PREP(I2S_TDM_SLOTS, 1) | GENMASK(1, 0),
	       i2s->base + (playback ? I2S_TX_TDM : I2S_RX_TDM));
	if (!playback)
		writel(params_period_bytes(params), i2s->base + I2S_RX_EOF);
	return 0;
}

static int esp32s31_i2s_trigger(struct snd_pcm_substream *substream, int cmd,
				struct snd_soc_dai *dai)
{
	struct esp32s31_i2s *i2s = snd_soc_dai_get_drvdata(dai);
	unsigned int reg = substream->stream == SNDRV_PCM_STREAM_PLAYBACK ?
			   I2S_TX_CONF : I2S_RX_CONF;
	u32 conf = readl(i2s->base + reg);
	u32 value;
	int ret;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		writel(conf | I2S_UPDATE, i2s->base + reg);
		ret = readl_poll_timeout_atomic(i2s->base + reg, value,
						!(value & I2S_UPDATE), 1, 1000);
		if (ret)
			return ret;
		writel(conf | I2S_START, i2s->base + reg);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		writel(conf & ~I2S_START, i2s->base + reg);
		return 0;
	default:
		return -EINVAL;
	}
}

static int esp32s31_i2s_dai_probe(struct snd_soc_dai *dai)
{
	struct esp32s31_i2s *i2s = snd_soc_dai_get_drvdata(dai);

	snd_soc_dai_init_dma_data(dai, &i2s->playback, &i2s->capture);
	return 0;
}

static const struct snd_soc_dai_ops esp32s31_i2s_ops = {
	.probe = esp32s31_i2s_dai_probe,
	.startup = esp32s31_i2s_startup,
	.shutdown = esp32s31_i2s_shutdown,
	.set_fmt = esp32s31_i2s_set_fmt,
	.hw_params = esp32s31_i2s_hw_params,
	.trigger = esp32s31_i2s_trigger,
};

static struct snd_soc_dai_driver esp32s31_i2s_dai = {
	.name = "esp32s31-i2s",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.capture = {
		.stream_name = "Capture",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.ops = &esp32s31_i2s_ops,
};

static const struct snd_pcm_hardware esp32s31_i2s_pcm_hardware = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.buffer_bytes_max = I2S_PCM_BUFFER_SIZE,
	.period_bytes_min = 512,
	.period_bytes_max = 4032,
	.periods_min = 2,
	.periods_max = 16,
	.fifo_size = 64,
};

static const struct snd_dmaengine_pcm_config esp32s31_i2s_pcm_config = {
	.pcm_hardware = &esp32s31_i2s_pcm_hardware,
	.prealloc_buffer_size = I2S_PCM_BUFFER_SIZE,
	.prepare_slave_config = snd_dmaengine_pcm_prepare_slave_config,
};

static const struct snd_soc_component_driver esp32s31_i2s_component = {
	.name = "esp32s31-i2s",
	.legacy_dai_naming = 1,
};

static void esp32s31_i2s_stop(void *data)
{
	struct esp32s31_i2s *i2s = data;

	writel(0, i2s->base + I2S_RX_CONF);
	writel(0, i2s->base + I2S_TX_CONF);
}

static int esp32s31_i2s_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct esp32s31_i2s *i2s;
	struct reset_control *reset;
	struct resource *res;
	struct clk *apb;
	int ret;

	i2s = devm_kzalloc(dev, sizeof(*i2s), GFP_KERNEL);
	if (!i2s)
		return -ENOMEM;
	i2s->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(i2s->base))
		return PTR_ERR(i2s->base);
	apb = devm_clk_get_enabled(dev, "apb");
	if (IS_ERR(apb))
		return PTR_ERR(apb);
	reset = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(reset))
		return PTR_ERR(reset);
	ret = reset_control_assert(reset);
	if (ret)
		return ret;
	ret = reset_control_deassert(reset);
	if (ret)
		return ret;
	i2s->mclk = devm_clk_get(dev, "mclk");
	if (IS_ERR(i2s->mclk))
		return PTR_ERR(i2s->mclk);
	ret = devm_add_action_or_reset(dev, esp32s31_i2s_stop, i2s);
	if (ret)
		return ret;
	writel(0, i2s->base + I2S_RX_RECOMB);
	writel(0, i2s->base + I2S_TX_PCM2PDM);
	writel(0, i2s->base + I2S_RX_PDM2PCM);
	writel(0, i2s->base + I2S_DESTINATION);
	i2s->playback.addr = res->start;
	i2s->playback.addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	i2s->playback.maxburst = 4;
	i2s->capture = i2s->playback;
	i2s->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(i2s->pinctrl))
		return PTR_ERR(i2s->pinctrl);
	i2s->pins[SNDRV_PCM_STREAM_PLAYBACK] =
		pinctrl_lookup_state(i2s->pinctrl, PINCTRL_STATE_DEFAULT);
	i2s->pins[SNDRV_PCM_STREAM_CAPTURE] =
		pinctrl_lookup_state(i2s->pinctrl, "capture");
	if (IS_ERR(i2s->pins[0]) || IS_ERR(i2s->pins[1]))
		return dev_err_probe(dev, -EINVAL, "missing clock pin states\n");
	mutex_init(&i2s->lock);
	platform_set_drvdata(pdev, i2s);
	ret = devm_snd_dmaengine_pcm_register(dev, &esp32s31_i2s_pcm_config, 0);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register DMA PCM\n");
	return devm_snd_soc_register_component(dev, &esp32s31_i2s_component,
					     &esp32s31_i2s_dai, 1);
}

static const struct of_device_id esp32s31_i2s_ids[] = {
	{ .compatible = "esp,esp32s31-i2s" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_i2s_ids);

static struct platform_driver esp32s31_i2s_driver = {
	.probe = esp32s31_i2s_probe,
	.driver = {
		.name = "esp32s31-i2s",
		.of_match_table = esp32s31_i2s_ids,
	},
};
module_platform_driver(esp32s31_i2s_driver);

MODULE_DESCRIPTION("ESP32-S31 standard I2S controller");
MODULE_LICENSE("GPL");
