#ifndef SONIXLINK_H
#define SONIXLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// SonixLink: the player, answered over the network by a phone.
//
// This protocol belongs to the project, not to the vendor. No bespoke framing,
// no length-prefixed records, no binary type signatures: it is HTTP with JSON
// bodies, and the library is not paged over the wire at all -- the phone
// downloads the index file once and queries it with its own SQLite. Everything
// that goes wrong is then visible in a browser.
//
//     GET  /api/info               what this player is, and how old its index
//     GET  /api/db                 the index file itself
//     GET  /api/covers             the player's own thumbnail cache, as a file
//     GET  /api/state              what is playing, volume, mode, battery
//     GET  /api/queue              the playback queue around the current track
//     GET  /api/history?limit=N    the latest N entries of what the queue has
//                                  played (playhistory.h), oldest first
//     GET  /api/favourites         the starred tracks, as they stand now
//     GET  /api/playlists          the playlists' names and sizes, as they stand
//     GET  /api/playlist?name=...  one playlist's tracks, in its order
//     GET  /api/cover?path=...     the artwork beside a track, when there is one
//     GET  /api/art?path=...       the same track's artwork whole, tag or file,
//                                  undecoded: the phone decodes it
//     GET  /api/browse?path=...    one folder of the card, as the file browser
//                                  lists it
//     GET  /api/ping               the app's heartbeat: nothing but "still here"
//     GET  /api/bye                the app has let go: the status bar forgets it
//                                  at once rather than after PEER_ALIVE_MS
//     POST /api/command?do=...     play, pause, next, prev, seek, volume, mode,
//                                  favourite, scan, and the queue page's own:
//                                  queue_remove (indices in the body),
//                                  queue_move&from=&to=, history_play&id=,
//                                  history_insert&id=&at=. The three that
//                                  change the order carry &rev=, the queue
//                                  revision they were worked out against, and
//                                  are dropped when the queue has moved on.
//
// Found two ways, so neither has to be perfect: a DNS-SD record for
// `_sonixlink._tcp`, which Android resolves by itself, and a plain UDP
// announcement for anything that would rather listen for one.
//
// Threading. One worker thread owns every socket and never blocks on the
// database for long. The player's state reaches it through sonixlink_publish()
// and the phone's commands leave through sonixlink_take_command(), pumped by
// the interface -- the same shape dlna.c uses.

#define SONIXLINK_TEXT_MAX 256
#define SONIXLINK_PATH_MAX 512

// The port the interface shows and the phone connects to.
#define SONIXLINK_PORT 7800

// What the player is doing. Filled in by the interface and handed over whole.
typedef struct {
	int play_state; // 0 stopped, 1 playing, 2 paused
	int play_mode;	// see sonixlink_mode_t
	int volume;		// 0..100
	unsigned progress_secs;
	unsigned duration_secs;

	char title[SONIXLINK_TEXT_MAX];
	char artist[SONIXLINK_TEXT_MAX];
	char album[SONIXLINK_TEXT_MAX];
	char album_artist[SONIXLINK_TEXT_MAX]; // empty when the tags have none
	char path[SONIXLINK_PATH_MAX];

	unsigned sample_rate;
	unsigned bitrate;
	uint8_t bits;
	bool lossless;

	int battery_percent;
	bool charging;

	bool favourite; // the track playing now is starred

	// The player's accent colour as "#rrggbb", so the app can wear the same one.
	char accent[8];

	// Where playback sits in the queue, and how long the queue is. Two numbers
	// rather than a queue request: the now-playing screen wants "4 di 12" once a
	// second, and asking for the whole window to read two integers would be
	// twenty kilobytes a second for a label.
	int queue_position; // -1 when there is no queue
	int queue_count;
	// Changes whenever the entries or their order do, so the app knows its copy
	// of the queue is stale without asking for it.
	unsigned queue_revision;

	bool scanning;
	unsigned scan_count;
	unsigned track_count;

	// How the player orders its lists (medialist_sort_prefs()): bit n of
	// sort_desc reverses list kind n, of sort_added puts it by date added. The
	// app orders its own copies the same way.
	unsigned sort_desc;
	unsigned sort_added;
	bool artist_by_album;
	bool favourites_reversed;

	// The "4/12" the player's own now-playing screen shows, as it shows it:
	// the place in the deal under shuffle, nothing (0) for a single track, a
	// book, a phone's DLNA stream, a radio outside a list.
	int display_position;
	int display_count;

	// Change whenever the favourites, or any playlist, do -- from the phone,
	// from the player's own screens, from a scan -- so the app refreshes its
	// copies only when they are stale.
	unsigned favourites_revision;
	unsigned playlists_revision;

	// The newest entry of the history, so the app reads it again only when
	// something has been played since. 0 while there is none.
	long long history_revision;
} sonixlink_state_t;

