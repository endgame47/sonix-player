#include "btstack.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "src/system/bluetooth/dbuslite.h"

#define BLUEZ_SERVICE "org.bluez"
#define BLUEZ_ROOT "/org/bluez"
#define BLUEZ_ADAPTER_PATH "/org/bluez/hci0"
#define BLUEZ_ADAPTER_IFACE "org.bluez.Adapter1"
#define BLUEZ_DEVICE_IFACE "org.bluez.Device1"
#define BLUEZ_TRANSPORT_IFACE "org.bluez.MediaTransport1"
#define BLUEZ_AGENT_MANAGER_IFACE "org.bluez.AgentManager1"
#define BLUEZ_AGENT_IFACE "org.bluez.Agent1"
#define BLUEZ_MEDIA_PLAYER_IFACE "org.bluez.MediaPlayer1"
#define BLUEZ_MEDIA_CONTROL_IFACE "org.bluez.MediaControl1"

#define BALSA_SERVICE "org.bluealsa"
#define BALSA_PCM_IFACE "org.bluealsa.PCM1"
#define BALSA_MANAGER_PATH "/org/bluealsa"
#define BALSA_MANAGER_IFACE "org.bluealsa.Manager1"

#define PROPS_IFACE "org.freedesktop.DBus.Properties"
#define OBJMAN_IFACE "org.freedesktop.DBus.ObjectManager"

// The player's agent. NoInputNoOutput because the device has neither a keypad
// nor a display the other end can be asked to compare a number on: pairing is
// Just Works, which is what every pair of headphones expects anyway.
#define AGENT_PATH "/org/sonix/bluetooth/agent"
#define AGENT_CAPABILITY "NoInputNoOutput"

// The A2DP sink UUID, which is what separates a pair of headphones from a phone
// in a scan list.
#define UUID_A2DP_SINK "0000110b-0000-1000-8000-00805f9b34fb"

#define CALL_MS 8000		// anything that is not pairing or connecting
#define OBJECT_PATH_MAX 160

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static dbus_conn_t *conn;
static void (*change_cb)(void);

// The addresses bluealsa currently has an A2DP source PCM for. More than one
// can exist in principle; the player only ever plays to the first.
#define MAX_PCMS 4
static char pcm_addr[MAX_PCMS][BT_ADDR_MAX];
static int pcm_count;

// The same list for the other direction: devices streaming into this one.
static char rx_addr[MAX_PCMS][BT_ADDR_MAX];
static int rx_count;

// What the remote player last said it was doing: "playing", "paused",
// "stopped", or empty when nothing has said. Written both by the signal and by
// the worker's own reading of the property.
static char media_status[32];
static btstack_track_t media_track;
static unsigned media_track_serial;

// One a{sv} of AVRCP track metadata into `out`. Every key the player does not
// use is stepped over by signature, the way the device properties are read.
static void read_track_props(dbus_reader_t *r, btstack_track_t *out) {
	dbus_array_iter_t props;
	if (!dbus_r_array_begin(r, "{sv}", &props)) {
		return;
	}
	while (dbus_r_array_more(r, &props)) {
		char key[64], sig[16];
		if (!dbus_r_string(r, key, sizeof(key)) || !dbus_r_signature(r, sig, sizeof(sig))) {
			return;
		}

		if (strcmp(key, "Title") == 0 && strcmp(sig, "s") == 0) {
			dbus_r_string(r, out->title, sizeof(out->title));
		} else if (strcmp(key, "Artist") == 0 && strcmp(sig, "s") == 0) {
			dbus_r_string(r, out->artist, sizeof(out->artist));
		} else if (strcmp(key, "Album") == 0 && strcmp(sig, "s") == 0) {
			dbus_r_string(r, out->album, sizeof(out->album));
		} else if (strcmp(key, "Duration") == 0 && strcmp(sig, "u") == 0) {
			uint32_t ms = 0;
			dbus_r_u32(r, &ms);
			out->duration_ms = (unsigned)ms;
		} else if (!dbus_r_skip(r, sig)) {
			return;
		}
	}
}

// Replaces the cache and bumps the serial only when something really moved: the
// sender repeats the same metadata on every status change, and a serial that
// moved every time would have the page redrawing for nothing.
static void store_track(const btstack_track_t *fresh) {
	pthread_mutex_lock(&lock);
	if (memcmp(&media_track, fresh, sizeof(media_track)) != 0) {
		media_track = *fresh;
		media_track_serial++;
	}
	pthread_mutex_unlock(&lock);
}

static void sleep_ms(int ms) {
	struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
}

// ---------------------------------------------------------------------------
// addresses and object paths
// ---------------------------------------------------------------------------

// AA:BB:CC:DD:EE:FF -> /org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF. False for
// anything that is not an address: this is the only place a string that came
// off the air turns into something sent to a daemon, so the shape is checked
// here rather than trusted.
static bool device_path(const char *address, char *out, size_t size) {
	if (!address) {
		return false;
	}
	char clean[BT_ADDR_MAX];
	size_t used = 0;
	for (const char *p = address; *p; p++) {
		char c = *p;
		if (c == ':' || c == '-') {
			c = '_';
		} else if (isxdigit((unsigned char)c)) {
			c = (char)toupper((unsigned char)c);
		} else {
			return false;
		}
		if (used + 1 >= sizeof(clean)) {
			return false;
		}
		clean[used++] = c;
	}
	clean[used] = '\0';
	if (used != 17) {
		return false;
	}
	snprintf(out, size, BLUEZ_ADAPTER_PATH "/dev_%s", clean);
	return true;
}

// The bluealsa PCM object for one of the two A2DP directions on this address.
//
// bluealsa names a PCM after the local profile and then after what the client
// does with it. "a2dpsrc/sink" is this device acting as an A2DP source, with a
// PCM the client writes into -- the headphones. "a2dpsnk/source" is this device
// acting as an A2DP sink, with a PCM the client reads out of -- the phone. The
// two words swap round between them, which reads wrong until it is said out
// loud: the profile is what the radio is doing and the mode is what the program
// is doing, and they are always opposites.
static bool pcm_path_dir(const char *address, bool receiving, char *out, size_t size) {
	char path[OBJECT_PATH_MAX];
	if (!device_path(address, path, sizeof(path))) {
		return false;
	}
	const char *tail = strrchr(path, '/');
	snprintf(out, size, "/org/bluealsa/hci0%s/%s", tail ? tail : "", receiving ? "a2dpsnk/source" : "a2dpsrc/sink");
	return true;
}

static bool pcm_path(const char *address, char *out, size_t size) {
	return pcm_path_dir(address, false, out, size);
}

// ..../dev_AA_BB_CC_DD_EE_FF/.... -> AA:BB:CC:DD:EE:FF
static bool address_from_path(const char *path, char *out, size_t size) {
	const char *at = strstr(path, "/dev_");
	if (!at) {
		return false;
	}
	at += 5;
	char address[BT_ADDR_MAX];
	size_t used = 0;
	for (int i = 0; i < 17; i++) {
		char c = at[i];
		if (c == '_') {
			c = ':';
		} else if (!isxdigit((unsigned char)c)) {
			return false;
		}
		if (used + 1 >= sizeof(address)) {
			return false;
		}
		address[used++] = c;
	}
	address[used] = '\0';
	snprintf(out, size, "%s", address);
	return true;
}

// ---------------------------------------------------------------------------
// the cache of bluealsa PCM objects
// ---------------------------------------------------------------------------

static bool path_is_a2dp_sink(const char *path) {
	return strncmp(path, "/org/bluealsa/", 14) == 0 && strstr(path, "/a2dpsrc/sink") != NULL;
}

static bool path_is_a2dp_source(const char *path) {
	return strncmp(path, "/org/bluealsa/", 14) == 0 && strstr(path, "/a2dpsnk/source") != NULL;
}

