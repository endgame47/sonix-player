#ifndef BTSTACK_H
#define BTSTACK_H

#include <stdbool.h>
#include <stddef.h>

// The player's own Bluetooth stack: one D-Bus connection that speaks to bluez
// and to bluealsa directly, in place of the bluez-tools binaries the firmware
// ships.
//
// What this replaces. The firmware's own arrangement makes everything Bluetooth
// a fork and an exec -- `bt-adapter --set Powered On`, `bt-device -l`,
// `bt-device -z <mac>`, `bluealsa-cli codec <path>` -- with the answer read back
// either from an exit status or from JSON the tool leaves in /data/bt_list.txt.
// It has three faults that no amount of care in the caller can fix:
//
//   a state that is polled is a state that is late. A list re-read every few
//   seconds never sees a connection that comes and goes between two reads;
//
//   a device name that came off the air is handed to another program's argument
//   list, and the answer is parsed out of its prose;
//
//   and pairing needs an agent. `bt-agent` is a second process holding the
//   registration, so the player cannot answer a pairing request itself, and
//   cannot tell a pairing that was refused from one nobody answered.
//
// So: one connection, one place. bluez says what happened through
// InterfacesAdded, InterfacesRemoved and PropertiesChanged the moment it
// happens; the agent lives here and answers the confirmations itself; and the
// audio profile is not guessed from an ACL link but read from bluealsa's own
// PCM objects, which exist exactly when there is something to write to.
//
// Threading. Every call in this file blocks and belongs to one thread -- the
// Bluetooth worker. Signals arrive on the D-Bus reader thread and only ever
// update a cache and ring the change callback; they never make a call of their
// own, because the reply would have to be read by the thread that is inside the
// handler.

#define BT_ADDR_MAX 20
#define BT_DEV_NAME_MAX 64
#define BT_CODEC_NAME_MAX 24

typedef struct {
	char address[BT_ADDR_MAX]; // AA:BB:CC:DD:EE:FF, as bluez spells it
	char name[BT_DEV_NAME_MAX];
	bool paired;
	bool trusted;
	bool connected; // the ACL link, not the audio profile
	bool audio_sink; // advertises the A2DP sink UUID: headphones, not a phone
	unsigned cod;	 // class of device, 0 when bluez has none (every LE-only device)
	int rssi;		 // 0 when bluez does not have one
} btstack_device_t;

// ---------------------------------------------------------------------------
// the connection
// ---------------------------------------------------------------------------

// Opens the system bus, subscribes to what bluez and bluealsa announce, and
// registers the player's pairing agent. Safe to call when it is already open.
bool btstack_open(void);

// Closes the connection and forgets everything cached from it.
void btstack_close(void);

// Whether a name currently has an owner on the bus. This, and not a process
// list, is what "bluetoothd is up" and "bluealsa is up" mean: a process exists
// well before it has claimed its name and answered anything.
bool btstack_service_ready(const char *name);
bool btstack_wait_service(const char *name, int timeout_ms);

// Hands bluez the player's pairing agent and asks to be the default one. Call
// after bluetoothd has claimed org.bluez; before that there is nobody to
// register with. Safe to call again -- an agent already registered stays.
bool btstack_register_agent(void);

// True while the socket is live. Goes false when dbus-daemon exits, which is
// the signal to open again.
bool btstack_alive(void);

// Called on the D-Bus reader thread whenever bluez or bluealsa announced a
// change. Record and return: it must not block and must not call back in here.
void btstack_set_change_cb(void (*cb)(void));

// ---------------------------------------------------------------------------
// the adapter
// ---------------------------------------------------------------------------

// True once /org/bluez/hci0 carries org.bluez.Adapter1 -- the real "bluetoothd
// is up and has found the controller", as opposed to "the process exists".
bool btstack_adapter_ready(void);

// The same, waited for. Returns as soon as it is true.
bool btstack_wait_adapter(int timeout_ms);

