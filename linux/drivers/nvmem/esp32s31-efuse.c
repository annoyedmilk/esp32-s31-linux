// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Marco Mueller <hello@annoyedmilk.ch>
 *
 * ESP32-S31 eFuse, read only
 *
 * The eFuse controller shows the eFuse blocks in its read registers.  The
 * nvmem offset is the register offset.  BLK1 has the factory MAC address in
 * the bytes 0x50 to 0x55, in the reverse order.
 */

#include <linux/etherdevice.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/nvmem-provider.h>
#include <linux/platform_device.h>

struct esp32s31_efuse {
	void __iomem *base;
};

static int esp32s31_efuse_read(void *context, unsigned int offset, void *val,
			       size_t bytes)
{
	struct esp32s31_efuse *efuse = context;
	u8 *buf = val;

	/* The registers take 32-bit reads only. */
	for (; bytes; bytes--, offset++)
		*buf++ = readl(efuse->base + (offset & ~3)) >> (8 * (offset & 3));

	return 0;
}

/*
 * The cell argument selects the address as ESP-IDF makes it from the factory
 * address, with two universal addresses for each chip: 0 Wi-Fi station,
 * 1 Wi-Fi access point, 2 Bluetooth, 3 Ethernet.  The odd ones are the
 * locally administered form of the even ones.
 */
static int esp32s31_efuse_mac_pp(void *context, const char *id, int index,
				 unsigned int offset, void *data, size_t bytes)
{
	u8 *mac = data;
	u8 universal;
	int i;

	if (bytes != ETH_ALEN || index < 0 || index > 3)
		return -EINVAL;

	for (i = 0; i < ETH_ALEN / 2; i++)
		swap(mac[i], mac[ETH_ALEN - 1 - i]);

	mac[5] += index / 2;
	if (index & 1) {
		universal = mac[0];
		mac[0] |= 0x02;
		if (mac[0] == universal)
			mac[0] ^= 0x04;
	}

	return 0;
}

static void esp32s31_efuse_fixup_cell_info(struct nvmem_device *nvmem,
					   struct nvmem_cell_info *cell)
{
	if (cell->name && !strcmp(cell->name, "mac-address"))
		cell->read_post_process = esp32s31_efuse_mac_pp;
}

static int esp32s31_efuse_probe(struct platform_device *pdev)
{
	struct nvmem_config config = {
		.name = "esp32s31-efuse",
		.id = NVMEM_DEVID_NONE,
		.owner = THIS_MODULE,
		.read_only = true,
		.root_only = true,
		.word_size = 1,
		.stride = 1,
		.reg_read = esp32s31_efuse_read,
		.fixup_dt_cell_info = esp32s31_efuse_fixup_cell_info,
	};
	struct device *dev = &pdev->dev;
	struct esp32s31_efuse *efuse;
	struct resource *res;

	efuse = devm_kzalloc(dev, sizeof(*efuse), GFP_KERNEL);
	if (!efuse)
		return -ENOMEM;

	efuse->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(efuse->base))
		return PTR_ERR(efuse->base);

	config.dev = dev;
	config.priv = efuse;
	config.size = resource_size(res);

	return PTR_ERR_OR_ZERO(devm_nvmem_register(dev, &config));
}

static const struct of_device_id esp32s31_efuse_of_match[] = {
	{ .compatible = "esp,esp32s31-efuse" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_efuse_of_match);

static struct platform_driver esp32s31_efuse_driver = {
	.driver = {
		.name = "esp32s31-efuse",
		.of_match_table = esp32s31_efuse_of_match,
	},
	.probe = esp32s31_efuse_probe,
};
module_platform_driver(esp32s31_efuse_driver);

MODULE_DESCRIPTION("ESP32-S31 eFuse driver");
MODULE_AUTHOR("Marco Mueller <hello@annoyedmilk.ch>");
MODULE_LICENSE("GPL");
