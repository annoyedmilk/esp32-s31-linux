// SPDX-License-Identifier: MIT
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

uint64_t monotonic_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int read_text(const char *path, char *buffer, size_t size)
{
	int fd;
	ssize_t count;

	if (!size)
		return -EINVAL;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	do {
		count = read(fd, buffer, size - 1);
	} while (count < 0 && errno == EINTR);
	if (count < 0) {
		int error = -errno;

		close(fd);
		return error;
	}
	close(fd);
	buffer[count] = '\0';
	buffer[strcspn(buffer, "\r\n")] = '\0';
	return 0;
}

int write_text(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	size_t length = strlen(value);
	ssize_t count;
	int result;

	if (fd < 0)
		return -errno;
	do {
		count = write(fd, value, length);
	} while (count < 0 && errno == EINTR);
	result = count < 0 ? -errno : (size_t)count == length ? 0 : -EIO;
	close(fd);
	return result;
}

static int write_pixels(int fd, const uint8_t *pixels, size_t length, off_t offset)
{
	while (length) {
		ssize_t count = pwrite(fd, pixels, length, offset);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return count < 0 ? -errno : -EIO;
		pixels += count;
		offset += count;
		length -= count;
	}
	return 0;
}

int display_open(void)
{
	struct fb_fix_screeninfo fixed;
	struct fb_var_screeninfo variable;
	int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	int error;

	if (fd < 0)
		return -errno;
	if (ioctl(fd, FBIOGET_FSCREENINFO, &fixed) ||
	    ioctl(fd, FBIOGET_VSCREENINFO, &variable) ||
	    flock(fd, LOCK_EX | LOCK_NB)) {
		error = -errno;
		goto fail;
	}
	if (variable.xres != SCREEN_WIDTH || variable.yres != SCREEN_HEIGHT ||
	    variable.bits_per_pixel != 16 || fixed.line_length != SCREEN_WIDTH * 2 ||
	    variable.xoffset || variable.yoffset || variable.red.offset != 11 ||
	    variable.red.length != 5 || variable.green.offset != 5 ||
	    variable.green.length != 6 || variable.blue.offset || variable.blue.length != 5) {
		error = -EINVAL;
		goto fail;
	}
	return fd;
fail:
	close(fd);
	return error;
}

static void flush_display(lv_display_t *lv, const lv_area_t *area, uint8_t *pixels)
{
	struct display *display = lv_display_get_user_data(lv);
	size_t row_bytes = (area->x2 - area->x1 + 1) * 2;

	for (int y = area->y1; y <= area->y2; y++) {
		size_t offset = (y * SCREEN_WIDTH + area->x1) * 2;

		if (display->snapshot)
			memcpy((uint8_t *)display->snapshot + offset, pixels, row_bytes);
		else if (!display->error)
			display->error = write_pixels(display->fd, pixels, row_bytes, offset);
		pixels += row_bytes;
	}
	if (!display->snapshot && !display->error && lv_display_flush_is_last(lv) &&
	    fsync(display->fd))
		display->error = -errno;
	lv_display_flush_ready(lv);
}

int display_init(struct display *display, int fd, bool snapshot)
{
	memset(display, 0, sizeof(*display));
	display->fd = fd;
	if (snapshot) {
		display->snapshot = calloc(SCREEN_WIDTH * SCREEN_HEIGHT, sizeof(uint16_t));
		if (!display->snapshot)
			return -ENOMEM;
	}
	display->lv = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT);
	if (!display->lv) {
		free(display->snapshot);
		display->snapshot = NULL;
		return -ENOMEM;
	}
	lv_display_set_color_format(display->lv, LV_COLOR_FORMAT_RGB565);
	lv_display_set_user_data(display->lv, display);
	lv_display_set_buffers(display->lv, display->tile, NULL, sizeof(display->tile),
			       LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(display->lv, flush_display);
	return 0;
}

void display_destroy(struct display *display)
{
	if (display->lv)
		lv_display_delete(display->lv);
	free(display->snapshot);
}

