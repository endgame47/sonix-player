#include "musicsettings.h"

#include <stdint.h>
#include <stdio.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/libraryscan.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/audio/eqsettings.h"
#include "src/gui/audio/msebsettings.h"
#include "src/gui/audio/peqpage.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/library/medialist.h"
#include "src/gui/library/music.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/playback/sleeptimer.h"
#include "src/system/audio/audio.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/audio/eq.h"
#include "src/system/playback/device_state.h"
#include "src/system/decode/decode.h"
#include "src/system/audio/replaygain.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"

lv_obj_t *musicsettings_screen;

// ---------------------------------------------------------------------------
// Filters: the CS43198's four digital filters, the same set the stock player
// exposes (ALSA "Digital Filter" 0..3, decompile FUN_004803e0).
// ---------------------------------------------------------------------------

static lv_obj_t *dacfilter_screen;
static lv_obj_t *filter_checks[4];

static const char *const FILTER_NAMES[4] = {
	"musicsettings_fast_roll_off_low_latency",
	"musicsettings_fast_roll_off_phase_compensated",
	"musicsettings_slow_roll_off_low_latency",
	"musicsettings_slow_roll_off_phase_compensated",
};

static void refresh_filter_checks(void) {
	int active = (int)config_get_int("audio", "dac_filter", 0);
	for (int i = 0; i < 4; i++) {
		if (filter_checks[i]) {
			lv_label_set_text(filter_checks[i], i == active ? "\xE2\x9C\x93" : "");
			lv_obj_set_style_text_color(filter_checks[i], theme()->accent, 0);
		}
	}
}

static void filter_row_cb(lv_event_t *e) {
	int filter = (int)(intptr_t)lv_event_get_user_data(e);

	config_set_int("audio", "dac_filter", filter);
	config_save();
	set_dac_filter(filter);
	refresh_filter_checks();
}

static void build_filter_page(gui_config_t *cfg) {
	dacfilter_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(dacfilter_screen, cfg, "musicsettings_filters");

	for (int i = 0; i < 4; i++) {
		settingsrow_add(container, FILTER_NAMES[i], &filter_checks[i], filter_row_cb, (void *)(intptr_t)i);
		lv_obj_set_style_text_font(filter_checks[i], &font_ui_26, 0);
	}

	// An explanatory note: DAC filters mean nothing to most listeners.
	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr("musicsettings_filter_note"));

	refresh_filter_checks();
	switcher_attach_back_gesture(dacfilter_screen);
}

// Gapless: local music only. A stream (radio, Qobuz while downloading) has no
// "next track" ready to hand, and the emulator has a PCM of its own. The
// switch tells the audio engine how to behave at track boundaries; anything
// that does not go through them is unaffected.
static lv_obj_t *gapless_switch;

static void gapless_toggle_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(gapless_switch, LV_STATE_CHECKED);
	config_set_int("audio", "gapless", on ? 1 : 0);
	config_save();
	audio_set_gapless(on);
}

// ---------------------------------------------------------------------------
// DRE: the DAC's Dynamic Range Enhancement, the ALSA "DRE_EN" switch the
// stock player writes (decompile FUN_00482120), applied at boot and on the
// toggle.
// ---------------------------------------------------------------------------

#ifdef BOARD_R1
	// R1 does not have the DRE_EN option in amixer
#else
	static lv_obj_t *dre_switch;

	static void dre_toggle_cb(lv_event_t *e) {
		(void)e;
		bool on = lv_obj_has_state(dre_switch, LV_STATE_CHECKED);
		config_set_int("audio", "dac_dre", on ? 1 : 0);
		config_save();
		set_dac_dre(on ? 1 : 0);
	}
#endif

#ifdef BOARD_R1
	// R1 does not have the NOS_EN option in amixer
#else
	// NOS: the DAC's non-oversampling mode, ALSA "NOS_EN" like the stock player
	// (decompile FUN_004820e0).
	static lv_obj_t *nos_switch;

	static void nos_toggle_cb(lv_event_t *e) {
		(void)e;
		bool on = lv_obj_has_state(nos_switch, LV_STATE_CHECKED);
		config_set_int("audio", "dac_nos", on ? 1 : 0);
		config_save();
		set_dac_nos(on ? 1 : 0);
	}
#endif

// High gain: the 6 dB gain step, done as the stock player does it -- a shift
// of the whole volume scale on the DAC's attenuation register.
static lv_obj_t *gain_switch;
static lv_obj_t *album_chain_switch;

static void gain_toggle_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(gain_switch, LV_STATE_CHECKED);
	config_set_int("audio", "high_gain", on ? 1 : 0);
	config_save();
	set_high_gain(on ? 1 : 0);
}

// ---------------------------------------------------------------------------
// MSEB: the stock player's MageSound tuning (see eq.c for the engine and the
// mapping); nine character sliders, -10..+10 around flat.
// ---------------------------------------------------------------------------

static lv_obj_t *mseb_screen;
static lv_obj_t *mseb_enable_switch;
static lv_obj_t *mseb_sliders[MSEB_BANDS];
static lv_obj_t *mseb_values[MSEB_BANDS];

static lv_obj_t *mseb_reset_btn;
static lv_obj_t *eq_reset_btn;

// A reset that would reset nothing is not offered: the button greys out with
// the page's own switch.
static void reset_button_enabled(lv_obj_t *btn, bool enabled) {
	if (!btn) {
		return;
	}
	if (enabled) {
		lv_obj_remove_state(btn, LV_STATE_DISABLED);
		lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_style_opa(btn, LV_OPA_COVER, 0);
	} else {
		lv_obj_add_state(btn, LV_STATE_DISABLED);
		lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_style_opa(btn, LV_OPA_40, 0);
	}
}

static void mseb_apply_sliders_enabled(bool enabled) {
	for (int i = 0; i < MSEB_BANDS; i++) {
		if (!mseb_sliders[i]) {
			continue;
		}
		if (enabled) {
			lv_obj_remove_state(mseb_sliders[i], LV_STATE_DISABLED);
		} else {
			lv_obj_add_state(mseb_sliders[i], LV_STATE_DISABLED);
		}
	}
}

static void mseb_enable_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(mseb_enable_switch, LV_STATE_CHECKED);
	mseb_set_enabled(on);
	mseb_apply_sliders_enabled(on);
	reset_button_enabled(mseb_reset_btn, on);
}

static void mseb_slider_cb(lv_event_t *e) {
	int band = (int)(intptr_t)lv_event_get_user_data(e);
	if (band < 0 || band >= MSEB_BANDS || !mseb_sliders[band]) {
		return;
	}
	int value = (int)lv_slider_get_value(mseb_sliders[band]);
	mseb_set_value(band, value);
	lv_label_set_text_fmt(mseb_values[band], "%+d", value);
}

static void mseb_released_cb(lv_event_t *e) {
	(void)e;
	mseb_save(); // one config write when the finger lifts, not per pixel
}

static void refresh_mseb_sliders(void) {
	for (int i = 0; i < MSEB_BANDS; i++) {
		theme_apply_slider_knob(mseb_sliders[i]);
		if (mseb_sliders[i]) {
			lv_obj_set_style_bg_color(mseb_sliders[i], theme()->accent, LV_PART_INDICATOR);
			lv_obj_set_style_bg_color(mseb_sliders[i], theme()->text_secondary, LV_PART_MAIN);
		}
	}
}

// ---------------------------------------------------------------------------
// Fade: the volume ramp at each end of a track (see audio.c).
// ---------------------------------------------------------------------------

static lv_obj_t *fade_screen;
static lv_obj_t *fade_switch;
static lv_obj_t *fade_card, *fade_value, *fade_slider;

