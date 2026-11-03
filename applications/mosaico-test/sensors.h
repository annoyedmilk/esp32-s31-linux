// SPDX-License-Identifier: MIT
#ifndef MOSAICO_SENSORS_H
#define MOSAICO_SENSORS_H

#include <stdbool.h>

#define SENSOR_AXES 3
#define SENSOR_ALL_AXES 7U
#define MAGNETOMETERS 2

struct sensor_vector {
	double value[SENSOR_AXES];
	unsigned int valid, overflow;
};

struct sensors {
	/* Engineering units: m/s2, degrees/s and microtesla respectively. */
	struct sensor_vector acceleration, rotation;
	struct sensor_vector magnetic[MAGNETOMETERS];
	unsigned int magnetic_count;
	bool motion_present;
};

void sensors_read(struct sensors *sensors, const char *directory, bool magnetic);
bool sensors_tilt(const struct sensor_vector *acceleration, double *pitch,
		  double *roll, double *gravity);

#endif
