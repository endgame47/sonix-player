#include "usbaudio.h"

#include "src/system/audio/audio.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/bluetooth/bluetooth.h"

#include <alsa/asoundlib.h>
#include <dirent.h>
#include <limits.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TYPEC_PORT_TYPE "/sys/class/typec/port0/port_type"
#define TYPEC_PARTNER "/sys/class/typec/port0-partner"
#define SOUND_CLASS "/sys/class/sound"

// How long something may sit on the port without turning into a sound card
// before it is taken for a host. Enumerating a USB audio device takes well
// under a second; three is generous and still short enough that a phone
// connects a beat late rather than not at all.
#define HANDOVER_SECS 3

// And how long the port must stay empty before dual role is offered again.
// Changing port_type while something is attached makes the kernel drop the
// connection and negotiate afresh, so the partner disappears for a moment after
// each write; without this delay the two states chase each other.
#define RELEASE_SECS 3

static int active_card = -1;			// the USB card in use, -1 when none
static char active_name[64];			// its id, for the interface
static char volume_control[64];			// the control the level is written to
static long volume_min, volume_max;
static unsigned int volume_count = 1; // how many channels that control carries

// Whether the port is set to dual role right now. The attribute lists the types
// it supports and brackets the one in force, so "[dual] source sink" is dual and
// "dual source [sink]" is not.
static bool port_is_dual(void) {
	FILE *f = fopen(TYPEC_PORT_TYPE, "r");
	if (!f) {
		return false;
	}
	char line[96] = {0};
	bool dual = fgets(line, sizeof(line), f) && strstr(line, "[dual]") != NULL;
	fclose(f);
	return dual;
}

// Whether anything at all is on the port. The kernel creates this node when a
// cable brings something with it and takes it away when it goes, before any
// role or protocol is worked out.
static bool partner_present(void) {
	DIR *dir = opendir(TYPEC_PARTNER);
	if (!dir) {
		return false;
	}
	closedir(dir);
	return true;
}

// Writes port_type, and says whether it took.
static bool write_port_type(const char *want) {
	FILE *f = fopen(TYPEC_PORT_TYPE, "w");
	if (!f) {
		static bool said;
		if (!said) {
			said = true;
			fprintf(stderr, "usbaudio: %s is not there; the port cannot take a peripheral\n", TYPEC_PORT_TYPE);
		}
		return false;
	}
	bool ok = fputs(want, f) >= 0 && fputc('\n', f) != EOF;
	if (fclose(f) != 0) {
		ok = false;
	}
	return ok;
}

// Hands the port back to the other end. See the state machine in
// arbitrate_port(): this is what a phone needs.
static void port_become_sink(void) {
	if (!port_is_dual()) {
		return;
	}
	bool ok = write_port_type("sink");
	fprintf(stderr, "usbaudio: something on the port is not a sound card; sink %s\n",
			ok ? "written, the other end can be the host" : "REFUSED");
}

// Allows the port to take a peripheral, and says whether it stuck.
//
// Written whenever the port is found not to be dual, not once at startup: the
// setting does not stay put, most likely because the gadget is bound and
// unbound as the cable comes and goes. Checking and rewriting costs one read a
// second and a write only when the answer is wrong.
bool usbaudio_ensure_dual(void) {
	if (port_is_dual()) {
		return true;
	}

	bool ok = write_port_type("dual");
	bool stuck = ok && port_is_dual();

	// Said when the answer changes, not once a second: if something out there
	// keeps putting the port back, this runs every poll and a line per second
	// would bury the log it is meant to explain.
	static int said = -1;
	int outcome = stuck ? 0 : (ok ? 1 : 2);
	if (outcome != said) {
		said = outcome;
		fprintf(stderr, "usbaudio: dual role %s\n",
				stuck ? "set" : (ok ? "written but did not stick" : "REFUSED by the driver"));
	}
	return stuck;
}

// At startup the port is offered dual role only when it is empty. Booting with
// a phone or a PC already on the cable and grabbing the host role from it is
// the same failure as doing it later, and here there is no reason to: whatever
// is on the port arrived before this player did.
bool usbaudio_init(void) {
	if (partner_present()) {
		fprintf(stderr, "usbaudio: something is already on the port at startup; leaving the role alone\n");
		return false;
	}
	return usbaudio_ensure_dual();
}

