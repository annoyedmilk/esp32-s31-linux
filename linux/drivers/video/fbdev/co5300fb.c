// SPDX-License-Identifier: GPL-2.0-only
/* CO5300 QSPI framebuffer for the ESP-Mosaico CoreBoard V1.2 panel. */
#include <linux/bitmap.h>
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>

#define CO5300_WIDTH		480
#define CO5300_HEIGHT		480
#define CO5300_STRIDE		(CO5300_WIDTH * 2)
#define CO5300_SIZE		(CO5300_STRIDE * CO5300_HEIGHT)
#define CO5300_UPDATE_LINES	8
#define CO5300_DIRTY_LINES	2
#define CO5300_DIRTY_BLOCKS	(CO5300_HEIGHT / CO5300_DIRTY_LINES)
#define CO5300_UPDATE_SIZE	(CO5300_STRIDE * CO5300_UPDATE_LINES)
#define CO5300_WRITE_COMMAND	0x02
#define CO5300_WRITE_PIXELS	0x32
#define CO5300_COLUMN_ADDRESS	0x2a
#define CO5300_PAGE_ADDRESS	0x2b
#define CO5300_MEMORY_WRITE	0x2c
#define CO5300_SLEEP_OUT		0x11
#define CO5300_DISPLAY_ON	0x29
#define CO5300_DISPLAY_OFF	0x28

struct co5300fb {
	struct spi_device *spi;
	struct gpio_desc *reset;
	struct regulator *supply;
	/* Serialize panel commands and the staging buffer. */
	struct mutex lock;
	struct fb_deferred_io defio;
	/* Protect damage recorded by concurrent drawing callbacks. */
	spinlock_t damage_lock;
	DECLARE_BITMAP(dirty, CO5300_DIRTY_BLOCKS);
	u32 palette[16];
	u8 header[4];
	u8 data[CO5300_UPDATE_SIZE];
};

/* The opcode and register address stay on one wire, pixels use four. */
static int co5300fb_transfer(struct co5300fb *par, u8 cmd,
			     size_t len, bool pixels)
{
	struct spi_transfer xfers[2] = {
		{ .tx_buf = par->header, .len = sizeof(par->header) },
		{ .tx_buf = par->data, .len = len,
		  .tx_nbits = pixels ? SPI_NBITS_QUAD : SPI_NBITS_SINGLE },
	};

	par->header[0] = pixels ? CO5300_WRITE_PIXELS : CO5300_WRITE_COMMAND;
	par->header[1] = 0;
	par->header[2] = cmd;
	par->header[3] = 0;

	return spi_sync_transfer(par->spi, xfers, len ? 2 : 1);
}

static int co5300fb_command(struct co5300fb *par, u8 cmd, u8 value)
{
	par->data[0] = value;
	return co5300fb_transfer(par, cmd, 1, false);
}

static int co5300fb_window(struct co5300fb *par, u8 cmd, u16 first, u16 last)
{
	par->data[0] = first >> 8;
	par->data[1] = first;
	par->data[2] = last >> 8;
	par->data[3] = last;
	return co5300fb_transfer(par, cmd, 4, false);
}

static int co5300fb_init_panel(struct co5300fb *par)
{
	/* Panel settings from the ESP-Mosaico BSP's 480x480 CO5300 sequence. */
	static const struct {
		u8 cmd;
		u8 value;
	} settings[] = {
		{ 0xfe, 0x20 }, { 0x19, 0x10 }, { 0x1c, 0xa0 },
		{ 0xfe, 0x00 }, { 0xc4, 0x80 }, { 0x3a, 0x55 },
		{ 0x36, 0x00 }, { 0x53, 0x20 }, { 0x51, 0xff },
		{ 0x63, 0xff },
	};
	unsigned int i;
	int ret;

	gpiod_set_value_cansleep(par->reset, 1);
	usleep_range(10000, 12000);
	gpiod_set_value_cansleep(par->reset, 0);
	msleep(150);

	ret = co5300fb_transfer(par, CO5300_SLEEP_OUT, 0, false);
	if (ret)
		return ret;
	msleep(600);
	for (i = 0; i < ARRAY_SIZE(settings); i++) {
		ret = co5300fb_command(par, settings[i].cmd, settings[i].value);
		if (ret)
			return ret;
	}
	return 0;
}

/* The panel requires even page boundaries, including for a one-line change. */
static void co5300fb_mark_dirty(struct fb_info *info, unsigned int first,
				unsigned int last)
{
	struct co5300fb *par = info->par;
	unsigned long flags;

	first /= CO5300_DIRTY_LINES;
	last /= CO5300_DIRTY_LINES;
	spin_lock_irqsave(&par->damage_lock, flags);
	bitmap_set(par->dirty, first, last - first + 1);
	spin_unlock_irqrestore(&par->damage_lock, flags);
}

