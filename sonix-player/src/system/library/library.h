#ifndef LIBRARY_H
#define LIBRARY_H

#include <stdbool.h>
#include <stddef.h>

// The music index.
//
// SQLite, with MEDIA_TABLE holding one row per track and
// ALBUM/ARTIST/ALBUM_ARTIST/GENRE holding the distinct values (see the schema
// in library.c). It lives beside the thumbnail cache under .local, so
// everything the player generates is in one place.
//
// Ordering is the part worth reading about. Lists are not in byte order: they
// are in the order `listorder` gives, a collation this file registers and
// nothing else defines. It groups a string by what its first character *is*
// before comparing anything: punctuation first, then digits, then Latin, Greek,
// Cyrillic, Japanese kana, Chinese, Korean, and anything else last. Byte order
// would scatter those through each other.

// Opens (creating if needed) the database and registers the collation. Safe to
// call again; returns false if the file cannot be opened, in which case every
// call below is a no-op.
bool library_open(const char *db_path);
void library_close(void);

// Closes and reopens the index when its file is no longer the one that was
// opened: deleted (from the file manager, the Wi-Fi transfer page) or replaced
// by another under the same name. A deleted database stays readable through the
// open handle, so without this the library would go on showing what it held.
// Reopening a deleted one creates it empty. True when it reopened.
bool library_reopen_if_replaced(void);

// How many tracks the index currently holds.
int library_track_count(void);

// ---------------------------------------------------------------------------
// Scanning
//
// Walking a card and reading a tag out of every file takes minutes, so it runs
// on its own thread and the UI polls the counters below.
// ---------------------------------------------------------------------------

// The folders at the root of the card the scan reads; none for the whole card.
// A scan limited to some folders leaves out the files at the root of the card
// too. library_scan_folders() returns an array the caller frees with
// library_scan_folders_free(), NULL with *count 0 for the whole card.
char **library_scan_folders(int *count);
void library_scan_folders_free(char **names, int count);
void library_scan_folders_set(const char *const *names, int count);

// Starts a scan of `root`, wiping whatever was indexed before. Returns false
// if a scan is already running or the database is not open. A Detect changes
// run in progress is stopped first: the scan does everything it would.
bool library_scan_start(const char *root);

// "Detect changes": at startup and whenever the card comes back -- put in
// again, returned by a computer, or left by the Wi-Fi transfer -- the index is
// brought up to date without emptying it. The folders chosen for the scan are walked, and only a
// folder whose names differ from what the index noted of it is looked into:
// the tracks whose file is gone are taken out, and the files the index does not
// have are read and added.
//
// Remembered in [library] detect_changes. Off until the first scan completes,
// when the interface turns it on unless it was already set either way
// (library_detect_changes_chosen). Nothing happens on a library that was never
// scanned: building it is the scan's job.
bool library_detect_changes(void);
void library_set_detect_changes(bool on);
bool library_detect_changes_chosen(void);

// Whether Detect changes also reads again every indexed file whose
// modification time differs from the one the index holds: a file retagged,
// or replaced by another copy under the same name. One stat per indexed file,
// so the run takes longer. On by default, [library] detect_retagged.
bool library_detect_retagged(void);
void library_set_detect_retagged(bool on);

// ---------------------------------------------------------------------------
// How the index files tracks
//
// A tag naming several artists or genres -- "A & B", "A feat. B", "Rock; Pop"
// -- can be split at the separators chosen below, and the track is then filed
// under each name: in the Artists or Genres list, on each one's page, in the
// search. The tag itself is not touched, and is what a track shows.
//
// And an album can be joined: tracks with the same album name in the same
// folder are one record, whoever the artists on them, so a disc where only
// some tracks carry an album artist is not dealt out as several.
//
// All of it is worked out from what the index already holds, so a change is
// applied with library_reorganize() rather than a scan.
// ---------------------------------------------------------------------------

