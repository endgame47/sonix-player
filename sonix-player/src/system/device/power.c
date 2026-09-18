/*
 * This file is largely LLM-written, be warned
 */

#include "power.h"

// LVGL 9.2 made lv_timer_t opaque. There is a lv_timer_set_period() but no
// getter, and both places here have to read the period before changing it.
#include "lvgl/src/misc/lv_timer_private.h"

#include "src/system/audio/waveform.h"
#include "src/system/device/system.h"

#include <dirent.h>
#include <sys/reboot.h>
#include <sys/stat.h>

#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/audio.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/device/clock.h"
#include "src/system/playback/device_state.h"
#include "src/system/remote/dlna.h"
#include "src/system/remote/sonixlink.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/device/adb.h"
#include "src/system/library/library.h"
#include "src/system/remote/airplay.h"
#include "src/system/streaming/radio.h"
#include "src/system/net/wifi.h"
#include "src/system/net/wifitransfer.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/bluetooth/btreceiver.h"
#include "src/system/core/config.h"
#include "src/system/device/usb.h"
#include "src/system/audio/usbdac.h"
#include "src/system/gearboy/gearboy.h"
#include "src/system/device/led.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// How often the power state machine polls. Kept short so a power-button press
// feels responsive; the work done per tick is trivial.
#define POWER_TICK_MS 250

// How long ago a marker was stamped, from the tick the caller is working with.
//
// A mark can be a fraction ahead of `now`: the tick reads lv_tick_get() once at
// the top and carries that value through, while power_screen_off() -- called
// from inside the very same tick -- stamps its markers with a fresh
// lv_tick_get(). A plain unsigned `now - mark` would then read as forty-nine
// days rather than as minus one, so the difference is taken signed and clamped
// at zero.
static uint32_t since(uint32_t mark, uint32_t now) {
	int32_t elapsed = (int32_t)(now - mark);
	return elapsed > 0 ? (uint32_t)elapsed : 0;
}

// The dimmest level offered on the slider. Zero would be accepted by the
// backlight driver and put the PWM out, but it would leave a black screen on a
// fully powered panel: turning the screen off is the blank node's job.
#define BRIGHTNESS_MIN 1
#define BRIGHTNESS_MAX_DEFAULT 100

// The two blank levels written to the framebuffer node, spelled out here
// rather than pulled in from linux/fb.h so the host build compiles unchanged.
#define FB_BLANK_UNBLANK 0
#define FB_BLANK_POWERDOWN 4

// Never treat a backlight level below this as a valid "on" level. Guards against
// adopting a leftover dim value -- e.g. a previous run that exited with the
// screen off left the panel at BRIGHTNESS_MIN -- which would make "screen on"
// come back essentially black (backlight technically on, but too dim to see).
#define BRIGHTNESS_ON_FLOOR 20

// After unblanking, wait this long before pushing a new brightness at the panel.
// The fb blank notifier re-inits the panel + re-powers the backlight on unblank;
// a brightness write that lands mid-reinit can be swallowed, leaving the screen
// dark. A short settle makes the wake reliable.
#define SCREEN_ON_SETTLE_US 60000 // 60ms

// Bluetooth. A firmware whose /etc/init.d/S80_bt_init has not been replaced
// still powers the chip up at every boot -- rfkill on, patchram, bluetoothd,
// bluealsa -- whether anything uses it or not. Putting it back down is
// bluetooth.c's job and not a script's: the firmware's bt_suspend ends with
// `killall dbus-daemon` and would take the bus with it.

// Smooth dim: step size and per-step delay when ramping the backlight up/down.
// ~17 steps * 12ms ~= 200ms for a full 1<->100 fade.
#define FADE_STEP 6
#define FADE_STEP_DELAY_US 12000

// --- Module state (owned by the LVGL/main thread unless noted) ---
static power_config_t g_cfg;
static lv_display_t *g_disp;
static lv_timer_t *g_refr_timer; // display refresh timer, paused while screen is off
static long g_max_brightness = -1;
static bool g_screen_on = true;
static uint32_t g_screen_off_at; // lv_tick when the panel last went dark

// The standby countdown: the idle wait before a suspend attempt. It restarts at
// every activity -- something playing, a key, the cable going in or out --
// rather than counting from the screen blank. Pause therefore counts as idle
// (the count advances and the device suspends), while a playing track or a
// cable just unplugged clear it. The short gap between one track and the next
// (gapless load) is far too brief to reach the timeout.
static uint32_t g_mem_active_ms;   // last activity tick, for standby purposes
static bool g_usb_present_last;    // cable state on the previous tick, to spot a change
static bool g_usb_present_known;   // false until the cable has been read at least once


static long g_hw_brightness = -1;	   // last brightness value actually written to sysfs
static uint32_t g_last_playing_ms;	   // last tick audio was PLAYING (suspend clock)
static uint32_t g_ignore_button_until; // tick until which power presses are absorbed
static uint32_t g_last_active_boot_ms;	// last activity, on a clock that counts suspended time
static bool g_bt_powered_down;			// the boot-time stack has already been put down

// Cross-thread fields: written by input threads, read by the state machine.
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_power_button_pending = false;
static uint32_t g_last_button_activity_ms; // last physical-button/explicit activity

// --- Small sysfs helpers ---
static long read_long_from_file(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return -1;
	}
	long value = -1;
	if (fscanf(f, "%ld", &value) != 1) {
		value = -1;
	}
	fclose(f);
	return value;
}

static void write_long_to_file(const char *path, long value) {
	FILE *f = fopen(path, "w");
	if (!f) {
		perror("power: open sysfs for write");
		return;
	}
	fprintf(f, "%ld", value); // no trailing newline, matching Rockbox's sysfs writer
	fclose(f);
}

static long brightness_max(void) { return g_max_brightness > 0 ? g_max_brightness : BRIGHTNESS_MAX_DEFAULT; }

// Write a backlight level to sysfs, clamped to [BRIGHTNESS_MIN, max]. No-op on
// the host build (no brightness path), but still tracks the value.
static void backlight_write(long value) {
	long max = brightness_max();
	if (value > max) {
		value = max;
	}
	if (value < BRIGHTNESS_MIN) {
		value = BRIGHTNESS_MIN;
	}
	g_hw_brightness = value;
	if (g_cfg.brightness_path) {
		write_long_to_file(g_cfg.brightness_path, value);
	}
}

// Smoothly ramp the backlight from its current level to target, mirroring the
// stock/Rockbox dim-to-off feel. Blocking, but only for ~200ms during a screen
// transition (the UI isn't visible mid-transition anyway).
static void fade_to(long target) {
	if (g_hw_brightness < 0) {
		backlight_write(target);
		return;
	}
	int dir = (target > g_hw_brightness) ? 1 : -1;
	while (g_hw_brightness != target) {
		long next = g_hw_brightness + dir * FADE_STEP;
		if ((dir > 0 && next > target) || (dir < 0 && next < target)) {
			next = target;
		}
		backlight_write(next);
		if (g_hw_brightness != target) {
			usleep(FADE_STEP_DELAY_US);
		}
	}
}

// Turn the panel on and off through the framebuffer blank node.
//
// Off is FB_BLANK_POWERDOWN and not FB_BLANK_NORMAL, because the two drivers
// behind the node read the value differently. The display driver only tests
// for non-zero, so either value stops the controller, drops its clock and
// calls the panel's power-off -- reset low, VCC and VCCIO opened at the PMIC.
// The touch driver compares against POWERDOWN exactly, and does nothing at
// all for the other blank levels: with NORMAL the controller kept scanning
// and holding its rail up for a screen nobody could see.
//
// What it does on the way down is its own decision, taken from the gesture
// flag set just before the blank: doze when double-tap wake is on, a full
// power-off when it is not. The backlight follows either way -- its driver
// takes the blank as a reason to put the PWM at zero.
static void screen_power(bool on) {
	if (!g_cfg.blank_path) {
		return;
	}
	write_long_to_file(g_cfg.blank_path, on ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN);
}

// Enable/disable every LVGL input device (i.e. the touchscreen). Physical
// buttons live in their own threads and are unaffected, so volume/power keys
// keep working while the screen is off -- only stray touches are ignored.
static void set_indevs_enabled(bool enable) {
	lv_indev_t *indev = lv_indev_get_next(NULL);
	while (indev) {
		lv_indev_enable(indev, enable);
		indev = lv_indev_get_next(indev);
	}
}

