// SPDX-License-Identifier: GPL-2.0-only
/* Select the ESP32-S31 ROM download path before the SBI system reset. */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>

#define ESP32S31_FORCE_DOWNLOAD	BIT(2)

struct esp32s31_reboot_mode {
	void __iomem *ctrl;
	struct notifier_block notifier;
	bool bootloader;
};

static ssize_t next_mode_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct esp32s31_reboot_mode *mode = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", READ_ONCE(mode->bootloader) ?
			   "bootloader" : "normal");
}

static ssize_t next_mode_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct esp32s31_reboot_mode *mode = dev_get_drvdata(dev);

	if (sysfs_streq(buf, "bootloader"))
		WRITE_ONCE(mode->bootloader, true);
	else if (sysfs_streq(buf, "normal"))
		WRITE_ONCE(mode->bootloader, false);
	else
		return -EINVAL;
	return count;
}
static DEVICE_ATTR_RW(next_mode);

static struct attribute *esp32s31_reboot_mode_attrs[] = {
	&dev_attr_next_mode.attr,
	NULL,
};
ATTRIBUTE_GROUPS(esp32s31_reboot_mode);

static int esp32s31_reboot_mode_notify(struct notifier_block *notifier,
				      unsigned long action, void *command)
{
	struct esp32s31_reboot_mode *mode = container_of(notifier,
					struct esp32s31_reboot_mode, notifier);
	bool bootloader = READ_ONCE(mode->bootloader);
	u32 value;

	if (action != SYS_RESTART)
		return NOTIFY_DONE;
	/* Also support callers of the standard LINUX_REBOOT_CMD_RESTART2 API. */
	if (command)
		bootloader = !strcmp(command, "bootloader");
	value = readl(mode->ctrl) & ~ESP32S31_FORCE_DOWNLOAD;
	if (bootloader)
		value |= ESP32S31_FORCE_DOWNLOAD;
	writel(value, mode->ctrl);
	readl(mode->ctrl);
	return NOTIFY_DONE;
}

static int esp32s31_reboot_mode_probe(struct platform_device *pdev)
{
	struct esp32s31_reboot_mode *mode;
	int ret;

	mode = devm_kzalloc(&pdev->dev, sizeof(*mode), GFP_KERNEL);
	if (!mode)
		return -ENOMEM;
	mode->ctrl = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mode->ctrl))
		return PTR_ERR(mode->ctrl);
	mode->notifier.notifier_call = esp32s31_reboot_mode_notify;
	platform_set_drvdata(pdev, mode);
	ret = devm_register_reboot_notifier(&pdev->dev, &mode->notifier);
	if (ret)
		return ret;
	return 0;
}

static const struct of_device_id esp32s31_reboot_mode_ids[] = {
	{ .compatible = "esp,esp32s31-reboot-mode" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_reboot_mode_ids);

static struct platform_driver esp32s31_reboot_mode_driver = {
	.probe = esp32s31_reboot_mode_probe,
	.driver = {
		.name = "esp32s31-reboot-mode",
		.of_match_table = esp32s31_reboot_mode_ids,
		.dev_groups = esp32s31_reboot_mode_groups,
	},
};
module_platform_driver(esp32s31_reboot_mode_driver);

MODULE_DESCRIPTION("ESP32-S31 ROM download reboot mode");
MODULE_LICENSE("GPL");