// Under `lock`.
static void pcm_add(char list[][BT_ADDR_MAX], int *count, const char *address) {
	for (int i = 0; i < *count; i++) {
		if (strcasecmp(list[i], address) == 0) {
			return;
		}
	}
	if (*count < MAX_PCMS) {
		snprintf(list[(*count)++], BT_ADDR_MAX, "%s", address);
	}
}

// Under `lock`.
static void pcm_remove(char list[][BT_ADDR_MAX], int *count, const char *address) {
	for (int i = 0; i < *count; i++) {
		if (strcasecmp(list[i], address) == 0) {
			list[i][0] = '\0';
			for (int j = i; j + 1 < *count; j++) {
				memcpy(list[j], list[j + 1], BT_ADDR_MAX);
			}
			(*count)--;
			return;
		}
	}
}

// Under `lock`.
static bool pcm_first(char list[][BT_ADDR_MAX], int count, char *address_out, size_t size) {
	bool any = count > 0;
	if (address_out && size) {
		snprintf(address_out, size, "%s", any ? list[0] : "");
	}
	return any;
}

bool btstack_audio_sink(char *address_out, size_t size) {
	pthread_mutex_lock(&lock);
	bool any = pcm_first(pcm_addr, pcm_count, address_out, size);
	pthread_mutex_unlock(&lock);
	return any;
}

bool btstack_audio_source(char *address_out, size_t size) {
	pthread_mutex_lock(&lock);
	bool any = pcm_first(rx_addr, rx_count, address_out, size);
	pthread_mutex_unlock(&lock);
	return any;
}

// ---------------------------------------------------------------------------
// reading a properties dictionary
// ---------------------------------------------------------------------------

// One a{sv}, with the keys this player cares about picked out and everything
// else stepped over by signature. `dev` may be NULL to read only for the side
// effects (there are none) -- it is passed NULL by nothing today.
static void read_device_props(dbus_reader_t *r, btstack_device_t *dev) {
	dbus_array_iter_t props;
	if (!dbus_r_array_begin(r, "{sv}", &props)) {
		return;
	}
	while (dbus_r_array_more(r, &props)) {
		char key[64], sig[16];
		if (!dbus_r_string(r, key, sizeof(key)) || !dbus_r_signature(r, sig, sizeof(sig))) {
			return;
		}

		if (strcmp(key, "Address") == 0 && strcmp(sig, "s") == 0) {
			dbus_r_string(r, dev->address, sizeof(dev->address));
		} else if ((strcmp(key, "Alias") == 0 || strcmp(key, "Name") == 0) && strcmp(sig, "s") == 0) {
			// Alias is what the user sees everywhere else, and bluez falls it
			// back to Name itself, so whichever arrives is the right answer --
			// except that Alias must win when both do.
			char text[BT_DEV_NAME_MAX];
			dbus_r_string(r, text, sizeof(text));
			if (text[0] && (strcmp(key, "Alias") == 0 || !dev->name[0])) {
				snprintf(dev->name, sizeof(dev->name), "%s", text);
			}
		} else if (strcmp(key, "Paired") == 0 && strcmp(sig, "b") == 0) {
			dbus_r_bool(r, &dev->paired);
		} else if (strcmp(key, "Trusted") == 0 && strcmp(sig, "b") == 0) {
			dbus_r_bool(r, &dev->trusted);
		} else if (strcmp(key, "Connected") == 0 && strcmp(sig, "b") == 0) {
			dbus_r_bool(r, &dev->connected);
		} else if (strcmp(key, "RSSI") == 0 && strcmp(sig, "n") == 0) {
			uint16_t raw = 0;
			dbus_r_u16(r, &raw);
			dev->rssi = (int)(int16_t)raw;
		} else if (strcmp(key, "Class") == 0 && strcmp(sig, "u") == 0) {
			// Carried by every BR/EDR inquiry result, so it is known before the
			// name is and even when the name never arrives.
			uint32_t raw = 0;
			dbus_r_u32(r, &raw);
			dev->cod = (unsigned)raw;
		} else if (strcmp(key, "UUIDs") == 0 && strcmp(sig, "as") == 0) {
			dbus_array_iter_t uuids;
			if (dbus_r_array_begin(r, "s", &uuids)) {
				while (dbus_r_array_more(r, &uuids)) {
					char uuid[48];
					if (!dbus_r_string(r, uuid, sizeof(uuid))) {
						break;
					}
					if (strcasecmp(uuid, UUID_A2DP_SINK) == 0) {
						dev->audio_sink = true;
					}
				}
			}
		} else if (!dbus_r_skip(r, sig)) {
			return;
		}
	}
}

// ---------------------------------------------------------------------------
// signals
// ---------------------------------------------------------------------------

static void note_change(void) {
	void (*cb)(void);
	pthread_mutex_lock(&lock);
	cb = change_cb;
	pthread_mutex_unlock(&lock);
	if (cb) {
		cb();
	}
}

static void on_signal(dbus_conn_t *c, const dbus_msg_t *m, void *user) {
	(void)c;
	(void)user;

	if (strcmp(m->interface, OBJMAN_IFACE) == 0 && strcmp(m->member, "InterfacesAdded") == 0) {
		dbus_reader_t r;
		dbus_reader_init(&r, m);
		char path[OBJECT_PATH_MAX];
		if (!dbus_r_string(&r, path, sizeof(path))) {
			return;
		}
		char address[BT_ADDR_MAX];
		if (path_is_a2dp_sink(path) && address_from_path(path, address, sizeof(address))) {
			pthread_mutex_lock(&lock);
			pcm_add(pcm_addr, &pcm_count, address);
			pthread_mutex_unlock(&lock);
			fprintf(stderr, "btstack: the A2DP sink on %s is ready\n", address);
		} else if (path_is_a2dp_source(path) && address_from_path(path, address, sizeof(address))) {
			pthread_mutex_lock(&lock);
			pcm_add(rx_addr, &rx_count, address);
			pthread_mutex_unlock(&lock);
			fprintf(stderr, "btstack: %s is streaming to this device\n", address);
		}
		note_change();
		return;
	}

	if (strcmp(m->interface, OBJMAN_IFACE) == 0 && strcmp(m->member, "InterfacesRemoved") == 0) {
		dbus_reader_t r;
		dbus_reader_init(&r, m);
		char path[OBJECT_PATH_MAX];
		if (!dbus_r_string(&r, path, sizeof(path))) {
			return;
		}
		char address[BT_ADDR_MAX];
		if (path_is_a2dp_sink(path) && address_from_path(path, address, sizeof(address))) {
			pthread_mutex_lock(&lock);
			pcm_remove(pcm_addr, &pcm_count, address);
			pthread_mutex_unlock(&lock);
			fprintf(stderr, "btstack: the A2DP sink on %s is gone\n", address);
		} else if (path_is_a2dp_source(path) && address_from_path(path, address, sizeof(address))) {
			pthread_mutex_lock(&lock);
			pcm_remove(rx_addr, &rx_count, address);
			pthread_mutex_unlock(&lock);
			fprintf(stderr, "btstack: %s has stopped streaming to this device\n", address);
		}
		note_change();
		return;
	}

	// The A2DP stream changing state, logged as it happens.
	//
	// Only an active stream is rendered: what is encoded into an idle one is
	// thrown away by the sink, and from inside the player that is
	// indistinguishable from working -- the writes go through, the queue drains
	// at the right rate, and nothing comes out of the headphones. bluez says so
	// in a signal, so there is no polling to pay for and the line lands at the
	// instant of the change, screen off included.
	if (strcmp(m->interface, PROPS_IFACE) == 0 && strcmp(m->member, "PropertiesChanged") == 0) {
		dbus_reader_t r;
		dbus_reader_init(&r, m);
		char iface[DBUS_NAME_MAX];
		if (!dbus_r_string(&r, iface, sizeof(iface))) {
			return;
		}

		if (strcmp(iface, BLUEZ_TRANSPORT_IFACE) == 0) {
			dbus_array_iter_t props;
			if (dbus_r_array_begin(&r, "{sv}", &props)) {
				while (dbus_r_array_more(&r, &props)) {
					char key[64], sig[16];
					if (!dbus_r_string(&r, key, sizeof(key)) || !dbus_r_signature(&r, sig, sizeof(sig))) {
						break;
					}
					if (strcmp(key, "State") == 0 && strcmp(sig, "s") == 0) {
						char state[32];
						if (dbus_r_string(&r, state, sizeof(state))) {
							const char *leaf = strrchr(m->path, '/');
							fprintf(stderr, "btstack: the A2DP stream %s -> %s\n", leaf ? leaf + 1 : m->path, state);
						}
					} else if (!dbus_r_skip(&r, sig)) {
						break;
					}
				}
			}
		}

		// What the remote player is doing, which is the one honest answer to
		// "should this key mean play or pause". bluez announces it the moment it
		// changes, whichever end caused the change -- a tap here, the space bar
		// over there, or the track ending -- so it is never stale by more than
		// the signal's own flight time.
		if (strcmp(iface, BLUEZ_MEDIA_PLAYER_IFACE) == 0) {
			dbus_array_iter_t props;
			if (dbus_r_array_begin(&r, "{sv}", &props)) {
				while (dbus_r_array_more(&r, &props)) {
					char key[64], sig[16];
					if (!dbus_r_string(&r, key, sizeof(key)) || !dbus_r_signature(&r, sig, sizeof(sig))) {
						break;
					}
					if (strcmp(key, "Status") == 0 && strcmp(sig, "s") == 0) {
						char status[32];
						if (dbus_r_string(&r, status, sizeof(status))) {
							pthread_mutex_lock(&lock);
							snprintf(media_status, sizeof(media_status), "%s", status);
							pthread_mutex_unlock(&lock);
							fprintf(stderr, "btstack: the remote player is %s\n", status);
						}
					} else if (strcmp(key, "Track") == 0 && strcmp(sig, "a{sv}") == 0) {
						// The sender announces the new track before the audio
						// changes over, so the page is right by the time the
						// first frame of it arrives.
						btstack_track_t fresh;
						memset(&fresh, 0, sizeof(fresh));
						read_track_props(&r, &fresh);
						store_track(&fresh);
					} else if (!dbus_r_skip(&r, sig)) {
						break;
					}
				}
			}
		}
	}

	// Anything else the matches let through -- a device that connected, an
	// adapter that was powered, a codec that was renegotiated -- is a reason to
	// re-read, and nothing here has to know which.
	note_change();
}

