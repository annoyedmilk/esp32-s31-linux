// SPDX-License-Identifier: BSD-2-Clause
// Author: Marco Müller <hello@annoyedmilk.ch>

#pragma once

/* The peripherals of the board, before and after the partitions are loaded. */
void board_init(void);
void board_start_display(void);

void init_sd_card(void);
/* The ESP-Hosted firmware: app_main() of external/esp-hosted-linux. */
void esp_hosted_main(void);