// CLOCK_BOOTTIME, which -- unlike lv_tick_get()'s CLOCK_MONOTONIC -- keeps
// counting while the SoC is suspended. Everything measured in wall-clock
// terms (the automatic shutdown) has to use this one, or a device that spends
// the night suspended would come back thinking no time had passed.
static uint32_t boottime_ms(void) {
	struct timespec ts;
#ifdef CLOCK_BOOTTIME
	if (clock_gettime(CLOCK_BOOTTIME, &ts) == 0) {
		return (uint32_t)(ts.tv_sec * 1000) + (uint32_t)(ts.tv_nsec / 1000000);
	}
#endif
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000) + (uint32_t)(ts.tv_nsec / 1000000);
}

// current idle time in ms considering both touch (LVGL) and physical buttons
static uint32_t input_idle_ms(uint32_t now) {
	uint32_t touch_idle = lv_display_get_inactive_time(g_disp);

	pthread_mutex_lock(&g_lock);
	uint32_t btn_idle = now - g_last_button_activity_ms;
	pthread_mutex_unlock(&g_lock);

	return touch_idle < btn_idle ? touch_idle : btn_idle;
}

static bool g_screen_on_hold;

// ---------------------------------------------------------------------------
// charge limit
//
// The mp2731 driver publishes its charge current as a module parameter and a
// couple of control nodes; which of them exists differs between firmware
// builds, so the first writable one wins. If none of them is there the setting
// is remembered but cannot be enforced, and the settings page says so rather
// than pretending.
// ---------------------------------------------------------------------------

// The nodes the stock player itself writes, in the order it writes them. Its
// three settings are (MaxPower, charge_control_limit_max, current_max):
//   fast charge  (2000, 0, 2000)
//   slow charge  ( 500, 500, 500)
//   no charge    (  10, 100, 0)
// So zero in constant_charge_current_max is how the firmware stops charging,
// and the values are in mA on this driver, not the microamps the kernel's
// power_supply class normally uses.
//
// One more write matters (stock FUN_00481740 does it unconditionally, last):
// the charger's *input* current limit, in microamps -- MaxPower * 1000, so 10
// mA on a stop and 2 A on full speed. Without it the MP2731 clamps the charge
// current to its minimum instead of zero and keeps trickling, so the battery
// creeps past the limit.
#define CHARGE_CURRENT_NODE "/sys/class/power_supply/usb/constant_charge_current_max"
#define CHARGE_LIMIT_NODE "/sys/class/power_supply/usb/charge_control_limit_max"
#define INPUT_CURRENT_NODE "/sys/class/power_supply/mp2731-charger/input_current_limited"

// What to restore when charging is allowed again, if the node read zero the
// first time (which it does when the device booted on the charger and the stock
// player had already stopped it).
#define CHARGE_CURRENT_DEFAULT 2000

static const char *g_charge_node;	 // the writable node, NULL if there is none
static long g_charge_node_default;	 // what it read before anything was written
static int g_charge_limit = 100;	 // 100 = charge to full
static bool g_charging_suspended;

// Set while something has deliberately forbidden charging regardless of the
// battery level -- DAC mode with the charger switched off. Kept separate from
// g_charging_suspended, which is the automatic limit doing its job: the tick
// must not undo a choice the user made.
static bool g_charging_blocked;


static void find_charge_node(void) {
	static bool searched;
	if (searched) {
		return;
	}
	searched = true;

	if (access(CHARGE_CURRENT_NODE, W_OK) != 0) {
		printf("power: %s is not writable; the charge limit cannot be enforced\n", CHARGE_CURRENT_NODE);
		return;
	}

	g_charge_node = CHARGE_CURRENT_NODE;
	g_charge_node_default = read_long_from_file(g_charge_node);

	// A zero here means charging is already stopped, which is not a sensible
	// thing to restore to.
	if (g_charge_node_default <= 0) {
		g_charge_node_default = CHARGE_CURRENT_DEFAULT;
	}

	printf("power: charge limit will use %s (restore value %ld mA)\n", g_charge_node, g_charge_node_default);
}

bool power_charge_limit_supported(void) {
	find_charge_node();
	return g_charge_node != NULL;
}

int power_get_charge_limit(void) { return g_charge_limit; }

void power_set_charge_limit(int percent) {
	if (percent < 80) {
		percent = 80;
	}
	if (percent > 100) {
		percent = 100;
	}
	g_charge_limit = percent;
}

// Starts or stops the charger with the full set of writes the stock player
// makes -- see the node comments above. `input_current_limited` may not exist
// on every firmware revision, so it is skipped silently when it is missing.
static void charger_run(bool run) {
	if (run) {
		write_long_to_file(CHARGE_LIMIT_NODE, 0);
		write_long_to_file(g_charge_node, g_charge_node_default);
		if (access(INPUT_CURRENT_NODE, W_OK) == 0) {
			write_long_to_file(INPUT_CURRENT_NODE, g_charge_node_default * 1000); // mA -> uA
		}
	} else {
		write_long_to_file(g_charge_node, 0);
		write_long_to_file(CHARGE_LIMIT_NODE, 100); // what the stock player pairs with a stop
		if (access(INPUT_CURRENT_NODE, W_OK) == 0) {
			write_long_to_file(INPUT_CURRENT_NODE, 10 * 1000); // stock's stop value: 10 mA
		}
	}
}

// Called from the tick. Stops the charger above the limit and starts it again
// three percent below, so it does not chatter on and off at the boundary.
static void apply_charge_limit(void) {
	find_charge_node();

	// A deliberate block outranks the percentage limit, and outlasts it: the
	// tick would otherwise restore the charger a second after the user turned
	// it off.
	if (g_charging_blocked) {
		if (g_charge_node && !g_charging_suspended) {
			charger_run(false);
			g_charging_suspended = true;
		}
		return;
	}

	if (!g_charge_node || g_charge_limit >= 100) {
		if (g_charging_suspended) {
			charger_run(true);
			g_charging_suspended = false;
		}
		return;
	}

	const char *level = read_battery_percent();
	if (!level || level[0] < '0' || level[0] > '9') {
		return;
	}

	int percent = atoi(level);

	if (!g_charging_suspended && percent >= g_charge_limit) {
		printf("power: battery at %d%%, stopping the charger at the %d%% limit\n", percent, g_charge_limit);
		charger_run(false);
		g_charging_suspended = true;
	} else if (g_charging_suspended && percent <= g_charge_limit - 3) {
		printf("power: battery back to %d%%, charging again\n", percent);
		charger_run(true);
		g_charging_suspended = false;
	}
}

// Forbids or allows charging outright, on top of whatever the percentage limit
// is doing. This is the one place that knows the full sequence the charger
// needs -- three nodes, in order, with the stock player's own values -- and
// callers have no business rediscovering it: writing charge_control_limit_max
// alone does nothing useful, and writing 0 into it means *fast charge*, not
// stop.
void power_set_charging_allowed(bool allowed) {
	find_charge_node();
	g_charging_blocked = !allowed;

	if (!g_charge_node) {
		printf("power: charging cannot be controlled on this firmware\n");
		return;
	}

	if (!allowed) {
		charger_run(false);
		g_charging_suspended = true;
		printf("power: charging forbidden\n");
	} else {
		charger_run(true);
		g_charging_suspended = false;
		printf("power: charging allowed again\n");
		apply_charge_limit(); // and let the percentage limit have its say
	}
}

// ---------------------------------------------------------------------------
// automatic shutdown
// ---------------------------------------------------------------------------

static bool g_auto_off_enabled;
static uint32_t g_auto_off_ms;

// ---------------------------------------------------------------------------
// Why a naive suspend-to-RAM does not work here
//
// Of the whole audio chain, the only module without suspend/resume callbacks is
// soc_aic, the I2S controller -- a block inside the SoC, so precisely the one
// that loses its registers when the SoC powers down. Nothing reprograms it on
// resume, and the first snd_pcm_open opens a device whose driver believes it is
// configured and whose hardware is not, which takes the device down. Reloading
// the module is no way out either: `rmmod soc_aic` followed by `insmod` crashes
// the device even cold, with no suspend involved, and soc_aic.ko exposes no
// power parameters.
//
// So most of the saving comes from cutting wake-ups -- screen off, Wi-Fi
// parked, Bluetooth down, inline-remote ADC stopped, timers paused -- and the
// suspend sequence further down tears the audio context all the way down before
// writing `mem`.
// ---------------------------------------------------------------------------

void power_set_auto_off(bool enabled, uint32_t minutes) {
	g_auto_off_enabled = enabled;
	g_auto_off_ms = minutes * 60u * 1000u;
}


static bool g_double_tap_wake; // rearmed at every blank; see power_set_double_tap_wake

