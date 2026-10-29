/* SPDX-License-Identifier: MIT */
/*
 * The pin blocks of the ESP32-S31, as pinctrl-esp32s31.c in the kernel uses
 * them: the IO MUX page, the GPIO page (with the GPIO matrix) and the CNNT
 * select word.
 */
#ifndef ESP32S31_H
#define ESP32S31_H

#include <stdint.h>

#define NPINS		62
#define NSIGS		256
#define NFUNCS		5
#define PAGE_WORDS	1024

#define IOMUX_WPD	(1u << 7)
#define IOMUX_WPU	(1u << 8)
#define IOMUX_IE	(1u << 9)
#define IOMUX_DRV_SHIFT	10
#define IOMUX_DRV_MASK	(3u << IOMUX_DRV_SHIFT)
#define IOMUX_MCU_SHIFT	12
#define IOMUX_MCU_MASK	(7u << IOMUX_MCU_SHIFT)
#define FUNC_GPIO	1

/* Each of OUT, OUT1, ENABLE and ENABLE1 has W1TS at +4 and W1TC at +8. */
#define GPIO_OUT	0x04
#define GPIO_OUT1	0x10
#define GPIO_ENABLE	0x34
#define GPIO_ENABLE1	0x40
#define GPIO_W1TS	4
#define GPIO_W1TC	8
#define GPIO_IN		0x64
#define GPIO_IN1	0x68
#define GPIO_PIN(n)	(0xf4 + 4 * (n))
#define GPIO_IN_SEL(s)	(0x2f4 + 4 * (s))
#define GPIO_OUT_SEL(n)	(0xaf4 + 4 * (n))

#define PIN_PAD_DRIVER	(1u << 2)
#define IN_SEL_PIN	0xffu
#define IN_SEL_MATRIX	(1u << 9)
#define OUT_SEL_SIG	0x1ffu
#define SIG_GPIO_OUT	256

/* The registers: two pages and the CNNT word, mapped or from a snapshot. */
struct regs {
	volatile uint32_t *iomux;
	volatile uint32_t *gpio;
	/* Set (W1TS) or clear (W1TC) the bits of mask in the register at base. */
	void (*w1)(struct regs *r, unsigned int base, uint32_t mask, int set);
	uint32_t (*cnnt_read)(struct regs *r);
	int (*cnnt_write)(struct regs *r, uint32_t val);
	int fd;
	uint32_t cnnt;
};

/* The modes of the "MM" column; a0 to a4 are the IO MUX functions. */
enum mode {
	MODE_A0,
	MODE_A4 = MODE_A0 + NFUNCS - 1,
	MODE_IP,
	MODE_OP,
	MODE_NO,
	MODE_MX,
};

enum pull { PULL_NONE, PULL_DOWN, PULL_UP, PULL_BOTH };

struct pin_state {
	enum mode mode;
	enum pull pull;
	int out;		/* the OUT bit */
	int level;		/* 1, 0, or -1 with the input off */
	unsigned int drive_ma;
	int open_drain;
	char func[128];
};

extern const char *const sig_in_names[NSIGS];
extern const char *const sig_out_names[NSIGS];

int pin_exists(unsigned int pin);
int pin_is_flash(unsigned int pin);
int pin_is_strapping(unsigned int pin);
const char *iomux_func_name(unsigned int pin, unsigned int func);
void short_name(const char *raw, char *buf, unsigned int len);

void pin_get(struct regs *r, unsigned int pin, struct pin_state *st);
int pin_level(struct regs *r, unsigned int pin);
void pin_set_mode(struct regs *r, unsigned int pin, enum mode mode);
void pin_set_pull(struct regs *r, unsigned int pin, enum pull pull);
void pin_set_out(struct regs *r, unsigned int pin, int high);

#endif
