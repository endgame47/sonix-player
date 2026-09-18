#include "systempage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/confirm.h"
#include "src/gui/shell/easteregg.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/settings/settings.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/device/factoryreset.h"
#include "src/system/device/firmware.h"
#include "src/system/audio/headset.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/device/sysinfo.h"
#include "src/system/device/system.h"

lv_obj_t *systempage_screen;
lv_obj_t *sysinfo_screen;

// The headset controls are under Settings > More (see settings.c). System is
// entered to update the firmware or wipe the device, twice a year, which has
// nothing to do with a switch touched when headphones change.

// ---------------------------------------------------------------------------
// Firmware update
//
// The stock player's procedure, step by step.
//
//   battery below 30% and not charging -> refuse, and say why
//   otherwise                          -> ask to update the system firmware
//   on OK, no .upt on the card         -> report no update file found
//   otherwise -> arm recovery and reboot; its kernel does the rest.
//
// The file check comes after the question, not before, because that is the
// order the stock player asks in, and it is also the right order: whether there
// is an update on this card is what the user came to find out.
// ---------------------------------------------------------------------------

#define FIRMWARE_MIN_BATTERY_PERCENT 30

static void firmware_confirmed(void *user) {
	(void)user;

	char path[512];
	if (!firmware_update_file_find(path, sizeof(path))) {
		gui_notify_popup("system_update_file_missing");
		return;
	}

	printf("firmware: update file %s\n", path);
	firmware_update_start();
}

static void firmware_clicked_cb(lv_event_t *e) {
	(void)e;

	// Charge check first, like the stock player: an update that loses power
	// halfway leaves the device in recovery with a half-written kernel, the one
	// failure here that cannot be undone from the UI. An unreadable level must
	// not block the update: the reader returns "!!" when it cannot find the
	// gauge (the host build, and any firmware naming that sysfs node
	// differently), and refusing to update over a battery nobody can measure
	// would be the opposite of useful.
	char *percent_text = read_battery_percent();
	int percent = 100;
	if (percent_text && percent_text[0] >= '0' && percent_text[0] <= '9') {
		percent = atoi(percent_text);
	}
	bool charging = read_battery_charging();

	if (percent < FIRMWARE_MIN_BATTERY_PERCENT && !charging) {
		gui_notify_popup("system_battery_warning");
		return;
	}

	confirm_show("system_update_firmware", "system_update_confirm", "system_update", firmware_confirmed, NULL);
}

// ---------------------------------------------------------------------------
// Factory reset
//
// Two confirmations in a row, like iOS and Android. Not a formality: the first
// asks whether to do it, the second says what is lost, and they are separate
// deliberately, because the second only appears after the first has closed, so
// they cannot be tapped through by accident.
// ---------------------------------------------------------------------------

static void factory_second_confirmed(void *user) {
	(void)user;
	// Point of no return: factoryreset_run() does not return, it reboots.
	factoryreset_run();
}

static void factory_first_confirmed(void *user) {
	(void)user;
	confirm_show("system_are_you_sure", "system_reset_confirm_note",
				 "system_reset_2", factory_second_confirmed, NULL);
}

static void factory_clicked_cb(lv_event_t *e) {
	(void)e;
	confirm_show("system_factory_reset", "system_reset_confirm",
				 "system_continue", factory_first_confirmed, NULL);
}

// ---------------------------------------------------------------------------
// microSD card
// ---------------------------------------------------------------------------

// The Adwaita red, from the same palette as the rest of the interface.
#define BAR_RED lv_color_make(224, 27, 36)

// Below this share of free space the bar turns red.
#define SD_LOW_PERCENT 10

static lv_obj_t *sd_value;
static lv_obj_t *sd_bar;