typedef enum {
	LIBRARY_SPLIT_SEMICOLON = 1 << 0, // ;
	LIBRARY_SPLIT_SLASH = 1 << 1,	  // /
	LIBRARY_SPLIT_AMPERSAND = 1 << 2, // &
	LIBRARY_SPLIT_COMMA = 1 << 3,	  // ,
	LIBRARY_SPLIT_FEAT = 1 << 4,	  // feat., ft., featuring, as a word
	LIBRARY_SPLIT_VS = 1 << 5,		  // vs., versus, as a word
	LIBRARY_SPLIT_X = 1 << 6,		  // x as a word, lower case, and the sign
	LIBRARY_SPLIT_PLUS = 1 << 7,	  // +
	LIBRARY_SPLIT_WITH = 1 << 8,	  // with, as a word
	LIBRARY_SPLIT_AND = 1 << 9,		  // and, as a word
} library_split_t;

// The full-width forms CJK tags are written with count as the separator they
// stand for: ／ as /, ＆ as &, ；as ;, ， and 、 as a comma, ＋ as +.

// [library] split_artists (off) and artist_separators (; / & feat.).
bool library_split_artists(void);
void library_set_split_artists(bool on);
unsigned library_artist_separators(void);
void library_set_artist_separators(unsigned separators);

// [library] split_album_artists (off): the album artist split as well, at the
// artist separators and with the same exceptions, so a record by "A & B" is
// listed under A and under B in Album artists. Only with split_artists on.
bool library_split_album_artists(void);
void library_set_split_album_artists(bool on);

// [library] split_genres (off) and genre_separators (; / ,).
bool library_split_genres(void);
void library_set_split_genres(bool on);
unsigned library_genre_separators(void);
void library_set_genre_separators(unsigned separators);

// The artists never split, wherever they appear in a tag ("AC/DC"), matched
// without regard to case. Kept in artist_exceptions.txt beside
// device_config.ini, one per line; "AC/DC" alone until the list is first
// saved. The array is the caller's, freed with library_artist_exceptions_free().
char **library_artist_exceptions(int *count);
void library_artist_exceptions_free(char **names, int count);
void library_set_artist_exceptions(const char *const *names, int count);

// [library] join_albums, off by default.
bool library_join_albums(void);
void library_set_join_albums(bool on);

// Files the index again under the settings above, on the scan thread, without
// reading the card: the artist and genre lists and the album keys are worked
// out anew from the tags already indexed. The listener hears REORGANIZING
// straight away and REORGANIZED when it is done. Busy with Detect changes, it
// runs as soon as that is over; behind a scan, the same without the
// REORGANIZING. False, with nothing to do, when there is no index or nothing
// in it.
//
// The index records which settings it was last filed under, so one filed
// under others -- the settings changed while the card was out or the index
// closed, a run stopped halfway -- is filed again by library_card_returned()
// and by library_organize_check().
bool library_reorganize(void);

// library_reorganize() when the index open now was filed under other settings.
// Returns whether it started.
bool library_organize_check(void);

// Called a few seconds after startup, and wherever the card is back and the
// index reopened. Starts the run above on the scan thread when the setting is
// on and nothing is scanning, and says whether it did. An index filed under
// other settings than the current ones (library_reorganize) is filed again as
// well: after the run, or on its own when the setting is off.
bool library_card_returned(const char *root);

