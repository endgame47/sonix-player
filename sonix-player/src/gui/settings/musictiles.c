#include "musictiles.h"

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/library/music.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"

lv_obj_t *musictiles_screen;

// ---------------------------------------------------------------------------
// Two lists on one canvas
//
// The control centre's page, for the Music page's tiles: the tiles on the page
// on top, in their order, the ones left out underneath, a heading each. Each
// row carries the tile's picture, at half size, and its name.
//
// A row is taken by its grip and nowhere else, wears the accent border while it
// is carried, and an accent rule shows where it would land. Both lists live on
// one canvas, so a row crossing from one to the other is one coordinate and not
// a change of parent. The page scrolls by itself while a row is held near its
// top or bottom edge.
// ---------------------------------------------------------------------------

#define ROW_HEIGHT 72
#define ROW_GAP 8
#define ROW_PITCH (ROW_HEIGHT + ROW_GAP)
#define ROW_RADIUS 12
#define ROW_PAD 20
#define ICON_BOX 56
// The tiles' own pictures are 112 px: shown at half that.
#define ICON_SCALE (LV_SCALE_NONE / 2)
#define HEADING_H 40

// The grip is only as large as its glyph and a margin round it; the room kept
// for it at the end of the row is wider.
#define GRIP_SIZE 48
#define GRIP_SPACE 64

// Either list is a drop target even when it is empty, so neither is ever
// shorter than one row.
#define LIST_MIN_ROWS 1

// How far the finger has to travel before a release counts as a drop.
#define DROP_MIN_TRAVEL 8

// How close to the top or bottom of the page a held row has to be before the
// page moves under it, and how far it moves each time the event comes round.
#define EDGE_PX 70
#define EDGE_STEP 18

typedef struct {
	lv_obj_t *row;
	lv_obj_t *icon;
	lv_obj_t *name;
	lv_obj_t *grip;
} entry_t;

static lv_obj_t *page;
static lv_obj_t *body;
static lv_obj_t *in_use_count;
static lv_obj_t *others_heading;
static lv_obj_t *drop_line;
static entry_t entries[MUSIC_TILE_COUNT];

// Which tile each row is showing. The first music_tiles_in_use_count() rows
// are the page's, the rest the ones left out. Rebuilt at every repaint, so the
// drag reads them rather than a number baked in at build time.
static music_tile_t row_button[MUSIC_TILE_COUNT];
static int row_count;

// The row being carried, or -1.
static int drag_row = -1;
static bool drag_moved;
static lv_point_t drag_from;

static void refresh(void);
static void refresh_async(void *unused);

// Where each part of the canvas starts. The second heading follows the first
// list, so both move as tiles cross between them.
static int in_use_top(void) { return HEADING_H; }

static int others_heading_y(void) {
	int n = music_tiles_in_use_count();
	return in_use_top() + (n < LIST_MIN_ROWS ? LIST_MIN_ROWS : n) * ROW_PITCH + 8;
}

static int others_top(void) { return others_heading_y() + HEADING_H; }

static void refresh(void) {
	int in_n = music_tiles_in_use_count();
	int out_n = music_tiles_hidden_count();
	row_count = 0;

	for (int i = 0; i < in_n && row_count < MUSIC_TILE_COUNT; i++, row_count++) {
		row_button[row_count] = music_tiles_in_use_at(i);
		lv_obj_set_y(entries[row_count].row, in_use_top() + i * ROW_PITCH);
	}
	for (int i = 0; i < out_n && row_count < MUSIC_TILE_COUNT; i++, row_count++) {
		row_button[row_count] = music_tiles_hidden_at(i);
		lv_obj_set_y(entries[row_count].row, others_top() + i * ROW_PITCH);
	}

	for (int i = 0; i < MUSIC_TILE_COUNT; i++) {
		if (i >= row_count) {
			lv_obj_set_hidden(entries[i].row, true);
			continue;
		}
		lv_obj_set_hidden(entries[i].row, false);
		lv_image_set_src(entries[i].icon, music_tile_icon(row_button[i]));
		lv_label_set_text(entries[i].name, tr(music_tile_tag(row_button[i])));
	}

	lv_label_set_text_fmt(in_use_count, "%d/%d", in_n, MUSIC_TILE_COUNT);
	lv_obj_set_y(others_heading, others_heading_y() + 10);

	int rows_below = out_n < LIST_MIN_ROWS ? LIST_MIN_ROWS : out_n;
	lv_obj_set_height(body, others_top() + rows_below * ROW_PITCH + 8);
}

static void refresh_async(void *unused) {
	(void)unused;
	refresh();
}

// ---------------------------------------------------------------------------
// Carrying a row
// ---------------------------------------------------------------------------

