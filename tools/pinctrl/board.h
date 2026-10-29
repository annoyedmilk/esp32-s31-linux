/* SPDX-License-Identifier: MIT */
/*
 * What the tool knows about the board: the line names and the reserved pins
 * of the device tree, the lines in use, and the header for -p.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>
#include <stdio.h>

#include "esp32s31.h"

#define NAME_LEN	32

/* A run of header pins: GPIOs from gpio on, or pins without one (label). */
struct pad_range {
	unsigned int first, last;
	int gpio;
	const char *label;
};

struct header {
	const char *compatible;
	const struct pad_range *ranges;
	unsigned int nranges;
};

struct board {
	char compatible[64];
	char model[96];
	char names[NPINS][NAME_LEN];
	uint64_t reserved;	/* gpio-reserved-ranges */
	uint64_t used;		/* GPIO_V2_LINE_FLAG_USED */
	int have_lines;		/* reserved and used are known */
	const struct header *hdr;
};

/* The registers and the board, from the hardware or from a snapshot. */
int live_open(struct regs *r);
int live_board(struct board *b);
int snapshot_load(const char *path, struct regs *r, struct board *b);
void snapshot_dump(FILE *f, struct regs *r, const struct board *b);

void copy_str(char *dst, unsigned int len, const char *src);
void board_init(struct board *b);
const struct header *board_header(const char *compatible);
unsigned int header_npads(const struct header *hdr);
int pad_gpio(const struct header *hdr, unsigned int pad, const char **label);
int gpio_pad(const struct header *hdr, unsigned int gpio);
void gpio_name(const struct board *b, unsigned int gpio, char *buf, unsigned int len);
int gpio_by_name(const struct board *b, const char *name, unsigned int len);

#endif