// One second per step, one to ten.
#define FADE_MIN_SECONDS 1
#define FADE_MAX_SECONDS 10
#define FADE_STEPS (FADE_MAX_SECONDS - FADE_MIN_SECONDS + 1)

static int fade_seconds_setting(void) {
	int v = (int)config_get_int("audio", "fade_seconds", 3);
	if (v < FADE_MIN_SECONDS) {
		v = FADE_MIN_SECONDS;
	}
	if (v > FADE_MAX_SECONDS) {
		v = FADE_MAX_SECONDS;
	}
	return v;
}

static void fade_apply_setting(void) {
	audio_set_fade(config_get_bool("audio", "fade", false), fade_seconds_setting());
}

static void fade_refresh_labels(void) {
	lv_label_set_text_fmt(fade_value, "%d s", fade_seconds_setting());
	bool on = config_get_bool("audio", "fade", false);
	if (on) {
		lv_obj_add_state(fade_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(fade_switch, LV_STATE_CHECKED);
	}
	settingsrow_toggle_slider_expanded(fade_card, on);
}

static void fade_toggle_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("audio", "fade", lv_obj_has_state(fade_switch, LV_STATE_CHECKED));
	config_save();
	fade_apply_setting();
	fade_refresh_labels();
}

static void fade_slider_cb(lv_event_t *e) {
	(void)e;
	int seconds = FADE_MIN_SECONDS + (int)lv_slider_get_value(fade_slider);
	config_set_int("audio", "fade_seconds", seconds);
	config_save();
	fade_apply_setting();
	lv_label_set_text_fmt(fade_value, "%d s", seconds);
}

static void build_fade_page(gui_config_t *cfg) {
	fade_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(fade_screen, cfg, "fade");

	fade_card = settingsrow_toggle_slider(container, "fade", FADE_STEPS, &fade_switch, &fade_value,
										  &fade_slider, fade_toggle_cb, fade_slider_cb);
	lv_slider_set_value(fade_slider, fade_seconds_setting() - FADE_MIN_SECONDS, LV_ANIM_OFF);

	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr("musicsettings_fade_note"));

	fade_refresh_labels();
	switcher_attach_back_gesture(fade_screen);
}

// ---------------------------------------------------------------------------
// Soundfield: the stock player's Sound Field module (labelled "Campo sonoro"
// in its Italian strings), a stereo width control. The engine lives in eq.c;
// this is its page.
// ---------------------------------------------------------------------------

static lv_obj_t *soundfield_screen;
static lv_obj_t *soundfield_switch;
static lv_obj_t *soundfield_card, *soundfield_value, *soundfield_slider;

// 0.00 to 2.00 in steps of 0.05: exactly the range the original's own plugin
// declares for its "width" parameter.
#define SOUNDFIELD_STEPS ((SOUNDFIELD_WIDTH_MAX - SOUNDFIELD_WIDTH_MIN) / SOUNDFIELD_WIDTH_STEP + 1)

static void soundfield_set_value_label(int hundredths) {
	// Printed by hand rather than with %.2f: the whole module is integer
	// arithmetic and there is no reason to drag a float formatter in for two
	// decimal places.
	lv_label_set_text_fmt(soundfield_value, "%d.%02d", hundredths / 100, hundredths % 100);
}

static void soundfield_refresh(void) {
	bool on = soundfield_get_enabled();
	soundfield_set_value_label(soundfield_get_width());
	if (on) {
		lv_obj_add_state(soundfield_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(soundfield_switch, LV_STATE_CHECKED);
	}
	settingsrow_toggle_slider_expanded(soundfield_card, on);
}

static void soundfield_toggle_cb(lv_event_t *e) {
	(void)e;
	soundfield_set_enabled(lv_obj_has_state(soundfield_switch, LV_STATE_CHECKED));
	soundfield_refresh();
}

static void soundfield_slider_cb(lv_event_t *e) {
	(void)e;
	int hundredths = SOUNDFIELD_WIDTH_MIN + (int)lv_slider_get_value(soundfield_slider) * SOUNDFIELD_WIDTH_STEP;
	soundfield_set_width(hundredths);
	soundfield_set_value_label(soundfield_get_width());
}

static void build_soundfield_page(gui_config_t *cfg) {
	soundfield_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(soundfield_screen, cfg, "musicsettings_soundfield");

	soundfield_card = settingsrow_toggle_slider(container, "musicsettings_soundfield", SOUNDFIELD_STEPS, &soundfield_switch,
											   &soundfield_value, &soundfield_slider, soundfield_toggle_cb,
											   soundfield_slider_cb);
	lv_slider_set_value(soundfield_slider,
						(soundfield_get_width() - SOUNDFIELD_WIDTH_MIN) / SOUNDFIELD_WIDTH_STEP, LV_ANIM_OFF);

	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr("musicsettings_soundstage_note"));

	soundfield_refresh();
	switcher_attach_back_gesture(soundfield_screen);
}

// ---------------------------------------------------------------------------
// Crossfeed -- the engine is in eq.c, this is its page.
//
// Three numbers and a switch. The switch and the amount share the expanding
// card (like soundfield), the other two sit in two cards below: turning
// crossfeed on without touching anything else already gives a sensible tuning,
// and the detail is further down for whoever wants it.
// ---------------------------------------------------------------------------

static lv_obj_t *corner_button(lv_obj_t *screen, gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph,
							   lv_event_cb_t cb, void *user);

static lv_obj_t *crossfeed_screen;
static lv_obj_t *crossfeed_switch;
static lv_obj_t *crossfeed_card, *crossfeed_value, *crossfeed_slider;
static lv_obj_t *crossfeed_cut_value, *crossfeed_cut_slider;
static lv_obj_t *crossfeed_delay_value, *crossfeed_delay_slider;
static lv_obj_t *crossfeed_detail_card, *crossfeed_delay_card;

#define CROSSFEED_LEVEL_STEPS ((CROSSFEED_LEVEL_MAX - CROSSFEED_LEVEL_MIN) / CROSSFEED_LEVEL_STEP + 1)
#define CROSSFEED_CUTOFF_STEPS ((CROSSFEED_CUTOFF_MAX - CROSSFEED_CUTOFF_MIN) / CROSSFEED_CUTOFF_STEP + 1)
#define CROSSFEED_DELAY_STEPS ((CROSSFEED_DELAY_MAX - CROSSFEED_DELAY_MIN) / CROSSFEED_DELAY_STEP + 1)

