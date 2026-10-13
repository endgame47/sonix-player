#include "gridpage.h"

#include <stdlib.h>

#include "lvgl/lvgl.h"

#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/device/usb.h"

#define GRID_GAP 14
#define TILE_RADIUS 12 // Adwaita card radius

static void unavailable_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active()) {
		return; // this was a swipe across the tile, not a tap on it
	}

	const char *label = lv_event_get_user_data(e);
	char message[160];

	// tr() on the way in as well: what is stored on the tile is the tag, and
	// the message has to name the tile the way the tile does.
	lv_snprintf(message, sizeof(message), tr("grid_not_yet_available"), tr(label));
	gui_notify_popup(message);
}

static void action_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active()) {
		return; // a swipe, not a tap
	}

	void (*action)(void) = (void (*)(void))lv_event_get_user_data(e);
	action();
}

// The two children add_tile() puts on every tile, in the order it makes them.
#define TILE_ICON_CHILD 0
#define TILE_LABEL_CHILD 1

void gridpage_set_tile(lv_obj_t *grid, int index, const lv_image_dsc_t *icon, const char *label) {
	if (!grid || index < 0 || index >= (int)lv_obj_get_child_count(grid)) {
		return;
	}
	lv_obj_t *tile = lv_obj_get_child(grid, index);
	if (!tile) {
		return;
	}
	if (icon) {
		lv_image_set_src(lv_obj_get_child(tile, TILE_ICON_CHILD), icon);
	}
	if (label) {
		lv_label_set_text(lv_obj_get_child(tile, TILE_LABEL_CHILD), tr(label));
	}
}

static void add_tile(lv_obj_t *grid, const grid_entry_t *entry, int width, int height) {
	lv_obj_t *tile = lv_btn_create(grid);
	lv_obj_set_size(tile, width, height);
	lv_obj_add_style(tile, &theme_style_card, 0);
	lv_obj_add_style(tile, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(tile, TILE_RADIUS, 0);
	lv_obj_set_style_shadow_width(tile, 0, 0);
	lv_obj_set_style_pad_all(tile, 8, 0);

	// Presses bubble up to the grid so a swipe can start on a tile: the whole
	// page has to be draggable, not just the gaps between the tiles.
	lv_obj_set_event_bubble(tile, true);

	lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(tile, 8, 0);
	lv_obj_set_scrollable(tile, false);

	// The tile artwork keeps its own colours, so unlike the interface glyphs it
	// is not run through the theme's recolour.
	lv_obj_t *icon = lv_image_create(tile);
	lv_image_set_src(icon, entry->icon);

	// The caption wraps rather than running out of the tile: a translated label
	// such as "Album-Interpreten" does not fit on one line. Centred, because a
	// tile is read as a block under its picture.
	lv_obj_t *label = lv_label_create(tile);
	lv_label_set_text(label, tr(entry->label));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_24_bold, 0);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

	if (entry->target) {
		lv_obj_add_event_cb(tile, switch_screen_cb, LV_EVENT_CLICKED, *entry->target);
	} else if (entry->action) {
		lv_obj_add_event_cb(tile, action_cb, LV_EVENT_CLICKED, (void *)entry->action);
	} else {
		lv_obj_add_event_cb(tile, unavailable_cb, LV_EVENT_CLICKED, (void *)entry->label);
		lv_obj_set_style_opa(icon, LV_OPA_40, 0);
		lv_obj_add_style(label, &theme_style_text_dim, 0);
	}
}

void gridpage_set_entries(lv_obj_t *grid, gui_config_t *cfg, const grid_entry_t *entries, int count, int columns,
						  int rows, bool clear_corner_buttons) {
	if (!grid) {
		return;
	}
	lv_obj_clean(grid);

	int top = clear_corner_buttons ? settingsrow_content_top(cfg) : cfg->top_bar_height;
	int usable_w = cfg->screen_width - (2 * cfg->padding);
	int usable_h = (cfg->screen_height - top) - (2 * cfg->padding);

	int tile_w = (usable_w - (columns - 1) * GRID_GAP) / columns;
	int tile_h = (usable_h - (rows - 1) * GRID_GAP) / rows;

	for (int i = 0; i < count; i++) {
		add_tile(grid, &entries[i], tile_w, tile_h);
	}
}

