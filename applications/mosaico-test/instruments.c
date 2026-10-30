// SPDX-License-Identifier: MIT
#include "instruments.h"
#include "theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265358979323846
#define STANDARD_GRAVITY 9.80665
#define RATE_RANGE 180.0
#define RATE_EXTENT 112

struct drawing {
	lv_layer_t *layer;
	int x, y;
};

static void line(struct drawing *drawing, double x1, double y1, double x2,
		 double y2, int width, uint32_t color)
{
	lv_draw_line_dsc_t dsc;

	lv_draw_line_dsc_init(&dsc);
	dsc.p1 = (lv_point_precise_t) { drawing->x + x1, drawing->y + y1 };
	dsc.p2 = (lv_point_precise_t) { drawing->x + x2, drawing->y + y2 };
	dsc.width = width;
	dsc.color = lv_color_hex(color);
	dsc.round_start = dsc.round_end = true;
	lv_draw_line(drawing->layer, &dsc);
}

static void circle(struct drawing *drawing, int x, int y, int radius,
		   uint32_t fill, uint32_t border, int width)
{
	lv_draw_rect_dsc_t dsc;
	lv_area_t area = { drawing->x + x - radius, drawing->y + y - radius,
			   drawing->x + x + radius, drawing->y + y + radius };

	lv_draw_rect_dsc_init(&dsc);
	dsc.radius = LV_RADIUS_CIRCLE;
	dsc.bg_color = lv_color_hex(fill);
	dsc.border_color = lv_color_hex(border);
	dsc.border_width = width;
	lv_draw_rect(drawing->layer, &dsc, &area);
}

static void text(struct drawing *drawing, const char *value, int x, int y,
		 int width, const lv_font_t *font, uint32_t color, lv_text_align_t align)
{
	lv_draw_label_dsc_t dsc;
	lv_area_t area = { drawing->x + x, drawing->y + y,
			   drawing->x + x + width - 1, drawing->y + y + 42 };

	lv_draw_label_dsc_init(&dsc);
	dsc.text = value;
	/* Draw tasks can outlive the callback's formatted stack strings. */
	dsc.text_local = true;
	dsc.font = font;
	dsc.color = lv_color_hex(color);
	dsc.align = align;
	lv_draw_label(drawing->layer, &dsc, &area);
}

static void dial(struct drawing *drawing, int cx, int cy, int radius)
{
	circle(drawing, cx, cy, radius, UI_PAPER, UI_RULE, 1);
	for (unsigned int i = 0; i < 24; i++) {
		double angle = i * PI / 12;
		int inner = radius - (i % 6 ? 5 : 10);

		line(drawing, cx + cos(angle) * inner, cy + sin(angle) * inner,
		     cx + cos(angle) * (radius - 1), cy + sin(angle) * (radius - 1),
		     1, i % 6 ? UI_RULE : UI_MUTED);
	}
}

static void draw_tilt(struct drawing *drawing, const struct sensor_vector *acceleration)
{
	const int cx = 105, cy = 91, radius = 78;
	double pitch, roll, gravity;

	dial(drawing, cx, cy, radius);
	line(drawing, cx - 55, cy, cx + 55, cy, 1, UI_RULE);
	line(drawing, cx, cy - 55, cx, cy + 55, 1, UI_RULE);
	circle(drawing, cx, cy, 22, UI_PAPER, UI_RULE, 1);
	if (sensors_tilt(acceleration, &pitch, &roll, &gravity)) {
		/* Project gravity into the sensor's XY plane, without integrating gyro drift. */
		int x = cx + lround(-acceleration->value[0] / gravity * 58);
		int y = cy + lround(acceleration->value[1] / gravity * 58);

		line(drawing, cx, cy, x, y, 2, UI_GREEN);
		circle(drawing, x, y, 9, UI_GREEN, UI_PAPER, 2);
		if (acceleration->value[2] < 0)
			text(drawing, "Inverted", 0, 174, 210, &lv_font_montserrat_14,
			     UI_ACCENT, LV_TEXT_ALIGN_CENTER);
		else
			text(drawing, "Gravity / XY", 0, 174, 210, &lv_font_montserrat_14,
			     UI_MUTED, LV_TEXT_ALIGN_CENTER);
	} else {
		text(drawing, "No tilt data", 33, 80, 144, &lv_font_montserrat_14,
		     UI_MUTED, LV_TEXT_ALIGN_CENTER);
	}
}

static int rate_length(const struct sensor_vector *rotation, unsigned int axis)
{
	if (!(rotation->valid & (1U << axis)))
		return 0;
	return lround(fmax(-1, fmin(1, rotation->value[axis] / RATE_RANGE)) * RATE_EXTENT);
}