// Told about a Detect changes run: LOOKING as soon as it is asked for, on the
// thread that asked; then, on the scan thread, ADDING with the number of new
// files (`added`) and of changed ones (`updated`) when there are some and they
// are about to be read, and as the thread's last word either FINISHED with the
// tracks that went in, came out and were read again (all zero when nothing
// changed) or STOPPED when it was cut short with nothing done. The index filed
// again on the same thread afterwards (library_card_returned) comes before
// that last word, as a REORGANIZED or STOPPED of its own.
//
// About library_reorganize(): REORGANIZING on the thread that asked,
// REORGANIZED on the scan thread when it is done, or STOPPED when it was not.
//
// And SCANNED on the scan thread when a library_scan_start() scan has gone
// through to the end.
typedef enum {
	LIBRARY_UPDATE_LOOKING,
	LIBRARY_UPDATE_ADDING,
	LIBRARY_UPDATE_FINISHED,
	LIBRARY_UPDATE_STOPPED,
	LIBRARY_UPDATE_REORGANIZING,
	LIBRARY_UPDATE_REORGANIZED,
	LIBRARY_UPDATE_SCANNED,
} library_update_event_t;
typedef void (*library_update_listener_t)(library_update_event_t event, int added, int removed, int updated);
void library_set_update_listener(library_update_listener_t listener);

// Whether there is an index to ask. False while the card is handed to a
// computer over USB -- the export closes the database -- while the card is out,
// and before the first open. Asked by the interface before it opens a page that
// would have nothing to show.
bool library_is_open(void);

// True from the moment a scan starts until its thread has finished.
bool library_scan_running(void);

// Tracks written to the index so far -- what the progress page counts up.
int library_scan_found(void);

// The folder being read right now, copied into the caller's buffer for the
// progress page. A copy and not a pointer: the scan thread rewrites that string
// at every folder, and reading it mid-rewrite means reading half a string,
// possibly without a terminator.
void library_scan_current_folder(char *out, size_t size);

// Asks a running scan to stop early; it winds up at the next file.
void library_scan_stop(void);

// Whether the scan names every file in the log as it reads it.
//
// Off by default, and it belongs off: a line per file is a write to the card
// per file, so the scan gets slower and the card gets worn for nothing. It
// exists for the one thing nothing else catches -- a player killed from
// outside runs no handler and leaves no report, and then the last line written
// is the only evidence of which file it had reached. Developer options has the
// switch, so a user can hand back a useful log without ADB.
//
// Remembered in [library] log_database, and it takes effect at once, even on a
// scan already running.
bool library_log_database(void);
void library_set_log_database(bool enabled);

// ---------------------------------------------------------------------------
// Reading the index
// ---------------------------------------------------------------------------

typedef enum {
	LIBRARY_LIST_TRACKS,
	LIBRARY_LIST_ALBUMS,
	LIBRARY_LIST_ARTISTS,
	LIBRARY_LIST_ALBUM_ARTISTS,
	LIBRARY_LIST_GENRES,
	LIBRARY_LIST_FAVOURITES, // in order of addition, oldest first
	LIBRARY_LIST_PLAYLIST,	 // one playlist's own table, in the order it was built
} library_list_t;

// Fills `out` with up to `max` names, in the collation's order (see above).
// Returns how many were written.
int library_list(library_list_t kind, char out[][256], int max);

// Optional filter for LIBRARY_LIST_TRACKS: only the tracks of one album,
// artist, album artist or genre.
typedef enum {
	LIBRARY_FILTER_NONE = 0,
	LIBRARY_FILTER_ALBUM,
	LIBRARY_FILTER_ARTIST,
	LIBRARY_FILTER_ALBUM_ARTIST,
	LIBRARY_FILTER_GENRE,
	// The tracks, albums or artists whose name contains the value, matched the
	// way library_search() matches. For the handles below only.
	LIBRARY_FILTER_SEARCH,
} library_filter_t;

// Streams the whole result set, sorted by the collation, one row per call:
// `name` is never NULL; `path` and `artist` are non-NULL for tracks only.
// Return false from the callback to stop early. Returns rows delivered.
//
// For a caller that wants every row and can hold them. The list pages do not:
// they use the handles further down, which keep four bytes a row instead of
// the row. What is left here is the short walks -- finding the album after this
// one, and the like.
typedef bool (*library_row_cb)(const char *name, const char *path, const char *artist, void *user);
int library_for_each(library_list_t kind, library_filter_t filter, const char *value, library_row_cb cb, void *user);

