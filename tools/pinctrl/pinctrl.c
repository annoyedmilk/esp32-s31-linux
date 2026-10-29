// SPDX-License-Identifier: BSD-3-Clause
/*
 * pinctrl for the ESP32-S31: get and set the state of the pins from the
 * registers.  The command line, the output format and the GPIO list parser
 * come from pinctrl.c of https://github.com/raspberrypi/utils.
 *
 * Copyright (c) 2023, Raspberry Pi Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the copyright holder nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "board.h"

#define MAX_NUMS	128
#define NO_CHANGE	(-1)

static const char *const program_name = "pinctrl";
static const char *const pull_names[] = { "pn", "pd", "pu", "--" };
static const char *const mode_names[] = {
	[MODE_A0] = "a0", "a1", "a2", "a3", "a4",
	[MODE_IP] = "ip", [MODE_OP] = "op", [MODE_NO] = "no", [MODE_MX] = "mx",
};

static int pin_mode, verbose_mode, force_mode;
static struct regs regs;
static struct board board;

struct poll_gpio_state {
	unsigned int num;
	unsigned int gpio;
	char name[NAME_LEN + 16];
	int level;
};

static struct poll_gpio_state poll_gpios[MAX_NUMS];
static unsigned int num_poll_gpios;

static void usage(void)
{
	const char *name = program_name;

	printf("\n");
	printf("WARNING! %s set writes directly to the IO MUX and GPIO registers\n", name);
	printf("ignoring whatever else may be using them (such as Linux drivers) -\n");
	printf("it is designed as a debug tool, only use it if you know what you\n");
	printf("are doing and at your own risk! A write can race with the\n");
	printf("read-modify-write of the kernel pin driver.\n");
	printf("\n");
	printf("Running %s with the help argument prints this help.\n", name);
	printf("%s can get and print the state of a GPIO (or all GPIOs)\n", name);
	printf("and can be used to set the function, pulls and value of a GPIO.\n");
	printf("%s must be run as root.\n", name);
	printf("Use:\n");
	printf("  %s [-p] [-v] get [GPIO]\n", name);
	printf("OR\n");
	printf("  %s [-p] [-v] [-e] [-f] set <GPIO> [options]\n", name);
	printf("OR\n");
	printf("  %s [-p] [-v] poll <GPIO>\n", name);
	printf("OR\n");
	printf("  %s [-p] [-v] funcs [GPIO]\n", name);
	printf("OR\n");
	printf("  %s [-p] [-v] lev [GPIO]\n", name);
	printf("OR\n");
	printf("  %s dump > regs.txt\n", name);
	printf("\n");
	printf("GPIO is a comma-separated list of GPIO names, numbers or ranges (without\n");
	printf("spaces), e.g. 4 or 8-19 or PA_CTRL,50-51\n");
	printf("\n");
	printf("Note that omitting [GPIO] from \"%s get\" prints all GPIOs.\n", name);
	printf("If the -p option is given, GPIO numbers are replaced by pin numbers on the\n");
	printf("board: header J2 on the Function-CoreBoard-1, the pads of the\n");
	printf("ESP32-S31-WROOM-3 module on the module alone and on the Korvo-1.\n");
	printf("If the -v option is given, the output also has the drive strength and\n");
	printf("open drain. Including the -e option in a \"set\" causes %s to echo back\n", name);
	printf("the new pin states.\n");
	printf("%s funcs will dump all the possible GPIO alt functions in CSV format\n", name);
	printf("or if [GPIO] is specified the alternate funcs just for that specific GPIO.\n");
	printf("%s dump writes the registers and the board to a snapshot, and\n", name);
	printf("--regs FILE reads the snapshot instead of the hardware.\n");
	printf("\n");
	printf("%s set refuses the flash pins, the pins in gpio-reserved-ranges of the\n", name);
	printf("device tree, and the lines that a kernel driver uses (-f overrides the\n");
	printf("last). It warns before it changes a strapping pin (36, 37, 60, 61).\n");
	printf("\n");
	printf("Valid [options] for %s set are:\n", name);
	printf("  ip      set GPIO as input\n");
	printf("  op      set GPIO as output\n");
	printf("  a0-a4   set GPIO to IO MUX function 0 to 4 (a1 is the GPIO matrix)\n");
	printf("  no      set GPIO to no function (NONE)\n");
	printf("  pu      set GPIO in-pad pull up\n");
	printf("  pd      set GPIO in-pad pull down\n");
	printf("  pn      set GPIO pull none (no pull)\n");
	printf("  dh      set GPIO to drive high (1) level (only valid if set to be an output)\n");
	printf("  dl      set GPIO to drive low (0) level (only valid if set to be an output)\n");
	printf("Examples:\n");
	printf("  %s get              Prints state of all GPIOs one per line\n", name);
	printf("  %s get 10           Prints state of GPIO10\n", name);
	printf("  %s get 10,11        Prints state of GPIO10 and GPIO11\n", name);
	printf("  %s set 47 a3        Set GPIO47 to fsel 3 function (CAM_DATA1_IN)\n", name);
	printf("  %s -e set 47 pu     Enable GPIO47 in-pad pull up, echoing the result\n", name);
	printf("  %s set 47 op        Set GPIO47 to be an output\n", name);
	printf("  %s set 47 dl        Set GPIO47 to output low/zero (must already be set as an output)\n", name);
	printf("  %s set 47 ip pd     Set GPIO47 to input with pull down\n", name);
	printf("  %s set 47 op pn dh  Set GPIO47 to output with no pull and driving high\n", name);
	printf("  %s lev 47           Prints the level (1 or 0) of GPIO47\n", name);
}

/* With -p, a number is a pin: -1 and its label for a pin without a GPIO. */
static int num_to_gpio(unsigned int num, const char **label)
{
	*label = NULL;
	if (!pin_mode)
		return pin_exists(num) ? (int)num : -2;
	return pad_gpio(board.hdr, num, label);
}

