#include "azindex.h"

#include <stdlib.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/device/power.h"

// The track lists' numbers, so the two strips are one design.
#define BUCKETS LIBRARY_INDEX_BUCKETS
#define BAR_WIDTH 28
#define HIDE_MS 2500 // no scrolling and no touch for this long: it goes
#define HINT_MS 450	 // the big letter outstays the finger by a moment
#define ENGAGE_PX 6	 // upward or downward movement that claims the press

static const char *const TEXT[BUCKETS] = {"#", "A", "B", "C", "D", "E", "F", "G", "H", "I",
										  "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S",
										  "T", "U", "V", "W", "X", "Y", "Z", "\xE2\x80\xA6"};

struct azindex {
	lv_obj_t *list;
	int row_pitch;
	void (*jumped)(void);

	lv_obj_t *bar;
	lv_obj_t *letters[BUCKETS];
	lv_obj_t *hint;
	lv_obj_t *hint_label;

	// The strip read from top to bottom -- already turned round on a Z-A list
	// -- and the row each letter jumps to.
	int row[BUCKETS];
	bool enabled;
	bool descending;

	uint32_t wanted_at;
	uint32_t hint_at;
	// Where a press on the strip started, and whether it has turned out to be
	// the strip's rather than the page's. Until it has, nothing moves.
	lv_point_t press;
	bool engaged;
};

// Every strip built, for the timer and the theme refresh. Pages build theirs
// once and keep them.
#define MAX_STRIPS 4
static azindex_t *strips[MAX_STRIPS];
static int strip_count;
// Runs only while a strip or a letter is up (timer_cb); whatever shows one
// starts it.
static lv_timer_t *timer;

static void timer_start(void) {
	if (timer) {
		lv_timer_resume(timer);
	}
}

static void show_bar(azindex_t *ix, bool shown) {
	if (shown) {
		lv_obj_set_hidden(ix->bar, false);
		lv_obj_move_foreground(ix->bar);
		timer_start();
	} else {
		lv_obj_set_hidden(ix->bar, true);
	}
}

void azindex_flash(azindex_t *ix) {
	if (!ix || !ix->enabled) {
		return;
	}
	ix->wanted_at = lv_tick_get();
	if (lv_obj_is_hidden(ix->bar)) {
		show_bar(ix, true);
	}
}

static void hint_show(azindex_t *ix, int slot) {
	int bucket = ix->descending ? BUCKETS - 1 - slot : slot;
	lv_label_set_text(ix->hint_label, TEXT[bucket]);
	lv_obj_set_hidden(ix->hint, false);
	lv_obj_move_foreground(ix->hint);
	ix->hint_at = lv_tick_get();
	timer_start();
}

void azindex_set_rows(azindex_t *ix, const int first[LIBRARY_INDEX_BUCKETS], int count, bool descending) {
	if (!ix) {
		return;
	}
	ix->enabled = first && count >= AZINDEX_MIN_ROWS;
	ix->descending = descending;
	show_bar(ix, false);
	lv_obj_set_hidden(ix->hint, true);
	if (!ix->enabled) {
		return;
	}

	// Top to bottom on screen, which is the alphabet turned round on a Z-A
	// list. The label texts follow the same order, so the strip always reads
	// the way the list under it runs.
	for (int slot = 0; slot < BUCKETS; slot++) {
		int bucket = descending ? BUCKETS - 1 - slot : slot;
		ix->row[slot] = first[bucket];
		lv_label_set_text(ix->letters[slot], TEXT[bucket]);
	}

	// A letter nothing starts with still has to answer: it lands on the first
	// row of the next letter that does, which is where those rows would be.
	// The ones past the last letter in use fall back to the previous.
	int next = -1;
	for (int slot = BUCKETS - 1; slot >= 0; slot--) {
		if (ix->row[slot] >= 0) {
			next = ix->row[slot];
		} else {
			ix->row[slot] = next;
		}
	}
	int previous = -1;
	for (int slot = 0; slot < BUCKETS; slot++) {
		if (ix->row[slot] >= 0) {
			previous = ix->row[slot];
		} else {
			ix->row[slot] = previous;
		}
	}
}

