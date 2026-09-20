#ifndef BLUETOOTH_H
#define BLUETOOTH_H

#include <stdbool.h>
#include <stdint.h>

// The Bluetooth radio, driven by the player itself.
//
// Nothing here goes through the firmware's scripts or the bluez-tools.
// This file is the policy -- what the interface asks for and what the user's
// settings mean -- and btstack.h underneath it is the stack: one D-Bus
// connection to bluez and bluealsa, the player's own pairing agent, and a
// bring-up that starts brcm_patchram_plus, bluetoothd and bluealsa itself and
// waits on each of them rather than sleeping.
//
// The order matters and is kept in one place. Up: radio, firmware, hci0,
// bluetoothd, agent, bluealsa, adapter powered. Down: the exact reverse, with
// the audio closed before the daemon that carries it -- which is what the
// firmware's bt_suspend gets backwards.
//
// The stack is taken down when Bluetooth is switched off rather than left
// resident, because an unused one costs both memory and current on a device
// with neither to spare. Bringing it back takes a couple of seconds, which is
// what bluetooth_busy() is for.
//
// Everything here is safe to call from the interface thread; the blocking work
// happens on this module's own worker.

#define BT_MAX_DEVICES 24
#define BT_NAME_MAX 64
#define BT_MAC_MAX 20

typedef enum {
	BT_STATE_OFF = 0,
	BT_STATE_ON,		// powered, nothing connected
	BT_STATE_CONNECTED, // at least one device is on the other end
} bt_state_t;

typedef struct {
	char mac[BT_MAC_MAX];
	char name[BT_NAME_MAX];
	bool paired;
	bool connected;
	bool trusted;
	int rssi; // scan results only, 0 when unknown
} bt_device_t;

typedef enum {
	BT_OP_IDLE = 0,
	BT_OP_BUSY,
	BT_OP_OK,
	BT_OP_FAILED,
} bt_op_t;

// Starts the worker and, when the switch was left on, brings the radio up.
void bluetooth_init(void);

// False when the device has no Bluetooth at all.
bool bluetooth_available(void);

bool bluetooth_get_enabled(void);
void bluetooth_set_enabled(bool on);

// The same switch without recording it as the user's choice: for the idle park,
// which turns the radio off to save current and turns it back on at the next
// wake. bluetooth_get_enabled() follows it, so everything that asks "is
// Bluetooth up" sees the truth while it is parked.
void bluetooth_set_enabled_transient(bool on);

// Call after a resume from suspend-to-RAM. The daemons live through `mem` --
// they are only processes -- but the radio under them does not, and nothing
// tells them so: the stack looks up and answers nothing. This puts it back,
// and goes to the headphones the resume interrupted. Does nothing when
// Bluetooth is off, and nothing when the radio was parked (the unpark already
// rebuilds it).
void bluetooth_notify_resume(void);

// True while the radio is coming up or going down -- ten seconds, the first
// time, because the whole stack has to be reloaded.
bool bluetooth_busy(void);

bt_state_t bluetooth_get_state(void);

// The name of the device as other phones see it.
const char *bluetooth_local_name(void);

// Renames the player. Kept in the config, so it survives a reboot and outlives
// the firmware's own name file, which is on a read-only filesystem. False for a
// name that is empty once its spaces are taken off. The adapter is told at once
// when the radio is on, and at the next bring-up when it is not.
bool bluetooth_set_local_name(const char *name);

// Paired devices, then the ones a scan has turned up. Bumped whenever either
// list changes.
uint32_t bluetooth_devices_serial(void);
int bluetooth_get_paired(bt_device_t *out, int max);
int bluetooth_get_found(bt_device_t *out, int max);

// The device on the other end right now, if there is one.
bool bluetooth_connected_device(bt_device_t *out);

void bluetooth_scan_start(void);
void bluetooth_scan_stop(void);
bool bluetooth_scan_running(void);

// Re-reads what bluez knows now. bluez announces its own changes, so this is
// only for the moments the interface wants a fresh answer immediately.
void bluetooth_refresh(void);

void bluetooth_pair(const char *mac);
void bluetooth_connect(const char *mac);
void bluetooth_disconnect(const char *mac);
void bluetooth_forget(const char *mac);