static void sd_refresh(void) {
	if (!sd_value || !sd_bar) {
		return;
	}

	sysinfo_storage_t usage;
	if (!sysinfo_sd_usage(&usage) || !usage.present) {
		lv_label_set_text(sd_value, tr("system_no_card"));
		lv_obj_add_flag(sd_bar, LV_OBJ_FLAG_HIDDEN);
		return;
	}

	char used[32], total[32], text[160];
	sysinfo_format_size(usage.used, used, sizeof(used));
	sysinfo_format_size(usage.total, total, sizeof(total));
	// A read-only card is worth saying here rather than leaving the user to
	// discover it as five separate features that quietly do nothing.
	if (storage_sd_writable()) {
		snprintf(text, sizeof(text), "%s / %s", used, total);
	} else {
		snprintf(text, sizeof(text), "%s / %s  (%s)", used, total, tr("system_read_only"));
	}
	lv_label_set_text(sd_value, text);

	lv_obj_remove_flag(sd_bar, LV_OBJ_FLAG_HIDDEN);
	lv_bar_set_value(sd_bar, usage.used_percent, LV_ANIM_OFF);

	// Red once free space drops below the threshold. The comparison is integer
	// and multiplies instead of dividing: a percentage computed first would
	// lose exactly the borderline cases, and on a two-terabyte card free * 100
	// still fits comfortably in 64 bits.
	bool low = usage.free * 100 <= usage.total * SD_LOW_PERCENT;
	lv_obj_set_style_bg_color(sd_bar, low ? BAR_RED : theme()->accent, LV_PART_INDICATOR);
}

// ---------------------------------------------------------------------------
// The two versions, and the five taps
//
// As on Android: developer options stay hidden until the build number is tapped
// five times. Not security -- anyone reading this knows how -- but keeping a
// page that serves two people out of everyone else's way.
// ---------------------------------------------------------------------------

#define DEVOPTIONS_TAPS 5

// The tap from which the remaining count is announced. Nothing is said before
// that, or there would be no discovery left.
#define DEVOPTIONS_HINT_FROM 3

static int build_taps;

bool systempage_devoptions_unlocked(void) { return config_get_int("system", "devoptions_unlocked", 0) != 0; }

static void build_tapped_cb(lv_event_t *e) {
	(void)e;

	if (systempage_devoptions_unlocked()) {
		gui_notify_popup("system_devoptions_already_on");
		return;
	}

	build_taps++;
	int missing = DEVOPTIONS_TAPS - build_taps;

	if (missing > 0) {
		if (build_taps >= DEVOPTIONS_HINT_FROM) {
			char text[96];
			snprintf(text, sizeof(text), tr("system_devoptions_countdown"), missing);
			gui_notify_popup(text);
		}
		return;
	}

	config_set_int("system", "devoptions_unlocked", 1);
	config_save();
	build_taps = 0;

	settings_refresh_devoptions();
	gui_notify_popup("system_devoptions_on");
}

// ---------------------------------------------------------------------------
// building the two pages
// ---------------------------------------------------------------------------

// A card with the name on the left and the value on the right, like any other
// row, but not clickable: the versions and the card usage are information, not
// commands. `value_out` receives the right-hand label.
static lv_obj_t *info_row(lv_obj_t *parent, const char *name, lv_obj_t **value_out) {
	lv_obj_t *row = settingsrow_add(parent, name, value_out, NULL, NULL);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
	return row;
}

static void sysinfo_loaded_cb(lv_event_t *e) {
	(void)e;
	// Card usage changes while the player runs, so it is re-read on every
	// opening, and the tap counters start over.
	sd_refresh();
	build_taps = 0;
	easteregg_reset();
}

// The usage bar is redrawn with the new palette.
static void refresh_theme(void) { sd_refresh(); }