int display_snapshot(struct display *display, const char *path)
{
	FILE *file = fopen(path, "wb");
	int result = 0;

	if (!file)
		return -errno;
	if (fprintf(file, "P6\n%d %d\n255\n", SCREEN_WIDTH, SCREEN_HEIGHT) < 0)
		result = -EIO;
	for (unsigned int i = 0; !result && i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
		unsigned int pixel = display->snapshot[i];
		uint8_t rgb[] = {
			((pixel >> 11) << 3) | (pixel >> 13),
			(((pixel >> 5) & 63) << 2) | ((pixel >> 9) & 3),
			((pixel & 31) << 3) | ((pixel & 31) >> 2),
		};

		if (fwrite(rgb, 1, sizeof(rgb), file) != sizeof(rgb))
			result = -EIO;
	}
	if (fclose(file) && !result)
		result = -errno;
	return result;
}

static int find_input(const char *wanted)
{
	for (unsigned int i = 0; i < 32; i++) {
		char path[64], name[128] = { 0 };
		int fd;

		snprintf(path, sizeof(path), "/dev/input/event%u", i);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 && !strcmp(name, wanted))
			return fd;
		close(fd);
	}
	return -1;
}

static void resync_touch(struct inputs *inputs)
{
	struct input_absinfo slot;
	int32_t ids[TOUCH_SLOTS + 1] = { ABS_MT_TRACKING_ID };
	int32_t xs[TOUCH_SLOTS + 1] = { ABS_MT_POSITION_X };
	int32_t ys[TOUCH_SLOTS + 1] = { ABS_MT_POSITION_Y };

	memset(inputs->slots, 0, sizeof(inputs->slots));
	if (!ioctl(inputs->touch_fd, EVIOCGABS(ABS_MT_SLOT), &slot))
		inputs->slot = slot.value;
	if (ioctl(inputs->touch_fd, EVIOCGMTSLOTS(sizeof(ids)), ids) < 0 ||
	    ioctl(inputs->touch_fd, EVIOCGMTSLOTS(sizeof(xs)), xs) < 0 ||
	    ioctl(inputs->touch_fd, EVIOCGMTSLOTS(sizeof(ys)), ys) < 0)
		return;
	for (unsigned int i = 0; i < TOUCH_SLOTS; i++)
		inputs->slots[i] = (struct contact) {
			.x = xs[i + 1], .y = ys[i + 1], .down = ids[i + 1] >= 0,
		};
}

static void resync_key(struct inputs *inputs)
{
	unsigned long bits[(KEY_MAX + 8 * sizeof(unsigned long)) /
			   (8 * sizeof(unsigned long))] = { 0 };
	unsigned int word = KEY_PROG1 / (8 * sizeof(unsigned long));
	unsigned int bit = KEY_PROG1 % (8 * sizeof(unsigned long));

	inputs->key_released = false;
	if (ioctl(inputs->key_fd, EVIOCGKEY(sizeof(bits)), bits) >= 0)
		inputs->key_released = !(bits[word] & (1UL << bit));
}

void inputs_open(struct inputs *inputs)
{
	memset(inputs, 0, sizeof(*inputs));
	inputs->touch_fd = find_input("CST9220 Touchscreen");
	inputs->key_fd = find_input("keys");
	for (unsigned int i = 0; i < USB_INPUTS; i++)
		inputs->usb_fd[i] = -1;
	resync_touch(inputs);
	resync_key(inputs);
}

void inputs_close(struct inputs *inputs)
{
	if (inputs->touch_fd >= 0)
		close(inputs->touch_fd);
	if (inputs->key_fd >= 0)
		close(inputs->key_fd);
	for (unsigned int i = 0; i < USB_INPUTS; i++) {
		if (inputs->usb_fd[i] >= 0)
			close(inputs->usb_fd[i]);
	}
}