lv_obj_t *gridpage_build(lv_obj_t *screen, gui_config_t *cfg, const grid_entry_t *entries, int count, int columns,
						 int rows, bool clear_corner_buttons) {
	lv_obj_add_style(screen, &theme_style_screen, 0);

	int top = clear_corner_buttons ? settingsrow_content_top(cfg) : cfg->top_bar_height;

	lv_obj_t *grid = lv_obj_create(screen);
	lv_obj_set_size(grid, lv_pct(100), cfg->screen_height - top);
	lv_obj_align(grid, LV_ALIGN_TOP_LEFT, 0, top);
	lv_obj_set_style_bg_opa(grid, 0, 0);
	lv_obj_set_style_border_width(grid, 0, 0);
	lv_obj_set_style_radius(grid, 0, 0);
	lv_obj_set_style_pad_all(grid, cfg->padding, 0);
	lv_obj_set_style_pad_gap(grid, GRID_GAP, 0);
	lv_obj_set_scrollable(grid, false);

	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	// START on the main axis, not CENTER: the tile size is computed for a full
	// `columns` x `rows` grid, so a page with fewer entries reads as that grid
	// with empty places. Centring would drift an odd tile into the middle of
	// its row, giving that page a shape no other one has.
	lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	gridpage_set_entries(grid, cfg, entries, count, columns, rows, clear_corner_buttons);

	// The player can be pulled in from any tiled page.
	player_sheet_attach_drag(grid, true);
	// Presses die here (the tiles bubble only one level up), so the grid must
	// carry the swipe-back drag itself, like the lists do.
	switcher_attach_back_gesture(grid);

	// Handed back for the same reason: a caller that wants a gesture of its own
	// on the page has to put it on this object, because nothing below it
	// bubbles any further.
	return grid;
}

void gridpage_set_scrolling(lv_obj_t *grid) {
	if (!grid) {
		return;
	}
	lv_obj_set_scrollable(grid, true);
	lv_obj_set_scroll_dir(grid, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_ACTIVE);
	// No give past either end, so the page does not bounce under a pull from
	// the bottom edge, which belongs to the album carousel
	// (coverflow_attach_edge).
	lv_obj_set_scroll_elastic(grid, false);
}

// How long the scrollbar stays up when a page that scrolls is shown: long
// enough to be seen, short enough not to sit over the tiles.
#define SCROLL_HINT_MS 1500

static lv_timer_t *scroll_hint_timer;
static lv_obj_t *scroll_hint_grid;

static void scroll_hint_end_cb(lv_timer_t *timer) {
	(void)timer;
	scroll_hint_timer = NULL; // a repeat count of one deletes the timer after this call
	if (scroll_hint_grid) {
		lv_obj_set_scrollbar_mode(scroll_hint_grid, LV_SCROLLBAR_MODE_ACTIVE);
		scroll_hint_grid = NULL;
	}
}

void gridpage_hint_scroll(lv_obj_t *grid) {
	if (!grid || !lv_obj_is_scrollable(grid)) {
		return;
	}
	// The page's real size: on its first showing nothing has laid it out yet.
	lv_obj_update_layout(grid);
	if (lv_obj_get_scroll_top(grid) + lv_obj_get_scroll_bottom(grid) <= 0) {
		return; // everything fits, nothing to hint at
	}
	if (scroll_hint_timer) {
		lv_timer_delete(scroll_hint_timer);
		scroll_hint_timer = NULL;
		if (scroll_hint_grid && scroll_hint_grid != grid) {
			lv_obj_set_scrollbar_mode(scroll_hint_grid, LV_SCROLLBAR_MODE_ACTIVE);
		}
	}
	scroll_hint_grid = grid;
	lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_ON);
	scroll_hint_timer = lv_timer_create(scroll_hint_end_cb, SCROLL_HINT_MS, NULL);
	lv_timer_set_repeat_count(scroll_hint_timer, 1);
}

// What the empty panel was built with, so it can go back to saying "scan"
// after it has said "no card", and the check that waits for the card.
typedef struct {
	const lv_image_dsc_t *icon;
	const char *text;
	lv_timer_t *card_timer;
	void (*card_back)(void);
} empty_panel_t;

// How often a panel saying "no card" looks for the card again.
#define CARD_CHECK_MS 1000

