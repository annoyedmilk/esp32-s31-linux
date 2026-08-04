// SPDX-License-Identifier: BSD-2-Clause
// Author: Marco Müller <hello@annoyedmilk.ch>

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_rom_crc.h"
#include "esp_rom_sys.h"
#include "esp_clk_tree.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_private/wifi.h"
#include "soc/hp_system_reg.h"
#include "soc/interrupts.h"
#include "esp32s31-wifi-ipc.h"
#include "driver/gpio.h"
#include "esp32s31/rom/ets_sys.h"
#include "hal/cpu_utility_ll.h"
#include "esp32s31/rom/cache.h"
#include "esp_private/esp_clk_tree_common.h"
#include "esp_private/gpio.h"
#include "esp_private/periph_ctrl.h"
#include "hal/cache_ll.h"
#include "hal/assist_debug_ll.h"
#include "heap_memory_layout.h"
#include "hal/sdmmc_ll.h"
#include "hal/wdt_hal.h"
#include "soc/soc.h"
#include "soc/soc_caps.h"
#include "soc/clk_tree_defs.h"
#include "soc/cpu_apm_reg.h"
#include "soc/hp_apm_reg.h"
#include "soc/hp_mem_apm_reg.h"
#include <soc/reg_base.h>
#include "soc/gpio_pins.h"
#include "soc/gpio_sig_map.h"
#include "soc/sdmmc_pins.h"
#include "soc/spi_mem_c_reg.h"
#include "display.h"

#define PSRAM_BASE                  0x50000000U
#define OPENSBI_LOAD_ADDR           0x50E00000U
#define OPENSBI_LOAD_SIZE           0x00040000U
#define KERNEL_LOAD_ADDR            0x50000000U
#define KERNEL_IMAGE_SIZE_OFFSET    0x10U
#define KERNEL_MAGIC2_OFFSET        0x38U
#define KERNEL_MAGIC2               0x05435352U
#define KERNEL_SIZE_MAGIC           0x455A4953U
#define INITRAMFS_LOAD_ADDR         0x50800000U
#define INITRAMFS_PARTITION_SIZE    0x00200000U
#define INITRAMFS_NEWC_MAGIC0       0x37303730U
#define LINUX_HART                  1U

/*
 * Internal SRAM handed to Linux.  Both regions are kept out of the ESP-IDF
 * heap because this firmware stays resident: the pool backs the kernel's
 * coherent DMA allocations and the window carries the Wi-Fi rings, and SRAM
 * is reached without the data cache, which is what makes either usable from
 * both harts at once.
 */
#define LINUX_DMA_POOL_ADDR         0x2F040000U
#define LINUX_DMA_POOL_SIZE         0x00010000U
#define WIFI_IPC_ADDR               0x2F050000U
#define WIFI_IPC_SIZE               0x00010000U

SOC_RESERVE_MEMORY_REGION(LINUX_DMA_POOL_ADDR,
                          LINUX_DMA_POOL_ADDR + LINUX_DMA_POOL_SIZE,
                          linux_dma_pool);
SOC_RESERVE_MEMORY_REGION(WIFI_IPC_ADDR, WIFI_IPC_ADDR + WIFI_IPC_SIZE,
                          wifi_ipc);

_Static_assert(sizeof(struct esp32s31_ipc) <= WIFI_IPC_SIZE,
               "Wi-Fi IPC block exceeds its reserved SRAM window");

/*
 * APM region attribute word: one nibble per REE mode (R0 through R3), each
 * holding X at bit 0, W at bit 1 and R at bit 2.  0x7777 grants read, write
 * and execute to every mode, which is what bring-up needs before any real
 * privilege separation exists.
 */
#define APM_REGION_ATTR_ALL_RWX     0x7777U

/*
 * MSPI PMS section attribute: secure and non-secure read/write granted with
 * ECC left disabled.  The flash and PSRAM controllers share this bit layout,
 * so the SPI_FMEM_C names describe all four register banks written below.
 */
#define MSPI_PMS_ATTR_RW \
    (SPI_FMEM_C_PMS0_RD_ATTR | SPI_FMEM_C_PMS0_WR_ATTR | \
     SPI_FMEM_C_PMS0_NONSECURE_RD_ATTR | SPI_FMEM_C_PMS0_NONSECURE_WR_ATTR)

#define SD_POWER_EN_GPIO            GPIO_NUM_39
#define SD_TARGET_CCLK_HZ           50000000U