// The same, with a say in the ordering. Only meaningful for track lists.
typedef enum {
	LIBRARY_ORDER_DEFAULT = 0, // the collation's order: by title, or by disc
							   // position inside a single album
	LIBRARY_ORDER_ALBUM,	   // grouped by album, each record in disc order
	LIBRARY_ORDER_ADDED,	   // by when the file landed on the card, oldest
							   // first; all tracks, albums, artists and album
							   // artists only (a row is as new as its newest file)
	LIBRARY_ORDER_YEAR,		   // by release year, earliest first; all tracks and
							   // all albums only (an album's year is its tracks'
							   // latest). Rows with no year come last.
	LIBRARY_ORDER_YEAR_DESC,   // the same, latest first, and still with the rows
							   // that have no year last -- which is why it is an
							   // order of its own and not the one above read
							   // backwards
} library_order_t;
int library_for_each_ordered(library_list_t kind, library_filter_t filter, const char *value, library_order_t order,
							 library_row_cb cb, void *user);

// Looks a track's display title up by its file path (the scan stored it).
// False when the file is not in the index; `out` untouched then.
bool library_track_title(const char *path, char *out, size_t out_size);

// An album is its name AND whose it is: two records called "Greatest Hits" by
// two different artists are two albums. Whose is the album artist, the artist
// on a file that has no album artist, and the folder on a file that has
// neither.
//
// Everything that names one album -- a row of the Albums list, the value a
// track list is filtered by, the record a queue remembers -- carries both, as
// the name, this separator and a short key. A value without the separator is
// a name alone and matches every album of that name, which is what a value
// saved before albums were told apart still means.
#define LIBRARY_ALBUM_KEY_SEP '\x1f'

// The part of such a value a person reads: the name, without the key.
void library_album_title(const char *value, char *out, size_t size);

// Whether two album values name the same record. A name alone matches either
// form of it, so a record remembered before albums were told apart is still
// found.
bool library_album_same(const char *a, const char *b);

// The album a track belongs to, as its name. False when the file is not
// indexed or carries no album tag.
bool library_track_album(const char *path, char *out, size_t out_size);

// The same album as a value that names the record and not every album of its
// name: what "Show album" opens.
bool library_track_album_value(const char *path, char *out, size_t out_size);

// Finds indexed tracks by file name alone -- the whole batch in one pass over
// the table, because a name without its folders cannot use an index and would
// otherwise cost a scan each. `cb` fires once per match with the position the
// name held in `names`; a name matching two tracks fires twice for that slot.
// Returns how many matches were delivered.
//
// What an imported playlist is resolved with: the entries name files that a
// folder rename, or a card the list was written on, has moved, and the file
// name is what survives. The callback runs with the index locked, so it must
// copy what it needs and not call back in here.
typedef void (*library_basename_cb)(int slot, const char *path, void *user);
int library_match_basenames(const char *const *names, int count, library_basename_cb cb, void *user);

// Title and artist of an indexed track, in one lookup. For lists built from a
// list of paths -- a playlist -- where the rows have to be named without a
// query to name them.
bool library_track_names(const char *path, char *title_out, size_t title_size, char *artist_out, size_t artist_size);

// The artist an album is credited to: the album artist of its first track, or
// that track's artist when it has none -- the name the album list shows. False
// when the album has no tracks. `album` is an album value (see above).
bool library_album_artist(const char *album, char *out, size_t size);

// --- Audio quality ---
//
// Which of four badges a track earns, from what the scan wrote down: the
// container, the sample rate and the bit depth.
typedef enum {
	LIBRARY_QUALITY_NONE = 0, // not indexed, or the numbers were never read
	LIBRARY_QUALITY_LOSSY,	  // mp3, ogg, opus, AAC
	LIBRARY_QUALITY_CD,		  // 16 bit at 44.1 kHz or below
	LIBRARY_QUALITY_HIFI,	  // anything above that
	LIBRARY_QUALITY_DSD,
} library_quality_t;

