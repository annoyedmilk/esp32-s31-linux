/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Author: Marco Mueller <hello@annoyedmilk.ch>
 *
 * Pin functions of the Espressif ESP32-S31 for the "pinmux" property.
 *
 * Each cell routes one signal of one pin: the pin in bits 31:16, the IO MUX
 * function in bits 13:11, the direction in bits 10:9 and the GPIO matrix
 * signal in bits 8:0.  A pin with two signals, for example MDIO with an
 * input and an output signal, has two cells.
 */

#ifndef _DT_BINDINGS_PINCTRL_ESP32S31_H
#define _DT_BINDINGS_PINCTRL_ESP32S31_H

#define ESP32S31_PIN_IN			(1 << 9)
#define ESP32S31_PIN_OUT		(1 << 10)

#define ESP32S31_PINMUX(pin, func, dir, sig) \
	(((pin) << 16) | ((func) << 11) | (dir) | (sig))

/* A peripheral connected directly through the IO MUX. */
#define ESP32S31_IOMUX(pin, func)	ESP32S31_PINMUX(pin, func, 0, 0)
#define ESP32S31_IOMUX_IN(pin, func, sig) \
	ESP32S31_PINMUX(pin, func, ESP32S31_PIN_IN, sig)

/* A peripheral signal through the GPIO matrix (IO MUX function 1). */
#define ESP32S31_MATRIX_IN(pin, sig) \
	ESP32S31_PINMUX(pin, 1, ESP32S31_PIN_IN, sig)
#define ESP32S31_MATRIX_OUT(pin, sig) \
	ESP32S31_PINMUX(pin, 1, ESP32S31_PIN_OUT, sig)
#define ESP32S31_MATRIX_INOUT(pin, sig) \
	ESP32S31_PINMUX(pin, 1, ESP32S31_PIN_IN | ESP32S31_PIN_OUT, sig)

/* GPIO matrix signals */
#define ESP32S31_SIG_I2C0_SCL		68
#define ESP32S31_SIG_I2C0_SDA		69
#define ESP32S31_SIG_I2C1_SCL		70
#define ESP32S31_SIG_I2C1_SDA		71
#define ESP32S31_SIG_GMII_MDI		107
#define ESP32S31_SIG_GMII_MDC		108
#define ESP32S31_SIG_GMII_MDO		109
#define ESP32S31_SIG_GMAC_RXDV		178
#define ESP32S31_SIG_GMAC_RXD0		179
#define ESP32S31_SIG_GMAC_RXD1		180
#define ESP32S31_SIG_GMAC_RXD2		181
#define ESP32S31_SIG_GMAC_RXD3		182
#define ESP32S31_SIG_GMAC_RX_CLK	184
#define ESP32S31_SIG_RMT_OUT0		246

#endif