// ---------------------------------------------------------------------------
// the pairing agent
//
// bluez calls exactly one agent for a pairing, and the firmware starts
// `bt-agent` for the job. Two agents cannot both be the default, which is why
// the bring-up stops that one: from here on the player answers for itself.
//
// With NoInputNoOutput the only calls that actually arrive are
// RequestConfirmation (answered by accepting: the user has just tapped this
// device on the screen, which IS the confirmation) and AuthorizeService. The
// rest are implemented because an agent that does not reply leaves bluez
// waiting for its whole timeout, and a pairing that hangs for thirty seconds is
// indistinguishable from one that failed.
// ---------------------------------------------------------------------------

static void on_call(dbus_conn_t *c, const dbus_msg_t *m, void *user) {
	(void)user;

	if (strcmp(m->interface, BLUEZ_AGENT_IFACE) != 0) {
		dbus_error(c, m, "org.freedesktop.DBus.Error.UnknownInterface", "no such interface here");
		return;
	}

	// Everything that is answered by simply agreeing. An empty reply is what
	// bluez reads as "accepted".
	if (strcmp(m->member, "Release") == 0 || strcmp(m->member, "Cancel") == 0 ||
		strcmp(m->member, "RequestConfirmation") == 0 || strcmp(m->member, "RequestAuthorization") == 0 ||
		strcmp(m->member, "DisplayPasskey") == 0 || strcmp(m->member, "DisplayPinCode") == 0) {
		fprintf(stderr, "btstack: agent %s -> accepted\n", m->member);
		dbus_reply_begin(c, m, "");
		dbus_reply_send(c);
		return;
	}

	if (strcmp(m->member, "AuthorizeService") == 0) {
		// The UUID is logged rather than filtered. A pair of headphones asks to
		// be allowed A2DP and AVRCP, and often one or two vendor services on
		// top; refusing those has broken remote-control buttons on real
		// hardware, and the user authorised this device by tapping it.
		dbus_reader_t r;
		dbus_reader_init(&r, m);
		char path[OBJECT_PATH_MAX] = "", uuid[48] = "";
		dbus_r_string(&r, path, sizeof(path));
		dbus_r_string(&r, uuid, sizeof(uuid));
		fprintf(stderr, "btstack: agent authorising %s on %s\n", uuid, path);
		dbus_reply_begin(c, m, "");
		dbus_reply_send(c);
		return;
	}

	// A PIN or a passkey cannot be produced by a device with no keypad. Refused
	// explicitly, because that is an answer: bluez gives up at once instead of
	// waiting for a reply that was never coming.
	if (strcmp(m->member, "RequestPinCode") == 0 || strcmp(m->member, "RequestPasskey") == 0) {
		fprintf(stderr, "btstack: agent %s -> refused (no keypad on this device)\n", m->member);
		dbus_error(c, m, "org.bluez.Error.Rejected", "no input on this device");
		return;
	}

	dbus_error(c, m, "org.freedesktop.DBus.Error.UnknownMethod", "not implemented");
}

// ---------------------------------------------------------------------------
// opening and closing
// ---------------------------------------------------------------------------

static bool call_ok(const char *destination, const char *path, const char *interface, const char *member,
					int timeout_ms) {
	pthread_mutex_lock(&lock);
	dbus_conn_t *c = conn;
	pthread_mutex_unlock(&lock);
	if (!c) {
		return false;
	}
	dbus_call_begin(c, destination, path, interface, member, "");
	char err[DBUS_NAME_MAX];
	bool ok = dbus_call_send(c, timeout_ms, err, sizeof(err));
	if (!ok) {
		fprintf(stderr, "btstack: %s.%s -> %s\n", interface, member, err[0] ? err : "no reply");
	}
	return ok;
}

