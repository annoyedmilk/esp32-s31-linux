// SPDX-License-Identifier: MIT
#ifndef MOSAICO_UI_H
#define MOSAICO_UI_H

#include "tests.h"
#include "instruments.h"

struct ui {
	struct tests *tests;
	struct instruments instruments;
	lv_obj_t *status, *pattern_panel, *markers[CONTACTS];
	bool quit, camera_pending;
	unsigned int pattern;
	char previous[1024];
};

void ui_init(struct ui *ui, struct tests *tests, enum page page);
void ui_refresh(struct ui *ui);

#endif