static void poll_usb_inputs(struct inputs *inputs)
{
	struct input_event event;
	uint64_t now = monotonic_ms();

	for (unsigned int i = 0; i < USB_INPUTS; i++) {
		ssize_t count;

		if (inputs->usb_fd[i] < 0)
			continue;
		while ((count = read(inputs->usb_fd[i], &event, sizeof(event))) == sizeof(event)) {
			if (event.type == EV_KEY && event.value == 1) {
				inputs->usb_events++;
				inputs->usb_last_code = event.code;
			} else if (event.type == EV_REL) {
				if (event.code == REL_X)
					inputs->mouse_x += event.value;
				if (event.code == REL_Y)
					inputs->mouse_y += event.value;
			}
		}
		if (!count || (count < 0 && errno == ENODEV)) {
			close(inputs->usb_fd[i]);
			inputs->usb_fd[i] = -1;
		}
	}
	if (now < inputs->usb_next_scan)
		return;
	inputs->usb_next_scan = now + 1000;
	inputs->usb_devices = 0;
	/* Re-discover by device identity, since event numbers change on replug. */
	for (unsigned int i = 0; i < USB_INPUTS; i++) {
		struct input_id id;
		char path[64];
		int fd;

		if (inputs->usb_fd[i] >= 0) {
			inputs->usb_devices++;
			continue;
		}
		snprintf(path, sizeof(path), "/dev/input/event%u", i);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (!ioctl(fd, EVIOCGID, &id) && id.bustype == BUS_USB) {
			inputs->usb_fd[i] = fd;
			inputs->usb_devices++;
		} else {
			close(fd);
		}
	}
}

bool inputs_poll(struct inputs *inputs)
{
	struct input_event event;
	bool pressed = false;

	poll_usb_inputs(inputs);
	while (read(inputs->touch_fd, &event, sizeof(event)) == sizeof(event)) {
		if (event.type == EV_SYN && event.code == SYN_DROPPED)
			inputs->touch_lost = true;
		if (inputs->touch_lost) {
			if (event.type == EV_SYN && event.code == SYN_REPORT) {
				resync_touch(inputs);
				inputs->touch_lost = false;
			}
			continue;
		}
		if (event.type != EV_ABS)
			continue;
		if (event.code == ABS_MT_SLOT)
			inputs->slot = event.value;
		if (inputs->slot < 0 || inputs->slot >= TOUCH_SLOTS)
			continue;
		struct contact *contact = &inputs->slots[inputs->slot];

		if (event.code == ABS_MT_TRACKING_ID)
			contact->down = event.value >= 0;
		else if (event.code == ABS_MT_POSITION_X)
			contact->x = event.value;
		else if (event.code == ABS_MT_POSITION_Y)
			contact->y = event.value;
	}
	/* The controller uses sixteen tracking slots, even for two fingers. */
	unsigned int active = 0;

	for (unsigned int i = 0; i < CONTACTS; i++)
		inputs->contact[i].down = false;
	for (unsigned int i = 0; i < TOUCH_SLOTS && active < CONTACTS; i++) {
		if (inputs->slots[i].down)
			inputs->contact[active++] = inputs->slots[i];
	}
	while (read(inputs->key_fd, &event, sizeof(event)) == sizeof(event)) {
		if (event.type == EV_SYN && event.code == SYN_DROPPED)
			inputs->key_lost = true;
		if (inputs->key_lost) {
			if (event.type == EV_SYN && event.code == SYN_REPORT) {
				resync_key(inputs);
				inputs->key_lost = false;
			}
			continue;
		}
		if (event.type != EV_KEY || event.code != KEY_PROG1)
			continue;
		if (!event.value)
			inputs->key_released = true;
		if (event.value == 1 && inputs->key_released) {
			inputs->key_released = false;
			inputs->key_presses++;
			pressed = true;
		}
	}
	return pressed;
}

void inputs_read(lv_indev_t *indev, lv_indev_data_t *data)
{
	struct inputs *inputs = lv_indev_get_user_data(indev);
	static lv_point_t previous;

	data->state = LV_INDEV_STATE_RELEASED;
	for (unsigned int i = 0; i < CONTACTS; i++) {
		if (!inputs->contact[i].down)
			continue;
		previous.x = LV_CLAMP(0, inputs->contact[i].x, SCREEN_WIDTH - 1);
		previous.y = LV_CLAMP(0, inputs->contact[i].y, SCREEN_HEIGHT - 1);
		data->state = LV_INDEV_STATE_PRESSED;
		break;
	}
	data->point = previous;
}
