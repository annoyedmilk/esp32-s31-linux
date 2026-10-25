// SPDX-License-Identifier: BSD-2-Clause
// Author: Marco Müller <hello@annoyedmilk.ch>

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_rom_crc.h"
#include "bootloader_random.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp32s31/rom/ets_sys.h"
#include "riscv/csr.h"
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
#include "adapter.h"
#include "interface.h"
#include "shm_flash.h"
#include "loader.h"

#define PSRAM_BASE                  0x50000000U
#define OPENSBI_LOAD_ADDR           0x50F00000U
#define OPENSBI_LOAD_SIZE           0x00030000U
#define KERNEL_LOAD_ADDR            0x50000000U
#define KERNEL_IMAGE_SIZE_OFFSET    0x10U
#define KERNEL_MAGIC2_OFFSET        0x38U
#define KERNEL_MAGIC2               0x05435352U
#define KERNEL_SIZE_MAGIC           0x455A4953U
#define PSRAM_PMA_SIZE              0x01000000U
#define PSRAM_UNCACHED_BASE         0xC0000000U
#define LINUX_HART                  1U

/* SRAM of the kernel (dma-pool and trap-sram in esp32s31.dtsi), not heap. */
#define LINUX_DMA_POOL_ADDR         0x2F040000U
#define LINUX_DMA_POOL_SIZE         0x00010000U

SOC_RESERVE_MEMORY_REGION(LINUX_DMA_POOL_ADDR,
                          LINUX_DMA_POOL_ADDR + LINUX_DMA_POOL_SIZE,
                          linux_dma_pool);

/* The ROM start code of hart 1 uses this stack: keep it out of the heap. */
#define ROM_HART1_STACK_TOP         0x2F07E600U
#define ROM_HART1_STACK_SIZE        0x00000400U

SOC_RESERVE_MEMORY_REGION(ROM_HART1_STACK_TOP - ROM_HART1_STACK_SIZE,
                          ROM_HART1_STACK_TOP,
                          rom_hart1_stack);

/* One nibble (X bit 0, W bit 1, R bit 2) for REE modes 0 to 2 and for TEE. */
#define APM_REGION_ATTR_ALL_RWX     0x7777U

/* R/W for secure and non-secure access, no ECC; same bits on all banks. */
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

/* Open each APM: one region, all addresses, RWX for all modes and masters. */
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

    /* The four PMS sections of the flash and of the PSRAM controller. */
    for (int i = 0; i < 4; i++) {
        const uint32_t psram_off = DR_REG_PSRAM_MSPI0_BASE - DR_REG_FLASH_SPI0_BASE;

        REG_WRITE(SPI_FMEM_C_PMS0_ATTR_REG + i * 4, MSPI_PMS_ATTR_RW);
        REG_WRITE(SPI_SMEM_C_PMS0_ATTR_REG + i * 4, MSPI_PMS_ATTR_RW);
        REG_WRITE(SPI_FMEM_C_PMS0_ATTR_REG + psram_off + i * 4, MSPI_PMS_ATTR_RW);
        REG_WRITE(SPI_SMEM_C_PMS0_ATTR_REG + psram_off + i * 4, MSPI_PMS_ATTR_RW);
    }

    /* Clear the permission faults that the filters latched while they opened. */
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

/* The stack-spill monitor of hart 1 would trigger on kernel stacks. */
static void disable_linux_hart_stack_protector(void)
{
    assist_debug_ll_sp_spill_monitor_disable(LINUX_HART);
    assist_debug_ll_sp_spill_interrupt_disable(LINUX_HART);
    assist_debug_ll_sp_spill_set_min(LINUX_HART, 0);
    assist_debug_ll_sp_spill_set_max(LINUX_HART, 0xffffffff);
}

/* Lock hart 0 out of the memory of hart 1 (docs/internals.md, "Protection"). */
#define PMP_NO_ACCESS               (PMP_L | PMP_NAPOT)

static void protect_linux_memory(void)
{
    PMP_RESET_AND_ENTRY_SET(13, PMPADDR_NAPOT(PSRAM_BASE, PSRAM_BASE + PSRAM_PMA_SIZE),
                            PMP_NO_ACCESS);
    PMP_RESET_AND_ENTRY_SET(14, PMPADDR_NAPOT(PSRAM_UNCACHED_BASE,
                                              PSRAM_UNCACHED_BASE + PSRAM_PMA_SIZE),
                            PMP_NO_ACCESS);
    PMP_RESET_AND_ENTRY_SET(15, PMPADDR_NAPOT(LINUX_DMA_POOL_ADDR,
                                              LINUX_DMA_POOL_ADDR + LINUX_DMA_POOL_SIZE),
                            PMP_NO_ACCESS);

    if (PMP_ENTRY_CFG_READ(13) != PMP_NO_ACCESS ||
        PMP_ENTRY_CFG_READ(14) != PMP_NO_ACCESS ||
        PMP_ENTRY_CFG_READ(15) != PMP_NO_ACCESS) {
        ESP_LOGW(TAG, "PMP entries 13-15 are in use: Linux memory not protected");
    }
}