static const char *TAG = "s31-linux-loader";

struct kernel_size_manifest {
    uint32_t magic;
    uint32_t image_size;
    uint32_t image_crc32;
};

static void fence_i(void)
{
    __asm__ volatile ("fence.i" ::: "memory");
}

static void flush_l1_cache_before_handoff(void)
{
    cache_ll_writeback_all(CACHE_LL_LEVEL_ALL, CACHE_TYPE_DATA, CACHE_LL_ID_ALL);
    cache_ll_invalidate_all(CACHE_LL_LEVEL_ALL, CACHE_TYPE_ALL, CACHE_LL_ID_ALL);
    fence_i();
}

static void log_cache_mode(void)
{
    struct cache_mode icache = { .cache_type = CACHE_L1_ICACHE0 };
    struct cache_mode dcache = { .cache_type = CACHE_L1_DCACHE };

    Cache_Get_Mode(&icache);
    Cache_Get_Mode(&dcache);
    ESP_LOGI(TAG, "cache: I=%" PRIu32 "K/%u-way/%uB D=%" PRIu32
                  "K/%u-way/%uB PSRAM-exec=%s",
             icache.cache_size / 1024, icache.cache_ways,
             icache.cache_line_size, dcache.cache_size / 1024,
             dcache.cache_ways, dcache.cache_line_size,
             Cache_Address_Through_Cache(KERNEL_LOAD_ADDR) ? "cached" : "bypass");
}

/*
 * Grant unrestricted access through every Access Permission Manager on the
 * chip.  Each APM is programmed with a single region spanning the whole
 * 32-bit address space, marked RWX for all REE modes, and its filter is then
 * enabled so the (always-passing) check is actually applied.  FUNC_CTRL
 * enables the per-master filters; every bit is set so no bus master is left
 * unfiltered and therefore inconsistently permissive.
 */
static void open_apm(void)
{
    REG_WRITE(CPU_APM_REGION0_ADDR_START_REG, 0);
    REG_WRITE(CPU_APM_REGION0_ADDR_END_REG, 0xFFFFFFFF);
    REG_WRITE(CPU_APM_REGION0_ATTR_REG, APM_REGION_ATTR_ALL_RWX);
    REG_WRITE(CPU_APM_REGION_FILTER_EN_REG, 1);
    REG_WRITE(CPU_APM_FUNC_CTRL_REG, 0xFFFFFFFF);

    REG_WRITE(HP_APM_REGION0_ADDR_START_REG, 0);
    REG_WRITE(HP_APM_REGION0_ADDR_END_REG, 0xFFFFFFFF);
    REG_WRITE(HP_APM_REGION0_ATTR_REG, APM_REGION_ATTR_ALL_RWX);
    REG_WRITE(HP_APM_REGION_FILTER_EN_REG, 1);
    REG_WRITE(HP_APM_FUNC_CTRL_REG, 0xFFFFFFFF);

    REG_WRITE(HP_MEM_APM_REGION0_ADDR_START_REG, 0);
    REG_WRITE(HP_MEM_APM_REGION0_ADDR_END_REG, 0xFFFFFFFF);
    REG_WRITE(HP_MEM_APM_REGION0_ATTR_REG, APM_REGION_ATTR_ALL_RWX);
    REG_WRITE(HP_MEM_APM_REGION_FILTER_EN_REG, 1);
    REG_WRITE(HP_MEM_APM_FUNC_CTRL_REG, 0xFFFFFFFF);

    /*
     * Open all four flash/external-memory PMS sections on both MSPI
     * controllers.  The PSRAM controller mirrors the SPI_MEM_C register
     * layout at DR_REG_PSRAM_MSPI0_BASE, so the same four section
     * attributes are written again at that fixed offset.
     */
    for (int i = 0; i < 4; i++) {
        const uint32_t psram_off = DR_REG_PSRAM_MSPI0_BASE - DR_REG_FLASH_SPI0_BASE;

        REG_WRITE(SPI_FMEM_C_PMS0_ATTR_REG + i * 4, MSPI_PMS_ATTR_RW);
        REG_WRITE(SPI_SMEM_C_PMS0_ATTR_REG + i * 4, MSPI_PMS_ATTR_RW);
        REG_WRITE(SPI_FMEM_C_PMS0_ATTR_REG + psram_off + i * 4, MSPI_PMS_ATTR_RW);
        REG_WRITE(SPI_SMEM_C_PMS0_ATTR_REG + psram_off + i * 4, MSPI_PMS_ATTR_RW);
    }

    /* Discard any permission faults latched while the filters were opening. */
    REG_WRITE(HP_APM_M0_STATUS_CLR_REG, 1);
    REG_WRITE(HP_APM_M1_STATUS_CLR_REG, 1);
    REG_WRITE(HP_APM_M2_STATUS_CLR_REG, 1);
    REG_WRITE(HP_APM_M3_STATUS_CLR_REG, 1);
    REG_WRITE(HP_APM_M4_STATUS_CLR_REG, 1);
    REG_WRITE(HP_APM_M5_STATUS_CLR_REG, 1);
    REG_WRITE(HP_APM_M6_STATUS_CLR_REG, 1);

    REG_WRITE(HP_MEM_APM_M0_STATUS_CLR_REG, 1);
    REG_WRITE(HP_MEM_APM_M1_STATUS_CLR_REG, 1);
    REG_WRITE(HP_MEM_APM_M2_STATUS_CLR_REG, 1);
    REG_WRITE(HP_MEM_APM_M3_STATUS_CLR_REG, 1);
    REG_WRITE(HP_MEM_APM_M4_STATUS_CLR_REG, 1);
    REG_WRITE(HP_MEM_APM_M5_STATUS_CLR_REG, 1);

    REG_WRITE(CPU_APM_M0_STATUS_CLR_REG, 1);
    REG_WRITE(CPU_APM_M1_STATUS_CLR_REG, 1);
    REG_WRITE(CPU_APM_M2_STATUS_CLR_REG, 1);
    REG_WRITE(CPU_APM_M3_STATUS_CLR_REG, 1);
}

