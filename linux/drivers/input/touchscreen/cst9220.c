// SPDX-License-Identifier: GPL-2.0-only
/* Hynitron CST9220 runtime protocol; no firmware updates. */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/unaligned.h>

#define CST9220_REG_REPORT		0xd000
#define CST9220_REG_DEBUG_MODE		0xd101
#define CST9220_REG_NORMAL_MODE		0xd109
#define CST9220_REG_PROJECT_ID		0xd204
#define CST9220_CHIP_ID			0x9220
#define CST9220_PROJECT_LEGACY		0x542f
#define CST9220_REPORT_ACK		0xab
#define CST9220_MAX_POINTS		5
#define CST9220_MAX_SLOTS		16
#define CST9220_POINT_SIZE		5
#define CST9220_REPORT_SIZE		(CST9220_MAX_POINTS * CST9220_POINT_SIZE + 5)
#define CST9220_POLL_INTERVAL_MS		20

struct cst9220 {
	struct i2c_client *client;
	struct input_dev *input;
	struct touchscreen_properties prop;
	bool legacy;
};

static int cst9220_write_command(struct cst9220 *ts, u16 command)
{
	u8 data[] = { command >> 8, command };
	int ret;

	ret = i2c_master_send(ts->client, data, sizeof(data));
	if (ret < 0)
		return ret;

	return ret == sizeof(data) ? 0 : -EIO;
}

/* The controller requires a STOP between address selection and the read. */
static int cst9220_read_register(struct cst9220 *ts, u16 reg, u8 *data, int len)
{
	int ret;

	ret = cst9220_write_command(ts, reg);
	if (ret)
		return ret;

	ret = i2c_master_recv(ts->client, data, len);
	if (ret < 0)
		return ret;

	return ret == len ? 0 : -EIO;
}

static int cst9220_read_report(struct cst9220 *ts, u8 *report)
{
	u8 ack[] = { CST9220_REG_REPORT >> 8, 0, CST9220_REPORT_ACK };
	int ret;

	ret = cst9220_read_register(ts, CST9220_REG_REPORT, report,
				    CST9220_REPORT_SIZE);
	if (ret)
		return ret;

	/* Current firmware holds the frame until acknowledged. */
	if (!ts->legacy) {
		ret = i2c_master_send(ts->client, ack, sizeof(ack));
		if (ret < 0)
			return ret;
		if (ret != sizeof(ack))
			return -EIO;
	}

	if (!report[0])
		return 0;
	if (report[0] == CST9220_REPORT_ACK || report[6] != CST9220_REPORT_ACK)
		return -EPROTO;
	if ((report[5] & 0x7f) > CST9220_MAX_POINTS)
		return -EPROTO;

	return report[5] & 0x7f;
}

static void cst9220_poll(struct input_dev *input)
{
	struct cst9220 *ts = input_get_drvdata(input);
	u8 report[CST9220_REPORT_SIZE];
	int count, i;

	count = cst9220_read_report(ts, report);
	if (count < 0)
		dev_err_ratelimited(&ts->client->dev, "report read failed: %d\n", count);

	for (i = 0; i < count; i++) {
		const u8 *point = report + (i ? i * CST9220_POINT_SIZE + 2 : 0);
		unsigned int x = (point[1] << 4) | (point[3] >> 4);
		unsigned int y = (point[2] << 4) | (point[3] & 0x0f);
		unsigned int slot = point[0] >> 4;

		/* Release records retain their coordinates and clear only the status. */
		if (((point[0] & 0x0f) >> 1) != 3 ||
		    x > ts->prop.max_x || y > ts->prop.max_y)
			continue;

		input_mt_slot(input, slot);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, true);
		touchscreen_report_pos(input, &ts->prop, x, y, true);
	}

	/* Release stale contacts on both empty frames and transfer errors. */
	input_mt_sync_frame(input);
	input_sync(input);
}

static int cst9220_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct cst9220 *ts;
	u8 identity[4];
	u16 project;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	ts->client = client;

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable supply\n");
	msleep(100);

	ret = cst9220_write_command(ts, CST9220_REG_DEBUG_MODE);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enter debug mode\n");
	usleep_range(10000, 11000);

	ret = cst9220_read_register(ts, CST9220_REG_PROJECT_ID, identity,
				    sizeof(identity));
	if (ret)
		return dev_err_probe(dev, ret, "failed to read identity\n");
	if (get_unaligned_le16(identity + 2) != CST9220_CHIP_ID)
		return dev_err_probe(dev, -ENODEV, "unsupported controller\n");

	project = get_unaligned_le16(identity);
	ts->legacy = project == CST9220_PROJECT_LEGACY;
	if (ts->legacy) {
		ret = cst9220_write_command(ts, CST9220_REG_NORMAL_MODE);
		if (ret)
			return dev_err_probe(dev, ret, "failed to enter normal mode\n");
		usleep_range(10000, 11000);
	}

	ts->input = devm_input_allocate_device(dev);
	if (!ts->input)
		return -ENOMEM;

	ts->input->name = "CST9220 Touchscreen";
	ts->input->id.bustype = BUS_I2C;
	input_set_drvdata(ts->input, ts);
	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, 479, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, 479, 0, 0);
	touchscreen_parse_properties(ts->input, true, &ts->prop);

	ret = input_mt_init_slots(ts->input, CST9220_MAX_SLOTS,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;

	ret = input_setup_polling(ts->input, cst9220_poll);
	if (ret)
		return ret;
	input_set_poll_interval(ts->input, CST9220_POLL_INTERVAL_MS);

	ret = input_register_device(ts->input);
	if (ret)
		return ret;

	dev_info(dev, "project %#x, %s reports\n", project,
		 ts->legacy ? "legacy" : "acknowledged");
	return 0;
}

static const struct of_device_id cst9220_of_match[] = {
	{ .compatible = "hynitron,cst9220" },
	{}
};
MODULE_DEVICE_TABLE(of, cst9220_of_match);

static struct i2c_driver cst9220_driver = {
	.driver = {
		.name = "cst9220",
		.of_match_table = cst9220_of_match,
	},
	.probe = cst9220_probe,
};
module_i2c_driver(cst9220_driver);

MODULE_DESCRIPTION("Hynitron CST9220 touchscreen driver");
MODULE_LICENSE("GPL");