// ---------------------------------------------------------------------------
// The inline-remote ADC
//
// /root/work/rootfs/rootfs/module_driver/sa_earpods_adc.sh loads its driver
// with sea_poll_ms=10: the analogue line that reads the three buttons on a
// headphone cable's remote is sampled a hundred times a second, for as long as
// the device is on, and it holds regulator aldo4 while it does. Nothing here
// reads the remote, so that is a hundred wake-ups a second and a regulator held
// up for nothing.
//
// The node's exact path is a runtime discovery -- the stock player shells out
// to `find /sys/devices/platform/earpods_adc/earpods_adc -name earpods_adc_sw`
// -- so this looks for it the same way, once.
// ---------------------------------------------------------------------------

#define EARPODS_ROOT "/sys/devices/platform/earpods_adc"

static bool earpods_find(char *out, size_t out_size, const char *dir, int depth) {
	if (depth > 4) {
		return false;
	}

	DIR *d = opendir(dir);
	if (!d) {
		return false;
	}

	bool found = false;
	struct dirent *entry;
	while (!found && (entry = readdir(d)) != NULL) {
		if (entry->d_name[0] == '.') {
			continue;
		}

		char path[512];
		snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);

		if (strcmp(entry->d_name, "earpods_adc_sw") == 0) {
			snprintf(out, out_size, "%s", path);
			found = true;
			break;
		}

		struct stat info;
		if (lstat(path, &info) == 0 && S_ISDIR(info.st_mode)) {
			found = earpods_find(out, out_size, path, depth + 1);
		}
	}

	closedir(d);
	return found;
}

static void earpods_adc_set(bool on) {
	static char node[512];
	static bool looked;

	if (!looked) {
		looked = true;
		if (!earpods_find(node, sizeof(node), EARPODS_ROOT, 0)) {
			node[0] = '\0';
		}
	}

	if (!node[0]) {
		return; // not this firmware
	}

	FILE *f = fopen(node, "w");
	if (!f) {
		return;
	}
	fputs(on ? "on" : "off", f);
	fclose(f);
	printf("power: inline-remote ADC %s (%s)\n", on ? "on" : "off", node);
}

// ---------------------------------------------------------------------------
// Parking the radio
//
// The stock player switches Wi-Fi off before every suspend (0x47f730 ->
// system_if_wifi_turn_off, and it waits up to a second for the confirmation)
// and switches it back on afterwards if the setting says so. An associated
// CYW43438 is 20-40 mA of the standby budget, and the SDIO rail it hangs off
// is deliberately kept alive across a suspend (msc0_keep_power_in_suspend=y),
// so nothing else is going to save it.
//
// Not on every screen-off, though: the screen goes dark after thirty seconds
// and a pocket-check a minute later would find the radio re-associating for no
// reason. Only after the screen has been dark this long with nothing playing.
//
// Sixty seconds, the same wait the suspend uses, and the same wait for both
// radios: a minute after the screen goes dark, anything nobody is using goes
// off -- Bluetooth first, then Wi-Fi, then the SoC itself if the Standby toggle
// allows it. The radios do not wait for that toggle, and with them off the
// suspend becomes possible in the first place.
#define RADIO_PARK_AFTER_MS_DEFAULT (60 * 1000)
static uint32_t g_radio_park_after_ms = RADIO_PARK_AFTER_MS_DEFAULT;

// No settings switch for this: parking is always on, and the two in_use() tests
// below are the only places that decide when not to switch off.
static bool g_wifi_parked; // this module switched it off, so it switches it back on
static bool g_bt_parked;

// Whether what is playing right now comes off the network. The download flags
// tested by wifi_in_use() do not cover it: they are raised while a fetch is
// wanted and dropped as soon as the bytes are in, so an album whose next track
// is already cached looks exactly like an idle device -- and parking the radio
// there means the track after that never arrives. What decides is who owns the
// file being played.
//
// radio_is_playing(), not radio_is_active(): a station merely loaded and
// stopped uses nothing, and counting it as in-use would leave the radio on and
// the automatic shutdown disabled for the rest of the day after one listen.
static bool streaming_playback_active(void) {
	if (radio_is_playing()) {
		return true;
	}
	if (audio_get_status() == AUDIO_STATUS_STOPPED) {
		return false;
	}

	char path[512];
	audio_get_current_file(path, sizeof(path));
	if (!path[0]) {
		return false;
	}
	return qobuzcache_owns(path) || tidalcache_owns(path) || podcastcache_owns(path);
}

// Whether the network is the point rather than an idle drain: an AirPlay
// receiver waiting to be found, a DLNA server, a browser halfway through an
// upload, a station streaming, a Qobuz/Tidal/podcast fetch in flight.
//
// The *_network_wanted() halves matter as much as the downloading ones: between
// the end of one track and the first bytes of the next there is an HTTPS request
// during which nothing plays and nothing downloads, and parking the radio in
// that gap stops the next track from ever arriving.
static bool wifi_in_use(void) {
	// A scan or a connect in flight counts as use: parking the radio out from
	// under wpa_supplicant mid-association leaves the driver to sort out a
	// teardown it did not expect.
	return wifi_busy() || wifi_scan_running() || streaming_playback_active() || airplay_get_enabled() ||
		   dlna_get_enabled() || sonixlink_get_enabled() ||
		   wifitransfer_get_enabled() || qobuzcache_downloading_id() > 0 ||
		   qobuzcache_network_wanted() || tidalcache_downloading_id() > 0 || tidalcache_network_wanted() ||
		   podcastcache_downloading_id() > 0 || podcastcache_network_wanted();
}

// Pushes the next suspend attempt out by the anti-hammer delay. A radio that has
// just been told to go off is still going off: writing `mem` while the SDIO
// chip's teardown is in flight leaves every suspend failing with the main thread
// stuck in D on wait_on_page_bit. The park and the suspend both fall due sixty
// seconds after the screen goes dark, so without this they would land on the
// same tick.
static void mem_defer_after_radio_change(void);

// Takes Wi-Fi down and records that this module did it, so the same module puts
// it back. Shared by the idle timer below and the suspend path.
static void park_wifi_now(void) {
	if (g_wifi_parked || !wifi_get_enabled()) {
		return;
	}
	g_wifi_parked = true;
	printf("power: Wi-Fi off\n");
	wifi_set_enabled_transient(false);
	mem_defer_after_radio_change();
}

// Bluetooth's own "the point rather than an idle drain": a sink being written
// to, a link that is up at all, a scan or a pairing in flight. A radio with
// nothing on the other end of it is worth exactly as much off as on.
static bool bluetooth_in_use(void) {
	return bluetooth_audio_active() || bluetooth_connected_device(NULL) || bluetooth_scan_running() ||
		   bluetooth_busy();
}

// Whether the player has anything on. This is what holds the shutdown timer at
// zero, so it is a list of things the player is DOING, which is not the same
// list as "the radio has to stay up".
//
// A pair of headphones merely connected is not on it: a silent link is exactly
// the state the shutdown timer exists for. Nor is an AirPlay or DLNA server
// switched on but idle -- those hold the Wi-Fi radio (see wifi_in_use) without
// holding the device awake, and counting them here would let one forgotten
// toggle disable the automatic shutdown for good.
//
// A key pressed with the screen dark counts. Skipping tracks in a pocket is the
// user doing something, and switching off under their fingers is not what a
// shutdown timer is for.
static bool player_is_busy(bool playing, uint32_t input_idle) {
	return playing ||							   // a track or a station
		   input_idle < POWER_TICK_MS * 4 ||	   // a key just pressed
		   usbdac_is_active() ||				   // a computer feeding the DAC
		   usb_storage_active() ||				   // the card exported over USB
		   gearboy_running() ||					   // the emulator
		   library_scan_running() ||			   // a scan, which is also writing to the card
		   audiobookdb_scan_running() || wifitransfer_running() || dlna_running() || airplay_running() ||
		   sonixlink_is_connected() || // a phone driving the player, not a switch left on
		   qobuzcache_downloading_id() > 0 || tidalcache_downloading_id() > 0 || podcastcache_downloading_id() > 0;
}

static void park_bluetooth_now(void) {
	if (g_bt_parked || !bluetooth_get_enabled()) {
		return;
	}
	g_bt_parked = true;
	printf("power: Bluetooth off\n");
	bluetooth_set_enabled_transient(false);
	mem_defer_after_radio_change();
}

// Everything that holds the Wi-Fi radio up.
//
// The Bluetooth half of it is not a mistake: the two radios sit on one chip.
// Taking Wi-Fi down mid-teardown is the hazard described over park_wifi_now(),
// and `ifconfig wlan0 down` -- which is the whole of the firmware's
// wifi_off.sh -- takes bcmdhd through dhd_stop() on the shared part.
//
// It asks about the LINK and not about sound going over it. With the screen
// already dark and the album finishing, "playing" goes false the instant the
// last track ends, and the very next pass -- 250 ms later -- would otherwise
// park the Wi-Fi radio out from under a live pair of headphones. The cost is
// the Wi-Fi radio staying associated while headphones are connected and quiet;
// the alternative is dropping the link the user is wearing.
static bool wifi_wanted(void) {
	return wifi_in_use() || bluetooth_busy() || bluetooth_audio_active() || bluetooth_connected_device(NULL);
}

