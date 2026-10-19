// SPDX-License-Identifier: BSD-2-Clause
// Author: Marco Müller <hello@annoyedmilk.ch>

#pragma once

#include "driver/gpio.h"

/* The peripherals of the board, before and after the partitions are loaded. */
void board_init(void);
void board_start_display(void);

void init_rmt_led(gpio_num_t gpio);
void init_sd_card(void);
void init_emac(void);
/* The ESP-Hosted firmware: app_main() of external/esp-hosted-linux. */
void esp_hosted_main(void);
