#ifndef MUSICSETTINGS_H
#define MUSICSETTINGS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Settings that belong to the music library rather than to the device: reached
// from the icon in the corner of the Music page, opposite the back button.
extern lv_obj_t *musicsettings_screen;

void musicsettings_init(gui_config_t *cfg);

// The EQ and MSEB pages, so the control centre's quick buttons can open them
// on a long press. They are settings pages like any other -- switch_screen()
// takes them.
lv_obj_t *musicsettings_eq_screen(void);
lv_obj_t *musicsettings_mseb_screen(void);

// Switches those two on or off from outside (the control centre's quick
// buttons), keeping the pages' own switches and sliders in step.
void musicsettings_set_eq_enabled(bool enabled);
void musicsettings_set_mseb_enabled(bool enabled);

// The same for the other two the control centre exposes: the crossfade, which
// has a page of its own, and the 6 dB gain step, a plain switch on this page.
// Both are shortcuts, not second copies -- they read and write the one
// setting, and the page follows.
lv_obj_t *musicsettings_fade_screen(void);
bool musicsettings_fade_enabled(void);
void musicsettings_set_fade_enabled(bool enabled);

// Which of the two shuffles "Play in random order" starts: with this on it is
// the one that comes round again instead of stopping at the end of the deal.
// Only that action reads it; the player's mode button still reaches both.
// Whether the Music page puts Playlists on its sixth tile and Browse on the
// corner button, rather than the other way round.
bool musicsettings_playlists_first(void);

bool musicsettings_endless_shuffle(void);

bool musicsettings_high_gain(void);
void musicsettings_set_high_gain(bool enabled);

#endif /* MUSICSETTINGS_H */