static struct esp32s31_ipc *const ipc = (struct esp32s31_ipc *)WIFI_IPC_ADDR;

#define IPC_DOORBELL_TO_LINUX_REG   HP_SYSTEM_CPU_INT_FROM_CPU_0_REG
#define IPC_DOORBELL_TO_FIRMWARE    ETS_CPU_INTR_FROM_CPU_1_SOURCE
#define IPC_DOORBELL_TO_FIRMWARE_REG HP_SYSTEM_CPU_INT_FROM_CPU_1_REG

static bool ipc_ring_pop(struct esp32s31_ipc_ring *ring, void *buf, uint32_t *len)
{
    uint32_t tail = ring->tail;
    struct esp32s31_ipc_slot *slot;

    if (tail == ring->head) {
        return false;
    }

    slot = &ring->slot[tail % ESP32S31_IPC_SLOTS];
    *len = slot->len;
    if (*len > ESP32S31_IPC_SLOT_DATA) {
        *len = ESP32S31_IPC_SLOT_DATA;
    }
    memcpy(buf, slot->data, *len);

    __atomic_store_n(&ring->tail, tail + 1, __ATOMIC_RELEASE);
    return true;
}

static bool ipc_ring_push(struct esp32s31_ipc_ring *ring, const void *buf,
                          uint32_t len)
{
    uint32_t head = ring->head;
    struct esp32s31_ipc_slot *slot;

    if (len > ESP32S31_IPC_SLOT_DATA ||
        head - __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE) >=
        ESP32S31_IPC_SLOTS) {
        return false;
    }

    slot = &ring->slot[head % ESP32S31_IPC_SLOTS];
    memcpy(slot->data, buf, len);
    slot->len = len;

    __atomic_store_n(&ring->head, head + 1, __ATOMIC_RELEASE);
    return true;
}

/* Frames from the air: hand them to Linux and release the Wi-Fi buffer. */
static esp_err_t ipc_wifi_rx(void *buffer, uint16_t len, void *eb)
{
    bool queued = ipc_ring_push(&ipc->to_linux, buffer, len);

    if (eb) {
        esp_wifi_internal_free_rx_buffer(eb);
    }
    if (queued) {
        REG_WRITE(IPC_DOORBELL_TO_LINUX_REG, 1);
    }
    return ESP_OK;
}

static TaskHandle_t ipc_tx_task_handle;

/* esp_wifi_internal_tx() may block, so the doorbell only wakes the task. */
static void ipc_from_linux_isr(void *arg)
{
    BaseType_t higher_priority_woken = pdFALSE;

    REG_WRITE(IPC_DOORBELL_TO_FIRMWARE_REG, 0);
    vTaskNotifyGiveFromISR(ipc_tx_task_handle, &higher_priority_woken);
    portYIELD_FROM_ISR(higher_priority_woken);
}

