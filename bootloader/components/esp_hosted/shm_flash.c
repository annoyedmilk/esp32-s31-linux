// SPDX-License-Identifier: Apache-2.0
/*
 * A window of the flash for Linux, through the flash mailbox of the shared
 * block (esp32s31-hosted-shm.h).  Linux makes an MTD device of it.  The
 * window is the "rootfs" and "data" partitions, which must be adjacent.
 *
 * Linux reads the window through the flash cache, where this file maps it.
 * It programs and erases through the mailbox.  With flash auto-suspend, the
 * cache stays on during a program or erase, and Linux runs on from PSRAM.
 * Each cache miss to the flash suspends the operation.  After the operation,
 * ESP-IDF invalidates the cache lines of the mapped window, and the two harts
 * share the data cache.
 */

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "spi_flash_mmap.h"
#include "soc/hp_system_reg.h"
#include "esp32s31-hosted-shm.h"
#include "shm_flash.h"

/* Without auto-suspend, a program or erase freezes the cache under Linux. */
#if !CONFIG_SPI_FLASH_AUTO_SUSPEND
#error "the flash window needs CONFIG_SPI_FLASH_AUTO_SUSPEND"
#endif

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

static esp_err_t flash_op(uint32_t cmd, uint32_t addr, uint32_t len)
{
	uint8_t *data = (uint8_t *)(ESP_SHM_ADDR + ESP_SHM_FLASH_DATA);

	if (cmd == ESP_SHM_FLASH_PROGRAM)
		return esp_flash_write(NULL, data, s_base + addr, len);
	return esp_flash_erase_region(NULL, s_base + addr, len);
}

/*
 * A cache miss of hart 1 can wait behind the suspends of a program or erase
 * longer than the core bus timeout (65535 cycles at most).  The bus then
 * answers with an error, and Linux gets an access fault on a correct PSRAM
 * address.  Turn the timeout off for the two harts: the access then gets its
 * data later.
 */
static void disable_bus_timeouts(void)
{
	REG_CLR_BIT(HP_SYSTEM_CORE_IBUS_TIMEOUT_CONF_REG, HP_SYSTEM_CORE_IBUS_TIMEOUT_PROTECT_EN);
	REG_CLR_BIT(HP_SYSTEM_CORE_DBUS_TIMEOUT_CONF_REG, HP_SYSTEM_CORE_DBUS_TIMEOUT_PROTECT_EN);
}

static bool flash_request_valid(uint32_t cmd, uint32_t addr, uint32_t len)
{
	if (addr >= s_size || len > s_size - addr)
		return false;
	if (cmd == ESP_SHM_FLASH_ERASE)
		return addr % ESP_SHM_FLASH_BLOCK == 0 && len == ESP_SHM_FLASH_BLOCK;
	return cmd == ESP_SHM_FLASH_PROGRAM && len <= ESP_SHM_FLASH_DATA_SIZE &&
	       addr % ESP_SHM_FLASH_SECTOR + len <= ESP_SHM_FLASH_SECTOR;
}

void shm_flash_init(void)
{
	const esp_partition_t *root = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
							      0x40, "rootfs");
	const esp_partition_t *data = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
							      0x40, "data");

	const void *map = NULL;
	spi_flash_mmap_handle_t handle;

	disable_bus_timeouts();
	s_base = 0;
	s_size = 0;
	if (root && data && data->address == root->address + root->size &&
	    spi_flash_mmap(root->address, root->size + data->size,
			   SPI_FLASH_MMAP_DATA, &map, &handle) == ESP_OK) {
		s_base = root->address;
		s_size = root->size + data->size;
	}
	*flash_word(ESP_SHM_FLASH_MAP_OFF) = (uint32_t)map;
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
	else
		err = flash_op(cmd, addr, len);
	if (err != ESP_OK)
		ESP_LOGE(TAG, "command %u at 0x%x: %s", (unsigned)cmd, (unsigned)addr,
			 esp_err_to_name(err));
	*flash_word(ESP_SHM_FLASH_STATUS_OFF) = err;
	__atomic_store_n(flash_word(ESP_SHM_FLASH_CMD_OFF), 0, __ATOMIC_RELEASE);
	return true;
}
