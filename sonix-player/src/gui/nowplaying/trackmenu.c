#include "trackmenu.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/library/medialist.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/library/playlistpage.h"
#include "src/gui/streaming/qobuzpage.h"
#include "src/gui/streaming/tidalpage.h"
#include "src/gui/shell/popover.h"
#include "src/gui/settings/musicsettings.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/nowplaying/cover.h"
#include "src/system/audio/audio.h"
#include "src/system/audio/replaygain.h"
#include "src/system/decode/decode.h"
#include "src/system/library/metadata.h"
#include "src/system/playback/device_state.h"
#include "src/system/core/lang.h"
#include "src/gui/streaming/podcastcard.h"
#include "src/gui/streaming/podcastpage.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/library/library.h"
#include "src/system/playback/playlist.h"
#include "src/system/core/utils.h"
#include "lvgl/src/core/lv_obj_scroll_private.h"

lv_obj_t *queue_screen;
lv_obj_t *details_screen;

// The queue is windowed like every other long list here: a fixed pool of row
// widgets rides the scroll position, and each row looks up its title (one
// indexed database lookup) at the moment it binds. Opening the page costs a
// dozen binds, not hundreds, which is what makes it instant.
#define QUEUE_THUMB_SIZE 56
#define QUEUE_ROW_HEIGHT 72
#define QUEUE_ROW_GAP 8
#define QUEUE_ROW_PITCH (QUEUE_ROW_HEIGHT + QUEUE_ROW_GAP)
#define QUEUE_ROW_POOL 12
#define QUEUE_GRIP_SIZE 56

// The room a heading takes above its rows: History over the tracks already
// played, Now playing over the track playing and the ones after it.
#define QUEUE_HEADER_H 48

// The page rests with Now playing at the top, and the history sits above it.
// Pulling down into the history meets resistance: the list moves
// QUEUE_PULL_NUM / QUEUE_PULL_DEN of what the finger does, and gives way once
// it has moved QUEUE_PULL_OPEN_PX. Let go before that and it springs back.
#define QUEUE_PULL_NUM 2
#define QUEUE_PULL_DEN 5
#define QUEUE_PULL_OPEN_PX 56

// Carrying a row: how far the finger has to travel before a release counts as
// a drop, and how close to an edge of the list it has to be before the list
// moves under it, by QUEUE_EDGE_STEP each time the finger is read.
#define QUEUE_DROP_MIN_TRAVEL 8
#define QUEUE_EDGE_PX 70
#define QUEUE_EDGE_STEP 18

typedef struct {
	lv_obj_t *button;
	lv_obj_t *icon;
	lv_obj_t *label;
	lv_obj_t *check; // the tick on a chosen row while choosing
	lv_obj_t *grip;	 // the handle on a row that can be moved, while reordering
	int index;		 // position in the queue, or -1
	bool has_thumb;
	cover_image_t thumb;
} queue_row_t;

static lv_obj_t *queue_list; // the scrollable viewport
static lv_obj_t *queue_body; // fixed-height canvas the pool rows sit on
static lv_obj_t *queue_empty;
static lv_obj_t *queue_history_header;
static lv_obj_t *queue_now_header;
static queue_row_t queue_rows[QUEUE_ROW_POOL];
static int queue_total;	  // how many tracks the queue holds, from its first
static int queue_current; // the track playing, as the rows were laid out for it
static int queue_view_h;  // the height of the viewport
static lv_obj_t *queue_title;

// The two buttons always in the corner put the page into choosing rows to take
// out (SELECT) or moving them by hand (REORDER). A long press on a row of the
// history picks the row up, to be put down in what is still to play.
typedef enum {
	QUEUE_IDLE,
	QUEUE_SELECT,
	QUEUE_REORDER,
} queue_mode_t;

static queue_mode_t queue_mode;
// While choosing: one per entry of the queue, and how many are set. Only what
// is still to play can be chosen.
static uint8_t *queue_chosen;
static int queue_chosen_count;
static lv_obj_t *queue_select_btn;
static lv_obj_t *queue_reorder_btn;
static lv_obj_t *queue_remove_btn;
static lv_obj_t *queue_close_btn;

// The row being carried, by its grip or, out of the history, by the row itself.
// The row stays where it is, dimmed, and a copy of it follows the finger; the
// accent rule says where it would land.
static struct {
	bool active;
	bool moved;
	bool by_row; // held by the row: its scroll chain is off until the drop
	int from;	 // its position in the queue
	int target;	 // the position it would be put in front of, or -1
	lv_obj_t *held;
	lv_point_t start;
	int32_t grab_dx;
	int32_t grab_dy;
} queue_drag;
static lv_obj_t *queue_drag_layer;
static lv_obj_t *queue_ghost;
static lv_obj_t *queue_drop_line;
// A row carried out of the history is let go of as a tap would be: the click
// that follows the drop must not play it.
static bool queue_drop_guard;

// The pull into the history, for the drag under way. Only a drag that began at
// or below Now playing meets it.
static struct {
	bool on;
	bool adjusting;	 // the list is being put back where the pull wants it
	int32_t natural; // where the list would be without the resistance
	int32_t last;	 // where it was left last
} queue_pull;

static lv_obj_t *details_card;
static lv_obj_t *details_scroll;
static gui_config_t *g_cfg;

// ---------------------------------------------------------------------------
// Queue: the tracks already played, the one playing and the ones after it,
// tappable to jump.
// ---------------------------------------------------------------------------

// Where Now playing begins on the canvas: under the history, when there is one.
static int queue_now_top(void) { return queue_current > 0 ? QUEUE_HEADER_H + queue_current * QUEUE_ROW_PITCH : 0; }

// Where queued track `index` sits on the canvas. `queue_total` gives the end of
// the last row.
static int queue_row_y(int index) {
	if (index < queue_current) {
		return QUEUE_HEADER_H + index * QUEUE_ROW_PITCH;
	}
	return queue_now_top() + QUEUE_HEADER_H + (index - queue_current) * QUEUE_ROW_PITCH;
}

// The row at height `y` on the canvas, or the nearest one.
static int queue_index_at(int y) {
	int index;
	if (y < queue_now_top()) {
		index = (y - QUEUE_HEADER_H) / QUEUE_ROW_PITCH;
	} else {
		index = queue_current + (y - queue_now_top() - QUEUE_HEADER_H) / QUEUE_ROW_PITCH;
	}
	if (index >= queue_total) {
		index = queue_total - 1;
	}
	return index < 0 ? 0 : index;
}

// The scroll position furthest down the list can go.
static int queue_scroll_max(void) {
	int max = lv_obj_get_height(queue_body) - queue_view_h;
	return max < 0 ? 0 : max;
}

static void queue_row_paint_check(queue_row_t *row) {
	bool on = queue_mode == QUEUE_SELECT && queue_chosen && row->index >= 0 && row->index < queue_total && queue_chosen[row->index];
	if (on) {
		lv_obj_set_style_image_recolor(row->check, theme()->accent, 0);
	}
	lv_obj_set_hidden(row->check, !on);
}

// The tick, the grip and the dimming of the row being carried: what a row
// shows of the mode the page is in.
static void queue_row_paint_tools(queue_row_t *row) {
	queue_row_paint_check(row);
	bool grip = queue_mode == QUEUE_REORDER && row->index > queue_current && row->index < queue_total;
	lv_obj_set_hidden(row->grip, !grip);
	bool carried = queue_drag.active && row->index >= 0 && row->index == queue_drag.from;
	lv_opa_t opa = carried ? LV_OPA_40 : LV_OPA_COVER;
	if (lv_obj_get_style_opa(row->button, 0) != opa) {
		lv_obj_set_style_opa(row->button, opa, 0);
	}
}

