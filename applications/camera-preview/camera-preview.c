// SPDX-License-Identifier: MIT
/* SC101IOT V4L2 capture to a 480x480 RGB565 framebuffer. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define DISPLAY_WIDTH 480
#define DISPLAY_HEIGHT 480
#define PREVIEW_HEIGHT 360
#define TOP_MARGIN ((DISPLAY_HEIGHT - PREVIEW_HEIGHT) / 2)
#define TILE_ROWS 8
#define MAX_BUFFERS 4

_Static_assert(PREVIEW_HEIGHT % TILE_ROWS == 0, "Preview must contain complete tiles");

struct mapped_buffer {
	void *address;
	size_t length;
};

static volatile sig_atomic_t stopping;
static int luma[256];
static int red_v[256], green_u[256], green_v[256], blue_u[256];
static unsigned int source_x[DISPLAY_WIDTH], source_y[PREVIEW_HEIGHT];

static void stop_preview(int signal_number)
{
	(void)signal_number;
	stopping = 1;
}

static double seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

static int camera_ioctl(int fd, unsigned long request, void *argument)
{
	int result;

	do {
		result = ioctl(fd, request, argument);
	} while (result < 0 && errno == EINTR && !stopping);
	return result;
}

static int write_pixels(int fd, const void *pixels, size_t size, off_t offset)
{
	const unsigned char *data = pixels;

	while (size) {
		ssize_t written = pwrite(fd, data, size, offset);

		if (written < 0 && errno == EINTR && !stopping)
			continue;
		if (written <= 0)
			return -1;
		data += written;
		size -= written;
		offset += written;
	}
	return 0;
}

static unsigned int clipped(int value)
{
	if (value < 0)
		return 0;
	if (value > 255)
		return 255;
	return value;
}

/* BT.601 limited-range conversion; all work inside the pixel loop is integer. */
static void convert_tile(uint16_t *output, const uint8_t *input,
			 unsigned int stride, unsigned int first_y,
			 unsigned int rows)
{
	for (unsigned int y = 0; y < rows; y++) {
		const uint8_t *row = input + source_y[first_y + y] * stride;

		for (unsigned int x = 0; x < DISPLAY_WIDTH; x++) {
			unsigned int sx = source_x[x];
			const uint8_t *pair = row + (sx & ~1U) * 2;
			int value = luma[pair[(sx & 1) ? 3 : 1]];
			unsigned int r = clipped((value + red_v[pair[2]] + 128) >> 8);
			unsigned int g = clipped((value + green_u[pair[0]] +
						 green_v[pair[2]] + 128) >> 8);
			unsigned int b = clipped((value + blue_u[pair[0]] + 128) >> 8);

			output[y * DISPLAY_WIDTH + x] =
				((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
		}
	}
}

static void initialize_conversion(unsigned int width, unsigned int height)
{
	for (unsigned int i = 0; i < 256; i++) {
		luma[i] = 298 * ((int)i - 16);
		red_v[i] = 409 * ((int)i - 128);
		green_u[i] = -100 * ((int)i - 128);
		green_v[i] = -208 * ((int)i - 128);
		blue_u[i] = 516 * ((int)i - 128);
	}
	for (unsigned int x = 0; x < DISPLAY_WIDTH; x++)
		source_x[x] = x * width / DISPLAY_WIDTH;
	for (unsigned int y = 0; y < PREVIEW_HEIGHT; y++)
		source_y[y] = y * height / PREVIEW_HEIGHT;
}

struct capture {
	int fd;
	int streaming;
	unsigned int mapped;
	struct mapped_buffer buffers[MAX_BUFFERS];
	struct v4l2_format format;
};

static void close_capture(struct capture *capture)
{
	if (capture->fd < 0)
		return;
	if (capture->streaming) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		camera_ioctl(capture->fd, VIDIOC_STREAMOFF, &type);
	}
	for (unsigned int i = 0; i < capture->mapped; i++)
		munmap(capture->buffers[i].address, capture->buffers[i].length);
	close(capture->fd);
	memset(capture, 0, sizeof(*capture));
	capture->fd = -1;
}

/* A new fd and queue clear the old VB2 error state after sensor detach. */
static int open_capture(struct capture *capture, const char *path)
{
	struct v4l2_requestbuffers request = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
		.count = 2,
	};
	struct v4l2_input input = { .index = 0 };
	int error;

	capture->fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (capture->fd < 0)
		return -errno;
	if (camera_ioctl(capture->fd, VIDIOC_ENUMINPUT, &input))
		goto fail;
	if (input.status & V4L2_IN_ST_NO_SIGNAL) {
		errno = ENOLINK;
		goto fail;
	}
	capture->format.type = request.type;
	if (camera_ioctl(capture->fd, VIDIOC_G_FMT, &capture->format))
		goto fail;
	if (capture->format.fmt.pix.pixelformat != V4L2_PIX_FMT_UYVY ||
	    capture->format.fmt.pix.width != 640 ||
	    capture->format.fmt.pix.height != 480 ||
	    capture->format.fmt.pix.bytesperline != 1280 ||
	    capture->format.fmt.pix.sizeimage < 640 * 480 * 2) {
		errno = EINVAL;
		goto fail;
	}
	if (camera_ioctl(capture->fd, VIDIOC_REQBUFS, &request))
		goto fail;
	if (request.count < 2 || request.count > MAX_BUFFERS) {
		errno = EINVAL;
		goto fail;
	}
	for (unsigned int i = 0; i < request.count; i++) {
		struct v4l2_buffer buffer = {
			.type = request.type, .memory = request.memory, .index = i,
		};

		if (camera_ioctl(capture->fd, VIDIOC_QUERYBUF, &buffer))
			goto fail;
		if (buffer.length < capture->format.fmt.pix.sizeimage) {
			errno = EIO;
			goto fail;
		}
		capture->buffers[i].length = buffer.length;
		capture->buffers[i].address = mmap(NULL, buffer.length,
			PROT_READ | PROT_WRITE, MAP_SHARED, capture->fd, buffer.m.offset);
		if (capture->buffers[i].address == MAP_FAILED)
			goto fail;
		capture->mapped++;
		if (camera_ioctl(capture->fd, VIDIOC_QBUF, &buffer))
			goto fail;
	}
	if (camera_ioctl(capture->fd, VIDIOC_STREAMON, &request.type))
		goto fail;
	capture->streaming = 1;
	initialize_conversion(capture->format.fmt.pix.width,
			      capture->format.fmt.pix.height);
	return 0;
fail:
	error = errno;
	close_capture(capture);
	return -error;
}

