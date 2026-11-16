// SPDX-License-Identifier: MIT
#include "tests.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <ifaddrs.h>
#include <linux/input.h>
#include <math.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

const char *const page_names[PAGE_COUNT] = {
	"Home", "Display", "Touch", "Camera", "Keys & motor", "IMU", "Compass",
	"Audio", "Wi-Fi", "Bluetooth", "USB", "Storage", "System"
};

static void append(char *text, size_t size, const char *format, ...)
{
	size_t used = strlen(text);
	va_list args;

	if (used >= size)
		return;
	va_start(args, format);
	vsnprintf(text + used, size - used, format, args);
	va_end(args);
}

static void job_stop(struct job *job)
{
	if (job->pid > 0) {
		int status;

		kill(job->pid, SIGTERM);
		/* Helpers are bounded; escalate without leaving a capture queue open. */
		for (unsigned int i = 0; i < 20; i++) {
			if (waitpid(job->pid, &status, WNOHANG) != 0)
				goto reaped;
			usleep(10000);
		}
		kill(job->pid, SIGKILL);
		while (waitpid(job->pid, &status, 0) < 0 && errno == EINTR)
			;
	}
reaped:
	if (job->fd >= 0)
		close(job->fd);
	job->pid = 0;
	job->fd = -1;
}

static int storage_check(void)
{
	char path[] = "/root/.mosaico-test-XXXXXX";
	unsigned char written[4096], readback[4096];
	uint64_t started = monotonic_ms();
	int fd = mkstemp(path);

	if (fd < 0) {
		perror("create NAND test file");
		return 1;
	}
	/* No test file survives a signal or an interrupted test. */
	if (unlink(path))
		goto fail;
	for (unsigned int block = 0; block < 16; block++) {
		for (unsigned int i = 0; i < sizeof(written); i++)
			written[i] = (i * 17 + block * 31) & 255;
		if (pwrite(fd, written, sizeof(written), block * sizeof(written)) != sizeof(written))
			goto fail;
	}
	if (fsync(fd))
		goto fail;
	for (unsigned int block = 0; block < 16; block++) {
		if (pread(fd, readback, sizeof(readback), block * sizeof(readback)) != sizeof(readback))
			goto fail;
		for (unsigned int i = 0; i < sizeof(readback); i++) {
			if (readback[i] != ((i * 17 + block * 31) & 255)) {
				fprintf(stderr, "NAND test: data mismatch\n");
				close(fd);
				return 1;
			}
		}
	}
	close(fd);
	printf("PASS: 64 KiB written, synced and read.\nElapsed: %llu ms (includes fsync).\nTemporary file removed.\n",
	       (unsigned long long)(monotonic_ms() - started));
	return 0;
fail:
	perror("NAND file test");
	close(fd);
	unlink(path);
	return 1;
}

static void job_start(struct tests *tests, const char *const argv[], bool camera, bool storage)
{
	struct job *job = &tests->job;
	int pipefd[2];
	pid_t parent = getpid();

	job_stop(job);
	job->used = 0;
	job->output[0] = '\0';
	if (pipe2(pipefd, O_CLOEXEC))
		goto fail;
	job->pid = fork();
	if (job->pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		goto fail;
	}
	if (!job->pid) {
		prctl(PR_SET_PDEATHSIG, SIGTERM);
		if (getppid() != parent)
			_exit(1);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		if (camera) {
			char number[32];

			fcntl(tests->framebuffer, F_SETFD, 0);
			snprintf(number, sizeof(number), "%d", tests->framebuffer);
			execlp("camera-preview", "camera-preview", "-e", number, NULL);
		} else if (storage) {
			int result = storage_check();

			fflush(NULL);
			_exit(result);
		} else {
			execvp(argv[0], (char *const *)argv);
		}
		perror("start helper");
		_exit(127);
	}
	close(pipefd[1]);
	job->fd = pipefd[0];
	fcntl(job->fd, F_SETFL, O_NONBLOCK);
	job->deadline = camera ? 0 : monotonic_ms() + 15000;
	return;
fail:
	job->pid = 0;
	snprintf(job->output, sizeof(job->output), "Cannot start test: %s", strerror(errno));
}

