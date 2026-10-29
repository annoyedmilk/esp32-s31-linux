// SPDX-License-Identifier: MIT
/*
 * The headers for -p, the pin names, and register snapshots for tests
 * without hardware.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"

/* The pads of the ESP32-S31-WROOM-3 module. */
static const struct pad_range wroom3[] = {
	{ 1, 2, -1, "gnd" },
	{ 3, 4, -1, "3v3" },
	{ 5, 5, -1, "en" },
	{ 6, 6, 2, NULL },
	{ 7, 7, 3, NULL },
	{ 8, 8, 0, NULL },
	{ 9, 9, 1, NULL },
	{ 10, 25, 4, NULL },
	{ 26, 26, -1, "gnd" },
	{ 27, 32, 20, NULL },
	{ 33, 39, -1, "nc" },
	{ 40, 40, -1, "usb_dp" },
	{ 41, 41, -1, "usb_dm" },
	{ 42, 46, 33, NULL },
	{ 47, 48, -1, "gnd" },
	{ 49, 51, 38, NULL },
	{ 52, 67, 42, NULL },
	{ 68, 71, 58, NULL },
	{ 72, 90, -1, "gnd" },
	{ 91, 91, -1, "nc" },
	{ 92, 94, -1, "gnd" },
	{ 95, 95, -1, "epad" },
	{ 96, 99, -1, "gnd" },
};

/* Header J2 of the ESP32-S31-Function-CoreBoard-1. */
static const struct pad_range j2[] = {
	{ 1, 2, -1, "gnd" },
	{ 3, 3, 58, NULL },
	{ 4, 4, 59, NULL },
	{ 5, 5, 61, NULL },
	{ 6, 6, 60, NULL },
	{ 7, 7, 2, NULL },
	{ 8, 8, -1, "gnd" },
	{ 9, 9, 0, NULL },
	{ 10, 10, 3, NULL },
	{ 11, 11, 49, NULL },
	{ 12, 12, 1, NULL },
	{ 13, 13, 47, NULL },
	{ 14, 14, 48, NULL },
	{ 15, 15, 45, NULL },
	{ 16, 16, 46, NULL },
	{ 17, 17, 43, NULL },
	{ 18, 18, 44, NULL },
	{ 19, 19, 40, NULL },
	{ 20, 20, 42, NULL },
	{ 21, 21, 39, NULL },
	{ 22, 22, 38, NULL },
	{ 23, 23, 37, NULL },
	{ 24, 24, 36, NULL },
	{ 25, 25, 35, NULL },
	{ 26, 31, 20, NULL },
	{ 32, 32, 4, NULL },
	{ 33, 34, -1, "gnd" },
	{ 35, 36, -1, "3v3" },
	{ 37, 38, -1, "gnd" },
	{ 39, 40, -1, "5v" },
};

#define HEADER(c, t) { c, t, sizeof(t) / sizeof(t[0]) }

static const struct header headers[] = {
	HEADER("esp,esp32s31-function-coreboard-1", j2),
	HEADER("esp,esp32s31-korvo-1", wroom3),
	HEADER("esp,esp32s31-wroom-3", wroom3),
};

const struct header *board_header(const char *compatible)
{
	unsigned int i;

	for (i = 0; i < sizeof(headers) / sizeof(headers[0]); i++)
		if (!strcmp(headers[i].compatible, compatible))
			return &headers[i];
	return NULL;
}