/* Retry association only while Linux still wants a connection. */
static bool ipc_want_connection;

static void ipc_run_command(void)
{
    wifi_config_t cfg = { 0 };
    esp_err_t err;
    uint32_t code;

    code = __atomic_exchange_n(&ipc->cmd.code, ESP32S31_IPC_CMD_NONE,
                               __ATOMIC_ACQUIRE);

    switch (code) {
    case ESP32S31_IPC_CMD_CONNECT:
        memcpy(cfg.sta.ssid, ipc->cmd.ssid, sizeof(cfg.sta.ssid));
        memcpy(cfg.sta.password, ipc->cmd.psk, sizeof(cfg.sta.password));

        /*
         * A station that is already associating rejects a new config, so
         * stand the old attempt down first and let it settle.
         */
        ipc_want_connection = false;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));

        ESP_LOGI(TAG, "connecting to \"%s\"", (const char *)cfg.sta.ssid);
        err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set config failed: %s", esp_err_to_name(err));
            break;
        }
        ipc_want_connection = true;
        esp_wifi_connect();
        break;
    case ESP32S31_IPC_CMD_DISCONNECT:
        ipc_want_connection = false;
        esp_wifi_disconnect();
        break;
    }
}

static void ipc_tx_task(void *arg)
{
    static uint8_t frame[ESP32S31_IPC_SLOT_DATA];
    uint32_t len;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ipc_run_command();
        while (ipc_ring_pop(&ipc->to_firmware, frame, &len)) {
            esp_wifi_internal_tx(WIFI_IF_STA, frame, len);
        }
    }
}

static void ipc_set_link(uint32_t up)
{
    __atomic_store_n(&ipc->link_up, up, __ATOMIC_RELEASE);
    REG_WRITE(IPC_DOORBELL_TO_LINUX_REG, 1);
}

static void ipc_wifi_event(void *arg, esp_event_base_t base, int32_t id,
                           void *data)
{
    if (base != WIFI_EVENT) {
        return;
    }

    switch (id) {
    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "associated");
        ipc_set_link(1);
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        ipc_set_link(0);
        if (ipc_want_connection) {
            esp_wifi_connect();
        }
        break;
    }
}

static esp_err_t start_ipc(void)
{
    esp_err_t err;

    memset(ipc, 0, sizeof(*ipc));
    err = esp_wifi_get_mac(WIFI_IF_STA, ipc->mac);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(ipc_tx_task, "wifi_ipc_tx", 4096, NULL, 5,
                    &ipc_tx_task_handle) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_intr_alloc(IPC_DOORBELL_TO_FIRMWARE, 0, ipc_from_linux_isr,
                         NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     ipc_wifi_event, NULL);
    if (err != ESP_OK) {
        return err;
    }

    esp_wifi_internal_reg_rxcb(WIFI_IF_STA, ipc_wifi_rx);

    ipc->version = ESP32S31_IPC_VERSION;
    __atomic_store_n(&ipc->magic, ESP32S31_IPC_MAGIC, __ATOMIC_RELEASE);

    ESP_LOGI(TAG, "wifi ipc at 0x%08" PRIx32 ", mac %02x:%02x:%02x:%02x:%02x:%02x",
             (uint32_t)WIFI_IPC_ADDR, ipc->mac[0], ipc->mac[1], ipc->mac[2],
             ipc->mac[3], ipc->mac[4], ipc->mac[5]);
    return ESP_OK;
}

/*
 * Bringing the radio up reads the flash, and a flash transaction disables the
 * cache Linux executes from, so this has to complete before hart 1 is
 * released.  NVS is switched off for the same reason: nothing may write flash
 * once the kernel is running.
 */
static void start_wifi(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err;

    cfg.nvs_enable = false;

    err = esp_event_loop_create_default();
    if (err == ESP_OK) {
        err = esp_wifi_init(&cfg);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err == ESP_OK) {
        err = start_ipc();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi setup failed: %s", esp_err_to_name(err));
    }
}

static void disable_watchdog(wdt_inst_t inst)
{
    wdt_hal_context_t ctx;

    wdt_hal_init(&ctx, inst, 0, false);
    wdt_hal_write_protect_disable(&ctx);
    wdt_hal_disable(&ctx);
}

