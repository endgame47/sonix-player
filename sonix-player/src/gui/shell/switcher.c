#include "switcher.h"

#include <stdio.h>

#include "lvgl/lvgl.h"

#include "src/gui/library/browser.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/library/libraryscan.h"
#include "src/gui/shell/main_menu.h"
#include "src/gui/library/music.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/nowplaying/trackmenu.h"

// Diameter of the round back button. Small enough to stay out of the way of
// the artwork, big enough to still be a comfortable touch target.
#define BACK_BTN_SIZE 56

static lv_obj_t *back_btn;
static lv_obj_t *back_btn_icon;

// Chevron colour state: white only when it sits over the player's dark
// artwork backdrop (see back_btn_over_cover / back_btn_player_mode).
static bool chevron_in_player;
static bool chevron_over_cover;

// Where the back button sits depends on whether the page below it shows the
// status bar: normally it tucks in underneath, on the player (status bar
// hidden, artwork at the top) it moves up to the top edge.
static int8_t back_btn_padding;
static int8_t back_btn_top_bar_height;

// On the player the status bar is hidden, so there is no row of its own for the
// chevron to line up with and it would sit against the top edge. This is the
// air it keeps instead, and what the player's own header row is aligned to.
#define BACK_BTN_PLAYER_TOP 14

static void place_back_btn(bool below_top_bar) {
	// Same offsets as the corner buttons on the right (playlists, settings,
	// options): the whole header row of round buttons sits on one line. On
	// the player (below_top_bar false) the status bar is not there to sit
	// under, so the chevron takes the offset above.
	int y = back_btn_padding + (below_top_bar ? back_btn_top_bar_height : BACK_BTN_PLAYER_TOP);
	lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, back_btn_padding, y);
}

// Where the back button goes. A page can be reached by more than one route --
// the player from the Musica page or from a track in the browser -- so the way
// back is whatever was on screen before, not a fixed parent.
#define SCREEN_HISTORY_DEPTH 8

static lv_obj_t *screen_history[SCREEN_HISTORY_DEPTH];
static int screen_history_len;

static void history_push(lv_obj_t *screen) {
	if (!screen) {
		return;
	}

	if (screen_history_len == SCREEN_HISTORY_DEPTH) {
		// Drop the oldest rather than refusing to record: a very deep path
		// still ends up walking back to the menu, just not through every step.
		for (int i = 1; i < SCREEN_HISTORY_DEPTH; i++) {
			screen_history[i - 1] = screen_history[i];
		}
		screen_history_len--;
	}

	screen_history[screen_history_len++] = screen;
}

static lv_obj_t *history_pop(void) {
	if (screen_history_len == 0) {
		return NULL;
	}
	return screen_history[--screen_history_len];
}

static void load_screen(lv_obj_t *target_screen);

// Forgets the whole back-stack: the next chevron press lands on the main
// menu. For pages that end a one-way flow (a finished scan) where walking
// back through the flow's screens would make no sense.
void screen_history_reset(void) {
	screen_history_len = 0;
	switcher_set_player_return(NULL);
}

// Loads a screen without touching the back-stack. For restoring a page that
// was never really left -- the player sheet putting its opener back.
void switch_screen_no_history(lv_obj_t *target_screen) { load_screen(target_screen); }

// The page (queue, details, the music settings...) that was opened from the
// player's own menu: leaving it goes back INTO the player, not to the page
// the sheet originally slid over. Set by the opener, consumed by the back
// navigation, dropped by anything that makes it stale.
static lv_obj_t *player_return_screen;

void switcher_set_player_return(lv_obj_t *screen) { player_return_screen = screen; }

static bool back_goes_to_player(void) {
	return player_return_screen && lv_screen_active() == player_return_screen;
}

// ---------------------------------------------------------------------------
// The iOS swipe-back, done properly: the page follows the finger to the
// right, and the page it would go back to shows underneath (a snapshot,
// slightly dimmed, exactly like iOS's parked previous page). Past a third of
// the width on release it completes; short of that it snaps back.
// ---------------------------------------------------------------------------

#define BACK_DRAG_COMMIT_PX 10	// sideways movement that starts the drag
#define BACK_COMMIT_FRACTION 3	// release beyond width/3 goes back
#define BACK_ANIM_MS 140

