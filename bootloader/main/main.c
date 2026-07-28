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
#include "driver/gpio.h"
#include "riscv/csr.h"
#include "esp32s31/rom/cache.h"
#include "esp_private/esp_clk_tree_common.h"
#include "esp_private/gpio.h"
#include "esp_private/periph_ctrl.h"
#include "hal/cache_ll.h"
#include "hal/assist_debug_ll.h"
#include "hal/sdmmc_ll.h"
#include "hal/wdt_hal.h"
#include "riscv/csr_clic.h"
#include "soc/soc.h"
#include "soc/soc_caps.h"
#include "soc/clic_reg.h"
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

/*
 * Total number of CLIC input slots: the 16 architectural local causes
 * followed by the SOC_CPU_INTR_NUM Interrupt Matrix inputs.  This is the
 * 48 that CLIC_INT_INFO.NUM_INT reports on the ESP32-S31.
 */
#define CLIC_SLOT_COUNT             (CLIC_EXT_INTR_NUM_OFFSET + SOC_CPU_INTR_NUM)

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

/*
 * The ESP32-S31 implements a single PMP entry, so it is spent on one
 * permanent full-address-space grant.  pmpaddr0 = 0x3fffffff is the NAPOT
 * encoding of a 2^32-byte range based at zero (all 30 encoded address bits
 * set), and pmpcfg0 = 0x9f is L | A=NAPOT | X | W | R.  The lock bit also
 * makes the entry apply to M-mode, and it cannot be rewritten before reset,
 * so OpenSBI must leave PMP-based domain isolation alone.
 */
static void install_global_pmp(void)
{
    RV_WRITE_CSR(pmpaddr0, PMP_FULL_SPACE_NAPOT_ADDR);
    RV_WRITE_CSR(pmpcfg0, PMP_ENTRY_RWX_LOCK_NAPOT);
}

static void mask_clic_slots(void)
{
    /*
     * Silence every CLIC input before handing off.  Each slot owns one
     * 32-bit control word whose four bytes are individually addressable:
     * byte 0 is IP, byte 1 IE, byte 2 ATTR, byte 3 CTL.  Clearing IE
     * disables the input and clearing ATTR returns it to M-mode,
     * level-triggered, non-vectored delivery, so OpenSBI and Linux start
     * from a known-quiet controller.
     */
    for (int i = 0; i < CLIC_SLOT_COUNT; i++) {
        *(volatile uint8_t *)BYTE_CLIC_INT_IE_REG(i) = 0;
        *(volatile uint8_t *)BYTE_CLIC_INT_ATTR_REG(i) = 0;
    }
}

static void disable_watchdog(wdt_inst_t inst)
{
    wdt_hal_context_t ctx;

    wdt_hal_init(&ctx, inst, 0, false);
    wdt_hal_write_protect_disable(&ctx);
    wdt_hal_disable(&ctx);
}

static void disable_watchdogs_and_clic(void)
{
    disable_watchdog(WDT_MWDT0);
    disable_watchdog(WDT_MWDT1);
    disable_watchdog(WDT_RWDT);

    mask_clic_slots();

    RV_WRITE_CSR(MTVT_CSR, 0);
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
    init_sd_card();

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

    /*
     * Relocate OpenSBI out of the flash XIP window into SRAM and jump to it.
     * The copy loop has to be inline assembly because it runs after the point
     * of no return: interrupts are hard-disabled first (mie = 0, then
     * mstatus.MIE cleared via csrci with bit 3), and once the destination is
     * being overwritten no C runtime state may be relied upon.
     *
     * fence orders the copy against the following fetches and fence.i
     * discards instruction-cache lines for the destination range, so the jump
     * observes the freshly written image rather than stale prefetched bytes.
     * a0 and a1 are zeroed because OpenSBI's fw_jump entry expects the hart
     * ID in a0 and a device-tree pointer in a1; this platform passes neither.
     */
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