static void crossfeed_refresh(void) {
	bool on = crossfeed_get_enabled();
	lv_label_set_text_fmt(crossfeed_value, "%d%%", crossfeed_get_level());
	lv_label_set_text_fmt(crossfeed_cut_value, "%d Hz", crossfeed_get_cutoff());
	lv_label_set_text_fmt(crossfeed_delay_value, "%d \xC2\xB5s", crossfeed_get_delay());
	if (on) {
		lv_obj_add_state(crossfeed_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(crossfeed_switch, LV_STATE_CHECKED);
	}
	settingsrow_toggle_slider_expanded(crossfeed_card, on);

	// The two detail cards disappear while crossfeed is off: they are numbers
	// that affect nothing, and leaving them there to be dragged is worse than
	// hiding them.
	if (on) {
		lv_obj_remove_flag(crossfeed_detail_card, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(crossfeed_delay_card, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(crossfeed_detail_card, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(crossfeed_delay_card, LV_OBJ_FLAG_HIDDEN);
	}
}

static void crossfeed_toggle_cb(lv_event_t *e) {
	(void)e;
	crossfeed_set_enabled(lv_obj_has_state(crossfeed_switch, LV_STATE_CHECKED));
	crossfeed_refresh();
}

static void crossfeed_level_cb(lv_event_t *e) {
	(void)e;
	crossfeed_set_level(CROSSFEED_LEVEL_MIN + (int)lv_slider_get_value(crossfeed_slider) * CROSSFEED_LEVEL_STEP);
	lv_label_set_text_fmt(crossfeed_value, "%d%%", crossfeed_get_level());
}

static void crossfeed_cutoff_cb(lv_event_t *e) {
	(void)e;
	crossfeed_set_cutoff(CROSSFEED_CUTOFF_MIN + (int)lv_slider_get_value(crossfeed_cut_slider) * CROSSFEED_CUTOFF_STEP);
	lv_label_set_text_fmt(crossfeed_cut_value, "%d Hz", crossfeed_get_cutoff());
}

static void crossfeed_delay_cb(lv_event_t *e) {
	(void)e;
	crossfeed_set_delay(CROSSFEED_DELAY_MIN + (int)lv_slider_get_value(crossfeed_delay_slider) * CROSSFEED_DELAY_STEP);
	lv_label_set_text_fmt(crossfeed_delay_value, "%d \xC2\xB5s", crossfeed_get_delay());
}

static void crossfeed_reset_cb(lv_event_t *e) {
	(void)e;
	crossfeed_set_level(CROSSFEED_LEVEL_DEFAULT);
	crossfeed_set_cutoff(CROSSFEED_CUTOFF_DEFAULT);
	crossfeed_set_delay(CROSSFEED_DELAY_DEFAULT);
	lv_slider_set_value(crossfeed_slider, (CROSSFEED_LEVEL_DEFAULT - CROSSFEED_LEVEL_MIN) / CROSSFEED_LEVEL_STEP,
						LV_ANIM_ON);
	lv_slider_set_value(crossfeed_cut_slider, (CROSSFEED_CUTOFF_DEFAULT - CROSSFEED_CUTOFF_MIN) / CROSSFEED_CUTOFF_STEP,
						LV_ANIM_ON);
	lv_slider_set_value(crossfeed_delay_slider, (CROSSFEED_DELAY_DEFAULT - CROSSFEED_DELAY_MIN) / CROSSFEED_DELAY_STEP,
						LV_ANIM_ON);
	crossfeed_refresh();
}

static void build_crossfeed_page(gui_config_t *cfg) {
	crossfeed_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(crossfeed_screen, cfg, "musicsettings_crossfeed");

	settingsrow_title_corner_slots(settingsrow_page_title(crossfeed_screen), cfg, 1);
	corner_button(crossfeed_screen, cfg, 0, &icon_reset, crossfeed_reset_cb, NULL);

	crossfeed_card =
		settingsrow_toggle_slider(container, "musicsettings_amount", CROSSFEED_LEVEL_STEPS, &crossfeed_switch,
								  &crossfeed_value, &crossfeed_slider, crossfeed_toggle_cb, crossfeed_level_cb);
	lv_slider_set_value(crossfeed_slider, (crossfeed_get_level() - CROSSFEED_LEVEL_MIN) / CROSSFEED_LEVEL_STEP,
						LV_ANIM_OFF);

	crossfeed_detail_card = settingsrow_slider(container, "musicsettings_cutoff_frequency", CROSSFEED_CUTOFF_STEPS,
											   &crossfeed_cut_value, &crossfeed_cut_slider, crossfeed_cutoff_cb);
	lv_slider_set_value(crossfeed_cut_slider, (crossfeed_get_cutoff() - CROSSFEED_CUTOFF_MIN) / CROSSFEED_CUTOFF_STEP,
						LV_ANIM_OFF);

	crossfeed_delay_card = settingsrow_slider(container, "musicsettings_delay", CROSSFEED_DELAY_STEPS, &crossfeed_delay_value,
											  &crossfeed_delay_slider, crossfeed_delay_cb);
	lv_slider_set_value(crossfeed_delay_slider, (crossfeed_get_delay() - CROSSFEED_DELAY_MIN) / CROSSFEED_DELAY_STEP,
						LV_ANIM_OFF);

	crossfeed_refresh();
	switcher_attach_back_gesture(crossfeed_screen);
}

// ---------------------------------------------------------------------------
// Channel balance
//
// The stock player's own module and its own numbers -- its parameter panel is
// declared in the binary as a slider from -20 to +20 dB in half-decibel steps
// with an Enable checkbox beside it, which is exactly this page. See eq.h for
// why it only ever attenuates.
// ---------------------------------------------------------------------------

static lv_obj_t *balance_screen;
static lv_obj_t *balance_switch;
static lv_obj_t *balance_card, *balance_value, *balance_slider;

#define BALANCE_STEPS ((BALANCE_MAX - BALANCE_MIN) / BALANCE_STEP + 1)

static void balance_set_value_label(int tenths) {
	// Printed by hand for the same reason the soundfield's is: the module is
	// integers all the way down and there is no float formatter to drag in.
	int magnitude = tenths < 0 ? -tenths : tenths;
	const char *side = tenths == 0 ? tr("centre") : (tenths < 0 ? tr("left") : tr("right"));
	if (tenths == 0) {
		lv_label_set_text(balance_value, side);
	} else {
		lv_label_set_text_fmt(balance_value, "%s %d.%d dB", side, magnitude / 10, magnitude % 10);
	}
}

static void balance_refresh(void) {
	bool on = balance_get_enabled();
	balance_set_value_label(balance_get_value());
	if (on) {
		lv_obj_add_state(balance_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(balance_switch, LV_STATE_CHECKED);
	}
	settingsrow_toggle_slider_expanded(balance_card, on);
}

static void balance_toggle_cb(lv_event_t *e) {
	(void)e;
	balance_set_enabled(lv_obj_has_state(balance_switch, LV_STATE_CHECKED));
	balance_refresh();
}

static void balance_slider_cb(lv_event_t *e) {
	(void)e;
	int tenths = BALANCE_MIN + (int)lv_slider_get_value(balance_slider) * BALANCE_STEP;
	balance_set_value(tenths);
	balance_set_value_label(balance_get_value());
}

static void build_balance_page(gui_config_t *cfg) {
	balance_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(balance_screen, cfg, "musicsettings_channel_balance");

	balance_card = settingsrow_toggle_slider(container, "musicsettings_balance", BALANCE_STEPS, &balance_switch,
											 &balance_value, &balance_slider, balance_toggle_cb, balance_slider_cb);
	lv_slider_set_value(balance_slider, (balance_get_value() - BALANCE_MIN) / BALANCE_STEP, LV_ANIM_OFF);

	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr("musicsettings_balance_note"));

	balance_refresh();
	switcher_attach_back_gesture(balance_screen);
}

// ---------------------------------------------------------------------------
// Replay gain
// ---------------------------------------------------------------------------

static lv_obj_t *rg_switch;
static lv_obj_t *rg_card, *rg_pills;
static lv_obj_t *rg_track_pill, *rg_album_pill;

static void rg_refresh(void) {
	if (!rg_switch) {
		return;
	}
	replaygain_mode_t mode = replaygain_mode();
	bool on = mode != REPLAYGAIN_OFF;

	if (on) {
		lv_obj_add_state(rg_switch, LV_STATE_CHECKED);
		lv_obj_remove_flag(rg_pills, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_remove_state(rg_switch, LV_STATE_CHECKED);
		lv_obj_add_flag(rg_pills, LV_OBJ_FLAG_HIDDEN);
	}
	settingsrow_pill_active(rg_track_pill, mode == REPLAYGAIN_TRACK);
	settingsrow_pill_active(rg_album_pill, mode == REPLAYGAIN_ALBUM);
}

// Whatever the setting becomes, the track already loaded has to be re-measured
// against it -- otherwise switching this on does nothing until the next song.
static void rg_apply_to_loaded_track(void) {
	device_state_t state;
	device_state_get(&state);
	replaygain_load(&state.metadata);
}

static void rg_toggle_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(rg_switch, LV_STATE_CHECKED);
	// Switching it back on returns to whichever of the two modes was last
	// chosen, rather than always to track gain.
	replaygain_set_mode(on ? (replaygain_mode_t)config_get_int("audio", "replaygain_last", REPLAYGAIN_TRACK)
						   : REPLAYGAIN_OFF);
	rg_apply_to_loaded_track();
	rg_refresh();
}

static void rg_pick_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	replaygain_mode_t mode = (replaygain_mode_t)(intptr_t)lv_event_get_user_data(e);
	replaygain_set_mode(mode);
	config_set_int("audio", "replaygain_last", (long)mode);
	config_save();
	rg_apply_to_loaded_track();
	rg_refresh();
}

// ---------------------------------------------------------------------------
// DSD gain compensation
//
// What the number means is in alsa-controls.h. Here it is one switch and the
// six compensations: off is 0 dB, so there is no 0 dB pill -- it would be a
// second way to say what the switch already says, and a state where the switch
// reads on and nothing is being compensated.
// ---------------------------------------------------------------------------

// The pills are +1 dB upwards; pill i carries index i + 1.
static lv_obj_t *dsd_gain_switch;
static lv_obj_t *dsd_gain_pills;
static lv_obj_t *dsd_gain_pill[DSD_GAIN_MAX_DB];

static void dsd_gain_refresh(void) {
	if (!dsd_gain_switch) {
		return;
	}
	int index = alsa_dsd_gain_index();
	bool on = index > 0;

	if (on) {
		lv_obj_add_state(dsd_gain_switch, LV_STATE_CHECKED);
		lv_obj_remove_flag(dsd_gain_pills, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_remove_state(dsd_gain_switch, LV_STATE_CHECKED);
		lv_obj_add_flag(dsd_gain_pills, LV_OBJ_FLAG_HIDDEN);
	}
	for (int i = 0; i < DSD_GAIN_MAX_DB; i++) {
		settingsrow_pill_active(dsd_gain_pill[i], i + 1 == index);
	}
}

static void dsd_gain_store(int index) {
	alsa_set_dsd_gain_index(index);
	config_set_int("audio", "dsd_gain", index);
	if (index > 0) {
		config_set_int("audio", "dsd_gain_last", index);
	}
	config_save();
	dsd_gain_refresh();
}

static void dsd_gain_toggle_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(dsd_gain_switch, LV_STATE_CHECKED);
	// Switching it on goes back to the last compensation chosen. The first time,
	// that is the whole 6 dB: this exists because DSD comes out quieter than the
	// stock player, and the largest step is the one that answers it.
	dsd_gain_store(on ? (int)config_get_int("audio", "dsd_gain_last", DSD_GAIN_MAX_DB) : 0);
}

static void dsd_gain_pick_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	dsd_gain_store((int)(intptr_t)lv_event_get_user_data(e));
}

// The round corner buttons on the MSEB and equalizer pages, the same shape the
// music page uses for its gear and its search.
static lv_obj_t *corner_button(lv_obj_t *screen, gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph,
							   lv_event_cb_t cb, void *user) {
	lv_obj_t *button = lv_btn_create(screen);
	lv_obj_set_size(button, 56, 56);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * (56 + 6), cfg->padding + cfg->top_bar_height);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user);

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);

	return button;
}

