// SPDX-License-Identifier: BSD-2-Clause
// Author: Roan Brand <brandroan@gmail.com>

#include "loader.h"

/* ESP32-S31-Function-CoreBoard-1: the WS2812 LED.  Linux starts USB itself. */

void board_init(void)
{
    init_rmt_led(GPIO_NUM_60);
}

void board_start_display(void)
{
}
