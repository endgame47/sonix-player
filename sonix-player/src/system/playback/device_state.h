#ifndef DEVICE_STATE_H
#define DEVICE_STATE_H

#include "src/system/audio/audio.h"
#include "src/system/library/library.h"
#include "src/system/library/metadata.h"

// Full snapshot of everything the UI cares about: what is playing, how far into
// it playback has got, the loaded track's metadata, battery, and volume. Each
// field is sourced from the subsystem that owns and locks it -- this struct is
// a snapshot, not a store.
typedef struct {
	audio_status_t status;
	char current_file[512];
	double progress_current_secs;
	double progress_total_secs;
	int stream_sample_rate;
	int stream_channels;

	song_metadata_t metadata; // metadata of current_file; has_tags=false if none loaded

	// A live stream (internet radio) rather than a file. Everything about it
	// comes from radio.c: `metadata.title` is the station, `metadata.artist`
	// is whatever the stream says is on air, `current_file` is empty (there is
	// no file), and the two progress figures are zero because a live stream
	// has neither a position nor a length. `live_cover` is the station icon on
	// disk when one was downloaded.
	//
	// Anything that draws now-playing has to look at this: without it a radio
	// reads as a track that has lost its file.
	bool live;
	char live_cover[256];

	char battery_percent[8];
	bool battery_charging;
	long volume;
} device_state_t;

// Fills out with a fresh snapshot of the current device state.
void device_state_get(device_state_t *out);

// Loads and starts playing a new file: reads its metadata (cached for
// subsequent device_state_get() calls) and starts playback. Also (re)builds
// the folder playback queue from the file's directory, so playback can
// continue to the following tracks when this one finishes.
void device_state_play_file(const char *filepath);
// The same, but from `position` seconds in rather than from the start: an
// audiobook picked up where it was left.
void device_state_play_file_at(const char *filepath, double position);
void device_state_restore_file(const char *filepath, double position); // paused, at position

// Writes the queue down now, because something other than a track change moved
// it: "add to queue" is the one that matters.
//
// The mirror on disk is otherwise refreshed only when a track is loaded, which
// is minutes away and may never come -- a track queued and then a power-off is
// a track the next boot has never heard of. One write at the moment the user
// changes the queue costs nothing and is the only time the two can be out of
// step for long.
void device_state_queue_changed(void);

// The boot-time restore for a queue that was a library list: the handle stands
// in for the list of paths. Takes ownership of `ix`.
//
// `extra` is what "add to queue" had put into that list and `extra_slots` where
// each one sat, so they go back where they were. Sorted by slot, because each
// insert shifts the ones after it. A slot of -1 marks a queue written down
// before slots were, and that entry lands after the current track.
void device_state_restore_index(library_index_t *ix, int start_index, const char *const *extra,
								const int *extra_slots, int extra_count, const char *filepath, double position);

// The boot-time restore with the queue that was saved alongside the track: the
// whole list comes back, so next and prev pick up where they were instead of
// being stuck on the one remembered song. `filepath` is the track to come up on
// (paused at `position`); if it is not in the list, `start_index` decides.
void device_state_restore_list(const char *const *list, int count, int start_index, bool custom, const char *filepath,
							   double position);

// Starts a library list as the queue, handing the queue the same handle the
// list is drawn from: the queue is then four bytes a track rather than a second
// copy of every path. Takes ownership of `ix`. False when the list could not be
// started, and then nothing has changed.
bool device_state_play_index(library_index_t *ix, int start_index);

// Play from an explicit list of full paths: a playlist file, a streaming album,
// anything with no query behind it. The queue is the list itself, so next/prev
// follow it instead of the file's folder.
void device_state_play_list(const char *const *list, int count, int start_index);

// For a queue whose order is its content (a podcast's episodes): neither
// shuffle nor repeat touch it. See playlist_load_paths_ordered().
void device_state_play_list_ordered(const char *const *list, int count, int start_index);

// Before a file starts, someone can say "not yet".
//
// This exists for the streaming services. Since the queue holds the whole album
// rather than only the tracks already downloaded, playback can reach a file
// that is not there yet. A registered callback receives the path and:
//
//   * returns true if it can be played (the file exists, or it is not theirs);
//   * returns false after taking responsibility for preparing it and restarting
//     playback once it is ready.
//
// Called on the GUI thread and must not block.
//
// Several can be registered, which is why this adds rather than sets. With a
// single slot the second registration would silently displace the first, and a
// queue would stop waiting for its downloads and skip tracks that have not
// arrived.
//
// All of them are called, in registration order, and the track starts only if
// all say yes. The first to say no takes charge and the walk stops there: the
// others have nothing to say about a file that is not theirs, and asking anyway
// would mean two preparations in flight on the same path.
typedef bool (*device_state_prepare_cb)(const char *path);
void device_state_add_prepare_cb(device_state_prepare_cb cb);