static void job_poll(struct job *job, uint64_t now)
{
	char buffer[256];
	ssize_t count;
	int status;

	if (!job->pid)
		return;
	/* Drain even after the visible buffer fills, so helpers never block on it. */
	while ((count = read(job->fd, buffer, sizeof(buffer))) > 0) {
		size_t keep = count;

		if (keep > sizeof(job->output) - job->used - 1)
			keep = sizeof(job->output) - job->used - 1;
		memcpy(job->output + job->used, buffer, keep);
		job->used += keep;
		job->output[job->used] = '\0';
	}
	if (waitpid(job->pid, &status, WNOHANG) == job->pid) {
		close(job->fd);
		job->fd = -1;
		job->pid = 0;
		if (!WIFEXITED(status) || WEXITSTATUS(status))
			append(job->output, sizeof(job->output), "\nHelper failed (status 0x%x).", status);
	} else if (job->deadline && now >= job->deadline) {
		job_stop(job);
		append(job->output, sizeof(job->output), "\nTest timed out; stopped.");
	}
}

static void motor_stop(struct tests *tests)
{
	if (tests->motor_fd >= 0) {
		struct input_event event = { .type = EV_FF, .code = tests->motor_effect };

		if (write(tests->motor_fd, &event, sizeof(event)) != sizeof(event))
			perror("stop motor effect");
		ioctl(tests->motor_fd, EVIOCRMFF, tests->motor_effect);
		close(tests->motor_fd);
		tests->motor_fd = -1;
	}
	tests->motor_pulses = 0;
}

static void motor_start(struct tests *tests)
{
	struct ff_effect effect = {
		.type = FF_RUMBLE, .id = -1,
		.u.rumble.strong_magnitude = 0xffff,
		.replay.length = 80,
	};

	motor_stop(tests);
	for (unsigned int i = 0; i < 32; i++) {
		char path[64], name[128] = { 0 };
		int fd;

		snprintf(path, sizeof(path), "/dev/input/event%u", i);
		fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 && strstr(name, "vibra") &&
		    !ioctl(fd, EVIOCSFF, &effect)) {
			tests->motor_fd = fd;
			tests->motor_effect = effect.id;
			tests->motor_pulses = 3;
			tests->motor_next = monotonic_ms();
			return;
		}
		close(fd);
	}
	snprintf(tests->job.output, sizeof(tests->job.output), "Motor unavailable; no effect uploaded.");
}

static void motor_poll(struct tests *tests, uint64_t now)
{
	struct input_event event = { .type = EV_FF, .code = tests->motor_effect, .value = 1 };

	if (tests->motor_fd < 0 || now < tests->motor_next)
		return;
	if (!tests->motor_pulses) {
		motor_stop(tests);
		return;
	}
	if (write(tests->motor_fd, &event, sizeof(event)) != sizeof(event)) {
		snprintf(tests->job.output, sizeof(tests->job.output), "Motor write failed: %s", strerror(errno));
		motor_stop(tests);
		return;
	}
	tests->motor_pulses--;
	tests->motor_next = now + 240;
}

static void network_status(char *text, size_t size)
{
	struct ifaddrs *addresses;
	char state[64] = "unavailable";

	read_text("/sys/class/net/wlan0/operstate", state, sizeof(state));
	snprintf(text, size, "wlan0: %s\n", state);
	if (!getifaddrs(&addresses)) {
		for (struct ifaddrs *a = addresses; a; a = a->ifa_next) {
			char address[INET_ADDRSTRLEN];

			if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET)
				continue;
			struct sockaddr_in *in = (struct sockaddr_in *)a->ifa_addr;

			if (inet_ntop(AF_INET, &in->sin_addr, address, sizeof(address)))
				append(text, size, "%s  %s\n", a->ifa_name, address);
		}
		freeifaddrs(addresses);
	}
	append(text, size, "\nUse the existing Wi-Fi configuration.\nSSH remains available while testing.");
}

