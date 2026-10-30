// SPDX-License-Identifier: MIT
#ifndef MOSAICO_INSTRUMENTS_H
#define MOSAICO_INSTRUMENTS_H

#include "platform.h"
#include "sensors.h"

struct instruments {
	const struct sensors *sensors;
	struct sensors previous;
	bool refreshed;
	lv_obj_t *tilt, *rates, *field[MAGNETOMETERS];
	lv_obj_t *pitch, *roll, *gravity, *note;
};

void instruments_create(struct instruments *view, const struct sensors *sensors, bool magnetic);
void instruments_refresh(struct instruments *view);

#endif
