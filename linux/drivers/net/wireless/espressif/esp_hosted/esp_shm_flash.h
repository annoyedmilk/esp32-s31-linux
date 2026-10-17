/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The flash window of the ESP32-S31 firmware as an MTD device.  Without MTD,
 * these do nothing.
 */
#ifndef _ESP_SHM_FLASH_H
#define _ESP_SHM_FLASH_H

#include <linux/device.h>
#include <linux/io.h>

struct esp_shm_flash;

#if IS_ENABLED(CONFIG_MTD)
struct esp_shm_flash *esp_shm_flash_probe(struct device *dev, void __iomem *shm,
					  void __iomem *doorbell);
void esp_shm_flash_wake(struct esp_shm_flash *flash);
void esp_shm_flash_remove(struct esp_shm_flash *flash);
#else
static inline struct esp_shm_flash *esp_shm_flash_probe(struct device *dev,
							void __iomem *shm,
							void __iomem *doorbell)
{
	return NULL;
}

static inline void esp_shm_flash_wake(struct esp_shm_flash *flash) {}
static inline void esp_shm_flash_remove(struct esp_shm_flash *flash) {}
#endif

#endif /* _ESP_SHM_FLASH_H */
