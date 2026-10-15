#include "animpark.h"

#include <stdlib.h>

#include "src/system/device/power.h"

// How often the watched objects are looked at for parking. Parking late costs
// nothing but a few frames of an animation nobody sees.
#define SWEEP_MS 500

typedef struct entry {
	lv_obj_t *obj;
	animpark_cb_t park;
	animpark_cb_t unpark;
	bool parked;
	// Drawn while parked, by something other than the refresh of the panel
	// (refr_start_cb sees those first): taken back at the next sweep.
	bool drawn;
	struct entry *next;
} entry_t;

static entry_t *entries;
// Runs while anything is unparked, or drawn while parked; paused otherwise.
static lv_timer_t *sweep_timer;

static void sweep_start(void) { lv_timer_resume(sweep_timer); }

static void park(entry_t *e) {
	e->parked = true;
	e->park(e->obj);
}

static void unpark(entry_t *e) {
	e->parked = false;
	e->unpark(e->obj);
	sweep_start();
}

static void sweep_cb(lv_timer_t *t) {
	bool screen_on = power_screen_is_on();
	bool live = false;
	for (entry_t *e = entries; e; e = e->next) {
		if (e->parked) {
			if (e->drawn && screen_on) {
				unpark(e);
			}
			e->drawn = false;
		} else if (!screen_on || !lv_obj_is_visible(e->obj)) {
			park(e);
		}
		if (!e->parked) {
			live = true;
		}
	}
	if (!live) {
		lv_timer_pause(t);
	}
}

// Whether `obj` sits on one of the screens and layers the display draws.
static bool on_shown_root(lv_obj_t *obj, lv_display_t *disp) {
	lv_obj_t *scr = lv_obj_get_screen(obj);
	return scr == lv_display_get_screen_active(disp) || scr == lv_display_get_screen_prev(disp) || scr == lv_display_get_layer_top(disp) || scr == lv_display_get_layer_sys(disp) || scr == lv_display_get_layer_bottom(disp);
}

// A parked object that comes into sight is given back here, at the start of
// the refresh that is about to draw it: given back once drawn, the frame would
// already show it parked -- a scrolling title back at its start, a centred one
// cut at both ends -- for one frame.
static void refr_start_cb(lv_event_t *ev) {
	lv_display_t *disp = lv_event_get_target(ev);
	if (!power_screen_is_on()) {
		return;
	}
	bool any = false;
	for (entry_t *e = entries; e && !any; e = e->next) {
		any = e->parked && lv_obj_get_display(e->obj) == disp && on_shown_root(e->obj, disp);
	}
	if (!any) {
		return;
	}

	// LVGL lays these out right after this event, before it draws. Done here
	// first, so that what is checked is where things will be drawn: a page
	// just loaded, or a panel just dragged, has not been laid out yet.
	lv_obj_t *roots[] = {lv_display_get_screen_active(disp), lv_display_get_screen_prev(disp), lv_display_get_layer_bottom(disp), lv_display_get_layer_top(disp), lv_display_get_layer_sys(disp)};
	for (unsigned i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
		if (roots[i]) {
			lv_obj_update_layout(roots[i]);
		}
	}

	for (entry_t *e = entries; e; e = e->next) {
		if (e->parked && lv_obj_get_display(e->obj) == disp && lv_obj_is_visible(e->obj)) {
			unpark(e);
		}
	}
}

static void draw_cb(lv_event_t *ev) {
	entry_t *e = lv_event_get_user_data(ev);
	if (e->parked && !e->drawn) {
		e->drawn = true;
		lv_timer_ready(sweep_timer);
		sweep_start();
	}
}

static void delete_cb(lv_event_t *ev) {
	entry_t *gone = lv_event_get_user_data(ev);
	for (entry_t **p = &entries; *p; p = &(*p)->next) {
		if (*p == gone) {
			*p = gone->next;
			break;
		}
	}
	free(gone);
}

static entry_t *find(const lv_obj_t *obj) {
	for (entry_t *e = entries; e; e = e->next) {
		if (e->obj == obj) {
			return e;
		}
	}
	return NULL;
}

void animpark_watch(lv_obj_t *obj, animpark_cb_t park_cb, animpark_cb_t unpark_cb) {
	if (!obj || !park_cb || !unpark_cb || find(obj)) {
		return;
	}
	entry_t *e = calloc(1, sizeof(*e));
	if (!e) {
		return; // the animation simply keeps running, as it did unwatched
	}
	if (!sweep_timer) {
		sweep_timer = lv_timer_create(sweep_cb, SWEEP_MS, NULL);
		lv_display_add_event_cb(lv_obj_get_display(obj), refr_start_cb, LV_EVENT_REFR_START, NULL);
	}
	e->obj = obj;
	e->park = park_cb;
	e->unpark = unpark_cb;
	e->next = entries;
	entries = e;
	lv_obj_add_event_cb(obj, draw_cb, LV_EVENT_DRAW_MAIN_BEGIN, e);
	lv_obj_add_event_cb(obj, delete_cb, LV_EVENT_DELETE, e);
	sweep_start();
}

bool animpark_is_parked(const lv_obj_t *obj) {
	entry_t *e = obj ? find(obj) : NULL;
	return e && e->parked;
}

void animpark_wake(lv_obj_t *root) {
	if (!root) {
		return;
	}
	for (entry_t *e = entries; e; e = e->next) {
		if (!e->parked) {
			continue;
		}
		for (lv_obj_t *o = e->obj; o; o = lv_obj_get_parent(o)) {
			if (o == root) {
				unpark(e);
				break;
			}
		}
	}
}