/* Return the newest completed buffer, or a negative error for reconnection. */
static int next_frame(struct capture *capture, struct v4l2_buffer *buffer,
		      unsigned int *discarded)
{
	struct pollfd descriptor = { .fd = capture->fd, .events = POLLIN };
	int ready = poll(&descriptor, 1, 3000);

	if (ready < 0)
		return -errno;
	if (!ready)
		return -ETIMEDOUT;
	if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
		return -ENODEV;
	*buffer = (struct v4l2_buffer) {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
	};
	if (camera_ioctl(capture->fd, VIDIOC_DQBUF, buffer))
		return errno == EAGAIN ? 0 : -errno;
	for (;;) {
		struct v4l2_buffer newer = {
			.type = buffer->type, .memory = buffer->memory,
		};

		if (camera_ioctl(capture->fd, VIDIOC_DQBUF, &newer)) {
			if (errno == EAGAIN)
				break;
			return -errno;
		}
		if (camera_ioctl(capture->fd, VIDIOC_QBUF, buffer))
			return -errno;
		*buffer = newer;
		(*discarded)++;
	}
	return buffer->index < capture->mapped ? 1 : -EIO;
}

static int display_tile(int fd, const uint16_t *pixels, unsigned int first_y,
			unsigned int rows)
{
	return write_pixels(fd, pixels, DISPLAY_WIDTH * rows * sizeof(*pixels),
			    (TOP_MARGIN + first_y) * DISPLAY_WIDTH * sizeof(*pixels));
}

static uint8_t waiting_glyph(char character, unsigned int row)
{
	static const struct {
		char character;
		uint8_t rows[7];
	} font[] = {
		{ 'A', { 14, 17, 17, 31, 17, 17, 17 } },
		{ 'C', { 14, 17, 16, 16, 16, 17, 14 } },
		{ 'E', { 31, 16, 16, 30, 16, 16, 31 } },
		{ 'F', { 31, 16, 16, 30, 16, 16, 16 } },
		{ 'G', { 14, 17, 16, 23, 17, 17, 15 } },
		{ 'I', { 14, 4, 4, 4, 4, 4, 14 } },
		{ 'M', { 17, 27, 21, 21, 17, 17, 17 } },
		{ 'N', { 17, 25, 25, 21, 19, 19, 17 } },
		{ 'O', { 14, 17, 17, 17, 17, 17, 14 } },
		{ 'R', { 30, 17, 17, 30, 20, 18, 17 } },
		{ 'T', { 31, 4, 4, 4, 4, 4, 4 } },
		{ 'W', { 17, 17, 17, 21, 21, 27, 17 } },
	};

	for (unsigned int i = 0; i < sizeof(font) / sizeof(*font); i++) {
		if (font[i].character == character)
			return font[i].rows[row];
	}
	return 0;
}