/* Reset entry of hart 1.  esp_cpu_configure_region_protection() locks PMP. */
static void IRAM_ATTR linux_hart_entry(void)
{
    /* The ROM PMA entry gives hart 1 no write to PSRAM; a lower index wins. */
    PMA_ENTRY_SET_NAPOT(2, PSRAM_BASE, PSRAM_PMA_SIZE,
                        PMA_NAPOT | PMA_EN | PMA_R | PMA_W | PMA_X);

    /* Hart ID in a0, no DTB in a1. */
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

/* Copy the start of a partition to PSRAM. */
static bool read_partition(const esp_partition_t *part, uint32_t addr,
                           size_t size)
{
    esp_err_t err = esp_partition_read(part, 0, (void *)addr, size);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reading %s partition failed: %s", part->label,
                 esp_err_to_name(err));
    }
    return err == ESP_OK;
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

    if (!read_partition(part, KERNEL_LOAD_ADDR, image_size)) {
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
        KERNEL_LOAD_ADDR + memory_size > OPENSBI_LOAD_ADDR) {
        ESP_LOGE(TAG, "invalid kernel memory size 0x%08" PRIx32,
                 (uint32_t)memory_size);
        return false;
    }

    ESP_LOGI(TAG, "kernel copied: flash=0x%08" PRIx32 " file=0x%08" PRIx32
                  " memory=0x%08" PRIx32 " psram=0x%08" PRIx32,
             part->address, image_size, (uint32_t)memory_size,
             (uint32_t)KERNEL_LOAD_ADDR);
    return true;
}

static bool load_opensbi_partition(void)
{
    const esp_partition_t *part = find_partition("opensbi");

    if (!part) {
        return false;
    }

    if (part->size < OPENSBI_LOAD_SIZE) {
        ESP_LOGE(TAG, "opensbi partition size 0x%08" PRIx32
                      " is smaller than the loaded window 0x%08" PRIx32,
                 part->size, (uint32_t)OPENSBI_LOAD_SIZE);
        return false;
    }

    /* fw_jump has no size manifest: a fixed window, OpenSBI clears its BSS. */
    if (!read_partition(part, OPENSBI_LOAD_ADDR, OPENSBI_LOAD_SIZE)) {
        return false;
    }

    ESP_LOGI(TAG, "opensbi copied: flash=0x%08" PRIx32 " size=0x%08" PRIx32
                  " psram=0x%08" PRIx32,
             part->address, (uint32_t)OPENSBI_LOAD_SIZE,
             (uint32_t)OPENSBI_LOAD_ADDR);
    return true;
}

/* A restart at full speed loops and scrolls the error away: wait first. */
static void restart_after_error(void)
{
    ESP_LOGE(TAG, "restart in 5 s");
    vTaskDelay(pdMS_TO_TICKS(5000));
    esp_restart();
}

void app_main(void)
{
    open_apm();
    disable_watchdogs();
    /* ESP-IDF stops the TRNG after the 2nd stage; the Linux hwrng needs it. */
    bootloader_random_enable();

    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM not initialized");
        restart_after_error();
    }

    if (!load_opensbi_partition() || !load_kernel_partition()) {
        restart_after_error();
    }

    board_start_display();

    ESP_LOGI(TAG, "handoff: OpenSBI=0x%08" PRIx32 " kernel=0x%08" PRIx32,
             (uint32_t)OPENSBI_LOAD_ADDR, (uint32_t)KERNEL_LOAD_ADDR);

    /* A radio start accesses the flash (cache off): do it before hart 1. */
    esp_hosted_main();
    /* ESP-Hosted starts Bluetooth at the first host request: do it now too. */
    if (esp_radio_service_apply(ESP_RADIO_SERVICE_BT, NULL, NULL, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "Bluetooth did not start");
    }

    disable_linux_hart_stack_protector();
    flush_l1_cache_before_handoff();

    shm_flash_share_cache();
    start_linux_hart();
    protect_linux_memory();
    ESP_LOGI(TAG, "hart 0 resident, heap: %u bytes free",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    /* Linux has the UART now: print only errors, so the lines do not mix. */
    esp_log_level_set("*", ESP_LOG_ERROR);
}