// A minute after a radio stops being wanted, it goes off. Not "unless something
// is playing", not "unless the Standby toggle is on" -- a radio with nothing on
// the other end of it is worth exactly as much off as on, and music coming out
// of the jack is no reason to keep the Wi-Fi associated.
//
// The minute runs from the later of two moments: the screen going dark, and the
// radio falling idle. Running it from the screen alone would make the wait
// vanish whenever the two do not coincide -- pausing an internet station an hour
// into a dark screen would take the Wi-Fi down on the next pass. The wait exists
// so that a pause somebody is about to undo does not cost a reassociation.
static uint32_t g_wifi_idle_since;
static uint32_t g_bt_idle_since;

static void park_radios_if_idle(uint32_t now) {
	// Held at now for as long as there is a reason to keep them, so `since`
	// below is "how long has nobody wanted this".
	if (g_screen_on || bluetooth_in_use()) {
		g_bt_idle_since = now;
	}
	if (g_screen_on || wifi_wanted()) {
		g_wifi_idle_since = now;
	}
	if (g_screen_on) {
		return;
	}

	// One radio per pass. Switching a radio off is a job posted to its worker,
	// and the flag this file reads flips before that job has run -- so parking
	// both in the same tick means writing to the SDIO chip while its neighbour's
	// teardown is still in flight, which wedges the suspend path. The next pass
	// is 250 ms away; the second radio can wait for it.
	if (!g_bt_parked && bluetooth_get_enabled() && since(g_bt_idle_since, now) >= g_radio_park_after_ms) {
		fprintf(stderr, "power: parking Bluetooth after %u ms unused\n", since(g_bt_idle_since, now));
		park_bluetooth_now();
		return;
	}

	if (g_wifi_parked || !wifi_get_enabled() || since(g_wifi_idle_since, now) < g_radio_park_after_ms) {
		return;
	}

	fprintf(stderr, "power: parking Wi-Fi after %u ms unused\n", since(g_wifi_idle_since, now));
	park_wifi_now();
}

static void unpark_radios(void) {
	if (g_wifi_parked) {
		g_wifi_parked = false;
		printf("power: Wi-Fi back on\n");
		wifi_set_enabled_transient(true);
	}
	if (g_bt_parked) {
		g_bt_parked = false;
		printf("power: Bluetooth back on\n");
		bluetooth_set_enabled_transient(true);
	}
}

// ---------------------------------------------------------------------------
// Suspend to RAM
//
// The stock firmware's standby writes `mem` 60 seconds after the screen goes
// dark, but only at the end of a precise sequence -- a real player stop, a wait
// for the stopped state, Wi-Fi and Bluetooth already dealt with, and only then
// the write. On resume it reuses nothing: the audio stream restarts from
// scratch (the CS43198's dai_shutdown/dai_prepare redo all the work).
//
// The same sequence is followed here, in the narrowest case:
//
//   * only with nothing playing (stopped or paused): no live music, no radio.
//     Pause is fine -- its PCM is already closed after 5 s and resuming reopens
//     everything from scratch. The only thing that crosses the suspend is RAM,
//     which is preserved.
//   * no live ALSA object: audio_suspend_freeze() also closes the PCM the
//     gapless path holds and waits for the audio thread to let go of everything.
//   * Wi-Fi parked first (an associated chip is 20-40 mA and the network would
//     die in the suspend anyway); with Bluetooth on it does not suspend at all,
//     because its resident daemons have no clean suspend path.
//   * not while the cable is carrying something: the card exported to a host,
//     a computer feeding the DAC, or an ADB session. A cable that is only
//     charging is not a reason to stay awake.
//   * sync() first: the removable microSD loses power in the suspend
//     (keep_power_in_suspend=0 in the driver's script), so anything bound for it
//     must already have got there.
//
// The wake is the write() returning: the power key and PC28 are wake sources
// registered by the stock drivers. On return the clock and the idle markers are
// realigned, and the output path is forced to re-init so the resumed hardware
// is reprogrammed before the first PCM is opened.
//
// Switched off with [power] standby_mem = 0; the wait is set by
// [power] standby_mem_seconds (60 by default, like the stock firmware).
// ---------------------------------------------------------------------------

static bool g_mem_enabled;
// Raised by a suspend that failed or that came straight back. It stops the
// countdown for THIS dark-screen spell only, and power_screen_on() clears it, so
// standby is never left silently disabled for the rest of the session.
static bool g_mem_blocked_this_dark;
static uint32_t g_mem_after_ms;
static int g_mem_instant_wakes;		// consecutive 0-1 s wakes: something wakes it at once
static int g_mem_instant_wakes_max; // past this many, standby blocks itself
static uint32_t g_mem_last_attempt_ms; // to avoid hammering while a condition blocks
static char g_mem_skip_logged[96];	   // the last skip reason already written to the log

// Standby, toggled at runtime from the power settings. Still tied to
// /sys/power/state being available: on the simulator that path is NULL, so
// standby stays off regardless. Switching it on here also clears a block left by
// a failed suspend, since it has been asked for explicitly.
void power_set_standby_enabled(bool enabled) {
	g_mem_enabled = enabled && g_cfg.power_state_path != NULL;
	g_mem_blocked_this_dark = false;
	g_mem_instant_wakes = 0;
}

// What the kernel says about the last suspend: counters and, above all, the
// device or the step that made the attempt fail. It lives in debugfs, which may
// not be mounted on this firmware, so both known paths are tried.
static void log_suspend_stats(void) {
	static const char *paths[] = {"/sys/kernel/debug/suspend_stats", "/d/suspend_stats"};
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		FILE *f = fopen(paths[i], "r");
		if (!f) {
			continue;
		}
		char line[128];
		printf("power: mem: %s:\n", paths[i]);
		while (fgets(line, sizeof(line), f)) {
			printf("power: mem:   %s", line);
		}
		fclose(f);
		return;
	}
	printf("power: mem: suspend_stats not readable (debugfs not mounted?)\n");
}

static void mem_defer_after_radio_change(void) { g_mem_last_attempt_ms = lv_tick_get(); }

// One log line per skipped suspend, but only one per reason: on a 250 ms tick
// the same reason would otherwise be logged four times a second for hours.
static void mem_skip(const char *why) {
	if (strncmp(g_mem_skip_logged, why, sizeof(g_mem_skip_logged) - 1) != 0) {
		snprintf(g_mem_skip_logged, sizeof(g_mem_skip_logged), "%s", why);
		printf("power: mem deferred: %s\n", why);
	}
	// Stamping the attempt means the next try is 30 s away, not immediate.
	g_mem_last_attempt_ms = lv_tick_get();
}

// ---------------------------------------------------------------------------
// The RTC wake alarm that carries the automatic shutdown across a suspend.
//
// The process does not run while suspended: a shutdown deadline falling during
// the sleep would go unseen, and the wake (always by the user) restarts the idle
// markers, so with standby on the shutdown would never fire at all. The hardware
// is there, though: the Ingenic RTC is a registered wake source
// (system-power-controller, RTCLDO powered across the suspend).
//
// So before writing `mem`, if the automatic shutdown is on,
// /sys/class/rtc/rtc0/wakealarm is programmed for the moment it would fire. If
// the RTC is what wakes the device (no user: the deadline arrived), it really
// shuts down. If the user wakes it first, turning the screen on disarms the
// alarm, and the next suspend re-arms it with the new deadline.
// ---------------------------------------------------------------------------
#define RTC_WAKEALARM_PATH "/sys/class/rtc/rtc0/wakealarm"

static bool g_rtc_alarm_armed;
static time_t g_rtc_alarm_at;
// The same deadline on CLOCK_BOOTTIME. The wall clock is not usable for this:
// it does not count the sleep, and clock_resync() -- the one thing that would
// fix it -- refuses to do anything until the date has been set by hand or by
// ntpdate, so on a device that has seen neither the deadline check never comes
// true. CLOCK_BOOTTIME counts suspended time and needs nobody's permission.
static uint32_t g_rtc_alarm_boot_at;

static bool rtc_wakealarm_write(const char *val) {
#ifdef HOST_BUILD
	(void)val;
	return false; // never touch the development machine's RTC
#else
	FILE *f = fopen(RTC_WAKEALARM_PATH, "w");
	if (!f) {
		return false;
	}
	fputs(val, f);
	return fclose(f) == 0;
#endif
}