// The five orders the player understands, in the order the phone cycles them.
typedef enum {
	SONIXLINK_MODE_NORMAL = 0,	 // play through the queue and stop
	SONIXLINK_MODE_REPEAT_ALL,	 // loop the queue
	SONIXLINK_MODE_REPEAT_ONE,	 // loop this track
	SONIXLINK_MODE_SHUFFLE,		 // random, once through
	SONIXLINK_MODE_SHUFFLE_REPEAT, // the same random order, over and over
} sonixlink_mode_t;

typedef enum {
	SONIXLINK_CMD_NONE = 0,
	SONIXLINK_CMD_PLAY,
	SONIXLINK_CMD_PAUSE,
	SONIXLINK_CMD_TOGGLE,
	SONIXLINK_CMD_STOP,
	SONIXLINK_CMD_NEXT,
	SONIXLINK_CMD_PREV,
	SONIXLINK_CMD_SEEK,		// arg: seconds from the start
	SONIXLINK_CMD_VOLUME,	// arg: 0..100
	SONIXLINK_CMD_MODE,		// arg: sonixlink_mode_t
	SONIXLINK_CMD_PLAY_PATH, // the track in the command's own `path`
	SONIXLINK_CMD_SCAN,
	SONIXLINK_CMD_FAVOURITE, // arg: 1 star, 0 unstar, -1 toggle; `path` says which
	SONIXLINK_CMD_QUEUE_INDEX, // arg: entry of the current queue to jump to

	// The circle-play menu on a list: `list` and `value` say which, `arg` how
	// (SONIXLINK_PLAY_*), exactly as the player's own menu on that list.
	SONIXLINK_CMD_PLAY_ALL,

	// A selection of tracks, in the command's `paths`, as the player's own
	// selection mode does with one: after the track playing, into the
	// favourites or out of them, into the playlist named by `value` (made
	// when there is none) or out of it.
	SONIXLINK_CMD_QUEUE_NEXT,
	SONIXLINK_CMD_FAVOURITES_ADD,
	SONIXLINK_CMD_FAVOURITES_REMOVE,
	SONIXLINK_CMD_PLAYLIST_ADD,
	SONIXLINK_CMD_PLAYLIST_REMOVE,

	// The queue page, as on the player: the entries of `paths` (queue indices,
	// as text) taken out; the entry at `arg` moved to `arg2`; an entry of the
	// history (`id`) put in right after the track playing and played, or put
	// in at slot `arg`. Only what comes after the track playing can change.
	SONIXLINK_CMD_QUEUE_REMOVE,
	SONIXLINK_CMD_QUEUE_MOVE,
	SONIXLINK_CMD_HISTORY_PLAY,
	SONIXLINK_CMD_HISTORY_INSERT,
} sonixlink_command_kind_t;

// The three rows of the circle-play menu.
typedef enum {
	SONIXLINK_PLAY_SEQUENCE = 0,
	SONIXLINK_PLAY_SHUFFLE,
	SONIXLINK_PLAY_RANDOM_TO_QUEUE, // one track of the list, at random, after the one playing
} sonixlink_play_how_t;

