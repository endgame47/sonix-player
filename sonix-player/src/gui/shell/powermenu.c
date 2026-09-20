#include "powermenu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/gui.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/system/device/clock.h"
#include "src/system/playback/device_state.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/remote/dlna.h"
#include "src/system/streaming/radio.h"

// ---------------------------------------------------------------------------
// The power menu, iOS-style: two slide-to-confirm pills over a near-opaque
// black veil, and a round cancel button at the bottom that turns red under the
// finger. Sliding is the point: powering off a music player in a pocket must
// take a deliberate gesture, not a stray tap.
// ---------------------------------------------------------------------------

#define SLIDE_WIDTH 380
#define SLIDE_HEIGHT 76
#define SLIDE_KNOB 64
// How far along (as a fraction of the track) the knob has to be on release.
#define SLIDE_COMMIT_PCT 88

// Horizontal inset of the knob's travel: LVGL runs the knob's CENTRE from
// edge to edge, which hangs half of it outside the pill at both ends. Padding
// the track by half a knob (plus a hair of margin) keeps it inside.
#define SLIDE_INSET (SLIDE_KNOB / 2 + 6)

#define CANCEL_SIZE 76

static lv_obj_t *panel;

typedef struct {
	lv_obj_t *pill; // the visible track
	lv_obj_t *slider;
	lv_obj_t *knob_icon;
	int knob_icon_w; // real bitmap width: the two action icons are 56 px
	lv_obj_t *label;
	void (*action)(void);
} slide_t;

static slide_t slide_off;
static slide_t slide_reboot;

bool powermenu_is_open(void) { return panel && !lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN); }

// ---------------------------------------------------------------------------
// opening and closing
// ---------------------------------------------------------------------------

static void slide_reset(slide_t *s);

void powermenu_show(void) {
	if (!panel) {
		return;
	}

	slide_reset(&slide_off);
	slide_reset(&slide_reboot);

	lv_obj_remove_flag(panel, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(panel);
	power_hold_screen_on(true);
}

static void hide(void) {
	if (panel) {
		lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
	}
	power_hold_screen_on(false);
	power_notify_activity();
}

static void async_show_cb(void *unused) {
	(void)unused;
	powermenu_show();
}

void powermenu_request_show(void) { gui_post(async_show_cb, NULL); }

// Both actions flush first: the settings file and the thumbnail cache are
// written lazily, and a DAP that loses them on every shutdown would be worse
// than one that takes an extra moment to stop.
static void do_power_off(void) {
	printf("power: shutting down\n");

	// Where the music had got to, before anything else stops. The player's own
	// poll writes this only every ten seconds while playing and on a change of
	// state, so without this flush a power-off loses up to ten seconds -- and a
	// position seeked to while paused, which changes no state at all, would
	// never be written.
	device_state_remember_flush();

	// The time goes into the RTC before anything else, exactly as the stock
	// player does on its way out: whatever the clock has learned since it was
	// last set is otherwise lost the moment the power goes.
	clock_shutdown();

	// The radio database is on the card: close it so its journal is tidied
	// away before the power goes, rather than left for the next boot to find.
	radio_store_close();

	// Streamed and downloaded tracks are transient and must not survive a
	// power cycle: without this the hidden folders carry a gigabyte of files
	// the user never put there and will not listen to again.
	qobuzcache_clear_on_exit();
	tidalcache_clear_on_exit();
	podcastcache_clear_on_exit();
	dlna_clear_on_exit();

	// Dark the panel *first*: `poweroff` goes through init's shutdown hooks,
	// which it must -- the raw syscall with USB attached leaves the PMIC to
	// boot the device straight back up -- and those take a few seconds. With
	// the screen already off the wait is invisible; without it the menu sits
	// frozen on-screen until init gets around to cutting power.
	power_screen_off();

	// The charger back on before the power goes: the driver keeps that bit
	// across a shutdown, and a device put away at its charge limit would meet
	// the next cable with a charger that does nothing.
	power_charging_release();

	sync();

	int rc = system("poweroff");
	(void)rc;
	sleep(8);

	// Last resort if init never got there.
	reboot(RB_POWER_OFF);
}

static void do_reboot(void) {
	printf("power: rebooting\n");
	device_state_remember_flush();
	clock_shutdown();
	radio_store_close();
	qobuzcache_clear_on_exit();
	tidalcache_clear_on_exit();
	podcastcache_clear_on_exit();
	dlna_clear_on_exit();
	power_screen_off(); // same instant-feedback trick as the shutdown
	sync();

	// Through init, the same way down as the shutdown above. `reboot(RB_AUTOBOOT)`
	// restarts the machine from inside this process: init's shutdown hooks never
	// run, so the daemons are not stopped and nothing is remounted read-only or
	// unmounted -- and the player does not unmount the card itself either, so on
	// that route nobody does.
	//
	// The syscall stays as the last resort, on the same eight-second guard, for
	// a firmware whose init never gets there.
	int rc = system("reboot");
	(void)rc;
	sleep(8);
	reboot(RB_AUTOBOOT);
	execl("/bin/sh", "sh", "-c", "reboot", (char *)NULL);
}

static void cancel_cb(lv_event_t *e) {
	(void)e;
	hide();
}

// ---------------------------------------------------------------------------
// the slide-to-confirm pills
// ---------------------------------------------------------------------------

// Keeps the knob icon riding on the knob: the slider's knob is drawn by LVGL
// at a position derived from the value, so the icon just mirrors that maths.
static void slide_track_knob(slide_t *s) {
	int value = lv_slider_get_value(s->slider);

	// The slider is SLIDE_HEIGHT narrower than the pill and centred in it, so
	// the knob's centre -- which LVGL runs from slider edge to slider edge --
	// travels [SLIDE_HEIGHT/2, SLIDE_WIDTH - SLIDE_HEIGHT/2] in pill
	// coordinates: always fully inside. The icon mirrors that in the pill.
	int travel = SLIDE_WIDTH - SLIDE_HEIGHT;
	int knob_center = SLIDE_HEIGHT / 2 + (travel * value) / 100;
	lv_obj_align(s->knob_icon, LV_ALIGN_LEFT_MID, knob_center - s->knob_icon_w / 2, 0);

	// The hint fades as the knob approaches, exactly like the real thing.
	lv_obj_set_style_opa(s->label, (lv_opa_t)(LV_OPA_COVER - (LV_OPA_COVER * value) / 100), 0);
}

static void slide_reset(slide_t *s) {
	lv_slider_set_value(s->slider, 0, LV_ANIM_OFF);
	slide_track_knob(s);
}

static void slide_event_cb(lv_event_t *e) {
	slide_t *s = lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);

	if (code == LV_EVENT_VALUE_CHANGED) {
		slide_track_knob(s);
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		if (lv_slider_get_value(s->slider) >= SLIDE_COMMIT_PCT) {
			s->action();
		} else {
			slide_reset(s); // not far enough: snap home
		}
	}
}