static void display_name(unsigned int gpio, char *buf, unsigned int len)
{
	gpio_name(&board, gpio, buf, len);
	if (pin_mode && strchr(buf, '/'))
		memmove(buf, strchr(buf, '/') + 1, strlen(strchr(buf, '/')));
}

static const char *level_name(int level)
{
	return level == 1 ? "hi" : level == 0 ? "lo" : "--";
}

static void print_gpio_alts_info(unsigned int num)
{
	const char *label;
	char name[NAME_LEN + 16], func[96];
	int gpio = num_to_gpio(num, &label);
	unsigned int f;

	if (gpio < 0)
		return;
	display_name(gpio, name, sizeof(name));
	printf("%u, %s", num, name);
	for (f = 0; f < NFUNCS; f++) {
		const char *raw = iomux_func_name(gpio, f);

		short_name(raw ? raw : "-", func, sizeof(func));
		printf(", %s", func);
	}
	printf("\n");
}

static int do_gpio_get(unsigned int num)
{
	const char *label;
	char name[NAME_LEN + 16];
	struct pin_state st;
	int gpio = num_to_gpio(num, &label);

	if (gpio == -1) {
		printf("%2u: %s\n", num, label);
		return 0;
	}
	if (gpio < 0)
		return 1;

	pin_get(&regs, gpio, &st);
	display_name(gpio, name, sizeof(name));
	printf("%2u: %2s %s %s | %s // %s = %s", num, mode_names[st.mode],
	       st.mode == MODE_OP ? (st.out ? "dh" : "dl") : "  ",
	       pull_names[st.pull], level_name(st.level), name, st.func);
	if (verbose_mode)
		printf(" (%umA%s)", st.drive_ma, st.open_drain ? ", open drain" : "");
	printf("\n");
	return 0;
}

/* Why set must not change this GPIO, or NULL. */
static const char *set_refusal(unsigned int gpio)
{
	if (pin_is_flash(gpio))
		return "it is a flash pin";
	if (!board.have_lines)
		return "the reserved and used lines are not known";
	if (board.reserved >> gpio & 1)
		return "it is in gpio-reserved-ranges of the device tree";
	if ((board.used >> gpio & 1) && !force_mode)
		return "a kernel driver uses it (-f changes it anyway)";
	return NULL;
}