bool btstack_service_ready(const char *name) {
	pthread_mutex_lock(&lock);
	dbus_conn_t *c = conn;
	pthread_mutex_unlock(&lock);
	if (!c || !name) {
		return false;
	}
	dbus_writer_t *w =
		dbus_call_begin(c, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner", "s");
	dbus_w_string(w, name);
	if (!dbus_call_send(c, CALL_MS, NULL, 0)) {
		return false;
	}
	dbus_reader_t r;
	dbus_reply_reader(c, &r);
	bool owned = false;
	return dbus_r_bool(&r, &owned) && owned;
}

bool btstack_wait_service(const char *name, int timeout_ms) {
	for (int waited = 0;; waited += 100) {
		if (btstack_service_ready(name)) {
			return true;
		}
		if (waited >= timeout_ms) {
			return false;
		}
		sleep_ms(100);
	}
}

static bool register_agent(dbus_conn_t *c) {
	dbus_writer_t *w = dbus_call_begin(c, BLUEZ_SERVICE, BLUEZ_ROOT, BLUEZ_AGENT_MANAGER_IFACE, "RegisterAgent", "os");
	dbus_w_path(w, AGENT_PATH);
	dbus_w_string(w, AGENT_CAPABILITY);
	char err[DBUS_NAME_MAX];
	if (!dbus_call_send(c, CALL_MS, err, sizeof(err))) {
		// AlreadyExists is not a failure: a previous run of this player left the
		// registration behind and it is still ours.
		if (strstr(err, "AlreadyExists") == NULL) {
			fprintf(stderr, "btstack: RegisterAgent refused: %s\n", err[0] ? err : "no reply");
			return false;
		}
	}

	w = dbus_call_begin(c, BLUEZ_SERVICE, BLUEZ_ROOT, BLUEZ_AGENT_MANAGER_IFACE, "RequestDefaultAgent", "o");
	dbus_w_path(w, AGENT_PATH);
	if (!dbus_call_send(c, CALL_MS, err, sizeof(err))) {
		fprintf(stderr, "btstack: RequestDefaultAgent refused: %s\n", err[0] ? err : "no reply");
		return false;
	}

	fprintf(stderr, "btstack: agent registered at %s\n", AGENT_PATH);
	return true;
}

bool btstack_register_agent(void) {
	pthread_mutex_lock(&lock);
	dbus_conn_t *c = conn;
	pthread_mutex_unlock(&lock);
	return c && register_agent(c);
}

static const char *const MATCHES[] = {
	"type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager'",
	"type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
	"type='signal',sender='org.bluealsa',interface='org.freedesktop.DBus.ObjectManager'",
	"type='signal',sender='org.bluealsa',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
};

bool btstack_open(void) {
	pthread_mutex_lock(&lock);
	bool already = conn && dbus_alive(conn);
	pthread_mutex_unlock(&lock);
	if (already) {
		return true;
	}
	btstack_close();

	dbus_conn_t *c = dbus_connect_system(NULL, on_call, NULL);
	if (!c) {
		return false;
	}
	dbus_set_signal_handler(c, on_signal, NULL);
	for (size_t i = 0; i < sizeof(MATCHES) / sizeof(MATCHES[0]); i++) {
		// A rule naming a service that is not on the bus yet is still accepted:
		// dbus-daemon matches on the name, not on who owns it today.
		dbus_add_match(c, MATCHES[i]);
	}

	pthread_mutex_lock(&lock);
	conn = c;
	pthread_mutex_unlock(&lock);
	return true;
}

void btstack_close(void) {
	pthread_mutex_lock(&lock);
	dbus_conn_t *c = conn;
	conn = NULL;
	pcm_count = 0;
	pthread_mutex_unlock(&lock);
	if (c) {
		dbus_disconnect(c);
	}
}

bool btstack_alive(void) {
	pthread_mutex_lock(&lock);
	bool live = conn && dbus_alive(conn);
	pthread_mutex_unlock(&lock);
	return live;
}

void btstack_set_change_cb(void (*cb)(void)) {
	pthread_mutex_lock(&lock);
	change_cb = cb;
	pthread_mutex_unlock(&lock);
}

// ---------------------------------------------------------------------------
// properties
// ---------------------------------------------------------------------------

static bool set_bool_property(const char *path, const char *interface, const char *name, bool value) {
	pthread_mutex_lock(&lock);
	dbus_conn_t *c = conn;
	pthread_mutex_unlock(&lock);
	if (!c) {
		return false;
	}
	dbus_writer_t *w = dbus_call_begin(c, BLUEZ_SERVICE, path, PROPS_IFACE, "Set", "ssv");
	dbus_w_string(w, interface);
	dbus_w_string(w, name);
	dbus_w_variant_bool(w, value);
	char err[DBUS_NAME_MAX];
	bool ok = dbus_call_send(c, CALL_MS, err, sizeof(err));
	if (!ok) {
		fprintf(stderr, "btstack: %s = %s refused: %s\n", name, value ? "true" : "false",
				err[0] ? err : "no reply");
	}
	return ok;
}

static bool set_u32_property(const char *path, const char *interface, const char *name, uint32_t value) {
	pthread_mutex_lock(&lock);
	dbus_conn_t *c = conn;
	pthread_mutex_unlock(&lock);
	if (!c) {
		return false;
	}
	dbus_writer_t *w = dbus_call_begin(c, BLUEZ_SERVICE, path, PROPS_IFACE, "Set", "ssv");
	dbus_w_string(w, interface);
	dbus_w_string(w, name);
	dbus_w_variant_u32(w, value);
	char err[DBUS_NAME_MAX];
	bool ok = dbus_call_send(c, CALL_MS, err, sizeof(err));
	if (!ok) {
		fprintf(stderr, "btstack: %s = %u refused: %s\n", name, value, err[0] ? err : "no reply");
	}
	return ok;
}

static bool get_bool_property(const char *path, const char *interface, const char *name, bool *out) {
	pthread_mutex_lock(&lock);
	dbus_conn_t *c = conn;
	pthread_mutex_unlock(&lock);
	if (!c) {
		return false;
	}
	dbus_writer_t *w = dbus_call_begin(c, BLUEZ_SERVICE, path, PROPS_IFACE, "Get", "ss");
	dbus_w_string(w, interface);
	dbus_w_string(w, name);
	if (!dbus_call_send(c, CALL_MS, NULL, 0)) {
		return false;
	}
	dbus_reader_t r;
	dbus_reply_reader(c, &r);
	char sig[16];
	if (!dbus_r_signature(&r, sig, sizeof(sig)) || strcmp(sig, "b") != 0) {
		return false;
	}
	return dbus_r_bool(&r, out);
}

bool btstack_adapter_ready(void) {
	bool powered = false;
	// Any property of Adapter1 answering means bluetoothd is up AND has the
	// controller: the object does not carry the interface before then.
	return get_bool_property(BLUEZ_ADAPTER_PATH, BLUEZ_ADAPTER_IFACE, "Powered", &powered);
}

bool btstack_wait_adapter(int timeout_ms) {
	for (int waited = 0;; waited += 200) {
		if (btstack_adapter_ready()) {
			return true;
		}
		if (waited >= timeout_ms) {
			return false;
		}
		sleep_ms(200);
	}
}

bool btstack_set_powered(bool on) {
	return set_bool_property(BLUEZ_ADAPTER_PATH, BLUEZ_ADAPTER_IFACE, "Powered", on);
}

bool btstack_get_powered(bool *out) {
	return get_bool_property(BLUEZ_ADAPTER_PATH, BLUEZ_ADAPTER_IFACE, "Powered", out);
}

bool btstack_set_discoverable(bool on) {
	return set_bool_property(BLUEZ_ADAPTER_PATH, BLUEZ_ADAPTER_IFACE, "Discoverable", on);
}

// Seconds, and 0 means "until told otherwise". bluez defaults to 180 and turns
// visibility off by itself when they run out, which from the outside looks like
// the player disappearing in the middle of a search.
bool btstack_set_discoverable_timeout(unsigned seconds) {
	return set_u32_property(BLUEZ_ADAPTER_PATH, BLUEZ_ADAPTER_IFACE, "DiscoverableTimeout", seconds);
}

bool btstack_set_pairable(bool on) {
	return set_bool_property(BLUEZ_ADAPTER_PATH, BLUEZ_ADAPTER_IFACE, "Pairable", on);
}

bool btstack_set_alias(const char *alias) {
	pthread_mutex_lock(&lock);
	dbus_conn_t *c = conn;
	pthread_mutex_unlock(&lock);
	if (!c || !alias || !alias[0]) {
		return false;
	}
	dbus_writer_t *w = dbus_call_begin(c, BLUEZ_SERVICE, BLUEZ_ADAPTER_PATH, PROPS_IFACE, "Set", "ssv");
	dbus_w_string(w, BLUEZ_ADAPTER_IFACE);
	dbus_w_string(w, "Alias");
	dbus_w_variant_string(w, alias);
	return dbus_call_send(c, CALL_MS, NULL, 0);
}

bool btstack_discovery(bool on) {
	pthread_mutex_lock(&lock);
	dbus_conn_t *c = conn;
	pthread_mutex_unlock(&lock);
	if (!c) {
		return false;
	}
	dbus_call_begin(c, BLUEZ_SERVICE, BLUEZ_ADAPTER_PATH, BLUEZ_ADAPTER_IFACE, on ? "StartDiscovery" : "StopDiscovery",
					"");
	char err[DBUS_NAME_MAX];
	if (dbus_call_send(c, CALL_MS, err, sizeof(err))) {
		return true;
	}
	// Already discovering, or already stopped. Both mean the caller has what it
	// asked for, and treating them as failures is how a scan button that works
	// reports an error.
	if (strstr(err, "InProgress") || strstr(err, "NotAuthorized") || strstr(err, "Failed")) {
		fprintf(stderr, "btstack: discovery %s: %s (already in the wanted state)\n", on ? "on" : "off", err);
		return true;
	}
	fprintf(stderr, "btstack: discovery %s -> %s\n", on ? "on" : "off", err[0] ? err : "no reply");
	return false;
}

// ---------------------------------------------------------------------------
// devices
// ---------------------------------------------------------------------------

int btstack_devices(btstack_device_t *out, int max) {
	pthread_mutex_lock(&lock);
	dbus_conn_t *c = conn;
	pthread_mutex_unlock(&lock);
	if (!c || !out || max <= 0) {
		return 0;
	}

	dbus_call_begin(c, BLUEZ_SERVICE, "/", OBJMAN_IFACE, "GetManagedObjects", "");
	char err[DBUS_NAME_MAX];
	if (!dbus_call_send(c, CALL_MS, err, sizeof(err))) {
		fprintf(stderr, "btstack: GetManagedObjects -> %s\n", err[0] ? err : "no reply");
		return 0;
	}

	dbus_reader_t r;
	dbus_reply_reader(c, &r);

	dbus_array_iter_t objects;
	if (!dbus_r_array_begin(&r, "{oa{sa{sv}}}", &objects)) {
		return 0;
	}

	int count = 0;
	while (dbus_r_array_more(&r, &objects)) {
		char path[OBJECT_PATH_MAX];
		if (!dbus_r_string(&r, path, sizeof(path))) {
			break;
		}

		btstack_device_t dev;
		memset(&dev, 0, sizeof(dev));
		bool is_device = false;

		dbus_array_iter_t interfaces;
		if (!dbus_r_array_begin(&r, "{sa{sv}}", &interfaces)) {
			break;
		}
		while (dbus_r_array_more(&r, &interfaces)) {
			char iface[96];
			if (!dbus_r_string(&r, iface, sizeof(iface))) {
				break;
			}
			if (strcmp(iface, BLUEZ_DEVICE_IFACE) == 0) {
				is_device = true;
				read_device_props(&r, &dev);
			} else if (!dbus_r_skip(&r, "a{sv}")) {
				break;
			}
		}

		if (r.bad) {
			break;
		}
		if (!is_device) {
			continue;
		}
		// bluez always publishes Address, but a record without one cannot be
		// acted on and must not become a row with an empty address.
		if (!dev.address[0] && !address_from_path(path, dev.address, sizeof(dev.address))) {
			continue;
		}
		if (count < max) {
			out[count++] = dev;
		}
	}

	if (r.bad) {
		fprintf(stderr, "btstack: the GetManagedObjects reply stopped parsing after %d devices\n", count);
	}
	return count;
}

static bool device_call(const char *address, const char *member, int timeout_ms) {
	char path[OBJECT_PATH_MAX];
	if (!device_path(address, path, sizeof(path))) {
		fprintf(stderr, "btstack: '%s' is not an address\n", address ? address : "(null)");
		return false;
	}
	return call_ok(BLUEZ_SERVICE, path, BLUEZ_DEVICE_IFACE, member, timeout_ms);
}

bool btstack_pair(const char *address, int timeout_ms) { return device_call(address, "Pair", timeout_ms); }

bool btstack_connect(const char *address, int timeout_ms) { return device_call(address, "Connect", timeout_ms); }

bool btstack_disconnect(const char *address) { return device_call(address, "Disconnect", CALL_MS); }

bool btstack_trust(const char *address, bool on) {
	char path[OBJECT_PATH_MAX];
	if (!device_path(address, path, sizeof(path))) {
		return false;
	}
	return set_bool_property(path, BLUEZ_DEVICE_IFACE, "Trusted", on);
}

bool btstack_remove(const char *address) {
	pthread_mutex_lock(&lock);
	dbus_conn_t *c = conn;
	pthread_mutex_unlock(&lock);
	char path[OBJECT_PATH_MAX];
	if (!c || !device_path(address, path, sizeof(path))) {
		return false;
	}
	dbus_writer_t *w = dbus_call_begin(c, BLUEZ_SERVICE, BLUEZ_ADAPTER_PATH, BLUEZ_ADAPTER_IFACE, "RemoveDevice", "o");
	dbus_w_path(w, path);
	char err[DBUS_NAME_MAX];
	bool ok = dbus_call_send(c, CALL_MS, err, sizeof(err));
	if (!ok) {
		// A device bluez does not have is a device that has been forgotten,
		// which is what the caller wanted.
		if (strstr(err, "DoesNotExist")) {
			return true;
		}
		fprintf(stderr, "btstack: RemoveDevice %s -> %s\n", address, err[0] ? err : "no reply");
	}
	return ok;
}

// ---------------------------------------------------------------------------
// bluealsa
// ---------------------------------------------------------------------------

void btstack_refresh_audio(void) {
	pthread_mutex_lock(&lock);
	dbus_conn_t *c = conn;
	pthread_mutex_unlock(&lock);
	if (!c) {
		return;
	}

	dbus_call_begin(c, BALSA_SERVICE, "/", OBJMAN_IFACE, "GetManagedObjects", "");
	if (!dbus_call_send(c, CALL_MS, NULL, 0)) {
		// bluealsa not on the bus yet: no PCM can exist, which is exactly what
		// an empty list says.
		pthread_mutex_lock(&lock);
		pcm_count = 0;
		rx_count = 0;
		pthread_mutex_unlock(&lock);
		return;
	}

	dbus_reader_t r;
	dbus_reply_reader(c, &r);

	char found[MAX_PCMS][BT_ADDR_MAX];
	char found_rx[MAX_PCMS][BT_ADDR_MAX];
	int count = 0;
	int count_rx = 0;

	dbus_array_iter_t objects;
	if (dbus_r_array_begin(&r, "{oa{sa{sv}}}", &objects)) {
		while (dbus_r_array_more(&r, &objects)) {
			char path[OBJECT_PATH_MAX];
			if (!dbus_r_string(&r, path, sizeof(path)) || !dbus_r_skip(&r, "a{sa{sv}}")) {
				break;
			}
			char address[BT_ADDR_MAX];
			if (path_is_a2dp_sink(path) && address_from_path(path, address, sizeof(address)) && count < MAX_PCMS) {
				snprintf(found[count++], BT_ADDR_MAX, "%s", address);
			} else if (path_is_a2dp_source(path) && address_from_path(path, address, sizeof(address)) &&
					   count_rx < MAX_PCMS) {
				snprintf(found_rx[count_rx++], BT_ADDR_MAX, "%s", address);
			}
		}
	}

	pthread_mutex_lock(&lock);
	pcm_count = count;
	for (int i = 0; i < count; i++) {
		snprintf(pcm_addr[i], BT_ADDR_MAX, "%s", found[i]);
	}
	rx_count = count_rx;
	for (int i = 0; i < count_rx; i++) {
		snprintf(rx_addr[i], BT_ADDR_MAX, "%s", found_rx[i]);
	}
	pthread_mutex_unlock(&lock);
}

int btstack_codecs_dir(const char *address, bool receiving, char out[][BT_CODEC_NAME_MAX], int max, char *selected,
					   size_t selected_size) {
	if (selected && selected_size) {
		selected[0] = '\0';
	}
	pthread_mutex_lock(&lock);
	dbus_conn_t *c = conn;
	pthread_mutex_unlock(&lock);

	char path[OBJECT_PATH_MAX];
	if (!c || !pcm_path_dir(address, receiving, path, sizeof(path))) {
		return 0;
	}

	// What it is playing with now is a property; what it could play with is a
	// method. Two calls, and the first is the one that matters if the second is
	// not there -- an older bluealsa without GetCodecs still answers Codec.
	if (selected && selected_size) {
		dbus_writer_t *w = dbus_call_begin(c, BALSA_SERVICE, path, PROPS_IFACE, "Get", "ss");
		dbus_w_string(w, BALSA_PCM_IFACE);
		dbus_w_string(w, "Codec");
		if (dbus_call_send(c, CALL_MS, NULL, 0)) {
			dbus_reader_t r;
			dbus_reply_reader(c, &r);
			char sig[16];
			if (dbus_r_signature(&r, sig, sizeof(sig)) && strcmp(sig, "s") == 0) {
				dbus_r_string(&r, selected, selected_size);
			}
		}
	}

	if (!out || max <= 0) {
		return 0;
	}

	dbus_call_begin(c, BALSA_SERVICE, path, BALSA_PCM_IFACE, "GetCodecs", "");
	char err[DBUS_NAME_MAX];
	if (!dbus_call_send(c, CALL_MS, err, sizeof(err))) {
		fprintf(stderr, "btstack: GetCodecs on %s -> %s\n", address, err[0] ? err : "no reply");
		return 0;
	}

	dbus_reader_t r;
	dbus_reply_reader(c, &r);

	// a{sa{sv}}: the codec name is the key, its properties are of no interest
	// here.
	int count = 0;
	dbus_array_iter_t codecs;
	if (dbus_r_array_begin(&r, "{sa{sv}}", &codecs)) {
		while (dbus_r_array_more(&r, &codecs)) {
			char name[BT_CODEC_NAME_MAX];
			if (!dbus_r_string(&r, name, sizeof(name)) || !dbus_r_skip(&r, "a{sv}")) {
				break;
			}
			if (name[0] && count < max) {
				snprintf(out[count++], BT_CODEC_NAME_MAX, "%s", name);
			}
		}
	}
	return count;
}

// bluealsa spells the sample format as a word of its own: the top bit is the
// sign, the next the endianness, and the low byte the width in bits. Only the
// five it actually uses are named.
static const char *format_name(uint16_t format) {
	switch (format) {
	case 0x0108:
		return "8 bit";
	case 0x8210:
		return "16 bit";
	case 0x8318:
		return "24 bit (packed)";
	case 0x8418:
		return "24 bit";
	case 0x8420:
		return "32 bit";
	default:
		return "unknown format";
	}
}

bool btstack_audio_info(const char *address, char *out, size_t size) {
	if (out && size) {
		out[0] = '\0';
	}
	pthread_mutex_lock(&lock);
	dbus_conn_t *c = conn;
	pthread_mutex_unlock(&lock);

	char path[OBJECT_PATH_MAX];
	if (!c || !out || !size || !pcm_path(address, path, sizeof(path))) {
		return false;
	}

	dbus_writer_t *w = dbus_call_begin(c, BALSA_SERVICE, path, PROPS_IFACE, "GetAll", "s");
	dbus_w_string(w, BALSA_PCM_IFACE);
	if (!dbus_call_send(c, CALL_MS, NULL, 0)) {
		return false;
	}

	dbus_reader_t r;
	dbus_reply_reader(c, &r);

	char codec[BT_CODEC_NAME_MAX] = "?";
	uint32_t rate = 0;
	uint8_t channels = 0;
	uint16_t format = 0;

	dbus_array_iter_t props;
	if (!dbus_r_array_begin(&r, "{sv}", &props)) {
		return false;
	}
	while (dbus_r_array_more(&r, &props)) {
		char key[64], sig[16];
		if (!dbus_r_string(&r, key, sizeof(key)) || !dbus_r_signature(&r, sig, sizeof(sig))) {
			return false;
		}
		if (strcmp(key, "Codec") == 0 && strcmp(sig, "s") == 0) {
			dbus_r_string(&r, codec, sizeof(codec));
		} else if (strcmp(key, "Sampling") == 0 && strcmp(sig, "u") == 0) {
			dbus_r_u32(&r, &rate);
		} else if (strcmp(key, "Channels") == 0 && strcmp(sig, "y") == 0) {
			dbus_r_byte(&r, &channels);
		} else if (strcmp(key, "Format") == 0 && strcmp(sig, "q") == 0) {
			dbus_r_u16(&r, &format);
		} else if (!dbus_r_skip(&r, sig)) {
			return false;
		}
	}

	snprintf(out, size, "%s, %u Hz, %u ch, %s", codec, (unsigned)rate, (unsigned)channels, format_name(format));
	return !r.bad;
}

// The low byte of the format word is the width; the two the daemon spells with
// a note attached ("packed") carry the same width in the same place.
static unsigned format_bits(uint16_t format) {
	unsigned bits = format & 0xFFu;
	return (bits == 8 || bits == 16 || bits == 24 || bits == 32) ? bits : 0;
}

bool btstack_stream_info(const char *address, bool receiving, btstack_stream_t *out) {
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));

	pthread_mutex_lock(&lock);
	dbus_conn_t *c = conn;
	pthread_mutex_unlock(&lock);

	char path[OBJECT_PATH_MAX];
	if (!c || !pcm_path_dir(address, receiving, path, sizeof(path))) {
		return false;
	}

	dbus_writer_t *w = dbus_call_begin(c, BALSA_SERVICE, path, PROPS_IFACE, "GetAll", "s");
	dbus_w_string(w, BALSA_PCM_IFACE);
	if (!dbus_call_send(c, CALL_MS, NULL, 0)) {
		return false;
	}

	dbus_reader_t r;
	dbus_reply_reader(c, &r);

	uint32_t rate = 0;
	uint8_t channels = 0;
	uint16_t format = 0;

	dbus_array_iter_t props;
	if (!dbus_r_array_begin(&r, "{sv}", &props)) {
		return false;
	}
	while (dbus_r_array_more(&r, &props)) {
		char key[64], sig[16];
		if (!dbus_r_string(&r, key, sizeof(key)) || !dbus_r_signature(&r, sig, sizeof(sig))) {
			return false;
		}
		if (strcmp(key, "Codec") == 0 && strcmp(sig, "s") == 0) {
			dbus_r_string(&r, out->codec, sizeof(out->codec));
		} else if (strcmp(key, "Sampling") == 0 && strcmp(sig, "u") == 0) {
			dbus_r_u32(&r, &rate);
		} else if (strcmp(key, "Channels") == 0 && strcmp(sig, "y") == 0) {
			dbus_r_byte(&r, &channels);
		} else if (strcmp(key, "Format") == 0 && strcmp(sig, "q") == 0) {
			dbus_r_u16(&r, &format);
		} else if (!dbus_r_skip(&r, sig)) {
			return false;
		}
	}

	out->rate = (unsigned)rate;
	out->channels = (unsigned)channels;
	out->bits = format_bits(format);
	return !r.bad;
}

