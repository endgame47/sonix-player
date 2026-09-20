#ifndef GUI_H
#define GUI_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

typedef struct {
	const uint32_t screen_width;
	const uint32_t screen_height;
	const int8_t padding;
	const int8_t top_bar_height;
	const char *sd_root_path;
} gui_config_t;

// Above this the volume readout turns red -- the status bar's number and the
// pop-up's alike. Not a limit, just the point where the scale stops being
// polite: this DAC is at -21 dB there, and every step above is loud.
#define VOLUME_WARN_PERCENT 55
#define VOLUME_WARN_COLOR lv_color_make(224, 60, 50)

void gui_init(gui_config_t *cfg);

// The bridge from any thread to the UI thread: `cb(user)` runs on the UI
// thread within a few tens of milliseconds.
//
// Do not use lv_async_call() from another thread. Without LV_USE_OS (not
// enabled here) LVGL has no locks, and lv_async_call appends a timer to the
// list the UI is walking at that moment -- a real SIGSEGV on the device. This
// bridge touches nothing in LVGL on the posting side.
// False when the queue was full and the message dropped -- a caller that
// passed heap memory owns it again and has to free it.
bool gui_post(void (*cb)(void *), void *user);

// The descriptor the main loop waits on instead of sleeping on a timer: it
// becomes readable as soon as something is posted. -1 when it could not be
// opened, in which case the loop falls back to usleep().
//
// Only the UI thread may read it (post_drain_cb drains it); it exists to be
// passed to poll().
int gui_post_wake_fd(void);

// Called from the main loop when poll() reports the descriptor is ready: marks
// the queue-draining timer ready so posted work runs on the next pass instead
// of waiting for its period to elapse.
void gui_post_service(void);

void gui_notify_popup(const char *text);

// Whether the card is there and ours. False while it is exported to a computer
// and false when there is none in the slot.
bool gui_card_available(void);

// Says which of the two it is. The library cannot tell them apart -- its index
// is on the card either way -- and the wrong one of the two sends the user
// looking for a cable that is not plugged in, or for a card that is.
void gui_notify_no_card(void);

// The same three-second notice with a glyph over the sentence, recoloured to
// `color`. For the messages that are about a thing rather than about a
// failure -- the headphones, when the volume belongs to them and not to the
// player.
void gui_notify_popup_icon(const char *text, const lv_image_dsc_t *icon, lv_color_t color);

// ---------------------------------------------------------------------------
// The modal notice
//
// A card over a dimmed screen that stays up until gui_modal_hide() takes it
// down -- for the things that take longer than a toast can wait for, such as
// pairing headphones, which takes the better part of half a minute.
//
// The backdrop swallows taps on purpose: there is nothing useful to press
// while it is up, and a second pairing started on top of the first confuses
// the stack.
//
// Interface thread only.
// ---------------------------------------------------------------------------

void gui_modal_show(const lv_image_dsc_t *icon, lv_color_t color, const char *title, const char *subtitle);
void gui_modal_hide(void);
bool gui_modal_visible(void);

// A bar under the title, for the jobs whose length is known in advance.
//
// A modal with no bar says "something is happening" and nothing else, which is
// the right answer for a pairing. Copying a folder is the other kind: the
// bytes to move are counted in advance, so a real share can be shown.
//
// Call with a percentage to show the bar and move it; -1 hides it again.
// gui_modal_show() hides it by itself, so a modal that wants one asks after.
void gui_modal_progress(int percent);

// ---------------------------------------------------------------------------
// Notifications from other threads
//
// The physical buttons and a Bluetooth remote's keys are read on their own
// threads, and LVGL is not safe to touch from there. Each of these hands the
// work to the UI thread through gui_post() and returns immediately.
// ---------------------------------------------------------------------------

typedef enum {
	GUI_KEY_PLAY_PAUSE,
	GUI_KEY_NEXT,
	GUI_KEY_PREV,
} gui_key_t;

void gui_notify_key(gui_key_t key);

// New volume level, already applied to the hardware: this only updates what is
// on screen.
void gui_notify_volume(int percent);

// The power key was held down.
void gui_notify_power_menu(void);

#endif /* GUI_H */
