#include "settingsrow.h"

// LVGL 9.2 made lv_event_t opaque and moved its fields here. This file swaps
// e->user_data around a wrapped callback, which is a write, and there is no
// public setter for it.
#include "lvgl/src/misc/lv_event_private.h"

#include "lvgl/lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

// Matches the floating back button, which the heading sits beside.
#define CORNER_BTN_SIZE 56
// Gap pages leave between two corner buttons: 6 px where they are placed by
// slot, 4 px in the flex shelf medialist uses. The larger value is the one to
// reserve, since underestimating it lets a heading run under a button.
#define CORNER_SLOT_GAP 6

#define ROW_HEIGHT 100

// A toggle card that also carries a slider, at its open height.
#define TOGGLE_SLIDER_OPEN_H 180

// Stepped slider: a thin round track with a small mark on each intermediate
// value. Slightly thicker than Adwaita's own trough: on a 480 px panel held at
// arm's length, 6 px is a hairline with no room for the step marks.
#define SLIDER_TRACK_HEIGHT 10
#define SLIDER_TICK_WIDTH 3
// Marks sit inside the track, so they are shorter than it and centred on it.
#define SLIDER_TICK_HEIGHT 6
// Past this many positions the marks stop helping: on a 400 px track forty
// notches read as a dotted line, not a scale. Finer sliders (Soundfield width,
// 0.00 to 2.00 in steps of 0.05) go without.
#define SLIDER_TICK_MAX_STEPS 16
#define ROW_RADIUS 12 // Adwaita boxed-list corner radius
#define ROW_GAP 8

// Every page heading, row name and card name goes through this file, so the
// tr() calls live here rather than in thirty call sites. A name that is not a
// known key (a user-typed preset, an SSID) comes back from tr() unchanged.
static void fit_title(lv_obj_t *label, int width);

lv_obj_t *settingsrow_title(lv_obj_t *screen, gui_config_t *cfg, const char *text) {
	lv_obj_t *label = lv_label_create(screen);
	lv_label_set_text(label, tr(text));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_32, 0);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

	// Beside the back button, not above it: the two read as one header row.
	int left = cfg->padding + CORNER_BTN_SIZE + 14;
	lv_obj_set_width(label, cfg->screen_width - left - cfg->padding - CORNER_BTN_SIZE - 14);
	// Always one line. LV_LABEL_LONG_DOT wraps first and puts the ellipsis at
	// the end of the last line that fits, so a long album name would grow the
	// header to two lines; pinning the height to one line makes it truncate
	// instead.
	lv_obj_set_height(label, lv_font_get_line_height(&font_ui_32));
	fit_title(label, cfg->screen_width - left - cfg->padding - CORNER_BTN_SIZE - 14);
	lv_obj_align(label, LV_ALIGN_TOP_LEFT, left, cfg->top_bar_height + cfg->padding + 10);

	return label;
}

int settingsrow_content_top(gui_config_t *cfg) {
	return cfg->top_bar_height + cfg->padding + CORNER_BTN_SIZE + cfg->padding;
}

// Every page built by settingsrow_page() reopens scrolled to the top; a long
// page would otherwise stay wherever it was last left.
//
// The reset runs on unload, not on load. On load it would race: many pages
// rebuild their content inside LV_EVENT_SCREEN_LOADED, and LVGL calls callbacks
// in registration order, so this one -- registered inside settingsrow_page() --
// would run before the content exists. On unload the page is already off
// screen, so it scrolls back invisibly.
//
// The sheet player needs no handling: it opens on the top layer without a
// screen change, so the page beneath is never unloaded and keeps its position.
static void page_scroll_top_cb(lv_event_t *e) {
	lv_obj_t *container = (lv_obj_t *)lv_event_get_user_data(e);
	if (container) {
		lv_obj_scroll_to_y(container, 0, LV_ANIM_OFF);
	}
}

lv_obj_t *settingsrow_page(lv_obj_t *screen, gui_config_t *cfg, const char *title) {
	lv_obj_add_style(screen, &theme_style_screen, 0);

	settingsrow_title(screen, cfg, title);

	int top = settingsrow_content_top(cfg);

	lv_obj_t *container = lv_obj_create(screen);
	lv_obj_set_size(container, lv_pct(100), cfg->screen_height - top);
	lv_obj_align(container, LV_ALIGN_TOP_LEFT, 0, top);
	lv_obj_set_style_bg_opa(container, 0, 0);
	lv_obj_set_style_border_width(container, 0, 0);
	lv_obj_set_style_radius(container, 0, 0);
	lv_obj_set_style_pad_hor(container, cfg->padding, 0);
	lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(container, ROW_GAP, 0);

	// Settings pages do not pull the player sheet in: a stray horizontal drag
	// would drag it over the option being adjusted. The swipe-back gesture must
	// still be able to start on the page body.
	switcher_attach_back_gesture(container);

	lv_obj_add_event_cb(screen, page_scroll_top_cb, LV_EVENT_SCREEN_UNLOADED, container);

	return container;
}