// ---------------------------------------------------------------------------
// AVRCP going out
// ---------------------------------------------------------------------------

// The player object the remote device registered, if it registered one. bluez
// numbers them per device (/player0, /player1 ...) and reuses the numbers
// across reconnections, so the path is looked up rather than remembered.
static bool remote_player_path(dbus_conn_t *c, const char *address, char *out, size_t size) {
	char prefix[OBJECT_PATH_MAX];
	if (!device_path(address, prefix, sizeof(prefix))) {
		return false;
	}
	size_t prefix_len = strlen(prefix);

	dbus_call_begin(c, BLUEZ_SERVICE, "/", OBJMAN_IFACE, "GetManagedObjects", "");
	if (!dbus_call_send(c, CALL_MS, NULL, 0)) {
		return false;
	}

	dbus_reader_t r;
	dbus_reply_reader(c, &r);

	dbus_array_iter_t objects;
	if (!dbus_r_array_begin(&r, "{oa{sa{sv}}}", &objects)) {
		return false;
	}
	while (dbus_r_array_more(&r, &objects)) {
		char path[OBJECT_PATH_MAX];
		if (!dbus_r_string(&r, path, sizeof(path))) {
			return false;
		}

		// Only the interface names matter here, so the property dictionaries
		// are stepped over rather than read.
		bool is_player = false;
		dbus_array_iter_t ifaces;
		if (dbus_r_array_begin(&r, "{sa{sv}}", &ifaces)) {
			while (dbus_r_array_more(&r, &ifaces)) {
				char iface[DBUS_NAME_MAX];
				if (!dbus_r_string(&r, iface, sizeof(iface)) || !dbus_r_skip(&r, "a{sv}")) {
					return false;
				}
				if (strcmp(iface, BLUEZ_MEDIA_PLAYER_IFACE) == 0) {
					is_player = true;
				}
			}
		}

		if (is_player && strncmp(path, prefix, prefix_len) == 0 && path[prefix_len] == '/') {
			snprintf(out, size, "%s", path);
			return true;
		}
	}
	return false;
}

