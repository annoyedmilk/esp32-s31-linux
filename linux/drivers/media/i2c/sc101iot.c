// SPDX-License-Identifier: GPL-2.0-only
/* SmartSens SC101IOT parallel YUV sensor, with board-supplied 20 MHz XCLK. */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/unaligned.h>
#include <media/v4l2-async.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

#define SC101IOT_WIDTH		640
#define SC101IOT_HEIGHT		480
#define SC101IOT_FIRMWARE	"sc101iot/vga-uyvy.bin"

struct sc101iot {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct mutex lock;
	struct clk *xclk;
	struct regulator *vdd;
	struct gpio_desc *pwdn;
	const struct firmware *firmware;
	bool streaming;
};

static struct sc101iot *to_sc101iot(struct v4l2_subdev *sd)
{
	return container_of(sd, struct sc101iot, sd);
}

/* The register high byte selects an I2C page through register 0xf0. */
static int sc101iot_write(struct i2c_client *client, u16 reg, u8 value)
{
	int ret;

	ret = i2c_smbus_write_byte_data(client, 0xf0, reg >> 8);
	if (ret)
		return ret;
	return i2c_smbus_write_byte_data(client, reg & 0xff, value);
}

static int sc101iot_read(struct i2c_client *client, u16 reg)
{
	int ret;

	ret = i2c_smbus_write_byte_data(client, 0xf0, reg >> 8);
	if (ret)
		return ret;
	return i2c_smbus_read_byte_data(client, reg & 0xff);
}

static int sc101iot_initialize(struct sc101iot *sensor)
{
	struct i2c_client *client = v4l2_get_subdevdata(&sensor->sd);
	const u8 *data = sensor->firmware->data;
	unsigned int i, count = get_unaligned_le16(data + 6);
	int ret;

	ret = sc101iot_read(client, 0x3103);
	if (ret < 0)
		return ret;
	ret = sc101iot_write(client, 0x3103, ret | 1);
	if (ret)
		return ret;
	usleep_range(5000, 6000);

	for (i = 0; i < count; i++) {
		u8 reg = data[12 + i * 2];
		u8 value = data[13 + i * 2];

		if (reg == 0xfe) {
			msleep(value);
			continue;
		}
		ret = i2c_smbus_write_byte_data(client, reg, value);
		if (ret)
			return ret;
	}
	/* Mosaico's SC101IOT DVP timing uses zero PCLK delay (BSP patch). */
	ret = sc101iot_read(client, 0x3640);
	if (ret < 0)
		return ret;
	return sc101iot_write(client, 0x3640, ret & ~3);
}

static int sc101iot_stream(struct v4l2_subdev *sd, int enable)
{
	struct sc101iot *sensor = to_sc101iot(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int value, ret = 0;

	mutex_lock(&sensor->lock);
	if (sensor->streaming == !!enable)
		goto unlock;
	if (enable) {
		gpiod_set_value_cansleep(sensor->pwdn, 0);
		msleep(20);
		ret = sc101iot_initialize(sensor);
		if (ret)
			goto sleep;
	}
	value = sc101iot_read(client, 0x3100);
	if (value < 0) {
		ret = value;
		goto sleep;
	}
	ret = sc101iot_write(client, 0x3100, (value & ~1) | !!enable);
	if (!ret && enable) {
		sensor->streaming = true;
		goto unlock;
	}
sleep:
	gpiod_set_value_cansleep(sensor->pwdn, 1);
	sensor->streaming = false;
unlock:
	mutex_unlock(&sensor->lock);
	return ret;
}

static void sc101iot_format(struct v4l2_mbus_framefmt *format)
{
	*format = (struct v4l2_mbus_framefmt) {
		.width = SC101IOT_WIDTH,
		.height = SC101IOT_HEIGHT,
		.code = MEDIA_BUS_FMT_UYVY8_2X8,
		.field = V4L2_FIELD_NONE,
		.colorspace = V4L2_COLORSPACE_SMPTE170M,
		.ycbcr_enc = V4L2_YCBCR_ENC_601,
		.quantization = V4L2_QUANTIZATION_LIM_RANGE,
		.xfer_func = V4L2_XFER_FUNC_709,
	};
}

static int sc101iot_get_format(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state,
			       struct v4l2_subdev_format *format)
{
	if (format->pad)
		return -EINVAL;
	sc101iot_format(&format->format);
	return 0;
}

static int sc101iot_enum_code(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state,
			      struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad || code->index)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_UYVY8_2X8;
	return 0;
}

static int sc101iot_enum_size(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state,
			      struct v4l2_subdev_frame_size_enum *size)
{
	if (size->pad || size->index || size->code != MEDIA_BUS_FMT_UYVY8_2X8)
		return -EINVAL;
	size->min_width = size->max_width = SC101IOT_WIDTH;
	size->min_height = size->max_height = SC101IOT_HEIGHT;
	return 0;
}

