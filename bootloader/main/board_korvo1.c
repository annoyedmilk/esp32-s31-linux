// SPDX-License-Identifier: BSD-2-Clause
// Author: Marco Müller <hello@annoyedmilk.ch>

#include "sdkconfig.h"
#include "esp_log.h"
#include "display.h"
#include "loader.h"

/* ESP32-S31-Korvo-1: microSD slot, WS2812 LED and the 800x480 RGB panel. */

static const char *TAG = "s31-linux-korvo1";

void board_init(void)
{
    init_sd_card();
    init_rmt_led(GPIO_NUM_37);
}

void board_start_display(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
    /* GPIO33/34 are the USB Serial/JTAG D-/D+ pins and also LCD data pins. */
    ESP_LOGI(TAG, "display disabled: GPIO33/34 reserved for USB Serial/JTAG");
#else
    if (!display_init()) {
        ESP_LOGW(TAG, "continuing without display");
    }
#endif
}
