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
#include "riscv/csr.h"
#include "esp32s31/rom/cache.h"
#include "hal/cache_ll.h"
#include "hal/assist_debug_ll.h"
#include "hal/wdt_hal.h"
#include "soc/soc.h"
#include "soc/cpu_apm_reg.h"
#include "soc/hp_apm_reg.h"
#include "soc/hp_mem_apm_reg.h"
#include "display.h"

#define PSRAM_BASE                  0x50000000U
#define OPENSBI_XIP_ADDR            0x40030000U
#define OPENSBI_RAM_ADDR            0x2F000000U
#define OPENSBI_COPY_SIZE           0x00040000U
#define KERNEL_LOAD_ADDR            0x50000000U
#define KERNEL_IMAGE_SIZE_OFFSET    0x10U
#define KERNEL_MAGIC2_OFFSET        0x38U
#define KERNEL_MAGIC2               0x05435352U
#define KERNEL_SIZE_MAGIC           0x455A4953U
#define INITRAMFS_LOAD_ADDR         0x50800000U
#define INITRAMFS_PARTITION_SIZE    0x00200000U
#define INITRAMFS_NEWC_MAGIC0       0x37303730U
#define PMP_FULL_SPACE_NAPOT_ADDR   0x3FFFFFFFU
#define PMP_ENTRY_RWX_LOCK_NAPOT    0x9FU

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