static bool back_dragging;			// a press is being tracked
static bool back_drag_engaged;		// this press has committed to the drag
static bool back_drag_click_guard;	// suppresses the tap the drag ends with
static bool back_anim_running;
static lv_point_t back_drag_start;
static lv_obj_t *back_underlay;			// full-screen container on the bottom layer
static lv_draw_buf_t *back_snapshot;	// the previous page, rendered once

bool switcher_back_drag_active(void) { return back_drag_click_guard; }

static bool back_stays_in_page(void);

static void back_drag_click_guard_clear_cb(void *unused) {
	(void)unused;
	back_drag_click_guard = false;
}

// The chevron belongs to the page, so it rides along with the drag (and the
// animation that finishes it) instead of hanging in the air.
static void back_btn_set_drag_offset(int32_t x) {
	if (back_btn) {
		lv_obj_set_style_translate_x(back_btn, x, 0);
	}
}

// Public flavour of the same, for the player sheet: its chevron rides the
// sheet while it is dragged or animated in and out.
void back_btn_translate(int32_t x) { back_btn_set_drag_offset(x); }

int back_btn_centre_y(void) {
	if (!back_btn) {
		return back_btn_padding + BACK_BTN_PLAYER_TOP;
	}
	return back_btn_padding + BACK_BTN_PLAYER_TOP + lv_obj_get_height(back_btn) / 2;
}

static void back_underlay_destroy(void) {
	if (back_underlay) {
		lv_obj_delete(back_underlay);
		back_underlay = NULL;
	}
	if (back_snapshot) {
		lv_draw_buf_destroy(back_snapshot);
		back_snapshot = NULL;
	}
}

