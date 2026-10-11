#ifndef SETTINGSROW_H
#define SETTINGSROW_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The row shape shared by every settings page: same height, radius and text as
// a file browser row, so a list of settings and a list of tracks read as the
// same interface.

// The page heading, sitting to the right of the floating back button and
// vertically centred on it. Shared so every page's title lines up in exactly
// the same place.
lv_obj_t *settingsrow_title(lv_obj_t *screen, gui_config_t *cfg, const char *text);

// How far down a page's content has to start to clear the heading and the
// corner buttons.
int settingsrow_content_top(gui_config_t *cfg);

// Builds the page scaffolding (title + column container) and returns the
// container the rows go into.
lv_obj_t *settingsrow_page(lv_obj_t *screen, gui_config_t *cfg, const char *title);

// Narrows a heading to clear `buttons` controls in the page's top right corner.
// The heading is built reserving room for one; a page with more has to say so,
// or a long title runs underneath them. Harmless to call with 1, and 0 gives a
// page with no corner buttons the whole width back.
void settingsrow_title_corner_slots(lv_obj_t *title, gui_config_t *cfg, int buttons);

// The heading a settingsrow_page() built, for a page whose title changes after
// it is built (Language names itself in two languages and one of them moves).
lv_obj_t *settingsrow_page_title(lv_obj_t *screen);

// Adds a row: name on the left, current value on the right. `value_out` may be
// NULL for a row that just opens something.
lv_obj_t *settingsrow_add(lv_obj_t *parent, const char *name, lv_obj_t **value_out, lv_event_cb_t cb,
						  void *user_data);

// The row's own name label, for the one row whose text is not a fixed tag: the
// AirPods row, which says which model is connected. Everything else sets its
// name once, at build time, from the language file.
lv_obj_t *settingsrow_name_label(lv_obj_t *row);

// For a row whose name is data rather than a tag (a search result, a preset):
// the name stays at the row's size and wraps to at most `lines` lines, ending
// in an ellipsis past that.
void settingsrow_name_lines(lv_obj_t *row, int lines);

// The same row without the chevron, for an action that does not open a page:
// the two library scans, which ask for confirmation instead.
lv_obj_t *settingsrow_action(lv_obj_t *parent, const char *name, lv_event_cb_t cb, void *user_data);

// Paints a row's chevron: active gives it the "on" green at full strength,
// otherwise it goes back to the quiet themed one. For rows whose page carries
// something that can be switched on (Equaliser, MSEB).
void settingsrow_chevron_active(lv_obj_t *row, bool active);

// A card with a name, its current value on the right, and a slider underneath
// that snaps to whole steps. `steps` is how many positions it has; the caller
// turns a position into a value and writes the label itself.
lv_obj_t *settingsrow_slider(lv_obj_t *parent, const char *name, int steps, lv_obj_t **value_out,
							 lv_obj_t **slider_out, lv_event_cb_t cb);

// The same card with a switch on the right instead of a value.
lv_obj_t *settingsrow_toggle(lv_obj_t *parent, const char *name, lv_obj_t **switch_out, lv_event_cb_t cb);

// A switch as every page draws it: 68 x 36 in the theme colours, with a touch
// area larger than the switch. It changes on a tap, never at the end of a drag
// or a scroll that started on it. cb, if given, gets LV_EVENT_VALUE_CHANGED
// with the new state already set, as with a plain lv_switch.
lv_obj_t *settingsrow_switch(lv_obj_t *parent, lv_event_cb_t cb);

// Both on one card: the name and its switch on the top line, a stepped slider
// (marks and all) with its value underneath. For an option whose "how long"
// only means anything while it is on.
lv_obj_t *settingsrow_toggle_slider(lv_obj_t *parent, const char *name, int steps, lv_obj_t **switch_out,
									lv_obj_t **value_out, lv_obj_t **slider_out, lv_event_cb_t toggle_cb,
									lv_event_cb_t slider_cb);

// Grows the card to show the slider, or collapses it to a plain toggle row.
// Call it whenever the switch changes (and once at startup).
void settingsrow_toggle_slider_expanded(lv_obj_t *card, bool expanded);

// ---------------------------------------------------------------------------
// Pill cards
//
// A card with a name, optionally a switch, and a row of pill buttons under it:
// how a setting that is a CHOICE is put, rather than a yes/no or a number.
// ReplayGain (off, per track, per record), the DSD output mode and the
// screensaver's source all use this shape.
// ---------------------------------------------------------------------------

// Name, switch, and a row of pills that the caller shows or hides with the
// switch. Both outputs are filled; the card is returned so a caller can hide
// the whole thing.
lv_obj_t *settingsrow_toggle_pills(lv_obj_t *parent, const char *title, lv_event_cb_t toggle_cb,
								   lv_obj_t **switch_out, lv_obj_t **pills_out);

// The same without the switch, for a choice that is always one of its options
// rather than something that can be off.
lv_obj_t *settingsrow_pills(lv_obj_t *parent, const char *title, lv_obj_t **pills_out);

// One pill. `value` reaches the callback as its user data.
lv_obj_t *settingsrow_pill(lv_obj_t *parent, const char *text, int value, lv_event_cb_t cb);

// The same pill with `text` written on it as it stands, for a label that is a
// number and a unit -- "+3 dB" is the same in every language, and putting it
// through the translation table would only invite seven tags per row that a
// translator has to copy and can get wrong.
lv_obj_t *settingsrow_pill_text(lv_obj_t *parent, const char *text, int value, lv_event_cb_t cb);

// Paints a pill as the chosen one or not. Called again after a theme change:
// the colours are hand-set, so nothing else moves them.
void settingsrow_pill_active(lv_obj_t *pill, bool active);

// ---------------------------------------------------------------------------
// A length of time, chosen on two wheels
//
// The same card as above with wheels in place of the pills: hours on the left,
// minutes on the right. For a setting whose answer is not a short list -- the
// sleep timers, where twenty minutes to fall asleep, an hour and a half for a
// record and however long is left of a journey are all ordinary answers.
//
// The look is the clock's: quiet dimmed digits, the chosen row on a soft
// rounded highlight in the accent colour, no saturated selection bar. Shared
// here rather than copied into each of the three settings pages that use it.
// ---------------------------------------------------------------------------

typedef struct {
	lv_obj_t *card;
	lv_obj_t *toggle;
	lv_obj_t *wheels; // hidden when the switch is off; the card shrinks with it
	lv_obj_t *hours;
	lv_obj_t *minutes;
} settingsrow_duration_t;

// `change_cb` fires on either wheel; read the answer with the call below rather
// than from the event.
void settingsrow_toggle_duration(lv_obj_t *parent, const char *title, lv_event_cb_t toggle_cb,
								 lv_event_cb_t change_cb, settingsrow_duration_t *out);

// Shows or hides the wheels, and matches the switch to `on`.
void settingsrow_duration_expanded(settingsrow_duration_t *d, bool on);

// The two wheels as one number of minutes, and back.
int settingsrow_duration_minutes(const settingsrow_duration_t *d);
void settingsrow_duration_set_minutes(settingsrow_duration_t *d, int minutes);

// Called again after a theme or accent change: the wheels' colours are
// hand-set, so nothing else moves them.
void settingsrow_duration_repaint(settingsrow_duration_t *d);

#endif /* SETTINGSROW_H */