// The outcome of the last action the user asked for, reported once.
bt_op_t bluetooth_take_op_result(char *name_out, int name_size);

// Puts the radio and its daemons down, in order, without opening or touching
// the system bus. For the one moment that needs it: the boot of a player whose
// Bluetooth switch is off, on a firmware whose init script started the whole
// stack anyway. The firmware's own bt_suspend cannot be used for this -- it
// ends with `killall dbus-daemon`, and the bus it would take away is the one
// this player's media controls and volume bridge live on.
void bluetooth_power_down_stack(void);

// ---------------------------------------------------------------------------
// A2DP
//
// bluealsa runs as an A2DP *source* (this is how the bring-up starts it): it
// takes a PCM stream and encodes it for the headphones. Playing through it is a
// matter of opening the right ALSA device -- the firmware ships the bluealsa
// plugin and /etc/alsa/conf.d/20-bluealsa.conf, so
// "bluealsa:DEV=<mac>,PROFILE=a2dp" is a PCM name alsa-lib already understands,
// `plug` wrapper and all.
//
// The codec is negotiated by bluealsa when the link comes up and can be
// changed afterwards; which ones are on offer depends on what the headphones
// answered with. The list is asked of bluealsa itself, over
// org.bluealsa.PCM1.GetCodecs, so it says what this build actually carries
// rather than what a table here claims.
// ---------------------------------------------------------------------------

#define BT_MAX_CODECS 12
#define BT_CODEC_MAX 24

// True when there is an A2DP sink to write to -- i.e. when the player should
// open the bluealsa PCM rather than the jack. There is deliberately no on/off
// switch beside it: stock has none either, connected headphones are the
// output, and a toggle that leaves the music in the jack while they are
// connected only confuses.
bool bluetooth_audio_active(void);

// The ALSA device name to play through, or an empty string when the audio
// belongs to the jack. Safe to call from the interface thread.
void bluetooth_output_pcm(char *out, int size);

// What bluez says about the A2DP streams on the connected sink: "fdN=state"
// per transport, idle, pending or active. Only an active stream is being
// rendered, so this is what separates "the headphones are silent" from "the
// write path is broken", which from inside the player look the same.
//
// Split in two on purpose. The question costs a GetManagedObjects and is
// answered on the Bluetooth worker; request() asks for a fresh answer and
// returns at once, streams() copies the last one and says how old it is. The
// playback thread must never wait on D-Bus.
void bluetooth_request_a2dp_streams(void);
bool bluetooth_a2dp_streams(char *out, int size, int *age_ms);

// The codecs the connected sink offers and the one in use. The list is read
// on the worker; this only copies what it last found.
//
// The player picks the best of them by itself at every connection and there is
// no setting in front of that. bluetooth_set_codec() moves the link that is up
// onto something else and is not remembered: the next connection starts from the
// top again.
//
// Only the headphone direction. As a receiver the phone is the source and the
// choice is the phone's; nothing here can move it.
int bluetooth_get_codecs(char out[][BT_CODEC_MAX], int max, char *selected, int selected_size);
void bluetooth_refresh_codecs(void);
void bluetooth_set_codec(const char *codec);

// What THIS player can encode and decode, which is a property of the bluealsa
// binary the firmware ships and is known as soon as the stack comes up --
// before anything connects, and whether or not anything ever does. Read out of
// the daemon's own help.
//
// `receiving` picks the decoding list, which need not be the same as the
// encoding one: encoder and decoder are different libraries and a build can have
// one without the other.
int bluetooth_local_codecs(char out[][BT_CODEC_MAX], int max, bool receiving);

// Whether other devices can find this one in a search.
//
// Off everywhere except the Bluetooth list page, which is the only place where
// being found is what the user is there for. Pairing from this side does not
// need it, and a player permanently visible to every phone in the room is not
// what anybody asked for.
void bluetooth_set_discoverable(bool on);

