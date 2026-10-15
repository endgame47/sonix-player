#include "pagetimer.h"

#include <stdlib.h>

typedef struct {
	lv_timer_t *timer;
	lv_obj_t *screen_a;
	lv_obj_t *screen_b;
} pagetimer_t;

static void follow(pagetimer_t *pt) {
	lv_obj_t *active = lv_screen_active();
	if (active && (active == pt->screen_a || active == pt->screen_b)) {
		// A timer paused for longer than its period runs on the very next pass.
		lv_timer_resume(pt->timer);
	} else {
		lv_timer_pause(pt->timer);
	}
}

// SCREEN_LOADED comes before the old screen's SCREEN_UNLOADED, and by then the
// new one is already active: moving between the two screens of one timer
// keeps it running.
static void screen_cb(lv_event_t *e) { follow(lv_event_get_user_data(e)); }

void pagetimer_bind(lv_timer_t *timer, lv_obj_t *screen_a, lv_obj_t *screen_b) {
	if (!timer || !screen_a) {
		return;
	}
	pagetimer_t *pt = malloc(sizeof(*pt));
	if (!pt) {
		return; // the timer simply keeps running, as it did unbound
	}
	pt->timer = timer;
	pt->screen_a = screen_a;
	pt->screen_b = screen_b;
	lv_obj_add_event_cb(screen_a, screen_cb, LV_EVENT_SCREEN_LOADED, pt);
	lv_obj_add_event_cb(screen_a, screen_cb, LV_EVENT_SCREEN_UNLOADED, pt);
	if (screen_b) {
		lv_obj_add_event_cb(screen_b, screen_cb, LV_EVENT_SCREEN_LOADED, pt);
		lv_obj_add_event_cb(screen_b, screen_cb, LV_EVENT_SCREEN_UNLOADED, pt);
	}
	follow(pt);
}