// Which list a track was started from, so the queue becomes that list and not
// the whole library: starting a track inside a playlist must not queue every
// track in the index.
typedef enum {
	SONIXLINK_LIST_ALL = 0, // every track in the index
	SONIXLINK_LIST_ALBUM,
	SONIXLINK_LIST_ARTIST,
	SONIXLINK_LIST_ALBUM_ARTIST,
	SONIXLINK_LIST_GENRE,
	SONIXLINK_LIST_FAVOURITES,
	SONIXLINK_LIST_PLAYLIST, // `value` is the playlist's name, without M3U_
	SONIXLINK_LIST_QUEUE,	 // the queue as it stands: do not rebuild it
	SONIXLINK_LIST_FOLDER,	 // the track's folder, as the file browser queues it
	SONIXLINK_LIST_ALBUMS,	 // every record, each in its running order (circle-play on Albums)
} sonixlink_list_t;

typedef struct {
	sonixlink_command_kind_t kind;
	int arg;
	// The track a command is about, empty for the ones that are not about one.
	// Carried here rather than in a single shared slot so two path commands in
	// the same pump cannot end up pointing at the same file.
	char path[SONIXLINK_PATH_MAX];

	// For SONIXLINK_CMD_PLAY_PATH: the list the track was picked from, and what
	// narrows it (the album's name, the artist's, the playlist's).
	int list;
	char value[SONIXLINK_TEXT_MAX];

	// For the selection commands: `path_count` paths, one after another in
	// one block, each ended by its NUL. Owned by the command: whoever takes it
	// lets go of it with sonixlink_command_free().
	char *paths;
	int path_count;

	// For the queue page's commands: the second number of a move, the entry of
	// the history, and the queue revision the phone saw (`has_revision` false
	// when it sent none).
	int arg2;
	long long id;
	unsigned revision;
	bool has_revision;
} sonixlink_command_t;

void sonixlink_command_free(sonixlink_command_t *cmd);

// Starts the worker. It sits idle until the switch is on.
void sonixlink_init(void);

// The switch, remembered across reboots in [wireless] sonixlink.
bool sonixlink_get_enabled(void);
void sonixlink_set_enabled(bool on);

// Whether a phone has asked for anything recently, and what to show on the
// page: the address it should be pointed at, and who last spoke.
bool sonixlink_is_connected(void);
void sonixlink_peer(char *out, size_t out_size);
void sonixlink_address(char *out, size_t out_size);

// Where the index file lives, so it can be served. Called once at startup.
void sonixlink_set_db_path(const char *path);

// The card's root: the only tree /api/browse may list. Called once at startup.
void sonixlink_set_card_root(const char *path);

// Where the thumbnail cache lives -- the same file the player fills while its
// own lists are drawn (see src/gui/cover.c). It is sent to the phone as it
// stands: the pictures are already decoded and scaled, and the key is worked
// out from the index the phone already has.
void sonixlink_set_thumbs_path(const char *path);

// The playback queue, as much of it as the phone is shown.
//
// The whole queue is not published: it can be the entire library, and holding a
// second copy of seven thousand paths to answer a page nobody may open is not
// worth the memory. What goes across is one window of it: around the track
// playing now, or the stretch the phone last asked for (/api/queue?from=N),
// which is how the app pages through all of it.
#define SONIXLINK_QUEUE_WINDOW 200

// `paths` holds `count` entries starting at queue position `first`. `total` is
// how long the queue really is, `position` where playback sits in it and
// `revision` playlist_revision() when the window was read.
void sonixlink_publish_queue(int total, int position, int first, const char *const *paths, int count,
							 unsigned revision);

// The start of the stretch the phone asked for last, or -1 when it has asked
// for none since the queue last changed.
int sonixlink_queue_wanted(void);

// Hands the worker a fresh picture of the player. Call it on a timer.
void sonixlink_publish(const sonixlink_state_t *state);

// A connected Bluetooth link (an RFCOMM socket, see sonixlink_bt.h), served
// like a network client but kept open between requests. The descriptor is
// taken over either way: closed at once when the service is off.
void sonixlink_adopt_link(int fd);

// Takes one queued command, oldest first. False when there is nothing.
bool sonixlink_take_command(sonixlink_command_t *out);

// The track SONIXLINK_CMD_PLAY_PATH asks for.
void sonixlink_take_path(char *out, size_t out_size);

#endif /* SONIXLINK_H */