static void go(azindex_t *ix, int slot) {
	if (slot < 0) {
		slot = 0;
	}
	if (slot >= BUCKETS) {
		slot = BUCKETS - 1;
	}

	hint_show(ix, slot); // the letter shows even where there are no rows

	int row = ix->row[slot];
	if (row < 0) {
		return;
	}
	lv_obj_scroll_to_y(ix->list, row * ix->row_pitch, LV_ANIM_OFF);
	if (ix->jumped) {
		ix->jumped();
	}
}

// The slot the finger is over. Asked of the letters themselves rather than
// worked out from the strip's height: the labels are laid out with even gaps
// around them, so they do not quite fill the box, and arithmetic on the box
// answers a press near either end with the neighbouring letter.
static void go_at(azindex_t *ix, lv_point_t point) {
	for (int slot = 0; slot < BUCKETS; slot++) {
		lv_area_t area;
		lv_obj_get_coords(ix->letters[slot], &area);
		if (point.y <= area.y2) {
			go(ix, slot);
			return;
		}
	}
	go(ix, BUCKETS - 1); // past the last letter: the last letter
}

// The strip carries the page's own gestures as well -- the swipe back and the
// pull that brings the player in -- so a press on it does not commit to the
// alphabet until it is clear that is what it is. Sideways belongs to the page;
// up and down, or a tap that does not move at all, belongs to the strip.
static void bar_cb(lv_event_t *e) {
	azindex_t *ix = lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);

	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		ix->engaged = false;
		return;
	}

	lv_indev_t *indev = lv_indev_active();

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		if (code == LV_EVENT_RELEASED && !ix->engaged && indev) {
			// It never moved: a tap on a letter, acted on at the lift.
			lv_point_t point;
			lv_indev_get_point(indev, &point);
			go_at(ix, point);
		}
		ix->hint_at = lv_tick_get(); // from here the big letter fades out
		ix->engaged = false;
		return;
	}

	if (!indev) {
		return;
	}
	lv_point_t point;
	lv_indev_get_point(indev, &point);

	azindex_flash(ix); // the strip stays up for as long as a finger is on it

	if (code == LV_EVENT_PRESSED) {
		ix->press = point;
		ix->engaged = false;
		return;
	}
	if (code != LV_EVENT_PRESSING) {
		return;
	}

	if (!ix->engaged) {
		int dx = LV_ABS(point.x - ix->press.x);
		int dy = LV_ABS(point.y - ix->press.y);
		if (dx > dy || dy < ENGAGE_PX) {
			return; // sideways is the page's; too little to tell yet
		}
		ix->engaged = true;
	}
	go_at(ix, point);
}

// Takes the strip and the big letter away once nothing has asked for them for
// a moment, and stops itself once none is left.
static void timer_cb(lv_timer_t *t) {
	bool showing = false;
	for (int i = 0; i < strip_count; i++) {
		azindex_t *ix = strips[i];
		if (!lv_obj_is_hidden(ix->bar) && lv_tick_elaps(ix->wanted_at) > HIDE_MS) {
			show_bar(ix, false);
		}
		if (!lv_obj_is_hidden(ix->hint) && lv_tick_elaps(ix->hint_at) > HINT_MS) {
			lv_obj_set_hidden(ix->hint, true);
		}
		if (!lv_obj_is_hidden(ix->bar) || !lv_obj_is_hidden(ix->hint)) {
			showing = true;
		}
	}
	if (!showing) {
		lv_timer_pause(t);
	}
}

// The two hand-set colours; the letters follow their shared style on their own.
static void theme_refresh(void) {
	for (int i = 0; i < strip_count; i++) {
		lv_obj_set_style_bg_color(strips[i]->bar, theme()->surface, 0);
		lv_obj_set_style_bg_color(strips[i]->hint, theme()->accent, 0);
	}
}

