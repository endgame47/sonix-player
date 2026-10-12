#ifndef TOPBAR_H
#define TOPBAR_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

void topbar_init(gui_config_t *cfg);

// Repaints the clock. Called by the timer, and by the time setter the moment
// the user confirms a new time.
void topbar_refresh_clock(void);

// Repaints the volume readout. Called by the poll timer and, immediately, by
// whatever just changed the level.
void topbar_refresh_volume(int percent);

// Repaints the play/pause indicator. The battery poll only comes round every
// five seconds, so the player calls this the moment the transport state
// changes rather than letting the glyph lag behind the button.
void topbar_refresh_playback(void);

// Repaints the two radio glyphs, left of the battery: bluetooth first, then
// wifi, then the charge percentage. Each appears only while its radio is on,
// and is drawn dimmed while that radio is up but connected to nothing -- the
// same reading as iOS's, and cheaper than a second colour. Called on its own
// timer, and straight away by the pages that switch a radio.
void topbar_refresh_radios(void);

// Detect changes, right of play/pause: the search glyph while the card is being
// looked over, the check glyph for a few seconds once the run is over, then
// nothing. IDLE takes the glyph away at once.
typedef enum {
	TOPBAR_LIBRARY_IDLE,
	TOPBAR_LIBRARY_CHECKING,
	TOPBAR_LIBRARY_CHECKED,
} topbar_library_t;
void topbar_set_library_check(topbar_library_t state);

// The bar in `bg` with its glyphs and type in `ink` while `screen` is the page
// shown, for a page painted in a colour of its own. Pass false with the same
// screen to give the bar back its theme colours; a call naming another screen
// than the one recorded changes nothing.
void topbar_tint_on_screen(lv_obj_t *screen, bool on, lv_color_t bg, lv_color_t ink);

// Which page is being shown, so the tint above follows it. Called as each page
// is loaded.
void topbar_page_shown(lv_obj_t *screen);

// Shows or hides the status bar. Pages that want the full screen height (the
// player, with its artwork flush to the top) hide it on entry.
void topbar_set_hidden(bool hidden);
bool topbar_is_hidden(void);

// Draws the bar above everything else on the top layer. The control centre
// asks for this on the way in: its sheet covers the whole screen, and the
// clock, battery and volume have to stay readable over it.
void topbar_bring_to_front(void);

// Shows or hides the charge percentage next to the battery shell (config
// "screen"/"battery_percent", on by default). The shell itself always stays --
// it is what says how much is left at a glance.
void topbar_set_battery_percent(bool shown);

// Where the clock sits in the bar, or that it is not shown at all (config
// "screen"/"clock_pos").
#define TOPBAR_CLOCK_LEFT 0
#define TOPBAR_CLOCK_CENTER 1
#define TOPBAR_CLOCK_RIGHT 2
#define TOPBAR_CLOCK_HIDDEN 3
void topbar_set_clock_position(int pos);

#endif // TOPBAR_H
