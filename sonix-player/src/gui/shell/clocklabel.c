#include "clocklabel.h"

#include <time.h>

#include "src/system/device/clock.h"

// Between the last figure and the suffix.
#define SUFFIX_GAP_PX 3

// The height of a figure as a share of the font's size, in hundredths: what
// the top of the suffix is lined up with.
#define FIGURE_HEIGHT_PCT 71

// How far down the top of a figure sits in a label set in `font`.
static int32_t figure_top(const lv_font_t *font, int px) { return (int32_t)(font->line_height - font->base_line) - (int32_t)(px * FIGURE_HEIGHT_PCT / 100); }

lv_obj_t *clocklabel_create(lv_obj_t *parent, const lv_font_t *font, int font_px, const lv_font_t *suffix_font, int suffix_px) {
	lv_obj_t *label = lv_label_create(parent);
	lv_obj_set_style_text_font(label, font, 0);
	// The suffix lives in the label's right padding, outside its text: the
	// label must not take that for something to scroll to.
	lv_obj_set_scrollable(label, false);

	// Its colour is the label's, inherited, so a style that tints the label
	// tints both.
	lv_obj_t *suffix = lv_label_create(label);
	// Floating: left out of the label's content size, which the right padding
	// already makes room for. Counted, it would widen the label by its own
	// width a second time and push the centred time off to the left.
	lv_obj_set_floating(suffix, true);
	lv_obj_set_style_text_font(suffix, suffix_font, 0);
	lv_obj_set_y(suffix, figure_top(font, font_px) - figure_top(suffix_font, suffix_px));
	lv_obj_set_hidden(suffix, true);
	return label;
}

void clocklabel_refresh(lv_obj_t *label) {
	if (!label) {
		return;
	}
	lv_obj_t *suffix = lv_obj_get_child(label, 0);

	char time_text[16] = "--:--";
	char suffix_text[8] = "";
	if (clock_is_set()) {
		time_t now = time(NULL);
		struct tm local;
		localtime_r(&now, &local);
		clock_format_hm_parts(time_text, sizeof(time_text), suffix_text, sizeof(suffix_text), local.tm_hour, local.tm_min);
	}
	lv_label_set_text(label, time_text);
	if (!suffix) {
		return;
	}

	if (!suffix_text[0]) {
		lv_obj_set_hidden(suffix, true);
		lv_obj_set_style_pad_right(label, 0, 0);
		return;
	}
	lv_label_set_text(suffix, suffix_text);
	lv_point_t time_size, suffix_size;
	lv_text_get_size(&time_size, time_text, lv_obj_get_style_text_font(label, 0), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	lv_text_get_size(&suffix_size, suffix_text, lv_obj_get_style_text_font(suffix, 0), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	lv_obj_set_x(suffix, time_size.x + SUFFIX_GAP_PX);
	lv_obj_set_style_pad_right(label, SUFFIX_GAP_PX + suffix_size.x, 0);
	lv_obj_set_hidden(suffix, false);
}