static void queue_paint_rows(void) {
	for (int i = 0; i < QUEUE_ROW_POOL; i++) {
		queue_row_paint_tools(&queue_rows[i]);
	}
}

// The corner, for the mode the page is in: the two ways in while idle, the bin
// and the close button while choosing, and only the reorder button while
// reordering, lit in the accent; otherwise the local colour is taken off again,
// so the icon style, and with it the palette, reaches the glyph. An empty queue
// has nothing to choose or move.
static void queue_mode_show(void) {
	bool idle = queue_mode == QUEUE_IDLE && queue_total > 0;
	bool select = queue_mode == QUEUE_SELECT;
	bool reorder = queue_mode == QUEUE_REORDER;
	lv_obj_set_hidden(queue_select_btn, !idle);
	lv_obj_set_hidden(queue_reorder_btn, !(idle || reorder));
	lv_obj_set_hidden(queue_remove_btn, !select);
	lv_obj_set_hidden(queue_close_btn, !select);

	lv_obj_t *glyph = lv_obj_get_child(queue_reorder_btn, 0);
	if (glyph && reorder) {
		lv_obj_set_style_image_recolor(glyph, theme()->accent, 0);
	} else if (glyph) {
		lv_obj_remove_local_style_prop(glyph, LV_STYLE_IMAGE_RECOLOR, 0);
	}
	// The bin is there from the start, faded until something is chosen.
	lv_obj_set_style_opa(queue_remove_btn, queue_chosen_count > 0 ? LV_OPA_COVER : LV_OPA_40, 0);

	settingsrow_title_corner_slots(queue_title, g_cfg, idle || select ? 2 : (reorder ? 1 : 0));
	queue_paint_rows();
}

static void queue_mode_set(queue_mode_t mode) {
	if (mode != QUEUE_SELECT) {
		free(queue_chosen);
		queue_chosen = NULL;
		queue_chosen_count = 0;
	}
	queue_mode = mode;
	queue_mode_show();
}

// Starts choosing, with nothing chosen yet.
static bool queue_selection_start(void) {
	if (queue_total <= 0) {
		return false;
	}
	free(queue_chosen);
	queue_chosen = calloc((size_t)queue_total, 1);
	if (!queue_chosen) {
		queue_mode_set(QUEUE_IDLE);
		return false;
	}
	queue_chosen_count = 0;
	queue_mode = QUEUE_SELECT;
	queue_mode_show();
	return true;
}

// Chooses the row, or lets it go. The mode stays on until it is closed.
static void queue_selection_toggle(queue_row_t *row) {
	int index = row->index;
	if (queue_mode != QUEUE_SELECT || !queue_chosen || index <= queue_current || index >= queue_total) {
		return;
	}
	queue_chosen[index] = !queue_chosen[index];
	queue_chosen_count += queue_chosen[index] ? 1 : -1;
	queue_mode_show();
}

static queue_row_t *queue_row_of(lv_obj_t *button) {
	for (int i = 0; i < QUEUE_ROW_POOL; i++) {
		if (queue_rows[i].button == button && queue_rows[i].index >= 0) {
			return &queue_rows[i];
		}
	}
	return NULL;
}

static void queue_row_drop_thumb(queue_row_t *row) {
	if (row->has_thumb) {
		// The widget must stop pointing at the pixels before they go.
		lv_image_set_src(row->icon, &icon_music2);
		lv_obj_add_style(row->icon, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_COVER, 0);
		cover_free(&row->thumb);
		row->has_thumb = false;
	}
}

// No artwork on these rows: a queue is a list of what is coming, and a column
// of covers is slower to read and slower to build. The playing track gets the
// now-playing mark in the accent colour instead; everything else gets the plain
// note.
//
// Its own function because the mark changes without the row changing: painting
// it only where a row is bound would leave the mark on a finished track until
// the list scrolled far enough to rebind that row.
static void queue_row_paint_mark(queue_row_t *row) {
	if (!row->icon || row->index < 0) {
		return;
	}
	bool is_playing_row = row->index == playlist_current_index();
	lv_image_set_src(row->icon, is_playing_row ? &icon_now_playing : &icon_music2);
	lv_obj_add_style(row->icon, &theme_style_icon, 0);
	if (is_playing_row) {
		lv_obj_set_style_image_recolor(row->icon, theme()->accent, 0);
	} else {
		lv_obj_remove_local_style_prop(row->icon, LV_STYLE_IMAGE_RECOLOR, 0);
	}
	lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_COVER, 0);
}

// Points a pool row at queued track `index` and resolves its title on the spot;
// an indexed lookup is cheap enough to do during a scroll.
static void queue_row_bind(queue_row_t *row, int index) {
	if (row->index == index) {
		return;
	}

	queue_row_drop_thumb(row);
	row->index = index;

	if (index < 0 || index >= queue_total) {
		lv_obj_set_hidden(row->button, true);
		return;
	}

	char path[512];
	if (!playlist_path_at(index, path, sizeof(path))) {
		lv_obj_set_hidden(row->button, true);
		return;
	}

	char title[256];
	if (!library_track_title(path, title, sizeof(title))) {
		// Not in the library: it may be a Qobuz track, which is kept out of the
		// library on purpose. The title is still there, in the sidecar file next
		// to the track. Without this the queue showed a column of numbers, which
		// were the file names.
		song_metadata_t meta;
		metadata_read(path, &meta);
		if (meta.title[0]) {
			snprintf(title, sizeof(title), "%.250s", meta.title);
		} else {
			const char *slash = strrchr(path, '/');
			snprintf(title, sizeof(title), "%.250s", slash ? slash + 1 : path);
			char *dot = strrchr(title, '.');
			if (dot) {
				*dot = '\0';
			}
		}
	}

	lv_obj_set_hidden(row->button, false);
	lv_obj_set_y(row->button, queue_row_y(index));
	lv_label_set_text(row->label, title);

	queue_row_paint_mark(row);
	queue_row_paint_tools(row);
}

static void queue_window_update(void) {
	if (queue_total <= 0) {
		for (int i = 0; i < QUEUE_ROW_POOL; i++) {
			queue_row_bind(&queue_rows[i], -1);
		}
		return;
	}

	int scroll = lv_obj_get_scroll_y(queue_list);
	if (scroll < 0) {
		scroll = 0;
	}

	int first = queue_index_at(scroll) - 1;
	if (first + QUEUE_ROW_POOL > queue_total) {
		first = queue_total - QUEUE_ROW_POOL;
	}
	if (first < 0) {
		first = 0;
	}

	for (int i = 0; i < QUEUE_ROW_POOL; i++) {
		int index = first + i;
		queue_row_bind(&queue_rows[index % QUEUE_ROW_POOL], index < queue_total ? index : -1);
	}
}

// Lays the page out for the queue as it is now: the headings, the canvas and,
// at the next window update, every row.
static void queue_layout(void) {
	// The whole queue, from its first track: the ones already played are part
	// of it and are how the album being listened to can be seen at all.
	queue_total = playlist_count();
	if (queue_total < 0) {
		queue_total = 0;
	}
	int current = playlist_current_index();
	if (current >= queue_total) {
		current = queue_total - 1;
	}
	queue_current = current < 0 ? 0 : current;

	lv_obj_set_hidden(queue_history_header, queue_total == 0 || queue_current == 0);
	lv_obj_set_hidden(queue_now_header, queue_total == 0);
	lv_obj_set_y(queue_now_header, queue_now_top() + 10);

	// Tall enough for Now playing to reach the top of the page however little
	// comes after it.
	int height = queue_total ? queue_row_y(queue_total) : QUEUE_ROW_PITCH;
	if (queue_total && height < queue_now_top() + queue_view_h) {
		height = queue_now_top() + queue_view_h;
	}
	lv_obj_set_height(queue_body, height);
	// A scroll set next is clamped to the scrollable range, and that range comes
	// from the height just set. Without the layout being brought up to date
	// first the clamp uses the old height.
	lv_obj_update_layout(queue_list);

	for (int i = 0; i < QUEUE_ROW_POOL; i++) {
		queue_rows[i].index = -2; // force every row to rebind
	}
	lv_obj_set_hidden(queue_empty, queue_total != 0);
	queue_pull.on = false;
}

