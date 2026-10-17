#ifndef RADIO_H
#define RADIO_H

#include <stdbool.h>
#include <stddef.h>

// Internet radio, on top of the radio-browser.info directory.
//
// Three separate things live here, and they are separate on purpose:
//
//   * the directory  -- lists of languages, countries and genres, and the
//                       stations under each. Fetched over plain HTTP (see
//                       http.h) on a worker thread; the page polls.
//   * the store      -- an SQLite database on the card, <card>/.local/radio.db,
//                       holding the starred stations and the last few played.
//                       Same shape as the music library's own database, and
//                       next to it.
//   * the transport  -- one thread that pulls the stream into a buffer of a
//                       few seconds and one that decodes out of it and hands
//                       the frames to audio_external_*(), the same door
//                       AirPlay uses. audio.c's own decoder is not involved:
//                       a live stream has no length, no seeking and no end,
//                       and pretending otherwise would mean lying to every
//                       part of the player that asks a track a question.
//
// Everything the directory has is offered. Nothing is filtered out by codec,
// by scheme or by the directory's "broken" flag: a station that cannot be
// played here is listed, marked, and says why when it is tapped.
//
// https:// works: the device's own OpenSSL is opened at run time and the
// certificate chain and hostname are both verified (see tls.h). It needs a CA
// bundle to be present; without one an https station is listed, marked, and
// says what is missing when it is tapped.
//
// MP3 and AAC (ADTS) are the two codecs in the stream path. The codec the
// directory reports is often stale, so a station is tried anyway and the answer
// comes from the bytes.

#define RADIO_NAME_MAX 128
#define RADIO_URL_MAX 512

typedef struct {
	char uuid[40];
	char name[RADIO_NAME_MAX];
	char url[RADIO_URL_MAX]; // url_resolved when the directory had one
	char favicon[256];
	char country[64];
	char tags[128];
	char codec[16];
	int bitrate; // kbps, 0 when unknown
} radio_station_t;

// One entry of a "browse by..." list: a language, a country or a genre.
typedef struct {
	char name[96];	// what the API wants back as the search term
	char label[96]; // the same thing with a capital letter on the front
	int stationcount;
} radio_term_t;

typedef enum {
	RADIO_BROWSE_LANGUAGES,
	RADIO_BROWSE_COUNTRIES,
	RADIO_BROWSE_GENRES,
} radio_browse_t;

// ---------------------------------------------------------------------------
// The directory (worker thread; the page polls)
// ---------------------------------------------------------------------------

typedef enum {
	RADIO_JOB_IDLE = 0,
	RADIO_JOB_BUSY,
	RADIO_JOB_OK,
	RADIO_JOB_FAILED,
} radio_job_t;

// Starts the worker. Cheap; no network happens until something is asked for.
void radio_init(void);

// Asks for one of the three "browse by" lists, replacing whatever was in
// flight: the old answer is no longer wanted.
void radio_request_terms(radio_browse_t kind);

// How many stations one request brings back. Lists are paged rather than
// capped, since a country can hold thousands of stations: the page asks for the
// next lot on scrolling near the bottom.
#define RADIO_PAGE_SIZE 100

// Asks for the stations under one term. `kind` says which list `value` came
// from; `value` is the entry's `name`, matched exactly. `offset` is how many to
// skip: 0 for the first page, then RADIO_PAGE_SIZE at a time.
void radio_request_stations(radio_browse_t kind, const char *value, int offset);

// Stations whose name contains `text`. Fills the same result list as
// radio_request_stations().
void radio_request_search(const char *text, int offset);

// Where the current job is. Reading OK or FAILED leaves it there until the
// next request, so a page that redraws twice sees the same answer twice.
radio_job_t radio_job_state(void);

// Bumped every time a job finishes, so a page can redraw only when there is
// something new.
unsigned radio_job_serial(void);

// Copies out whatever the last finished job produced. The two are exclusive:
// a terms job fills the first, a stations job the second.
int radio_get_terms(radio_term_t *out, int max);
int radio_get_stations(radio_station_t *out, int max);

// How many stations the server sent for the last page, including any the parser
// threw away (no name, or no address). The page needs this rather than the kept
// count to tell whether the page was full, and so whether another exists, and
// where the next one starts.
int radio_last_raw_count(void);

// ---------------------------------------------------------------------------
// The store: favourites and recents, in <card>/.local/radio.db
// ---------------------------------------------------------------------------

// Opens (and creates) the database. Called once at startup with the card's
// root; a NULL or unusable path leaves the store closed, and every call below
// then does nothing rather than failing, so a player with no card still works.
bool radio_store_open(const char *sd_root);
void radio_store_close(void);

// Closes and reopens the store when radio.db was deleted or replaced under the
// open handle.
void radio_store_reopen_if_replaced(void);

int radio_fav_count(void);
bool radio_fav_get(int index, radio_station_t *out);
bool radio_fav_contains(const char *uuid);
void radio_fav_add(const radio_station_t *station);
void radio_fav_remove(const char *uuid);