static void rate_value(char *buffer, size_t size, const struct sensor_vector *rotation, unsigned int axis)
{
	if (rotation->valid & (1U << axis))
		snprintf(buffer, size, "%+.1f", rotation->value[axis]);
	else
		snprintf(buffer, size, "--");
}

static void draw_rates(struct drawing *drawing, const struct sensor_vector *rotation)
{
	static const uint32_t colors[] = { UI_ACCENT, UI_GREEN, UI_BLUE };
	const int center = 226;

	for (unsigned int i = 0; i < SENSOR_AXES; i++) {
		int y = 12 + i * 27;
		char label[32];

		snprintf(label, sizeof(label), "%c", "XYZ"[i]);
		text(drawing, label, 0, y - 9, 26, &lv_font_montserrat_14,
		     colors[i], LV_TEXT_ALIGN_LEFT);
		line(drawing, center - RATE_EXTENT, y, center + RATE_EXTENT, y, 2, UI_RULE);
		if (rate_length(rotation, i))
			line(drawing, center, y, center + rate_length(rotation, i), y, 5, colors[i]);
		rate_value(label, sizeof(label), rotation, i);
		line(drawing, center, y - 6, center, y + 6, 1, UI_MUTED);
		text(drawing, label, 348, y - 9, 84, &lv_font_montserrat_14,
		     UI_INK, LV_TEXT_ALIGN_RIGHT);
	}
}

static int field_angle(const struct sensor_vector *field)
{
	/* One degree is approximately one pixel at the tip of this dial. */
	return lround(atan2(field->value[1], field->value[0]) * 180 / PI);
}

static void needle(struct drawing *drawing, int cx, int cy, int angle)
{
	double dx = cos(angle * PI / 180), dy = -sin(angle * PI / 180);
	lv_draw_triangle_dsc_t dsc;

	line(drawing, cx - dx * 41, cy - dy * 41, cx, cy, 3, UI_MUTED);
	lv_draw_triangle_dsc_init(&dsc);
	dsc.color = lv_color_hex(UI_ACCENT);
	dsc.p[0] = (lv_point_precise_t) { drawing->x + cx + dx * 54,
					drawing->y + cy + dy * 54 };
	dsc.p[1] = (lv_point_precise_t) { drawing->x + cx - dy * 7,
					drawing->y + cy + dx * 7 };
	dsc.p[2] = (lv_point_precise_t) { drawing->x + cx + dy * 7,
					drawing->y + cy - dx * 7 };
	lv_draw_triangle(drawing->layer, &dsc);
	circle(drawing, cx, cy, 4, UI_INK, UI_PAPER, 1);
}

static void axis_value(char *buffer, size_t size, const struct sensor_vector *field, unsigned int axis)
{
	if (field->overflow & (1U << axis))
		snprintf(buffer, size, "%c  RANGE", "XYZ"[axis]);
	else if (field->valid & (1U << axis))
		snprintf(buffer, size, "%c  %+.1f uT", "XYZ"[axis], field->value[axis]);
	else
		snprintf(buffer, size, "%c  --", "XYZ"[axis]);
}

static void field_strength(char *buffer, size_t size, const struct sensors *sensors, unsigned int index)
{
	const struct sensor_vector *field = &sensors->magnetic[index];

	if (field->valid == SENSOR_ALL_AXES) {
		double magnitude = hypot(hypot(field->value[0], field->value[1]), field->value[2]);

		snprintf(buffer, size, "%.1f uT", magnitude);
	} else {
		snprintf(buffer, size, "%s", field->overflow ? "Sensor over range" :
			 index >= sensors->magnetic_count ? "Not available" : "Waiting for data");
	}
}