static void disable_watchdogs(void)
{
    disable_watchdog(WDT_MWDT0);
    disable_watchdog(WDT_MWDT1);
    disable_watchdog(WDT_RWDT);
}

/*
 * The stack-spill monitor of the hart that runs Linux would fire on kernel
 * stacks, so widen it away.  Hart 0 keeps its own monitor.
 */
static void disable_linux_hart_stack_protector(void)
{
    assist_debug_ll_sp_spill_monitor_disable(LINUX_HART);
    assist_debug_ll_sp_spill_interrupt_disable(LINUX_HART);
    assist_debug_ll_sp_spill_set_min(LINUX_HART, 0);
    assist_debug_ll_sp_spill_set_max(LINUX_HART, 0xffffffff);
}

static esp_err_t configure_sd_pin(gpio_num_t gpio, bool pull_up)
{
    esp_err_t err;

    err = gpio_pulldown_dis(gpio);
    if (err != ESP_OK) {
        return err;
    }
    err = pull_up ? gpio_pullup_en(gpio) : gpio_pullup_dis(gpio);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_input_enable(gpio);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_iomux_output(gpio, SDMMC_LL_IOMUX_FUNC);
    if (err != ESP_OK) {
        return err;
    }
    return gpio_set_drive_capability(gpio, GPIO_DRIVE_CAP_3);
}

static void init_sd_card(void)
{
    static const gpio_num_t pins[] = {
        SDMMC_SLOT0_IOMUX_PIN_NUM_D0,
        SDMMC_SLOT0_IOMUX_PIN_NUM_D1,
        SDMMC_SLOT0_IOMUX_PIN_NUM_D2,
        SDMMC_SLOT0_IOMUX_PIN_NUM_D3,
        SDMMC_SLOT0_IOMUX_PIN_NUM_CLK,
        SDMMC_SLOT0_IOMUX_PIN_NUM_CMD,
    };
    uint32_t source_hz;
    uint32_t div;
    esp_err_t err;

    err = gpio_set_direction(SD_POWER_EN_GPIO, GPIO_MODE_OUTPUT);
    if (err == ESP_OK) {
        err = gpio_set_level(SD_POWER_EN_GPIO, 0);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to enable SD slot power: %s", esp_err_to_name(err));
        return;
    }

    err = esp_clk_tree_enable_src(SDMMC_CLK_SRC_DEFAULT, true);
    if (err == ESP_OK) {
        err = esp_clk_tree_src_get_freq_hz(SDMMC_CLK_SRC_DEFAULT,
                                           ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED,
                                           &source_hz);
    }
    if (err != ESP_OK || !source_hz) {
        ESP_LOGW(TAG, "failed to acquire SD host clock: %s", esp_err_to_name(err));
        return;
    }

    div = (source_hz + SD_TARGET_CCLK_HZ - 1) / SD_TARGET_CCLK_HZ;
    if (div < 1) {
        div = 1;
    } else if (div > 16) {
        div = 16;
    }

    PERIPH_RCC_ATOMIC() {
        sdmmc_ll_pad_set_pin_dedicated_ctrl(&SDMMC, true);
        sdmmc_ll_enable_bus_clock(0, true);
        sdmmc_ll_select_clk_source(&SDMMC, SDMMC_CLK_SRC_DEFAULT);
        sdmmc_ll_set_clock_div(&SDMMC, div);
        sdmmc_ll_init_phase_delay(&SDMMC);
        sdmmc_ll_reset_register(0);
        sdmmc_ll_mem_force_power_on(&SDMMC);
    }
    esp_rom_delay_us(10);

    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        bool pull_up = pins[i] != SDMMC_SLOT0_IOMUX_PIN_NUM_CLK;

        err = configure_sd_pin(pins[i], pull_up);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to configure SD GPIO %d: %s",
                     pins[i], esp_err_to_name(err));
            return;
        }
    }

    /*
     * The Korvo-1 microSD slot has no card-detect, write-protect or SDIO
     * interrupt contacts, so those host inputs are driven from the GPIO
     * matrix constant sources rather than from a pad.  CARD_DETECT_N is
     * active low and tied to 0 to report a card as always present,
     * WRITE_PRT is tied to an inverted 1 so the card never reads as write
     * protected, and CARD_INT_N is held at 1 to keep the SDIO interrupt
     * deasserted.
     */
    err = gpio_matrix_input(GPIO_MATRIX_CONST_ZERO_INPUT,
                            SD_CARD_DETECT_N_1_PAD_IN_IDX, false);
    if (err == ESP_OK) {
        err = gpio_matrix_input(GPIO_MATRIX_CONST_ONE_INPUT,
                                SD_CARD_WRITE_PRT_1_PAD_IN_IDX, true);
    }
    if (err == ESP_OK) {
        err = gpio_matrix_input(GPIO_MATRIX_CONST_ONE_INPUT,
                                SD_CARD_INT_N_1_PAD_IN_IDX, false);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to route SD slot status signals: %s",
                 esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "SD host: source=%" PRIu32 "Hz cclk_in=%" PRIu32 "Hz div=%" PRIu32
                  " verid=0x%08" PRIx32,
             source_hz, source_hz / div, div, sdmmc_ll_get_version_id(&SDMMC));
}

