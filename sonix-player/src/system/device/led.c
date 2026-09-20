#include "led.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PATTERN_NODE "/sys/class/leds/sgm31324-leds/led_pattern"
#define RED_TRIGGER_NODE "/sys/class/leds/red/trigger"

// Pattern numbers, as registered by leds_sgm31324_add.sh and used by the
// stock binary (FUN_0047ce00's logical->pattern table). See led.h for the
// full story.
//
// Pattern 1 is what the stock player writes at startup (logical state 1, the
// power-on aqua). Pattern 6 is what its LED state machine forces -- last,
// overriding even the playback colour -- while the charger flag is set: the
// register block programs the red channel in blink mode with a ramp, i.e.
// the breathing red. Pattern 5 (logical 0, all-zero registers) is "off".
#define PATTERN_IDLE_AQUA 1
#define PATTERN_CHARGING_RED 6
#define PATTERN_PCM_48K 2
#define PATTERN_PCM_192K 3
#define PATTERN_PCM_HI 4
#define PATTERN_DSD_WHITE 11
#define PATTERN_OFF 5 // the logical-0 state: all registers zero
// Podcast purple: pattern 13, added by hand to the driver script by the user,
// like 12 for transfers. (The first attempt used 10, the stock MQA rendering
// pattern, which lit up magenta rather than purple.)
#define PATTERN_PODCAST_PURPLE 13
// Wi-Fi transfer: pattern 12, added by hand to the driver script
// (leds_sgm31324_add.sh, regs="000E7D00686601010706"). Lit while a transfer is
// active, and never turned off by the standby option: with the screen dark it
// is the only sign the transfer server is running.
#define PATTERN_WIFI_TRANSFER 12
// Bluetooth receiver: pattern 14, blue at full brightness, added by hand to the
// driver script like 12 and 13. Lit for as long as the mode is on, screen off
// included -- it is the only sign the player is being used as a pair of
// headphones.
#define PATTERN_BT_RECEIVER 14

// With the "LED off in standby" option on, this long without anything playing
// turns the LED off entirely.
#define IDLE_OFF_SECONDS 20

static bool have_pattern_node;
static bool have_red_node;

static int current_pattern = -1;
static int red_trigger_state = -1; // -1 unknown, 0 "none", 1 "breathing"

// The three inputs the pattern is resolved from. The charger wins over
// everything -- the stock LED state machine applies its charging override
// after the playback colour -- and playback wins over idle.
static bool s_charging;
static bool s_playing;
static int s_rate;
static bool s_podcast;		 // what is playing is a podcast episode
static bool s_wifi_transfer; // a Wi-Fi transfer is active
static bool s_bt_receiver;	 // Bluetooth receiver mode is on
static bool s_dac_active;	 // DAC mode is on: the gadget is up
static bool s_dac_streaming; // ...and the computer is actually sending audio
static int s_dac_rate;

static bool s_led_enabled = true; // the LED master switch
static bool s_idle_off_enabled;
static bool s_standby;		  // screen off (set by power.c)
static time_t s_standby_since; // when the screen went off
static time_t s_last_playing; // when something was last playing
static bool s_on_charger;	  // a cable is supplying, charge finished or not
static time_t s_charger_since; // when that last changed

static void write_str(const char *path, const char *value) {
	FILE *f = fopen(path, "w");
	if (!f) {
		return;
	}
	fputs(value, f);
	fclose(f);
}

static void set_pattern(int pattern) {
	if (!have_pattern_node || pattern == current_pattern) {
		return;
	}
	current_pattern = pattern;

	char buf[16];
	snprintf(buf, sizeof(buf), "%d", pattern);
	write_str(PATTERN_NODE, buf);
}

static int rate_pattern(int sample_rate) {
	// The same rate thresholds the stock player uses. DSD would enter as its
	// PCM-equivalent rate (2822400 and up) if a DSD decoder ever lands.
	if (sample_rate >= 2822400) {
		return PATTERN_DSD_WHITE;
	}
	if (sample_rate > 192000) {
		return PATTERN_PCM_HI;
	}
	if (sample_rate > 48000) {
		return PATTERN_PCM_192K;
	}
	return PATTERN_PCM_48K;
}

// When the "off in standby" countdown starts: the latest of the screen going
// dark, the last moment something was playing, and the cable going in or out.
static time_t idle_window_start(void) {
	time_t latest = s_standby_since > s_last_playing ? s_standby_since : s_last_playing;
	return s_charger_since > latest ? s_charger_since : latest;
}

