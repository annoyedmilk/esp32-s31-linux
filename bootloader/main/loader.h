// SPDX-License-Identifier: BSD-2-Clause
// Author: Marco Müller <hello@annoyedmilk.ch>

#pragma once

/* The display of the board, after the partitions are loaded. */
void board_start_display(void);

/* The ESP-Hosted firmware: app_main() of external/esp-hosted-linux. */
void esp_hosted_main(void);
