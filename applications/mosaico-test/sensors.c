// SPDX-License-Identifier: MIT
#include "sensors.h"
#include "platform.h"

#include <ctype.h>
#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEGREES_PER_RADIAN 57.29577951308232

static bool number(const char *path, double *value)
{
	char buffer[64], *end;

	if (read_text(path, buffer, sizeof(buffer)))
		return false;
	errno = 0;
	*value = strtod(buffer, &end);
	if (end == buffer || errno || !isfinite(*value))
		return false;
	while (isspace((unsigned char)*end))
		end++;
	return !*end;
}

static void vector_read(struct sensor_vector *vector, const char *device,
			const char *channel, double unit, bool magnetic)
{
	char path[512];
	double scale;

	snprintf(path, sizeof(path), "%s/in_%s_scale", device, channel);
	if (!number(path, &scale) || scale <= 0)
		return;
	for (unsigned int i = 0; i < SENSOR_AXES; i++) {
		double raw;

		snprintf(path, sizeof(path), "%s/in_%s_%c_raw", device, channel, "xyz"[i]);
		if (!number(path, &raw))
			continue;
		/* The BMM150 driver uses INT_MIN for a compensated-axis overflow. */
		if (magnetic && raw == INT_MIN) {
			vector->overflow |= 1U << i;
			continue;
		}
		vector->value[i] = raw * scale * unit;
		if (isfinite(vector->value[i]))
			vector->valid |= 1U << i;
		else
			vector->value[i] = 0;
	}
}

void sensors_read(struct sensors *sensors, const char *directory, bool magnetic)
{
	glob_t paths = { 0 };
	char pattern[512];

	memset(sensors, 0, sizeof(*sensors));
	snprintf(pattern, sizeof(pattern), "%s/iio:device*", directory);
	if (glob(pattern, 0, NULL, &paths)) {
		globfree(&paths);
		return;
	}
	for (size_t i = 0; i < paths.gl_pathc; i++) {
		const char *device = paths.gl_pathv[i];
		char path[512], name[64];

		snprintf(path, sizeof(path), "%s/name", device);
		if (read_text(path, name, sizeof(name)))
			continue;
		if (!magnetic && !strcmp(name, "bmi270")) {
			sensors->motion_present = true;
			vector_read(&sensors->acceleration, device, "accel", 1, false);
			vector_read(&sensors->rotation, device, "anglvel", DEGREES_PER_RADIAN, false);
			break;
		}
		if (magnetic && !strcmp(name, "bmm150") && sensors->magnetic_count < MAGNETOMETERS) {
			struct sensor_vector *field = &sensors->magnetic[sensors->magnetic_count++];

			/* IIO magnetic-field scales are in gauss; 1 G = 100 microtesla. */
			vector_read(field, device, "magn", 100, true);
		}
	}
	globfree(&paths);
}

bool sensors_tilt(const struct sensor_vector *acceleration, double *pitch,
		  double *roll, double *gravity)
{
	double x = acceleration->value[0];
	double y = acceleration->value[1];
	double z = acceleration->value[2];

	if (acceleration->valid != SENSOR_ALL_AXES)
		return false;
	*gravity = hypot(hypot(x, y), z);
	/* A near-zero acceleration vector has no useful gravity direction. */
	if (!isfinite(*gravity) || *gravity < 0.05)
		return false;
	*pitch = atan2(-x, hypot(y, z)) * DEGREES_PER_RADIAN;
	*roll = atan2(y, z) * DEGREES_PER_RADIAN;
	return true;
}