static int do_gpio_set(unsigned int num, int fsparam, int drive, int pull)
{
	const char *label, *why;
	struct pin_state st;
	int gpio = num_to_gpio(num, &label);
	int mode;

	if (gpio == -1) {
		printf("Pin %u cannot be set\n", num);
		return 1;
	}
	if (gpio < 0)
		return 1;

	why = set_refusal(gpio);
	if (why) {
		fprintf(stderr, "GPIO%d not changed: %s\n", gpio, why);
		return 1;
	}

	pin_get(&regs, gpio, &st);
	mode = fsparam != NO_CHANGE ? fsparam : (int)st.mode;
	if (drive != NO_CHANGE && mode != MODE_OP) {
		printf("Can't set pin value, not an output\n");
		return 1;
	}
	if (pin_is_strapping(gpio))
		fprintf(stderr, "GPIO%d is a strapping pin: the next reset reads its level\n",
			gpio);

	/* The level first, so that a new output does not start at the old one. */
	if (drive != NO_CHANGE)
		pin_set_out(&regs, gpio, drive);
	if (fsparam != NO_CHANGE)
		pin_set_mode(&regs, gpio, fsparam);
	if (pull != NO_CHANGE)
		pin_set_pull(&regs, gpio, pull);
	return 0;
}

static int do_gpio_level(unsigned int num)
{
	static const struct { const char *label, *code; } codes[] = {
		{ "gnd", "G" }, { "5v", "5" }, { "3v3", "3" },
	};
	const char *label;
	unsigned int i;
	int gpio = num_to_gpio(num, &label), level;

	if (gpio == -1) {
		const char *code = "?";

		for (i = 0; i < sizeof(codes) / sizeof(codes[0]); i++)
			if (!strcmp(label, codes[i].label))
				code = codes[i].code;
		printf("%s", code);
		return 0;
	}
	if (gpio < 0)
		return 1;

	level = pin_level(&regs, gpio);
	if (level >= 0)
		printf("%d", level);
	else
		printf("-");
	return 0;
}

static int do_gpio_poll_add(unsigned int num)
{
	const char *label;
	struct poll_gpio_state *new_gpio;
	int gpio = num_to_gpio(num, &label);

	if (gpio < 0 || num_poll_gpios >= MAX_NUMS)
		return 1;

	new_gpio = &poll_gpios[num_poll_gpios++];
	new_gpio->num = num;
	new_gpio->gpio = gpio;
	gpio_name(&board, gpio, new_gpio->name, sizeof(new_gpio->name));
	new_gpio->level = -2; /* Unknown */
	return 0;
}

/* Every millisecond: the Pi tool polls without a pause, which takes a hart here. */
static void do_gpio_poll(void)
{
	unsigned int idle_count = 0, i;
	struct timeval idle_start;

	while (num_poll_gpios) {
		int changed = 0;

		for (i = 0; i < num_poll_gpios; i++) {
			struct poll_gpio_state *state = &poll_gpios[i];
			int level = pin_level(&regs, state->gpio);

			if (level == state->level)
				continue;
			if (idle_count) {
				struct timeval now;
				uint64_t interval_us;

				gettimeofday(&now, NULL);
				interval_us = (uint64_t)(now.tv_sec - idle_start.tv_sec) * 1000000 +
					      (now.tv_usec - idle_start.tv_usec);
				printf("+%" PRIu64 "us\n", interval_us);
				idle_count = 0;
			}
			printf("%2u: %s // %s\n", state->num, level_name(level), state->name);
			state->level = level;
			changed = 1;
			fflush(stdout);
		}
		if (!changed) {
			if (!idle_count)
				gettimeofday(&idle_start, NULL);
			idle_count++;
		}
		usleep(1000);
	}
}