static void usb_status(char *text, size_t size)
{
	glob_t paths;
	char value[128];

	text[0] = '\0';
	if (!glob("/sys/class/usb_role/*/role", 0, NULL, &paths)) {
		for (size_t i = 0; i < paths.gl_pathc; i++) {
			if (!read_text(paths.gl_pathv[i], value, sizeof(value)))
				append(text, size, "Role: %s\n", value);
		}
		globfree(&paths);
	}
	if (!glob("/sys/class/udc/*/state", 0, NULL, &paths)) {
		for (size_t i = 0; i < paths.gl_pathc; i++) {
			if (!read_text(paths.gl_pathv[i], value, sizeof(value)))
				append(text, size, "Gadget: %s\n", value);
		}
		globfree(&paths);
	}
	append(text, size, "\nDevice: connect native Type-C to PC.\nHost: connect a powered OTG hub.\nCH343 remains the debug UART.\n\nROM USB flashing is a separate mode.");
}

static void memory_status(char *text, size_t size)
{
	FILE *file = fopen("/proc/meminfo", "r");
	char line[192];

	snprintf(text, size, "ESP32-S31, Linux hart at 320 MHz\n16 MiB PSRAM; SRAM shared with radio\nNOR 16 MiB; NAND 128 MiB\n\n");
	if (file) {
		while (fgets(line, sizeof(line), file)) {
			if (!strncmp(line, "MemTotal:", 9) || !strncmp(line, "MemAvailable:", 13) ||
			    !strncmp(line, "SwapTotal:", 10) || !strncmp(line, "SwapFree:", 9))
				append(text, size, "%s", line);
		}
		fclose(file);
	}
	append(text, size, "\nBattery gauge: 0x55 unresponsive.\nNo battery percentage is inferred.");
}

void tests_init(struct tests *tests, struct inputs *inputs, int framebuffer, bool snapshot)
{
	memset(tests, 0, sizeof(*tests));
	tests->inputs = inputs;
	tests->framebuffer = framebuffer;
	tests->snapshot = snapshot;
	tests->motor_fd = tests->job.fd = -1;
	audio_init(&tests->audio);
}

void tests_close(struct tests *tests)
{
	job_stop(&tests->job);
	motor_stop(tests);
	audio_close(&tests->audio);
}

void tests_enter(struct tests *tests, enum page page)
{
	tests_close(tests);
	tests->page = page;
	tests->sample_next = 0;
	tests->status[0] = tests->job.output[0] = '\0';
	memset(&tests->sensors, 0, sizeof(tests->sensors));
	audio_init(&tests->audio);
}

void tests_action(struct tests *tests, enum action action)
{
	static const char *const wifi[] = { "iw", "dev", "wlan0", "link", NULL };
	static const char *const bluetooth[] = { "bluetoothctl", "--timeout", "5", "show", NULL };

	if (tests->snapshot)
		return;
	switch (tests->page) {
	case PAGE_CAMERA:
		job_start(tests, NULL, true, false);
		break;
	case PAGE_KEYS:
		motor_start(tests);
		break;
	case PAGE_AUDIO:
		if (action == ACTION_PRIMARY)
			audio_arm(&tests->audio);
		else
			audio_record(&tests->audio, monotonic_ms());
		break;
	case PAGE_WIFI:
		job_start(tests, wifi, false, false);
		break;
	case PAGE_BLUETOOTH:
		job_start(tests, bluetooth, false, false);
		break;
	case PAGE_USB: {
		glob_t paths;
		int result = -ENODEV;

		if (!glob("/sys/class/usb_role/*/role", 0, NULL, &paths)) {
			if (paths.gl_pathc == 1)
				result = write_text(paths.gl_pathv[0], action == ACTION_PRIMARY ? "device\n" : "host\n");
			globfree(&paths);
		}
		snprintf(tests->job.output, sizeof(tests->job.output), "Role request: %s",
			 result ? strerror(-result) : "accepted");
		break;
	}
	case PAGE_STORAGE:
		job_start(tests, NULL, false, true);
		break;
	default:
		break;
	}
}