bool btstack_media_track(btstack_track_t *out) {
	if (!out) {
		return false;
	}
	pthread_mutex_lock(&lock);
	*out = media_track;
	pthread_mutex_unlock(&lock);
	return out->title[0] != '\0' || out->artist[0] != '\0';
}

unsigned btstack_media_serial(void) {
	pthread_mutex_lock(&lock);
	unsigned now = media_track_serial;
	pthread_mutex_unlock(&lock);
	return now;
}

bool btstack_media_status(char *out, size_t size) {
	if (!out || !size) {
		return false;
	}
	pthread_mutex_lock(&lock);
	snprintf(out, size, "%s", media_status);
	pthread_mutex_unlock(&lock);
	return out[0] != '\0';
}

// The player object of the device that is streaming, remembered between polls.
// Looking it up costs a GetManagedObjects -- every object bluez has, with every
// property of each -- and this is asked once a second while a device is
// streaming. The path is only worth what the Get on it answers, so a Get that
// fails throws it away and the next poll looks it up again.
static char media_player_path[OBJECT_PATH_MAX];
static char media_player_address[BT_ADDR_MAX];

static bool player_property(dbus_conn_t *c, const char *path, const char *name, const char *expect,
							dbus_reader_t *out) {
	dbus_writer_t *w = dbus_call_begin(c, BLUEZ_SERVICE, path, PROPS_IFACE, "Get", "ss");
	dbus_w_string(w, BLUEZ_MEDIA_PLAYER_IFACE);
	dbus_w_string(w, name);
	if (!dbus_call_send(c, CALL_MS, NULL, 0)) {
		return false;
	}

	dbus_reply_reader(c, out);
	char sig[16];
	return dbus_r_signature(out, sig, sizeof(sig)) && strcmp(sig, expect) == 0;
}