// Puts the list at `scroll`, within what it can reach.
static void queue_scroll_to(int scroll, lv_anim_enable_t anim) {
	int max = queue_scroll_max();
	scroll = scroll < 0 ? 0 : (scroll > max ? max : scroll);
	lv_obj_scroll_to_y(queue_list, scroll, anim);
}

static void queue_rebuild(void) {
	queue_layout();
	// Opened on Now playing rather than at the top: with the whole queue
	// listed, the top is wherever the queue began, which may be an hour ago.
	queue_scroll_to(queue_now_top(), LV_ANIM_OFF);
	queue_window_update();
	queue_mode_show();
}

// The pointer that is scrolling the list, if one is: the throw that follows a
// lift runs from the animation timer, where there is no active input device.
static lv_indev_t *queue_scrolling_indev(void) {
	for (lv_indev_t *indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
		if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER && lv_indev_get_scroll_obj(indev) == queue_list) {
			return indev;
		}
	}
	return NULL;
}

// Back to Now playing, for a pull into the history that did not go far enough.
static void queue_pull_spring_back(lv_indev_t *indev) {
	queue_pull.on = false;
	queue_pull.adjusting = true;
	if (indev) {
		// The throw would go on carrying the list the other way.
		lv_indev_reset(indev, queue_list);
	}
	queue_scroll_to(queue_now_top(), LV_ANIM_ON);
	queue_pull.adjusting = false;
}

// While the finger drags the list above Now playing, it is kept to a part of
// what the finger does; once it has gone far enough it moves freely. After the
// lift, a list still above Now playing goes back to it.
static void queue_pull_follow(void) {
	lv_indev_t *indev = queue_scrolling_indev();
	if (!indev) {
		return;
	}
	int32_t anchor = queue_now_top();
	int32_t scroll = lv_obj_get_scroll_y(queue_list);
	if (lv_indev_get_state(indev) != LV_INDEV_STATE_PRESSED) {
		if (scroll < anchor) {
			queue_pull_spring_back(indev);
		}
		return;
	}

	queue_pull.natural += scroll - queue_pull.last;
	int32_t wanted = queue_pull.natural;
	if (wanted < anchor) {
		wanted = anchor - (anchor - wanted) * QUEUE_PULL_NUM / QUEUE_PULL_DEN;
	}
	if (anchor - wanted >= QUEUE_PULL_OPEN_PX) {
		queue_pull.on = false; // through: from here the list follows the finger
	}
	if (wanted != scroll) {
		queue_pull.adjusting = true;
		lv_obj_scroll_by_raw(queue_list, 0, scroll - wanted);
		queue_pull.adjusting = false;
	}
	queue_pull.last = wanted;
}

static void queue_scroll_cb(lv_event_t *e) {
	if (queue_pull.adjusting) {
		return;
	}
	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_RELEASED) {
		// Let go short of the history: back now, before a throw can carry
		// the list any further into it.
		if (queue_pull.on && lv_obj_get_scroll_y(queue_list) < queue_now_top()) {
			queue_pull_spring_back(queue_scrolling_indev());
		}
		return;
	}
	if (code == LV_EVENT_SCROLL_BEGIN) {
		// A drag starting at or below Now playing meets the pull; one that
		// starts in the history is already through it.
		lv_indev_t *indev = queue_scrolling_indev();
		if (indev) {
			// A finger on the list stops a spring back still under way, rather
			// than being fought by it.
			queue_pull.adjusting = true;
			lv_anim_delete(queue_list, NULL);
			queue_pull.adjusting = false;
		}
		int32_t scroll = lv_obj_get_scroll_y(queue_list);
		queue_pull.on = indev && queue_current > 0 && scroll >= queue_now_top();
		queue_pull.natural = scroll;
		queue_pull.last = scroll;
		return;
	}
	if (code == LV_EVENT_SCROLL_END) {
		if (queue_pull.on && lv_obj_get_scroll_y(queue_list) < queue_now_top()) {
			queue_pull_spring_back(NULL);
		}
		queue_pull.on = false;
		return;
	}
	if (queue_pull.on) {
		queue_pull_follow();
	}
	queue_window_update();
}

// The track changed under the page. With the page showing, the headings move
// round the new track, and a list resting on Now playing follows it there;
// otherwise only the marks are repainted, and opening the page lays it out.
// Called from the one place in the player that sees every change of track, the
// same way the browser and the library lists are told.
static void queue_drag_end(bool drop);

void trackmenu_notify_now_playing(void) {
	if (!queue_list || lv_screen_active() != queue_screen) {
		for (int i = 0; i < QUEUE_ROW_POOL; i++) {
			queue_row_paint_mark(&queue_rows[i]);
		}
		return;
	}

	int total = queue_total;
	int anchor = queue_now_top();
	int scroll = lv_obj_get_scroll_y(queue_list);
	queue_layout();
	if (queue_total != total) {
		// Entries came or went: the choices and the carried row no longer
		// point where they did.
		queue_drag_end(false);
		queue_mode_set(QUEUE_IDLE);
	} else if (queue_chosen && queue_chosen[queue_current]) {
		// The track playing cannot be chosen.
		queue_chosen[queue_current] = 0;
		queue_chosen_count--;
	}
	if (LV_ABS(scroll - anchor) <= 1) {
		queue_scroll_to(queue_now_top(), LV_ANIM_ON);
	} else if (scroll > queue_scroll_max()) {
		queue_scroll_to(scroll, LV_ANIM_OFF);
	}
	queue_window_update();
	queue_mode_show();
}

// ---------------------------------------------------------------------------
// Carrying a row: by its grip while reordering, or out of the history after a
// long press. Either way it can only be put down after the track playing.
// ---------------------------------------------------------------------------

// The accent rule in front of position `before`. Drawn on the canvas, so it
// scrolls with the rows.
static void queue_drop_line_show(int before) {
	if (!queue_drop_line) {
		queue_drop_line = lv_obj_create(queue_body);
		lv_obj_set_ignore_layout(queue_drop_line, true);
		lv_obj_set_scrollable(queue_drop_line, false);
		lv_obj_set_clickable(queue_drop_line, false);
		lv_obj_set_style_border_width(queue_drop_line, 0, 0);
		lv_obj_set_style_shadow_width(queue_drop_line, 0, 0);
		lv_obj_set_style_pad_all(queue_drop_line, 0, 0);
		lv_obj_add_style(queue_drop_line, &theme_style_accent_bg, 0);
		lv_obj_set_style_radius(queue_drop_line, 2, 0);
		lv_obj_set_size(queue_drop_line, lv_obj_get_width(queue_body), 4);
	}
	lv_obj_set_pos(queue_drop_line, 0, queue_row_y(before) - QUEUE_ROW_GAP / 2 - 2);
	lv_obj_set_hidden(queue_drop_line, false);
	lv_obj_move_foreground(queue_drop_line);
}

// Puts the copy where the finger is holding it. See kblayoutpage.c: the
// difference is read back rather than computed, so nothing here has to know
// about the parent's insets.
static void queue_ghost_follow(lv_point_t point) {
	lv_area_t here;
	lv_obj_get_coords(queue_ghost, &here);
	int32_t dx = (point.x - queue_drag.grab_dx) - here.x1;
	int32_t dy = (point.y - queue_drag.grab_dy) - here.y1;
	lv_obj_set_pos(queue_ghost, lv_obj_get_x(queue_ghost) + dx, lv_obj_get_y(queue_ghost) + dy);
}