static int open_registers(const char *regs_file)
{
	int ret;

	if (regs_file) {
		ret = snapshot_load(regs_file, &regs, &board);
		if (ret)
			printf("Failed to read %s - %s\n", regs_file, strerror(-ret));
		return ret;
	}

	ret = live_open(&regs);
	if (ret == -EACCES && geteuid())
		printf("Must be root\n");
	else if (ret == -ENOENT)
		printf("No /dev/esp32s31-gpiomem: the kernel needs CONFIG_PINCTRL_ESP32S31_GPIOMEM\n");
	else if (ret == -ENOSYS)
		printf("This build has no hardware access: use --regs FILE\n");
	else if (ret)
		printf("Failed to map the registers - %s\n", strerror(-ret));
	if (ret)
		return ret;

	/* Without the lines, get shows no names and set refuses all pins. */
	ret = live_board(&board);
	if (ret)
		printf("Cannot read the GPIO lines of the pin controller - %s\n", strerror(-ret));
	return 0;
}

/* One item of a GPIO list: a number, or a name that the board knows. */
static int parse_item(const char *p, unsigned int len, unsigned int max)
{
	unsigned int num;
	int used;

	if (sscanf(p, "%u%n", &num, &used) == 1 && (unsigned int)used == len)
		return num < max ? (int)num : -1;

	num = gpio_by_name(&board, p, len);
	if ((int)num < 0)
		return -1;
	if (pin_mode) {
		int pad = gpio_pad(board.hdr, num);

		if (pad < 0) {
			printf("Signal \"%.*s\" is not on a header pin\n", (int)len, p);
			exit(1);
		}
		return pad;
	}
	return num;
}

/* "4", "8-19", "PA_CTRL,50-51": set the bits of mask; NULL when done. */
static const char *parse_list(const char *p, uint64_t *mask, unsigned int max)
{
	while (p) {
		unsigned int len = strcspn(p, "-,");
		int first = parse_item(p, len, max), last;

		if (first < 0)
			return p;
		p += len;
		last = first;
		if (*p == '-') {
			p++;
			len = strcspn(p, "-,");
			last = parse_item(p, len, max);
			if (last < 0)
				return p;
			p += len;
		}
		if (last < first) {
			int tmp = last;

			last = first;
			first = tmp;
		}
		for (; first <= last; first++)
			mask[first / 64] |= 1ull << (first % 64);
		if (*p == '\0')
			return NULL;
		if (*p != ',')
			return p;
		p++;
	}
	return NULL;
}

enum set_param { SET_DRIVE, SET_FSEL, SET_PULL };

static int parse_set_arg(const char *arg, int *params)
{
	static const struct { const char *arg; enum set_param param; int val; } args[] = {
		{ "dh", SET_DRIVE, 1 }, { "dl", SET_DRIVE, 0 },
		{ "ip", SET_FSEL, MODE_IP }, { "op", SET_FSEL, MODE_OP },
		{ "no", SET_FSEL, MODE_NO }, { "a0", SET_FSEL, MODE_A0 },
		{ "a1", SET_FSEL, MODE_A0 + 1 }, { "a2", SET_FSEL, MODE_A0 + 2 },
		{ "a3", SET_FSEL, MODE_A0 + 3 }, { "a4", SET_FSEL, MODE_A0 + 4 },
		{ "pu", SET_PULL, PULL_UP }, { "pd", SET_PULL, PULL_DOWN },
		{ "pn", SET_PULL, PULL_NONE },
	};
	unsigned int i;

	for (i = 0; i < sizeof(args) / sizeof(args[0]); i++) {
		if (!strcmp(arg, args[i].arg)) {
			params[args[i].param] = args[i].val;
			return 0;
		}
	}
	printf("Unknown argument \"%s\"\n", arg);
	return 1;
}