static void apply(void) {
	// s_charging is "a charge is going in", not "a cable is in": the caller
	// takes the battery being full, or held at its limit, out of it, and hands
	// this a settled answer rather than a reading that is still wobbling. See
	// led.h.
	bool charging_now = s_charging;

	// The separate red classdev, where the firmware has one, driven the way the
	// stock binary's own red-LED helper drives it: breathing on the charger,
	// released otherwise. No module on this firmware registers it, so the write
	// is skipped and the charging red comes from the pattern below.
	int want_red = charging_now ? 1 : 0;
	if (have_red_node && want_red != red_trigger_state) {
		red_trigger_state = want_red;
		write_str(RED_TRIGGER_NODE, charging_now ? "breathing" : "none");
	}

	int pattern;
	if (!s_led_enabled) {
		// The master switch is off: the LED stays dark, except that a charge
		// in progress still shows -- a device that looks dead while it is
		// charging is worse than a lit LED.
		set_pattern(charging_now ? PATTERN_CHARGING_RED : PATTERN_OFF);
		return;
	}
	if (s_playing && s_rate > 0) {
		// Playback always shows its colour (red while actually charging, the
		// stock priority). A podcast has its own colour, purple, whatever the
		// sample rate: it says what is being listened to, not how it is sampled.
		pattern = charging_now ? PATTERN_CHARGING_RED : s_podcast ? PATTERN_PODCAST_PURPLE : rate_pattern(s_rate);
	} else if (s_dac_streaming && s_dac_rate > 0) {
		// In DAC mode nothing plays through audio.c, so s_playing is false and
		// the LED went dark twenty seconds after the screen did -- with the
		// computer still sending music. The rate the host chose gets the same
		// colour a local track of that rate would.
		pattern = charging_now ? PATTERN_CHARGING_RED : rate_pattern(s_dac_rate);
	} else if (charging_now) {
		// Never darkened by the standby option: a charge in progress stays
		// visible.
		pattern = PATTERN_CHARGING_RED;
	} else if (s_wifi_transfer) {
		// A Wi-Fi transfer is running: its pattern stays lit even with the
		// screen off, since it is the only sign the server is running, and it
		// deliberately overrides the off-in-standby option.
		pattern = PATTERN_WIFI_TRANSFER;
	} else if (s_bt_receiver) {
		// Bluetooth receiver mode, lit whether or not the sender is sending
		// right now: the mode is what the LED reports, the way the transfer
		// above reports its server. Nothing plays through audio.c here, so the
		// branch at the top never fires and without this one the LED went dark
		// twenty seconds after the screen did -- with music still coming out of
		// the jack.
		pattern = PATTERN_BT_RECEIVER;
	} else if (s_dac_active) {
		// The mode is up but the computer is not sending anything yet. Same
		// argument as the transfer above: with the screen dark the LED is the
		// only sign the player is a sound card, so the standby option does not
		// reach it.
		pattern = PATTERN_IDLE_AQUA;
	} else if (s_idle_off_enabled && !s_on_charger && s_standby &&
			   time(NULL) - idle_window_start() >= IDLE_OFF_SECONDS) {
		// LED off in standby: the screen has been off this long with nothing
		// playing and nothing else going on. The window runs from whichever
		// came last -- the screen going dark, playback stopping, the cable
		// going in or out. Counted from the screen alone, a track paused with
		// the screen already off would find the window long since elapsed and
		// put the LED out the instant the music stopped, with no aqua in
		// between. With the screen on the aqua stays: the device is visibly in
		// use.
		//
		// And not on the charger at all, which is the point of s_on_charger: a
		// battery that reaches 100%, or its configured limit, has nothing left
		// to say in red, and a player left plugged in overnight would answer
		// the end of its charge by going dark. On a cable the LED keeps the
		// aqua that says the device is there -- the same exemption the transfer
		// server, the receiver and DAC mode have above.
		pattern = PATTERN_OFF;
	} else {
		pattern = PATTERN_IDLE_AQUA;
	}
	set_pattern(pattern);
}

void led_init(void) {
	have_pattern_node = access(PATTERN_NODE, W_OK) == 0;
	have_red_node = access(RED_TRIGGER_NODE, W_OK) == 0;

	if (!have_pattern_node && !have_red_node) {
		return; // host build, or firmware without the LED module
	}

	fprintf(stderr, "led: pattern node %s, red trigger %s\n", have_pattern_node ? "ok" : "missing",
			have_red_node ? "ok" : "missing");

	s_last_playing = time(NULL); // the power-on colour holds for the first idle window

	// Exactly the stock startup sequence: red trigger released, pattern 1.
	if (have_red_node) {
		write_str(RED_TRIGGER_NODE, "none");
		red_trigger_state = 0;
	}
	set_pattern(PATTERN_IDLE_AQUA);
}

void led_set_charging(bool charging) {
	s_charging = charging;
	apply(); // cheap: both writes only happen on a change
}

void led_set_on_charger(bool present) {
	if (present == s_on_charger) {
		return;
	}
	s_on_charger = present;
	// A cable going in or out restarts the idle window, so the aqua gets its
	// twenty seconds after an unplug instead of the LED going dark in the same
	// instant as the cable leaves.
	s_charger_since = time(NULL);
	apply();
}

void led_update_playback(bool playing, int sample_rate, bool podcast) {
	s_playing = playing && sample_rate > 0;
	s_rate = sample_rate;
	s_podcast = podcast;
	if (s_playing) {
		s_last_playing = time(NULL);
	}
	apply();
}

void led_set_dac(bool active, bool streaming, int sample_rate) {
	if (active == s_dac_active && streaming == s_dac_streaming && sample_rate == s_dac_rate) {
		return;
	}
	s_dac_active = active;
	s_dac_streaming = streaming;
	s_dac_rate = sample_rate;
	apply();
}

void led_set_wifi_transfer(bool active) {
	if (active == s_wifi_transfer) {
		return;
	}
	s_wifi_transfer = active;
	apply();
}

void led_set_bt_receiver(bool active) {
	if (active == s_bt_receiver) {
		return;
	}
	s_bt_receiver = active;
	apply();
}

void led_set_enabled(bool enabled) {
	s_led_enabled = enabled;
	apply();
}

void led_set_idle_off(bool enabled) {
	s_idle_off_enabled = enabled;
	apply();
}

void led_set_standby(bool standby) {
	if (standby && !s_standby) {
		s_standby_since = time(NULL); // the idle window starts here
	}
	s_standby = standby;
	if (!standby) {
		// Waking counts as activity: the aqua shows for a fresh idle window
		// before the option can dim it again.
		s_last_playing = time(NULL);
		s_standby_since = time(NULL);
	}
	apply();
}

