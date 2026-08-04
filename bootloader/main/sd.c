// SPDX-License-Identifier: BSD-2-Clause
// Author: Marco Müller <hello@annoyedmilk.ch>

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_clk_tree.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "esp_private/esp_clk_tree_common.h"
#include "esp_private/gpio.h"
#include "esp_private/periph_ctrl.h"
#include "hal/sdmmc_ll.h"
#include "soc/clk_tree_defs.h"
#include "soc/gpio_pins.h"
#include "soc/gpio_sig_map.h"
#include "soc/sdmmc_pins.h"
#include "loader.h"

#define SD_POWER_EN_GPIO    GPIO_NUM_39
#define SD_TARGET_CCLK_HZ   50000000U

static const char *TAG = "s31-linux-sd";

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

void init_sd_card(void)
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