static void rtc_alarm_disarm(void) {
	if (!g_rtc_alarm_armed) {
		return;
	}
	g_rtc_alarm_armed = false;
	rtc_wakealarm_write("0");
	printf("power: RTC wake alarm disarmed\n");
}

static void rtc_alarm_arm_for_auto_off(void) {
	g_rtc_alarm_armed = false;
	// Cleared on every path, not only when a new one is written: an alarm left
	// over from an earlier suspend outlives the setting that armed it, and fires
	// later to wake a device that has no reason to wake -- with the software
	// flag down, so nothing acts on it and it simply burns battery in the dark.
	if (!g_auto_off_enabled || g_auto_off_ms == 0) {
		rtc_wakealarm_write("0");
		return;
	}
	uint32_t idle_wall = boottime_ms() - g_last_active_boot_ms;
	if (idle_wall >= g_auto_off_ms) {
		rtc_wakealarm_write("0");
		return; // already expired: the normal awake tick handles it
	}
	long remaining_s = (long)((g_auto_off_ms - idle_wall) / 1000u) + 5; // margin
	// Cleared first (the kernel rejects a new alarm over a pending one), then
	// the absolute time in epoch seconds, which is how the RTC keeps it.
	if (!rtc_wakealarm_write("0")) {
		return;
	}
	char buf[32];
	time_t at = time(NULL) + remaining_s;
	snprintf(buf, sizeof(buf), "%lld", (long long)at);
	if (!rtc_wakealarm_write(buf)) {
		return;
	}
	g_rtc_alarm_armed = true;
	g_rtc_alarm_at = at;
	g_rtc_alarm_boot_at = boottime_ms() + (uint32_t)remaining_s * 1000u;
	printf("power: mem: RTC wake in %ld s for the automatic shutdown\n", remaining_s);
}

// The actual shutdown, shared by the normal awake tick and the RTC alarm path
// just after a resume.
static void power_auto_off_now(const char *why) {
	printf("power: automatic shutdown: %s\n", why);
	fflush(stdout);
	// Where the music had got to: this path never goes through the interface,
	// which is what normally writes it.
	device_state_remember_flush();
	clock_shutdown(); // the RTC gets the time before the power goes
	qobuzcache_clear_on_exit(); // the cache does not survive a shutdown
	tidalcache_clear_on_exit();
	podcastcache_clear_on_exit();
	dlna_clear_on_exit();
	sync();
	reboot(RB_POWER_OFF);
}

static void suspend_to_ram(void) {
	const char *path = g_cfg.power_state_path;

	printf("power: mem: starting the sequence (screen off for %u s)\n",
		   (unsigned)((lv_tick_get() - g_screen_off_at) / 1000));

	// 1. The freeze: a real stop, waited on until the track context is gone --
	//    play_file() returned, decoder and FILE* closed, gapless empty, PCM
	//    closed. Quiescing the ALSA objects is not enough: a paused track keeps
	//    the context alive, the card loses power in the suspend, and the first
	//    play after the wake (an unpause of that context) reboots the device. If
	//    there was a pause, the freeze arms a fresh restart: the next play
	//    rebuilds everything from scratch at the right position.
	if (!audio_suspend_freeze(3000)) {
		mem_skip("the audio context did not stop");
		return;
	}

	// 2. The time written where it survives: the RTC domain stays powered in
	//    suspend (RTCLDO at 1.8 V), but the copy in config is the insurance.
	clock_remember();

	// 3. The radio is already down: suspend_if_idle() switches it off a tick
	//    earlier on purpose -- never write `mem` with the SDIO teardown halfway.

	// 4. The card loses power in suspend, so nothing may be left in flight
	//    towards it.
	sync();

	// 4-ter. The RTC alarm: if the automatic shutdown deadline falls during the
	//        sleep, it is what wakes the device to actually shut down.
	rtc_alarm_arm_for_auto_off();

	// 5. The write that stops the SoC. Blocks until the wake: everything after
	//    it already runs on the other side.
	uint32_t before_wall = boottime_ms();
	printf("power: mem: writing '%s'\n", path);
	fflush(stdout);
	FILE *f = fopen(path, "w");
	if (!f) {
		printf("power: mem: %s will not open (%s) -- prototype off for this session\n", path, strerror(errno));
		g_mem_enabled = false;
		rtc_alarm_disarm(); // armed a moment ago for a sleep that never began
		return;
	}
	fputs("mem", f);
	int rc = fclose(f);
	int write_errno = errno;
	uint32_t slept_s = (boottime_ms() - before_wall) / 1000;

	// --- awake from here on ---
	// The line is flushed at once: if the wake ends badly, the log must at least
	// record that the suspend itself succeeded.
	printf("power: mem: awake after %u s (fclose=%d)\n", slept_s, rc);
	fflush(stdout);

	// There is no "one suspend per dark-screen spell" rule: the countdown simply
	// starts again, like any other countdown, and the instant-wake counter below
	// is what guards against a device that wakes itself in a loop.

	// Before the first play opens a PCM: force the output route to re-init. The
	// suspend powers the HBC3000 down and its kernel resume is a no-op, but the
	// software route survives, so auto_set_output() would see "X == X" and skip
	// hbc3000_enable(). A real X->Y->X on the mixer forces the chip's power-on
	// again, without which the first play after a standby reboots the device.
	//
	// And this is the whole of what a resume owes the audio path. Volume, gain,
	// digital filter, DRE and NOS are not rewritten because they are not lost:
	// the AXP2101 holds every rail at its working voltage through "mem"
	// (suspend_volage == work_voltage on all four DCDCs) and the codec driver's
	// PM callbacks are empty, so the CS43198 keeps its registers. See the note
	// on alsa_controls_reapply() in alsa-controls.c.
	if (rc == 0) {
		audio_force_output_reinit_after_resume();

		// And the same courtesy to the radio. The daemons came through the
		// suspend -- they are processes, nothing suspended them -- but the
		// chip under them did not, so without this the stack looks up and hears
		// nothing, and the headphones never come back. A radio that was parked
		// before the suspend is left to unpark_radios(), which rebuilds it
		// anyway.
		if (!g_bt_parked) {
			bluetooth_notify_resume();
		}
	}

	// The verdict. Never insist: a failed suspend followed by a retry a minute
	// later costs thirty seconds of freeze every round, which from outside looks
	// like a device that will no longer switch on. A failing suspend blocks
	// itself and leaves the device usable -- the diagnosis in the log is worth
	// more than an identical second attempt.
	if (rc != 0) {
		printf("power: mem: the suspend FAILED (%s) -- no further attempts while the screen stays off\n",
			   strerror(write_errno));
		log_suspend_stats();
		g_mem_blocked_this_dark = true;
		rtc_alarm_disarm(); // no stray alarm left over from a sleep that never began
		fflush(stdout);
	} else if (slept_s < 2) {
		// Succeeded but lasted no time: something woke it immediately. Once can
		// be chance; several in a row is a wake source that has to be found
		// rather than fought.
		g_mem_instant_wakes++;
		printf("power: mem: instant wake (%d of %d tolerated)\n", g_mem_instant_wakes,
			   g_mem_instant_wakes_max);
		if (g_mem_instant_wakes >= g_mem_instant_wakes_max) {
			printf("power: mem: too many instant wakes -- no further attempts while the screen stays off\n");
			log_suspend_stats();
			g_mem_blocked_this_dark = true;
		}
		fflush(stdout);
	} else {
		g_mem_instant_wakes = 0;
	}

	// The system clock did not count the sleep; the RTC did.
	clock_resync();

	// If the RTC alarm is what woke the device there is no user: the automatic
	// shutdown deadline arrived, so it shuts down here, before the idle markers
	// restart. A user wake before the deadline leaves the alarm armed, and
	// turning the screen on disarms it a moment later.
	//
	// The deadline is read off CLOCK_BOOTTIME rather than the wall clock (see
	// g_rtc_alarm_boot_at), and a queued power press counts as a user: the
	// disarm below only runs on the next tick, so without that test a press
	// within a couple of seconds of the deadline would switch the device off in
	// the user's hand.
	pthread_mutex_lock(&g_lock);
	bool user_woke_it = g_power_button_pending;
	pthread_mutex_unlock(&g_lock);

	if (rc == 0 && g_rtc_alarm_armed && !user_woke_it && (int32_t)(boottime_ms() - g_rtc_alarm_boot_at) >= 0) {
		power_auto_off_now("RTC wake after mem: the idle time has run out");
	}

	// No recovery of the old stream: there is nothing left to recover -- the
	// freeze closed everything before the suspend, and the next play is a fresh
	// start already armed.

	// A full idle wait again before the next sleep, and nothing else. The
	// automatic shutdown's own marker is deliberately left alone: the wake that
	// matters here is the RTC's, which is no sign of life, and restamping it
	// would push the shutdown out by a whole period on every suspend. That
	// shutdown counts on CLOCK_BOOTTIME, which counted the sleep; a real user
	// wake stamps the marker through power_screen_on().
	g_mem_active_ms = lv_tick_get();
	g_screen_off_at = lv_tick_get();
	g_mem_last_attempt_ms = lv_tick_get();
	g_mem_skip_logged[0] = '\0';
}

