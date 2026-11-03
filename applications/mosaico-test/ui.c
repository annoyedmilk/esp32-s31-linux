// SPDX-License-Identifier: MIT
#include "ui.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

struct entry {
	const char *icon, *caption;
};

static const struct entry entries[PAGE_COUNT] = {
	[PAGE_DISPLAY] = { LV_SYMBOL_IMAGE, "Panel" },
	[PAGE_TOUCH] = { LV_SYMBOL_EDIT, "Two fingers" },
	[PAGE_CAMERA] = { LV_SYMBOL_VIDEO, "Live view" },
	[PAGE_KEYS] = { LV_SYMBOL_BELL, "Input & haptics" },
	[PAGE_IMU] = { LV_SYMBOL_REFRESH, "Tilt & rotation" },
	[PAGE_COMPASS] = { LV_SYMBOL_GPS, "Magnetic field" },
	[PAGE_AUDIO] = { LV_SYMBOL_AUDIO, "Speaker & mic" },
	[PAGE_WIFI] = { LV_SYMBOL_WIFI, "Network" },
	[PAGE_BLUETOOTH] = { LV_SYMBOL_BLUETOOTH, "Controller" },
	[PAGE_USB] = { LV_SYMBOL_USB, "Devices & roles" },
	[PAGE_STORAGE] = { LV_SYMBOL_SD_CARD, "NOR & NAND" },
	[PAGE_SYSTEM] = { LV_SYMBOL_SETTINGS, "Memory & swap" },
};

static void show_page(struct ui *ui, enum page page);

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int width, int height, uint32_t color, int radius)
{
	lv_obj_t *obj = lv_obj_create(parent);

	lv_obj_remove_style_all(obj);
	lv_obj_set_pos(obj, x, y);
	lv_obj_set_size(obj, width, height);
	lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(obj, radius, 0);
	lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
	return obj;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int width,
		       const lv_font_t *font, uint32_t color)
{
	lv_obj_t *obj = lv_label_create(parent);

	lv_obj_set_pos(obj, x, y);
	lv_obj_set_width(obj, width);
	lv_obj_set_style_text_font(obj, font, 0);
	lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
	lv_label_set_text(obj, text);
	return obj;
}

static lv_obj_t *button(struct ui *ui, const char *text, int x, int y, int width,
			int height, lv_event_cb_t callback, uintptr_t value)
{
	lv_obj_t *obj = lv_button_create(lv_screen_active());
	lv_obj_t *caption;

	lv_obj_remove_style_all(obj);
	lv_obj_set_pos(obj, x, y);
	lv_obj_set_size(obj, width, height);
	lv_obj_set_style_bg_color(obj, lv_color_hex(UI_SURFACE), 0);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(obj, lv_color_hex(UI_RULE), LV_STATE_PRESSED);
	lv_obj_set_style_radius(obj, 5, 0);
	lv_obj_set_user_data(obj, ui);
	lv_obj_add_event_cb(obj, callback, LV_EVENT_CLICKED, (void *)value);
	if (text[0]) {
		caption = label(obj, text, 0, 0, width, &lv_font_montserrat_18, UI_INK);
		lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_center(caption);
	}
	return obj;
}

static void navigate(lv_event_t *event)
{
	struct ui *ui = lv_obj_get_user_data(lv_event_get_target_obj(event));
	enum page page = (uintptr_t)lv_event_get_user_data(event);

	show_page(ui, page);
}

static void exit_app(lv_event_t *event)
{
	struct ui *ui = lv_obj_get_user_data(lv_event_get_target_obj(event));

	ui->quit = true;
}

static void action(lv_event_t *event)
{
	struct ui *ui = lv_obj_get_user_data(lv_event_get_target_obj(event));

	tests_action(ui->tests, (uintptr_t)lv_event_get_user_data(event));
}

static void draw_pattern(struct ui *ui)
{
	static const uint32_t colors[] = { 0xff3030, 0x30e070, 0x3070ff, 0xffffff, 0x000000, 0x808080 };
	lv_obj_t *panel = ui->pattern_panel;

	lv_obj_clean(panel);
	if (ui->pattern < 3) {
		for (unsigned int i = 0; i < 6; i++)
			box(panel, i * 74, 0, 74, 276, colors[(i + ui->pattern) % 6], 0);
	} else {
		box(panel, 0, 0, 444, 276, colors[ui->pattern - 3], 0);
	}
}

static void next_pattern(lv_event_t *event)
{
	struct ui *ui = lv_obj_get_user_data(lv_event_get_target_obj(event));

	ui->pattern = (ui->pattern + 1) % 9;
	draw_pattern(ui);
}

static void show_home(struct ui *ui)
{
	lv_obj_t *screen = lv_screen_active();

	box(screen, 24, 22, 4, 20, UI_ACCENT, 0);
	label(screen, "Mosaico", 38, 20, 228, &lv_font_montserrat_18, UI_INK);
	label(screen, "CoreBoard 1.2", 280, 23, 138, &lv_font_montserrat_14, UI_MUTED);
	button(ui, LV_SYMBOL_CLOSE, 424, 12, 40, 40, exit_app, 0);
	label(screen, "Hardware tests", 24, 62, 408, &lv_font_montserrat_24, UI_INK);
	for (enum page page = PAGE_DISPLAY; page < PAGE_COUNT; page++) {
		unsigned int index = page - 1;
		int x = 24 + (index % 3) * 146;
		int y = 112 + (index / 3) * 78;
		lv_obj_t *tile = button(ui, "", x, y, 140, 72, navigate, page);

		lv_obj_set_style_bg_color(tile, lv_color_hex(UI_PAPER), 0);
		lv_obj_set_style_radius(tile, 3, 0);
		label(tile, entries[page].icon, 0, 2, 32, &lv_font_montserrat_24, UI_INK);
		label(tile, page_names[page], 0, 31, 140, &lv_font_montserrat_18, UI_INK);
		label(tile, entries[page].caption, 0, 54, 140, &lv_font_montserrat_14, UI_MUTED);
	}
	box(screen, 24, 432, 432, 1, UI_RULE, 0);
	label(screen, "12 tools", 24, 449, 120, &lv_font_montserrat_14, UI_MUTED);
	label(screen, "Tap to open", 330, 449, 126, &lv_font_montserrat_14, UI_MUTED);
}