static void queue_drag_start(queue_row_t *row, lv_obj_t *held, lv_point_t point) {
	lv_area_t area;
	lv_obj_get_coords(row->button, &area);
	queue_drag.active = true;
	queue_drag.moved = false;
	queue_drag.by_row = held == row->button;
	queue_drag.from = row->index;
	queue_drag.target = -1;
	queue_drag.held = held;
	queue_drag.start = point;
	queue_drag.grab_dx = point.x - area.x1;
	queue_drag.grab_dy = point.y - area.y1;

	if (queue_drag.by_row) {
		// A drag that goes on from the long press carries the row, not the
		// list. The row is a handle until the drop, and is not shown pressed.
		lv_obj_set_scroll_chain_ver(held, false);
		lv_obj_remove_state(held, LV_STATE_PRESSED);
	}

	// A copy on the layer above the list, because the row lives on a canvas
	// that clips it.
	queue_ghost = lv_obj_create(queue_drag_layer);
	lv_obj_set_size(queue_ghost, lv_area_get_width(&area), QUEUE_ROW_HEIGHT);
	lv_obj_add_style(queue_ghost, &theme_style_card, 0);
	lv_obj_set_style_radius(queue_ghost, 12, 0);
	lv_obj_set_style_border_width(queue_ghost, 2, 0);
	lv_obj_set_style_border_color(queue_ghost, theme()->accent, 0);
	lv_obj_set_style_shadow_width(queue_ghost, 0, 0);
	lv_obj_set_style_pad_all(queue_ghost, 8, 0);
	lv_obj_set_style_pad_column(queue_ghost, 14, 0);
	lv_obj_set_scrollable(queue_ghost, false);
	lv_obj_set_clickable(queue_ghost, false);
	lv_obj_set_flex_flow(queue_ghost, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(queue_ghost, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *icon = lv_image_create(queue_ghost);
	lv_obj_set_size(icon, QUEUE_THUMB_SIZE, QUEUE_THUMB_SIZE);
	lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
	lv_image_set_src(icon, &icon_music2);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);

	lv_obj_t *label = lv_label_create(queue_ghost);
	lv_label_set_text(label, lv_label_get_text(row->label));
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(label, 1);
	lv_obj_set_height(label, 32);
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_24, 0);

	if (!queue_drag.by_row) {
		lv_obj_t *grip = lv_image_create(queue_ghost);
		lv_obj_set_size(grip, QUEUE_GRIP_SIZE, QUEUE_GRIP_SIZE);
		lv_image_set_inner_align(grip, LV_IMAGE_ALIGN_CENTER);
		lv_image_set_src(grip, &icon_grip);
		lv_obj_add_style(grip, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(grip, LV_OPA_COVER, 0);
	}

	queue_ghost_follow(point);
	queue_paint_rows();
}

// Lets go of the row. With `drop`, it is put where the rule was, if the finger
// went anywhere and the queue is still the one the drag began on.
static void queue_drag_end(bool drop) {
	if (!queue_drag.active) {
		return;
	}
	queue_drag.active = false;
	if (queue_drag.by_row) {
		lv_obj_set_scroll_chain_ver(queue_drag.held, true);
		if (!drop) {
			// Let go of with the finger still down: the rest of the press
			// must neither play the row nor turn into the swipe back.
			for (lv_indev_t *indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
				if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
					lv_indev_wait_release(indev);
				}
			}
		}
	}
	if (queue_ghost) {
		lv_obj_delete(queue_ghost);
		queue_ghost = NULL;
	}
	if (queue_drop_line) {
		lv_obj_set_hidden(queue_drop_line, true);
	}
	queue_paint_rows();

	int from = queue_drag.from;
	int target = queue_drag.target;
	if (!drop || !queue_drag.moved || target < 0 || playlist_count() != queue_total || playlist_current_index() != queue_current) {
		return;
	}
	int to = target > from ? target - 1 : target;
	if (to == from) {
		return;
	}

	// A row taken out of the history above the top of the page takes its
	// height with it, and the heading too when it was the last one: the page
	// moves up by as much, and stays on what it shows.
	int scroll = lv_obj_get_scroll_y(queue_list);
	bool above = from < queue_current && queue_row_y(from) < scroll;
	int now_top = queue_now_top();
	unsigned revision = playlist_revision();
	playlist_move(from, to);
	if (playlist_revision() != revision) {
		// Written down now, as a removal is: the mirror on disk is otherwise
		// refreshed only when a track is loaded. A move turned down after the
		// queue became a list of paths has changed it too.
		device_state_queue_changed();
	}
	queue_layout();
	if (above) {
		scroll -= now_top - queue_now_top();
	}
	queue_scroll_to(scroll, LV_ANIM_OFF);
	queue_window_update();
	queue_mode_show();
}

// Follows the finger: the copy, the list at either edge, and the rule.
static void queue_drag_follow(lv_point_t point) {
	if (LV_ABS(point.y - queue_drag.start.y) > QUEUE_DROP_MIN_TRAVEL || LV_ABS(point.x - queue_drag.start.x) > QUEUE_DROP_MIN_TRAVEL) {
		queue_drag.moved = true;
	}

	// Not before the finger has moved: a row picked up near an edge would
	// otherwise set the list going under it straight away.
	lv_area_t view;
	lv_obj_get_coords(queue_list, &view);
	int step = 0;
	if (queue_drag.moved && point.y < view.y1 + QUEUE_EDGE_PX) {
		step = -QUEUE_EDGE_STEP;
	} else if (queue_drag.moved && point.y > view.y2 - QUEUE_EDGE_PX) {
		step = QUEUE_EDGE_STEP;
	}
	if (step != 0) {
		int scroll = lv_obj_get_scroll_y(queue_list);
		int wanted = scroll + step;
		int max = queue_scroll_max();
		wanted = wanted < 0 ? 0 : (wanted > max ? max : wanted);
		if (wanted != scroll) {
			lv_obj_scroll_to_y(queue_list, wanted, LV_ANIM_OFF);
		}
	}

	queue_ghost_follow(point);

	// The position the finger is in front of, counting from the middle of each
	// row of what is to play. Over the history, or over the top half of the
	// track playing, there is nowhere to put it.
	lv_area_t body;
	lv_obj_get_coords(queue_body, &body);
	int y = point.y - body.y1;
	int upcoming = queue_now_top() + QUEUE_HEADER_H;
	int target = -1;
	if (y >= upcoming + QUEUE_ROW_PITCH / 2) {
		target = queue_current + (y - upcoming + QUEUE_ROW_PITCH / 2) / QUEUE_ROW_PITCH;
		if (target > queue_total) {
			target = queue_total;
		}
	}
	// Where the row already is is not a move.
	if (target == queue_drag.from || target == queue_drag.from + 1) {
		target = -1;
	}
	queue_drag.target = target;
	if (target >= 0 && queue_drag.moved) {
		queue_drop_line_show(target);
	} else if (queue_drop_line) {
		lv_obj_set_hidden(queue_drop_line, true);
	}
}