// Builds what shows under the sliding page: a snapshot of the page the drag
// goes back to. Inside a browser subfolder the "previous page" is the same
// screen showing the parent folder -- that can't be rendered ahead of time,
// so a plain page-coloured panel stands in.
static void back_underlay_build(void) {
	back_underlay_destroy();

	back_underlay = lv_obj_create(lv_layer_bottom());
	lv_obj_set_size(back_underlay, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(back_underlay, 0, 0);
	lv_obj_set_style_bg_color(back_underlay, theme()->screen_bg, 0);
	lv_obj_set_style_bg_opa(back_underlay, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(back_underlay, 0, 0);
	lv_obj_set_style_radius(back_underlay, 0, 0);
	lv_obj_set_style_pad_all(back_underlay, 0, 0);
	lv_obj_remove_flag(back_underlay, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(back_underlay, LV_OBJ_FLAG_CLICKABLE);

	lv_obj_t *target = NULL;
	bool going_to_player = false;
	if (lv_screen_active() == browser_screen && browser_can_go_up()) {
		target = NULL; // parent folder: no ready-made page to show
	} else if (back_stays_in_page()) {
		// The step back stays on this same page (a list's internal stack:
		// episodes returning to results, results returning to the chips). What
		// will be there is not drawn anywhere yet, so the neutral panel goes
		// underneath, as for a browser folder. Using the top of the global
		// history here would show one page under the finger and land on a
		// different one at release.
		target = NULL;
	} else if (back_goes_to_player()) {
		// This page goes back INTO the player, so the player is what shows
		// underneath. It is parked hidden while closed; unhide it just for
		// the offscreen render.
		target = player_screen;
		going_to_player = true;
	} else if (screen_history_len > 0) {
		target = screen_history[screen_history_len - 1];
	} else {
		target = main_menu_screen;
	}

	if (target) {
		bool was_hidden = lv_obj_has_flag(target, LV_OBJ_FLAG_HIDDEN);
		if (was_hidden) {
			lv_obj_remove_flag(target, LV_OBJ_FLAG_HIDDEN);
		}
		back_snapshot = lv_snapshot_take(target, LV_COLOR_FORMAT_RGB565);
		if (was_hidden) {
			lv_obj_add_flag(target, LV_OBJ_FLAG_HIDDEN);
		}
		if (back_snapshot) {
			lv_obj_t *img = lv_image_create(back_underlay);
			lv_image_set_src(img, back_snapshot);
			lv_obj_set_pos(img, 0, 0);
		}
	}

	// The page underneath shows its own chevron, exactly as it will once the
	// drag lands on it -- every page except the main menu has one. The global
	// floating button is busy riding the page being dragged, so the underlay
	// carries a plain copy at the same spot.
	if (target != main_menu_screen) {
		lv_obj_t *chevron = lv_image_create(back_underlay);
		lv_image_set_src(chevron, &icon_chevron_left);
		// The same two placements as place_back_btn(): the player's own offset
		// where there is no status bar, the status bar's height everywhere else.
		int y = back_btn_padding + (going_to_player ? BACK_BTN_PLAYER_TOP : back_btn_top_bar_height);
		lv_obj_set_pos(chevron, back_btn_padding + 10, y + 10);
		if (going_to_player && chevron_over_cover) {
			lv_obj_set_style_image_recolor(chevron, lv_color_white(), 0);
		} else {
			lv_obj_set_style_image_recolor(chevron, theme()->text_primary, 0);
		}
		lv_obj_set_style_image_recolor_opa(chevron, LV_OPA_COVER, 0);
	}

	// The iOS dimming: the parked page sits in shadow until it comes forward.
	lv_obj_t *veil = lv_obj_create(back_underlay);
	lv_obj_set_size(veil, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(veil, 0, 0);
	lv_obj_set_style_bg_color(veil, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(veil, LV_OPA_30, 0);
	lv_obj_set_style_border_width(veil, 0, 0);
	lv_obj_set_style_radius(veil, 0, 0);
	lv_obj_remove_flag(veil, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(veil, LV_OBJ_FLAG_CLICKABLE);
}

static void back_anim_exec_cb(void *var, int32_t v) {
	lv_obj_set_x((lv_obj_t *)var, v);
	back_btn_set_drag_offset(v);
}

static void back_anim_commit_done_cb(lv_anim_t *a) {
	lv_obj_t *screen = (lv_obj_t *)a->var;
	back_btn_cb(NULL); // pops history / walks the browser up, loads the page below
	lv_obj_set_x(screen, 0);
	back_btn_set_drag_offset(0);
	back_underlay_destroy();
	back_anim_running = false;
	back_btn_sync_visibility();
}

static void back_anim_cancel_done_cb(lv_anim_t *a) {
	(void)a;
	back_btn_set_drag_offset(0);
	back_underlay_destroy();
	back_anim_running = false;
	back_btn_sync_visibility();
}

static bool back_gesture_off;

void back_gesture_blocked(bool blocked) {
	back_gesture_off = blocked;
	if (blocked) {
		back_dragging = false;
	}
}

static void back_drag_cb(lv_event_t *e) {
	if (back_gesture_off) {
		return;
	}

	lv_event_code_t code = lv_event_get_code(e);

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}

	if (code == LV_EVENT_PRESSED) {
		if (back_anim_running || player_sheet_is_open()) {
			return; // the sheet has its own horizontal gestures
		}
		lv_obj_t *screen = lv_screen_active();
		if (screen == main_menu_screen || screen == libraryscan_screen || screen == audiobookscan_screen) {
			return; // nowhere to go back to / scans exit through their buttons
		}
		lv_indev_get_point(indev, &back_drag_start);
		back_dragging = true;
		back_drag_engaged = false;
		return;
	}

	if (!back_dragging) {
		return;
	}

	lv_point_t point;
	lv_indev_get_point(indev, &point);
	int dx = point.x - back_drag_start.x;

	if (code == LV_EVENT_PRESSING) {
		if (!back_drag_engaged) {
			// Vertical scrolling must still work: the moment LVGL has begun
			// scrolling something, this press belongs to the scroll, full
			// stop. Otherwise the incidental horizontal drift of a fast
			// flick crosses the threshold and the list shimmies sideways.
			if (lv_indev_get_scroll_obj(indev)) {
				back_dragging = false;
				return;
			}
			int dy = point.y - back_drag_start.y;
			if (LV_ABS(dy) >= BACK_DRAG_COMMIT_PX && LV_ABS(dy) > LV_ABS(dx)) {
				back_dragging = false; // clearly a vertical gesture
				return;
			}
			if (dx <= -BACK_DRAG_COMMIT_PX) {
				back_dragging = false; // the player sheet's direction
				return;
			}
			// Only take over once the movement is decisively sideways.
			if (dx < BACK_DRAG_COMMIT_PX || LV_ABS(dx) < 2 * LV_ABS(dy)) {
				return;
			}
			back_drag_engaged = true;
			back_drag_click_guard = true;
			back_underlay_build();
		}

		lv_obj_set_x(lv_screen_active(), dx > 0 ? dx : 0);
		back_btn_set_drag_offset(dx > 0 ? dx : 0);
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		back_dragging = false;
		if (!back_drag_engaged) {
			return;
		}
		back_drag_engaged = false;

		lv_obj_t *screen = lv_screen_active();
		int width = lv_obj_get_width(screen);
		int from = dx > 0 ? dx : 0;
		bool go = dx > width / BACK_COMMIT_FRACTION;

		back_anim_running = true;
		lv_anim_t a;
		lv_anim_init(&a);
		lv_anim_set_var(&a, screen);
		lv_anim_set_exec_cb(&a, back_anim_exec_cb);
		lv_anim_set_values(&a, from, go ? width : 0);
		lv_anim_set_duration(&a, BACK_ANIM_MS);
		lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
		lv_anim_set_completed_cb(&a, go ? back_anim_commit_done_cb : back_anim_cancel_done_cb);
		lv_anim_start(&a);

		// Deferred, not cleared here: the CLICKED event of this same gesture
		// still has to see the guard (same dance as the player sheet's flag).
		lv_async_call(back_drag_click_guard_clear_cb, NULL);
	}
}

void switcher_attach_back_gesture(lv_obj_t *obj) {
	if (!obj) {
		return;
	}
	lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(obj, back_drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(obj, back_drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(obj, back_drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(obj, back_drag_cb, LV_EVENT_PRESS_LOST, NULL);
}

void switch_screen(lv_obj_t *target_screen) {
	// The player is not a page: it is a panel that slides in over whatever is
	// already on screen, so there is nothing to load and nothing to remember.
	if (target_screen == player_screen) {
		// Except when arriving from a page the player opened itself: the queue,
		// an audiobook's chapters, a Qobuz list. Tapping a track there brings
		// the player back up, but if the sheet slid over the queue, the next
		// chevron press would pull the queue out again instead of the page the
		// player was first opened from. Picking a track from the queue is not a
		// step forward: it is the same playback, and the queue is done.
		//
		// So the queue is closed here, exactly as the chevron would (see
		// back_btn_cb): the player return is consumed, the page underneath is
		// popped, and the sheet reopens over that.
		if (back_goes_to_player()) {
			player_return_screen = NULL;
			lv_obj_t *previous = history_pop();
			load_screen(previous ? previous : main_menu_screen);
		}
		player_sheet_open(true);
		return;
	}

	lv_obj_t *current = lv_screen_active();

	if (target_screen == main_menu_screen) {
		screen_history_len = 0; // the menu is the root; nothing above it
		player_return_screen = NULL;
	} else if (current && current != target_screen) {
		// Returning to the page just arrived from is a step back, not a step
		// forward: the history is shortened rather than extended.
		//
		// Without this check the stack grows like an accordion: leaving the
		// Wi-Fi password page once connected would record the password page,
		// and the next chevron press would pull it out again, asking for the
		// password of a network already connected to.
		if (screen_history_len > 0 && screen_history[screen_history_len - 1] == target_screen) {
			screen_history_len--;
		} else {
			history_push(current);
		}
	}

	load_screen(target_screen);
}

static void load_screen(lv_obj_t *target_screen) {
	// Every page keeps the status bar. The player is not a page -- it slides
	// over the top of one -- so nothing here has to make room for it.
	topbar_set_hidden(false);

	if (back_btn) {
		// The scan pages have their own way out (cancel / OK) and must not be
		// left halfway through by the chevron.
		if (target_screen == main_menu_screen || target_screen == libraryscan_screen ||
			target_screen == audiobookscan_screen) {
			lv_obj_add_flag(back_btn, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_HIDDEN);
		}

		place_back_btn(true);
	}

	// Re-read the folder on the way in: the card may have finished mounting
	// (or changed) since the browser last looked at it.
	if (target_screen == browser_screen) {
		browser_refresh();
	}

	// Opening the scan page is the request to scan.
	if (target_screen == libraryscan_screen) {
		libraryscan_begin();
	}

	// A page hidden through a theme, accent or tint change carries the palette
	// it was last walked for; this is where it catches up, one page at a time
	// instead of all of them at the moment of the change.
	theme_notify_screen_shown(target_screen);

	lv_screen_load(target_screen);
}

// Pages that have something to say before they will let go of the chevron.
// Both of the current ones are a page that is also the switch for a service --
// the Wi-Fi transfer and AirPlay -- and both ask before taking it down.
//
// A table rather than a single slot: with one slot, whichever page was built
// last would silently take the guard away from the other, and the symptom is a
// page that quietly stops asking.
//
// The table has to stay comfortably larger than the number of pages that
// register one, because filling it up is invisible from the outside: a dropped
// registration shows up only as a chevron that leaves the section instead of
// walking back through the page's own stack. That is what the log line below
// is for.
#define BACK_GUARDS_MAX 16

static struct {
	lv_obj_t *screen;
	bool (*handler)(void);
	bool (*peek)(void); // will the step back stay on this page?
} back_guards[BACK_GUARDS_MAX];

void switcher_set_back_guard(lv_obj_t *screen, bool (*handler)(void)) {
	if (!screen) {
		return;
	}

	// Setting the same page twice replaces its guard rather than using a
	// second slot.
	for (int i = 0; i < BACK_GUARDS_MAX; i++) {
		if (back_guards[i].screen == screen) {
			back_guards[i].handler = handler;
			return;
		}
	}

	for (int i = 0; i < BACK_GUARDS_MAX; i++) {
		if (!back_guards[i].screen) {
			back_guards[i].screen = screen;
			back_guards[i].handler = handler;
			return;
		}
	}

	// Not LV_LOG_WARN: that goes wherever LVGL's logging is pointed and is
	// compiled out in some builds, and what it costs to miss is a page whose
	// back button quietly leaves the section.
	fprintf(stderr, "switcher: no room for another back guard (the table holds %d)\n", BACK_GUARDS_MAX);
}

// The question the drag preview has to ask without moving anything: going back
// now, does the page stay the same? Pages that keep a stack of their own --
// Qobuz, Tidal, podcasts, every list that lives on one screen -- answer here,
// and the drag uses it to avoid drawing another screen's page underneath when
// that is not where the release will land.
void switcher_set_back_guard_peek(lv_obj_t *screen, bool (*peek)(void)) {
	if (!screen) {
		return;
	}
	for (int i = 0; i < BACK_GUARDS_MAX; i++) {
		if (back_guards[i].screen == screen) {
			back_guards[i].peek = peek;
			return;
		}
	}
}

// True when the step back from the active page will be consumed inside that
// page, i.e. its internal stack is not empty.
static bool back_stays_in_page(void) {
	lv_obj_t *active = lv_screen_active();
	for (int i = 0; i < BACK_GUARDS_MAX; i++) {
		if (back_guards[i].screen == active) {
			return back_guards[i].peek && back_guards[i].peek();
		}
	}
	return false;
}

// The guard for whatever page is on screen, or NULL.
static bool (*active_back_guard(void))(void) {
	lv_obj_t *active = lv_screen_active();
	for (int i = 0; i < BACK_GUARDS_MAX; i++) {
		if (back_guards[i].screen == active) {
			return back_guards[i].handler;
		}
	}
	return NULL;
}

void back_btn_cb(lv_event_t *e) {
	(void)e;

	// The player sits on top of everything else, so it is what the chevron
	// dismisses first.
	if (player_sheet_is_open()) {
		player_sheet_close(true);
		return;
	}

	// A page that wants a word first. It returns true when it has taken the
	// press for itself -- put up a question, say -- and false to let the
	// chevron carry on and leave.
	bool (*guard)(void) = active_back_guard();
	if (guard && guard()) {
		return;
	}

	// Inside the file browser the chevron is also what walks back up the
	// directory tree; it only leaves the page once there's nowhere left to go.
	if (lv_screen_active() == browser_screen && browser_go_up()) {
		return;
	}

	// A page opened from the player's own menu: leaving it goes back INTO
	// the player, over whatever page the sheet had originally slid across.
	if (back_goes_to_player()) {
		player_return_screen = NULL;
		lv_obj_t *previous = history_pop();
		load_screen(previous ? previous : main_menu_screen);
		player_sheet_open(false);
		return;
	}

	lv_obj_t *previous = history_pop();
	load_screen(previous ? previous : main_menu_screen);
}

void switch_screen_cb(lv_event_t *e) {
	// A swipe that started on this button is a swipe, not a tap.
	if (player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active()) {
		return;
	}

	lv_obj_t *target_screen = (lv_obj_t *)lv_event_get_user_data(e);

	switch_screen(target_screen);
}

// The chevron sits over the artwork when the player is open with a cover
// loaded, and the artwork backdrop is always the dark treatment, so there it
// must be light regardless of the theme. Both halves of that condition are
// tracked, because either can change while the other holds.
static void chevron_apply_color(void) {
	if (!back_btn_icon) {
		return;
	}
	if (chevron_in_player && chevron_over_cover) {
		lv_obj_set_style_image_recolor(back_btn_icon, lv_color_white(), 0);
		lv_obj_set_style_image_recolor_opa(back_btn_icon, LV_OPA_COVER, 0);
	} else {
		lv_obj_remove_local_style_prop(back_btn_icon, LV_STYLE_IMAGE_RECOLOR, 0);
		lv_obj_remove_local_style_prop(back_btn_icon, LV_STYLE_IMAGE_RECOLOR_OPA, 0);
	}
}

void back_btn_over_cover(bool over_cover) {
	chevron_over_cover = over_cover;
	chevron_apply_color();
}

// Set true by a page that wants no chevron at all: Gearboy, while a game runs.
// This is not a case for the visibility rule below -- that rule says which
// pages have a way back, and Gearboy does. It is that while playing, the only
// exit is the menu, and an arrow drawn over the frame would be a control the
// touchscreen (which belongs to the emulator then) delivers to nobody.
static bool chevron_forced_hidden;

void back_btn_force_hidden(bool hidden) {
	chevron_forced_hidden = hidden;
	back_btn_sync_visibility();
}

// The one rule for the chevron's visibility, re-applied after any animation
// completes: the open player always shows it, the main menu and the scan pages
// never do, every other page keeps it. Keeping this idempotent and running it
// last means no unlucky ordering inside a transition can leave a stray chevron
// painted on the menu, even for a single frame.
void back_btn_sync_visibility(void) {
	if (!back_btn) {
		return;
	}
	if (chevron_forced_hidden) {
		lv_obj_add_flag(back_btn, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	if (player_sheet_is_open()) {
		lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	lv_obj_t *screen = lv_screen_active();
	if (screen == main_menu_screen || screen == libraryscan_screen || screen == audiobookscan_screen) {
		lv_obj_add_flag(back_btn, LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_style_translate_x(back_btn, 0, 0);
	} else {
		lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_HIDDEN);
	}
}

// The player hides the status bar and has no title row, so its chevron sits
// right up in the corner; every other page centres it on the title beside it.
// The button is also forced visible there: the sheet can be pulled in from the
// main menu, where the chevron is normally hidden, and the player must always
// offer the way back regardless of what page it slid over.
void back_btn_player_mode(bool in_player) {
	if (!back_btn) {
		return;
	}

	if (chevron_forced_hidden) {
		lv_obj_add_flag(back_btn, LV_OBJ_FLAG_HIDDEN);
	} else if (in_player) {
		lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_t *screen = lv_screen_active();
		if (screen == main_menu_screen || screen == libraryscan_screen || screen == audiobookscan_screen) {
			lv_obj_add_flag(back_btn, LV_OBJ_FLAG_HIDDEN);
		}
	}
	chevron_in_player = in_player;
	chevron_apply_color();
	place_back_btn(!in_player);
}

// The chevron lives on the top layer, so one button serves every page.
void back_btn_init(gui_config_t *cfg) {
	back_btn_padding = cfg->padding;
	back_btn_top_bar_height = cfg->top_bar_height;

	back_btn = lv_btn_create(lv_layer_top());
	lv_obj_set_size(back_btn, BACK_BTN_SIZE, BACK_BTN_SIZE);
	// Bare chevron, no disc behind it. The button stays the full size so it is
	// still an easy target; it just does not draw itself.
	lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(back_btn, 0, 0);
	lv_obj_set_style_shadow_width(back_btn, 0, 0);
	lv_obj_set_style_pad_all(back_btn, 0, 0);
	lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);

	place_back_btn(true);

	back_btn_icon = lv_image_create(back_btn);
	lv_image_set_src(back_btn_icon, &icon_chevron_left);
	lv_obj_add_style(back_btn_icon, &theme_style_icon, 0);
	lv_obj_center(back_btn_icon);
}
