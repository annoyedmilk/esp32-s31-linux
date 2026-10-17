// SPDX-License-Identifier: Apache-2.0
/*
 * The persist flash partition for Linux, through the nvmem mailbox of the
 * shared block (esp32s31-hosted-shm.h).  A flash read, erase or write
 * suspends the external memory cache of the two harts, and Linux runs from
 * PSRAM through it.  Thus hart 1 is stalled during each flash operation.
 */

#include <string.h>
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "hal/cpu_utility_ll.h"
#include "soc/hp_system_reg.h"
#include "esp32s31-hosted-shm.h"
#include "shm_nvmem.h"

#define LINUX_HART	1
#define SECTOR_SIZE	4096U

_Static_assert(ESP_SHM_NV >= ESP_SHM_TO_FW_CTRL + 0x100,
	       "the mailbox must come after the ring control words");
_Static_assert(ESP_SHM_NV_DATA + ESP_SHM_NV_DATA_SIZE <= ESP_SHM_TO_FW_DATA,
	       "the nvmem data must not overlap the rings");
_Static_assert(SECTOR_SIZE % ESP_SHM_NV_DATA_SIZE == 0,
	       "a mailbox transfer must not cross a sector");

static const char *TAG = "FW_NVMEM";
static const esp_partition_t *s_part;

static volatile uint32_t *nv_word(uint32_t off)
{
	return (volatile uint32_t *)(ESP_SHM_ADDR + ESP_SHM_NV + off);
}

/* The stall takes effect only when the hart reports it. */
static void stall_linux(void)
{
	cpu_utility_ll_stall_cpu(LINUX_HART);
	while (!REG_GET_BIT(HP_SYSTEM_CPU_CORESTALLED_ST_REG,
			    HP_SYSTEM_REG_CORE1_CORESTALLED_ST))
		;
}

static esp_err_t nv_read(uint32_t addr, uint8_t *dst, uint32_t len)
{
	esp_err_t err;

	stall_linux();
	err = esp_partition_read(s_part, addr, dst, len);
	cpu_utility_ll_unstall_cpu(LINUX_HART);
	return err;
}

/* Change part of one sector.  A sector with the same data is not erased. */
static esp_err_t nv_write(uint32_t addr, const uint8_t *src, uint32_t len)
{
	uint32_t base = addr & ~(SECTOR_SIZE - 1);
	uint8_t *buf = heap_caps_malloc(SECTOR_SIZE, MALLOC_CAP_INTERNAL);
	esp_err_t err;

	if (!buf)
		return ESP_ERR_NO_MEM;

	stall_linux();
	err = esp_partition_read(s_part, base, buf, SECTOR_SIZE);
	if (err == ESP_OK && memcmp(buf + addr - base, src, len)) {
		memcpy(buf + addr - base, src, len);
		err = esp_partition_erase_range(s_part, base, SECTOR_SIZE);
		if (err == ESP_OK)
			err = esp_partition_write(s_part, base, buf, SECTOR_SIZE);
	}
	cpu_utility_ll_unstall_cpu(LINUX_HART);
	heap_caps_free(buf);
	return err;
}

void shm_nvmem_init(void)
{
	s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "persist");
	*nv_word(ESP_SHM_NV_SIZE_OFF) = s_part ? s_part->size : 0;
}

void shm_nvmem_service(void)
{
	uint32_t cmd = __atomic_load_n(nv_word(ESP_SHM_NV_CMD_OFF), __ATOMIC_ACQUIRE);
	uint32_t addr = *nv_word(ESP_SHM_NV_ADDR_OFF);
	uint32_t len = *nv_word(ESP_SHM_NV_LEN_OFF);
	uint8_t *data = (uint8_t *)(ESP_SHM_ADDR + ESP_SHM_NV_DATA);
	esp_err_t err;

	if (!cmd)
		return;

	if (!s_part || len > ESP_SHM_NV_DATA_SIZE ||
	    addr % SECTOR_SIZE + len > SECTOR_SIZE || addr + len > s_part->size)
		err = ESP_ERR_INVALID_ARG;
	else if (cmd == ESP_SHM_NV_READ)
		err = nv_read(addr, data, len);
	else if (cmd == ESP_SHM_NV_WRITE)
		err = nv_write(addr, data, len);
	else
		err = ESP_ERR_NOT_SUPPORTED;

	if (err != ESP_OK)
		ESP_LOGE(TAG, "command %u at 0x%x: %s", (unsigned)cmd, (unsigned)addr,
			 esp_err_to_name(err));
	*nv_word(ESP_SHM_NV_STATUS_OFF) = err;
	__atomic_store_n(nv_word(ESP_SHM_NV_CMD_OFF), 0, __ATOMIC_RELEASE);
}