bool btstack_set_powered(bool on);
bool btstack_get_powered(bool *out);
bool btstack_set_discoverable(bool on);
bool btstack_set_discoverable_timeout(unsigned seconds);
bool btstack_set_pairable(bool on);
bool btstack_set_alias(const char *alias);

// StartDiscovery/StopDiscovery. "Already discovering" is not an error: bluez
// answers org.bluez.Error.InProgress and that means the caller got what it
// asked for.
bool btstack_discovery(bool on);

// ---------------------------------------------------------------------------
// devices
// ---------------------------------------------------------------------------

// Every device bluez currently knows: paired ones and whatever the running
// discovery has turned up. One GetManagedObjects, no files, no parsing of
// anybody's output.
int btstack_devices(btstack_device_t *out, int max);

// Device1.Pair(). The agent registered by btstack_open() answers the
// confirmation, so this returns when the pairing has finished one way or the
// other and not when a prompt appeared.
bool btstack_pair(const char *address, int timeout_ms);

// Device1.Connect(): every profile the device offers. Returns when the ACL link
// is up, which is NOT the same as the audio being ready -- see
// btstack_audio_sink().
bool btstack_connect(const char *address, int timeout_ms);

bool btstack_disconnect(const char *address);
bool btstack_trust(const char *address, bool on);

// Adapter1.RemoveDevice: forgets the pairing and the link key.
bool btstack_remove(const char *address);

// ---------------------------------------------------------------------------
// the audio path
//
// bluealsa publishes one object per stream:
//
//     /org/bluealsa/hci0/dev_AA_BB_CC_DD_EE_FF/a2dpsrc/sink
//
// It exists exactly while there is an A2DP sink configured and something to
// write to, and it is announced with InterfacesAdded and InterfacesRemoved. So
// "can the player make a sound over Bluetooth" is not inferred from an ACL link
// or waited for on a side channel: it is this object being there.
// ---------------------------------------------------------------------------

// The address of the connected A2DP sink, or false when there is none. Read
// from the cache the signals keep, so it is current and costs nothing.
bool btstack_audio_sink(char *address_out, size_t size);

// The other direction, and the whole of Bluetooth receiver mode.
//
// With bluealsa started as an A2DP sink as well, a phone or a computer that
// connects here gets a PCM of its own:
//
//     /org/bluealsa/hci0/dev_AA_BB_CC_DD_EE_FF/a2dpsnk/source
//
// It is a capture device: bluealsa decodes what arrives over the air and the
// client reads frames out of it. Everything about it mirrors the sink above --
// same cache, same signals, same "it exists exactly while there is something
// there" -- so the question "is anything streaming to this device" is this
// object being present and not a guess about a link.
bool btstack_audio_source(char *address_out, size_t size);

// What bluealsa negotiated on one of the two streams, in parts rather than as
// one line: the receiver page prints the codec and the rate on their own.
// `receiving` picks the direction -- false for the headphones this device
// plays to, true for the phone it plays from.
typedef struct {
	char codec[BT_CODEC_NAME_MAX];
	unsigned rate; // Hz, 0 when bluealsa has not said
	unsigned channels;
	unsigned bits; // 0 for a format word this build does not name
} btstack_stream_t;

bool btstack_stream_info(const char *address, bool receiving, btstack_stream_t *out);

// A transport command sent the other way: org.bluez.MediaPlayer1 on the player
// the remote device registered, which is AVRCP going out instead of coming in.
//
// It is what makes the side keys mean something while this device is the sink.
// The keys cannot act on the local transport then -- there is nothing playing
// here -- and the thing that should answer them is the phone at the other end,
// exactly as a pair of headphones answers its own buttons by telling the
// player they were pressed.
//
// `member` is "Play", "Pause", "Next" or "Previous". The player object is
// found under the device rather than assumed: bluez numbers them per device
// and the number is not predictable.
bool btstack_media_command(const char *address, const char *member);