// Why a station cannot be played here, or NULL when it can. The text is meant
// to be shown to the user as it is.
const char *radio_station_problem(const radio_station_t *station);

// The last stations played, newest first. Kept to RADIO_RECENT_MAX: playing
// one that is already in the list moves it to the top instead of adding it
// again.
#define RADIO_RECENT_MAX 10
int radio_recent_count(void);
bool radio_recent_get(int index, radio_station_t *out);

// ---------------------------------------------------------------------------
// The stations the user wrote down: radio.txt in the root of the card
//
// One per line, "name, url". The comma is the first one on the line, so a name
// with a comma in it works as long as the comma is not the last thing before
// the address; blank lines and lines starting with # are skipped. A line that
// is a name in square brackets, "[ROCK]", is a heading: the stations under it,
// down to the next one, are shown as a group of that name.
//
// A plain file and not the database, because that is what it is for: a list
// somebody types on a computer and drops on the card, with no directory in the
// middle and no network needed to reach it. Nothing here writes to it.
// ---------------------------------------------------------------------------
#define RADIO_CUSTOM_MAX 200

// Reads the file again. Called when the page is opened, so a card edited under
// the player shows what is on it now.
void radio_custom_reload(void);

int radio_custom_count(void);
bool radio_custom_get(int index, radio_station_t *out);

// The heading a station opens, the first one under a "[...]" line: true, with
// the heading's text in `out`. False for every other station. The headings do
// not count as stations: indices, previous and next run over the stations
// alone.
bool radio_custom_heading(int index, char *out, size_t out_size);

// Whether there is a radio.txt at all, for a page that has to tell an empty
// list from a missing file.
bool radio_custom_file_present(void);

// Where the loaded station sits in radio.txt, playing or stopped: its index,
// or -1 when it does not come from there.
int radio_custom_current_index(void);

// Whether previous and next move along radio.txt: the loaded station comes
// from there, and there is somewhere else in it to go.
bool radio_custom_can_step(void);

// Plays the station `step` places on from the loaded one in radio.txt (-1 the
// one before), round from the end to the start. False, doing nothing, when the
// loaded station is not from radio.txt.
bool radio_custom_step(int step);

// ---------------------------------------------------------------------------
// Previous and next on a station
//
// A station steps along the list it was started from: radio.txt for its own
// stations, and for the rest whatever list the page hands over with
// radio_set_list() when one of its rows is tapped. A station started from
// nowhere in particular -- restored at boot, say -- has nowhere to step to.
// ---------------------------------------------------------------------------

// Copies `list` as the one to step along; NULL or 0 forgets it.
void radio_set_list(const radio_station_t *list, int count);

// Where the loaded station is in its list, and how long the list is. False
// when there is no station, it is in no list, or the list holds it alone.
// Either output may be NULL.
bool radio_list_position(int *index_out, int *count_out);

bool radio_can_step(void);

// Plays the station `step` places on (-1 the one before), round from the end
// to the start, passing over any this device cannot play. False, doing
// nothing, when there is nowhere to step to.
bool radio_step(int step);

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

typedef struct {
	bool active;	 // a station is loaded (connecting, playing or retrying)
	bool connecting; // ...and no audio has come out of it yet
	bool playing;
	char station[RADIO_NAME_MAX];
	char title[256];	 // what the stream says is on air, or empty
	char cover_path[256]; // downloaded favicon on disk, or empty
	char error[128];	  // why it is not playing, for the user to read
	int bitrate;
	int sample_rate;
	int channels;
	// What is actually coming down the wire, filled in when the decoder is
	// chosen: "MP3", "AAC" or "FLAC". The directory's own codec field is a
	// claim and is often wrong -- an AAC station listed as MP3 is the common
	// case -- so the player shows this one and not that.
	char codec[8];
	// The bit depth of a lossless station, 0 for a lossy one.
	int bits;
} radio_now_t;

// Starts a station. Stops local playback first: one PCM takes one writer.
// Returns immediately; watch radio_get_now() for how it goes.
bool radio_play(const radio_station_t *station);

// Stops the stream and lets go of the DAC, but keeps the station loaded: the
// player goes on showing which station it is on, with a play button. A live
// stream has no position to resume from, so radio_resume() makes a fresh
// connection.
void radio_stop(void);
bool radio_resume(void);

// Stops and unloads: no station at all any more. What starting a local track
// does, because then the player belongs to the track.
void radio_clear(void);

// True while a station is loaded: playing, connecting, or stopped. Tells the
// rest of the player that what it is showing is not a track.
bool radio_is_active(void);

// True only while the stream is actually running (or connecting).
bool radio_is_playing(void);

// True while a station is loaded and no audio has come out of it yet: the
// first connection, or a reconnection after a drop.
bool radio_is_connecting(void);

void radio_get_now(radio_now_t *out);

// Bumped when the station, the on-air title or the artwork changes.
unsigned radio_now_serial(void);

// The station currently loaded, for the star button on the player.
bool radio_current_station(radio_station_t *out);

#endif /* RADIO_H */