// The badge for one indexed track, by path. `rate_out` and `bits_out` are
// optional and receive what was stored (zero when the scan could not read
// them). A lossless file whose numbers are unknown answers NONE rather than a
// guess: no badge is better than a wrong one.
library_quality_t library_track_quality(const char *path, int *rate_out, int *bits_out);

// --- Favourites ---
//
// A small table of starred tracks, in its own right rather than a view over
// MEDIA_TABLE: a track starred from the browser may not even be indexed, and
// a favourite should survive a library rescan. Name/artist are stored at
// star time so the list can render without a join.

// Stars/unstars a track. Returns the new state (true = it is now a favourite).
bool library_fav_toggle(const char *path, const char *name, const char *artist);

bool library_fav_contains(const char *path);

// Stars every track in `rows` that is not starred yet, in one transaction and
// in the order given; one already starred keeps its place. Returns how many of
// them are favourites afterwards.
typedef struct {
	const char *path;
	const char *name;
	const char *artist;
} library_fav_row_t;
int library_fav_add_many(const library_fav_row_t *rows, int count);

// Unstars every track in `paths`, in one transaction. Returns how many were
// favourites and are not any more.
int library_fav_remove_many(const char *const *paths, int count);

// Removes from the favourites (and from the saved playback state) everything
// under `prefix`. It is what clears rows pointing into the stream cache, whose
// files are emptied and would otherwise show as dead favourites. Run once at
// startup. Returns how many rows it removed.
int library_forget_under(const char *prefix);

// The ordering used everywhere (group by script, case/accent folded, leading
// English/Italian articles skipped: "The Beatles" files under B). Exported so
// the audiobook database can register the identical collation.
int library_collate_listorder(void *unused, int len_a, const void *a, int len_b, const void *b);

// Whether that leading article is skipped at all. On by default. Switched off,
// "The Beatles" files under T.
//
// It reaches the music lists through the stored sort key, which is written at
// scan time: changing it only shows up after the next scan. That is deliberate
// rather than a shortcut -- reordering the lists without rewriting the keys
// would leave the A-Z strip counting one order while the rows are in another.
void library_set_skip_articles(bool on);
bool library_skip_articles(void);

// The letter a name is filed under in an A-Z index: 'A' to 'Z' for a Latin
// name, '#' for the empty names, the punctuation and the digits that the
// collation files above A, and LIBRARY_INDEX_PAST_Z for everything it files
// below Z -- Greek, Cyrillic, kana, Hangul, CJK. It follows the collation above
// (the article skipped, the accent folded away), so a strip of letters beside a
// list points at the rows the list really holds.
#define LIBRARY_INDEX_PAST_Z '~'
char library_index_letter(const char *name);

// The ordering above written down as bytes: a string whose plain byte order is
// the order library_collate_listorder() gives. The index keeps one per row, which
// is what lets a list be read off an index instead of sorted on every open --
// no index can be built on a collation registered at runtime.
//
// Exported rather than kept private because the audiobook index registers the
// identical collation (see above) and wants the identical key if it is ever to
// stop sorting too.
#define LIBRARY_SORT_KEY_MAX 512
void library_sort_key(const char *name, char *out, size_t out_size);

// The letter of the strip a key stands for: library_index_letter() answered
// from the key instead of the name, which the key already carries.
char library_index_letter_of_key(const char *key);

// '#', A to Z, and one at the end for everything the collation files below Z.
// The strip beside a list has one label per bucket, and the handles below count
// their rows into the same ones, so the two cannot drift apart.
#define LIBRARY_INDEX_BUCKETS 28
#define LIBRARY_INDEX_PAST_Z_SLOT 27

// Which bucket a letter belongs to: 0 for '#', 1..26 for A..Z, the last one for
// everything that sorts past Z.
int library_index_slot(char letter);

