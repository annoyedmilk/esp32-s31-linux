// SPDX-License-Identifier: MIT
#include "platform.h"
#include "tests.h"
#include "ui.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/kd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;

static void handle_signal(int number)
{
	interrupted = number;
}

static uint32_t tick_ms(void)
{
	return monotonic_ms();
}

static int run_ui(int framebuffer, const char *snapshot, enum page page)
{
	struct inputs inputs = { .touch_fd = -1, .key_fd = -1 };
	struct display display;
	struct tests tests;
	struct ui ui;
	lv_indev_t *pointer = NULL;
	int result;

	for (unsigned int i = 0; i < USB_INPUTS; i++)
		inputs.usb_fd[i] = -1;
	lv_init();
	lv_tick_set_cb(tick_ms);
	result = display_init(&display, framebuffer, snapshot != NULL);
	if (result) {
		fprintf(stderr, "Initialize display: %s\n", strerror(-result));
		return 1;
	}
	if (!snapshot) {
		inputs_open(&inputs);
		pointer = lv_indev_create();
		if (!pointer) {
			result = -ENOMEM;
			goto cleanup_display;
		}
		lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
		lv_indev_set_user_data(pointer, &inputs);
		lv_indev_set_read_cb(pointer, inputs_read);
		lv_indev_set_display(pointer, display.lv);
	}
	tests_init(&tests, &inputs, framebuffer, snapshot != NULL);
	ui_init(&ui, &tests, page);
	printf("Mosaico test ready; tests run on demand.\n");
	fflush(stdout);
	do {
		bool ai_pressed = !snapshot && inputs_poll(&inputs);

		tests_poll(&tests, monotonic_ms(), ai_pressed);
		ui_refresh(&ui);
		lv_timer_handler();
		if (ui.camera_pending && !snapshot) {
			/* Finish LVGL's initial opaque repaint before the child owns the viewport. */
			lv_refr_now(display.lv);
			tests_action(&tests, ACTION_PRIMARY);
			ui.camera_pending = false;
		}
		if (snapshot) {
			lv_refr_now(display.lv);
			result = display_snapshot(&display, snapshot);
			break;
		}
		if (display.error) {
			result = display.error;
			break;
		}
		usleep(10000);
	} while (!ui.quit && !interrupted);
	tests_close(&tests);
	if (pointer)
		lv_indev_delete(pointer);
cleanup_display:
	inputs_close(&inputs);
	display_destroy(&display);
	lv_deinit();
	if (result)
		fprintf(stderr, "Mosaico test stopped: %s\n", strerror(-result));
	return result ? 1 : 0;
}

static int supervise(int framebuffer, enum page page)
{
	int console, display_mode, keyboard_mode, status = 0;
	pid_t child, parent = getpid();
	bool keyboard_changed = false;

	console = open("/dev/tty0", O_RDWR | O_CLOEXEC);
	if (console < 0 || ioctl(console, KDGETMODE, &display_mode) ||
	    ioctl(console, KDGKBMODE, &keyboard_mode)) {
		perror("query display console");
		if (console >= 0)
			close(console);
		return 1;
	}
	if (display_mode != KD_TEXT) {
		fprintf(stderr, "The display console is already in graphics mode.\n");
		close(console);
		return 1;
	}
	if (ioctl(console, KDSETMODE, KD_GRAPHICS)) {
		perror("take display console");
		close(console);
		return 1;
	}
	if (!ioctl(console, KDSKBMODE, K_OFF))
		keyboard_changed = true;
	child = fork();
	if (!child) {
		prctl(PR_SET_PDEATHSIG, SIGTERM);
		if (getppid() != parent)
			_exit(1);
		setpgid(0, 0);
		close(console);
		_exit(run_ui(framebuffer, NULL, page));
	}
	if (child < 0) {
		perror("start GUI");
		status = 1;
	} else {
		setpgid(child, child);
		if (interrupted)
			kill(-child, interrupted);
		/* Keep VT ownership out of the GUI: even an OOM kill restores text. */
		while (waitpid(child, &status, 0) < 0) {
			if (errno != EINTR) {
				status = 1;
				break;
			}
			if (interrupted)
				kill(-child, interrupted);
		}
		kill(-child, SIGKILL);
	}
	if (keyboard_changed && ioctl(console, KDSKBMODE, keyboard_mode))
		perror("restore keyboard mode");
	if (ioctl(console, KDSETMODE, display_mode))
		perror("restore display console");
	close(console);
	if (WIFSIGNALED(status))
		fprintf(stderr, "GUI terminated by signal %d; console restored.\n", WTERMSIG(status));
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}

static void usage(FILE *stream, const char *program)
{
	fprintf(stream, "Usage: %s [--page NAME] [--snapshot FILE.ppm]\n"
		"Manual ESP-Mosaico peripheral tests; stop with Home / X or Ctrl-C.\n"
		"--snapshot renders the UI without opening hardware or running tests.\n"
		"Pages: home, display, touch, camera, keys, imu, compass, audio,\n"
		"       wifi, bluetooth, usb, storage, system.\n", program);
}

int main(int argc, char **argv)
{
	static const char *const names[] = { "home", "display", "touch", "camera", "keys",
		"imu", "compass", "audio", "wifi", "bluetooth", "usb", "storage", "system" };
	static const struct option options[] = {
		{ "page", required_argument, NULL, 'p' },
		{ "snapshot", required_argument, NULL, 's' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct sigaction signals = { .sa_handler = handle_signal };
	const char *snapshot = NULL;
	enum page page = PAGE_HOME;
	int option, framebuffer, result;

	while ((option = getopt_long(argc, argv, "hp:s:", options, NULL)) != -1) {
		switch (option) {
		case 'p':
			for (page = PAGE_HOME; page < PAGE_COUNT; page++) {
				if (!strcmp(optarg, names[page]))
					break;
			}
			if (page == PAGE_COUNT) {
				fprintf(stderr, "Unknown page: %s\n", optarg);
				return 1;
			}
			break;
		case 's':
			snapshot = optarg;
			break;
		case 'h':
			usage(stdout, argv[0]);
			return 0;
		default:
			usage(stderr, argv[0]);
			return 1;
		}
	}
	if (optind != argc) {
		usage(stderr, argv[0]);
		return 1;
	}
	if (snapshot)
		return run_ui(-1, snapshot, page);
	sigemptyset(&signals.sa_mask);
	if (sigaction(SIGINT, &signals, NULL) || sigaction(SIGTERM, &signals, NULL) ||
	    sigaction(SIGHUP, &signals, NULL)) {
		perror("install signal handlers");
		return 1;
	}
	framebuffer = display_open();
	if (framebuffer < 0) {
		fprintf(stderr, "Open framebuffer: %s\n", strerror(-framebuffer));
		return 1;
	}
	result = supervise(framebuffer, page);
	close(framebuffer);
	return result;
}