// Every condition in one place, each of them logged, so the log explains why
// the device is not sleeping.
static void suspend_if_idle(uint32_t now, bool playing) {
	if (!g_mem_enabled || g_mem_blocked_this_dark || !g_cfg.power_state_path || g_screen_on) {
		return;
	}
	if (since(g_mem_active_ms, now) < g_mem_after_ms) {
		return; // not yet the full idle wait (see g_mem_active_ms)
	}
	if (since(g_mem_last_attempt_ms, now) < 30 * 1000) {
		return; // the last attempt gave up: do not hammer
	}
	if (playing) {
		mem_skip("music is playing");
		return;
	}
	// Pause counts as a stopped state: the PCM is already released after five
	// seconds ("DAC powered down" in the log) and resuming always reopens the
	// device from scratch, route included, which is the path every unpause
	// exercises. What must not cross the suspend is a live ALSA object, not a
	// remembered position: that sits in RAM, and RAM is preserved.
	if (wifi_in_use()) {
		mem_skip("something is using the network");
		return;
	}
	// Busy first, so the log tells the truth: bluetooth_in_use() counts a job in
	// flight as use, and right after the park that job is this file's own.
	if (bluetooth_busy()) {
		mem_skip("Bluetooth is still shutting down");
		return;
	}
	if (bluetooth_in_use()) {
		mem_skip("something is using Bluetooth");
		return;
	}
	if (bluetooth_get_enabled()) {
		// Enabled but idle: the park takes it down now and the suspend goes on
		// the next round, the same way Wi-Fi is handled below.
		park_bluetooth_now();
		mem_skip("Bluetooth just switched off: mem starts on the next round");
		return;
	}
	// The cable itself is not a reason -- a device on a desk charger should
	// still sleep -- but what the cable is carrying is: the card exported over
	// USB, or a computer feeding the DAC, both of which the suspend would cut.
	if (usb_storage_active() || usbdac_is_active()) {
		mem_skip("the USB cable is transferring something");
		return;
	}
	// And ADB, which is the same cable carrying something. adbd survives the
	// suspend as a process, but the gadget goes down with the SoC and the host
	// loses the device: a shell left open on a desk dies at the sixty-second
	// mark, and from the outside that is "ADB switches itself off when the
	// screen goes dark". The cable has to be there -- ADB left on with nothing
	// plugged in is a setting, not a connection, and must not keep a device in
	// a pocket awake all night.
	//
	// adb_is_running() walks /proc, so it is asked only once the cable has
	// already answered yes.
	if (usb_vbus_present() && adb_is_running()) {
		mem_skip("ADB is on with the cable plugged in");
		return;
	}
	if (wifi_get_enabled()) {
		// Wi-Fi goes off now, but the suspend waits for a later tick: the SDIO
		// chip's teardown is asynchronous, and writing `mem` right after the
		// radio goes down leaves every suspend failing with the main thread
		// stuck in D on wait_on_page_bit. Thirty seconds between the two gives
		// the driver time to settle.
		park_wifi_now();
		mem_skip("Wi-Fi just switched off: mem starts on the next round");
		return;
	}

	suspend_to_ram();
}

// ---------------------------------------------------------------------------
// What still runs with the panel dark
//
// Pausing the refresh timer and the animation timer at the blank is not enough
// on its own. These keep firing otherwise, and each firing wakes the CPU:
//
//   - LVGL's input-read timer, every 30 ms. lv_indev_enable(false) makes the
//     callback return immediately, but the timer still fires, and every firing
//     is a full pass of lv_timer_handler() and a return trip through the main
//     loop. Thirty-three wake-ups a second to decide there is nothing to read.
//   - the status bar's clock, radio and battery timers, painting a bar nobody
//     can see;
//   - the player page's progress timer, moving a slider nobody can see;
//   - the screensaver's own clock, redrawing a screensaver that is already in
//     the framebuffer and is not being scanned out.
//
// None of them has any effect while the panel is off, because nothing is
// presented. So they are all paused at the blank and started again at the
// wake, where each is also made ready so it runs once immediately rather than
// showing a stale value for a whole period.
//
// A page registers its timer here once and then forgets about it; power.c
// never looks at what the timer does.
// ---------------------------------------------------------------------------
#define STANDBY_TIMERS_MAX 16
static lv_timer_t *g_standby_timers[STANDBY_TIMERS_MAX];
// 0 = paused outright while the panel is dark. Anything else = kept running,
// at that period instead of its own. A timer that only paints can be stopped;
// one that also carries state the device needs in standby -- the battery poll
// drives the status LED, and the LED has to go out on its own after twenty
// seconds and turn red on a charger -- must merely be slowed down.
static uint32_t g_standby_timer_slow_ms[STANDBY_TIMERS_MAX];
static uint32_t g_standby_timer_normal_ms[STANDBY_TIMERS_MAX];
// Whether each one was actually running when the panel went dark. A registered
// timer is not necessarily live -- the screensaver's clock only runs while the
// screensaver is up -- and resuming one that was deliberately stopped would
// quietly start it painting a hidden page for ever.
static bool g_standby_timer_was_running[STANDBY_TIMERS_MAX];
static int g_standby_timer_count;

static void standby_register(lv_timer_t *timer, uint32_t slow_ms) {
	if (!timer) {
		return;
	}
	for (int i = 0; i < g_standby_timer_count; i++) {
		if (g_standby_timers[i] == timer) {
			g_standby_timer_slow_ms[i] = slow_ms; // last word wins
			return;
		}
	}
	if (g_standby_timer_count >= STANDBY_TIMERS_MAX) {
		fprintf(stderr, "power: no room to register another standby timer\n");
		return;
	}
	g_standby_timers[g_standby_timer_count] = timer;
	g_standby_timer_slow_ms[g_standby_timer_count] = slow_ms;
	g_standby_timer_count++;
}

void power_pause_in_standby(lv_timer_t *timer) { standby_register(timer, 0); }

void power_slow_in_standby(lv_timer_t *timer, uint32_t standby_period_ms) {
	standby_register(timer, standby_period_ms ? standby_period_ms : 1);
}

// LVGL's input-read timers, one per input device.
static void indev_timers_set_paused(bool paused) {
	lv_indev_t *indev = lv_indev_get_next(NULL);
	while (indev) {
		lv_timer_t *t = lv_indev_get_read_timer(indev);
		if (t) {
			if (paused) {
				lv_timer_pause(t);
			} else {
				lv_timer_resume(t);
			}
		}
		indev = lv_indev_get_next(indev);
	}
}

static void standby_timers_set_paused(bool paused) {
	for (int i = 0; i < g_standby_timer_count; i++) {
		lv_timer_t *t = g_standby_timers[i];
		if (!t) {
			continue;
		}

		if (g_standby_timer_slow_ms[i] != 0) {
			// Slowed, not stopped.
			if (paused) {
				g_standby_timer_normal_ms[i] = t->period; // opaque type, reached via lv_timer_private.h
				lv_timer_set_period(t, g_standby_timer_slow_ms[i]);
			} else if (g_standby_timer_normal_ms[i]) {
				lv_timer_set_period(t, g_standby_timer_normal_ms[i]);
				lv_timer_ready(t); // catch up at once on the way back
			}
			continue;
		}

		if (paused) {
			g_standby_timer_was_running[i] = !lv_timer_get_paused(t);
			if (g_standby_timer_was_running[i]) {
				lv_timer_pause(t);
			}
		} else if (g_standby_timer_was_running[i]) {
			lv_timer_resume(t);
			lv_timer_ready(t); // do not show a stale clock for a whole period
		}
	}
}

// Called on the UI thread around a screen transition, and returning true if it
// took over the frame. The GUI hangs the screensaver off it; nothing here knows
// what it does.
static bool (*g_wake_hook)(void);

void power_set_wake_hook(bool (*cb)(void)) { g_wake_hook = cb; }