// ---------------------------------------------------------------------------
// List handles
//
// Reading a whole list into RAM costs about a hundred bytes a row -- a name and
// a path, each its own allocation -- which on a large library is more memory
// than this device has. A handle holds four bytes a row instead, the SQLite row
// id, and the rows are read back a windowful at a time.
//
// The one ordered pass happens at open -- the same sort the streaming reader
// above does -- and only the row ids from it are kept.
// ---------------------------------------------------------------------------

typedef struct library_index library_index_t;

// Builds a handle over the same list library_for_each_ordered() would stream.
// `desc` reads the list backwards without asking the database for a second
// sort. NULL when the list cannot be built.
library_index_t *library_index_open(library_list_t kind, library_filter_t filter, const char *value,
									library_order_t order, bool desc);
void library_index_close(library_index_t *ix);


int library_index_count(const library_index_t *ix);

// True once the rows the handle names may no longer be the rows it was built
// over: a rescan, a card change, a favourite starred elsewhere. Row ids are
// reused, so a stale handle reads other tracks rather than none -- the caller
// has to rebuild rather than carry on.
bool library_index_stale(const library_index_t *ix);

// The number of rows in each A-Z bucket, in ascending order, for the strip
// beside the list. False when they cannot be trusted to describe this list --
// then the list simply has no strip.
bool library_index_buckets(const library_index_t *ix, int counts[LIBRARY_INDEX_BUCKETS]);

// Reads `count` rows from `offset`, in the handle's order, one callback each.
// Returns how many were delivered: fewer than asked means the end of the list,
// and zero on a stale handle. Unlike library_for_each(), an album row comes
// with a `path` (a track to take the cover from) and an `artist` (the album's).
int library_index_window(const library_index_t *ix, int offset, int count, library_row_cb cb, void *user);

// A second handle over the same rows, without asking the database for the
// sort again: the row ids are simply copied. What the playback queue is given
// when a list is started, so the queue does not have to be a second copy of the
// paths.
library_index_t *library_index_clone(const library_index_t *ix);

// What a handle was built from, in a form that can be written down and used to
// build the same handle again after a reboot.
typedef struct {
	bool valid;
	library_list_t kind;
	library_filter_t filter;
	library_order_t order;
	bool desc;
	char value[256];
} library_index_spec_t;
void library_index_describe(const library_index_t *ix, library_index_spec_t *out);

// Where a track sits in the handle, by its path: the row id comes from an
// indexed lookup and the position from a walk of the ids the handle holds.
// -1 when it is not in this list. What a queue uses to keep its place when the
// list underneath it has been rebuilt.
int library_index_find_path(const library_index_t *ix, const char *path);

// The same for a name list -- albums, artists, album artists, genres -- by the
// name a row of it carries. An album value with its key finds that record; a
// name alone finds the first album of that name. -1 when it is not in the list.
int library_index_find_name(const library_index_t *ix, const char *name);

// Bumped whenever the row ids of the tables behind `kind` may have moved.
// Favourites count separately from the index: starring a track rewrites one row
// of one table and must not invalidate a list of tracks.
unsigned library_revision(library_list_t kind);

// Search across the index, one callback per hit. `path` is set for tracks
// only. Returns the number of hits delivered.
//
// The query matches anywhere in the name, and matches the way the lists sort:
// case is ignored in every alphabet, not only in the Latin one, and an accented
// letter answers to the letter underneath ("bjork" finds "Björk"). LIKE's
// wildcards are not the user's to type -- "_" means an underscore.
typedef enum {
	LIBRARY_SEARCH_TRACK,
	LIBRARY_SEARCH_ALBUM,
	LIBRARY_SEARCH_ARTIST,
} library_search_type_t;
typedef void (*library_search_cb_t)(library_search_type_t type, const char *name, const char *path, void *user);
int library_search(const char *query, int per_category, library_search_cb_t cb, void *user);

