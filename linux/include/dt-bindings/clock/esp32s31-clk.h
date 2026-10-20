/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Author: Marco Mueller <hello@annoyedmilk.ch>
 *
 * Clocks and resets of the Espressif ESP32-S31.
 */

#ifndef _DT_BINDINGS_CLOCK_ESP32S31_H
#define _DT_BINDINGS_CLOCK_ESP32S31_H

/* LP_AONCLKRST */
#define ESP32S31_LP_CLK_MPLL		0

/* HP_SYS_CLKRST */
#define ESP32S31_CLK_CPU		0
#define ESP32S31_CLK_SYS		1
#define ESP32S31_CLK_APB		2
#define ESP32S31_CLK_EMAC_SYS		3

/* CNNT_SYS */
#define ESP32S31_CNNT_CLK_EMAC_REF	0

#define ESP32S31_CNNT_RST_EMAC		0

#endif
