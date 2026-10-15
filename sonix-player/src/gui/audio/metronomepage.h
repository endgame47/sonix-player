#ifndef METRONOMEPAGE_H
#define METRONOMEPAGE_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The metronome, from the More page: a drawn metronome whose arm swings with
// the beat, the tempo in beats per minute with its Italian name, a tap button
// that takes the tempo from the finger, and the beats in a bar.
//
// It clicks only while its page is open (metronome.h): leaving stops it. It is
// never heard over music: with something playing it says so and does not
// start, and music started while it clicks stops it. While it clicks the
// screen stays on, and the play key stops it. The tempo and the beats are kept
// in [metronome] bpm and beats.

extern lv_obj_t *metronomepage_screen;

void metronomepage_init(gui_config_t *cfg);

// A side key, before it reaches the player. True when the metronome took it:
// while it clicks, play stops it rather than starting the music, and the skip
// keys do nothing.
bool metronomepage_key(gui_key_t key);

#endif /* METRONOMEPAGE_H */