// Where a row would land if the finger lifted at `point`: which list, and the
// gap it would go into, counted with the carried row still in its place.
typedef struct {
	bool in_use;
	int at;
} target_t;

static target_t target_at(lv_point_t point) {
	lv_area_t area;
	lv_obj_get_coords(body, &area);
	int y = point.y - area.y1;

	target_t t;
	t.in_use = y < others_heading_y();
	int top = t.in_use ? in_use_top() : others_top();
	int count = t.in_use ? music_tiles_in_use_count() : music_tiles_hidden_count();
	t.at = (y - top + ROW_PITCH / 2) / ROW_PITCH;
	if (t.at < 0) {
		t.at = 0;
	}
	if (t.at > count) {
		t.at = count;
	}
	return t;
}

// The accent rule in the gap a drop would go into. On the canvas, so it
// scrolls with the rows.
static void drop_line_show(target_t t) {
	int top = t.in_use ? in_use_top() : others_top();
	lv_obj_set_pos(drop_line, 0, top + t.at * ROW_PITCH - ROW_GAP / 2 - 2);
	lv_obj_set_hidden(drop_line, false);
	lv_obj_move_foreground(drop_line);
}

static void carried_look(int index, bool on) {
	lv_obj_t *row = entries[index].row;
	lv_obj_set_style_border_width(row, on ? 2 : 0, 0);
	if (on) {
		lv_obj_set_style_border_color(row, theme()->accent, 0);
		lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
	}
}

// The gap is counted with the carried row in place; once it is lifted out,
// every gap below it in its own list is one earlier.
static void drop(int index, target_t t) {
	int in_n = music_tiles_in_use_count();
	bool from_in_use = index < in_n;
	int from = from_in_use ? index : index - in_n;
	int at = t.at;
	if (t.in_use == from_in_use && from < at) {
		at--;
	}
	if (t.in_use == from_in_use && at == from) {
		return;
	}
	if (t.in_use) {
		music_tiles_move_in_use(row_button[index], at);
	} else if (!music_tiles_move_hidden(row_button[index], at)) {
		toast_error("musictiles_keep_one");
	}
}

// The page follows the finger when a held row reaches either end of it.
static void edge_scroll(lv_point_t point) {
	lv_area_t view;
	lv_obj_get_coords(page, &view);
	int step = 0;
	if (point.y < view.y1 + EDGE_PX) {
		step = -EDGE_STEP;
	} else if (point.y > view.y2 - EDGE_PX) {
		step = EDGE_STEP;
	}
	if (step == 0) {
		return;
	}
	int scroll = lv_obj_get_scroll_y(page);
	int limit = scroll + lv_obj_get_scroll_bottom(page);
	int wanted = scroll + step;
	wanted = wanted < 0 ? 0 : (wanted > limit ? limit : wanted);
	if (wanted != scroll) {
		lv_obj_scroll_to_y(page, wanted, LV_ANIM_OFF);
	}
}

static void drag_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *grip = lv_event_get_current_target(e);

	lv_indev_t *indev = lv_indev_active();
	lv_point_t point = {0, 0};
	if (indev) {
		lv_indev_get_point(indev, &point);
	}

	if (code == LV_EVENT_PRESSED) {
		drag_row = -1;
		for (int i = 0; i < row_count; i++) {
			if (entries[i].grip == grip) {
				drag_row = i;
			}
		}
		if (drag_row < 0) {
			return;
		}
		drag_moved = false;
		drag_from = point;
		carried_look(drag_row, true);
		return;
	}

	if (drag_row < 0) {
		return;
	}

	if (code == LV_EVENT_PRESSING) {
		edge_scroll(point);
		if (LV_ABS(point.y - drag_from.y) > DROP_MIN_TRAVEL || LV_ABS(point.x - drag_from.x) > DROP_MIN_TRAVEL) {
			drag_moved = true;
		}
		if (drag_moved) {
			drop_line_show(target_at(point));
		}
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		int index = drag_row;
		drag_row = -1;
		lv_obj_set_hidden(drop_line, true);
		carried_look(index, false);

		if (code == LV_EVENT_RELEASED && drag_moved) {
			drop(index, target_at(point));
		}
		// Not here: the repaint rebinds the very row whose release is being
		// handled. It waits for the event to unwind.
		lv_async_call(refresh_async, NULL);
	}
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

static void loaded_cb(lv_event_t *e) {
	(void)e;
	refresh();
}

static lv_obj_t *make_heading(lv_obj_t *parent, const char *tag, int y) {
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, tr(tag));
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_set_pos(label, 4, y + 10);
	return label;
}

