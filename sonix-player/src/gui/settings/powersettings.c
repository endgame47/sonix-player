#include "powersettings.h"

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/led.h"
#include "src/system/device/power.h"

lv_obj_t *powersettings_screen;

// 80 to 100 in fives. Stopping short of full is what keeps a lithium cell
// healthy if the device mostly lives on the charger.
#define CHARGE_LIMIT_MIN 80
#define CHARGE_LIMIT_STEP 5
#define CHARGE_LIMIT_COUNT 5 // 80, 85, 90, 95, 100

static const struct {
	int minutes;
	const char *label;
} AUTO_OFF[] = {
	{15, "power_15_minutes"}, {30, "power_30_minutes"}, {60, "power_1_hour"}, {120, "power_2_hours"}, {180, "power_3_hours"}, {360, "power_6_hours"},
};
#define AUTO_OFF_COUNT ((int)(sizeof(AUTO_OFF) / sizeof(AUTO_OFF[0])))

static lv_obj_t *charge_value, *charge_slider;
static lv_obj_t *auto_off_switch, *auto_off_value, *auto_off_slider, *auto_off_card;
static lv_obj_t *led_on_switch, *led_off_switch, *led_off_card;
static lv_obj_t *standby_switch;
static lv_obj_t *charge_note;

// --- reading the saved values ---

static int charge_index(void) {
	int percent = (int)config_get_int("power", "charge_limit", 100);
	int index = (percent - CHARGE_LIMIT_MIN) / CHARGE_LIMIT_STEP;
	if (index < 0) {
		index = 0;
	}
	if (index >= CHARGE_LIMIT_COUNT) {
		index = CHARGE_LIMIT_COUNT - 1;
	}
	return index;
}

static int auto_off_index(void) {
	int minutes = (int)config_get_int("power", "auto_off_minutes", 30);
	for (int i = 0; i < AUTO_OFF_COUNT; i++) {
		if (AUTO_OFF[i].minutes == minutes) {
			return i;
		}
	}
	return 1;
}

// --- applying them ---

void powersettings_apply(void) {
	power_set_charge_limit(CHARGE_LIMIT_MIN + (charge_index() * CHARGE_LIMIT_STEP));

	power_set_auto_off(config_get_bool("power", "auto_off", false), (uint32_t)AUTO_OFF[auto_off_index()].minutes);

	// Suspend-to-RAM standby, on by default: saves battery by suspending the
	// player shortly after the screen goes off.
	power_set_standby_enabled(config_get_bool("power", "standby_mem", true));

	// The LED master switch comes first: with it off there is nothing for the
	// standby option to turn off, and led.c keeps the charge indicator alive
	// on its own regardless.
	led_set_enabled(config_get_bool("power", "led_on", true));
	led_set_idle_off(config_get_bool("power", "led_off_standby", true));
}