/*
 * Reset entry for hart 1, reached from the app-CPU ROM once its boot address
 * is set.  PMA is per-hart state and this hart comes out of reset without an
 * entry covering PSRAM, so OpenSBI would fault on the first write to its own
 * BSS; grant the same regions ESP-IDF gives its application core first.
 * OpenSBI's _start builds its own stack, so all it needs from here is the
 * entry ABI: hart ID in a0, device tree in a1.
 */
static void IRAM_ATTR linux_hart_entry(void)
{
    esp_cpu_configure_region_protection();

    __asm__ volatile (
        "li   a0, %0\n\t"
        "li   a1, 0\n\t"
        "li   t0, %1\n\t"
        "jr   t0"
        :: "i"(LINUX_HART), "i"(OPENSBI_LOAD_ADDR)
        : "a0", "a1", "t0");
}

static void start_linux_hart(void)
{
    esp_cpu_unstall(LINUX_HART);
    cpu_utility_ll_enable_clock_and_reset_app_cpu();
    ets_set_appcpu_boot_addr((uint32_t)linux_hart_entry);
}

static const esp_partition_t *find_partition(const char *name)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, name);
    if (!part) {
        ESP_LOGE(TAG, "%s partition not found", name);
    }
    return part;
}

static bool load_kernel_partition(void)
{
    const esp_partition_t *part = find_partition("linux");
    struct kernel_size_manifest manifest;
    uint64_t memory_size;
    uint32_t image_size;
    size_t psram_size;
    esp_err_t err;
    uint32_t magic2;

    if (!part) {
        return false;
    }

    err = esp_partition_read(part, part->size - sizeof(manifest),
                             &manifest, sizeof(manifest));
    if (err != ESP_OK || manifest.magic != KERNEL_SIZE_MAGIC ||
        manifest.image_size < KERNEL_MAGIC2_OFFSET + sizeof(uint32_t) ||
        manifest.image_size > part->size - sizeof(manifest)) {
        ESP_LOGE(TAG, "invalid kernel size manifest");
        return false;
    }
    image_size = manifest.image_size;

    psram_size = esp_psram_get_size();
    if (KERNEL_LOAD_ADDR + image_size > PSRAM_BASE + psram_size) {
        ESP_LOGE(TAG, "kernel range exceeds PSRAM size 0x%08x",
                 (unsigned)psram_size);
        return false;
    }

    err = esp_partition_read(part, 0, (void *)KERNEL_LOAD_ADDR, image_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reading linux partition failed: %s",
                 esp_err_to_name(err));
        return false;
    }

    if (esp_rom_crc32_le(0, (const uint8_t *)KERNEL_LOAD_ADDR, image_size) !=
        manifest.image_crc32) {
        ESP_LOGE(TAG, "kernel image CRC mismatch");
        return false;
    }

    magic2 = *(const volatile uint32_t *)(KERNEL_LOAD_ADDR + KERNEL_MAGIC2_OFFSET);
    if (magic2 != KERNEL_MAGIC2) {
        ESP_LOGE(TAG, "kernel image magic 0x%08" PRIx32
                      " != RISC-V image magic 0x%08" PRIx32,
                 magic2, (uint32_t)KERNEL_MAGIC2);
        return false;
    }

    memcpy(&memory_size,
           (const void *)(KERNEL_LOAD_ADDR + KERNEL_IMAGE_SIZE_OFFSET),
           sizeof(memory_size));
    if (memory_size < image_size ||
        KERNEL_LOAD_ADDR + memory_size > INITRAMFS_LOAD_ADDR) {
        ESP_LOGE(TAG, "invalid kernel memory size 0x%08" PRIx32,
                 (uint32_t)memory_size);
        return false;
    }
    memset((void *)(KERNEL_LOAD_ADDR + image_size), 0,
           (size_t)memory_size - image_size);

    ESP_LOGI(TAG, "kernel copied: flash=0x%08" PRIx32 " file=0x%08" PRIx32
                  " memory=0x%08" PRIx32 " psram=0x%08" PRIx32,
             part->address, image_size, (uint32_t)memory_size,
             (uint32_t)KERNEL_LOAD_ADDR);
    return true;
}