// Jump to entry `index` of the current queue.
bool device_state_play_queue_index(int index);

// Call when the current track has finished on its own (see
// device_state_take_completion). Advances the folder queue according to the
// active playback mode and starts the next track; returns true if playback
// continued (optionally copying the new track's path into out_path), or false
// if playback should stop (end of folder in normal mode / empty queue).
bool device_state_advance_auto(char *out_path, size_t out_size);

// Whether consecutive album playback -- one record rolling into the next -- is
// switched on, and the setting behind it.
bool device_state_album_chaining(void);
void device_state_set_album_chaining(bool on);

// And the same for folders: when a folder queue runs out, playback carries on
// into the next folder on the card instead of stopping. The order is the walk
// playlist_next_folder() describes -- an artist's albums, then that artist's
// loose singles, then the next artist -- and it comes round to the beginning
// rather than ending at the last folder.
//
// Only ever a folder queue. Starting a library list or a playlist and walking
// off the end of it into the directory tree would be a surprise.
bool device_state_folder_chaining(void);
void device_state_set_folder_chaining(bool on);

// User-initiated skip to the next/previous track in the folder queue. Start
// the new track and return true (optionally copying its path into out_path);
// return false if the queue is empty.
bool device_state_next(char *out_path, size_t out_size);
bool device_state_prev(char *out_path, size_t out_size);

// Returns true exactly once after the current track finished playing on its
// own (not from a user stop), so the caller can auto-advance the queue.
bool device_state_take_completion(void);

// Toggles play/pause based on the current playback status and returns the
// intended new status immediately, so callers can update UI optimistically
// without waiting for the playback thread to catch up. When playback has
// stopped (e.g. the track ended), this restarts the currently-loaded track
// from the beginning rather than doing nothing.
audio_status_t device_state_toggle_play_pause(void);

void device_state_seek(double seconds);

// Running through the track with a held button, without asking the decoder for
// anything until it is let go.
//
// A real seek drops what the sound card has queued and prepares the stream
// again; asked for four times a second it never lets the stream reach the
// buffer level it starts at, and what the user gets is silence followed by
// playback that does not come back. So a hold moves a number: scrub_by adds
// seconds to it (clamped to the track and starting from where the music is),
// device_state_get reports it as the position so the bar and the clock follow,
// and commit performs the one seek that matters. Any seek from elsewhere -- the
// slider, the queue moving on -- cancels an open scrub, and so does a track
// change: a position picked on one track means nothing on the next.
void device_state_scrub_by(double delta);
void device_state_scrub_commit(void);
bool device_state_scrub_active(double *position_out);
void device_state_stop(void);

// Call just before stopping playback because the storage it is reading from is
// being taken away -- the card removed, or exported to a computer. It notes
// where the track was, so that the next press of play carries on from there
// instead of starting the track again: from the engine's side that stop looks
// exactly like a track that ended.
//
// The note belongs to one press. Any track load clears it, and so does a play
// whose file is not the one that was interrupted.
void device_state_note_storage_gone(void);

void device_state_set_volume(long volume);
void device_state_change_volume(long amount);

// Volume as the UI and the hardware keys deal with it: 0-100.
int device_state_get_volume_percent(void);
void device_state_change_volume_percent(int delta);

// Re-reads the battery percentage from sysfs; call before device_state_get()
// to refresh the value it reports.
void device_state_refresh_battery(void);

// ---------------------------------------------------------------------------
// "Remember track"
//
// note()    called from the player's poll. Decides on its own whether there is
//           anything worth writing; cheap when there is not.
// flush()   called on the way out -- the power menu and the automatic
//           shutdown. Neither of those passes through the interface, so
//           without it the position is up to ten seconds stale after a normal
//           power-off, and stale by a whole seek after one made while paused.
// restored() told what the boot restore asked for, so a poll that lands before
//           the decoder has seeked cannot write a zero over it.
// ---------------------------------------------------------------------------
void device_state_remember_note(void);
void device_state_remember_flush(void);
void device_state_remember_restored(const char *path, double position);

#endif // DEVICE_STATE_H