// LDAC's bit rate, which is a daemon setting rather than something chosen per
// link: bluealsa takes it as --ldac-quality at start-up. The names are the
// daemon's own.
//
//   "abr"       adaptive, the rate follows the link
//   "high"      990 kbps
//   "standard"  660 kbps
//   "mobile"    330 kbps
//
// Changing it restarts bluealsa when the radio is on, because that is the only
// moment the option is read. The link comes back by itself; the audio stops for
// as long as that takes.
const char *bluetooth_ldac_quality(void);
void bluetooth_set_ldac_quality(const char *mode);

// ---------------------------------------------------------------------------
// The other direction: a device streaming to this one
//
// bluealsa is started as an A2DP sink as well as a source, so a phone or a
// computer can connect and send. What it publishes then is a capture PCM, and
// btreceiver.h is what reads from it; these three are the questions that have
// to be asked over D-Bus and therefore have to be answered on the worker.
// ---------------------------------------------------------------------------

typedef struct {
	char codec[BT_CODEC_MAX];
	unsigned rate; // Hz, 0 when bluealsa has not said
	unsigned channels;
	unsigned bits;
} bt_stream_t;

// The device streaming to this one, if there is one. Both names may be NULL.
bool bluetooth_receiver_device(char *mac_out, int mac_size, char *name_out, int name_size);

// Asks the worker for a fresh reading of what that stream is carrying; the
// answer lands in the cache the call below copies. Split for the same reason
// the A2DP stream state is: the question costs a D-Bus round trip and the
// thread that wants the answer is a capture loop that must not wait.
//
// The serial is what makes the two usable together. Copying the cache says
// nothing about WHEN it was filled, and after a codec change the difference
// matters more than the contents: reading it straight after asking gives back
// the format of the transport that has just been torn down, which opens a PCM
// that will never carry a frame. Read the serial before asking and wait for it
// to move, and the answer is one that came after the question.
void bluetooth_refresh_receiver(void);
bool bluetooth_receiver_stream(bt_stream_t *out);

// The title, artist and album the other end is playing.
typedef struct {
	char title[128];
	char artist[128];
	char album[128];
	unsigned duration_ms; // 0 when the sender did not say
} bt_track_t;

// The codecs the sending device offered on the link that is up, and the one it
// is using. Read on the spot rather than cached: only the receiver page asks.
// Returns how many names landed in `out`.
int bluetooth_receiver_codecs(char out[][BT_CODEC_MAX], int max, char *selected, int selected_size);

// What the sending device says it is playing, out of AVRCP's track metadata.
// False when it has said nothing -- a laptop streaming system audio usually
// has no track to name.
bool bluetooth_receiver_track(bt_track_t *out);

// Moves the incoming stream onto another of them. The link is renegotiated, so
// the audio stops for a moment and comes back on the new codec.
void bluetooth_receiver_set_codec(const char *codec);
unsigned bluetooth_receiver_stream_serial(void);

// An AVRCP transport command sent to that device: "Play", "Pause", "Next" or
// "Previous". Queued on the worker and returns at once.
void bluetooth_receiver_command(const char *member);

// Whether that device says it is playing. Read from the cache bluez's own
// PropertiesChanged keeps, so it costs nothing and is safe on any thread --
// which matters, because the one thing that asks is a key press.
bool bluetooth_receiver_playing(void);

// Says what the answer above will be until the sender confirms it. The command
// is queued and the confirmation is two round trips away; without this, a
// second press in that window reads the old state and sends the same command
// again, so a pause pressed twice stays paused.
void bluetooth_receiver_note_playing(bool playing);

// Whether the player's volume and the headphones' are one level or two.
//
// On, they are the same: the keys here move the headphones over AVRCP absolute
// volume, and their own buttons move the number on the screen. Off, they are
// two independent levels -- bluealsa attenuates the stream on this side and the
// headphones keep whatever their own buttons say. Persisted under [wireless]
// bt_volume_sync; btvolume.h has how each is arranged.
bool bluetooth_volume_sync(void);
void bluetooth_set_volume_sync(bool on);

// Tells the radio the volume changed. Cheap and safe to call on every step: it
// does nothing unless a device is connected, and the worker coalesces a held
// key down to one write.
void bluetooth_notify_volume(int percent);

#endif /* BLUETOOTH_H */
