#ifndef SPINNER_H
#define SPINNER_H

#include "lvgl/lvgl.h"

// The spinner: an icon rotating on itself, to say work is in progress.
//
// An icon rather than lv_spinner_create(): the artwork is the same lucide set
// the rest of the interface uses, it recolours with the theme, and rotating a
// small bitmap is cheaper than redrawing an arc in full on every frame.
//
// The animation dies with the object: LVGL cancels animations targeting an
// object when it destroys it, so a spinner created inside a list that will be
// cleared needs no bookkeeping. It also stands still while it cannot be seen
// (animpark.h).
//
// `icon` is &icon_loader_small or &icon_loader_big.
lv_obj_t *spinner_create(lv_obj_t *parent, const lv_image_dsc_t *icon);

#endif /* SPINNER_H */
