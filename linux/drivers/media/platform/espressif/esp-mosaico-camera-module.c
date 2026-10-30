// SPDX-License-Identifier: GPL-2.0-only
/* EEPROM presence and sensor lifetime for Mosaico's removable left camera. */

#include <linux/crc16.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#define MOSAICO_DESCRIPTOR_SIZE	0x86
#define MOSAICO_SCAN_MS		250
#define MOSAICO_DEBOUNCE		3

struct mosaico_camera_module {
	struct i2c_client *eeprom;
	struct i2c_client *sensor;
	struct device_node *sensor_node;
	struct delayed_work scan;
	u8 descriptor[MOSAICO_DESCRIPTOR_SIZE];
	unsigned int matches;
	unsigned int misses;
};

static int mosaico_camera_descriptor(struct i2c_client *client, u8 *raw)
{
	u8 offset = 0;
	struct i2c_msg messages[] = {
		{ .addr = client->addr, .len = 1, .buf = &offset },
		{ .addr = client->addr, .flags = I2C_M_RD,
		  .len = MOSAICO_DESCRIPTOR_SIZE, .buf = raw },
	};
	int ret;

	ret = i2c_transfer(client->adapter, messages, ARRAY_SIZE(messages));
	if (ret != ARRAY_SIZE(messages))
		return ret < 0 ? ret : -EIO;
	if (memcmp(raw, "ESP", 3) || raw[3] != 0x07)
		return -ENODEV;
	if (crc16(0xffff, raw, 0x34) != get_unaligned_le16(raw + 0x34) ||
	    crc16(0xffff, raw + 0x36, 8) != get_unaligned_le16(raw + 0x3e) ||
	    crc16(0xffff, raw + 0x40, 0x44) != get_unaligned_le16(raw + 0x84) ||
	    get_unaligned_le16(raw + 0x42) > 64)
		return -EBADMSG;
	return 0;
}

static void mosaico_camera_scan(struct work_struct *work)
{
	struct mosaico_camera_module *module = container_of(to_delayed_work(work),
						struct mosaico_camera_module, scan);
	struct i2c_board_info info = {};
	u8 raw[MOSAICO_DESCRIPTOR_SIZE];
	u32 address;
	int ret;

	ret = mosaico_camera_descriptor(module->eeprom, raw);
	if (ret) {
		module->matches = 0;
		if (module->misses < MOSAICO_DEBOUNCE)
			module->misses++;
		if (module->sensor && module->misses == MOSAICO_DEBOUNCE) {
			i2c_unregister_device(module->sensor);
			module->sensor = NULL;
			dev_info(&module->eeprom->dev, "left camera disconnected\n");
		}
		goto reschedule;
	}
	module->misses = 0;
	if (memcmp(module->descriptor, raw, sizeof(raw))) {
		memcpy(module->descriptor, raw, sizeof(raw));
		module->matches = 0;
		if (module->sensor) {
			i2c_unregister_device(module->sensor);
			module->sensor = NULL;
		}
	}
	if (module->sensor)
		goto reschedule;
	if (++module->matches < MOSAICO_DEBOUNCE)
		goto reschedule;
	module->matches = 0;

	ret = of_property_read_u32(module->sensor_node, "reg", &address);
	if (ret || address > 0x7f)
		goto reschedule;
	info.addr = address;
	info.fwnode = of_fwnode_handle(module->sensor_node);
	strscpy(info.type, "sc101iot", sizeof(info.type));
	module->sensor = i2c_new_client_device(module->eeprom->adapter, &info);
	if (IS_ERR(module->sensor)) {
		dev_err_ratelimited(&module->eeprom->dev,
				    "cannot create camera sensor: %ld\n",
				    PTR_ERR(module->sensor));
		module->sensor = NULL;
	} else {
		dev_info(&module->eeprom->dev, "left camera descriptor accepted\n");
	}
reschedule:
	schedule_delayed_work(&module->scan, msecs_to_jiffies(MOSAICO_SCAN_MS));
}

static int mosaico_camera_module_probe(struct i2c_client *client)
{
	struct mosaico_camera_module *module;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	module = devm_kzalloc(&client->dev, sizeof(*module), GFP_KERNEL);
	if (!module)
		return -ENOMEM;
	module->sensor_node = of_parse_phandle(client->dev.of_node, "sensor", 0);
	if (!module->sensor_node)
		return -EINVAL;
	module->eeprom = client;
	i2c_set_clientdata(client, module);
	INIT_DELAYED_WORK(&module->scan, mosaico_camera_scan);
	schedule_delayed_work(&module->scan, 0);
	return 0;
}

static void mosaico_camera_module_remove(struct i2c_client *client)
{
	struct mosaico_camera_module *module = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&module->scan);
	if (module->sensor)
		i2c_unregister_device(module->sensor);
	of_node_put(module->sensor_node);
}

static const struct of_device_id mosaico_camera_module_ids[] = {
	{ .compatible = "esp-mosaico,camera-module" },
	{ }
};
MODULE_DEVICE_TABLE(of, mosaico_camera_module_ids);

static struct i2c_driver mosaico_camera_module_driver = {
	.probe = mosaico_camera_module_probe,
	.remove = mosaico_camera_module_remove,
	.driver = {
		.name = "esp-mosaico-camera-module",
		.of_match_table = mosaico_camera_module_ids,
	},
};
module_i2c_driver(mosaico_camera_module_driver);

MODULE_DESCRIPTION("ESP-Mosaico removable left camera module");
MODULE_LICENSE("GPL");