void btstack_refresh_media_status(const char *address) {
	pthread_mutex_lock(&lock);
	dbus_conn_t *c = conn;
	pthread_mutex_unlock(&lock);
	if (!c || !address || !address[0]) {
		return;
	}

	char path[OBJECT_PATH_MAX];
	if (strcmp(media_player_address, address) == 0 && media_player_path[0]) {
		snprintf(path, sizeof(path), "%s", media_player_path);
	} else if (remote_player_path(c, address, path, sizeof(path))) {
		snprintf(media_player_path, sizeof(media_player_path), "%s", path);
		snprintf(media_player_address, sizeof(media_player_address), "%s", address);
	} else {
		media_player_path[0] = '\0';
		return;
	}

	char status[32] = "";
	btstack_track_t track;
	bool answered_status = false;
	bool answered_track = false;
	memset(&track, 0, sizeof(track));

	dbus_reader_t r;
	if (player_property(c, path, "Status", "s", &r)) {
		answered_status = dbus_r_string(&r, status, sizeof(status));
	}

	// And what it is playing. Read here as well as caught on the signal:
	// connecting to a device that is already playing brings no announcement
	// with it, and the page would sit empty until the next track.
	if (player_property(c, path, "Track", "a{sv}", &r)) {
		read_track_props(&r, &track);
		answered_track = true;
	}

	if (!answered_status && !answered_track) {
		media_player_path[0] = '\0'; // the object went away or was renumbered
		return;
	}

	// Only an answer replaces a cache. A Get can be refused while the link is
	// renegotiating, and that says nothing about what the device is playing;
	// writing an empty track through on it wipes what the announcement delivered
	// a moment earlier. What empties the caches is the sender going away, which
	// btstack_forget_media() is for.
	if (answered_track) {
		store_track(&track);
	}

	if (answered_status) {
		pthread_mutex_lock(&lock);
		snprintf(media_status, sizeof(media_status), "%s", status);
		pthread_mutex_unlock(&lock);
	}
}

void btstack_forget_media(void) {
	media_player_path[0] = '\0';
	media_player_address[0] = '\0';

	btstack_track_t empty;
	memset(&empty, 0, sizeof(empty));
	store_track(&empty);

	pthread_mutex_lock(&lock);
	media_status[0] = '\0';
	pthread_mutex_unlock(&lock);
}

void btstack_note_media_status(const char *status) {
	pthread_mutex_lock(&lock);
	snprintf(media_status, sizeof(media_status), "%s", status ? status : "");
	pthread_mutex_unlock(&lock);
}

bool btstack_media_command(const char *address, const char *member) {
	pthread_mutex_lock(&lock);
	dbus_conn_t *c = conn;
	pthread_mutex_unlock(&lock);
	if (!c || !member) {
		return false;
	}

	char path[OBJECT_PATH_MAX];
	if (remote_player_path(c, address, path, sizeof(path))) {
		if (call_ok(BLUEZ_SERVICE, path, BLUEZ_MEDIA_PLAYER_IFACE, member, CALL_MS)) {
			return true;
		}
	}

	// No player object, or it refused. MediaControl1 on the device itself is
	// the older road to the same AVRCP command and is still there in bluez 5:
	// it is deprecated, not absent, and a phone that registers no player still
	// answers it.
	if (!device_path(address, path, sizeof(path))) {
		return false;
	}
	return call_ok(BLUEZ_SERVICE, path, BLUEZ_MEDIA_CONTROL_IFACE, member, CALL_MS);
}