static void draw_field(struct drawing *drawing, const struct sensors *sensors, unsigned int index)
{
	const struct sensor_vector *field = &sensors->magnetic[index];
	char value[64];
	const int cx = 103, cy = 119;

	snprintf(value, sizeof(value), "BMM150 / %02u", index + 1);
	text(drawing, value, 0, 0, 206, &lv_font_montserrat_18, UI_INK, LV_TEXT_ALIGN_CENTER);
	dial(drawing, cx, cy, 67);
	text(drawing, "+Y", 82, 28, 42, &lv_font_montserrat_14, UI_MUTED, LV_TEXT_ALIGN_CENTER);
	text(drawing, "+X", 174, 111, 32, &lv_font_montserrat_14, UI_MUTED, LV_TEXT_ALIGN_CENTER);
	if ((field->valid & 3U) == 3U && hypot(field->value[0], field->value[1]) > 0.01) {
		needle(drawing, cx, cy, field_angle(field));
	} else {
		text(drawing, field->overflow & 3U ? "Over range" : "No XY data",
		     40, 110, 126, &lv_font_montserrat_14, UI_MUTED, LV_TEXT_ALIGN_CENTER);
	}
	field_strength(value, sizeof(value), sensors, index);
	text(drawing, value, 0, 200, 206, &lv_font_montserrat_18, UI_INK, LV_TEXT_ALIGN_CENTER);
	for (unsigned int i = 0; i < SENSOR_AXES; i++) {
		axis_value(value, sizeof(value), field, i);
		text(drawing, value, 16, 230 + i * 20, 174, &lv_font_montserrat_14,
		     field->overflow & (1U << i) ? UI_ACCENT : UI_MUTED, LV_TEXT_ALIGN_CENTER);
	}
}

static void draw(lv_event_t *event)
{
	struct instruments *view = lv_event_get_user_data(event);
	lv_obj_t *obj = lv_event_get_target_obj(event);
	lv_area_t area;
	struct drawing drawing = { .layer = lv_event_get_layer(event) };

	lv_obj_get_coords(obj, &area);
	drawing.x = area.x1;
	drawing.y = area.y1;
	if (obj == view->tilt)
		draw_tilt(&drawing, &view->sensors->acceleration);
	else if (obj == view->rates)
		draw_rates(&drawing, &view->sensors->rotation);
	else
		draw_field(&drawing, view->sensors, obj == view->field[1]);
}

static lv_obj_t *plot(struct instruments *view, int x, int y, int width, int height)
{
	lv_obj_t *obj = lv_obj_create(lv_screen_active());

	lv_obj_remove_style_all(obj);
	lv_obj_set_pos(obj, x, y);
	lv_obj_set_size(obj, width, height);
	lv_obj_set_style_bg_color(obj, lv_color_hex(UI_PAPER), 0);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
	lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(obj, draw, LV_EVENT_DRAW_MAIN, view);
	return obj;
}

static lv_obj_t *label(const char *text, int x, int y, int width, const lv_font_t *font, uint32_t color)
{
	lv_obj_t *obj = lv_label_create(lv_screen_active());

	lv_obj_set_pos(obj, x, y);
	lv_obj_set_width(obj, width);
	lv_obj_set_style_text_font(obj, font, 0);
	lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
	lv_label_set_text(obj, text);
	return obj;
}

void instruments_create(struct instruments *view, const struct sensors *sensors, bool magnetic)
{
	memset(view, 0, sizeof(*view));
	view->sensors = sensors;
	if (magnetic) {
		label("LOCAL MAGNETIC FIELD", 24, 82, 432, &lv_font_montserrat_14, UI_MUTED);
		view->field[0] = plot(view, 24, 120, 206, 292);
		view->field[1] = plot(view, 250, 120, 206, 292);
		label("Board magnets affect both sensors.", 24, 428, 432, &lv_font_montserrat_14, UI_MUTED);
		label("XY direction only / not calibrated north", 24, 450, 432, &lv_font_montserrat_14, UI_MUTED);
		return;
	}
	label("BMI270 / SENSOR AXES", 24, 82, 432, &lv_font_montserrat_14, UI_MUTED);
	view->tilt = plot(view, 24, 112, 222, 198);
	label("PITCH", 276, 126, 180, &lv_font_montserrat_14, UI_MUTED);
	view->pitch = label("--", 276, 148, 180, &lv_font_montserrat_24, UI_INK);
	label("ROLL", 276, 198, 180, &lv_font_montserrat_14, UI_MUTED);
	view->roll = label("--", 276, 220, 180, &lv_font_montserrat_24, UI_INK);
	view->gravity = label("Waiting for data", 276, 266, 180, &lv_font_montserrat_14, UI_MUTED);
	label("ANGULAR VELOCITY", 24, 316, 240, &lv_font_montserrat_14, UI_MUTED);
	label("+/-180 deg/s", 300, 316, 156, &lv_font_montserrat_14, UI_MUTED);
	view->rates = plot(view, 24, 342, 432, 82);
	view->note = label("Waiting for sensor data", 24, 449, 432, &lv_font_montserrat_14, UI_MUTED);
}

static void set_value(lv_obj_t *label, double value, const char *unit, bool valid)
{
	char text[64];

	if (valid)
		snprintf(text, sizeof(text), "%+.1f%s", value, unit);
	else
		snprintf(text, sizeof(text), "--");
	if (strcmp(lv_label_get_text(label), text))
		lv_label_set_text(label, text);
}

