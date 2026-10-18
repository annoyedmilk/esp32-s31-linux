// SPDX-License-Identifier: GPL-2.0-only
/*
 * The flash window of the ESP32-S31 firmware, as an MTD device with the
 * partitions of the "flash" child node.  Linux cannot use the SPI flash
 * itself: hart 0 runs its code from it.  The firmware maps the window in the
 * flash cache, and Linux reads it there.  The firmware programs and erases
 * the window for Linux, through the flash mailbox of the shared block
 * (esp32s31-hosted-shm.h).  Linux runs on while it does: the firmware uses
 * flash auto-suspend.  An erase block is 64 KiB, for UBI.
 */

#include <linux/device.h>
#include <linux/io.h>
#include <linux/minmax.h>
#include <linux/mtd/mtd.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/wait.h>

#include "esp32s31-hosted-shm.h"
#include "esp_shm_flash.h"

/* A block erase takes longer when cache misses suspend it often. */
#define ESP_SHM_FLASH_TIMEOUT_MS	10000

struct esp_shm_flash {
	struct mtd_info mtd;
	void *window;			/* the window in the flash cache */
	void __iomem *mbox;		/* ESP_SHM_FLASH in the shared block */
	void __iomem *data;		/* ESP_SHM_FLASH_DATA */
	void __iomem *doorbell;
	struct mutex lock;		/* the mailbox */
	wait_queue_head_t wq;		/* the firmware completed a command */
};

static u32 mbox_read(struct esp_shm_flash *flash, u32 off)
{
	return ioread32(flash->mbox + off);
}

/* One command of the mailbox.  Called with the lock held. */
static int esp_shm_flash_cmd(struct esp_shm_flash *flash, u32 cmd, u32 addr,
			     const void *buf, u32 len)
{
	/* After a timeout, the firmware can still work on the old command. */
	if (mbox_read(flash, ESP_SHM_FLASH_CMD_OFF))
		return -EBUSY;
	iowrite32(addr, flash->mbox + ESP_SHM_FLASH_ADDR_OFF);
	iowrite32(len, flash->mbox + ESP_SHM_FLASH_LEN_OFF);
	if (cmd == ESP_SHM_FLASH_PROGRAM)
		memcpy_toio(flash->data, buf, len);
	/* The firmware reads the request when it sees the command. */
	wmb();
	iowrite32(cmd, flash->mbox + ESP_SHM_FLASH_CMD_OFF);
	iowrite32(1, flash->doorbell);

	if (!wait_event_timeout(flash->wq, !mbox_read(flash, ESP_SHM_FLASH_CMD_OFF),
				msecs_to_jiffies(ESP_SHM_FLASH_TIMEOUT_MS)))
		return -ETIMEDOUT;
	/* Read the status and the data only after the cleared command. */
	rmb();
	return mbox_read(flash, ESP_SHM_FLASH_STATUS_OFF) ? -EIO : 0;
}

/* The lock keeps a read from seeing half of a program or erase. */
static int esp_shm_flash_read(struct mtd_info *mtd, loff_t from, size_t len,
			      size_t *retlen, u_char *buf)
{
	struct esp_shm_flash *flash = container_of(mtd, struct esp_shm_flash, mtd);

	mutex_lock(&flash->lock);
	memcpy(buf, flash->window + from, len);
	mutex_unlock(&flash->lock);
	*retlen = len;
	return 0;
}

/* In mailbox blocks that do not cross a sector. */
static int esp_shm_flash_write(struct mtd_info *mtd, loff_t to, size_t len,
			       size_t *retlen, const u_char *buf)
{
	struct esp_shm_flash *flash = container_of(mtd, struct esp_shm_flash, mtd);
	int ret = 0;

	mutex_lock(&flash->lock);
	while (len) {
		u32 n = min_t(size_t, len,
			      ESP_SHM_FLASH_DATA_SIZE - to % ESP_SHM_FLASH_DATA_SIZE);

		ret = esp_shm_flash_cmd(flash, ESP_SHM_FLASH_PROGRAM, to, buf, n);
		if (ret)
			break;
		*retlen += n;
		to += n;
		buf += n;
		len -= n;
	}
	mutex_unlock(&flash->lock);
	return ret;
}

static int esp_shm_flash_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	struct esp_shm_flash *flash = container_of(mtd, struct esp_shm_flash, mtd);
	u64 addr;
	int ret = 0;

	mutex_lock(&flash->lock);
	for (addr = instr->addr; addr < instr->addr + instr->len;
	     addr += ESP_SHM_FLASH_BLOCK) {
		ret = esp_shm_flash_cmd(flash, ESP_SHM_FLASH_ERASE, addr, NULL,
					ESP_SHM_FLASH_BLOCK);
		if (ret) {
			instr->fail_addr = addr;
			break;
		}
	}
	mutex_unlock(&flash->lock);
	return ret;
}

struct esp_shm_flash *esp_shm_flash_probe(struct device *dev, void __iomem *shm,
					  void __iomem *doorbell)
{
	u32 size = ioread32(shm + ESP_SHM_FLASH + ESP_SHM_FLASH_SIZE_OFF);
	u32 map = ioread32(shm + ESP_SHM_FLASH + ESP_SHM_FLASH_MAP_OFF);
	struct device_node *np;
	struct esp_shm_flash *flash;
	struct mtd_info *mtd;
	int ret;

	np = of_get_child_by_name(dev->of_node, "flash");
	if (!size || !np)
		goto none;
	flash = kzalloc_obj(*flash);
	if (!flash)
		goto none;
	flash->window = memremap(map, size, MEMREMAP_WB);
	if (!flash->window)
		goto free;

	flash->mbox = shm + ESP_SHM_FLASH;
	flash->data = shm + ESP_SHM_FLASH_DATA;
	flash->doorbell = doorbell;
	mutex_init(&flash->lock);
	init_waitqueue_head(&flash->wq);

	mtd = &flash->mtd;
	mtd->name = "esp32s31-flash";
	mtd->type = MTD_NORFLASH;
	mtd->flags = MTD_CAP_NORFLASH;
	mtd->size = size;
	mtd->erasesize = ESP_SHM_FLASH_BLOCK;
	mtd->writesize = 1;
	mtd->writebufsize = ESP_SHM_FLASH_DATA_SIZE;
	mtd->owner = THIS_MODULE;
	mtd->dev.parent = dev;
	mtd->_read = esp_shm_flash_read;
	mtd->_write = esp_shm_flash_write;
	mtd->_erase = esp_shm_flash_erase;
	mtd_set_of_node(mtd, np);

	ret = mtd_device_register(mtd, NULL, 0);
	if (!ret)
		return flash;
	dev_warn(dev, "no flash device: %d\n", ret);
	memunmap(flash->window);
free:
	kfree(flash);
none:
	of_node_put(np);
	return NULL;
}

void esp_shm_flash_wake(struct esp_shm_flash *flash)
{
	if (flash)
		wake_up(&flash->wq);
}

void esp_shm_flash_remove(struct esp_shm_flash *flash)
{
	if (!flash)
		return;
	mtd_device_unregister(&flash->mtd);
	of_node_put(mtd_get_of_node(&flash->mtd));
	memunmap(flash->window);
	kfree(flash);
}