int main(int argc, char *argv[])
{
	static const char *const cmds[] = { "get", "set", "lev", "level", "poll", "funcs", "dump" };
	const char *regs_file = NULL, *cmd = NULL, *list, *p;
	int set, get, level, poll, funcs, dump, echo = 0;
	int params[] = { [SET_DRIVE] = NO_CHANGE, [SET_FSEL] = NO_CHANGE, [SET_PULL] = NO_CHANGE };
	unsigned int i;
	uint64_t mask[MAX_NUMS / 64] = { 0 };
	unsigned int num, first, last;
	int ret = 0, first_pin = 1, cmd_inferred;

	argv++;
	argc--;
	while (argc && argv[0][0] == '-') {
		const char *arg = *(argv++);

		argc--;
		if (!strcmp(arg, "-e")) {
			echo = 1;
		} else if (!strcmp(arg, "-f")) {
			force_mode = 1;
		} else if (!strcmp(arg, "-h")) {
			usage();
			return 0;
		} else if (!strcmp(arg, "-p")) {
			pin_mode = 1;
		} else if (!strcmp(arg, "-v")) {
			verbose_mode = 1;
		} else if (!strcmp(arg, "--regs") && argc) {
			regs_file = *(argv++);
			argc--;
		} else {
			printf("Unknown option '%s' - try \"%s help\"\n", arg, program_name);
			return 1;
		}
	}

	if (argc && !strcmp(argv[0], "help")) {
		usage();
		return 0;
	}
	if (open_registers(regs_file))
		return 1;

	/* As the Pi tool: without a command, a GPIO list and options mean set. */
	for (i = 0; argc && i < sizeof(cmds) / sizeof(cmds[0]); i++)
		if (!strcmp(argv[0], cmds[i]))
			cmd = cmds[i];
	cmd_inferred = !cmd;
	if (cmd) {
		argv++;
		argc--;
	} else {
		cmd = argc > 1 ? "set" : "get";
	}
	get = !strcmp(cmd, "get");
	set = !strcmp(cmd, "set");
	level = !strcmp(cmd, "level") || !strcmp(cmd, "lev");
	poll = !strcmp(cmd, "poll");
	funcs = !strcmp(cmd, "funcs");
	dump = !strcmp(cmd, "dump");
	if (!get && !set && !level && !poll && !funcs && !dump) {
		printf("Unknown command \"%s\"\n", cmd);
		return 1;
	}
	if (dump) {
		snapshot_dump(stdout, &regs, &board);
		return 0;
	}

	if (pin_mode && !board.hdr) {
		printf("No PIN numbers known for this board - pin mode disabled\n");
		pin_mode = 0;
	}
	first = pin_mode ? 1 : 0;
	last = pin_mode ? header_npads(board.hdr) : NPINS - 1;

	if (argc) {
		list = *(argv++);
		argc--;
		p = parse_list(list, mask, last + 1);
		if (p) {
			printf("Unknown %s \"%s\"\n",
			       p == list && cmd_inferred ? "command" : "GPIO", p);
			return 1;
		}
	} else if (set || poll) {
		printf("Need GPIO number to %s\n", set ? "set" : "poll");
		return 1;
	}

	if (set && !argc) {
		printf("Need a function or pull to set\n");
		return 1;
	}
	if ((get || funcs || level) && argc) {
		printf("Too many arguments\n");
		return 1;
	}
	while (argc--)
		if (parse_set_arg(*(argv++), params))
			return 1;

	if (!mask[0] && !mask[1])
		memset(mask, 0xff, sizeof(mask));

	for (num = first; num <= last; num++) {
		if (!(mask[num / 64] >> (num % 64) & 1))
			continue;
		if (get)
			do_gpio_get(num);
		if (set)
			ret |= do_gpio_set(num, params[SET_FSEL], params[SET_DRIVE],
					   params[SET_PULL]);
		if (level) {
			const char *label;

			if (num_to_gpio(num, &label) == -2)
				continue;
			if (!first_pin)
				printf(" ");
			do_gpio_level(num);
			first_pin = 0;
		}
		if (poll)
			do_gpio_poll_add(num);
		if (funcs)
			print_gpio_alts_info(num);
	}
	if (level)
		printf("\n");

	if (set && echo)
		for (num = first; num <= last; num++)
			if (mask[num / 64] >> (num % 64) & 1)
				do_gpio_get(num);

	if (poll)
		do_gpio_poll();
	return ret;
}
