// SPDX-License-Identifier: MIT
#ifndef MOSAICO_TESTS_H
#define MOSAICO_TESTS_H

#include "audio.h"
#include "platform.h"
#include "sensors.h"
#include <sys/types.h>

enum page {
	PAGE_HOME, PAGE_DISPLAY, PAGE_TOUCH, PAGE_CAMERA, PAGE_KEYS,
	PAGE_IMU, PAGE_COMPASS, PAGE_AUDIO, PAGE_WIFI, PAGE_BLUETOOTH,
	PAGE_USB, PAGE_STORAGE, PAGE_SYSTEM, PAGE_COUNT
};

enum action {
	ACTION_PRIMARY, ACTION_SECONDARY
};

struct job {
	pid_t pid;
	int fd;
	uint64_t deadline;
	size_t used;
	char output[768];
};

struct tests {
	struct inputs *inputs;
	struct audio audio;
	struct job job;
	struct sensors sensors;
	enum page page;
	int framebuffer, motor_fd, motor_effect;
	unsigned int motor_pulses;
	uint64_t motor_next, sample_next;
	bool snapshot;
	char status[1024];
};

extern const char *const page_names[PAGE_COUNT];
void tests_init(struct tests *tests, struct inputs *inputs, int framebuffer, bool snapshot);
void tests_enter(struct tests *tests, enum page page);
void tests_action(struct tests *tests, enum action action);
void tests_poll(struct tests *tests, uint64_t now, bool ai_pressed);
void tests_close(struct tests *tests);

#endif