// ---------------------------------------------------------------------------
// Playlists
//
// One table per playlist, named "M3U_" followed by the playlist's own name --
// so a playlist called Sera is the table "M3U_Sera". The name goes in as
// written, quoted, because SQLite takes anything between double quotes and a
// mangled name would no longer be the name the user typed.
//
// A table answers a name, a track count and which entries are there from the
// index, which is one file the card has open already: opening a playlist reads
// nothing off the card. The .m3u form is what Backup writes and what Import
// reads.
//
// Each row carries the path, the duration, and a title and artist written down
// when it went in. A row on screen shows the library's title and artist for
// the track when the library has it, and the written-down ones otherwise. A
// track the library has is shown; one it does not have is shown when `present`
// says its file was on the card -- set when the entry goes in, and cleared by
// a scan or a Detect changes run that walked its folder without finding it.
// ---------------------------------------------------------------------------

#define LIBRARY_PLAYLIST_PREFIX "M3U_"

typedef struct {
	char path[1024];
	char title[512];
	char artist[256];
	long seconds; // -1 when it could not be worked out, as in M3U
	bool present; // whether the file was on the card (see above)
} library_playlist_row_t;

// How big one of these is: a path, a title and an artist at full length, near
// enough two kilobytes. That is fine for one on the stack and ruinous for an
// array of them -- five thousand entries would be nine megabytes on a device
// with ten free -- which is why everything below reads and writes a page at a
// time and nothing hands out the whole list at once.

// The playlists there are, alphabetically. Fills a malloc'd array of malloc'd
// names; release with library_playlist_names_free().
int library_playlist_names(char ***names_out);
void library_playlist_names_free(char **names, int count);

bool library_playlist_exists(const char *name);

// An empty playlist. False if one by that name is already there.
bool library_playlist_create(const char *name);
bool library_playlist_drop(const char *name);

// Fails when `new_name` is taken. Renaming to the current name succeeds and
// does nothing.
bool library_playlist_rename(const char *name, const char *new_name);

// Appends a row, creating the playlist if it is not there.
bool library_playlist_append(const char *name, const library_playlist_row_t *row);

// Appending many, one at a time, under one transaction: an import writes
// hundreds of rows and a commit each would be hundreds of card writes, while an
// array of them all would be the megabytes described above.
//
// begin() creates the playlist if it is not there. Every row() that returns
// false has left the writer failed, and end() then rolls the whole thing back;
// end() also rolls back when `keep` is false. The writer is freed either way.
typedef struct library_playlist_writer library_playlist_writer_t;
library_playlist_writer_t *library_playlist_write_begin(const char *name);
bool library_playlist_write_row(library_playlist_writer_t *writer, const library_playlist_row_t *row);
bool library_playlist_write_end(library_playlist_writer_t *writer, bool keep);

// The same, for a caller that already holds the rows. Only for a handful of
// them: see the note on the size of one.
bool library_playlist_append_all(const char *name, const library_playlist_row_t *rows, int count);

// Rows [offset, offset+count) in order, every entry including the hidden ones,
// with the title and artist a row shows (see above), into a buffer the caller
// owns. Returns how many were written: fewer than asked means the end of the
// playlist. Read
// in pages, so that nothing holds more than a pageful and the index lock is
// released between them.
int library_playlist_page(const char *name, int offset, int count, library_playlist_row_t *out);

// Every row, in order, in one malloc'd array; free() it when done. Two
// kilobytes a row -- see above -- so this is for a caller that knows the
// playlist is small. Nothing in the player uses it; the readers all go through
// library_playlist_page().
int library_playlist_rows(const char *name, library_playlist_row_t **rows_out);

// The first row naming this path, gone.
bool library_playlist_remove_path(const char *name, const char *path);

// Whether any row of the playlist names this path.
bool library_playlist_contains(const char *name, const char *path);

// Removes the entries at `positions` -- counted as the page shows the list,
// hidden entries left out -- in one transaction. Returns how many went.
int library_playlist_remove_positions(const char *name, const int *positions, int count);

