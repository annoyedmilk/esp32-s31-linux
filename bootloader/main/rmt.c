// SPDX-License-Identifier: BSD-2-Clause
// Author: Marco Müller <hello@annoyedmilk.ch>

#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_private/gpio.h"
#include "esp_private/periph_ctrl.h"
#include "hal/rmt_ll.h"
#include "soc/gpio_sig_map.h"
#include "soc/rmt_struct.h"
#include "loader.h"

/*
 * The Korvo-1 WS2812 RGB LED is on GPIO37.  The Linux LED driver sends the
 * bits through RMT channel 0.  The clock and reset bits of the RMT are in
 * HP_SYS_CLKRST registers that hart 0 also writes.  Thus set them here, one
 * time, before hart 1 starts.  Linux then writes only the channel registers
 * and the channel RAM.
 */
#define RMT_LED_GPIO        GPIO_NUM_37
#define RMT_LED_CHANNEL     0

/* XTAL (40 MHz) / 2 = one tick of 50 ns. */
#define RMT_GROUP_DIV       1
#define RMT_CHANNEL_DIV     2

static const char *TAG = "s31-linux-rmt";

void init_rmt_led(void)
{
    esp_err_t err;

    PERIPH_RCC_ATOMIC() {
        rmt_ll_enable_bus_clock(0, true);
        rmt_ll_reset_register(0);
    }
    rmt_ll_set_group_clock_src(&RMT, RMT_LED_CHANNEL, RMT_CLK_SRC_XTAL,
                               RMT_GROUP_DIV, 1, 0);
    rmt_ll_enable_group_clock(&RMT, true);
    rmt_ll_mem_force_power_on(&RMT);
    rmt_ll_enable_mem_access_nonfifo(&RMT, true);

    rmt_ll_tx_reset_channels_clock_div(&RMT, 1U << RMT_LED_CHANNEL);
    rmt_ll_tx_set_channel_clock_div(&RMT, RMT_LED_CHANNEL, RMT_CHANNEL_DIV);
    rmt_ll_tx_set_mem_blocks(&RMT, RMT_LED_CHANNEL, 1);
    rmt_ll_tx_enable_carrier_modulation(&RMT, RMT_LED_CHANNEL, false);
    rmt_ll_tx_enable_loop(&RMT, RMT_LED_CHANNEL, false);
    rmt_ll_tx_enable_wrap(&RMT, RMT_LED_CHANNEL, false);
    rmt_ll_tx_fix_idle_level(&RMT, RMT_LED_CHANNEL, 0, true);
    rmt_ll_enable_interrupt(&RMT, RMT_LL_EVENT_TX_MASK(RMT_LED_CHANNEL), false);
    rmt_ll_clear_interrupt_status(&RMT, RMT_LL_EVENT_TX_MASK(RMT_LED_CHANNEL));

    err = gpio_set_direction(RMT_LED_GPIO, GPIO_MODE_OUTPUT);
    if (err == ESP_OK) {
        err = gpio_matrix_output(RMT_LED_GPIO, RMT_SIG_OUT0_IDX, false, false);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to route RMT to GPIO%d: %s", RMT_LED_GPIO,
                 esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "RMT channel %d on GPIO%d, 50 ns tick", RMT_LED_CHANNEL,
             RMT_LED_GPIO);
}