// Where card `n`'s device really is. Empty when it has none.
//
// realpath and not readlink: the link's own target is relative
// ("../../../1-1:1.0"), so looking for "/usb" in it never matches. Resolved,
// the same link comes out as /sys/devices/platform/jz-dwc2/usb1/1-1/1-1:1.0.
static void card_device_path(int n, char *out, size_t out_size) {
	char link[PATH_MAX];
	char resolved[PATH_MAX];

	out[0] = '\0';
	snprintf(link, sizeof(link), SOUND_CLASS "/card%d/device", n);
	if (realpath(link, resolved)) {
		snprintf(out, out_size, "%s", resolved);
	}
}

// Whether card `n` hangs off the USB bus rather than off the board.
//
// Asked of the bus and not of the name: the built-in card is called whatever
// the machine driver calls it, and a USB device whatever its maker wrote in its
// descriptor. Two ways, because one of them is exact and the other always
// works: a USB device's `subsystem` link resolves to .../bus/usb, and failing
// that the device's own path runs through the controller's usbN directory.
static bool card_is_usb(int n) {
	char link[PATH_MAX];
	char resolved[PATH_MAX];

	snprintf(link, sizeof(link), SOUND_CLASS "/card%d/device/subsystem", n);
	if (realpath(link, resolved)) {
		size_t len = strlen(resolved);
		if (len >= 4 && strcmp(resolved + len - 4, "/usb") == 0) {
			return true;
		}
	}

	card_device_path(n, resolved, sizeof(resolved));
	return strstr(resolved, "/usb") != NULL;
}

// The lowest-numbered USB sound card, or -1. Lowest rather than newest because
// there is only ever one socket: a second would mean a hub, and the first is as
// good a choice as any.
static int find_usb_card(void) {
	DIR *dir = opendir(SOUND_CLASS);
	if (!dir) {
		return -1;
	}

	int found = -1;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (strncmp(de->d_name, "card", 4) != 0) {
			continue;
		}
		char *end = NULL;
		long n = strtol(de->d_name + 4, &end, 10);
		if (!end || *end || n < 0) {
			continue; // pcmC0D0p and the rest of the class
		}
		if (card_is_usb((int)n) && (found < 0 || n < found)) {
			found = (int)n;
		}
	}
	closedir(dir);
	return found;
}

static void read_card_id(int card, char *out, size_t out_size) {
	char path[128];
	snprintf(path, sizeof(path), SOUND_CLASS "/card%d/id", card);

	out[0] = '\0';
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	if (fgets(out, (int)out_size, f)) {
		out[strcspn(out, "\r\n")] = '\0';
	}
	fclose(f);
}

