// SPDX-License-Identifier: MIT
/* Host-only illustrations. No peripheral is opened or test action invoked. */
#include "platform.h"
#include "tests.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

static void sample_sensors(struct sensors *sensors)
{
	sensors->motion_present = true;
	sensors->acceleration = (struct sensor_vector) {
		.value = { -3.8, 2.4, 8.7 }, .valid = SENSOR_ALL_AXES,
	};
	sensors->rotation = (struct sensor_vector) {
		.value = { 42.3, -115.8, 8.4 }, .valid = SENSOR_ALL_AXES,
	};
	sensors->magnetic_count = MAGNETOMETERS;
	sensors->magnetic[0] = (struct sensor_vector) {
		.value = { 46.8, 81.2, 136.4 }, .valid = SENSOR_ALL_AXES,
	};
	sensors->magnetic[1] = (struct sensor_vector) {
		.value = { -32.1, 64.8, 0 },
		.valid = (1U << 0) | (1U << 1),
		.overflow = 1U << 2, /* Demonstrate an over-range Z axis. */
	};
}

int main(int argc, char **argv)
{
	struct inputs inputs = { .touch_fd = -1, .key_fd = -1 };
	struct display display;
	struct tests tests;
	struct ui ui;
	enum page page;
	int result;

	if (argc != 3) {
		fprintf(stderr, "Usage: %s home|imu|compass OUTPUT.ppm\n", argv[0]);
		return 1;
	}
	if (!strcmp(argv[1], "home"))
		page = PAGE_HOME;
	else if (!strcmp(argv[1], "imu"))
		page = PAGE_IMU;
	else if (!strcmp(argv[1], "compass"))
		page = PAGE_COMPASS;
	else {
		fprintf(stderr, "Unknown example: %s\n", argv[1]);
		return 1;
	}
	for (unsigned int i = 0; i < USB_INPUTS; i++)
		inputs.usb_fd[i] = -1;
	lv_init();
	result = display_init(&display, -1, true);
	if (result)
		goto done;
	tests_init(&tests, &inputs, -1, true);
	ui_init(&ui, &tests, page);
	sample_sensors(&tests.sensors);
	ui_refresh(&ui);
	lv_refr_now(display.lv);
	result = display_snapshot(&display, argv[2]);
	tests_close(&tests);
	display_destroy(&display);
done:
	lv_deinit();
	if (result)
		fprintf(stderr, "Render example: %s\n", strerror(-result));
	return result ? 1 : 0;
}