// What that player says it is doing: "playing", "paused", "stopped", or false
// when nothing has said. It is the only honest answer to "should this one key
// mean play or pause", and the answer has to come from the machine the music is
// on -- a guess made from whether frames are arriving is right until the sender
// pauses, at which point nothing arrives, nothing fails, and the guess stays
// stuck on "playing" for ever.
//
// Kept current by the PropertiesChanged bluez emits on MediaPlayer1, whichever
// end caused the change.
bool btstack_media_status(char *out, size_t size);

// What the remote player says it is playing, out of AVRCP's track metadata.
// bluez publishes it as the MediaPlayer1 Track property and announces every
// change, so this is a cache rather than a call.
typedef struct {
	char title[128];
	char artist[128];
	char album[128];
	unsigned duration_ms; // 0 when the sender did not say
} btstack_track_t;

bool btstack_media_track(btstack_track_t *out);

// Bumped whenever the cache above changes, so a page can redraw on the change
// rather than on a timer.
unsigned btstack_media_serial(void);

// Reads the status and the track outright, for the moment a link comes up and
// no signal has been sent yet. On the Bluetooth worker, like everything else
// here.
//
// What it reads replaces the cache; what it fails to read leaves the cache
// alone. The player object is looked up on every call and the reads can be
// refused while a link is renegotiating, and a failure there says nothing about
// what the device is playing.
void btstack_refresh_media_status(const char *address);

// Empties both caches. For the sender going away: nothing is streaming, so the
// last track is not what is playing.
void btstack_forget_media(void);

// Records a status without asking anyone. For the instant after a command is
// sent: the signal that confirms it is a round trip away, and a second press
// arriving in the meantime must not send the same command twice.
void btstack_note_media_status(const char *status);

// Re-reads the PCM list from bluealsa. Only needed after connecting to the bus,
// when the objects that already existed were never announced.
void btstack_refresh_audio(void);

// What the sink offers and what it settled on, over org.bluealsa.PCM1.
int btstack_codecs(const char *address, char out[][BT_CODEC_NAME_MAX], int max, char *selected, size_t selected_size);

// The same two, either way round. `receiving` picks the a2dpsnk/source PCM --
// what a phone is sending to this player -- instead of the a2dpsrc/sink one.
int btstack_codecs_dir(const char *address, bool receiving, char out[][BT_CODEC_NAME_MAX], int max, char *selected,
					   size_t selected_size);
bool btstack_select_codec_dir(const char *address, bool receiving, const char *codec);

// The PCM's volume word, both channels and their mute bits together. Carried
// whole across a codec change: the new PCM starts at bluealsa's default and
// nothing resends what the other end had already set.
bool btstack_pcm_volume_get(const char *address, bool receiving, int *out);
bool btstack_pcm_volume_set(const char *address, bool receiving, int volume);
bool btstack_select_codec(const char *address, const char *codec);

// One line describing the audio path bluealsa has actually built: the codec it
// negotiated, and the rate, channel count and sample width it is encoding from.
// For example "AAC, 44100 Hz, 2 ch, 16 bit".
//
// It is here because without it the player is blind to the half of the path it
// does not own. "It chops over Bluetooth" has completely different causes at
// SBC 44.1 and at LDAC 96, and from the outside the two look identical.
bool btstack_audio_info(const char *address, char *out, size_t size);

// The version string of the bluealsa daemon that answered on the bus, from the
// Version property of org.bluealsa.Manager1 (present since well before 4.1.1,
// so a failure here means no daemon, not an old one).
//
// It is here because the daemon is a file in the firmware, not part of this
// build, and the two that can be at that path behave differently. Without this
// line, "does it still chop" is asked of an unknown binary.
bool btstack_bluealsa_version(char *out, size_t size);

// The A2DP streams bluez holds for a device, as "fdN=state" separated by
// spaces: idle, pending or active. "active" is the only one in which what the
// encoder sends is being rendered, so it is what separates a silent pair of
// headphones from a broken write path -- and more than one entry means bluez
// is holding a transport nobody is using.
bool btstack_a2dp_streams(const char *address, char *out, size_t size);

#endif /* BTSTACK_H */