// The device's own playback volume, whatever it decided to call it.
//
// USB audio devices name it themselves -- "PCM Playback Volume" is the usual
// one, but headsets ship "Speaker Playback Volume" and "Headphone Playback
// Volume" too -- so the control is found by shape rather than by name: the
// first integer control whose name ends in "Playback Volume". Its range and
// its channel count come with it, because a USB device's are its own and
// nothing like the CS43198's.
//
// Writable is part of the shape. A control that only reports a level is not a
// volume control for this purpose, and taking it for one would stand the
// software attenuation down in favour of something that cannot move.
static void find_volume_control(int card) {
	snd_ctl_t *ctl;
	snd_ctl_elem_list_t *list;
	char name[32];

	volume_control[0] = '\0';
	volume_min = volume_max = 0;
	volume_count = 1;

	snprintf(name, sizeof(name), "hw:%d", card);
	if (snd_ctl_open(&ctl, name, 0) < 0) {
		return;
	}

	snd_ctl_elem_list_alloca(&list);
	if (snd_ctl_elem_list(ctl, list) < 0) {
		snd_ctl_close(ctl);
		return;
	}
	unsigned int count = snd_ctl_elem_list_get_count(list);
	if (snd_ctl_elem_list_alloc_space(list, count) < 0 || snd_ctl_elem_list(ctl, list) < 0) {
		snd_ctl_close(ctl);
		return;
	}

	static const char SUFFIX[] = "Playback Volume";
	for (unsigned int i = 0; i < count && !volume_control[0]; i++) {
		snd_ctl_elem_id_t *id;
		snd_ctl_elem_info_t *info;

		snd_ctl_elem_id_alloca(&id);
		snd_ctl_elem_info_alloca(&info);
		snd_ctl_elem_list_get_id(list, i, id);
		snd_ctl_elem_info_set_id(info, id);
		if (snd_ctl_elem_info(ctl, info) < 0 ||
			snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_INTEGER ||
			!snd_ctl_elem_info_is_writable(info)) {
			continue;
		}

		const char *elem = snd_ctl_elem_info_get_name(info);
		size_t elen = strlen(elem);
		size_t slen = sizeof(SUFFIX) - 1;
		if (elen < slen || strcmp(elem + elen - slen, SUFFIX) != 0) {
			continue;
		}

		snprintf(volume_control, sizeof(volume_control), "%s", elem);
		volume_min = snd_ctl_elem_info_get_min(info);
		volume_max = snd_ctl_elem_info_get_max(info);
		volume_count = snd_ctl_elem_info_get_count(info);
		if (volume_count < 1 || volume_count > 8) {
			volume_count = 1;
		}
	}

	snd_ctl_elem_list_free_space(list);
	snd_ctl_close(ctl);

	if (volume_control[0]) {
		fprintf(stderr, "usbaudio: volume goes to '%s' (%ld..%ld, %u ch)\n", volume_control, volume_min,
				volume_max, volume_count);
	} else {
		fprintf(stderr, "usbaudio: the device has no playback volume control\n");
	}
}

// Hands the level to the device's own control, and checks that it landed.
//
// A dongle that publishes a Feature Unit it does not implement takes the write
// and stays where it was, and there is nothing in the return code to say so.
// Left unchecked that is the worst failure this player has: the software
// attenuation stands down for a control that does nothing, and the stream
// reaches a pair of headphones at full scale with the volume keys moving a
// number on the screen and nothing else.
//
// So the value is read back. A driver is allowed to round it -- a control with
// eight steps will -- and that is not a failure; sitting at the top of its
// range after being asked for the bottom half is. When that happens the
// control is given up and swvolume.c takes the level over from the next block
// of samples, which is a degree of attenuation in software rather than none at
// all in hardware.
void usbaudio_apply_volume(int percent) {
	if (active_card < 0 || !volume_control[0] || volume_max <= volume_min) {
		return;
	}
	if (percent < 0) {
		percent = 0;
	}
	if (percent > 100) {
		percent = 100;
	}

	long span = volume_max - volume_min;
	long value = volume_min + (span * percent + 50) / 100;

	snd_ctl_t *ctl;
	char card[32];
	snprintf(card, sizeof(card), "hw:%d", active_card);
	if (snd_ctl_open(&ctl, card, 0) < 0) {
		return;
	}

	snd_ctl_elem_id_t *id;
	snd_ctl_elem_value_t *elem;
	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_value_alloca(&elem);

	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, volume_control);
	snd_ctl_elem_value_set_id(elem, id);
	// Every channel the control carries, and no more: writing two values to a
	// mono control is harmless but writing one to a stereo control leaves the
	// right ear where it was.
	for (unsigned int c = 0; c < volume_count; c++) {
		snd_ctl_elem_value_set_integer(elem, c, value);
	}

	int wrote = snd_ctl_elem_write(ctl, elem);
	long back = value;
	if (wrote >= 0) {
		snd_ctl_elem_value_t *check;
		snd_ctl_elem_value_alloca(&check);
		snd_ctl_elem_value_set_id(check, id);
		if (snd_ctl_elem_read(ctl, check) >= 0) {
			back = snd_ctl_elem_value_get_integer(check, 0);
		}
	}
	snd_ctl_close(ctl);

	// Asked for something below the top and still sitting at the top. The slack
	// is never zero: on a control with only a few steps, max - 0 would read as
	// "stuck" the moment the level really is the maximum.
	long slack = span / 20;
	if (slack < 1) {
		slack = 1;
	}
	bool stuck = value <= volume_max - slack && back >= volume_max - slack;
	if (wrote < 0 || stuck) {
		fprintf(stderr,
				"usbaudio: '%s' did not take %d%% (asked %ld, reads %ld); the level goes to the samples "
				"instead\n",
				volume_control, percent, value, back);
		volume_control[0] = '\0';
	}
}

