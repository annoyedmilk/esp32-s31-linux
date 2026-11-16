// SPDX-License-Identifier: MIT
#ifndef MOSAICO_PLATFORM_H
#define MOSAICO_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lvgl.h>

#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 480
#define CONTACTS 2
#define TOUCH_SLOTS 16
#define USB_INPUTS 32

struct contact {
	int x, y;
	bool down;
};

struct inputs {
	int touch_fd, key_fd, slot;
	bool touch_lost, key_lost, key_released;
	unsigned int key_presses;
	struct contact contact[CONTACTS];
	struct contact slots[TOUCH_SLOTS];
	int usb_fd[USB_INPUTS];
	unsigned int usb_events, usb_last_code, usb_devices;
	int64_t mouse_x, mouse_y;
	uint64_t usb_next_scan;
};

struct display {
	int fd, error;
	uint16_t *snapshot;
	uint16_t tile[SCREEN_WIDTH * 8];
	lv_display_t *lv;
};

uint64_t monotonic_ms(void);
int display_open(void);
int display_init(struct display *display, int fd, bool snapshot);
void display_destroy(struct display *display);
int display_snapshot(struct display *display, const char *path);
void inputs_open(struct inputs *inputs);
void inputs_close(struct inputs *inputs);
bool inputs_poll(struct inputs *inputs);
void inputs_read(lv_indev_t *indev, lv_indev_data_t *data);
int read_text(const char *path, char *buffer, size_t size);
int write_text(const char *path, const char *value);

#endif
