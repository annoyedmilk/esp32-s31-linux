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
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp32s31/rom/cache.h"
#include "esp32s31/rom/ets_sys.h"
#include "hal/assist_debug_ll.h"
#include "hal/cache_ll.h"
#include "hal/cpu_utility_ll.h"
#include "hal/wdt_hal.h"
#include "heap_memory_layout.h"
#include "soc/soc.h"
#include "soc/cpu_apm_reg.h"
#include "soc/hp_apm_reg.h"
#include "soc/hp_mem_apm_reg.h"
#include "soc/reg_base.h"
#include "soc/spi_mem_c_reg.h"
#include "display.h"
#include "loader.h"

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
 * The kernel's coherent DMA pool, kept out of the ESP-IDF heap because this
 * firmware stays resident.  The dma-pool node in esp32s31.dtsi carries the
 * same range.
 */
#define LINUX_DMA_POOL_ADDR         0x2F040000U
#define LINUX_DMA_POOL_SIZE         0x00010000U

SOC_RESERVE_MEMORY_REGION(LINUX_DMA_POOL_ADDR,
                          LINUX_DMA_POOL_ADDR + LINUX_DMA_POOL_SIZE,
                          linux_dma_pool);

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

static const char *TAG = "s31-linux-loader";

struct kernel_size_manifest {
    uint32_t magic;
    uint32_t image_size;
    uint32_t image_crc32;
};

struct apm_block {
    uint32_t region0_start;
    uint32_t region0_end;
    uint32_t region0_attr;
    uint32_t region_filter_en;
    uint32_t func_ctrl;
};

static const struct apm_block apm_blocks[] = {
    { CPU_APM_REGION0_ADDR_START_REG, CPU_APM_REGION0_ADDR_END_REG,
      CPU_APM_REGION0_ATTR_REG, CPU_APM_REGION_FILTER_EN_REG,
      CPU_APM_FUNC_CTRL_REG },
    { HP_APM_REGION0_ADDR_START_REG, HP_APM_REGION0_ADDR_END_REG,
      HP_APM_REGION0_ATTR_REG, HP_APM_REGION_FILTER_EN_REG,
      HP_APM_FUNC_CTRL_REG },
    { HP_MEM_APM_REGION0_ADDR_START_REG, HP_MEM_APM_REGION0_ADDR_END_REG,
      HP_MEM_APM_REGION0_ATTR_REG, HP_MEM_APM_REGION_FILTER_EN_REG,
      HP_MEM_APM_FUNC_CTRL_REG },
};

static const uint32_t apm_status_clr_regs[] = {
    HP_APM_M0_STATUS_CLR_REG, HP_APM_M1_STATUS_CLR_REG,
    HP_APM_M2_STATUS_CLR_REG, HP_APM_M3_STATUS_CLR_REG,
    HP_APM_M4_STATUS_CLR_REG, HP_APM_M5_STATUS_CLR_REG,
    HP_APM_M6_STATUS_CLR_REG,
    HP_MEM_APM_M0_STATUS_CLR_REG, HP_MEM_APM_M1_STATUS_CLR_REG,
    HP_MEM_APM_M2_STATUS_CLR_REG, HP_MEM_APM_M3_STATUS_CLR_REG,
    HP_MEM_APM_M4_STATUS_CLR_REG, HP_MEM_APM_M5_STATUS_CLR_REG,
    CPU_APM_M0_STATUS_CLR_REG, CPU_APM_M1_STATUS_CLR_REG,
    CPU_APM_M2_STATUS_CLR_REG, CPU_APM_M3_STATUS_CLR_REG,
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
    for (size_t i = 0; i < sizeof(apm_blocks) / sizeof(apm_blocks[0]); i++) {
        const struct apm_block *b = &apm_blocks[i];

        REG_WRITE(b->region0_start, 0);
        REG_WRITE(b->region0_end, 0xFFFFFFFF);
        REG_WRITE(b->region0_attr, APM_REGION_ATTR_ALL_RWX);
        REG_WRITE(b->region_filter_en, 1);
        REG_WRITE(b->func_ctrl, 0xFFFFFFFF);
    }

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
    for (size_t i = 0;
         i < sizeof(apm_status_clr_regs) / sizeof(apm_status_clr_regs[0]); i++) {
        REG_WRITE(apm_status_clr_regs[i], 1);
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
