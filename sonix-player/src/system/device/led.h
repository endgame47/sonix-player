#ifndef LED_H
#define LED_H

#include <stdbool.h>

// The RGB status LED, driven exactly the way the stock player drives it.
//
// The firmware loads leds_sgm31324_add.ko, which registers a table of preset
// colour patterns with the SGM31324 RGB controller and exposes them as small
// integers on /sys/class/leds/sgm31324-leds/led_pattern. The stock binary
// just writes the pattern number for the state it is in; the numbers below
// come out of its logical->pattern table (FUN_0047ce00) and its callers:
//
//   1   aqua           device on, nothing playing (written at stock startup)
//   2   yellow         PCM at or below 48 kHz
//   3   aqua           PCM above 48 up to 192 kHz
//   4   orange         PCM above 192 kHz
//   5   off            the logical-0 state (all-zero registers)
//   6   red breathing  charging: the stock LED state machine forces this,
//                      last, whenever its charger flag is set; the register
//                      block (blink period, ~50% duty, ramp, current on the
//                      red channel only) is the charging pulse
//   11  white          DSD (the white LED on GPIO PF11, wled_pattern_id=11)
//   8   green          MQA
//   9   blue           MQA Studio
//   10  magenta        MQA rendering
//
// Priority when states overlap, matching the stock state machine exactly:
// charging red > playback colour > idle aqua (the stock code applies the
// charging override after computing the playback colour).
//
// The stock binary also has a helper for a dedicated red LED classdev
// (/sys/class/leds/red/trigger, "breathing"/"none"); at startup it writes
// "none" once and never touches it again -- no kernel module in the firmware
// registers a "red" LED, so the node is vestigial on this device. It is still
// driven here (breathing while charging) in case a firmware revision grows
// it; a missing node is quietly skipped.
//
// Everything degrades to a no-op when the sysfs nodes are missing (the host
// build, or firmware without the module loaded).

// Probes the sysfs nodes and replays the stock startup sequence: red trigger
// released, power-on aqua (pattern 1).
void led_init(void);

// Red breathing while a charge is actually going in, overriding the playback
// colour like the stock player does. Not "a cable is in": a battery at 100%%,
// or held at the configured charge limit, has nothing left to indicate and the
// LED goes back to whatever it would be showing with no cable -- the playback
// colour, a mode's colour, or the idle aqua. The red is an override on top of
// the rest, not a state of its own.
//
// Cheap to call repeatedly: it only writes when the resolved pattern changes.
// But the caller must hand it a settled answer -- the pattern is a ramp
// programmed into the LED controller, and rewriting it restarts the ramp, so a
// reading that flaps comes out as a red that stutters.
void led_set_charging(bool charging);

// Whether a cable is supplying at all, charge finished or not. It does not
// light anything by itself; what it does is keep the darken-in-standby option
// off the LED. Without it a player left plugged in overnight answers the end of
// its charge by going dark -- the red stops having anything to say and the
// standby option, whose window ran out hours ago, takes the LED straight out.
void led_set_on_charger(bool present);

// Picks the pattern for what is playing (shown when not charging). `playing`
// false or rate 0 falls back to the idle aqua. `podcast` true paints the
// purple (pattern 10) instead of the sample-rate colour. Cheap to call
// repeatedly: only writes on change.
void led_update_playback(bool playing, int sample_rate, bool podcast);

// Wi-Fi transfer active: pattern 12 (a register added by hand to the driver
// script). Stays lit with the screen off, since it indicates the server is
// running, and overrides the darken-in-standby option.
void led_set_wifi_transfer(bool active);

// Bluetooth receiver mode: pattern 14, blue at full brightness (another
// register added by hand to the driver script). Same argument as the transfer
// above, and the same exemption from the standby option -- nothing plays
// through audio.c in that mode, so led_update_playback() reads a stopped
// player, and the LED went out twenty seconds after the screen did with a
// computer still sending music into the jack.
void led_set_bt_receiver(bool active);

// DAC mode. Nothing plays through audio.c there, so led_update_playback() sees
// a stopped player and the LED went out twenty seconds after the screen did,
// with the computer still sending music. While the host streams, the rate gets
// the colour a local track of that rate would; while the mode is merely up, the
// LED stays on the idle colour rather than being darkened by the standby
// option -- the same reasoning as the transfer above.
void led_set_dac(bool active, bool streaming, int sample_rate);

// The LED master switch. Off keeps the LED dark whatever else is
// going on -- with one exception, the same one the stock player makes: a
// charge in progress still shows its red, because a device that looks dead
// on the charger is worse than a lit LED. On (the default) restores the
// normal behaviour, including the standby option below.
void led_set_enabled(bool enabled);

// The darken-in-standby option: with it on, the LED goes dark after
// twenty seconds in standby (screen off) with nothing playing. A charge in
// progress is never darkened by it -- the red keeps breathing -- and any
// playback or wake lights the LED right back up. Re-evaluated on the same
// polls as the rest, so the switch-off lands within a few seconds.
void led_set_idle_off(bool enabled);

// Screen state, fed by power.c: the standby option only ever darkens the LED
// while the screen is off. Waking restarts the idle window.
void led_set_standby(bool standby);

#endif /* LED_H */