// One pill: a rounded container carrying the hint text, with a transparent
// slider inside it that is half a knob narrower on each side -- LVGL runs the
// knob's centre from slider edge to slider edge, so insetting the slider is
// what keeps the white disc inside the pill at both extremes.
static void make_slide(lv_obj_t *parent, slide_t *s, const lv_image_dsc_t *icon, lv_color_t icon_tint,
					   const char *text, void (*action)(void)) {
	s->action = action;

	// The visible pill: translucent white over the blur, like iOS.
	s->pill = lv_obj_create(parent);
	lv_obj_set_size(s->pill, SLIDE_WIDTH, SLIDE_HEIGHT);
	lv_obj_set_style_radius(s->pill, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(s->pill, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(s->pill, LV_OPA_20, 0);
	lv_obj_set_style_border_width(s->pill, 0, 0);
	lv_obj_set_style_pad_all(s->pill, 0, 0);
	lv_obj_remove_flag(s->pill, LV_OBJ_FLAG_SCROLLABLE);

	// The instruction, centred on the pill; it fades as the knob advances.
	s->label = lv_label_create(s->pill);
	lv_label_set_text(s->label, tr(text));
	lv_obj_set_style_text_font(s->label, &font_ui_24, 0);
	lv_obj_set_style_text_color(s->label, lv_color_white(), 0);
	lv_obj_center(s->label);

	// The working part: invisible track, white disc knob.
	s->slider = lv_slider_create(s->pill);
	lv_obj_set_size(s->slider, SLIDE_WIDTH - SLIDE_HEIGHT, SLIDE_HEIGHT);
	lv_obj_center(s->slider);
	lv_slider_set_range(s->slider, 0, 100);
	lv_slider_set_value(s->slider, 0, LV_ANIM_OFF);
	// Zero padding, or the theme's default insets shift the knob's travel and
	// the icon riding it ends up off the disc's centre.
	lv_obj_set_style_pad_all(s->slider, 0, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(s->slider, LV_OPA_TRANSP, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(s->slider, LV_OPA_TRANSP, LV_PART_INDICATOR);

	lv_obj_set_style_radius(s->slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
	lv_obj_set_style_bg_color(s->slider, lv_color_white(), LV_PART_KNOB);
	lv_obj_set_style_bg_opa(s->slider, LV_OPA_COVER, LV_PART_KNOB);
	lv_obj_set_style_pad_all(s->slider, (SLIDE_KNOB - SLIDE_HEIGHT) / 2, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(s->slider, 10, LV_PART_KNOB);
	lv_obj_set_style_shadow_opa(s->slider, LV_OPA_40, LV_PART_KNOB);
	lv_obj_set_style_shadow_color(s->slider, lv_color_black(), LV_PART_KNOB);

	// Drag only, never tap. A plain LVGL slider jumps its value to wherever the
	// finger presses, so one tap near the far end of the track would pass the
	// commit threshold -- exactly the careless gesture the pill exists to
	// prevent. With ADV_HITTEST the slider only answers a press that begins on
	// the knob, so the value can only be dragged to the end. A tap on the rest
	// of the pill does nothing.
	lv_obj_add_flag(s->slider, LV_OBJ_FLAG_ADV_HITTEST);
	// The knob stays easy to grab: the extended area applies to its hit-test,
	// not to the track.
	lv_obj_set_ext_click_area(s->slider, SLIDE_HEIGHT / 2);

	lv_obj_add_event_cb(s->slider, slide_event_cb, LV_EVENT_VALUE_CHANGED, s);
	lv_obj_add_event_cb(s->slider, slide_event_cb, LV_EVENT_RELEASED, s);
	lv_obj_add_event_cb(s->slider, slide_event_cb, LV_EVENT_PRESS_LOST, s);

	// The icon rides the knob, drawn in the pill's coordinate space (the knob
	// itself cannot hold children).
	s->knob_icon = lv_image_create(s->pill);
	lv_image_set_src(s->knob_icon, icon);
	// A touch smaller than the raw 56 px bitmap, so the disc keeps a clear
	// rim around the glyph. Scaling happens about the centre, so the
	// centring maths below is unaffected.
	lv_image_set_scale(s->knob_icon, 200); // 200/256 = ~44 px
	s->knob_icon_w = (int)icon->header.w;
	lv_obj_set_style_image_recolor(s->knob_icon, icon_tint, 0);
	lv_obj_set_style_image_recolor_opa(s->knob_icon, LV_OPA_COVER, 0);
	lv_obj_add_flag(s->knob_icon, LV_OBJ_FLAG_IGNORE_LAYOUT);

	slide_track_knob(s);
}

void powermenu_init(gui_config_t *cfg) {
	panel = lv_obj_create(lv_layer_top());
	lv_obj_set_size(panel, cfg->screen_width, cfg->screen_height);
	lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 0);
	// Nearly-opaque black: the page underneath is still just visible, but the
	// menu clearly owns the screen.
	lv_obj_set_style_bg_color(panel, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(panel, LV_OPA_80, 0);
	lv_obj_set_style_border_width(panel, 0, 0);
	lv_obj_set_style_radius(panel, 0, 0);
	lv_obj_set_style_pad_all(panel, 0, 0);
	lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

	// The two pills, upper third of the screen, like the real thing.
	lv_obj_t *pills = lv_obj_create(panel);
	lv_obj_set_size(pills, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_align(pills, LV_ALIGN_TOP_MID, 0, cfg->top_bar_height + 70);
	lv_obj_set_style_bg_opa(pills, 0, 0);
	lv_obj_set_style_border_width(pills, 0, 0);
	lv_obj_set_style_pad_all(pills, 0, 0);
	lv_obj_set_style_pad_gap(pills, 26, 0);
	lv_obj_remove_flag(pills, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(pills, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	make_slide(pills, &slide_off, &icon_power_off, lv_color_make(224, 27, 36), "powermenu_slide_to_power_off", do_power_off);
	// A fixed blue, deliberately not the accent: the two slides are power off
	// (red) and reboot (blue), and an accent set to red or orange would make
	// them impossible to tell apart at a glance.
	make_slide(pills, &slide_reboot, &icon_reboot, lv_color_make(0, 122, 255), "powermenu_slide_to_restart", do_reboot);

	// Cancel: a round button at the bottom centre that goes red under the
	// finger, with its label floating underneath.
	lv_obj_t *cancel = lv_btn_create(panel);
	lv_obj_set_size(cancel, CANCEL_SIZE, CANCEL_SIZE);
	lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, -84);
	lv_obj_set_style_radius(cancel, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(cancel, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(cancel, LV_OPA_30, 0);
	lv_obj_set_style_bg_color(cancel, lv_color_make(224, 27, 36), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_style_border_width(cancel, 0, 0);
	lv_obj_set_style_shadow_width(cancel, 0, 0);
	lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *cancel_icon = lv_image_create(cancel);
	lv_image_set_src(cancel_icon, &icon_close);
	lv_obj_set_style_image_recolor(cancel_icon, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(cancel_icon, LV_OPA_COVER, 0);
	lv_obj_center(cancel_icon);

	lv_obj_t *cancel_label = lv_label_create(panel);
	lv_label_set_text(cancel_label, tr("cancel"));
	lv_obj_set_style_text_font(cancel_label, &font_ui_24, 0);
	lv_obj_set_style_text_color(cancel_label, lv_color_white(), 0);
	lv_obj_align(cancel_label, LV_ALIGN_BOTTOM_MID, 0, -44);

}