static int sc101iot_get_interval(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_frame_interval *interval)
{
	if (interval->pad)
		return -EINVAL;
	interval->interval = (struct v4l2_fract) { 1, 15 };
	return 0;
}

static const struct v4l2_subdev_video_ops sc101iot_video_ops = {
	.s_stream = sc101iot_stream,
};

static const struct v4l2_subdev_pad_ops sc101iot_pad_ops = {
	.enum_mbus_code = sc101iot_enum_code,
	.enum_frame_size = sc101iot_enum_size,
	.get_fmt = sc101iot_get_format,
	.set_fmt = sc101iot_get_format,
	.get_frame_interval = sc101iot_get_interval,
};

static const struct v4l2_subdev_ops sc101iot_ops = {
	.video = &sc101iot_video_ops,
	.pad = &sc101iot_pad_ops,
};

static void sc101iot_release_firmware(void *data)
{
	release_firmware(data);
}

static void sc101iot_power_off(void *data)
{
	struct sc101iot *sensor = data;

	gpiod_set_value_cansleep(sensor->pwdn, 1);
	clk_disable_unprepare(sensor->xclk);
	regulator_disable(sensor->vdd);
}

static int sc101iot_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct sc101iot *sensor;
	const u8 *data;
	int ret, high, low;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -EOPNOTSUPP;
	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;
	sensor->pwdn = devm_gpiod_get(dev, "powerdown", GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->pwdn))
		return PTR_ERR(sensor->pwdn);
	sensor->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(sensor->vdd))
		return PTR_ERR(sensor->vdd);
	sensor->xclk = devm_clk_get(dev, NULL);
	if (IS_ERR(sensor->xclk))
		return PTR_ERR(sensor->xclk);
	if (clk_get_rate(sensor->xclk) != 20000000)
		return dev_err_probe(dev, -EINVAL, "expected 20 MHz XCLK\n");
	ret = request_firmware(&sensor->firmware, SC101IOT_FIRMWARE, dev);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, sc101iot_release_firmware,
				       (void *)sensor->firmware);
	if (ret)
		return ret;
	data = sensor->firmware->data;
	if (sensor->firmware->size < 12 || memcmp(data, "SC10", 4) ||
	    get_unaligned_le16(data + 4) != 1 ||
	    !get_unaligned_le16(data + 6) ||
	    get_unaligned_le16(data + 6) > 512 || get_unaligned_le32(data + 8) ||
	    sensor->firmware->size != 12 + 2 * get_unaligned_le16(data + 6))
		return dev_err_probe(dev, -EINVAL, "invalid VGA register firmware\n");

	ret = regulator_enable(sensor->vdd);
	if (ret)
		return ret;
	ret = clk_prepare_enable(sensor->xclk);
	if (ret) {
		regulator_disable(sensor->vdd);
		return ret;
	}
	ret = devm_add_action_or_reset(dev, sc101iot_power_off, sensor);
	if (ret)
		return ret;
	gpiod_set_value_cansleep(sensor->pwdn, 0);
	msleep(20);
	high = sc101iot_read(client, 0x31f7);
	low = high < 0 ? high : sc101iot_read(client, 0x31f8);
	gpiod_set_value_cansleep(sensor->pwdn, 1);
	if (high < 0 || low < 0)
		return high < 0 ? high : low;
	if ((high << 8 | low) != 0xda4a)
		return -ENODEV;

	mutex_init(&sensor->lock);
	v4l2_i2c_subdev_init(&sensor->sd, client, &sc101iot_ops);
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		return ret;
	ret = v4l2_async_register_subdev(&sensor->sd);
	if (ret) {
		media_entity_cleanup(&sensor->sd.entity);
		return ret;
	}
	dev_info(dev, "SC101IOT detected, VGA UYVY at 15 fps\n");
	return 0;
}

static void sc101iot_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);

	/* The bridge stops DMA and returns buffers before sensor storage is freed. */
	v4l2_async_unregister_subdev(sd);
	gpiod_set_value_cansleep(to_sc101iot(sd)->pwdn, 1);
	media_entity_cleanup(&sd->entity);
}

static const struct of_device_id sc101iot_ids[] = {
	{ .compatible = "smartsens,sc101iot" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc101iot_ids);

static struct i2c_driver sc101iot_driver = {
	.probe = sc101iot_probe,
	.remove = sc101iot_remove,
	.driver = {
		.name = "sc101iot",
		.of_match_table = sc101iot_ids,
	},
};
module_i2c_driver(sc101iot_driver);

MODULE_FIRMWARE(SC101IOT_FIRMWARE);
MODULE_DESCRIPTION("SmartSens SC101IOT DVP sensor");
MODULE_LICENSE("GPL");