static bool load_opensbi_partition(void)
{
    const esp_partition_t *part = find_partition("opensbi");
    esp_err_t err;

    if (!part) {
        return false;
    }

    if (part->size < OPENSBI_LOAD_SIZE) {
        ESP_LOGE(TAG, "opensbi partition size 0x%08" PRIx32
                      " is smaller than the loaded window 0x%08" PRIx32,
                 part->size, (uint32_t)OPENSBI_LOAD_SIZE);
        return false;
    }

    /*
     * fw_jump carries no size manifest, so a fixed window is loaded and
     * OpenSBI's own startup clears whatever follows the image as BSS.
     */
    err = esp_partition_read(part, 0, (void *)OPENSBI_LOAD_ADDR,
                             OPENSBI_LOAD_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reading opensbi partition failed: %s",
                 esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "opensbi copied: flash=0x%08" PRIx32 " size=0x%08" PRIx32
                  " psram=0x%08" PRIx32,
             part->address, (uint32_t)OPENSBI_LOAD_SIZE,
             (uint32_t)OPENSBI_LOAD_ADDR);
    return true;
}

static bool load_initramfs_partition(void)
{
    const esp_partition_t *part = find_partition("initramfs");
    esp_err_t err;
    uint32_t magic0;

    if (!part) {
        return false;
    }

    if (part->size != INITRAMFS_PARTITION_SIZE) {
        ESP_LOGE(TAG, "initramfs partition size 0x%08" PRIx32
                      " != expected 0x%08" PRIx32,
                 part->size, (uint32_t)INITRAMFS_PARTITION_SIZE);
        return false;
    }

    err = esp_partition_read(part, 0, (void *)INITRAMFS_LOAD_ADDR, part->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reading initramfs partition failed: %s",
                 esp_err_to_name(err));
        return false;
    }

    magic0 = *(const volatile uint32_t *)INITRAMFS_LOAD_ADDR;
    if (magic0 != INITRAMFS_NEWC_MAGIC0) {
        ESP_LOGE(TAG, "initramfs magic 0x%08" PRIx32
                      " != newc prefix 0x%08" PRIx32,
                 magic0, (uint32_t)INITRAMFS_NEWC_MAGIC0);
        return false;
    }

    ESP_LOGI(TAG, "initramfs copied: flash=0x%08" PRIx32 " size=0x%08" PRIx32
                  " psram=0x%08" PRIx32,
             part->address, part->size, (uint32_t)INITRAMFS_LOAD_ADDR);
    return true;
}

void app_main(void)
{
    open_apm();
    disable_watchdogs();

    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM not initialized");
        esp_restart();
    }
    log_cache_mode();
    init_sd_card();

    if (!load_opensbi_partition() || !load_kernel_partition() ||
        !load_initramfs_partition()) {
        esp_restart();
    }

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
    /* GPIO33/34 are native USB Serial/JTAG D-/D+ and LCD RGB data pins. */
    ESP_LOGI(TAG, "display disabled: GPIO33/34 reserved for USB Serial/JTAG");
#else
    if (!display_init()) {
        ESP_LOGW(TAG, "continuing without display");
    }
#endif

    ESP_LOGI(TAG, "handoff: OpenSBI=0x%08" PRIx32 " kernel=0x%08" PRIx32
                  " initramfs=0x%08" PRIx32,
             (uint32_t)OPENSBI_LOAD_ADDR, (uint32_t)KERNEL_LOAD_ADDR,
             (uint32_t)INITRAMFS_LOAD_ADDR);

    start_wifi();

    disable_linux_hart_stack_protector();
    flush_l1_cache_before_handoff();

    start_linux_hart();
    ESP_LOGI(TAG, "hart 0 resident");
}