azindex_t *azindex_create(lv_obj_t *screen, gui_config_t *cfg, lv_obj_t *list, int row_pitch,
						  void (*jumped)(void)) {
	if (strip_count >= MAX_STRIPS) {
		return NULL;
	}
	azindex_t *ix = calloc(1, sizeof(*ix));
	if (!ix) {
		return NULL;
	}
	ix->list = list;
	ix->row_pitch = row_pitch;
	ix->jumped = jumped;

	int content_top = settingsrow_content_top(cfg);

	ix->bar = lv_obj_create(screen);
	lv_obj_remove_style_all(ix->bar);
	lv_obj_set_size(ix->bar, BAR_WIDTH, cfg->screen_height - content_top - 8);
	lv_obj_align(ix->bar, LV_ALIGN_TOP_RIGHT, -2, content_top + 4);
	lv_obj_set_style_bg_color(ix->bar, theme()->surface, 0);
	lv_obj_set_style_bg_opa(ix->bar, LV_OPA_60, 0);
	lv_obj_set_style_radius(ix->bar, BAR_WIDTH / 2, 0);
	lv_obj_set_scrollable(ix->bar, false);
	lv_obj_set_hidden(ix->bar, true);
	lv_obj_set_flex_flow(ix->bar, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(ix->bar, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// The strip takes its own presses -- it must not scroll the list underneath
	// -- but it is still part of the page, so the swipe back and the pull that
	// brings the player in start on it as well; bar_cb tells them apart.
	lv_obj_set_clickable(ix->bar, true);
	lv_obj_add_event_cb(ix->bar, bar_cb, LV_EVENT_PRESSED, ix);
	lv_obj_add_event_cb(ix->bar, bar_cb, LV_EVENT_PRESSING, ix);
	lv_obj_add_event_cb(ix->bar, bar_cb, LV_EVENT_RELEASED, ix);
	lv_obj_add_event_cb(ix->bar, bar_cb, LV_EVENT_PRESS_LOST, ix);
	switcher_attach_back_gesture(ix->bar);
	player_sheet_attach_drag(ix->bar, true);

	for (int i = 0; i < BUCKETS; i++) {
		ix->letters[i] = lv_label_create(ix->bar);
		lv_label_set_text(ix->letters[i], TEXT[i]);
		lv_obj_add_style(ix->letters[i], &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(ix->letters[i], &font_ui_14, 0);
		lv_obj_set_clickable(ix->letters[i], false);
	}

	// The letter under the finger, in the middle of the screen where the hand
	// is not, in the accent colour so it stands off the cards under it.
	ix->hint = lv_obj_create(screen);
	lv_obj_remove_style_all(ix->hint);
	lv_obj_set_size(ix->hint, 132, 124);
	lv_obj_align(ix->hint, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_bg_color(ix->hint, theme()->accent, 0);
	lv_obj_set_style_bg_opa(ix->hint, LV_OPA_90, 0);
	lv_obj_set_style_radius(ix->hint, 26, 0);
	lv_obj_set_scrollable(ix->hint, false);
	lv_obj_set_clickable(ix->hint, false);
	lv_obj_set_hidden(ix->hint, true);

	ix->hint_label = lv_label_create(ix->hint);
	lv_label_set_text(ix->hint_label, "");
	lv_obj_set_style_text_font(ix->hint_label, &font_ui_64_bold, 0);
	lv_obj_set_style_text_color(ix->hint_label, lv_color_white(), 0);
	lv_obj_center(ix->hint_label);

	strips[strip_count++] = ix;
	if (!timer) {
		timer = lv_timer_create(timer_cb, 200, NULL);
		lv_timer_pause(timer); // nothing to take away until a strip is shown
		power_pause_in_standby(timer);
		theme_register_refresh(theme_refresh);
	}
	return ix;
}