static void queue_drag_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *target = lv_event_get_current_target(e);
	if (code == LV_EVENT_PRESSED && queue_mode == QUEUE_REORDER && !queue_drag.active) {
		// A grip, taken hold of.
		for (int i = 0; i < QUEUE_ROW_POOL; i++) {
			queue_row_t *row = &queue_rows[i];
			if (row->grip == target && row->index > queue_current && row->index < queue_total) {
				lv_indev_t *indev = lv_indev_active();
				lv_point_t point = {0, 0};
				if (indev) {
					lv_indev_get_point(indev, &point);
				}
				queue_drag_start(row, target, point);
				break;
			}
		}
		return;
	}
	if (!queue_drag.active || target != queue_drag.held) {
		return;
	}

	if (code == LV_EVENT_PRESSING) {
		// The swipe back must not see a carried row go sideways.
		lv_event_stop_bubbling(e);
		lv_indev_t *indev = lv_indev_active();
		if (indev) {
			lv_point_t point;
			lv_indev_get_point(indev, &point);
			queue_drag_follow(point);
		}
		return;
	}
	if (code == LV_EVENT_RELEASED) {
		// A row carried by itself is let go of as if tapped, and the click
		// comes next.
		queue_drop_guard = queue_drag.by_row;
		queue_drag_end(true);
	} else if (code == LV_EVENT_PRESS_LOST) {
		queue_drag_end(false);
	}
}

static void queue_row_pressed_cb(lv_event_t *e) {
	(void)e;
	queue_drop_guard = false;
}

static void queue_row_long_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active() || queue_drag.active) {
		return;
	}
	lv_obj_t *button = lv_event_get_current_target(e);
	queue_row_t *row = queue_row_of(button);
	if (!row) {
		return;
	}
	lv_indev_t *indev = lv_indev_active();

	// Only a row of the history: picked up, to be put down in what is to play.
	// On any other row a long press is a slow tap.
	if (row->index >= queue_current || queue_mode == QUEUE_SELECT || !indev) {
		return;
	}
	lv_point_t point;
	lv_indev_get_point(indev, &point);
	queue_drag_start(row, button, point);
}

static void queue_remove_cb(lv_event_t *e) {
	(void)e;
	if (queue_mode != QUEUE_SELECT || queue_chosen_count == 0) {
		return;
	}
	int *indices = malloc((size_t)queue_chosen_count * sizeof(*indices));
	int n = 0;
	for (int i = 0; indices && i < queue_total && n < queue_chosen_count; i++) {
		if (queue_chosen[i]) {
			indices[n++] = i;
		}
	}
	queue_mode_set(QUEUE_IDLE);
	bool removed = indices && playlist_remove(indices, n);
	free(indices);
	if (!removed) {
		return;
	}
	// Written down now, as an addition is: the mirror on disk is otherwise
	// refreshed only when a track is loaded.
	device_state_queue_changed();
	toast_success("queue_tracks_removed");
	// Only what is to play can go, so the page stays where it is.
	int scroll = lv_obj_get_scroll_y(queue_list);
	queue_layout();
	queue_scroll_to(scroll, LV_ANIM_OFF);
	queue_window_update();
	queue_mode_show();
}

static void queue_close_cb(lv_event_t *e) {
	(void)e;
	queue_mode_set(QUEUE_IDLE);
}

static void queue_select_cb(lv_event_t *e) {
	(void)e;
	if (queue_mode == QUEUE_IDLE) {
		queue_selection_start();
	}
}

// On, and on again turns it off. Every drop is written down when it happens,
// so there is nothing to save on the way out.
static void queue_reorder_cb(lv_event_t *e) {
	(void)e;
	queue_drag_end(false);
	queue_mode_set(queue_mode == QUEUE_REORDER ? QUEUE_IDLE : QUEUE_REORDER);
}

static void queue_row_clicked_cb(lv_event_t *e) {
	if (queue_drop_guard) {
		queue_drop_guard = false;
		return;
	}
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}

	queue_row_t *row = queue_row_of(lv_event_get_current_target(e));
	if (!row) {
		return;
	}
	switch (queue_mode) {
	case QUEUE_SELECT:
		queue_selection_toggle(row);
		return;
	case QUEUE_REORDER:
		return; // the rows are being arranged, not played
	case QUEUE_IDLE:
		break;
	}
	if (device_state_play_queue_index(row->index)) {
		player_refresh_now_playing();
		switch_screen(player_screen);
	}
}

static void queue_loaded_cb(lv_event_t *e) {
	(void)e;
	queue_rebuild();
}

static void queue_unloaded_cb(lv_event_t *e) {
	(void)e;
	queue_drag_end(false);
	queue_mode_set(QUEUE_IDLE);
}

static lv_obj_t *queue_corner_button(gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph, lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(queue_screen);
	lv_obj_set_size(button, 56, 56);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * (56 + 6), cfg->padding + cfg->top_bar_height);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
	lv_obj_center(icon);
	lv_obj_set_hidden(button, true);
	return button;
}

static lv_obj_t *queue_header(const char *tag, int y) {
	lv_obj_t *label = lv_label_create(queue_body);
	lv_label_set_text(label, tr(tag));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_22_bold, 0);
	lv_obj_set_pos(label, 4, y + 10);
	lv_obj_set_hidden(label, true);
	return label;
}

