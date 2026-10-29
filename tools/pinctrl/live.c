// SPDX-License-Identifier: MIT
/*
 * The registers through /dev/esp32s31-gpiomem, and the board from the
 * device tree and the GPIO character device.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "board.h"

#define GPIOMEM		"/dev/esp32s31-gpiomem"
#define GPIOMEM_CNNT	0x2000
#define PINCTRL_COMPAT	"esp,esp32s31-pinctrl"

static void live_w1(struct regs *r, unsigned int base, uint32_t mask, int set)
{
	r->gpio[(base + (set ? GPIO_W1TS : GPIO_W1TC)) / 4] = mask;
}

static uint32_t live_cnnt_read(struct regs *r)
{
	uint32_t val = 0;

	if (pread(r->fd, &val, sizeof(val), GPIOMEM_CNNT) != sizeof(val))
		fprintf(stderr, "pinctrl: cannot read the CNNT word: %s\n", strerror(errno));
	return val;
}

static int live_cnnt_write(struct regs *r, uint32_t val)
{
	if (pwrite(r->fd, &val, sizeof(val), GPIOMEM_CNNT) == sizeof(val))
		return 0;
	fprintf(stderr, "pinctrl: cannot write the CNNT word: %s\n", strerror(errno));
	return -errno;
}

static volatile uint32_t *map_page(int fd, off_t off)
{
	void *p = mmap(NULL, PAGE_WORDS * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);

	return p == MAP_FAILED ? NULL : p;
}

int live_open(struct regs *r)
{
	r->fd = open(GPIOMEM, O_RDWR | O_SYNC | O_CLOEXEC);
	if (r->fd < 0)
		return -errno;
	r->iomux = map_page(r->fd, 0);
	r->gpio = map_page(r->fd, PAGE_WORDS * 4);
	if (!r->iomux || !r->gpio)
		return -errno;
	r->w1 = live_w1;
	r->cnnt_read = live_cnnt_read;
	r->cnnt_write = live_cnnt_write;
	return 0;
}

static ssize_t read_file(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n >= 0)
		buf[n] = '\0';
	return n;
}

/* gpio-line-names: NUL-separated strings in line order. */
static void read_names(struct board *b, const char *node)
{
	char path[256], buf[4096];
	ssize_t n;
	unsigned int gpio = 0;
	const char *p;

	snprintf(path, sizeof(path), "%s/gpio-line-names", node);
	n = read_file(path, buf, sizeof(buf));
	for (p = buf; n > 0 && p < buf + n && gpio < NPINS; p += strlen(p) + 1)
		copy_str(b->names[gpio++], NAME_LEN, p);
}

/* gpio-reserved-ranges: pairs of big-endian cells, first and count. */
static void read_reserved(struct board *b, const char *node)
{
	unsigned char buf[256];
	char path[256];
	ssize_t n, i;

	snprintf(path, sizeof(path), "%s/gpio-reserved-ranges", node);
	n = read_file(path, (char *)buf, sizeof(buf));
	for (i = 0; i + 8 <= n; i += 8) {
		uint32_t first = buf[i] << 24 | buf[i + 1] << 16 | buf[i + 2] << 8 | buf[i + 3];
		uint32_t count = buf[i + 4] << 24 | buf[i + 5] << 16 | buf[i + 6] << 8 | buf[i + 7];

		for (; count && first < NPINS; count--, first++)
			b->reserved |= 1ull << first;
	}
}

static int read_used(struct board *b, const char *chip)
{
	char path[80];
	unsigned int gpio;
	int fd;

	snprintf(path, sizeof(path), "/dev/%.*s", 64, chip);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	for (gpio = 0; gpio < NPINS; gpio++) {
		struct gpio_v2_line_info info = { .offset = gpio };

		if (ioctl(fd, GPIO_V2_GET_LINEINFO_IOCTL, &info) == 0 &&
		    (info.flags & GPIO_V2_LINE_FLAG_USED))
			b->used |= 1ull << gpio;
	}
	close(fd);
	return 0;
}

/* The gpiochip whose device tree node is the pin controller. */
static int find_chip(char *chip, size_t len)
{
	DIR *d = opendir("/sys/bus/gpio/devices");
	struct dirent *e;
	int found = 0;

	while (d && !found && (e = readdir(d))) {
		char path[300], compat[128];

		snprintf(path, sizeof(path), "/sys/bus/gpio/devices/%s/of_node/compatible",
			 e->d_name);
		if (read_file(path, compat, sizeof(compat)) > 0 && !strcmp(compat, PINCTRL_COMPAT)) {
			copy_str(chip, len, e->d_name);
			found = 1;
		}
	}
	if (d)
		closedir(d);
	return found ? 0 : -ENOENT;
}

int live_board(struct board *b)
{
	char chip[64], node[128];

	read_file("/proc/device-tree/compatible", b->compatible, sizeof(b->compatible));
	read_file("/proc/device-tree/model", b->model, sizeof(b->model));
	b->hdr = board_header(b->compatible);
	if (find_chip(chip, sizeof(chip)))
		return -ENOENT;

	snprintf(node, sizeof(node), "/sys/bus/gpio/devices/%s/of_node", chip);
	read_names(b, node);
	read_reserved(b, node);
	if (read_used(b, chip))
		return -errno;
	b->have_lines = 1;
	return 0;
}