/* Copy and cut src to len - 1 bytes. */
void copy_str(char *dst, unsigned int len, const char *src)
{
	unsigned int n = strlen(src);

	if (n >= len)
		n = len - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

void board_init(struct board *b)
{
	memset(b, 0, sizeof(*b));
}

unsigned int header_npads(const struct header *hdr)
{
	return hdr->ranges[hdr->nranges - 1].last;
}

/* The GPIO of a pad, or -1 and its label; -2 for no such pad. */
int pad_gpio(const struct header *hdr, unsigned int pad, const char **label)
{
	unsigned int i;

	for (i = 0; hdr && i < hdr->nranges; i++) {
		const struct pad_range *pr = &hdr->ranges[i];

		if (pad < pr->first || pad > pr->last)
			continue;
		*label = pr->label;
		return pr->gpio < 0 ? -1 : pr->gpio + (int)(pad - pr->first);
	}
	return -2;
}

int gpio_pad(const struct header *hdr, unsigned int gpio)
{
	const char *label;
	unsigned int pad;

	for (pad = 1; hdr && pad <= header_npads(hdr); pad++)
		if (pad_gpio(hdr, pad, &label) == (int)gpio)
			return pad;
	return -1;
}

/* "NAME/GPIOn", or "GPIOn" for a line without a name. */
void gpio_name(const struct board *b, unsigned int gpio, char *buf, unsigned int len)
{
	if (b->names[gpio][0])
		snprintf(buf, len, "%s/GPIO%u", b->names[gpio], gpio);
	else
		snprintf(buf, len, "GPIO%u", gpio);
}

/* As the Pi tool: each part of "NAME/GPIOn" names the GPIO. */
int gpio_by_name(const struct board *b, const char *name, unsigned int len)
{
	char full[NAME_LEN + 16];
	unsigned int gpio;

	for (gpio = 0; gpio < NPINS; gpio++) {
		const char *p;

		if (!pin_exists(gpio))
			continue;
		gpio_name(b, gpio, full, sizeof(full));
		for (p = full; *p;) {
			unsigned int part = strcspn(p, "/");

			if (part == len && !memcmp(p, name, len))
				return gpio;
			p += part;
			p += *p == '/';
		}
	}
	return -1;
}

/* The snapshot backend keeps both pages in memory. */
static uint32_t snap_pages[2][PAGE_WORDS];

static void snap_w1(struct regs *r, unsigned int base, uint32_t mask, int set)
{
	uint32_t *reg = (uint32_t *)&r->gpio[base / 4];

	*reg = set ? *reg | mask : *reg & ~mask;
}

static uint32_t snap_cnnt_read(struct regs *r)
{
	return r->cnnt;
}

static int snap_cnnt_write(struct regs *r, uint32_t val)
{
	r->cnnt = val;
	return 0;
}

static int load_words(volatile uint32_t *page, const char *args)
{
	unsigned int off, n = 0;
	int used;

	if (sscanf(args, "%x%n", &off, &used) != 1 || off % 32 || off / 4 + 8 > PAGE_WORDS)
		return -1;
	for (args += used; n < 8; n++, args += used)
		if (sscanf(args, "%" SCNx32 "%n", (uint32_t *)&page[off / 4 + n], &used) != 1)
			return -1;
	return 0;
}

static void load_names(struct board *b, char *list)
{
	unsigned int gpio;

	for (gpio = 0; gpio < NPINS && list; gpio++) {
		char *end = strchr(list, ',');

		if (end)
			*end++ = '\0';
		copy_str(b->names[gpio], NAME_LEN, list);
		list = end;
	}
}

static int load_line(char *line, struct regs *r, struct board *b)
{
	char *args = line + strcspn(line, " ");

	args += *args == ' ';
	line[strcspn(line, " ")] = '\0';
	if (!strcmp(line, "compatible"))
		copy_str(b->compatible, sizeof(b->compatible), args);
	else if (!strcmp(line, "model"))
		copy_str(b->model, sizeof(b->model), args);
	else if (!strcmp(line, "names"))
		load_names(b, args);
	else if (!strcmp(line, "reserved"))
		b->reserved = strtoull(args, NULL, 16);
	else if (!strcmp(line, "used"))
		b->used = strtoull(args, NULL, 16);
	else if (!strcmp(line, "cnnt"))
		r->cnnt = strtoul(args, NULL, 16);
	else if (!strcmp(line, "iomux"))
		return load_words(r->iomux, args);
	else if (!strcmp(line, "gpio"))
		return load_words(r->gpio, args);
	else if (line[0] != '#' && line[0])
		return -1;
	return 0;
}

int snapshot_load(const char *path, struct regs *r, struct board *b)
{
	char line[NPINS * NAME_LEN + 16];	/* the names line is the longest */
	unsigned int lineno = 0;
	FILE *f = fopen(path, "r");

	if (!f)
		return -errno;

	r->iomux = snap_pages[0];
	r->gpio = snap_pages[1];
	r->w1 = snap_w1;
	r->cnnt_read = snap_cnnt_read;
	r->cnnt_write = snap_cnnt_write;
	r->fd = -1;
	while (fgets(line, sizeof(line), f)) {
		lineno++;
		line[strcspn(line, "\n")] = '\0';
		if (load_line(line, r, b)) {
			fprintf(stderr, "%s:%u: not a snapshot line\n", path, lineno);
			fclose(f);
			return -EINVAL;
		}
	}
	fclose(f);
	b->have_lines = 1;
	b->hdr = board_header(b->compatible);
	return 0;
}

static void dump_page(FILE *f, const char *name, volatile uint32_t *page)
{
	unsigned int off, i;

	for (off = 0; off < PAGE_WORDS * 4; off += 32) {
		fprintf(f, "%s %03x", name, off);
		for (i = 0; i < 8; i++)
			fprintf(f, " %08" PRIx32, page[off / 4 + i]);
		fprintf(f, "\n");
	}
}

void snapshot_dump(FILE *f, struct regs *r, const struct board *b)
{
	unsigned int gpio;

	fprintf(f, "# esp32s31-pinctrl snapshot 1\n");
	fprintf(f, "compatible %s\nmodel %s\nnames ", b->compatible, b->model);
	for (gpio = 0; gpio < NPINS; gpio++)
		fprintf(f, "%s%s", gpio ? "," : "", b->names[gpio]);
	fprintf(f, "\nreserved %016" PRIx64 "\nused %016" PRIx64 "\n", b->reserved, b->used);
	fprintf(f, "cnnt %08" PRIx32 "\n", r->cnnt_read(r));
	dump_page(f, "iomux", r->iomux);
	dump_page(f, "gpio", r->gpio);
}