// Steps the heading font down instead of truncating it.
//
// Two corner buttons leave a track list 246 px, which is not enough for a
// heading like "Tous les morceaux" at 32 px. The size steps down until the
// words fit; only a name too long even at the smallest step (an album title out
// of a tag, say) still gets the ellipsis.
static void fit_title(lv_obj_t *label, int width) {
	// Down to 24, because headings such as "Opzioni di visualizzazione" do not
	// fit at 26 either, and an ellipsis in a page title tells the reader
	// nothing.
	static lv_font_t *const STEPS[] = {&font_ui_32, &font_ui_28, &font_ui_26, &font_ui_24};

	// The text must be sized with the ellipsis out of the way: a
	// LV_LABEL_LONG_DOT label does not keep the original string, so once LVGL
	// has shortened it lv_label_get_text() returns the truncated form, which
	// fits, and the step-down would never trigger. Setting the text to NULL is
	// LVGL's own "restore and refresh" and puts the covered characters back;
	// changing the long mode only asks for a refresh that has not happened yet
	// when the text is read on the next line.
	lv_label_set_text(label, NULL);

	const char *text = lv_label_get_text(label);
	if (!text || !text[0] || width <= 0) {
		return;
	}

	for (size_t i = 0; i < sizeof(STEPS) / sizeof(STEPS[0]); i++) {
		lv_point_t size;
		lv_text_get_size(&size, text, STEPS[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		if (size.x <= width || i + 1 == sizeof(STEPS) / sizeof(STEPS[0])) {
			lv_obj_set_style_text_font(label, STEPS[i], 0);
			lv_obj_set_height(label, lv_font_get_line_height(STEPS[i]));
			break;
		}
	}
}

// The same treatment as fit_title(), for a row name.
//
// Needed by verbose languages: the German "Auf Werkseinstellungen
// zuruecksetzen" reaches the card's inner edge exactly at 24 px, so any
// slightly longer entry is cut off by the border.
//
// Anything that already fits is left alone: the size drops only when needed,
// like the page title, so rows in other languages are untouched.
static void fit_row_label(lv_obj_t *label, int32_t width) {
	static lv_font_t *const STEPS[] = {&font_ui_24, &font_ui_22, &font_ui_20};
	const size_t count = sizeof(STEPS) / sizeof(STEPS[0]);

	const char *text = lv_label_get_text(label);
	if (!text || !text[0] || width <= 0) {
		return;
	}

	for (size_t i = 0; i < count; i++) {
		lv_point_t size;
		lv_text_get_size(&size, text, STEPS[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		if (size.x <= width || i + 1 == count) {
			// Only when it actually changes: setting the same font again
			// retriggers layout, and this runs from a layout event.
			if (lv_obj_get_style_text_font(label, LV_PART_MAIN) != STEPS[i]) {
				lv_obj_set_style_text_font(label, STEPS[i], 0);
			}
			return;
		}
	}
}

// Space left between the row name and whatever sits to its right.
#define ROW_LABEL_GAP 12

// A row's real width is known only after the column layout has placed it, not
// while it is being built.
static void fit_row(lv_obj_t *row) {
	if (!row || lv_obj_get_child_count(row) == 0) {
		return;
	}

	lv_obj_t *label = lv_obj_get_child(row, 0);
	if (!label || !lv_obj_check_type(label, &lv_label_class)) {
		return;
	}

	int32_t avail = lv_obj_get_content_width(row);

	// The right side holds either the chevron or the value, and neither space
	// belongs to the name. The value counts as much as the chevron: a name like
	// "Version du systeme d'exploitation" at 24 px runs straight into the
	// number beside it.
	lv_obj_t *right = lv_obj_get_child(row, 1);
	if (right) {
		avail -= lv_obj_get_width(right) + ROW_LABEL_GAP;
	}

	fit_row_label(label, avail);
}

static void row_resized_cb(lv_event_t *e) { fit_row(lv_event_get_target(e)); }

// The value can change after the row is built (versions, free card space), and
// changing it changes how much width is left for the name.
static void row_value_resized_cb(lv_event_t *e) { fit_row((lv_obj_t *)lv_event_get_user_data(e)); }

// Reduces the heading width on pages that keep more than one control in the
// top right corner.
//
// settingsrow_title() reserves room for exactly one button, which is not enough
// for the Music page (four) or for a track list (up to three): a long heading
// runs straight under the buttons.
void settingsrow_title_corner_slots(lv_obj_t *title, gui_config_t *cfg, int buttons) {
	if (!title || buttons < 0) {
		return;
	}
	int left = cfg->padding + CORNER_BTN_SIZE + 14;
	// Zero is a real answer and not "leave it alone": a page with no corner
	// buttons gets the whole width, which is the difference between a heading
	// that reads and one that ends in an ellipsis.
	int reserved = buttons > 0 ? buttons * CORNER_BTN_SIZE + (buttons - 1) * CORNER_SLOT_GAP : 0;
	int width = cfg->screen_width - left - cfg->padding - reserved - 14;
	lv_obj_set_width(title, width);
	fit_title(title, width);
}

// Returns the heading settingsrow_page() created, for pages whose title is not
// fixed text. Found by class rather than by index, so callers do not depend on
// the order in which this file creates children.
lv_obj_t *settingsrow_page_title(lv_obj_t *screen) {
	uint32_t count = lv_obj_get_child_count(screen);
	for (uint32_t i = 0; i < count; i++) {
		lv_obj_t *child = lv_obj_get_child(screen, (int32_t)i);
		if (lv_obj_check_type(child, &lv_label_class)) {
			return child;
		}
	}
	return NULL;
}

// Shared shell for the richer rows: the same card, tall enough for a control
// underneath the name.
static lv_obj_t *make_card(lv_obj_t *parent, const char *name, lv_obj_t **value_out, int height) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, height);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, ROW_RADIUS, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_hor(card, 20, 0);
	lv_obj_set_style_pad_ver(card, 14, 0);
	lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	// As on the plain rows, presses on the card body reach the page container
	// so the swipe-back drag can start on a toggle or slider card too; the
	// control inside keeps its own presses.
	lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);

	lv_obj_t *label = lv_label_create(card);
	lv_label_set_text(label, tr(name));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

	if (value_out) {
		*value_out = lv_label_create(card);
		lv_obj_add_style(*value_out, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(*value_out, &font_ui_24, 0);
		lv_obj_align(*value_out, LV_ALIGN_TOP_RIGHT, 0, 0);
	}

	return card;
}

// Every stepped slider this file has built, so a theme change can repaint
// their knob rings: the ring colour depends on the theme, and these pages are
// built once at startup.
#define SETTINGSROW_MAX_SLIDERS 16
static lv_obj_t *built_sliders[SETTINGSROW_MAX_SLIDERS];
static int built_slider_count;

static void settingsrow_refresh_theme(void) {
	for (int i = 0; i < built_slider_count; i++) {
		theme_apply_slider_knob(built_sliders[i]);
	}
}

// The Adwaita slider shared by every card that carries one: thin round trough,
// accent fill, plain white knob lifted by a shadow.
static lv_obj_t *make_slider(lv_obj_t *card, int steps, lv_event_cb_t cb) {
	lv_obj_t *slider = lv_slider_create(card);
	lv_obj_set_width(slider, lv_pct(100));
	lv_obj_set_height(slider, SLIDER_TRACK_HEIGHT);
	lv_slider_set_range(slider, 0, steps > 1 ? steps - 1 : 1);

	// With a 0..steps-1 range the slider lands on whole steps: the value is an
	// index into a list of choices, not a continuous number.
	lv_slider_set_mode(slider, LV_SLIDER_MODE_NORMAL);

	// The trough must read against the card it sits on: surface_pressed is
	// nearly the card colour itself, so a mid grey derived from the secondary
	// text is used instead, which works in both themes.
	//
	// Shared styles rather than hand-set colours: a colour set here would freeze
	// the accent that was live when the page was built, and these pages are
	// built once at startup.
	lv_obj_add_style(slider, &theme_style_slider_track, LV_PART_MAIN);

	lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_add_style(slider, &theme_style_accent_bg, LV_PART_INDICATOR);

	lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
	lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
	theme_apply_slider_knob(slider);
	if (built_slider_count < SETTINGSROW_MAX_SLIDERS) {
		if (built_slider_count == 0) {
			theme_register_refresh(settingsrow_refresh_theme);
		}
		built_sliders[built_slider_count++] = slider;
	}
	lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(slider, 8, LV_PART_KNOB);
	lv_obj_set_style_shadow_opa(slider, LV_OPA_30, LV_PART_KNOB);
	lv_obj_set_style_shadow_color(slider, lv_color_black(), LV_PART_KNOB);
	lv_obj_set_style_shadow_offset_y(slider, 1, LV_PART_KNOB);

	if (cb) {
		lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, NULL);
	}
	return slider;
}

// A mark on every value the slider can stop on except the two ends, which the
// track itself already shows. The slider must already be laid out, since the
// marks are positioned from its geometry.
static void make_ticks(lv_obj_t *parent, lv_obj_t *slider, int steps, int y) {
	int track_x = lv_obj_get_x(slider);
	int track_w = lv_obj_get_width(slider);

	if (steps > SLIDER_TICK_MAX_STEPS) {
		return;
	}

	for (int i = 1; i < steps - 1; i++) {
		lv_obj_t *tick = lv_obj_create(parent);
		lv_obj_set_size(tick, SLIDER_TICK_WIDTH, SLIDER_TICK_HEIGHT);
		lv_obj_set_pos(tick, track_x + ((track_w - SLIDER_TICK_WIDTH) * i) / (steps - 1), y);
		lv_obj_set_style_radius(tick, LV_RADIUS_CIRCLE, 0);
		// Inside the track, so they read as notches on the scale rather than a
		// second row of dots below it: a light mark visible on the accent fill
		// and on the grey trough alike.
		lv_obj_set_style_bg_color(tick, lv_color_white(), 0);
		lv_obj_set_style_bg_opa(tick, LV_OPA_60, 0);
		lv_obj_set_style_border_width(tick, 0, 0);
		lv_obj_remove_flag(tick, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_remove_flag(tick, LV_OBJ_FLAG_SCROLLABLE);
	}
}

lv_obj_t *settingsrow_slider(lv_obj_t *parent, const char *name, int steps, lv_obj_t **value_out,
							 lv_obj_t **slider_out, lv_event_cb_t cb) {
	lv_obj_t *card = make_card(parent, name, value_out, 140);

	lv_obj_t *slider = make_slider(card, steps, cb);
	lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -18);

	lv_obj_update_layout(card);
	make_ticks(card, slider, steps, lv_obj_get_y(slider) + (SLIDER_TRACK_HEIGHT - SLIDER_TICK_HEIGHT) / 2);

	if (slider_out) {
		*slider_out = slider;
	}
	return card;
}

// The switch and the gap the name must not reach into. 68 is the switch, 16 is
// the space between the two.
#define TOGGLE_RESERVED (68 + 16)

lv_obj_t *settingsrow_toggle(lv_obj_t *parent, const char *name, lv_obj_t **switch_out, lv_event_cb_t cb) {
	lv_obj_t *card = make_card(parent, name, NULL, ROW_HEIGHT);

	// The name sits on the row midline like every other option row; pages that
	// add a subtitle move it up themselves.
	lv_obj_t *label = lv_obj_get_child(card, 0);
	lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

	// And it stops before the switch: a label free to be as wide as its text
	// runs under the switch and off the right edge of the card as soon as a
	// name is a whole sentence. Two lines are the most a row of this height
	// holds, which is enough for every name in every language the player ships.
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_max_width(label, lv_pct(100), 0);
	lv_obj_set_style_pad_right(label, TOGGLE_RESERVED, 0);

	lv_obj_t *toggle = lv_switch_create(card);
	lv_obj_set_size(toggle, 68, 36);
	lv_obj_align(toggle, LV_ALIGN_RIGHT_MID, 0, 0);
	// Shared theme styles rather than colours frozen at creation: a hand-set
	// colour would keep the palette the row was built under, leaving off-toggles
	// white after a switch to the dark theme.
	lv_obj_add_style(toggle, &theme_style_switch, LV_PART_MAIN);
	lv_obj_add_style(toggle, &theme_style_switch_checked, LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_add_event_cb(toggle, cb, LV_EVENT_VALUE_CHANGED, NULL);

	if (switch_out) {
		*switch_out = toggle;
	}
	return card;
}

// The parts of a toggle+slider card, stored on the card itself so expanding
// and collapsing it is one call rather than six.
typedef struct {
	lv_obj_t *name;
	lv_obj_t *toggle;
	lv_obj_t *value;
	lv_obj_t *slider;
	lv_obj_t *ticks; // step marks in one box, so they hide as a group
	int closed_h;
	int open_h;
} toggle_slider_t;

void settingsrow_toggle_slider_expanded(lv_obj_t *card, bool expanded) {
	toggle_slider_t *ts = card ? lv_obj_get_user_data(card) : NULL;
	if (!ts) {
		return;
	}

	if (expanded) {
		lv_obj_set_height(card, ts->open_h);
		lv_obj_remove_flag(ts->slider, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ts->ticks, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ts->value, LV_OBJ_FLAG_HIDDEN);
		// Name and switch move up to the card's first line to make room for
		// the slider below them.
		lv_obj_align(ts->name, LV_ALIGN_TOP_LEFT, 0, 0);
		lv_obj_align(ts->toggle, LV_ALIGN_TOP_RIGHT, 0, -4);
	} else {
		lv_obj_set_height(card, ts->closed_h);
		lv_obj_add_flag(ts->slider, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ts->ticks, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ts->value, LV_OBJ_FLAG_HIDDEN);
		// Back to an ordinary toggle row.
		lv_obj_align(ts->name, LV_ALIGN_LEFT_MID, 0, 0);
		lv_obj_align(ts->toggle, LV_ALIGN_RIGHT_MID, 0, 0);
	}
}

lv_obj_t *settingsrow_toggle_slider(lv_obj_t *parent, const char *name, int steps, lv_obj_t **switch_out,
									lv_obj_t **value_out, lv_obj_t **slider_out, lv_event_cb_t toggle_cb,
									lv_event_cb_t slider_cb) {
	// Built at its open height so the slider and its marks land in the right
	// places; the caller collapses it afterwards if the switch is off. The
	// value sits under the name rather than beside it, because the switch
	// already owns the top right corner.
	lv_obj_t *card = make_card(parent, name, NULL, TOGGLE_SLIDER_OPEN_H);

	lv_obj_t *name_label = lv_obj_get_child(card, 0);
	lv_obj_align(name_label, LV_ALIGN_TOP_LEFT, 0, 0);

	lv_obj_t *toggle = lv_switch_create(card);
	lv_obj_set_size(toggle, 68, 36);
	lv_obj_align(toggle, LV_ALIGN_TOP_RIGHT, 0, -4);
	lv_obj_add_style(toggle, &theme_style_switch, LV_PART_MAIN);
	lv_obj_add_style(toggle, &theme_style_switch_checked, LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_add_event_cb(toggle, toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);

	lv_obj_t *value = lv_label_create(card);
	lv_obj_add_style(value, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(value, &font_ui_22, 0);
	lv_obj_align(value, LV_ALIGN_TOP_LEFT, 0, 42);

	lv_obj_t *slider = make_slider(card, steps, slider_cb);
	lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -10);
	lv_obj_set_ext_click_area(slider, 20);

	// Step marks as on the plain slider card; without them the control stops
	// reading as a slider with a handful of positions. They go in a
	// transparent box of their own so the card can hide them as a group.
	lv_obj_update_layout(card);

	lv_obj_t *ticks = lv_obj_create(card);
	lv_obj_set_size(ticks, lv_obj_get_width(slider), SLIDER_TICK_HEIGHT);
	// Centred on the track, like settingsrow_slider: parked below the trough
	// instead, the marks would be left floating under the slider on a card that
	// expands and collapses.
	lv_obj_set_pos(ticks, lv_obj_get_x(slider), lv_obj_get_y(slider) + (SLIDER_TRACK_HEIGHT - SLIDER_TICK_HEIGHT) / 2);
	lv_obj_set_style_bg_opa(ticks, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(ticks, 0, 0);
	lv_obj_set_style_pad_all(ticks, 0, 0);
	lv_obj_remove_flag(ticks, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(ticks, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_update_layout(ticks);

	for (int i = 1; i < steps - 1 && steps <= SLIDER_TICK_MAX_STEPS; i++) {
		lv_obj_t *tick = lv_obj_create(ticks);
		lv_obj_set_size(tick, SLIDER_TICK_WIDTH, SLIDER_TICK_HEIGHT);
		lv_obj_set_pos(tick, ((lv_obj_get_width(slider) - SLIDER_TICK_WIDTH) * i) / (steps - 1), 0);
		lv_obj_set_style_radius(tick, LV_RADIUS_CIRCLE, 0);
		// Same light mark as the plain slider card: it must show on the accent
		// fill and on the grey trough alike.
		lv_obj_set_style_bg_color(tick, lv_color_white(), 0);
		lv_obj_set_style_bg_opa(tick, LV_OPA_60, 0);
		lv_obj_set_style_border_width(tick, 0, 0);
		lv_obj_remove_flag(tick, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_remove_flag(tick, LV_OBJ_FLAG_SCROLLABLE);
	}

	toggle_slider_t *ts = malloc(sizeof(*ts));
	if (ts) {
		ts->name = name_label;
		ts->toggle = toggle;
		ts->value = value;
		ts->slider = slider;
		ts->ticks = ticks;
		ts->closed_h = ROW_HEIGHT;
		ts->open_h = TOGGLE_SLIDER_OPEN_H;
		lv_obj_set_user_data(card, ts);
	}

	if (switch_out) {
		*switch_out = toggle;
	}
	if (value_out) {
		*value_out = value;
	}
	if (slider_out) {
		*slider_out = slider;
	}
	return card;
}

// Row click actions go through this shim so that a swipe-back drag ending on a
// row does not also activate it, the same contract as the library lists.
typedef struct {
	lv_event_cb_t cb;
	void *user_data;
} row_action_t;

static void row_clicked_cb(lv_event_t *e) {
	row_action_t *action = lv_event_get_user_data(e);
	if (!action || !action->cb) {
		return;
	}
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return; // a swipe across the row, not a tap on it
	}
	// Swap in the caller's user data so the wrapped callback sees what it was
	// registered with, then restore it for the next event.
	e->user_data = action->user_data;
	action->cb(e);
	e->user_data = action;
}

// A row's chevron, if it has one: the only image among its children.
static lv_obj_t *row_chevron(lv_obj_t *row) {
	if (!row) {
		return NULL;
	}
	uint32_t count = lv_obj_get_child_count(row);
	for (uint32_t i = 0; i < count; i++) {
		lv_obj_t *child = lv_obj_get_child(row, (int32_t)i);
		if (lv_obj_check_type(child, &lv_image_class)) {
			return child;
		}
	}
	return NULL;
}

void settingsrow_chevron_active(lv_obj_t *row, bool active) {
	lv_obj_t *chev = row_chevron(row);
	if (!chev) {
		return;
	}
	if (active) {
		// Adwaita green, the "this one is on" colour, at full strength so it
		// stands out against the muted grey of every other row.
		lv_obj_set_style_image_recolor(chev, lv_color_make(46, 194, 126), 0);
		lv_obj_set_style_image_recolor_opa(chev, LV_OPA_COVER, 0);
		lv_obj_set_style_image_opa(chev, LV_OPA_COVER, 0);
	} else {
		lv_obj_remove_local_style_prop(chev, LV_STYLE_IMAGE_RECOLOR, 0);
		lv_obj_remove_local_style_prop(chev, LV_STYLE_IMAGE_RECOLOR_OPA, 0);
		lv_obj_set_style_image_opa(chev, LV_OPA_60, 0);
	}
}

lv_obj_t *settingsrow_action(lv_obj_t *parent, const char *name, lv_event_cb_t cb, void *user_data) {
	lv_obj_t *row = settingsrow_add(parent, name, NULL, cb, user_data);
	// An action row opens no page, so its chevron would be misleading.
	lv_obj_t *chev = row_chevron(row);
	if (chev) {
		lv_obj_delete(chev);
	}
	return row;
}

lv_obj_t *settingsrow_add(lv_obj_t *parent, const char *name, lv_obj_t **value_out, lv_event_cb_t cb,
						  void *user_data) {
	lv_obj_t *row = lv_btn_create(parent);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, ROW_HEIGHT);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, ROW_RADIUS, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 20, 0);
	// Presses bubble up to the page container, so the swipe-back drag can start
	// on a row and not only in the gaps between rows.
	lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);

	// Rows live for the whole run, so these action records are never freed.
	row_action_t *action = malloc(sizeof(*action));
	if (action) {
		action->cb = cb;
		action->user_data = user_data;
		lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, action);
	}

	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text(label, tr(name));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

	// The name steps down a size if it does not fit; see fit_row_label(). Hooked
	// to the resize event rather than called now, because the row has no width
	// yet at this point.
	lv_obj_add_event_cb(row, row_resized_cb, LV_EVENT_SIZE_CHANGED, NULL);

	if (value_out) {
		*value_out = lv_label_create(row);
		lv_obj_add_style(*value_out, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(*value_out, &font_ui_24, 0);
		lv_obj_align(*value_out, LV_ALIGN_RIGHT_MID, 0, 0);
		lv_obj_add_event_cb(*value_out, row_value_resized_cb, LV_EVENT_SIZE_CHANGED, row);
	} else {
		// A plain row opens another page, which the chevron announces; rows
		// with a value use their right edge for the value instead.
		lv_obj_t *chev = lv_image_create(row);
		lv_image_set_src(chev, &icon_chevron_right);
		lv_obj_add_style(chev, &theme_style_icon, 0);
		lv_obj_set_style_image_opa(chev, LV_OPA_60, 0);
		lv_obj_align(chev, LV_ALIGN_RIGHT_MID, 0, 0);
	}

	return row;
}

// The name label is the first thing settingsrow_add() puts in a row, and the
// only child a caller could want back.
lv_obj_t *settingsrow_name_label(lv_obj_t *row) { return row ? lv_obj_get_child(row, 0) : NULL; }

// ---------------------------------------------------------------------------
// Pill cards
// ---------------------------------------------------------------------------

// A toggle card with its choices on pills underneath, shown only while the
// switch is on. The same shape the audiobook options page uses, and here for
// the same reason: "track or album" means nothing while ReplayGain is off.
lv_obj_t *settingsrow_toggle_pills(lv_obj_t *parent, const char *title, lv_event_cb_t toggle_cb,
								   lv_obj_t **switch_out, lv_obj_t **pills_out) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 20, 0);
	lv_obj_set_style_pad_row(card, 18, 0);
	lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *head = lv_obj_create(card);
	lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(head, 0, 0);
	lv_obj_set_style_border_width(head, 0, 0);
	lv_obj_set_style_pad_all(head, 0, 0);
	lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(head, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(head, 12, 0);

	lv_obj_t *name = lv_label_create(head);
	lv_label_set_text(name, tr(title));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);
	// The name takes what is left after the switch and wraps inside it. Without
	// this a long title -- "Compensazione guadagno DSD" in Italian, longer still
	// in German -- runs straight under the switch.
	lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
	lv_obj_set_flex_grow(name, 1);

	lv_obj_t *toggle = lv_switch_create(head);
	lv_obj_set_size(toggle, 68, 36);
	lv_obj_add_style(toggle, &theme_style_switch, LV_PART_MAIN);
	lv_obj_add_style(toggle, &theme_style_switch_checked, LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_add_event_cb(toggle, toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);

	lv_obj_t *pills = lv_obj_create(card);
	lv_obj_set_size(pills, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(pills, 0, 0);
	lv_obj_set_style_border_width(pills, 0, 0);
	lv_obj_set_style_pad_all(pills, 0, 0);
	lv_obj_set_style_pad_gap(pills, 12, 0);
	lv_obj_remove_flag(pills, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(pills, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(pills, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	*switch_out = toggle;
	*pills_out = pills;
	return card;
}

// The same card without the switch, for a choice that is always one of its
// options rather than something that can be off: the player layout is one, and
// so is the repeat mode.
lv_obj_t *settingsrow_pills(lv_obj_t *parent, const char *title, lv_obj_t **pills_out) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 20, 0);
	lv_obj_set_style_pad_row(card, 18, 0);
	lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *name = lv_label_create(card);
	lv_label_set_text(name, tr(title));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);

	lv_obj_t *pills = lv_obj_create(card);
	lv_obj_set_size(pills, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(pills, 0, 0);
	lv_obj_set_style_border_width(pills, 0, 0);
	lv_obj_set_style_pad_all(pills, 0, 0);
	lv_obj_set_style_pad_gap(pills, 12, 0);
	lv_obj_remove_flag(pills, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(pills, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(pills, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	*pills_out = pills;
	return card;
}

static lv_obj_t *pill_make(lv_obj_t *parent, const char *label_text, int value, lv_event_cb_t cb) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 56);
	lv_obj_set_style_pad_hor(btn, 26, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0); // Adwaita pill button
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)value);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, label_text);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_center(label);

	return btn;
}

lv_obj_t *settingsrow_pill(lv_obj_t *parent, const char *text, int value, lv_event_cb_t cb) {
	return pill_make(parent, tr(text), value, cb);
}

lv_obj_t *settingsrow_pill_text(lv_obj_t *parent, const char *text, int value, lv_event_cb_t cb) {
	return pill_make(parent, text, value, cb);
}

void settingsrow_pill_active(lv_obj_t *pill, bool active) {
	if (!pill) {
		return;
	}
	lv_obj_set_style_bg_color(pill, active ? theme()->accent : theme()->surface_pressed, 0);
	lv_obj_set_style_text_color(lv_obj_get_child(pill, 0), active ? lv_color_white() : theme()->text_primary, 0);
}

// ---------------------------------------------------------------------------
// The duration wheels
// ---------------------------------------------------------------------------

static void paint_duration_roller(lv_obj_t *roller) {
	if (!roller) {
		return;
	}
	lv_obj_set_style_text_color(roller, theme()->text_secondary, 0);
	lv_obj_set_style_bg_color(roller, theme()->surface_pressed, LV_PART_SELECTED);
	lv_obj_set_style_text_color(roller, theme()->accent, LV_PART_SELECTED);
}

static void fill_wheel_numbers(char *out, size_t size, int last, const char *format) {
	size_t used = 0;
	for (int value = 0; value <= last && used < size; value++) {
		used += (size_t)snprintf(out + used, size - used, format, value);
		if (value != last) {
			used += (size_t)snprintf(out + used, size - used, "\n");
		}
	}
}

static lv_obj_t *make_duration_roller(lv_obj_t *parent, const char *options, int width, lv_event_cb_t change_cb) {
	lv_obj_t *roller = lv_roller_create(parent);
	lv_roller_set_options(roller, options, LV_ROLLER_MODE_NORMAL);
	lv_obj_set_width(roller, width);

	lv_obj_set_style_bg_opa(roller, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(roller, 0, 0);
	lv_obj_set_style_shadow_width(roller, 0, 0);
	lv_obj_set_style_text_font(roller, &font_ui_24, 0);
	lv_obj_set_style_text_line_space(roller, 14, 0);

	lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, LV_PART_SELECTED);
	lv_obj_set_style_radius(roller, 10, LV_PART_SELECTED);
	lv_obj_set_style_text_font(roller, &font_ui_24_bold, LV_PART_SELECTED);

	paint_duration_roller(roller);
	if (change_cb) {
		lv_obj_add_event_cb(roller, change_cb, LV_EVENT_VALUE_CHANGED, NULL);
	}

	// After the font and the line spacing, never before: the call works the
	// height out there and then from the font in force, so asked first it sizes
	// the box for LVGL's default font and the rows either side come out cut in
	// half. Same trap as the clock's pickers.
	lv_roller_set_visible_row_count(roller, 3);
	return roller;
}

// The unit beside each wheel. "h" and "min" are left as they are, which is what
// the player already does everywhere it prints a length.
static void wheel_unit(lv_obj_t *parent, const char *text) {
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, text);
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
}

void settingsrow_toggle_duration(lv_obj_t *parent, const char *title, lv_event_cb_t toggle_cb,
								 lv_event_cb_t change_cb, settingsrow_duration_t *out) {
	memset(out, 0, sizeof(*out));
	out->card = settingsrow_toggle_pills(parent, title, toggle_cb, &out->toggle, &out->wheels);

	lv_obj_set_flex_align(out->wheels, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(out->wheels, 10, 0);

	// Static because lv_roller keeps the pointer it is given rather than
	// copying the options, and these two lists are the same for every wheel on
	// every page.
	static char hours_options[24 * 4];
	static char minutes_options[60 * 4];
	if (!hours_options[0]) {
		fill_wheel_numbers(hours_options, sizeof(hours_options), 23, "%d");
		fill_wheel_numbers(minutes_options, sizeof(minutes_options), 59, "%02d");
	}

	out->hours = make_duration_roller(out->wheels, hours_options, 76, change_cb);
	wheel_unit(out->wheels, "h");
	out->minutes = make_duration_roller(out->wheels, minutes_options, 86, change_cb);
	wheel_unit(out->wheels, "min");
}

void settingsrow_duration_expanded(settingsrow_duration_t *d, bool on) {
	if (!d || !d->toggle) {
		return;
	}
	if (on) {
		lv_obj_add_state(d->toggle, LV_STATE_CHECKED);
		lv_obj_remove_flag(d->wheels, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_remove_state(d->toggle, LV_STATE_CHECKED);
		lv_obj_add_flag(d->wheels, LV_OBJ_FLAG_HIDDEN);
	}
}

int settingsrow_duration_minutes(const settingsrow_duration_t *d) {
	if (!d || !d->hours || !d->minutes) {
		return 0;
	}
	return (int)lv_roller_get_selected(d->hours) * 60 + (int)lv_roller_get_selected(d->minutes);
}

void settingsrow_duration_set_minutes(settingsrow_duration_t *d, int minutes) {
	if (!d || !d->hours || !d->minutes) {
		return;
	}
	if (minutes < 0) {
		minutes = 0;
	}
	lv_roller_set_selected(d->hours, (uint32_t)(minutes / 60), LV_ANIM_OFF);
	lv_roller_set_selected(d->minutes, (uint32_t)(minutes % 60), LV_ANIM_OFF);
}

void settingsrow_duration_repaint(settingsrow_duration_t *d) {
	if (!d) {
		return;
	}
	paint_duration_roller(d->hours);
	paint_duration_roller(d->minutes);
}