// --- the two "back to flat" buttons ---------------------------------------

static void refresh_mseb_sliders_values(void);

static void mseb_reset_action(void *user) {
	(void)user;
	mseb_reset();
	refresh_mseb_sliders_values();
}

static void mseb_reset_cb(lv_event_t *e) {
	(void)e;
	confirm_show("musicsettings_reset_mseb", "musicsettings_mseb_reset_confirm_note", "reset", mseb_reset_action, NULL);
}

static void refresh_eq_sliders_values(void);

static void eq_reset_action(void *user) {
	(void)user;
	eq_reset();
	refresh_eq_sliders_values();
}

static void eq_reset_cb(lv_event_t *e) {
	(void)e;
	confirm_show("musicsettings_reset_the_equaliser", "musicsettings_eq_reset_confirm_note", "reset", eq_reset_action, NULL);
}

static void mseb_open_settings_cb(lv_event_t *e) {
	(void)e;
	switch_screen(msebsettings_screen());
}

static void eq_open_settings_cb(lv_event_t *e) {
	(void)e;
	switch_screen(eqsettings_screen());
}

// Puts every MSEB slider where the engine's values are -- after a reset, a
// preset, or a change of range (whose ends have just moved).
static void refresh_mseb_sliders_values(void) {
	int range = mseb_get_range();
	for (int i = 0; i < MSEB_BANDS; i++) {
		if (!mseb_sliders[i]) {
			continue;
		}
		lv_slider_set_range(mseb_sliders[i], -range, range);
		lv_slider_set_value(mseb_sliders[i], mseb_get_value(i), LV_ANIM_OFF);
		lv_label_set_text_fmt(mseb_values[i], "%+d", mseb_get_value(i));
	}
}

static void build_mseb_page(gui_config_t *cfg) {
	mseb_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(mseb_screen, cfg, "mseb");

	settingsrow_toggle(container, "on", &mseb_enable_switch, mseb_enable_cb);
	if (mseb_get_enabled()) {
		lv_obj_add_state(mseb_enable_switch, LV_STATE_CHECKED);
	}

	for (int i = 0; i < MSEB_BANDS; i++) {
		lv_obj_t *card = lv_obj_create(container);
		lv_obj_set_width(card, lv_pct(100));
		lv_obj_set_height(card, 104);
		lv_obj_add_style(card, &theme_style_card, 0);
		lv_obj_set_style_radius(card, 12, 0);
		lv_obj_set_style_border_width(card, 0, 0);
		lv_obj_set_style_shadow_width(card, 0, 0);
		lv_obj_set_style_pad_hor(card, 20, 0);
		lv_obj_set_style_pad_ver(card, 12, 0);
		lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);

		lv_obj_t *name = lv_label_create(card);
		lv_label_set_text(name, tr(mseb_band_name[i]));
		lv_obj_add_style(name, &theme_style_text, 0);
		lv_obj_set_style_text_font(name, &font_ui_22, 0);
		lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);

		mseb_values[i] = lv_label_create(card);
		lv_label_set_text_fmt(mseb_values[i], "%+d", mseb_get_value(i));
		lv_obj_add_style(mseb_values[i], &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(mseb_values[i], &font_ui_22, 0);
		lv_obj_align(mseb_values[i], LV_ALIGN_TOP_RIGHT, 0, 0);

		lv_obj_t *slider = lv_slider_create(card);
		lv_obj_set_width(slider, lv_pct(100));
		lv_obj_set_height(slider, 10);
		lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -10);
		lv_slider_set_range(slider, -mseb_get_range(), mseb_get_range());
		// MSEB sliders are bipolar: zero sits in the middle and the coloured
		// bar grows from there, forwards for positives and backwards for
		// negatives. Without symmetrical mode LVGL fills from the left, so "-3"
		// would read as "almost nothing" rather than "a step below".
		lv_slider_set_mode(slider, LV_SLIDER_MODE_SYMMETRICAL);
		lv_slider_set_value(slider, mseb_get_value(i), LV_ANIM_OFF);

		lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
		lv_obj_set_style_bg_color(slider, theme()->text_secondary, LV_PART_MAIN);
		lv_obj_set_style_bg_opa(slider, LV_OPA_40, LV_PART_MAIN);
		lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
		lv_obj_set_style_bg_color(slider, theme()->accent, LV_PART_INDICATOR);
		lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
		lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
		theme_apply_slider_knob(slider);
		lv_obj_set_style_pad_all(slider, 7, LV_PART_KNOB);
		lv_obj_set_style_shadow_width(slider, 6, LV_PART_KNOB);
		lv_obj_set_style_shadow_opa(slider, LV_OPA_30, LV_PART_KNOB);
		lv_obj_set_style_shadow_color(slider, lv_color_black(), LV_PART_KNOB);
		// Drag the knob, do not tap the track. A plain LVGL slider jumps its
		// value to wherever the finger lands, so brushing a row on the way past
		// would move a band. With ADV_HITTEST the slider only answers a press
		// that begins on the knob; anywhere else on the track the press goes to
		// the card behind it and scrolls the page.
		lv_obj_add_flag(slider, LV_OBJ_FLAG_ADV_HITTEST);
		// The knob is 24 px across, and with ADV_HITTEST the extended click area
		// belongs to the knob alone rather than padding the whole track.
		lv_obj_set_ext_click_area(slider, 18);
		lv_obj_add_event_cb(slider, mseb_slider_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
		lv_obj_add_event_cb(slider, mseb_released_cb, LV_EVENT_RELEASED, NULL);

		mseb_sliders[i] = slider;
	}

	for (int i = 0; i < MSEB_BANDS; i++) {
		lv_obj_set_style_opa(mseb_sliders[i], LV_OPA_40, LV_STATE_DISABLED);
	}
	mseb_apply_sliders_enabled(mseb_get_enabled());

	// The gear and the reset, in the corner: the same pair of buttons the music
	// page wears.
	corner_button(mseb_screen, cfg, 0, &icon_music_settings, mseb_open_settings_cb, NULL);
	mseb_reset_btn = corner_button(mseb_screen, cfg, 1, &icon_reset, mseb_reset_cb, NULL);
	reset_button_enabled(mseb_reset_btn, mseb_get_enabled());
	msebsettings_set_reload_cb(refresh_mseb_sliders_values);

	switcher_attach_back_gesture(mseb_screen);
	theme_register_refresh(refresh_mseb_sliders);
}