void tests_poll(struct tests *tests, uint64_t now, bool ai_pressed)
{
	job_poll(&tests->job, now);
	motor_poll(tests, now);
	if (tests->page == PAGE_AUDIO) {
		if (ai_pressed)
			audio_trigger(&tests->audio, now);
		audio_poll(&tests->audio, now);
	}
	if (now < tests->sample_next)
		return;
	tests->sample_next = now + 250;
	if (tests->snapshot) {
		snprintf(tests->status, sizeof(tests->status),
			 "Host UI snapshot\nHardware tests are inactive.\n\nRun mosaico-test on the board\nto read live peripheral data.");
		return;
	}
	switch (tests->page) {
	case PAGE_CAMERA:
		snprintf(tests->status, sizeof(tests->status), tests->job.pid ?
			 "Live preview / automatic camera reconnection" : "Preview stopped. Return Home to retry.");
		break;
	case PAGE_TOUCH:
		snprintf(tests->status, sizeof(tests->status), "CST9220: %s\nTouch with one or two fingers.\n",
			 tests->inputs->touch_fd >= 0 ? "connected" : "unavailable");
		for (unsigned int i = 0; i < CONTACTS; i++)
			append(tests->status, sizeof(tests->status), "\nFinger %u: %s  x=%d  y=%d", i + 1,
			       tests->inputs->contact[i].down ? "DOWN" : "up", tests->inputs->contact[i].x,
			       tests->inputs->contact[i].y);
		break;
	case PAGE_KEYS:
		snprintf(tests->status, sizeof(tests->status),
			 "AI key: %s\nPress count: %u\n\nPress AI to check the counter.\nMotor runs only with the test button.\nThree 80 ms pulses.\n\n%s",
			 tests->inputs->key_fd >= 0 ? "connected" : "unavailable",
			 tests->inputs->key_presses, tests->job.output);
		break;
	case PAGE_IMU:
	case PAGE_COMPASS:
		sensors_read(&tests->sensors, "/sys/bus/iio/devices", tests->page == PAGE_COMPASS);
		break;
	case PAGE_AUDIO:
		snprintf(tests->status, sizeof(tests->status), "%s", tests->audio.status);
		break;
	case PAGE_WIFI:
		network_status(tests->status, sizeof(tests->status));
		if (tests->job.output[0])
			snprintf(tests->status, sizeof(tests->status), "%s", tests->job.output);
		break;
	case PAGE_BLUETOOTH:
		snprintf(tests->status, sizeof(tests->status),
			 "Controller: %s\n\nCheck starts the stock Bluetooth service\non demand and queries the controller.\nIt does not start device discovery.\n\n%.700s",
			 access("/sys/class/bluetooth/hci0", F_OK) ? "unavailable" : "hci0", tests->job.output);
		break;
	case PAGE_USB:
		usb_status(tests->status, sizeof(tests->status));
		append(tests->status, sizeof(tests->status),
		       "\nUSB input nodes: %u  key presses: %u\nLast key code: %u  mouse: %lld, %lld\n",
		       tests->inputs->usb_devices, tests->inputs->usb_events, tests->inputs->usb_last_code,
		       (long long)tests->inputs->mouse_x, (long long)tests->inputs->mouse_y);
		append(tests->status, sizeof(tests->status), "\n%s", tests->job.output);
		break;
	case PAGE_STORAGE: {
		struct statvfs fs;

		snprintf(tests->status, sizeof(tests->status),
			 "NOR: bootloader, kernel, read-only root\nNAND: writable files and swap\nZram takes priority over NAND swap.\n");
		if (!statvfs("/root", &fs))
			append(tests->status, sizeof(tests->status), "\nWritable free: %.1f MiB\n",
			       (double)fs.f_bavail * fs.f_frsize / (1024 * 1024));
		append(tests->status, sizeof(tests->status), "\n%s", tests->job.output);
		break;
	}
	case PAGE_SYSTEM:
		memory_status(tests->status, sizeof(tests->status));
		break;
	default:
		break;
	}
}
