#include "gridpage.h"

#include "lvgl/lvgl.h"

#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

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
	lv_obj_add_flag(tile, LV_OBJ_FLAG_EVENT_BUBBLE);

	lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(tile, 8, 0);
	lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

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
	lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	// START on the main axis, not CENTER: the tile size is computed for a full
	// `columns` x `rows` grid, so a page with fewer entries reads as that grid
	// with empty places. Centring would drift an odd tile into the middle of
	// its row, giving that page a shape no other one has.
	lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	int usable_w = cfg->screen_width - (2 * cfg->padding);
	int usable_h = (cfg->screen_height - top) - (2 * cfg->padding);

	int tile_w = (usable_w - (columns - 1) * GRID_GAP) / columns;
	int tile_h = (usable_h - (rows - 1) * GRID_GAP) / rows;

	for (int i = 0; i < count; i++) {
		add_tile(grid, &entries[i], tile_w, tile_h);
	}

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