// One entry moved from one position to another, the rest closing up behind it.
// Positions are counted from the top of the list as the page shows it, not row
// ids: removals leave holes in `idx`, so the two are not the same number.
//
// The cost is the distance dragged, not the length of the playlist. Bumps the
// playlist generation, so a page holding a handle over this list notices.
bool library_playlist_move(const char *name, int from, int to);

// How many entries are shown (see above). No card is touched.
int library_playlist_count(const char *name);

// True once per card: whether the .m3u files that were the playlists have
// already been taken into the index. See playlists_init().
bool library_playlists_migrated(void);
void library_playlists_set_migrated(void);

// A remembered track count for a playlist held as an .m3u file, where counting
// means one card lookup per entry -- seconds of blocked interface on a list of
// several hundred deep paths.
//
// `mtime` and `size` are the playlist file's, as it was when counted: get()
// returns -1 unless they still match, so an edited playlist is counted again
// rather than answered from a stale row.
//
// Unused, since a playlist in the index answers its count from a column.
int library_playlist_count_get(const char *path, long mtime, long size);
void library_playlist_count_save(const char *path, long mtime, long size, int tracks);

// Whether the remembered number was arrived at by walking this playlist against
// the card as it is mounted now, rather than against an earlier mount of it.
// The caller draws the remembered number either way; this says whether the walk
// is worth doing again in the background.
bool library_playlist_count_is_verified(const char *path, long mtime, long size);

// Playback state for the "remember track" option: one track plus position.
void library_playback_state_save(const char *path, double position);
bool library_playback_state_load(char *path_out, size_t path_size, double *position_out);

// The playback queue, mirrored on disk so it survives a power cycle. Without it
// a reboot leaves the player holding the remembered track and nothing around
// it, so next and prev have nowhere to go. `paths` is in playback order (a
// shuffle deal already applied),
// `current_index` the position within it, `custom` whether it is a library
// list (never rebuilt from a folder) or a plain folder queue.
void library_queue_save(const char *const *paths, int count, int current_index, bool custom);

// Cheap partial update for the common case: the position moved, the list did
// not. Rewriting a few thousand rows on every track change is not free.
void library_queue_save_index(int current_index);

// Which record the queue is, kept beside it so that "play albums back to back"
// still knows after a reboot. Empty, or absent, when the queue is not a record.
void library_queue_save_album(const char *album);
bool library_queue_load_album(char *out, int size);

// The same job for a queue that is a library list: the query it was built from
// and where playback had got to, instead of every row of its answer. Replaces
// whichever of the two forms was stored before, so exactly one of them ever
// describes the saved queue.
// `position` is an index into the entries as stored, not into the order they
// are playing in: a shuffled queue is dealt again when it comes back, and the
// old deal's position would land on an unrelated track. `extra` is whatever was
// added to the queue after it was built -- not part of the query, and few
// enough to write out as rows.
// `extra_slots[i]` is where extra[i] sits in the playing order, which is what
// puts it back where it was rather than in front of playback. Pass NULL to
// store no slots.
void library_queue_save_query(const library_index_spec_t *spec, int position, const char *const *extra,
							  const int *extra_slots, int extra_count);
bool library_queue_load_query(library_index_spec_t *spec, int *position_out);

// The rows beside a saved query -- what "add to queue" put into a library list
// -- with the slot each one occupied, already sorted so that restoring them in
// the order given lands every one of them in the right place. A row saved
// before the slot existed comes back as -1 and sorts last. Both arrays are the
// caller's: free the paths with library_queue_free() and the slots with free().
int library_queue_load_extra(char ***paths_out, int **slots_out);

// Loads the saved queue. Returns the entry count (0 if there is none), fills
// *paths_out with a malloc'd array of malloc'd strings -- release it with
// library_queue_free() -- plus the position and the custom flag if asked.
int library_queue_load(char ***paths_out, int *index_out, bool *custom_out);
void library_queue_free(char **paths, int count);

#endif /* LIBRARY_H */