static void co5300fb_mark_range(struct fb_info *info, unsigned long off,
				size_t len)
{
	if (!len || off >= CO5300_SIZE)
		return;
	len = min_t(size_t, len, CO5300_SIZE - off);
	co5300fb_mark_dirty(info, off / CO5300_STRIDE,
			    (off + len - 1) / CO5300_STRIDE);
}

static int co5300fb_update(struct fb_info *info)
{
	struct co5300fb *par = info->par;
	const u16 *fb = (const u16 *)info->screen_buffer;
	DECLARE_BITMAP(dirty, CO5300_DIRTY_BLOCKS);
	unsigned int block, end, y, pixels, x;
	unsigned long flags;
	int ret = 0;

	mutex_lock(&par->lock);
	/* New damage during this transfer belongs to the next update. */
	spin_lock_irqsave(&par->damage_lock, flags);
	bitmap_copy(dirty, par->dirty, CO5300_DIRTY_BLOCKS);
	bitmap_zero(par->dirty, CO5300_DIRTY_BLOCKS);
	spin_unlock_irqrestore(&par->damage_lock, flags);
	if (bitmap_empty(dirty, CO5300_DIRTY_BLOCKS))
		goto out;

	ret = co5300fb_window(par, CO5300_COLUMN_ADDRESS, 0, CO5300_WIDTH - 1);
	if (ret)
		goto restore;
	for (block = find_first_bit(dirty, CO5300_DIRTY_BLOCKS);
	     block < CO5300_DIRTY_BLOCKS;
	     block = find_next_bit(dirty, CO5300_DIRTY_BLOCKS, end)) {
		end = find_next_zero_bit(dirty, CO5300_DIRTY_BLOCKS, block);
		end = min(end, block + CO5300_UPDATE_LINES / CO5300_DIRTY_LINES);
		y = block * CO5300_DIRTY_LINES;
		pixels = (end - block) * CO5300_DIRTY_LINES * CO5300_WIDTH;
		ret = co5300fb_window(par, CO5300_PAGE_ADDRESS, y,
				      end * CO5300_DIRTY_LINES - 1);
		if (ret)
			goto restore;
		/* fbdev exposes native-endian RGB565; the panel takes MSB first. */
		for (x = 0; x < pixels; x++) {
			u16 pixel = fb[y * CO5300_WIDTH + x];

			par->data[x * 2] = pixel >> 8;
			par->data[x * 2 + 1] = pixel;
		}
		ret = co5300fb_transfer(par, CO5300_MEMORY_WRITE, pixels * 2, true);
		if (ret)
			goto restore;
	}
	goto out;
restore:
	/* Preserve failed updates without losing concurrent damage. */
	spin_lock_irqsave(&par->damage_lock, flags);
	bitmap_or(par->dirty, par->dirty, dirty, CO5300_DIRTY_BLOCKS);
	spin_unlock_irqrestore(&par->damage_lock, flags);
out:
	mutex_unlock(&par->lock);
	return ret;
}

static void co5300fb_deferred_io(struct fb_info *info, struct list_head *pages)
{
	struct co5300fb *par = info->par;
	struct fb_deferred_io_pageref *pageref;
	int ret;

	list_for_each_entry(pageref, pages, list)
		co5300fb_mark_range(info, pageref->offset, PAGE_SIZE);
	ret = co5300fb_update(info);
	if (ret)
		dev_err_ratelimited(&par->spi->dev, "frame update failed: %d\n", ret);
}

static void co5300fb_damage_range(struct fb_info *info, off_t off, size_t len)
{
	co5300fb_mark_range(info, off, len);
	schedule_delayed_work(&info->deferred_work, info->fbdefio->delay);
}

static void co5300fb_damage_area(struct fb_info *info, u32 x, u32 y,
				 u32 width, u32 height)
{
	if (!width || !height || y >= CO5300_HEIGHT)
		return;
	co5300fb_mark_dirty(info, y,
			    y + min_t(u32, height, CO5300_HEIGHT - y) - 1);
	schedule_delayed_work(&info->deferred_work, info->fbdefio->delay);
}

FB_GEN_DEFAULT_DEFERRED_SYSMEM_OPS(co5300fb, co5300fb_damage_range,
				   co5300fb_damage_area)

static int co5300fb_setcolreg(unsigned int regno, unsigned int red,
			      unsigned int green, unsigned int blue,
			      unsigned int transp, struct fb_info *info)
{
	struct co5300fb *par = info->par;

	if (regno >= ARRAY_SIZE(par->palette))
		return -EINVAL;
	par->palette[regno] = ((red >> 11) << 11) |
			     ((green >> 10) << 5) | (blue >> 11);
	return 0;
}

static void co5300fb_destroy(struct fb_info *info)
{
	vfree(info->screen_buffer);
	framebuffer_release(info);
}