static void refresh_labels(void) {
	lv_label_set_text_fmt(charge_value, "%d%%", CHARGE_LIMIT_MIN + (charge_index() * CHARGE_LIMIT_STEP));
	lv_label_set_text(auto_off_value, tr(AUTO_OFF[auto_off_index()].label));

	// The LED master switch, and the standby option that hangs off it: with the
	// LED switched off entirely there is nothing left for the standby option to
	// do, so it greys out.
	bool led_on = config_get_bool("power", "led_on", true);
	if (led_on) {
		lv_obj_add_state(led_on_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(led_on_switch, LV_STATE_CHECKED);
	}

	if (config_get_bool("power", "led_off_standby", true)) {
		lv_obj_add_state(led_off_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(led_off_switch, LV_STATE_CHECKED);
	}

	if (led_on) {
		lv_obj_remove_state(led_off_switch, LV_STATE_DISABLED);
		lv_obj_set_style_opa(led_off_card, LV_OPA_COVER, 0);
	} else {
		lv_obj_add_state(led_off_switch, LV_STATE_DISABLED);
		lv_obj_set_style_opa(led_off_card, LV_OPA_40, 0);
	}

	if (config_get_bool("power", "standby_mem", true)) {
		lv_obj_add_state(standby_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(standby_switch, LV_STATE_CHECKED);
	}

	// One card: the slider is only on it while the toggle is on.
	bool auto_off = config_get_bool("power", "auto_off", false);
	if (auto_off) {
		lv_obj_add_state(auto_off_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(auto_off_switch, LV_STATE_CHECKED);
	}
	settingsrow_toggle_slider_expanded(auto_off_card, auto_off);
}

static void charge_changed_cb(lv_event_t *e) {
	(void)e;
	int index = (int)lv_slider_get_value(charge_slider);
	config_set_int("power", "charge_limit", CHARGE_LIMIT_MIN + (index * CHARGE_LIMIT_STEP));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void auto_off_changed_cb(lv_event_t *e) {
	(void)e;
	int index = (int)lv_slider_get_value(auto_off_slider);
	config_set_int("power", "auto_off_minutes", AUTO_OFF[index].minutes);
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void led_off_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "led_off_standby", lv_obj_has_state(led_off_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

// The master switch. With the LED off the standby option has nothing left to
// do, so it greys out -- and the charger still lights the LED regardless, so
// a charge in progress is never invisible.
static void led_on_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "led_on", lv_obj_has_state(led_on_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void auto_off_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "auto_off", lv_obj_has_state(auto_off_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void standby_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "standby_mem", lv_obj_has_state(standby_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

void powersettings_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(powersettings_screen, cfg, "power");

	// Suspend-to-RAM standby: suspends the player shortly after the screen goes
	// off to save battery, and playback resumes on wake. On by default. What it
	// waits for is the screen going dark, and that timeout lives on the Screen
	// page, beside the brightness it belongs with.
	settingsrow_toggle(container, "power_standby_mem", &standby_switch, standby_toggled_cb);
	lv_obj_t *standby_note = lv_label_create(container);
	lv_label_set_long_mode(standby_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(standby_note, lv_pct(100));
	lv_obj_add_style(standby_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(standby_note, &font_ui_22, 0);
	lv_label_set_text(standby_note, tr("power_standby_mem_note"));

	settingsrow_slider(container, "power_charge_limit", CHARGE_LIMIT_COUNT, &charge_value, &charge_slider,
					   charge_changed_cb);
	lv_slider_set_value(charge_slider, charge_index(), LV_ANIM_OFF);

	// The LED master switch, and the standby option that depends on it.
	settingsrow_toggle(container, "power_led_on", &led_on_switch, led_on_toggled_cb);
	led_off_card = settingsrow_toggle(container, "power_led_off_in_standby", &led_off_switch, led_off_toggled_cb);
	// No "turn Wi-Fi off in standby" entry: parking the radio is automatic, and
	// wifi_in_use() in power.c knows on its own when not to turn it off --
	// Qobuz, Tidal, podcasts, radio, AirPlay, DLNA, transfers.

	// Toggle and "after how long" on one card: the slider is only there while
	// the option is on.
	auto_off_card = settingsrow_toggle_slider(container, "power_auto_power_off", AUTO_OFF_COUNT,
											  &auto_off_switch, &auto_off_value, &auto_off_slider,
											  auto_off_toggled_cb, auto_off_changed_cb);
	lv_slider_set_value(auto_off_slider, auto_off_index(), LV_ANIM_OFF);

	// If the charger driver has no control node there is nothing to enforce
	// the limit with, and saying so is better than a setting that silently
	// does nothing.
	charge_note = lv_label_create(container);
	lv_label_set_long_mode(charge_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(charge_note, lv_pct(100));
	lv_obj_add_style(charge_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(charge_note, &font_ui_22, 0);
	lv_label_set_text(charge_note, power_charge_limit_supported()
									   ? tr("power_auto_off_note")
									   : tr("power_charge_limit_unavailable"));

	// Everything saved becomes effective now, at startup -- including the LED
	// standby option, which lives in led.c rather than power.c.
	powersettings_apply();
	refresh_labels();
}
