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
#define ESP32S31_CLK_I2C0_APB		4
#define ESP32S31_CLK_I2C0		5
#define ESP32S31_CLK_I2C1_APB		6
#define ESP32S31_CLK_I2C1		7
#define ESP32S31_CLK_RMT_SYS		8
#define ESP32S31_CLK_RMT		9
#define ESP32S31_CLK_SDMMC_SYS		10
#define ESP32S31_CLK_SDIO_LS		11

#define ESP32S31_RST_I2C0		0
#define ESP32S31_RST_I2C1		1
#define ESP32S31_RST_RMT		2

/* CNNT_SYS */
#define ESP32S31_CNNT_CLK_EMAC_REF	0

#define ESP32S31_CNNT_RST_EMAC		0
#define ESP32S31_CNNT_RST_SDMMC		1

#endif