static void invalidate(lv_obj_t *obj, int x, int y, int width, int height)
{
	lv_area_t area;

	lv_obj_get_coords(obj, &area);
	area.x1 += x;
	area.y1 += y;
	area.x2 = area.x1 + width - 1;
	area.y2 = area.y1 + height - 1;
	lv_obj_invalidate_area(obj, &area);
}

static bool tilt_changed(const struct sensor_vector *a, const struct sensor_vector *b)
{
	double pitch, roll, ga, gb;
	bool va = sensors_tilt(a, &pitch, &roll, &ga);
	bool vb = sensors_tilt(b, &pitch, &roll, &gb);

	if (va != vb)
		return true;
	if (!va)
		return false;
	return (a->value[2] < 0) != (b->value[2] < 0) ||
	       lround(-a->value[0] / ga * 58) != lround(-b->value[0] / gb * 58) ||
	       lround(a->value[1] / ga * 58) != lround(b->value[1] / gb * 58);
}

static bool direction_changed(const struct sensor_vector *a, const struct sensor_vector *b)
{
	double na = hypot(a->value[0], a->value[1]);
	double nb = hypot(b->value[0], b->value[1]);
	bool va = (a->valid & 3U) == 3U && na > 0.01;
	bool vb = (b->valid & 3U) == 3U && nb > 0.01;

	if (va != vb || (a->overflow & 3U) != (b->overflow & 3U))
		return true;
	if (!va)
		return false;
	return field_angle(a) != field_angle(b);
}

static void refresh_rates(struct instruments *view)
{
	const struct sensor_vector *now = &view->sensors->rotation;
	const struct sensor_vector *before = &view->previous.rotation;
	char a[32], b[32];

	for (unsigned int i = 0; i < SENSOR_AXES; i++) {
		int y = 12 + i * 27;

		if (rate_length(now, i) != rate_length(before, i))
			invalidate(view->rates, 110, y - 7, 233, 15);
		rate_value(a, sizeof(a), now, i);
		rate_value(b, sizeof(b), before, i);
		if (strcmp(a, b))
			invalidate(view->rates, 348, y - 9, 84, 18);
	}
}

static void refresh_field(struct instruments *view, unsigned int index)
{
	const struct sensor_vector *now = &view->sensors->magnetic[index];
	const struct sensor_vector *before = &view->previous.magnetic[index];
	lv_obj_t *obj = view->field[index];
	char a[64], b[64];

	if (direction_changed(now, before))
		invalidate(obj, 38, 53, 131, 133);
	field_strength(a, sizeof(a), view->sensors, index);
	field_strength(b, sizeof(b), &view->previous, index);
	if (strcmp(a, b))
		invalidate(obj, 0, 200, 206, 22);
	for (unsigned int i = 0; i < SENSOR_AXES; i++) {
		axis_value(a, sizeof(a), now, i);
		axis_value(b, sizeof(b), before, i);
		if (strcmp(a, b))
			invalidate(obj, 16, 230 + i * 20, 174, 18);
	}
}

void instruments_refresh(struct instruments *view)
{
	const struct sensors *sensors = view->sensors;

	if (!sensors || (view->refreshed && !memcmp(sensors, &view->previous, sizeof(*sensors))))
		return;
	if (view->tilt) {
		double pitch = 0, roll = 0, gravity = 0;
		bool valid = sensors_tilt(&sensors->acceleration, &pitch, &roll, &gravity);
		const char *note = !sensors->motion_present ? "BMI270 not available" :
			!valid ? "Tilt unavailable / check accelerometer" :
			fabs(gravity - STANDARD_GRAVITY) > STANDARD_GRAVITY * 0.2 ?
			"Moving / acceleration also affects tilt" : "Tilt from gravity / rotate to check gyro";

		set_value(view->pitch, pitch, "\xc2\xb0", valid);
		set_value(view->roll, roll, "\xc2\xb0", valid);
		set_value(view->gravity, gravity / STANDARD_GRAVITY, " g", valid);
		if (strcmp(lv_label_get_text(view->note), note))
			lv_label_set_text(view->note, note);
		if (tilt_changed(&sensors->acceleration, &view->previous.acceleration))
			lv_obj_invalidate(view->tilt);
		refresh_rates(view);
	}
	for (unsigned int i = 0; i < MAGNETOMETERS; i++) {
		if (view->field[i])
			refresh_field(view, i);
	}
	memcpy(&view->previous, sensors, sizeof(*sensors));
	view->refreshed = true;
}
