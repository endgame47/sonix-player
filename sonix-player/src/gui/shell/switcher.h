#ifndef GUI_UTILS_H
#define GUI_UTILS_H

#include "lvgl/lvgl.h"
#include "src/gui/shell/gui.h"

void switch_screen(lv_obj_t *target_screen);
void screen_history_reset(void); // forget the back-stack (back goes to the menu)
void switch_screen_no_history(lv_obj_t *target_screen); // load without recording

// Attach the iOS-style interactive swipe-back to a page surface (a screen or
// a scrollable list/container that swallows presses): the page follows the
// finger rightward, showing the previous page underneath.
void switcher_attach_back_gesture(lv_obj_t *screen);

// True during (and for the tap at the end of) a swipe-back drag. Click
// handlers check it, so the release of a drag doesn't also activate the row
// or button it happens to land on -- the same contract as
// player_sheet_drag_active().
bool switcher_back_drag_active(void);
void back_btn_cb(lv_event_t *e);
void switch_screen_cb(lv_event_t *e);
void back_btn_init(gui_config_t *cfg);

// Lets one page take the back chevron for itself. While `screen` is the active
// one, `handler` runs before the chevron does anything: returning true means it
// dealt with the press (put up a question, say) and the page stays; returning
// false lets the chevron leave as usual.
//
// Set once at page construction, one per screen. Registering the same screen
// again replaces its guard.
void switcher_set_back_guard(lv_obj_t *screen, bool (*handler)(void));

// The guard's preview: answers "would going back stay on this page?" without
// changing anything. The swipe-back uses it to choose what to draw underneath
// the dragged page.
void switcher_set_back_guard_peek(lv_obj_t *screen, bool (*peek)(void));

// True while the player sheet is open: parks the chevron higher, in the
// artwork's corner, instead of on the (hidden) title row.
void back_btn_player_mode(bool in_player);

// True while the open player shows a loaded cover: the chevron sits on the
// always-dark artwork backdrop, so it goes white regardless of the theme.
void back_btn_over_cover(bool over_cover);

// Horizontal offset for the chevron, so it can ride whatever is being
// dragged (the player sheet's slide uses it; the page swipe-back does its
// own). 0 puts it back in place.
void back_btn_translate(int32_t x);

// Where the chevron's middle sits while the player is open, from the top of the
// screen. For a page that wants to put something of its own on the same line as
// the way back -- the player's Studio arrangement puts the ellipsis there.
int back_btn_centre_y(void);

// Re-asserts the chevron's visibility against the page actually on screen:
// hidden on the main menu and the scan pages, shown everywhere else. Cheap and
// idempotent, and called from every animation-completion path, since otherwise
// an unlucky ordering can paint the chevron on the main menu for a frame.
void back_btn_sync_visibility(void);

// Removes the chevron entirely until it is put back. For a page where touch
// does not belong to LVGL -- Gearboy while a game runs: the arrow would only be
// drawn, since the screen belongs to the emulator at that moment.
void back_btn_force_hidden(bool hidden);

// Suspends the drag-to-go-back gesture. For a page that owns the whole panel
// and needs horizontal touch for itself: a Lua app running full screen, where
// a sideways drag is the app's input, not a request to leave.
void back_gesture_blocked(bool blocked);

// Marks `screen` as having been opened from the player's own menu: backing
// out of it reopens the player instead of just popping the history. Cleared
// automatically once consumed, on any jump to the main menu, and by
// screen_history_reset(); pass NULL to clear by hand.
void switcher_set_player_return(lv_obj_t *screen);

#endif