static void build_sysinfo_page(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(sysinfo_screen, cfg, "system_about");

	// --- device and DAC: two read-only rows ---
	// Neither value goes through tr(): a device name and a silicon part
	// number are the same in every language.
	lv_obj_t *device_value = NULL;
	info_row(container, "system_device", &device_value);
	#ifdef BOARD_R1
		lv_label_set_text(device_value, "Hiby R1");
	#else
		lv_label_set_text(device_value, "Hiby R3 Pro II");
	#endif

	lv_obj_t *dac_value = NULL;
	info_row(container, "dac", &dac_value);
	#ifdef BOARD_R1
		lv_label_set_text(dac_value, "Cirrus Logic CS43131");
	#else
		lv_label_set_text(dac_value, "Dual Cirrus Logic CS43198");
	#endif

	// The serial number, from the SoC efuse: the same one printed on the box
	// ("R3PII" plus the first eight hex digits of the chip id). Not translated,
	// being an identifier.
	lv_obj_t *serial_value = NULL;
	info_row(container, "system_serial_number", &serial_value);
	const char *sn = sysinfo_serial_number();
	lv_label_set_text(serial_value, sn[0] ? sn : "\xE2\x80\x94");

	// --- microSD card: value on the right, bar underneath ---
	lv_obj_t *sd_card = lv_obj_create(container);
	lv_obj_set_width(sd_card, lv_pct(100));
	lv_obj_set_height(sd_card, 116);
	lv_obj_add_style(sd_card, &theme_style_card, 0);
	lv_obj_set_style_radius(sd_card, 12, 0);
	lv_obj_set_style_border_width(sd_card, 0, 0);
	lv_obj_set_style_shadow_width(sd_card, 0, 0);
	lv_obj_set_style_pad_hor(sd_card, 20, 0);
	lv_obj_set_style_pad_ver(sd_card, 16, 0);
	lv_obj_remove_flag(sd_card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(sd_card, LV_OBJ_FLAG_CLICKABLE);

	lv_obj_t *sd_name = lv_label_create(sd_card);
	lv_label_set_text(sd_name, tr("system_sd_card"));
	lv_obj_add_style(sd_name, &theme_style_text, 0);
	lv_obj_set_style_text_font(sd_name, &font_ui_24, 0);
	lv_obj_align(sd_name, LV_ALIGN_TOP_LEFT, 0, 0);

	sd_value = lv_label_create(sd_card);
	lv_obj_add_style(sd_value, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(sd_value, &font_ui_20, 0);
	lv_obj_align(sd_value, LV_ALIGN_TOP_RIGHT, 0, 0);
	lv_label_set_text(sd_value, "");

	sd_bar = lv_bar_create(sd_card);
	lv_obj_set_width(sd_bar, lv_pct(100));
	lv_obj_set_height(sd_bar, 10);
	lv_obj_align(sd_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_bar_set_range(sd_bar, 0, 100);
	lv_obj_set_style_radius(sd_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_bg_color(sd_bar, theme()->text_secondary, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(sd_bar, LV_OPA_40, LV_PART_MAIN);
	lv_obj_set_style_radius(sd_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(sd_bar, theme()->accent, LV_PART_INDICATOR);

	// --- the two versions ---
	// The system version carries the thank-you (see easteregg.c), so it stays
	// clickable where the rows above it are not.
	lv_obj_t *os_value = NULL;
	lv_obj_t *os_row = settingsrow_add(container, "system_operating_system_version", &os_value, NULL, NULL);
	const char *os = sysinfo_os_version();
	lv_label_set_text(os_value, os[0] ? os : "\xE2\x80\x94"); // em dash
	easteregg_attach(os_row);

	// Five taps reveal the developer options.
	lv_obj_t *build_value = NULL;
	settingsrow_add(container, "system_build_number", &build_value, build_tapped_cb, NULL);
	const char *build = sysinfo_build_version();
	lv_label_set_text(build_value, build[0] ? build : "\xE2\x80\x94");

	lv_obj_add_event_cb(sysinfo_screen, sysinfo_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(sysinfo_screen);
	theme_register_refresh(refresh_theme);
}

void systempage_init(gui_config_t *cfg) {
	// The versions are read once: the file does not change while the player
	// runs.
	sysinfo_load();

	build_sysinfo_page(cfg);

	lv_obj_t *container = settingsrow_page(systempage_screen, cfg, "system");

	settingsrow_add(container, "system_about", NULL, switch_screen_cb, sysinfo_screen);
	// Actions, not pages: settingsrow_action leaves off the chevron.
	settingsrow_action(container, "system_update_firmware", firmware_clicked_cb, NULL);
	settingsrow_action(container, "system_factory_reset", factory_clicked_cb, NULL);

	switcher_attach_back_gesture(systempage_screen);
}
