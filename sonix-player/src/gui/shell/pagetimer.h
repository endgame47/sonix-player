#ifndef PAGETIMER_H
#define PAGETIMER_H

#include "lvgl/lvgl.h"

// A page's own polling timer, run only while the page is the one on screen.
//
// A page that follows something changing elsewhere -- the radio worker, the
// USB DAC, the Bluetooth receiver -- polls it on a timer, and the timer's
// callback does nothing unless its page is the active screen. The timer still
// fires, though, and keeps the main loop waking up whichever page is open,
// with the screen off as well. Bound here, it is paused for as long as neither
// of its screens is loaded and started again when one is, so its first pass
// comes straight after the page appears.
//
// `screen_b` is for a timer shared by two pages, and may be NULL. The screens
// live as long as the timer.
void pagetimer_bind(lv_timer_t *timer, lv_obj_t *screen_a, lv_obj_t *screen_b);

#endif /* PAGETIMER_H */
