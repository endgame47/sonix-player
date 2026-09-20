#ifndef USBDAC_H
#define USBDAC_H

#include <stdbool.h>

// USB DAC mode: the player stops being a player and becomes a sound card.
//
// How the stock firmware does it, read out of its own binary rather than
// guessed, because the shell script in /usr/bin is only half the story:
//
//   1. a configfs gadget with the `uac_sa` function -- Ingenic's own USB Audio
//      Class function, compiled into this kernel (the only two functions this
//      kernel has are mass_storage and uac_sa);
//   2. the function then creates /dev/uac_sa, which is NOT just a status
//      channel: it is the audio itself. The stock player waits up to five
//      seconds for the node to appear, opens it O_RDWR and puts a thread on
//      it;
//   3. that thread asks the driver what the host is doing with two ioctls and
//      then read()s PCM off it, frame by frame, into the same output the rest
//      of the player uses.
//
// The three ioctls, with the numbers the stock binary passes (plain ordinals,
// not _IOR-encoded):
//
//   ioctl(fd, 0, &state)   0 = the host is not streaming, 1 = it is
//   ioctl(fd, 1, &format)  three words: format, sample rate, flags
//   ioctl(fd, 2, &cap)     1 = this build also does capture (ignored here: the
//                          gadget is declared OUT-only, and the stock player
//                          only starts its recording thread when the function
//                          was built the other way)
//
// The rate is the host's to choose, which is the part the shell script hides:
// it writes 48000 and stops there, but the kernel function carries the string
// "32k~384khz" and announces that whole range to the host. The rate in use at
// any moment is whatever the computer picked, and it arrives through ioctl 1 --
// which is why the stock player's display follows the host instead of showing
// one fixed number.
//
// Frame size: the stock player allocates `rate * 8 / 100` bytes, which is ten
// milliseconds at eight bytes per frame -- stereo, 32 bits per sample. So the
// samples come off the node as 32-bit stereo whatever the host sends, except
// in DSD, where the flags word is 1 and the payload is one bit per sample.

typedef struct {
	bool starting;	 // being switched on: slow work in progress on a worker
	bool active;	 // DAC mode is on: the gadget is up
	bool streaming;	 // ...and the host is actually sending audio
	int sample_rate; // what the host chose, not what was requested
	int bits;		 // 32 for PCM, 1 for DSD
	int channels;
	bool dsd;
	char error[128];
} usbdac_state_t;

// Turns DAC mode on: tears down whatever gadget owns the USB controller,
// builds the UAC one, waits for /dev/uac_sa and starts the audio thread.
//
// Local playback is stopped first: in DAC mode the player is not playing
// anything of its own, and one PCM takes one writer.
//
// Returns immediately: the work happens on a worker thread and the result
// arrives in the state, because building the gadget shells out to mount and
// then waits up to five seconds for /dev/uac_sa. Watch `starting`, then
// `active` or `error`.
bool usbdac_start(void);

// Back to normal: stops the thread, closes the node, drops the UAC gadget and
// puts the mass-storage one back on the controller.
void usbdac_stop(void);

bool usbdac_is_active(void);
void usbdac_get_state(usbdac_state_t *out);

// Bumped whenever the state changes, so a page can redraw only when there is
// something new -- the rate changing under it included.
unsigned usbdac_serial(void);

// ---------------------------------------------------------------------------
// Charging
// ---------------------------------------------------------------------------
//
// "Limited USB current" in the stock settings, and the reason it exists: a DAC
// drawing charge current off the same cable it is taking audio from is a noise
// source, and some people would rather run off the battery.
//
// It stops the charge rather than throttling the input: the cable keeps
// supplying the player, and only what would go into the battery is held off.
// Remembered in config as [usb] dac_charge_disable and applied whenever DAC
// mode starts.
void usbdac_set_charging(bool enabled);
bool usbdac_charging_enabled(void);

#endif /* USBDAC_H */