static void build_queue_page(gui_config_t *cfg) {
	lv_obj_add_style(queue_screen, &theme_style_screen, 0);
	queue_title = settingsrow_title(queue_screen, cfg, "trackmenu_queue");
	// Two pairs in the same two places: the ways in, and choosing.
	queue_reorder_btn = queue_corner_button(cfg, 0, &icon_arrow_down_up, queue_reorder_cb);
	queue_select_btn = queue_corner_button(cfg, 1, &icon_select_mode, queue_select_cb);
	queue_close_btn = queue_corner_button(cfg, 0, &icon_close, queue_close_cb);
	queue_remove_btn = queue_corner_button(cfg, 1, &icon_trash, queue_remove_cb);

	int content_top = settingsrow_content_top(cfg);
	int width = cfg->screen_width - 2 * cfg->padding;
	queue_view_h = cfg->screen_height - content_top;

	queue_list = lv_obj_create(queue_screen);
	lv_obj_set_size(queue_list, cfg->screen_width, queue_view_h);
	lv_obj_align(queue_list, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(queue_list, 0, 0);
	lv_obj_set_style_border_width(queue_list, 0, 0);
	lv_obj_set_style_radius(queue_list, 0, 0);
	lv_obj_set_style_pad_hor(queue_list, cfg->padding, 0);
	lv_obj_set_style_pad_ver(queue_list, 0, 0);
	lv_obj_set_scroll_dir(queue_list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(queue_list, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_add_event_cb(queue_list, queue_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
	lv_obj_add_event_cb(queue_list, queue_scroll_cb, LV_EVENT_SCROLL, NULL);
	lv_obj_add_event_cb(queue_list, queue_scroll_cb, LV_EVENT_SCROLL_END, NULL);
	lv_obj_add_event_cb(queue_list, queue_scroll_cb, LV_EVENT_RELEASED, NULL);

	queue_body = lv_obj_create(queue_list);
	lv_obj_set_width(queue_body, width);
	lv_obj_set_height(queue_body, QUEUE_ROW_PITCH);
	lv_obj_set_pos(queue_body, 0, 0);
	lv_obj_set_style_bg_opa(queue_body, 0, 0);
	lv_obj_set_style_border_width(queue_body, 0, 0);
	lv_obj_set_style_pad_all(queue_body, 0, 0);
	lv_obj_set_scrollable(queue_body, false);
	lv_obj_set_event_bubble(queue_body, true);

	queue_history_header = queue_header("queue_history", 0);
	queue_now_header = queue_header("queue_now_playing", 0);

	queue_empty = lv_label_create(queue_list);
	lv_label_set_text(queue_empty, tr("trackmenu_queue_empty"));
	lv_obj_set_style_text_align(queue_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(queue_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(queue_empty, &font_ui_24, 0);
	lv_obj_align(queue_empty, LV_ALIGN_TOP_MID, 0, 120);
	lv_obj_set_hidden(queue_empty, true);

	for (int i = 0; i < QUEUE_ROW_POOL; i++) {
		queue_row_t *row = &queue_rows[i];

		row->button = lv_btn_create(queue_body);
		lv_obj_set_size(row->button, width, QUEUE_ROW_HEIGHT);
		lv_obj_set_x(row->button, 0);
		lv_obj_add_style(row->button, &theme_style_card, 0);
		lv_obj_add_style(row->button, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_radius(row->button, 12, 0);
		lv_obj_set_style_border_width(row->button, 0, 0);
		lv_obj_set_style_shadow_width(row->button, 0, 0);
		lv_obj_set_style_pad_all(row->button, 8, 0);
		lv_obj_set_style_pad_column(row->button, 14, 0);
		lv_obj_set_hidden(row->button, true);
		lv_obj_set_event_bubble(row->button, true);
		lv_obj_add_event_cb(row->button, queue_row_pressed_cb, LV_EVENT_PRESSED, NULL);
		lv_obj_add_event_cb(row->button, queue_row_clicked_cb, LV_EVENT_CLICKED, NULL);
		lv_obj_add_event_cb(row->button, queue_row_long_cb, LV_EVENT_LONG_PRESSED, NULL);
		lv_obj_add_event_cb(row->button, queue_drag_cb, LV_EVENT_PRESSING, NULL);
		lv_obj_add_event_cb(row->button, queue_drag_cb, LV_EVENT_RELEASED, NULL);
		lv_obj_add_event_cb(row->button, queue_drag_cb, LV_EVENT_PRESS_LOST, NULL);
		lv_obj_set_flex_flow(row->button, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row->button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

		row->icon = lv_image_create(row->button);
		lv_obj_set_size(row->icon, QUEUE_THUMB_SIZE, QUEUE_THUMB_SIZE);
		lv_image_set_inner_align(row->icon, LV_IMAGE_ALIGN_CENTER);

		row->label = lv_label_create(row->button);
		lv_label_set_long_mode(row->label, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(row->label, 1);
		lv_obj_set_height(row->label, 32);
		lv_obj_add_style(row->label, &theme_style_text, 0);
		lv_obj_set_style_text_font(row->label, &font_ui_24, 0);

		row->check = lv_image_create(row->button);
		lv_image_set_src(row->check, &icon_check);
		lv_obj_add_style(row->check, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(row->check, LV_OPA_COVER, 0);
		lv_obj_set_clickable(row->check, false);
		lv_obj_set_hidden(row->check, true);

		// The handle, while reordering. Not a button: it is the part of the
		// row that can be taken hold of, and a drag that starts on it carries
		// the row, not the list.
		row->grip = lv_obj_create(row->button);
		lv_obj_set_size(row->grip, QUEUE_GRIP_SIZE, QUEUE_GRIP_SIZE);
		lv_obj_set_style_bg_opa(row->grip, 0, 0);
		lv_obj_set_style_border_width(row->grip, 0, 0);
		lv_obj_set_style_pad_all(row->grip, 0, 0);
		lv_obj_set_scrollable(row->grip, false);
		lv_obj_set_clickable(row->grip, true);
		lv_obj_set_scroll_chain_ver(row->grip, false);
		lv_obj_set_scroll_chain_hor(row->grip, false);
		lv_obj_t *grip_icon = lv_image_create(row->grip);
		lv_image_set_src(grip_icon, &icon_grip);
		lv_obj_add_style(grip_icon, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(grip_icon, LV_OPA_COVER, 0);
		lv_obj_center(grip_icon);
		lv_obj_add_event_cb(row->grip, queue_drag_cb, LV_EVENT_PRESSED, NULL);
		lv_obj_add_event_cb(row->grip, queue_drag_cb, LV_EVENT_PRESSING, NULL);
		lv_obj_add_event_cb(row->grip, queue_drag_cb, LV_EVENT_RELEASED, NULL);
		lv_obj_add_event_cb(row->grip, queue_drag_cb, LV_EVENT_PRESS_LOST, NULL);
		lv_obj_set_hidden(row->grip, true);

		row->index = -1;
		row->has_thumb = false;
	}

	// Over everything, and transparent to the finger: only the copy of a
	// carried row is ever on it, and only while one is carried.
	queue_drag_layer = lv_obj_create(queue_screen);
	lv_obj_set_size(queue_drag_layer, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(queue_drag_layer, 0, 0);
	lv_obj_set_style_bg_opa(queue_drag_layer, 0, 0);
	lv_obj_set_style_border_width(queue_drag_layer, 0, 0);
	lv_obj_set_style_pad_all(queue_drag_layer, 0, 0);
	lv_obj_set_scrollable(queue_drag_layer, false);
	lv_obj_set_clickable(queue_drag_layer, false);
	lv_obj_set_ignore_layout(queue_drag_layer, true);

	// No player-sheet swipe on the queue: it kept sliding the player over the
	// list being read. The swipe-back drag still works from here.
	switcher_attach_back_gesture(queue_list);
	lv_obj_add_event_cb(queue_screen, queue_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(queue_screen, queue_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}

// ---------------------------------------------------------------------------
// Details: everything known about the playing file.
// ---------------------------------------------------------------------------

static void details_add_row(const char *name, const char *value) {
	if (!value || !value[0]) {
		return; // an empty field is noise, not information
	}

	lv_obj_t *row = lv_obj_create(details_card);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, 0, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_gap(row, 2, 0);
	lv_obj_set_scrollable(row, false);
	// Presses over the text must climb to the scroll surface, or the swipe
	// gestures can never start on the card's content.
	lv_obj_set_event_bubble(row, true);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);

	lv_obj_t *name_label = lv_label_create(row);
	lv_label_set_text(name_label, tr(name));
	lv_obj_add_style(name_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(name_label, &font_ui_18, 0);

	lv_obj_t *value_label = lv_label_create(row);
	lv_label_set_text(value_label, value);
	lv_label_set_long_mode(value_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(value_label, lv_pct(100));
	lv_obj_add_style(value_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(value_label, &font_ui_24, 0);
}

// Which file the page describes: a specific one (opened from a library
// list's popover) or, when empty, whatever is playing right now (opened from
// the player's own menu).
static char details_path[512];

// The common tail of both variants: tags, format, size, path.
// `codec` is the name the decoder reports ("MP3", "AAC", "ALAC"...), empty when
// it has none; `kbps` is the bitrate the file declares, 0 when it declares
// none; `lossy` says the format discards information. See decode.h: for a lossy
// file the bit/kHz pair describes the PCM leaving the decoder, not what was
// encoded.
// One ReplayGain figure from the tags: the correction, and the peak it was
// measured against when the file gives one.
static void details_add_gain(const char *name, float gain_db, float peak) {
	char buffer[64];
	if (peak > 0) {
		snprintf(buffer, sizeof(buffer), tr("trackmenu_rg_gain_peak"), gain_db, peak);
	} else {
		snprintf(buffer, sizeof(buffer), "%+.2f dB", gain_db);
	}
	details_add_row(name, buffer);
}

// `playing` adds what the player is applying to this file right now, which
// only exists for the track being played.
static void details_fill(const char *file, const song_metadata_t *tags, int bits, double sample_rate,
						 int channels, double duration_secs, const char *codec, int kbps, bool lossy,
						 bool playing) {
	char buffer[64];

	const char *slash = strrchr(file, '/');
	details_add_row("trackmenu_title", tags->title[0] ? tags->title : (slash ? slash + 1 : file));
	details_add_row("trackmenu_artist", tags->artist);
	details_add_row("trackmenu_album_artist", tags->album_artist);
	details_add_row("trackmenu_album", tags->album);
	details_add_row("genre", tags->genre);

	if (tags->year > 0) {
		snprintf(buffer, sizeof(buffer), "%d", tags->year);
		details_add_row("trackmenu_year", buffer);
	}
	if (tags->track_number > 0) {
		snprintf(buffer, sizeof(buffer), "%d", tags->track_number);
		details_add_row("track", buffer);
	}
	if (tags->disc_number > 0) {
		snprintf(buffer, sizeof(buffer), "%d", tags->disc_number);
		details_add_row("trackmenu_disc_number", buffer);
	}

	// A lossy file is described by its bitrate, not by bits and kHz: those two
	// numbers belong to the PCM leaving the decoder and are identical for a
	// 96 kbps AAC and a 320 kbps one. Lossless files keep the usual pair.
	if (lossy) {
		if (kbps > 0 && codec && codec[0]) {
			snprintf(buffer, sizeof(buffer), "%d kbps %s", kbps, codec);
		} else if (kbps > 0) {
			snprintf(buffer, sizeof(buffer), "%d kbps", kbps);
		} else {
			snprintf(buffer, sizeof(buffer), "%s", codec && codec[0] ? codec : "");
		}
		if (buffer[0]) {
			details_add_row("trackmenu_format", buffer);
		}
	} else if (sample_rate > 0) {
		snprintf(buffer, sizeof(buffer), tr("trackmenu_bit_khz_channels"), bits > 0 ? bits : 16,
				 sample_rate / 1000.0, channels);
		details_add_row("trackmenu_format", buffer);
	}

	// A separate bitrate row, only for local lossless tracks: size over
	// duration, which is how dense the file is. Lossy files already carry it in
	// the row above, and for a streamed track the cache file may be half
	// downloaded, making the number meaningless.
	if (!lossy && !qobuzcache_owns(file) && !tidalcache_owns(file) && duration_secs > 0.5) {
		struct stat st_kbps;
		if (stat(file, &st_kbps) == 0 && st_kbps.st_size > 0) {
			int avg = (int)((double)st_kbps.st_size * 8.0 / duration_secs / 1000.0 + 0.5);
			snprintf(buffer, sizeof(buffer), "%d kbps", avg);
			details_add_row("trackmenu_bitrate", buffer);
		}
	}

	if (tags->has_track_gain) {
		details_add_gain("trackmenu_rg_track", tags->track_gain_db, tags->track_peak);
	}
	if (tags->has_album_gain) {
		details_add_gain("trackmenu_rg_album", tags->album_gain_db, tags->album_peak);
	}
	// The applied figure can differ from both tags: it is the one the mode
	// picks, lowered when the peak would clip, and capped at +12 dB.
	if (playing && (tags->has_track_gain || tags->has_album_gain)) {
		if (replaygain_mode() == REPLAYGAIN_OFF) {
			details_add_row("trackmenu_rg_applied", tr("trackmenu_rg_off"));
		} else {
			snprintf(buffer, sizeof(buffer), "%+.2f dB", replaygain_current_db());
			details_add_row("trackmenu_rg_applied", buffer);
		}
	}

	// No size row for a streamed track: it would be the size of the cache file,
	// which may still be downloading -- a number that grows on screen by itself
	// and describes the download, not the track.
	struct stat st;
	if (!qobuzcache_owns(file) && !tidalcache_owns(file) && stat(file, &st) == 0) {
		snprintf(buffer, sizeof(buffer), "%.1f MB", st.st_size / (1024.0 * 1024.0));
		details_add_row("trackmenu_size", buffer);
	}

	// The path, except for a streamed track: that lives in a cache directory
	// nobody chose, under a numeric name, and showing it implies a file of the
	// user's own that does not exist. The source service is shown instead.
	if (qobuzcache_owns(file)) {
		details_add_row("trackmenu_source", "qobuz");
	} else if (tidalcache_owns(file)) {
		details_add_row("trackmenu_source", "tidal");
	} else {
		details_add_row("trackmenu_file", file);
	}
}

static void details_rebuild(void) {
	lv_obj_clean(details_card);

	if (details_path[0]) {
		// An arbitrary library track: tags read from the file itself, format
		// from the decoder's header (opened and closed without decoding).
		song_metadata_t tags;
		metadata_read(details_path, &tags);

		int bits = 0, channels = 0;
		double sample_rate = 0;
		decode_format_t format = decode_detect_format(details_path);
		double duration_secs = 0;
		char codec[16] = "";
		int kbps = 0;
		bool lossy = false;
		decoder_t *dec = format != DECODE_FORMAT_UNKNOWN ? decoder_open_info(details_path, format) : NULL;
		if (dec) {
			bits = decoder_source_bits(dec);
			channels = decoder_channels(dec);
			sample_rate = decoder_sample_rate(dec);
			snprintf(codec, sizeof(codec), "%s", decoder_codec_name(dec));
			kbps = decoder_bitrate_kbps(dec);
			lossy = decoder_is_lossy(dec);
			uint64_t frames = decoder_total_pcm_frames(dec);
			if (sample_rate > 0 && frames > 0) {
				duration_secs = (double)frames / sample_rate;
			}
			decoder_close(dec);
		}

		// A lossy file that declares no bitrate: use the real average, which can
		// be computed here because the whole file is present (streamed tracks
		// never reach this branch).
		if (lossy && kbps <= 0 && duration_secs > 0.5) {
			struct stat st_lossy;
			if (stat(details_path, &st_lossy) == 0 && st_lossy.st_size > 0) {
				kbps = (int)((double)st_lossy.st_size * 8.0 / duration_secs / 1000.0 + 0.5);
			}
		}

		details_fill(details_path, &tags, bits, sample_rate, channels, duration_secs, codec, kbps, lossy,
					 false);
		return;
	}

	device_state_t state;
	device_state_get(&state);

	if (!state.current_file[0]) {
		details_add_row("trackmenu_state", tr("trackmenu_nothing_is_playing"));
		return;
	}

	// The stream, as it is actually being played.
	char codec[16] = "";
	audio_get_stream_codec(codec, sizeof(codec));
	bool lossy = audio_stream_is_lossy();
	int kbps = audio_get_stream_bitrate_kbps();

	// As in the player: when the file declares no bitrate, fall back to the real
	// average, but only for a local file -- a streamed one may still be half
	// downloaded and the number would climb by itself.
	if (lossy && kbps <= 0 && !qobuzcache_owns(state.current_file) && !tidalcache_owns(state.current_file) &&
		state.progress_total_secs > 0.5) {
		struct stat st_lossy;
		if (stat(state.current_file, &st_lossy) == 0 && st_lossy.st_size > 0) {
			kbps = (int)((double)st_lossy.st_size * 8.0 / state.progress_total_secs / 1000.0 + 0.5);
		}
	}

	details_fill(state.current_file, &state.metadata, audio_get_stream_bits(), state.stream_sample_rate,
				 state.stream_channels, state.progress_total_secs, codec, kbps, lossy, true);
}

static void details_loaded_cb(lv_event_t *e) {
	(void)e;
	details_rebuild();

	// Always start at the top. The page is built once and refilled on every
	// open, so the scroll container otherwise keeps the position from last time
	// and the card for another track opens halfway down. The first row is the
	// title, which is what should be visible on entry.
	//
	// After details_rebuild(), not before: that is what sets the content height,
	// and LVGL clamps a scroll past the bottom of whatever was there before.
	if (details_scroll) {
		lv_obj_scroll_to_y(details_scroll, 0, LV_ANIM_OFF);
	}
}

static void build_details_page(gui_config_t *cfg) {
	lv_obj_add_style(details_screen, &theme_style_screen, 0);
	settingsrow_title(details_screen, cfg, "details");

	int content_top = settingsrow_content_top(cfg);

	lv_obj_t *scroll = lv_obj_create(details_screen);
	lv_obj_set_size(scroll, cfg->screen_width, cfg->screen_height - content_top);
	lv_obj_align(scroll, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(scroll, 0, 0);
	lv_obj_set_style_border_width(scroll, 0, 0);
	lv_obj_set_style_radius(scroll, 0, 0);
	lv_obj_set_style_pad_hor(scroll, cfg->padding, 0);
	lv_obj_set_style_pad_ver(scroll, 0, 0);
	lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
	details_scroll = scroll;

	details_card = lv_obj_create(scroll);
	lv_obj_set_width(details_card, lv_pct(100));
	lv_obj_set_height(details_card, LV_SIZE_CONTENT);
	lv_obj_add_style(details_card, &theme_style_card, 0);
	lv_obj_set_style_radius(details_card, 12, 0);
	lv_obj_set_style_border_width(details_card, 0, 0);
	lv_obj_set_style_shadow_width(details_card, 0, 0);
	lv_obj_set_style_pad_all(details_card, 18, 0);
	lv_obj_set_style_pad_gap(details_card, 14, 0);
	lv_obj_set_scrollable(details_card, false);
	lv_obj_set_event_bubble(details_card, true);
	lv_obj_set_flex_flow(details_card, LV_FLEX_FLOW_COLUMN);

	// Deliberately not player_sheet_attach_drag(): this page is opened from the
	// player, so dragging it sideways back into the player was one gesture
	// meaning two contradictory things. Removing details_screen from gui.c's
	// swipe list was not enough on its own -- the drag was attached here, to the
	// scroll container, and that is what kept working.
	switcher_attach_back_gesture(scroll);
	lv_obj_add_event_cb(details_screen, details_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}

// ---------------------------------------------------------------------------
// the popover
// ---------------------------------------------------------------------------

static void open_queue_cb(void *user) {
	(void)user;
	trackmenu_open_queue();
}

// The same page without going through the menu: the player calls this when a
// podcast is playing, where the overflow button becomes the episodes button --
// and a podcast's episodes are exactly the queue.
void trackmenu_open_queue(void) {
	player_sheet_close(false);
	switch_screen(queue_screen);
	switcher_set_player_return(queue_screen);
}

static void open_details_cb(void *user) {
	(void)user;
	details_path[0] = '\0'; // the playing track
	player_sheet_close(false);
	switch_screen(details_screen);
	switcher_set_player_return(details_screen);
}

// "Add to playlist" for the track the player has loaded: the same picker the
// library lists open, on the playing file.
static void add_to_playlist_cb(void *user) {
	(void)user;
	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0]) {
		return;
	}
	player_sheet_close(false);
	playlistpage_add_track(state.current_file);
	switcher_set_player_return(playlistpage_screen);
}

// "Show album": the record the playing track came off. Backing out of it
// returns to the player, like everything else this menu opens.
static void open_album_cb(void *user) {
	(void)user;

	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0]) {
		return;
	}

	// Podcasts come first, because there is no album to show: the other episodes
	// are what is wanted. The feed id was kept by the sidecar file when the
	// episode was downloaded; the "album" title alone would not do, because two
	// podcasts can share a name and the catalogue is queried by id.
	long long feed_id = 0;
	if (podcastcache_feed_id(state.current_file, &feed_id)) {
		device_state_t now;
		device_state_get(&now);
		player_sheet_close(false);
		if (!podcastpage_open_feed(feed_id, now.metadata.album)) {
			gui_notify_popup("trackmenu_podcast_failed");
			return;
		}
		switcher_set_player_return(podcastcard_feed_screen);
		return;
	}

	// A Qobuz track is not in the local library, so its album is not looked up
	// in the database: it is reopened on Qobuz, using the id the sidecar file
	// kept when the track was downloaded.
	char qobuz_album[40];
	if (qobuzcache_album_id(state.current_file, qobuz_album, sizeof(qobuz_album))) {
		char title[256];
		if (!library_track_album(state.current_file, title, sizeof(title)) || !title[0]) {
			device_state_t now;
			device_state_get(&now);
			snprintf(title, sizeof(title), "%s", now.metadata.album);
		}
		player_sheet_close(false);
		if (!qobuzpage_open_album(qobuz_album, title)) {
			gui_notify_popup("trackmenu_qobuz_album_failed");
			return;
		}
		switcher_set_player_return(qobuz_list_screen);
		return;
	}

	// Same for Tidal, for the same reason: a streamed track's album is reopened
	// on the service it came from, not in the local database, where it is not.
	char tidal_album[40];
	if (tidalcache_album_id(state.current_file, tidal_album, sizeof(tidal_album))) {
		char title[256];
		if (!library_track_album(state.current_file, title, sizeof(title)) || !title[0]) {
			device_state_t now;
			device_state_get(&now);
			snprintf(title, sizeof(title), "%s", now.metadata.album);
		}
		player_sheet_close(false);
		if (!tidalpage_open_album(tidal_album, title)) {
			gui_notify_popup("trackmenu_tidal_album_failed");
			return;
		}
		switcher_set_player_return(tidal_list_screen);
		return;
	}

	char album[256];
	if (!library_track_album_value(state.current_file, album, sizeof(album)) || !album[0]) {
		gui_notify_popup("trackmenu_no_album");
		return;
	}

	player_sheet_close(false);
	medialist_open(album, LIBRARY_LIST_TRACKS, LIBRARY_FILTER_ALBUM, album);
	// Over a track list the album takes that list's place on the same page,
	// and the page brings the list back with the player over it itself.
	if (!medialist_step_back_to_player()) {
		switcher_set_player_return(medialist_tracks_screen);
	}
}

static void open_settings_cb(void *user) {
	(void)user;
	player_sheet_close(false);
	switch_screen(musicsettings_screen);
	switcher_set_player_return(musicsettings_screen);
}

// Public: the details page for a specific file, from a library list's menu.
// NOT from the player, so backing out returns to the list, not the sheet.
void trackmenu_details_open(const char *path) {
	if (!path || !path[0]) {
		return;
	}
	snprintf(details_path, sizeof(details_path), "%s", path);
	switch_screen(details_screen);
	switcher_set_player_return(NULL);
}

void trackmenu_open(lv_obj_t *anchor) {
	// Two tables rather than one with the label patched at runtime, because of
	// the string-extraction tool: it reads popover_item_t initialisers (see
	// LABEL_STRUCTS in tools/extract_strings.py), not assignments. A
	// "trackmenu_show_podcast" written with an equals sign would never reach the
	// language files and would stay untranslated everywhere.
	//
	// The action behind both is the same: open_album_cb inspects where the
	// playing file came from and decides for itself. Only the wording differs,
	// because a podcast episode has no album and calling it one would be a
	// borrowed term to make a menu line up.
	static const popover_item_t ITEMS[] = {
		{"trackmenu_queue", open_queue_cb, NULL},
		{"add_to_playlist", add_to_playlist_cb, NULL},
		{"show_album", open_album_cb, NULL},
		{"details", open_details_cb, NULL},
		{"settings", open_settings_cb, NULL},
	};
	static const popover_item_t ITEMS_PODCAST[] = {
		{"trackmenu_queue", open_queue_cb, NULL},
		{"add_to_playlist", add_to_playlist_cb, NULL},
		{"trackmenu_show_podcast", open_album_cb, NULL},
		{"details", open_details_cb, NULL},
		{"settings", open_settings_cb, NULL},
	};

	device_state_t state;
	device_state_get(&state);
	bool podcast = state.current_file[0] && podcastcache_is_episode(state.current_file);

	popover_show(anchor, podcast ? ITEMS_PODCAST : ITEMS, 5);
}

void trackmenu_init(gui_config_t *cfg) {
	g_cfg = cfg;
	(void)g_cfg;
	build_queue_page(cfg);
	build_details_page(cfg);
}
