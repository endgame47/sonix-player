#include "corners.h"

#include <stdint.h>

#include "lvgl/lvgl.h"

#include "src/system/core/config.h"

#define CORNER_RADIUS 28
// Samples per pixel along each axis, for the antialiased edge of the arc.
#define CORNER_SAMPLES 4

enum { TOP_LEFT, TOP_RIGHT, BOTTOM_LEFT, BOTTOM_RIGHT };

// Alpha only (A8): LVGL draws such an image in its recolour, black by default,
// with the alpha as coverage -- a quarter of the memory of ARGB8888 and a plain
// fill rather than an image blend. Built, and the objects created, only once
// the option is on: with it off, which is the default, none of this is touched.
static lv_obj_t *corner[4];
static uint8_t pixels[4][CORNER_RADIUS * CORNER_RADIUS];
static lv_image_dsc_t image[4];
static bool enabled;

// The share of the pixel that lies outside the circle, as alpha. Worked out for
// the top-left corner, whose arc is centred on (CORNER_RADIUS, CORNER_RADIUS),
// and mirrored into the other three.
static void build_masks(void) {
	const float r = CORNER_RADIUS;
	for (int y = 0; y < CORNER_RADIUS; y++) {
		for (int x = 0; x < CORNER_RADIUS; x++) {
			int outside = 0;
			for (int sy = 0; sy < CORNER_SAMPLES; sy++) {
				for (int sx = 0; sx < CORNER_SAMPLES; sx++) {
					float dx = r - (x + (sx + 0.5f) / CORNER_SAMPLES);
					float dy = r - (y + (sy + 0.5f) / CORNER_SAMPLES);
					if (dx * dx + dy * dy > r * r) {
						outside++;
					}
				}
			}
			uint8_t alpha = (uint8_t)(outside * 255 / (CORNER_SAMPLES * CORNER_SAMPLES));

			int mx = CORNER_RADIUS - 1 - x;
			int my = CORNER_RADIUS - 1 - y;
			pixels[TOP_LEFT][y * CORNER_RADIUS + x] = alpha;
			pixels[TOP_RIGHT][y * CORNER_RADIUS + mx] = alpha;
			pixels[BOTTOM_LEFT][my * CORNER_RADIUS + x] = alpha;
			pixels[BOTTOM_RIGHT][my * CORNER_RADIUS + mx] = alpha;
		}
	}

	for (int i = 0; i < 4; i++) {
		image[i].header.magic = LV_IMAGE_HEADER_MAGIC;
		image[i].header.cf = LV_COLOR_FORMAT_A8;
		image[i].header.w = CORNER_RADIUS;
		image[i].header.h = CORNER_RADIUS;
		image[i].header.stride = CORNER_RADIUS;
		image[i].data_size = sizeof(pixels[i]);
		image[i].data = (const uint8_t *)pixels[i];
	}
}

// Anything created on the system layer later (the busy veils, the screenshot
// flash) would otherwise land on top of the corners, so they are moved back up
// whenever its children change. Moving them sends the same event again, hence
// the guard.
static void sys_layer_changed_cb(lv_event_t *e) {
	static bool moving;
	lv_obj_t *layer = lv_event_get_current_target(e);
	uint32_t count = lv_obj_get_child_count(layer);
	if (moving || !corner[3] || count < 4 || lv_obj_get_index(corner[3]) == (int32_t)count - 1) {
		return;
	}
	moving = true;
	for (int i = 0; i < 4; i++) {
		lv_obj_move_foreground(corner[i]);
	}
	moving = false;
}

// The masks and the four objects, the first time the option is on.
static void create_corners(void) {
	if (corner[0]) {
		return;
	}
	build_masks();

	static const lv_align_t align[4] = {LV_ALIGN_TOP_LEFT, LV_ALIGN_TOP_RIGHT, LV_ALIGN_BOTTOM_LEFT,
										LV_ALIGN_BOTTOM_RIGHT};
	lv_obj_t *layer = lv_layer_sys();
	for (int i = 0; i < 4; i++) {
		corner[i] = lv_image_create(layer);
		lv_image_set_src(corner[i], &image[i]);
		lv_obj_set_style_image_recolor(corner[i], lv_color_black(), 0);
		lv_obj_set_clickable(corner[i], false);
		lv_obj_set_ignore_layout(corner[i], true);
		lv_obj_align(corner[i], align[i], 0, 0);
	}
}

static void apply(void) {
	if (enabled) {
		create_corners();
	}
	for (int i = 0; i < 4 && corner[i]; i++) {
		lv_obj_set_hidden(corner[i], !enabled);
	}
}

void corners_init(gui_config_t *cfg) {
	(void)cfg;
	lv_obj_add_event_cb(lv_layer_sys(), sys_layer_changed_cb, LV_EVENT_CHILD_CHANGED, NULL);

	enabled = config_get_int("ui", "rounded_corners", 0) != 0;
	apply();
}

bool corners_enabled(void) { return enabled; }

void corners_set_enabled(bool on) {
	if (on == enabled) {
		return;
	}
	enabled = on;
	config_set_int("ui", "rounded_corners", on ? 1 : 0);
	config_save();
	apply();
}