static const struct fb_ops co5300fb_ops = {
	.owner = THIS_MODULE,
	FB_DEFAULT_DEFERRED_OPS(co5300fb),
	.fb_setcolreg = co5300fb_setcolreg,
	.fb_destroy = co5300fb_destroy,
};

static void co5300fb_disable_supply(void *data)
{
	regulator_disable(data);
}

static int co5300fb_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct co5300fb *par;
	struct fb_info *info;
	int ret;

	info = framebuffer_alloc(sizeof(*par), dev);
	if (!info)
		return -ENOMEM;
	par = info->par;
	par->spi = spi;
	mutex_init(&par->lock);
	spin_lock_init(&par->damage_lock);
	bitmap_fill(par->dirty, CO5300_DIRTY_BLOCKS);
	par->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(par->reset)) {
		ret = dev_err_probe(dev, PTR_ERR(par->reset), "reset GPIO\n");
		goto release;
	}
	par->supply = devm_regulator_get(dev, "vdd");
	if (IS_ERR(par->supply)) {
		ret = dev_err_probe(dev, PTR_ERR(par->supply), "panel supply\n");
		goto release;
	}
	ret = regulator_enable(par->supply);
	if (ret)
		goto release;
	ret = devm_add_action_or_reset(dev, co5300fb_disable_supply, par->supply);
	if (ret)
		goto release;

	info->screen_buffer = vzalloc(PAGE_ALIGN(CO5300_SIZE));
	if (!info->screen_buffer) {
		ret = -ENOMEM;
		goto release;
	}
	info->fix = (struct fb_fix_screeninfo) {
		.id = "CO5300", .type = FB_TYPE_PACKED_PIXELS,
		.visual = FB_VISUAL_TRUECOLOR, .line_length = CO5300_STRIDE,
		.smem_len = CO5300_SIZE,
	};
	info->var = (struct fb_var_screeninfo) {
		.xres = CO5300_WIDTH, .yres = CO5300_HEIGHT,
		.xres_virtual = CO5300_WIDTH, .yres_virtual = CO5300_HEIGHT,
		.bits_per_pixel = 16, .red = { 11, 5, 0 },
		.green = { 5, 6, 0 }, .blue = { 0, 5, 0 },
		.width = -1, .height = -1,
	};
	info->flags = FBINFO_VIRTFB;
	info->fbops = &co5300fb_ops;
	info->pseudo_palette = par->palette;
	par->defio.delay = max_t(unsigned long, 1, HZ / 50);
	par->defio.deferred_io = co5300fb_deferred_io;
	info->fbdefio = &par->defio;
	ret = fb_deferred_io_init(info);
	if (ret)
		goto free_buffer;
	ret = co5300fb_init_panel(par);
	if (ret)
		goto cleanup;
	/* Clear display RAM before making the panel visible. */
	ret = co5300fb_update(info);
	if (ret)
		goto cleanup;
	ret = co5300fb_transfer(par, CO5300_DISPLAY_ON, 0, false);
	if (ret)
		goto cleanup;
	msleep(600);
	spi_set_drvdata(spi, info);
	ret = register_framebuffer(info);
	if (ret)
		goto cleanup;
	dev_info(dev, "fb%d: 480x480 RGB565 QSPI framebuffer\n", info->node);
	return 0;

cleanup:
	gpiod_set_value_cansleep(par->reset, 1);
	fb_deferred_io_cleanup(info);
free_buffer:
	vfree(info->screen_buffer);
release:
	framebuffer_release(info);
	return ret;
}

static void co5300fb_remove(struct spi_device *spi)
{
	struct fb_info *info = spi_get_drvdata(spi);
	struct co5300fb *par = info->par;

	console_lock();
	fb_set_suspend(info, 1);
	console_unlock();
	fb_deferred_io_cleanup(info);
	co5300fb_transfer(par, CO5300_DISPLAY_OFF, 0, false);
	gpiod_set_value_cansleep(par->reset, 1);
	unregister_framebuffer(info);
}

static const struct of_device_id co5300fb_of_match[] = {
	{ .compatible = "esp-mosaico,co5300-480x480" },
	{}
};
MODULE_DEVICE_TABLE(of, co5300fb_of_match);

static const struct spi_device_id co5300fb_ids[] = {
	{ "co5300-480x480" },
	{}
};
MODULE_DEVICE_TABLE(spi, co5300fb_ids);

static struct spi_driver co5300fb_driver = {
	.driver = {
		.name = "co5300fb",
		.of_match_table = co5300fb_of_match,
	},
	.probe = co5300fb_probe,
	.remove = co5300fb_remove,
	.id_table = co5300fb_ids,
};
module_spi_driver(co5300fb_driver);

MODULE_DESCRIPTION("CO5300 480x480 QSPI framebuffer");
MODULE_LICENSE("GPL");