static int display_waiting(int fd, uint16_t *pixels)
{
	static const char message[] = "WAITING FOR CAMERA";
	const int scale = 4;
	int left = (DISPLAY_WIDTH - ((int)strlen(message) * 6 - 1) * scale) / 2;
	int top = (PREVIEW_HEIGHT - 7 * scale) / 2;

	for (unsigned int first_y = 0; first_y < PREVIEW_HEIGHT; first_y += TILE_ROWS) {
		memset(pixels, 0, DISPLAY_WIDTH * TILE_ROWS * sizeof(*pixels));
		for (unsigned int y = 0; y < TILE_ROWS; y++) {
			int glyph_y = (int)(first_y + y) - top;

			if (glyph_y < 0 || glyph_y >= 7 * scale)
				continue;
			for (unsigned int c = 0; c < sizeof(message) - 1; c++) {
				uint8_t bits = waiting_glyph(message[c], glyph_y / scale);

				for (int x = 0; x < 5 * scale; x++) {
					if (bits & (1 << (4 - x / scale)))
						pixels[y * DISPLAY_WIDTH + left +
						       c * 6 * scale + x] = 0xffe0;
				}
			}
		}
		if (display_tile(fd, pixels, first_y, TILE_ROWS))
			return -1;
	}
	return fsync(fd);
}

static void usage(FILE *stream, const char *program)
{
	fprintf(stream, "Usage: %s [-c CAMERA] [-f FRAMEBUFFER]\n"
		"Preview SC101IOT VGA video; reconnect after unplug/replug.\n"
		"Defaults: /dev/video0 and /dev/fb0. Stop with Ctrl-C.\n", program);
}