void power_screen_off(void) {
	waveform_set_screen_on(false); // told, but not a reason to stop -- see waveform.h
	if (!g_screen_on) {
		return;
	}
	g_screen_on = false;
	g_screen_off_at = lv_tick_get();
	// The screen going dark opens a new idle window: the standby countdown
	// starts here, unless activity keeps pushing it back.
	g_mem_active_ms = g_screen_off_at;

	// Written right before the blank, either way round: the touch driver reads
	// this flag when the blank reaches it, and chooses doze or a full
	// power-off from it. Off has to be written too, not merely left as the
	// settings page last put it, or the controller dozes on a night when
	// nothing is listening for the gesture.
	power_set_double_tap_wake(g_double_tap_wake);

	set_indevs_enabled(false); // no touch events into LVGL while the panel is down
	fade_to(BRIGHTNESS_MIN);   // smooth dim down
	screen_power(false);

	// With the panel dark, draw what the next wake should open on straight
	// into the framebuffer. Unblanking presents whatever the framebuffer
	// already holds, and it holds the last thing drawn, so without this the
	// page underneath flashes up before the screensaver however early the wake
	// hook runs. Done here it costs one invisible frame and the wake shows the
	// right thing from its very first scan-out.
	if (g_wake_hook && g_wake_hook()) {
		lv_obj_invalidate(lv_layer_top());
		lv_obj_t *scr = lv_screen_active();
		if (scr) {
			lv_obj_invalidate(scr);
		}
		lv_refr_now(g_disp);
	}

	// Stop rendering entirely while the panel is dark (physical buttons live
	// on their own threads, so they are still handled by the state machine).
	if (g_refr_timer) {
		lv_timer_pause(g_refr_timer);
	}

	// And stop reading input through LVGL. The touch controller is powered down
	// with the panel and the double-tap wake arrives on the button threads, so
	// this timer has nothing left to do.
	indev_timers_set_paused(true);

	// Everything that only paints: the status bar, the progress slider, the
	// screensaver's clock.
	standby_timers_set_paused(true);

	// And stop the animations with it. LVGL's animation timer runs every ~33 ms
	// for as long as any animation is alive, and the scrolling titles in the
	// player and the control centre are set to repeat for ever -- thirty
	// wake-ups a second computing positions for text nobody can see. They pick
	// up again on the way back.
	lv_timer_t *anim_timer = lv_anim_get_timer();
	if (anim_timer) {
		lv_timer_pause(anim_timer);
	}

	led_set_standby(true); // the LED standby option keys off this

	// The screen going dark is the natural moment to write the time down: a
	// battery pulled out while the device sits in standby then costs a few
	// seconds of clock, not however long was left on the slow timer.
	clock_remember();

	fprintf(stderr, "power: screen off\n");
}

void power_screen_on(void) {
	waveform_set_screen_on(true);
	if (g_screen_on) {
		return;
	}
	g_screen_on = true;

	// And disarms the automatic-shutdown RTC alarm: there is a user. The
	// shutdown timer goes back to zero here and stays there until the screen is
	// dark again -- that is what makes it a shutdown timer rather than a
	// stopwatch that happens to notice the screen.
	rtc_alarm_disarm();
	g_last_active_boot_ms = boottime_ms();

	// A new dark spell gets a clean slate: whatever went wrong with the last
	// suspend, the countdown starts again the next time the screen goes off.
	g_mem_blocked_this_dark = false;
	g_mem_instant_wakes = 0;

	// Whatever the screen coming back is for, the radios are probably part of
	// it. Asked for first, because associating and rebuilding a Bluetooth
	// stack take seconds, and they may as well be the seconds the panel spends
	// waking up.
	unpark_radios();

	// The level the screen comes back at is the one the user chose, however dim.
	// No too-dim floor is applied here: that guess belongs at init, where the
	// stored level is adopted from the panel (see power_init). Applied here it
	// would overwrite a deliberate setting on every screen-off/on, while a
	// reboot -- which reads the setting and never passes through here -- would
	// honour it.

	// The clock first: the screen is about to show it, and a screen-off spell
	// may well have included a suspend the system clock did not count.
	clock_resync();

	// Before anything is drawn: whatever wants to own the first frame of the
	// wake (the screensaver) is raised here, while the panel is still blank.
	// Called after the wake repaint, it would show the page underneath for a
	// split second before appearing.
	if (g_wake_hook) {
		(void)g_wake_hook();
	}

	screen_power(true);			 // panel, backlight and touch controller back up
	usleep(SCREEN_ON_SETTLE_US); // let the panel + backlight finish re-initializing
	set_indevs_enabled(true);
	indev_timers_set_paused(false);

	// The painting timers come back before the repaint below, and each runs
	// once straight away, so the first frame after the wake already has the
	// right time, the right battery and the right playback position on it.
	standby_timers_set_paused(false);

	// resume ongoing refresh (if it was paused) ...
	if (g_refr_timer) {
		lv_timer_resume(g_refr_timer);
	}
	lv_timer_t *anim_timer = lv_anim_get_timer();
	if (anim_timer) {
		lv_timer_resume(anim_timer);
	}
	// ... and force an immediate, full repaint of the framebuffer. The blank
	// cleared the panel to black, so the whole UI must be redrawn; this is not
	// gated on g_refr_timer, and lv_refr_now() draws even if the timer is paused.
	//
	// The sysfs blank/unblank cycle tears down the panel's scan-out, and on this
	// hardware the controller does not re-present the framebuffer just because
	// fresh pixels were copied into it -- it must be kicked explicitly. How
	// depends on which display path is active, so main.c owns the kick: it
	// restores the video mode (some drivers reset the page-flipping display's
	// double-height virtual resolution on blank) and re-arms scan-out around
	// this one wake repaint.
	display_wake_begin(g_disp);
	lv_obj_t *scr = lv_screen_active();
	if (scr) {
		lv_obj_invalidate(scr);
	}
	lv_obj_invalidate(lv_layer_top());
	lv_refr_now(g_disp);
	display_wake_end(g_disp);

	// The panel came back at its pre-blank (dim) level, and the backlight driver
	// may not have restored the configured brightness on unblank. Forcing the
	// tracked value to MIN and fading up guarantees every ramp step is written
	// to sysfs after the unblank and settle, so the backlight reliably returns.
	g_hw_brightness = BRIGHTNESS_MIN;
	fade_to(g_cfg.brightness); // smooth ramp back up over the freshly-drawn UI
	lv_display_trigger_activity(g_disp);

	led_set_standby(false);

	fprintf(stderr, "power: screen on (brightness=%ld)\n", g_cfg.brightness);
}

void power_toggle_screen(void) {
	if (g_screen_on) {
		power_screen_off();
	} else {
		power_screen_on();
	}
}

bool power_screen_is_on(void) { return g_screen_on; }

// Powers the Bluetooth chip down. Called once at startup, on a player whose
// Bluetooth switch is off: the init scripts leave the whole stack running even
// though nothing here talks to it.
void power_bluetooth_off(void) {
	if (g_bt_powered_down) {
		return;
	}
	g_bt_powered_down = true;
	fprintf(stderr, "power: switching off the Bluetooth the init script left running\n");
	bluetooth_power_down_stack();
}

