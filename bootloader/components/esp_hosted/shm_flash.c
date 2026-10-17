// SPDX-License-Identifier: Apache-2.0
/*
 * A window of the flash for Linux, through the flash mailbox of the shared
 * block (esp32s31-hosted-shm.h).  Linux makes an MTD device of it.  The
 * window is the "rootfs" and "data" partitions, which must be adjacent.
 *
 * A flash read, program or erase of ESP-IDF suspends the external memory
 * cache of the two harts, and Linux runs from PSRAM through it.  Thus hart 1
 * is stalled during each flash operation.  A block erase is 16 sector erases,
 * with one tick between them, so that Linux runs.  Linux does not read the
 * window through the cache, so no cache line of it can be stale after a
 * write.
 */

#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/cpu_utility_ll.h"
#include "soc/hp_system_reg.h"
#include "esp32s31-hosted-shm.h"
#include "shm_flash.h"

#define LINUX_HART		1
#define LINUX_DOORBELL_REG	HP_SYSTEM_CPU_INT_FROM_CPU_2_REG

_Static_assert(ESP_SHM_FLASH >= ESP_SHM_TO_FW_CTRL + 0x100,
	       "the mailbox must come after the ring control words");
_Static_assert(ESP_SHM_FLASH_DATA + ESP_SHM_FLASH_DATA_SIZE <= ESP_SHM_TO_FW_DATA,
	       "the mailbox data must not overlap the rings");
_Static_assert(ESP_SHM_FLASH_SECTOR % ESP_SHM_FLASH_DATA_SIZE == 0,
	       "a mailbox transfer must not cross a sector");

static const char *TAG = "FW_FLASH";
static uint32_t s_base;
static uint32_t s_size;

static volatile uint32_t *flash_word(uint32_t off)
{
	return (volatile uint32_t *)(ESP_SHM_ADDR + ESP_SHM_FLASH + off);
}

/*
 * The stall takes effect only when the hart reports it.  A hart in WFI
 * reports it only when it wakes up, so ring Linux.  Linux then sees one more
 * doorbell, which it can always take.
 */
static void stall_linux(void)
{
	cpu_utility_ll_stall_cpu(LINUX_HART);
	REG_WRITE(LINUX_DOORBELL_REG, 1);
	while (!REG_GET_BIT(HP_SYSTEM_CPU_CORESTALLED_ST_REG,
			    HP_SYSTEM_REG_CORE1_CORESTALLED_ST))
		;
}

static esp_err_t flash_op(uint32_t cmd, uint32_t addr, uint32_t len)
{
	uint8_t *data = (uint8_t *)(ESP_SHM_ADDR + ESP_SHM_FLASH_DATA);
	esp_err_t err;

	stall_linux();
	if (cmd == ESP_SHM_FLASH_READ)
		err = esp_flash_read(NULL, data, s_base + addr, len);
	else if (cmd == ESP_SHM_FLASH_PROGRAM)
		err = esp_flash_write(NULL, data, s_base + addr, len);
	else
		err = esp_flash_erase_region(NULL, s_base + addr, len);
	cpu_utility_ll_unstall_cpu(LINUX_HART);
	return err;
}

static esp_err_t flash_erase_block(uint32_t addr)
{
	uint32_t end = addr + ESP_SHM_FLASH_BLOCK;
	esp_err_t err = ESP_OK;

	for (; addr < end && err == ESP_OK; addr += ESP_SHM_FLASH_SECTOR) {
		err = flash_op(ESP_SHM_FLASH_ERASE, addr, ESP_SHM_FLASH_SECTOR);
		vTaskDelay(1);
	}
	return err;
}

static bool flash_request_valid(uint32_t cmd, uint32_t addr, uint32_t len)
{
	if (addr >= s_size || len > s_size - addr)
		return false;
	if (cmd == ESP_SHM_FLASH_ERASE)
		return addr % ESP_SHM_FLASH_BLOCK == 0 && len == ESP_SHM_FLASH_BLOCK;
	return (cmd == ESP_SHM_FLASH_READ || cmd == ESP_SHM_FLASH_PROGRAM) &&
	       len <= ESP_SHM_FLASH_DATA_SIZE &&
	       addr % ESP_SHM_FLASH_SECTOR + len <= ESP_SHM_FLASH_SECTOR;
}

void shm_flash_init(void)
{
	const esp_partition_t *root = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
							      0x40, "rootfs");
	const esp_partition_t *data = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
							      0x40, "data");

	s_base = 0;
	s_size = 0;
	if (root && data && data->address == root->address + root->size) {
		s_base = root->address;
		s_size = root->size + data->size;
	}
	*flash_word(ESP_SHM_FLASH_SIZE_OFF) = s_size;
}

/* Returns true when it completed a request, which Linux waits for. */
bool shm_flash_service(void)
{
	uint32_t cmd = __atomic_load_n(flash_word(ESP_SHM_FLASH_CMD_OFF), __ATOMIC_ACQUIRE);
	uint32_t addr = *flash_word(ESP_SHM_FLASH_ADDR_OFF);
	uint32_t len = *flash_word(ESP_SHM_FLASH_LEN_OFF);
	esp_err_t err;

	if (!cmd)
		return false;

	if (!flash_request_valid(cmd, addr, len))
		err = ESP_ERR_INVALID_ARG;
	else if (cmd == ESP_SHM_FLASH_ERASE)
		err = flash_erase_block(addr);
	else
		err = flash_op(cmd, addr, len);
	if (err != ESP_OK)
		ESP_LOGE(TAG, "command %u at 0x%x: %s", (unsigned)cmd, (unsigned)addr,
			 esp_err_to_name(err));
	*flash_word(ESP_SHM_FLASH_STATUS_OFF) = err;
	__atomic_store_n(flash_word(ESP_SHM_FLASH_CMD_OFF), 0, __ATOMIC_RELEASE);
	return true;
}
