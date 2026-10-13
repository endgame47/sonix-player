#include "lowbattery.h"

#include <stdio.h>
#include <stdlib.h>

#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"

#define CHECK_MS 5000

#define WARN_PERCENT 5
// The notice comes back only after a charge: the gauge reads the cell's
// voltage, which climbs a few points again whenever the load drops.
#define WARN_AGAIN_PERCENT 10

#define OFF_PERCENT 1
// Readings in a row at OFF_PERCENT or under before the countdown starts: a
// voltage gauge dips for a moment under a burst of load, and the first reading
// after a boot or a wake can be stale.
#define OFF_READINGS 3
// A cell still above this is not empty, whatever the gauge says: a gauge that
// failed and reads 0 must not switch the player off at every boot.
#define OFF_MAX_MV 3600
#define COUNTDOWN_S 10

#define COLOR_EMPTY lv_color_make(224, 27, 36)

static lv_timer_t *check_timer;
static lv_timer_t *countdown_timer;
static int countdown_left;
static int low_readings;
static bool warned;

// The level in percent, or -1 when the gauge gives none.
static int level(void) {
	sync_battery_from_sysfs();
	const char *text = read_battery_percent();
	if (!text || text[0] < '0' || text[0] > '9') {
		return -1;
	}
	return atoi(text);
}

static bool on_battery(void) { return !usb_vbus_present() && !read_battery_charging(); }

static void countdown_show(void) {
	char text[200];
	snprintf(text, sizeof(text), tr("battery_empty_countdown"), countdown_left);
	gui_modal_show(&icon_battery_warning, COLOR_EMPTY, "battery_empty_title", text);
}

static void countdown_stop(void) {
	if (countdown_timer) {
		lv_timer_delete(countdown_timer);
		countdown_timer = NULL;
	}
	gui_modal_hide();
}

static void countdown_cb(lv_timer_t *timer) {
	(void)timer;
	if (!on_battery()) {
		printf("power: charger plugged in, low battery shutdown called off\n");
		countdown_stop();
		low_readings = 0;
		return;
	}
	countdown_left--;
	if (countdown_left > 0) {
		countdown_show();
		return;
	}
	lv_timer_delete(countdown_timer);
	countdown_timer = NULL;
	printf("power: battery empty, shutting down\n");
	fflush(stdout);
	power_shutdown();
}

static void countdown_start(int mv) {
	printf("power: battery at %d%% or under (%d mV), shutting down in %d s\n", OFF_PERCENT, mv, COUNTDOWN_S);
	countdown_left = COUNTDOWN_S;
	confirm_close(); // the 5 % notice, if it is still up
	if (!power_screen_is_on()) {
		power_screen_on();
	}
	countdown_show();
	countdown_timer = lv_timer_create(countdown_cb, 1000, NULL);
}

static void check_cb(lv_timer_t *timer) {
	(void)timer;
	if (countdown_timer) {
		return;
	}
	int percent = level();
	if (percent >= WARN_AGAIN_PERCENT) {
		warned = false;
	}
	if (percent < 0 || !on_battery()) {
		low_readings = 0;
		return;
	}

	if (percent <= OFF_PERCENT) {
		int mv = read_battery_millivolts();
		if (mv >= OFF_MAX_MV) {
			low_readings = 0;
			return;
		}
		low_readings++;
		if (low_readings >= OFF_READINGS) {
			low_readings = 0;
			countdown_start(mv);
		}
		return;
	}
	low_readings = 0;

	// Only in the band above the shutdown: a player switched on at 1 % goes
	// straight to the countdown.
	if (!warned && percent <= WARN_PERCENT) {
		warned = true;
		printf("power: battery at %d%%\n", percent);
		char message[200];
		snprintf(message, sizeof(message), tr("battery_low_message"), percent);
		confirm_notice("battery_low_title", message);
	}
}

void lowbattery_init(gui_config_t *cfg) {
	(void)cfg;
	if (check_timer) {
		return;
	}
	// Not registered with power.c's standby list: it keeps its period with the
	// screen dark, which is when a battery left playing runs out.
	check_timer = lv_timer_create(check_cb, CHECK_MS, NULL);
}