static void open_apm(void)
{
    REG_WRITE(CPU_APM_REGION0_ADDR_START_REG, 0);
    REG_WRITE(CPU_APM_REGION0_ADDR_END_REG, 0xFFFFFFFF);
    REG_WRITE(CPU_APM_REGION0_ATTR_REG, 0x7777);
    REG_WRITE(CPU_APM_REGION_FILTER_EN_REG, 1);
    REG_WRITE(CPU_APM_FUNC_CTRL_REG, 0xFFFFFFFF);

    REG_WRITE(HP_APM_REGION0_ADDR_START_REG, 0);
    REG_WRITE(HP_APM_REGION0_ADDR_END_REG, 0xFFFFFFFF);
    REG_WRITE(HP_APM_REGION0_ATTR_REG, 0x7777);
    REG_WRITE(HP_APM_REGION_FILTER_EN_REG, 1);
    REG_WRITE(HP_APM_FUNC_CTRL_REG, 0xFFFFFFFF);

    REG_WRITE(HP_MEM_APM_REGION0_ADDR_START_REG, 0);
    REG_WRITE(HP_MEM_APM_REGION0_ADDR_END_REG, 0xFFFFFFFF);
    REG_WRITE(HP_MEM_APM_REGION0_ATTR_REG, 0x7777);
    REG_WRITE(HP_MEM_APM_REGION_FILTER_EN_REG, 1);
    REG_WRITE(HP_MEM_APM_FUNC_CTRL_REG, 0xFFFFFFFF);

    for (int i = 0; i < 4; i++) {
        REG_WRITE(DR_REG_FLASH_SPI0_BASE + 0x100 + i * 4, 0x1B);
        REG_WRITE(DR_REG_FLASH_SPI0_BASE + 0x130 + i * 4, 0x1B);
        REG_WRITE(DR_REG_PSRAM_MSPI0_BASE + 0x100 + i * 4, 0x1B);
        REG_WRITE(DR_REG_PSRAM_MSPI0_BASE + 0x130 + i * 4, 0x1B);
    }

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

static void install_global_pmp(void)
{
    RV_WRITE_CSR(pmpaddr0, PMP_FULL_SPACE_NAPOT_ADDR);
    RV_WRITE_CSR(pmpcfg0, PMP_ENTRY_RWX_LOCK_NAPOT);
}

static void mask_clic_slots(void)
{
    for (int i = 0; i < 128; i++) {
        volatile uint8_t *ie = (volatile uint8_t *)(0x10801000 + i * 4 + 1);
        volatile uint8_t *attr = (volatile uint8_t *)(0x10801000 + i * 4 + 2);
        *ie = 0;
        *attr = 0;
    }
}

static void disable_watchdogs_and_clic(void)
{
    volatile uint32_t *timg0 = (volatile uint32_t *)0x20580000;
    volatile uint32_t *timg1 = (volatile uint32_t *)0x20581000;

    timg0[0x64 / 4] = 0x50D83AA1;
    timg0[0x48 / 4] = 0;
    timg0[0x64 / 4] = 0;

    timg1[0x64 / 4] = 0x50D83AA1;
    timg1[0x48 / 4] = 0;
    timg1[0x64 / 4] = 0;

    wdt_hal_context_t rtc_wdt_ctx;
    wdt_hal_init(&rtc_wdt_ctx, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&rtc_wdt_ctx);
    wdt_hal_disable(&rtc_wdt_ctx);

    mask_clic_slots();

    asm volatile ("csrw 0x307, zero");
}

static void clear_mstatus_mprv(void)
{
    RV_CLEAR_CSR(mstatus, MSTATUS_MPRV);
}

static void disable_stack_protector(void)
{
    assist_debug_ll_sp_spill_monitor_disable(0);
    assist_debug_ll_sp_spill_monitor_disable(1);
    assist_debug_ll_sp_spill_interrupt_disable(0);
    assist_debug_ll_sp_spill_interrupt_disable(1);
    assist_debug_ll_sp_spill_set_min(0, 0);
    assist_debug_ll_sp_spill_set_max(0, 0xffffffff);
    assist_debug_ll_sp_spill_set_min(1, 0);
    assist_debug_ll_sp_spill_set_max(1, 0xffffffff);
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
    const esp_partition_t *opensbi_part;
    const void *opensbi_ptr;
    esp_partition_mmap_handle_t mmap_handle;
    uintptr_t copy_src;
    uintptr_t copy_dst;
    const uintptr_t copy_end = OPENSBI_RAM_ADDR + OPENSBI_COPY_SIZE;
    const uintptr_t opensbi_entry = OPENSBI_RAM_ADDR;
    esp_err_t err;

    open_apm();
    disable_watchdogs_and_clic();
    clear_mstatus_mprv();
    install_global_pmp();

    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM not initialized");
        esp_restart();
    }
    log_cache_mode();

    opensbi_part = find_partition("opensbi");
    if (!opensbi_part) {
        esp_restart();
    }

    err = esp_partition_mmap(opensbi_part, 0, opensbi_part->size,
                             ESP_PARTITION_MMAP_INST,
                             &opensbi_ptr, &mmap_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_partition_mmap failed: %s", esp_err_to_name(err));
        esp_restart();
    }

    if ((uintptr_t)opensbi_ptr != OPENSBI_XIP_ADDR) {
        ESP_LOGE(TAG, "OpenSBI mmap address %p != linked load address 0x%08" PRIx32,
                 opensbi_ptr, (uint32_t)OPENSBI_XIP_ADDR);
        esp_restart();
    }

    if (!load_kernel_partition() || !load_initramfs_partition()) {
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
    mask_clic_slots();

    ESP_LOGI(TAG, "handoff: OpenSBI 0x%08" PRIx32 "->0x%08" PRIx32
                  " kernel=0x%08" PRIx32 " initramfs=0x%08" PRIx32,
             (uint32_t)OPENSBI_XIP_ADDR, (uint32_t)OPENSBI_RAM_ADDR,
             (uint32_t)KERNEL_LOAD_ADDR, (uint32_t)INITRAMFS_LOAD_ADDR);

    disable_stack_protector();
    flush_l1_cache_before_handoff();

    copy_src = OPENSBI_XIP_ADDR;
    copy_dst = OPENSBI_RAM_ADDR;

    __asm__ volatile (
        "csrw  mie, zero\n\t"
        "csrci mstatus, 0x8\n\t"
        "1:\n\t"
        "lw    t0, 0(%0)\n\t"
        "sw    t0, 0(%1)\n\t"
        "addi  %0, %0, 4\n\t"
        "addi  %1, %1, 4\n\t"
        "bltu  %1, %2, 1b\n\t"
        "fence\n\t"
        "fence.i\n\t"
        "mv    a0, zero\n\t"
        "mv    a1, zero\n\t"
        "jr    %3"
        : "+&r"(copy_src), "+&r"(copy_dst)
        : "r"(copy_end), "r"(opensbi_entry)
        : "t0", "a0", "a1", "memory");
    __builtin_unreachable();
}