// Remember track / remember volume: what survives a power cycle.
static lv_obj_t *remember_track_switch;
static lv_obj_t *remember_volume_switch;

static void remember_track_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(remember_track_switch, LV_STATE_CHECKED);
	config_set_int("player", "remember_track", on ? 1 : 0);
	config_save();
}

static void remember_volume_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(remember_volume_switch, LV_STATE_CHECKED);
	config_set_int("player", "remember_volume", on ? 1 : 0);
	if (on) {
		// Every output's own level, each into its own slot, as in the status bar
		// poll: what is remembered is where each socket was left, not the one
		// number the player happens to be at.
		//
		// The profiles' levels and not the one the hardware is at: in line out
		// the output sits at the fixed index, which is a mode and not a level
		// the user chose to come back to.
		volume_profile_persist_now();
	}
	config_save();
}

// ---------------------------------------------------------------------------
// Equaliser: the 10-band graphic EQ (src/system/eq.c), one vertical slider per
// octave band, +/-12 dB.
// ---------------------------------------------------------------------------

static lv_obj_t *eq_screen;
static lv_obj_t *eq_enable_switch;
static lv_obj_t *eq_sliders[EQ_BANDS];

// Puts every equalizer slider where the engine's gains are, after a reset.
static void refresh_eq_sliders_values(void) {
	for (int i = 0; i < EQ_BANDS; i++) {
		if (eq_sliders[i]) {
			lv_slider_set_value(eq_sliders[i], eq_get_band(i), LV_ANIM_OFF);
		}
	}
}

static void eq_apply_sliders_enabled(bool enabled) {
	for (int i = 0; i < EQ_BANDS; i++) {
		if (!eq_sliders[i]) {
			continue;
		}
		if (enabled) {
			lv_obj_remove_state(eq_sliders[i], LV_STATE_DISABLED);
		} else {
			lv_obj_add_state(eq_sliders[i], LV_STATE_DISABLED);
		}
	}
}

static void eq_enable_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(eq_enable_switch, LV_STATE_CHECKED);
	eq_set_enabled(on);
	eq_apply_sliders_enabled(on);
	reset_button_enabled(eq_reset_btn, on);
}

static void eq_slider_cb(lv_event_t *e) {
	int band = (int)(intptr_t)lv_event_get_user_data(e);
	if (band >= 0 && band < EQ_BANDS && eq_sliders[band]) {
		eq_set_band(band, (int)lv_slider_get_value(eq_sliders[band]));
	}
}

// "31" ... "16k": the label under each slider.
static void eq_freq_text(int freq, char *out, size_t size) {
	freq &= 0xffff; // the table tops out at 16000; quiets -Wformat-truncation
	if (freq >= 1000) {
		snprintf(out, size, "%dk", freq / 1000);
	} else {
		snprintf(out, size, "%d", freq);
	}
}

// Re-paint the sliders' fills after a theme/accent change.
static void refresh_eq_sliders(void) {
	for (int i = 0; i < EQ_BANDS; i++) {
		theme_apply_slider_knob(eq_sliders[i]);
		if (eq_sliders[i]) {
			lv_obj_set_style_bg_color(eq_sliders[i], theme()->accent, LV_PART_INDICATOR);
			lv_obj_set_style_bg_color(eq_sliders[i], theme()->text_secondary, LV_PART_MAIN);
		}
	}
}

