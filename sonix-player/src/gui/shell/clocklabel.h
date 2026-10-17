#ifndef CLOCKLABEL_H
#define CLOCKLABEL_H

#include "lvgl/lvgl.h"

// The time as a label of its own, with AM or PM after it in smaller letters at
// the top of the figures, the way a clock writes it, rather than as a third
// word of the same size. In the 24-hour format there is nothing after it.
//
// It is an ordinary label, with the suffix a child of it in the room its right
// padding leaves: anything done to the label -- moved, re-parented, hidden,
// tinted with a style -- the suffix follows.

// `font` for the time and `suffix_font` for AM and PM, with their sizes in
// pixels, which are what the suffix is lined up by.
lv_obj_t *clocklabel_create(lv_obj_t *parent, const lv_font_t *font, int font_px, const lv_font_t *suffix_font, int suffix_px);

// The time now, in the format the settings say; "--:--" while the clock has
// never been set.
void clocklabel_refresh(lv_obj_t *label);

#endif /* CLOCKLABEL_H */