int main(int argc, char **argv)
{
	struct capture capture = { .fd = -1 };
	struct fb_fix_screeninfo fixed;
	struct fb_var_screeninfo variable;
	struct sigaction action = { .sa_handler = stop_preview };
	uint16_t black[DISPLAY_WIDTH] = {};
	uint16_t *pixels = NULL;
	unsigned int shown = 0, discarded = 0, errors = 0, connections = 0;
	unsigned int report_frames = 0, consecutive_errors = 0;
	double started = 0, report_time = 0, conversion_time = 0, display_time = 0;
	int framebuffer = -1, console = -1, saved_mode = KD_TEXT;
	int result = 1, console_changed = 0, previous_error = 0;
	const char *camera_path = "/dev/video0";
	const char *framebuffer_path = "/dev/fb0";
	int option;

	while ((option = getopt(argc, argv, "hc:f:")) != -1) {
		switch (option) {
		case 'c':
			camera_path = optarg;
			break;
		case 'f':
			framebuffer_path = optarg;
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
	setvbuf(stdout, NULL, _IOLBF, 0);
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGTERM, &action, NULL) ||
	    sigaction(SIGINT, &action, NULL) ||
	    sigaction(SIGHUP, &action, NULL)) {
		perror("install signal handlers");
		goto cleanup;
	}
	framebuffer = open(framebuffer_path, O_RDWR | O_CLOEXEC);
	if (framebuffer < 0 || ioctl(framebuffer, FBIOGET_FSCREENINFO, &fixed) ||
	    ioctl(framebuffer, FBIOGET_VSCREENINFO, &variable)) {
		perror("open/query framebuffer");
		goto cleanup;
	}
	if (variable.xres != DISPLAY_WIDTH || variable.yres != DISPLAY_HEIGHT ||
	    variable.bits_per_pixel != 16 || fixed.line_length != DISPLAY_WIDTH * 2 ||
	    variable.xoffset || variable.yoffset ||
	    variable.red.offset != 11 || variable.red.length != 5 ||
	    variable.green.offset != 5 || variable.green.length != 6 ||
	    variable.blue.offset != 0 || variable.blue.length != 5) {
		fprintf(stderr, "Expected 480x480 RGB565 framebuffer\n");
		goto cleanup;
	}
	if (flock(framebuffer, LOCK_EX | LOCK_NB)) {
		perror("lock framebuffer");
		goto cleanup;
	}
	pixels = malloc(DISPLAY_WIDTH * TILE_ROWS * sizeof(*pixels));
	if (!pixels) {
		perror("allocate RGB conversion buffer");
		goto cleanup;
	}
	console = open("/dev/tty0", O_RDWR | O_CLOEXEC);
	if (console < 0 || ioctl(console, KDGETMODE, &saved_mode) ||
	    ioctl(console, KDSETMODE, KD_GRAPHICS)) {
		perror("take display console");
		goto cleanup;
	}
	console_changed = 1;
	for (unsigned int y = 0; y < DISPLAY_HEIGHT; y++) {
		if (write_pixels(framebuffer, black, sizeof(black),
				 y * fixed.line_length))
			goto display_failure;
	}
	if (display_waiting(framebuffer, pixels))
		goto display_failure;
	started = seconds();
	printf("Preview PID %ld: 480x360 RGB565, automatic camera reconnection\n",
	       (long)getpid());
	while (!stopping) {
		struct v4l2_buffer buffer;
		double before, displayed;
		int ready;

		if (capture.fd < 0) {
			ready = open_capture(&capture, camera_path);
			if (ready) {
				if (ready != previous_error) {
					printf("Waiting for camera: %s\n", strerror(-ready));
					previous_error = ready;
				}
				poll(NULL, 0, 750);
				continue;
			}
			connections++;
			consecutive_errors = 0;
			previous_error = 0;
			report_frames = shown;
			report_time = seconds();
			conversion_time = display_time = 0;
			printf("Camera connected: session=%u\n", connections);
		}
		ready = next_frame(&capture, &buffer, &discarded);
		if (stopping)
			break;
		if (ready < 0) {
			if (ready == -EINTR)
				continue;
			goto reconnect;
		}
		if (!ready)
			continue;
		if ((buffer.flags & V4L2_BUF_FLAG_ERROR) ||
		    buffer.bytesused < capture.format.fmt.pix.sizeimage ||
		    buffer.bytesused > capture.buffers[buffer.index].length) {
			errors++;
			if (++consecutive_errors >= 30) {
				ready = -EIO;
				goto reconnect;
			}
			if (camera_ioctl(capture.fd, VIDIOC_QBUF, &buffer)) {
				ready = -errno;
				goto reconnect;
			}
			continue;
		}
		consecutive_errors = 0;
		/* A small tile avoids retaining a second full RGB image in RAM. */
		for (unsigned int y = 0; y < PREVIEW_HEIGHT; y += TILE_ROWS) {
			double converted;

			before = seconds();
			convert_tile(pixels, capture.buffers[buffer.index].address,
				     capture.format.fmt.pix.bytesperline, y, TILE_ROWS);
			converted = seconds();
			if (display_tile(framebuffer, pixels, y, TILE_ROWS))
				goto display_failure;
			conversion_time += converted - before;
			display_time += seconds() - converted;
		}
		if (camera_ioctl(capture.fd, VIDIOC_QBUF, &buffer)) {
			ready = -errno;
			goto reconnect;
		}
		before = seconds();
		if (fsync(framebuffer))
			goto display_failure;
		displayed = seconds();
		display_time += displayed - before;
		shown++;
		if (displayed - report_time >= 5) {
			unsigned int frames = shown - report_frames;

			printf("shown=%u fps=%.2f convert_ms=%.2f display_ms=%.2f "
			       "discarded=%u errors=%u session=%u\n", shown,
			       frames / (displayed - report_time),
			       conversion_time * 1000 / frames,
			       display_time * 1000 / frames, discarded, errors, connections);
			report_time = displayed;
			report_frames = shown;
			conversion_time = display_time = 0;
		}
		continue;
reconnect:
		printf("Camera disconnected or stalled: %s; releasing capture buffers\n",
		       strerror(-ready));
		close_capture(&capture);
		if (display_waiting(framebuffer, pixels))
			goto display_failure;
		poll(NULL, 0, 750);
	}
	result = 0;
	goto cleanup;
display_failure:
	perror("display frame");
cleanup:
	close_capture(&capture);
	if (console_changed)
		ioctl(console, KDSETMODE, saved_mode);
	if (console >= 0)
		close(console);
	if (framebuffer >= 0)
		close(framebuffer);
	free(pixels);
	if (started)
		printf("Preview stopped: frames=%u sessions=%u elapsed=%.2f\n",
		       shown, connections, seconds() - started);
	return result;
}