static void build_eq_page(gui_config_t *cfg) {
	eq_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(eq_screen, cfg, "equaliser");

	settingsrow_toggle(container, "on", &eq_enable_switch, eq_enable_cb);
	if (eq_get_enabled()) {
		lv_obj_add_state(eq_enable_switch, LV_STATE_CHECKED);
	}

	// The band card: ten vertical sliders side by side, frequency underneath.
	lv_obj_t *card = lv_obj_create(container);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, 360);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 14, 0);
	lv_obj_set_style_pad_gap(card, 0, 0);
	lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	for (int i = 0; i < EQ_BANDS; i++) {
		lv_obj_t *column = lv_obj_create(card);
		lv_obj_set_size(column, 38, lv_pct(100));
		lv_obj_set_style_bg_opa(column, 0, 0);
		lv_obj_set_style_border_width(column, 0, 0);
		lv_obj_set_style_pad_all(column, 0, 0);
		lv_obj_set_style_pad_gap(column, 8, 0);
		lv_obj_remove_flag(column, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

		lv_obj_t *slider = lv_slider_create(column);
		lv_obj_set_size(slider, 6, lv_pct(80));
		lv_slider_set_range(slider, EQ_GAIN_MIN_DB, EQ_GAIN_MAX_DB);
		lv_slider_set_value(slider, eq_get_band(i), LV_ANIM_OFF);

		lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
		lv_obj_set_style_bg_color(slider, theme()->text_secondary, LV_PART_MAIN);
		lv_obj_set_style_bg_opa(slider, LV_OPA_40, LV_PART_MAIN);
		lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
		lv_obj_set_style_bg_color(slider, theme()->accent, LV_PART_INDICATOR);
		lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
		lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
		theme_apply_slider_knob(slider);
		lv_obj_set_style_pad_all(slider, 7, LV_PART_KNOB);
		lv_obj_set_style_shadow_width(slider, 6, LV_PART_KNOB);
		lv_obj_set_style_shadow_opa(slider, LV_OPA_30, LV_PART_KNOB);
		lv_obj_set_style_shadow_color(slider, lv_color_black(), LV_PART_KNOB);
		// A 6 px trough is no touch target; each column's full width is.
		lv_obj_set_ext_click_area(slider, 16);
		lv_obj_add_event_cb(slider, eq_slider_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);

		eq_sliders[i] = slider;

		char text[8];
		eq_freq_text(eq_band_freq[i], text, sizeof(text));
		lv_obj_t *label = lv_label_create(column);
		lv_label_set_text(label, text);
		lv_obj_add_style(label, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(label, &font_ui_18, 0);
	}

	// A dead slider looks dead: half opacity while the EQ is off.
	for (int i = 0; i < EQ_BANDS; i++) {
		lv_obj_set_style_opa(eq_sliders[i], LV_OPA_40, LV_STATE_DISABLED);
	}
	eq_apply_sliders_enabled(eq_get_enabled());

	// The gear and the reset, in the corner, like the MSEB page.
	corner_button(eq_screen, cfg, 0, &icon_music_settings, eq_open_settings_cb, NULL);
	eq_reset_btn = corner_button(eq_screen, cfg, 1, &icon_reset, eq_reset_cb, NULL);
	reset_button_enabled(eq_reset_btn, eq_get_enabled());
	eqsettings_set_reload_cb(refresh_eq_sliders_values);

	// What the scale means, in one dim line under the sliders.
	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_label_set_text(note, tr("musicsettings_eq_note"));

	switcher_attach_back_gesture(eq_screen);
	theme_register_refresh(refresh_eq_sliders);
}

// ---------------------------------------------------------------------------
// What the control centre's quick buttons reach in here: the two pages, and
// the two switches, so a toggle from outside leaves this page consistent.
// ---------------------------------------------------------------------------

lv_obj_t *musicsettings_eq_screen(void) { return eq_screen; }
lv_obj_t *musicsettings_mseb_screen(void) { return mseb_screen; }

lv_obj_t *musicsettings_fade_screen(void) { return fade_screen; }

bool musicsettings_fade_enabled(void) { return config_get_bool("audio", "fade", false); }

bool musicsettings_endless_shuffle(void) { return config_get_bool("player", "endless_shuffle", false); }

// The setters below are also called by the control centre, which can sit open
// on top of this page, so the chevrons have to be repainted from here.
static void refresh_active_chevrons(void);

void musicsettings_set_fade_enabled(bool enabled) {
	config_set_bool("audio", "fade", enabled);
	config_save();
	fade_apply_setting();
	if (fade_switch) {
		fade_refresh_labels(); // the page's own switch and its slider follow
	}
	refresh_active_chevrons();
}

bool musicsettings_high_gain(void) { return config_get_int("audio", "high_gain", 0) != 0; }

void musicsettings_set_high_gain(bool enabled) {
	config_set_int("audio", "high_gain", enabled ? 1 : 0);
	config_save();
	set_high_gain(enabled ? 1 : 0);
	if (gain_switch) {
		if (enabled) {
			lv_obj_add_state(gain_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(gain_switch, LV_STATE_CHECKED);
		}
	}
}

void musicsettings_set_eq_enabled(bool enabled) {
	eq_set_enabled(enabled);
	if (eq_enable_switch) {
		if (enabled) {
			lv_obj_add_state(eq_enable_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(eq_enable_switch, LV_STATE_CHECKED);
		}
	}
	eq_apply_sliders_enabled(enabled);
	refresh_active_chevrons();
}

void musicsettings_set_mseb_enabled(bool enabled) {
	mseb_set_enabled(enabled);
	if (mseb_enable_switch) {
		if (enabled) {
			lv_obj_add_state(mseb_enable_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(mseb_enable_switch, LV_STATE_CHECKED);
		}
	}
	mseb_apply_sliders_enabled(enabled);
	refresh_active_chevrons();
}

// The rows whose chevron says whether what lies behind it is on. Every row
// that can be switched off, not a selection of them: a green chevron on MSEB
// and a grey one on an enabled fade is not a nuance, it is a lie.
static lv_obj_t *eq_row, *peq_row, *mseb_row, *soundfield_row, *crossfeed_row, *fade_row, *balance_row;

static void refresh_active_chevrons(void) {
	settingsrow_chevron_active(eq_row, eq_get_enabled());
	settingsrow_chevron_active(peq_row, peq_get_enabled());
	settingsrow_chevron_active(mseb_row, mseb_get_enabled());
	settingsrow_chevron_active(soundfield_row, soundfield_get_enabled());
	settingsrow_chevron_active(crossfeed_row, crossfeed_get_enabled());
	settingsrow_chevron_active(fade_row, musicsettings_fade_enabled());
	settingsrow_chevron_active(balance_row, balance_get_enabled());
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_filter_checks();
	// Back from either page: the chevron follows what was left switched on.
	refresh_active_chevrons();
}

// Rescanning the card takes minutes and cannot be undone halfway, so the row
// asks first. It opens no page of its own until the answer is yes, which is
// why it carries no chevron.
static void start_library_scan(void *user) {
	(void)user;
	switch_screen(libraryscan_screen);
}

static void scan_row_cb(lv_event_t *e) {
	(void)e;
	confirm_show("musicsettings_scan_the_music_library",
				 "musicsettings_rescan_confirm_note",
				 "scan", start_library_scan, NULL);
}

static void album_chain_cb(lv_event_t *e) {
	device_state_set_album_chaining(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

// ---------------------------------------------------------------------------
// Playback options
//
// What playback does around a track rather than to it: which track comes next
// when the queue runs out, what survives the power switch, and whether the
// silence between two tracks is real. A page of its own rather than toggles on
// the music page among the DAC's own settings: nothing here touches the sound.
// ---------------------------------------------------------------------------

static lv_obj_t *playback_screen;
static lv_obj_t *folder_chain_switch;
static lv_obj_t *scan_screen;
static lv_obj_t *keep_articles_switch;
static lv_obj_t *display_screen;
static lv_obj_t *album_view_switch;
static lv_obj_t *quality_badges_switch;

static void folder_chain_cb(lv_event_t *e) {
	device_state_set_folder_chaining(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

// The dim paragraph under a toggle that needs one.
static void option_note(lv_obj_t *parent, const char *text) {
	lv_obj_t *note = lv_label_create(parent);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr(text));
}

// ---------------------------------------------------------------------------
// Scan options
//
// The scan itself, and the one setting that only means anything to a scan: how
// a name is filed. It is a page of its own because the answer to "why did
// nothing change?" lives here -- the article switch is written into the index
// while it is being built, so it is read at the same moment the scan is
// started, in the same place.
// ---------------------------------------------------------------------------

static void keep_articles_cb(lv_event_t *e) {
	bool keep = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	config_set_int("player", "skip_articles", keep ? 0 : 1);
	config_save();
	// Straight away, so a scan started from the row above it uses the answer
	// the switch is showing.
	library_set_skip_articles(!keep);
}

static void build_scan_page(gui_config_t *cfg) {
	scan_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(scan_screen, cfg, "musicsettings_scan_options");
	settingsrow_title_corner_slots(settingsrow_page_title(scan_screen), cfg, 0);

	settingsrow_action(container, "musicsettings_scan_music_library", scan_row_cb, NULL);

	settingsrow_toggle(container, "musicsettings_do_not_ignore_articles", &keep_articles_switch, keep_articles_cb);
	if (!library_skip_articles()) {
		lv_obj_add_state(keep_articles_switch, LV_STATE_CHECKED);
	}

	switcher_attach_back_gesture(scan_screen);
}

// ---------------------------------------------------------------------------
// Display options
//
// What the lists show, as opposed to what playback does with them. A page of
// its own rather than rows on the music page: it is the third such question --
// how a name is filed, what happens after a track, what a list shows -- and the
// other two already have one.
// ---------------------------------------------------------------------------

static void album_view_cb(lv_event_t *e) {
	medialist_set_album_view(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

// Which of Browse and Playlists the Music page puts on its sixth tile. Kept
// here with the rest of the display options; the Music page reads it and paints
// itself.
static lv_obj_t *playlists_first_switch;

bool musicsettings_playlists_first(void) { return config_get_bool("music", "playlists_first", false); }

static void playlists_first_cb(lv_event_t *e) {
	config_set_bool("music", "playlists_first", lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
	config_save();
	music_refresh_layout();
}

static void quality_badges_cb(lv_event_t *e) {
	medialist_set_quality_badges(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static lv_obj_t *coverflow_switch;

static void coverflow_cb(lv_event_t *e) {
	coverflow_set_enabled(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

// ---------------------------------------------------------------------------
// Which of the two arrangements the now-playing page uses
//
// Here and not under Appearance, where everything applies to the whole
// interface: this applies to the music on the card and to nothing else -- not
// to a radio station, not to a stream from a phone, not to an audiobook.
//
// The page owns the setting; this stores it and asks the page to decide again.
// ---------------------------------------------------------------------------

static lv_obj_t *layout_standard_pill;
static lv_obj_t *layout_alternative_pill;
static lv_obj_t *layout_studio_pill;

static void layout_refresh(void) {
	if (!layout_standard_pill) {
		return;
	}
	player_layout_t chosen = player_layout_get();
	settingsrow_pill_active(layout_standard_pill, chosen == PLAYER_LAYOUT_STANDARD);
	settingsrow_pill_active(layout_alternative_pill, chosen == PLAYER_LAYOUT_ALTERNATIVE);
	settingsrow_pill_active(layout_studio_pill, chosen == PLAYER_LAYOUT_STUDIO);
}

static void layout_pick_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	player_layout_set((player_layout_t)(intptr_t)lv_event_get_user_data(e));
	layout_refresh();
}

static void build_display_page(gui_config_t *cfg) {
	display_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(display_screen, cfg, "musicsettings_display_options");
	settingsrow_title_corner_slots(settingsrow_page_title(display_screen), cfg, 0);

	// On: an artist is a list of their records. Off: a flat list of their
	// tracks, with the grouping button in the corner.
	settingsrow_toggle(container, "musicsettings_album_view", &album_view_switch, album_view_cb);
	option_note(container, "musicsettings_artist_opens_albums_note");
	if (medialist_album_view()) {
		lv_obj_add_state(album_view_switch, LV_STATE_CHECKED);
	}

	// A small mark under a track's title saying what the file is. It comes from
	// what the scan wrote down, so a library indexed by an older build wears no
	// badges until it is scanned again.
	settingsrow_toggle(container, "audio_quality", &quality_badges_switch, quality_badges_cb);
	option_note(container, "musicsettings_shows_the_tracks_audio_quality_wi");
	if (medialist_quality_badges()) {
		lv_obj_add_state(quality_badges_switch, LV_STATE_CHECKED);
	}

	// The album carousel. Off by default: it is a second way into the records,
	// not a replacement for the list, and the covers it draws are decoded at a
	// size the lists never ask for -- so switching it on fills the thumbnail
	// database with rather bigger pictures the first time it is flicked through.
	settingsrow_toggle(container, "cover_flow", &coverflow_switch, coverflow_cb);
	option_note(container, "cover_flow_note");
	if (coverflow_enabled()) {
		lv_obj_add_state(coverflow_switch, LV_STATE_CHECKED);
	}

	// How the now-playing page is arranged.
	lv_obj_t *layout_pills = NULL;
	settingsrow_pills(container, "musicsettings_player_layout", &layout_pills);
	layout_standard_pill =
		settingsrow_pill(layout_pills, "musicsettings_layout_standard", PLAYER_LAYOUT_STANDARD, layout_pick_cb);
	layout_alternative_pill =
		settingsrow_pill(layout_pills, "musicsettings_layout_alternative", PLAYER_LAYOUT_ALTERNATIVE, layout_pick_cb);
	layout_studio_pill =
		settingsrow_pill(layout_pills, "musicsettings_layout_studio", PLAYER_LAYOUT_STUDIO, layout_pick_cb);

	layout_refresh();
	theme_register_refresh(layout_refresh);

	// Said here rather than left to be discovered: the Waveform arrangement is
	// built out of a file on the card -- the shape of the track, the sleeve it
	// takes its colour from, the tags in the pill -- so a radio station, a
	// stream from a phone and an audiobook all get the standard one whatever
	// this says. Studio asks for less and takes all of them.
	option_note(container, "musicsettings_local_only_note");

	// Under the layout, because it is the other thing on this page about where
	// something is rather than about what it says.
	settingsrow_toggle(container, "musicsettings_playlists_first", &playlists_first_switch, playlists_first_cb);
	option_note(container, "musicsettings_playlists_first_note");
	if (musicsettings_playlists_first()) {
		lv_obj_add_state(playlists_first_switch, LV_STATE_CHECKED);
	}

	switcher_attach_back_gesture(display_screen);
}

static lv_obj_t *endless_shuffle_switch;

static void endless_shuffle_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("player", "endless_shuffle", lv_obj_has_state(endless_shuffle_switch, LV_STATE_CHECKED));
	config_save();
}

// The sleep timer for music, which is the card, Tidal and Qobuz: one transport,
// one timer. Books and podcasts have their own, on their own pages, because the
// stretch that suits one is not the one that suits another.
static settingsrow_duration_t sleep_row;

static void sleep_refresh(void) {
	settingsrow_duration_expanded(&sleep_row, sleeptimer_enabled(SLEEPTIMER_MUSIC));
	settingsrow_duration_repaint(&sleep_row);
}

static void sleep_toggle_cb(lv_event_t *e) {
	(void)e;
	sleeptimer_set_enabled(SLEEPTIMER_MUSIC, lv_obj_has_state(sleep_row.toggle, LV_STATE_CHECKED));
	sleep_refresh();
}

static void sleep_wheel_cb(lv_event_t *e) {
	(void)e;
	sleeptimer_set_minutes(SLEEPTIMER_MUSIC, settingsrow_duration_minutes(&sleep_row));
}

static void build_playback_page(gui_config_t *cfg) {
	playback_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(playback_screen, cfg, "musicsettings_playback_options");
	// No corner buttons on this page, so the heading can have the width the
	// default reserves for one -- "Opzioni di riproduzione" does not fit without
	// it.
	settingsrow_title_corner_slots(settingsrow_page_title(playback_screen), cfg, 0);

	// No gaps between tracks. Underneath it is a PCM that stays open (see
	// audio.h): it works only for music on the card, and only between tracks of
	// the same format.
	settingsrow_toggle(container, "musicsettings_gapless_playback", &gapless_switch, gapless_toggle_cb);
	if (config_get_int("audio", "gapless", 0)) {
		lv_obj_add_state(gapless_switch, LV_STATE_CHECKED);
	}

	// ReplayGain: off, or corrected per track or per record.
	rg_card = settingsrow_toggle_pills(container, "musicsettings_replay_gain", rg_toggle_cb, &rg_switch, &rg_pills);
	rg_track_pill = settingsrow_pill(rg_pills, "track", REPLAYGAIN_TRACK, rg_pick_cb);
	rg_album_pill = settingsrow_pill(rg_pills, "albums", REPLAYGAIN_ALBUM, rg_pick_cb);
	rg_refresh();

	// The fade at the two ends of a track: a page of its own because it carries
	// a length as well as a switch.
	build_fade_page(cfg);
	fade_row = settingsrow_add(container, "fade", NULL, switch_screen_cb, fade_screen);

	// Which of the two shuffles "Play in random order" starts. The mode button
	// on the player still reaches both by hand; this only decides what that one
	// action means. Off by default, so the action stays the plain shuffle: once
	// through the list and stop.
	settingsrow_toggle(container, "musicsettings_continuous_shuffle", &endless_shuffle_switch, endless_shuffle_cb);
	option_note(container, "musicsettings_shuffle_note");
	if (musicsettings_endless_shuffle()) {
		lv_obj_add_state(endless_shuffle_switch, LV_STATE_CHECKED);
	}

	// The sleep timer, directly under it, with its two wheels revealed by the
	// switch the way the screensaver's two pills are.
	settingsrow_toggle_duration(container, "sleep_timer", sleep_toggle_cb, sleep_wheel_cb, &sleep_row);
	settingsrow_duration_set_minutes(&sleep_row, sleeptimer_minutes(SLEEPTIMER_MUSIC));
	sleep_refresh();
	theme_register_refresh(sleep_refresh);

	// One record into the next, instead of the queue simply running out.
	settingsrow_toggle(container, "musicsettings_play_albums_back_to_back", &album_chain_switch, album_chain_cb);
	// What "back to back" means here, since the answer is not obvious: the next
	// record is the next one in the album list, and the repeat setting does not
	// get to send the same one round again.
	option_note(container, "musicsettings_album_chaining_note");
	if (device_state_album_chaining()) {
		lv_obj_add_state(album_chain_switch, LV_STATE_CHECKED);
	}

	// The same for a folder started from the browser. The order it walks in is
	// worth spelling out, because it is chosen and not obvious.
	settingsrow_toggle(container, "musicsettings_play_by_folder", &folder_chain_switch, folder_chain_cb);
	option_note(container, "musicsettings_folder_chaining_note");
	if (device_state_folder_chaining()) {
		lv_obj_add_state(folder_chain_switch, LV_STATE_CHECKED);
	}

	// What survives the power switch: the loaded track and the volume level.
	settingsrow_toggle(container, "musicsettings_remember_track", &remember_track_switch, remember_track_cb);
	if (config_get_int("player", "remember_track", 0)) {
		lv_obj_add_state(remember_track_switch, LV_STATE_CHECKED);
	}
	settingsrow_toggle(container, "musicsettings_remember_volume", &remember_volume_switch, remember_volume_cb);
	if (config_get_int("player", "remember_volume", 0)) {
		lv_obj_add_state(remember_volume_switch, LV_STATE_CHECKED);
	}

	switcher_attach_back_gesture(playback_screen);
}

static void musicsettings_loaded_cb(lv_event_t *e) {
	(void)e;
	rg_refresh();
}

void musicsettings_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(musicsettings_screen, cfg, "music");

	// The two pages of behaviour first: how the index is built, and what
	// playback does around a track. Then the sound, from the widest tool to the
	// narrowest, and the DAC's own switches at the end.
	build_scan_page(cfg);
	settingsrow_add(container, "musicsettings_scan_options", NULL, switch_screen_cb, scan_screen);

	build_playback_page(cfg);
	settingsrow_add(container, "musicsettings_playback_options", NULL, switch_screen_cb, playback_screen);

	build_display_page(cfg);
	settingsrow_add(container, "musicsettings_display_options", NULL, switch_screen_cb, display_screen);

	build_eq_page(cfg);
	eq_row = settingsrow_add(container, "equaliser", NULL, switch_screen_cb, eq_screen);

	// PEQ right below: the same thing said more precisely, so whoever comes
	// looking for the equaliser finds the slider one first and the parametric
	// one next to it.
	peqpage_init(cfg);
	peq_row = settingsrow_add(container, "peq", NULL, switch_screen_cb, peqpage_screen());

	build_mseb_page(cfg);
	mseb_row = settingsrow_add(container, "mseb", NULL, switch_screen_cb, mseb_screen);

	build_soundfield_page(cfg);
	soundfield_row = settingsrow_add(container, "musicsettings_soundfield", NULL, switch_screen_cb, soundfield_screen);

	// Crossfeed sits next to soundfield because they are relatives: one widens
	// the stereo image, the other pulls it back inside the head. Anyone looking
	// for one wants to see the other.
	build_crossfeed_page(cfg);
	crossfeed_row = settingsrow_add(container, "musicsettings_crossfeed", NULL, switch_screen_cb, crossfeed_screen);

	build_balance_page(cfg);
	balance_row = settingsrow_add(container, "musicsettings_channel_balance", NULL, switch_screen_cb, balance_screen);

	// The gain step: off = low gain (the stock default), on = +6 dB.
	settingsrow_toggle(container, "musicsettings_high_gain", &gain_switch, gain_toggle_cb);
	if (config_get_int("audio", "high_gain", 0)) {
		lv_obj_add_state(gain_switch, LV_STATE_CHECKED);
	}

	// How loud DSD comes out. The DAC is the only thing that plays it here, so
	// this is the only place its level can be touched at all.
	//
	// What was chosen last time is loaded before the row rather than with the
	// other settings at the end of this function: the row paints itself from
	// what the audio side holds, so a load after it would leave the switch off
	// with the compensation on.
	alsa_set_dsd_gain_index((int)config_get_int("audio", "dsd_gain", 0));
	settingsrow_toggle_pills(container, "musicsettings_dsd_gain", dsd_gain_toggle_cb, &dsd_gain_switch,
							 &dsd_gain_pills);
	for (int i = 0; i < DSD_GAIN_MAX_DB; i++) {
		char text[16];
		snprintf(text, sizeof(text), "+%d dB", i + 1);
		dsd_gain_pill[i] = settingsrow_pill_text(dsd_gain_pills, text, i + 1, dsd_gain_pick_cb);
	}
	dsd_gain_refresh();

	build_filter_page(cfg);
	settingsrow_add(container, "musicsettings_filters", NULL, switch_screen_cb, dacfilter_screen);

	#ifdef BOARD_R1
		// R1 does not have the NOS_EN option in ALSA
	#else
		// Non-oversampling, off by default like the stock player.
		settingsrow_toggle(container, "musicsettings_nos", &nos_switch, nos_toggle_cb);
		if (config_get_int("audio", "dac_nos", 0)) {
			lv_obj_add_state(nos_switch, LV_STATE_CHECKED);
		}
	#endif

	#ifdef BOARD_R1
		// R1 does not have the DRE_EN option in ALSA
	#else
		// The DAC's dynamic-range enhancement, on by default like the stock player.
		settingsrow_toggle(container, "musicsettings_dac_dre", &dre_switch, dre_toggle_cb);
		if (config_get_int("audio", "dac_dre", 1)) {
			lv_obj_add_state(dre_switch, LV_STATE_CHECKED);
		}
	#endif

	theme_register_refresh(rg_refresh);
	theme_register_refresh(dsd_gain_refresh);
	lv_obj_add_event_cb(musicsettings_screen, musicsettings_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);

	// What was chosen last time becomes effective at boot. audio_init has
	// already written a volume by this point, so the gain is re-applied here to
	// keep the two consistent.
	fade_apply_setting();

	set_dac_filter((int)config_get_int("audio", "dac_filter", 0));
	set_dac_dre((int)config_get_int("audio", "dac_dre", 1));
	set_dac_nos((int)config_get_int("audio", "dac_nos", 0));
	set_high_gain((int)config_get_int("audio", "high_gain", 0));


	lv_obj_add_event_cb(musicsettings_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}