static void make_row(entry_t *entry, int width) {
	lv_obj_t *row = lv_obj_create(body);
	lv_obj_set_size(row, width, ROW_HEIGHT);
	lv_obj_set_x(row, 0);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_set_style_radius(row, ROW_RADIUS, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, ROW_PAD, 0);
	lv_obj_set_style_pad_ver(row, 0, 0);
	lv_obj_set_scrollable(row, false);
	entry->row = row;

	lv_obj_t *box = lv_obj_create(row);
	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, ICON_BOX, ROW_HEIGHT);
	lv_obj_align(box, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_set_clickable(box, false);

	// The tile's picture keeps its colours, as on the tile.
	entry->icon = lv_image_create(box);
	lv_image_set_scale(entry->icon, ICON_SCALE);
	lv_obj_center(entry->icon);

	entry->name = lv_label_create(row);
	lv_obj_add_style(entry->name, &theme_style_text, 0);
	lv_obj_set_style_text_font(entry->name, &font_ui_24, 0);
	lv_label_set_long_mode(entry->name, LV_LABEL_LONG_DOT);
	lv_obj_set_width(entry->name, width - ROW_PAD - ICON_BOX - 16 - GRIP_SPACE);
	lv_obj_align(entry->name, LV_ALIGN_LEFT_MID, ICON_BOX + 16, 0);

	// The handle, and the only part of the row that can be taken hold of. Not
	// a button: a tap on it does nothing. A drag on it must not find the page
	// and scroll it, hence no scroll chain.
	entry->grip = lv_obj_create(row);
	lv_obj_set_size(entry->grip, GRIP_SIZE, GRIP_SIZE);
	// Centred in its room, which runs into the row's right padding.
	lv_obj_align(entry->grip, LV_ALIGN_RIGHT_MID, ROW_PAD - (GRIP_SPACE - GRIP_SIZE) / 2, 0);
	lv_obj_set_style_bg_opa(entry->grip, 0, 0);
	lv_obj_set_style_border_width(entry->grip, 0, 0);
	lv_obj_set_style_pad_all(entry->grip, 0, 0);
	lv_obj_set_scrollable(entry->grip, false);
	lv_obj_set_scroll_chain_ver(entry->grip, false);
	lv_obj_set_scroll_chain_hor(entry->grip, false);
	lv_obj_set_clickable(entry->grip, true);

	lv_obj_t *grip_icon = lv_image_create(entry->grip);
	lv_image_set_src(grip_icon, &icon_grip);
	lv_obj_add_style(grip_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(grip_icon, LV_OPA_COVER, 0);
	lv_obj_center(grip_icon);

	lv_obj_add_event_cb(entry->grip, drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(entry->grip, drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(entry->grip, drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(entry->grip, drag_cb, LV_EVENT_PRESS_LOST, NULL);
}

void musictiles_init(gui_config_t *cfg) {
	musictiles_screen = lv_obj_create(NULL);
	lv_obj_add_style(musictiles_screen, &theme_style_screen, 0);

	page = settingsrow_page(musictiles_screen, cfg, "musicsettings_rearrange_main_page");
	// No buttons in the corner, so the heading may run to the edge: the name
	// of the page is long in every language.
	settingsrow_title_corner_slots(settingsrow_page_title(musictiles_screen), cfg, 0);

	int width = cfg->screen_width - 2 * cfg->padding;

	body = lv_obj_create(page);
	lv_obj_set_width(body, width);
	lv_obj_set_height(body, ROW_PITCH);
	lv_obj_set_style_bg_opa(body, 0, 0);
	lv_obj_set_style_border_width(body, 0, 0);
	lv_obj_set_style_pad_all(body, 0, 0);
	lv_obj_set_scrollable(body, false);

	make_heading(body, "in_use", 0);
	others_heading = make_heading(body, "controlcentre_not_in_use", 0);

	// How many of the tiles are on the page, level with the first heading.
	in_use_count = lv_label_create(body);
	lv_obj_add_style(in_use_count, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(in_use_count, &font_ui_22, 0);
	lv_obj_align(in_use_count, LV_ALIGN_TOP_RIGHT, -4, 10);

	for (int i = 0; i < MUSIC_TILE_COUNT; i++) {
		make_row(&entries[i], width);
	}

	drop_line = lv_obj_create(body);
	lv_obj_set_size(drop_line, width, 4);
	lv_obj_set_scrollable(drop_line, false);
	lv_obj_set_clickable(drop_line, false);
	lv_obj_set_style_border_width(drop_line, 0, 0);
	lv_obj_set_style_shadow_width(drop_line, 0, 0);
	lv_obj_set_style_pad_all(drop_line, 0, 0);
	lv_obj_add_style(drop_line, &theme_style_accent_bg, 0);
	lv_obj_set_style_radius(drop_line, 2, 0);
	lv_obj_set_hidden(drop_line, true);

	lv_obj_add_event_cb(musictiles_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(musictiles_screen);

	refresh();
}