lv_obj_t *gridpage_empty_panel(lv_obj_t *screen, gui_config_t *cfg, const lv_image_dsc_t *icon, const char *text,
							   lv_event_cb_t scan_cb) {
	int top = settingsrow_content_top(cfg);

	lv_obj_t *panel = lv_obj_create(screen);
	lv_obj_remove_style_all(panel);
	lv_obj_set_size(panel, lv_pct(100), cfg->screen_height - top);
	lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, top);
	lv_obj_set_style_pad_hor(panel, cfg->padding * 2, 0);
	lv_obj_set_style_pad_bottom(panel, cfg->padding * 2, 0);
	lv_obj_set_style_pad_row(panel, 22, 0);
	lv_obj_set_scrollable(panel, false);
	lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_hidden(panel, true);

	lv_obj_t *picture = lv_image_create(panel);
	lv_image_set_src(picture, icon);
	lv_obj_add_style(picture, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(picture, LV_OPA_COVER, 0);

	lv_obj_t *label = lv_label_create(panel);
	lv_label_set_text(label, tr(text));
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_24, 0);

	lv_obj_t *button = lv_btn_create(panel);
	lv_obj_set_size(button, 240, 68);
	lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(button, theme()->accent, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_add_event_cb(button, scan_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *button_label = lv_label_create(button);
	lv_label_set_text(button_label, tr("scan"));
	lv_obj_set_style_text_font(button_label, &font_ui_24, 0);
	lv_obj_set_style_text_color(button_label, lv_color_white(), 0);
	lv_obj_center(button_label);

	empty_panel_t *state = calloc(1, sizeof(*state));
	if (state) {
		state->icon = icon;
		state->text = text;
	}
	lv_obj_set_user_data(panel, state);

	return panel;
}

// The panel's picture, line and button, in the order gridpage_empty_panel()
// creates them.
static void empty_panel_say(lv_obj_t *panel, const lv_image_dsc_t *icon, const char *text, bool button) {
	lv_image_set_src(lv_obj_get_child(panel, 0), icon);
	lv_label_set_text(lv_obj_get_child(panel, 1), tr(text));
	lv_obj_set_hidden(lv_obj_get_child(panel, 2), !button);
}

static void stop_card_check(empty_panel_t *state) {
	if (state && state->card_timer) {
		lv_timer_pause(state->card_timer);
	}
}

static void card_check_cb(lv_timer_t *timer) {
	lv_obj_t *panel = lv_timer_get_user_data(timer);
	empty_panel_t *state = lv_obj_get_user_data(panel);
	// Only while the panel is up on the page on show; a page left behind looks
	// again when it is opened.
	if (!state || lv_obj_is_hidden(panel) || lv_obj_get_screen(panel) != lv_screen_active()) {
		lv_timer_pause(timer);
		return;
	}
	if (gui_card_available()) {
		lv_timer_pause(timer);
		if (state->card_back) {
			state->card_back();
		}
	}
}

void gridpage_show_no_card(lv_obj_t *grid, lv_obj_t *panel, void (*card_back)(void)) {
	if (grid) {
		lv_obj_set_hidden(grid, true);
	}
	if (!panel) {
		return;
	}
	empty_panel_say(panel, &icon_sd_card, usb_storage_active() ? "usb_storage_shared" : "sd_card_missing", false);
	lv_obj_set_hidden(panel, false);

	empty_panel_t *state = lv_obj_get_user_data(panel);
	if (!state) {
		return;
	}
	state->card_back = card_back;
	if (!state->card_timer) {
		state->card_timer = lv_timer_create(card_check_cb, CARD_CHECK_MS, panel);
	}
	lv_timer_resume(state->card_timer);
}

void gridpage_show_empty(lv_obj_t *grid, lv_obj_t *panel, bool empty) {
	if (grid) {
		if (empty) {
			lv_obj_set_hidden(grid, true);
		} else {
			lv_obj_set_hidden(grid, false);
		}
	}
	if (panel) {
		empty_panel_t *state = lv_obj_get_user_data(panel);
		stop_card_check(state);
		if (state) {
			empty_panel_say(panel, state->icon, state->text, true);
		}
		if (empty) {
			// The accent may have changed since the panel was built.
			lv_obj_set_style_bg_color(lv_obj_get_child(panel, 2), theme()->accent, 0);
			lv_obj_set_hidden(panel, false);
		} else {
			lv_obj_set_hidden(panel, true);
		}
	}
}