// What bluez says about the A2DP streams on a device: one "fdN=state" per
// transport object, states being idle (configured but not streaming), pending
// (being acquired) or active (streaming).
//
// It answers the one question this side of the link cannot: the player can
// write at exactly the right rate and bluealsa can encode every frame of it,
// and if the stream is not active none of it is being rendered. Several
// entries at once are worth seeing too -- bluez keeps a transport it created
// for a configuration the encoder then refused, and that orphan goes on
// existing next to the live one.
bool btstack_a2dp_streams(const char *address, char *out, size_t size) {
	if (out && size) {
		out[0] = '\0';
	}
	pthread_mutex_lock(&lock);
	dbus_conn_t *c = conn;
	pthread_mutex_unlock(&lock);

	char dev[OBJECT_PATH_MAX];
	if (!c || !out || !size || !device_path(address, dev, sizeof(dev))) {
		return false;
	}
	size_t dev_len = strlen(dev);

	dbus_call_begin(c, BLUEZ_SERVICE, "/", OBJMAN_IFACE, "GetManagedObjects", "");
	if (!dbus_call_send(c, CALL_MS, NULL, 0)) {
		return false;
	}

	dbus_reader_t r;
	dbus_reply_reader(c, &r);

	dbus_array_iter_t objects;
	if (!dbus_r_array_begin(&r, "{oa{sa{sv}}}", &objects)) {
		return false;
	}

	size_t used = 0;
	int found = 0;
	while (dbus_r_array_more(&r, &objects)) {
		char path[OBJECT_PATH_MAX];
		if (!dbus_r_string(&r, path, sizeof(path))) {
			break;
		}
		bool mine = strncmp(path, dev, dev_len) == 0 && path[dev_len] == '/';

		dbus_array_iter_t interfaces;
		if (!dbus_r_array_begin(&r, "{sa{sv}}", &interfaces)) {
			break;
		}
		while (dbus_r_array_more(&r, &interfaces)) {
			char iface[96];
			if (!dbus_r_string(&r, iface, sizeof(iface))) {
				break;
			}
			if (!mine || strcmp(iface, BLUEZ_TRANSPORT_IFACE) != 0) {
				if (!dbus_r_skip(&r, "a{sv}")) {
					break;
				}
				continue;
			}

			char state[32] = "?";
			dbus_array_iter_t props;
			if (!dbus_r_array_begin(&r, "{sv}", &props)) {
				break;
			}
			while (dbus_r_array_more(&r, &props)) {
				char key[64], sig[16];
				if (!dbus_r_string(&r, key, sizeof(key)) || !dbus_r_signature(&r, sig, sizeof(sig))) {
					break;
				}
				if (strcmp(key, "State") == 0 && strcmp(sig, "s") == 0) {
					dbus_r_string(&r, state, sizeof(state));
				} else if (!dbus_r_skip(&r, sig)) {
					break;
				}
			}

			const char *leaf = strrchr(path, '/');
			leaf = leaf ? leaf + 1 : path;
			int n = snprintf(out + used, size - used, "%s%s=%s", used ? " " : "", leaf, state);
			if (n > 0 && (size_t)n < size - used) {
				used += (size_t)n;
			}
			found++;
		}
		if (r.bad) {
			break;
		}
	}

	if (!found) {
		snprintf(out, size, "no stream");
	}
	return !r.bad;
}

bool btstack_bluealsa_version(char *out, size_t size) {
	if (out && size) {
		out[0] = '\0';
	}
	pthread_mutex_lock(&lock);
	dbus_conn_t *c = conn;
	pthread_mutex_unlock(&lock);
	if (!c || !out || !size) {
		return false;
	}

	dbus_writer_t *w = dbus_call_begin(c, BALSA_SERVICE, BALSA_MANAGER_PATH, PROPS_IFACE, "Get", "ss");
	dbus_w_string(w, BALSA_MANAGER_IFACE);
	dbus_w_string(w, "Version");
	if (!dbus_call_send(c, CALL_MS, NULL, 0)) {
		return false;
	}

	dbus_reader_t r;
	dbus_reply_reader(c, &r);
	char sig[16];
	if (!dbus_r_signature(&r, sig, sizeof(sig)) || strcmp(sig, "s") != 0) {
		return false;
	}
	char raw[32];
	if (!dbus_r_string(&r, raw, sizeof(raw)) || !raw[0]) {
		return false;
	}
	// bluealsa spells it "v4.3.1"; hand back the number so the caller decides
	// how to print it.
	snprintf(out, size, "%s", raw[0] == 'v' ? raw + 1 : raw);
	return out[0] != '\0';
}

bool btstack_select_codec_dir(const char *address, bool receiving, const char *codec) {
	pthread_mutex_lock(&lock);
	dbus_conn_t *c = conn;
	pthread_mutex_unlock(&lock);

	char path[OBJECT_PATH_MAX];
	if (!c || !codec || !codec[0] || !pcm_path_dir(address, receiving, path, sizeof(path))) {
		return false;
	}

	dbus_writer_t *w = dbus_call_begin(c, BALSA_SERVICE, path, BALSA_PCM_IFACE, "SelectCodec", "sa{sv}");
	dbus_w_string(w, codec);
	dbus_array_t props;
	dbus_w_array_begin(w, "{sv}", &props); // no properties: bluealsa picks the defaults
	dbus_w_array_end(w, &props);

	char err[DBUS_NAME_MAX];
	// Renegotiating the codec tears the stream down and builds it again, which
	// the headphones take a moment to follow.
	bool ok = dbus_call_send(c, 15000, err, sizeof(err));
	fprintf(stderr, "btstack: codec %s on %s (%s) -> %s\n", codec, address, receiving ? "sink" : "source",
			ok ? "ok" : (err[0] ? err : "no reply"));
	return ok;
}

// The PCM's own volume, on either direction's stream.
//
// On the receiving side this is the level the sending phone sets over AVRCP and
// bluealsa applies to the decoded stream. Renegotiating the codec builds a new
// PCM, and the new one starts at bluealsa's default: the phone does not resend
// what it had already set, so without carrying the level across, the phone's
// volume keys appear to stop working after a codec change.
bool btstack_pcm_volume_get(const char *address, bool receiving, int *out) {
	pthread_mutex_lock(&lock);
	dbus_conn_t *c = conn;
	pthread_mutex_unlock(&lock);

	char path[OBJECT_PATH_MAX];
	if (!c || !out || !pcm_path_dir(address, receiving, path, sizeof(path))) {
		return false;
	}

	dbus_writer_t *w = dbus_call_begin(c, BALSA_SERVICE, path, PROPS_IFACE, "Get", "ss");
	dbus_w_string(w, BALSA_PCM_IFACE);
	dbus_w_string(w, "Volume");

	char err[DBUS_NAME_MAX];
	if (!dbus_call_send(c, CALL_MS, err, sizeof(err))) {
		return false;
	}

	dbus_reader_t r;
	dbus_reply_reader(c, &r);
	char sig[16];
	uint16_t both = 0;
	if (!dbus_r_signature(&r, sig, sizeof(sig)) || sig[0] != 'q' || !dbus_r_u16(&r, &both)) {
		return false;
	}
	// Two channels in one word, and the top bit of each byte is its mute
	// switch. The word is carried whole rather than taken apart: writing back
	// exactly what was read cannot get the mute or the balance wrong.
	*out = (int)both;
	return true;
}

bool btstack_pcm_volume_set(const char *address, bool receiving, int volume) {
	pthread_mutex_lock(&lock);
	dbus_conn_t *c = conn;
	pthread_mutex_unlock(&lock);

	char path[OBJECT_PATH_MAX];
	if (!c || volume < 0 || !pcm_path_dir(address, receiving, path, sizeof(path))) {
		return false;
	}

	dbus_writer_t *w = dbus_call_begin(c, BALSA_SERVICE, path, PROPS_IFACE, "Set", "ssv");
	dbus_w_string(w, BALSA_PCM_IFACE);
	dbus_w_string(w, "Volume");
	dbus_w_variant_u16(w, (uint16_t)volume);

	char err[DBUS_NAME_MAX];
	bool ok = dbus_call_send(c, CALL_MS, err, sizeof(err));
	if (!ok) {
		fprintf(stderr, "btstack: volume not restored on %s: %s\n", path, err[0] ? err : "no reply");
	}
	return ok;
}

// The headphone direction, which is what everything but the receiver page asks
// about.
int btstack_codecs(const char *address, char out[][BT_CODEC_NAME_MAX], int max, char *selected, size_t selected_size) {
	return btstack_codecs_dir(address, false, out, max, selected, selected_size);
}

bool btstack_select_codec(const char *address, const char *codec) {
	return btstack_select_codec_dir(address, false, codec);
}