// Who the port belongs to.
//
// Dual role is what lets headphones be seen at all -- as a sink this device
// offers Rd, headphones offer Rd, and two Rd's never notice each other -- so
// the port has to be dual while it is empty or there is nothing to detect.
//
// But a phone is dual-role too, and Android implements Try.SNK: left dual, the
// two agree that this device is the host, so it charges the phone and never
// offers it the card or the DAC.
//
// Neither role can be chosen in advance, because the thing that distinguishes
// the two cases only shows up after the port is already connected: headphones
// become a sound card, a phone does not. So the port stays dual while nothing
// is attached; when something attaches it has HANDOVER_SECS to turn into a
// sound card, and if it does not, the port goes sink and the other end gets to
// be the host. It goes back to dual once the port is empty again. The cost is
// that a phone connects a few seconds late rather than instantly.
static void arbitrate_port(bool audio_present) {
	static time_t attached_at;	// when the current non-audio partner appeared
	static time_t empty_at;		// when the port last became empty
	time_t now = time(NULL);

	if (audio_present) {
		attached_at = 0;
		empty_at = 0;
		usbaudio_ensure_dual();
		return;
	}

	if (partner_present()) {
		empty_at = 0;
		if (attached_at == 0) {
			attached_at = now;
		} else if (now - attached_at >= HANDOVER_SECS) {
			port_become_sink();
		}
		return;
	}

	attached_at = 0;
	if (empty_at == 0) {
		empty_at = now;
	}
	// Nothing on the port, and nothing on it for long enough that this is not
	// the gap left by a role write: it can be dual again, ready for the next
	// pair of headphones.
	if (now - empty_at >= RELEASE_SECS) {
		usbaudio_ensure_dual();
	}
}

void usbaudio_poll(void) {
	// Who owns the port comes first, and is asked whatever playback is doing:
	// the arbitration is about the socket, not about where the sound goes.
	int card = find_usb_card();
	arbitrate_port(card >= 0);

	// Bluetooth wins outright over where playback goes. Both would be pointing
	// it somewhere, and the one the listener is wearing is the one that was
	// chosen deliberately; fighting over audio_set_output_device() from two
	// pollers is how the sound ends up somewhere nobody asked for.
	if (bluetooth_audio_active()) {
		return;
	}

	if (card == active_card) {
		return;
	}

	active_card = card;
	if (card >= 0) {
		char pcm[64];
		// plughw and not hw: what comes out of the decoder is whatever the file
		// held, and a USB headset takes the two or three formats it was built
		// for. The plug layer converts; hw would simply refuse to open.
		snprintf(pcm, sizeof(pcm), "plughw:%d,0", card);
		char where[PATH_MAX];
		read_card_id(card, active_name, sizeof(active_name));
		card_device_path(card, where, sizeof(where));
		fprintf(stderr, "usbaudio: card%d '%s' at %s; playback goes to %s\n", card, active_name, where, pcm);

		// The level before the route, and not after it: between the two calls
		// the sound is already leaving over the port, and a device fresh off
		// the bus sits at whatever its own default is -- which for a dongle is
		// the top of its range. The other order puts a full-scale burst into a
		// pair of headphones somebody is wearing.
		find_volume_control(card);
		usbaudio_apply_volume(get_volume_percent());
		audio_set_output_device(pcm);
	} else {
		active_name[0] = '\0';
		volume_control[0] = '\0';
		fprintf(stderr, "usbaudio: the port is empty again; playback goes back to the jacks\n");
		audio_set_output_device(NULL);
	}
}

bool usbaudio_active(void) { return active_card >= 0; }

bool usbaudio_has_volume_control(void) { return active_card >= 0 && volume_control[0] && volume_max > volume_min; }

void usbaudio_card_name(char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return;
	}
	snprintf(out, out_size, "%s", active_name);
}