static void power_timer_cb(lv_timer_t *timer) {
	(void)timer;
	uint32_t now = lv_tick_get();

	// 1. consume a queued power-button short-press
	bool btn;
	pthread_mutex_lock(&g_lock);
	btn = g_power_button_pending;
	g_power_button_pending = false;
	pthread_mutex_unlock(&g_lock);

	if (btn) {
		if ((int32_t)(now - g_ignore_button_until) >= 0) {
			power_toggle_screen();
		}
		power_notify_activity(); // a power press always counts as activity
		lv_display_trigger_activity(g_disp);
		now = lv_tick_get();
	}

	// 2. music playing keeps resetting the suspend clock (but not the screen
	//    clock -- the screen still dims while a track plays untouched)
	// A live stream goes out through audio_external_*, which audio.c's own
	// status knows nothing about -- so without the second half of this the
	// automatic shutdown would count a playing radio as an idle device and
	// switch it off mid-broadcast.
	//
	// The Bluetooth receiver is the third of those: the music is on a phone and
	// comes in over A2DP, so neither of the first two answers says anything
	// about it. Streaming rather than merely active, because the mode left on
	// with the phone paused is exactly the idle state the timer exists for.
	bool playing = (audio_get_status() == AUDIO_STATUS_PLAYING) || radio_is_playing() || btreceiver_is_streaming();
	if (playing) {
		g_last_playing_ms = now;
	}

	// 3. idle computations
	uint32_t input_idle = input_idle_ms(now);

	// Any activity pushes the standby countdown back: something playing, a key
	// pressed recently, or the cable changing state. Pause is not activity (the
	// count advances and the device eventually suspends), and the short gap
	// between two tracks is far too brief to reach the timeout, so a suspend
	// cannot fall in the middle of a track change.
	bool usb_now = usb_vbus_present();
	bool usb_changed = g_usb_present_known && usb_now != g_usb_present_last;
	if (playing || input_idle < POWER_TICK_MS * 2 || usb_changed) {
		g_mem_active_ms = now;
	}
	g_usb_present_last = usb_now;
	g_usb_present_known = true;

	// The automatic shutdown is a shutdown timer, and behaves like one: it is
	// held at zero for as long as the screen is on or the player is doing
	// something, and it counts only from the moment the screen goes dark with
	// nothing left to do. Turning the screen on puts it back to zero, wherever
	// it had got to.
	//
	// It counts on CLOCK_BOOTTIME, which keeps running while the SoC is
	// suspended, so a device that spends the wait asleep still switches off at
	// the right moment.
	if (g_screen_on || player_is_busy(playing, input_idle)) {
		g_last_active_boot_ms = boottime_ms();
	}
	uint32_t off_idle_wall = boottime_ms() - g_last_active_boot_ms;

	// 4. auto screen-off after the configured input-idle timeout
	if (g_screen_on && g_cfg.screen_off_enabled && !g_screen_on_hold && input_idle >= g_cfg.screen_off_timeout_ms) {
		// Say why. A screen that goes dark on its own is indistinguishable
		// from a crash unless the log records that it was the idle timer.
		printf("power: screen off after %u ms without input (timeout %u ms)\n", input_idle,
			   g_cfg.screen_off_timeout_ms);
		power_screen_off();
	}

	// 4a-bis. the shutdown timer, once it has run out.
	//
	//     Before the suspend, not after it: on a tick where both are ready,
	//     suspending first would leave rtc_alarm_arm_for_auto_off() with a
	//     deadline already past, so it would arm nothing, that sleep would carry
	//     no alarm and the shutdown would slip by a whole period.
	if (g_auto_off_enabled && g_auto_off_ms > 0 && off_idle_wall >= g_auto_off_ms) {
		char why[64];
		snprintf(why, sizeof(why), "%u minutes idle", g_auto_off_ms / 60000u);
		power_auto_off_now(why);
	}

	// 4a-bis-2. the radios, a minute after the screen goes dark
	park_radios_if_idle(now);

	// 4a-bis-3. the real sleep, a minute after the screen goes dark. Stopped and
	//           off the network, the SoC halts the way the stock binary makes it
	//           halt.
	suspend_if_idle(now, playing);

	// 4b. the charger, if it is being held below full
	apply_charge_limit();

}

void power_notify_activity(void) {
	pthread_mutex_lock(&g_lock);
	g_last_button_activity_ms = lv_tick_get();
	pthread_mutex_unlock(&g_lock);
}

void power_notify_power_button(void) {
	pthread_mutex_lock(&g_lock);
	g_power_button_pending = true;
	pthread_mutex_unlock(&g_lock);
}

// --- Runtime configuration ---
void power_set_brightness(long value) {
	long max = brightness_max();
	if (value > max) {
		value = max;
	}
	if (value < BRIGHTNESS_MIN) {
		value = BRIGHTNESS_MIN;
	}
	g_cfg.brightness = value;
	if (g_screen_on) {
		backlight_write(value); // instant (no fade) for a live settings adjustment
	}
}

long power_get_brightness(void) { return g_cfg.brightness; }
long power_get_max_brightness(void) { return brightness_max(); }

void power_hold_screen_on(bool hold) {
	g_screen_on_hold = hold;
	if (!hold) {
		// The hold was released after minutes of untouched screen (a library
		// scan, the power menu): without resetting the idle clocks here the
		// screen-off timer would fire on the very next tick, blanking the
		// panel the moment the long job announces it has finished.
		power_notify_activity();
		if (g_disp) {
			lv_display_trigger_activity(g_disp);
		}
	}
}

// Double-tap-to-wake: the touch controller's gesture mode, the same sysfs
// switch the stock player writes (echo on/off > .../gesture_sw, decompile
// FUN_00481fc0). With it on, blanking the panel leaves the controller
// listening for a double tap, which it reports as a power-key press -- the
// button threads then wake the screen exactly as the real power key would.

bool power_double_tap_wake_enabled(void) { return g_double_tap_wake; }

#ifdef BOARD_R1
	// R1 does not support the required "gesture". GESTURE_WAKE_NODE does 
	// not exist and there is no equivalent i2c device
	void power_set_double_tap_wake(bool enabled) { }
#else
	#define GESTURE_WAKE_NODE "/sys/devices/i2c-1/1-0014/gesture_sw"

	void power_set_double_tap_wake(bool enabled) {
		g_double_tap_wake = enabled;
		FILE *f = fopen(GESTURE_WAKE_NODE, "w");
		if (!f) {
			printf("power: gesture node %s not writable\n", GESTURE_WAKE_NODE);
			return; // host build, or an unexpected firmware: nothing to switch
		}
		fputs(enabled ? "on" : "off", f);
		fclose(f);
		printf("power: double-tap wake %s\n", enabled ? "on" : "off");
	}
#endif

void power_set_screen_off_enabled(bool enabled) { g_cfg.screen_off_enabled = enabled; }
void power_set_screen_off_timeout(uint32_t ms) { g_cfg.screen_off_timeout_ms = ms; }

void power_get_config(power_config_t *out) {
	if (out) {
		*out = g_cfg;
	}
}

void power_init(const power_config_t *cfg, lv_display_t *disp) {
	g_cfg = *cfg;
	g_disp = disp;
	g_refr_timer = disp ? lv_display_get_refr_timer(disp) : NULL;
	g_screen_on = true;

	// discover the panel's maximum backlight level
	g_max_brightness = g_cfg.max_brightness_path ? read_long_from_file(g_cfg.max_brightness_path) : -1;

	// if no explicit on-brightness was configured, adopt whatever the panel is
	// currently set to (falling back to max). Never adopt a too-dim value: a
	// prior run may have exited with the screen off, leaving the panel at MIN --
	// adopting that would make the screen appear black once turned "on".
	if (g_cfg.brightness < BRIGHTNESS_MIN) {
		long cur = g_cfg.brightness_path ? read_long_from_file(g_cfg.brightness_path) : -1;
		g_cfg.brightness = cur >= BRIGHTNESS_ON_FLOOR ? cur : brightness_max();
	}
	screen_power(true); // make sure the panel is unblanked (in case a prior run left it off)
	backlight_write(g_cfg.brightness);

	// Nothing in this player reads the headphone cable's inline remote, and
	// leaving its ADC scanning costs a hundred wake-ups a second all day.
	earpods_adc_set(false);


	uint32_t now = lv_tick_get();
	g_last_button_activity_ms = now;
	g_last_playing_ms = now;
	g_ignore_button_until = now;

	g_last_active_boot_ms = boottime_ms(); // the wall-clock idle marker starts now

	// Suspend to RAM: on by default, 60 seconds after the screen goes dark,
	// matching the stock binary's standby.
	// [power] standby_mem = 0 disables it; standby_mem_seconds sets the wait.
	g_mem_enabled = g_cfg.power_state_path && config_get_int("power", "standby_mem", 1) != 0;
	long mem_secs = config_get_int("power", "standby_mem_seconds", 60);
	g_mem_instant_wakes_max = (int)config_get_int("power", "standby_mem_max_instant", 2);
	if (g_mem_instant_wakes_max < 1) {
		g_mem_instant_wakes_max = 1;
	}
	if (mem_secs < 15) {
		mem_secs = 15; // any lower risks the device sleeping under the fingers
	}
	g_mem_after_ms = (uint32_t)mem_secs * 1000u;

	// The radios go down on their own clock, not the Standby toggle's: they are
	// worth switching off whether or not the SoC is going to sleep.
	long park_secs = config_get_int("power", "radio_park_seconds", RADIO_PARK_AFTER_MS_DEFAULT / 1000);
	if (park_secs < 15) {
		park_secs = 15; // any lower and a pocket checked twice re-associates for nothing
	}
	g_radio_park_after_ms = (uint32_t)park_secs * 1000u;

	lv_timer_create(power_timer_cb, POWER_TICK_MS, NULL);

	printf("power: initialized (max_brightness=%ld, on=%ld, screen_off=%s/%ums, mem=%s/%lus)\n", g_max_brightness,
		   g_cfg.brightness, g_cfg.screen_off_enabled ? "on" : "off", g_cfg.screen_off_timeout_ms,
		   g_mem_enabled ? "TEST" : "no", (unsigned long)mem_secs);
}