static void show_page(struct ui *ui, enum page page)
{
	lv_obj_t *screen = lv_screen_active();
	const char *primary = NULL, *secondary = NULL;

	tests_enter(ui->tests, page);
	lv_obj_clean(screen);
	ui->status = NULL;
	ui->pattern_panel = NULL;
	ui->camera_pending = page == PAGE_CAMERA;
	memset(&ui->instruments, 0, sizeof(ui->instruments));
	memset(ui->markers, 0, sizeof(ui->markers));
	ui->previous[0] = '\0';
	if (page == PAGE_HOME) {
		show_home(ui);
		return;
	}
	button(ui, LV_SYMBOL_LEFT " Home", 12, 8, 104, 42, navigate, PAGE_HOME);
	label(screen, page_names[page], 138, 17, 318, &lv_font_montserrat_24, UI_INK);
	box(screen, 24, 60, 432, 1, UI_RULE, 0);
	if (page == PAGE_CAMERA) {
		ui->status = label(screen, "Starting camera...", 18, 441, 444, &lv_font_montserrat_14, UI_MUTED);
		return;
	}
	if (page == PAGE_IMU || page == PAGE_COMPASS) {
		instruments_create(&ui->instruments, &ui->tests->sensors, page == PAGE_COMPASS);
		return;
	}
	if (page == PAGE_DISPLAY) {
		label(screen, "CO5300  /  480 x 480 RGB565", 18, 68, 444, &lv_font_montserrat_18, UI_MUTED);
		button(ui, "Next pattern", 18, 421, 444, 46, next_pattern, 0);
		ui->pattern_panel = box(screen, 18, 108, 444, 276, 0x000000, 0);
		ui->pattern = 0;
		draw_pattern(ui);
		return;
	}
	box(screen, 18, 76, 444, 330, UI_SURFACE, 5);
	ui->status = label(screen, "Reading...", 32, 86, 416, &lv_font_montserrat_18, UI_INK);
	lv_obj_set_height(ui->status, 304);
	lv_label_set_long_mode(ui->status, LV_LABEL_LONG_CLIP);
	switch (page) {
	case PAGE_KEYS:
		primary = "Test three pulses";
		break;
	case PAGE_AUDIO:
		primary = "Arm AI playback";
		secondary = "Record mic 1 s";
		break;
	case PAGE_WIFI:
		primary = "Check Wi-Fi link";
		break;
	case PAGE_BLUETOOTH:
		primary = "Check controller";
		break;
	case PAGE_USB:
		primary = "Device / PC";
		secondary = "Host / OTG hub";
		break;
	case PAGE_STORAGE:
		primary = "Check 64 KiB file";
		break;
	default:
		label(screen, "Live data  /  test resources released on exit", 18, 434, 444,
		      &lv_font_montserrat_14, UI_MUTED);
		break;
	}
	if (primary)
		button(ui, primary, 18, 421, secondary ? 216 : 444, 46, action, ACTION_PRIMARY);
	if (secondary)
		button(ui, secondary, 246, 421, 216, 46, action, ACTION_SECONDARY);
	if (page == PAGE_TOUCH) {
		for (unsigned int i = 0; i < CONTACTS; i++) {
			ui->markers[i] = box(screen, 0, 0, 26, 26, i ? UI_ACCENT : UI_GREEN, 13);
			lv_obj_add_flag(ui->markers[i], LV_OBJ_FLAG_HIDDEN);
		}
	}
}

void ui_init(struct ui *ui, struct tests *tests, enum page page)
{
	lv_obj_t *screen = lv_screen_active();

	memset(ui, 0, sizeof(*ui));
	ui->tests = tests;
	lv_obj_remove_style_all(screen);
	lv_obj_set_style_bg_color(screen, lv_color_hex(UI_PAPER), 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
	lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
	show_page(ui, page);
}

void ui_refresh(struct ui *ui)
{
	instruments_refresh(&ui->instruments);
	if (ui->status && strcmp(ui->previous, ui->tests->status)) {
		snprintf(ui->previous, sizeof(ui->previous), "%s", ui->tests->status);
		lv_label_set_text(ui->status, ui->previous);
	}
	for (unsigned int i = 0; i < CONTACTS; i++) {
		struct contact *contact = &ui->tests->inputs->contact[i];

		if (!ui->markers[i])
			continue;
		if (contact->down) {
			lv_obj_remove_flag(ui->markers[i], LV_OBJ_FLAG_HIDDEN);
			lv_obj_set_pos(ui->markers[i], LV_CLAMP(0, contact->x - 13, 454),
				       LV_CLAMP(60, contact->y - 13, 394));
		} else {
			lv_obj_add_flag(ui->markers[i], LV_OBJ_FLAG_HIDDEN);
		}
	}
}
