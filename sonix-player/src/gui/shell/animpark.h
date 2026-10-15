#ifndef ANIMPARK_H
#define ANIMPARK_H

#include "lvgl/lvgl.h"

// Stops the endless animations of objects nobody can see.
//
// A spinner, a title that scrolls and a text field's caret each run an
// animation that never ends, and LVGL keeps its animation timer firing every
// 16 ms for as long as any animation exists -- whether or not the object is on
// screen. One spinner on a page left behind, or the player's title scrolling
// under a closed sheet, is enough to keep the main loop awake sixty times a
// second on every other page.
//
// A watched object is parked -- its animation taken away by `park` -- once it
// is not visible (lv_obj_is_visible) or the screen is off, checked twice a
// second. It is given back by `unpark` at the start of the first refresh that
// would draw it, before anything is drawn, so it never shows parked: a
// scrolling title and a spinner carry on from where they stopped. Parking is
// not meant to be seen at all; what is not on the panel cannot tell the
// difference.
//
// The object's animation is expected to be running when it is watched. Both
// callbacks touch only the object's own animation and never delete anything.
// The entry goes away with the object.
typedef void (*animpark_cb_t)(lv_obj_t *obj);

void animpark_watch(lv_obj_t *obj, animpark_cb_t park, animpark_cb_t unpark);

// Whether `obj` is watched and parked right now.
bool animpark_is_parked(const lv_obj_t *obj);

// Gives back the animations of every parked object under `root` (included),
// visible or not, for something about to draw it off screen: the swipe back
// takes a picture of the page underneath. They are parked again at the next
// check if they are still out of sight.
void animpark_wake(lv_obj_t *root);

#endif /* ANIMPARK_H */
