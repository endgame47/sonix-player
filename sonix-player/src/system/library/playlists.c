#include "playlists.h"

#include "src/system/decode/decode.h"
#include "src/system/library/cue.h"
#include "src/system/library/library.h"
#include "src/system/library/metadata.h"
#include "src/system/playback/playlist.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// See playlists.h for where a playlist lives and why. The tables are library.c's
// -- it owns the database handle and the lock -- and what is left here is the
// M3U side of it: reading one, writing one, and deciding what a line of text
// points at.

static char playlist_data_path[512];  // path used by the original firmware. used here for importing playlists into the new location
static char dir_path[512];
static char card_root[512];

// Cleared here rather than kept for the life of the process: this runs again
// when a card is mounted, and the next card has its own folder and its own flag.
static bool migration_tried;

void playlists_init(const char *sd_root) {
	migration_tried = false;
	if (!sd_root || !sd_root[0]) {
		dir_path[0] = '\0';
		card_root[0] = '\0';
		return;
	}
	snprintf(card_root, sizeof(card_root), "%s", sd_root);
	snprintf(dir_path, sizeof(dir_path), "%s/Playlist", sd_root);
	snprintf(playlist_data_path, sizeof(dir_path), "%s/playlist_data", sd_root);
}

const char *playlists_dir(void) { return dir_path; }

// The folder is made on demand, not at startup: a card with no playlists on
// it should not grow an empty folder just because the player booted.
static bool ensure_dir(void) {
	if (!dir_path[0]) {
		return false;
	}
	struct stat st;
	if (stat(dir_path, &st) == 0) {
		return S_ISDIR(st.st_mode);
	}
	if (mkdir(dir_path, 0777) == 0) {
		return true;
	}
	fprintf(stderr, "playlists: cannot create '%s': %s\n", dir_path, strerror(errno));
	return false;
}

// A playlist name becomes a file name, so the characters a FAT card cannot
// carry are refused rather than silently mangled.
static bool name_is_usable(const char *name) {
	if (!name || !name[0] || name[0] == '.') {
		return false;
	}
	if (strlen(name) > 200) {
		return false;
	}
	for (const char *c = name; *c; c++) {
		if (strchr("/\\:*?\"<>|", *c) != NULL) {
			return false;
		}
	}
	return true;
}

// Copies what fits and terminates. Used where truncation is the intent -- a row
// label, a path built from a card root the caller already bounded -- so that the
// compiler is not left guessing at a snprintf it cannot prove safe.
static void copy_capped(char *out, size_t out_size, const char *src) {
	if (!out || out_size == 0) {
		return;
	}
	size_t len = src ? strlen(src) : 0;
	if (len >= out_size) {
		len = out_size - 1;
	}
	if (len) {
		memcpy(out, src, len);
	}
	out[len] = '\0';
}

static bool file_for(const char *name, char *out, size_t out_size) {
	if (!dir_path[0] || !name_is_usable(name)) {
		return false;
	}
	// A truncated file name would point at the wrong playlist, or at none:
	// better to refuse than to act on half a path.
	return snprintf(out, out_size, "%s/%s.m3u", dir_path, name) < (int)out_size;
}

// ---------------------------------------------------------------------------
// the marker
// ---------------------------------------------------------------------------

static void strip_eol(char *line);
static bool read_line(FILE *f, char *line, size_t size);
static char *line_body(char *line);
static void resolve_entry(const char *line, const char *base, char *out, size_t out_size);

// What playlists_create() and a fresh playlists_add_track() put at the top.
#define MARKER_HEADER "#EXTM3U\n#" PLAYLISTS_MARKER "\n"

// True for the marker line, written as a comment or bare.
static bool is_marker_line(const char *line) {
	while (*line == '#' || *line == ' ' || *line == '\t') {
		line++;
	}
	size_t len = strlen(PLAYLISTS_MARKER);
	if (strncasecmp(line, PLAYLISTS_MARKER, len) != 0) {
		return false;
	}
	for (const char *rest = line + len; *rest; rest++) {
		if (*rest != ' ' && *rest != '\t' && *rest != '\r') {
			return false;
		}
	}
	return true;
}

// Whether a playlist file carries the marker.
//
// Only the header is read: the marker belongs above the entries, and a file
// whose first entry has gone by without one does not have it. The line cap is
// there for the file that is not a playlist at all -- a stray binary in the
// folder would otherwise be read to its end looking for a line that is not in
// it.
#define MARKER_SEARCH_LINES 16

static bool file_is_marked(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}

	bool marked = false;
	char line[1024];
	for (int i = 0; i < MARKER_SEARCH_LINES && read_line(f, line, sizeof(line)); i++) {
		char *text = line_body(line);
		if (!text[0]) {
			continue;
		}
		if (is_marker_line(text)) {
			marked = true;
			break;
		}
		if (text[0] != '#') {
			break; // the first entry: whatever came above it was not the marker
		}
	}

	fclose(f);
	return marked;
}

// ---------------------------------------------------------------------------
// listing
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The one-time move off the card
//
// A card may still carry playlists as .m3u files in its Playlist folder, so the
// first time that folder is looked at on a given card, every marked file in it
// becomes a table. The flag saying it has happened lives in that card's own
// index, next to the tables it made, so a card moved between devices is not
// done twice and a second card is not skipped.
//
// The files are left where they are: the folder is the backup folder, and a
// conversion must not delete the user's only other copy.
// ---------------------------------------------------------------------------

static int rows_from_file(const char *name, const char *file_path);

static void migrate_folder_once(void) {
	if (migration_tried || !dir_path[0] || library_playlists_migrated()) {
		return;
	}
	migration_tried = true;

	DIR *dir = opendir(dir_path);
	if (!dir) {
		library_playlists_set_migrated(); // no folder, nothing to move, never again
		return;
	}

	int moved = 0, tracks = 0;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		size_t len = strlen(de->d_name);
		if (len < 5 || strcasecmp(de->d_name + len - 4, ".m3u") != 0) {
			continue;
		}
		char full[600];
		if (snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name) >= (int)sizeof(full)) {
			continue;
		}
		// Only the ones this player wrote. An unmarked file is somebody else's
		// and stays where Import can find it, as it always did.
		if (!file_is_marked(full)) {
			continue;
		}

		char name[NAME_MAX + 1];
		size_t stem = len - 4 < sizeof(name) - 1 ? len - 4 : sizeof(name) - 1;
		memcpy(name, de->d_name, stem);
		name[stem] = '\0';
		if (!name_is_usable(name) || library_playlist_exists(name)) {
			continue;
		}

		int count = rows_from_file(name, full);
		if (count > 0) {
			moved++;
			tracks += count;
		}
	}
	closedir(dir);

	library_playlists_set_migrated();
	if (moved > 0) {
		fprintf(stderr, "playlists: %d playlist%s (%d tracks) moved from the card into the index; the files stay as "
						"backups\n",
				moved, moved == 1 ? "" : "s", tracks);
	}
}

int playlists_for_each(playlists_name_cb_t cb, void *user) {
	if (!cb) {
		return 0;
	}
	migrate_folder_once();

	char **names = NULL;
	int count = library_playlist_names(&names);

	int delivered = 0;
	for (int i = 0; i < count; i++) {
		if (!cb(names[i], user)) {
			break; // the caller has had enough
		}
		delivered++;
	}
	library_playlist_names_free(names, count);
	return delivered;
}

bool playlists_exists(const char *name) { return name_is_usable(name) && library_playlist_exists(name); }

// ---------------------------------------------------------------------------
// writing
// ---------------------------------------------------------------------------

bool playlists_create(const char *name) {
	if (!name_is_usable(name)) {
		return false;
	}
	return library_playlist_create(name);
}

// The seconds an #EXTINF line wants. -1 (the M3U "unknown") whenever the
// length cannot be had cheaply -- a WAV, or a file whose decoder will not
// open. Every player, the stock one included, accepts -1.
static long track_seconds(const char *track_path) {
	decode_format_t format = decode_detect_format(track_path);
	if (format == DECODE_FORMAT_UNKNOWN) {
		return -1;
	}

	decoder_t *dec = decoder_open_info(track_path, format);
	if (!dec) {
		return -1;
	}

	uint64_t frames = decoder_total_pcm_frames(dec);
	int rate = decoder_sample_rate(dec);
	decoder_close(dec);

	if (frames == 0 || rate <= 0) {
		return -1;
	}
	return (long)((frames + (uint64_t)rate / 2) / (uint64_t)rate);
}

void playlists_track_names(const char *track_path, char *title, size_t title_size, char *artist,
						   size_t artist_size) {
	title[0] = '\0';
	artist[0] = '\0';
	if (!track_path || !track_path[0]) {
		return;
	}
	// The index is asked first -- the scan already has both -- and the file's
	// own tags only when it has never seen this track.
	if (!library_track_names(track_path, title, title_size, artist, artist_size) || !title[0]) {
		song_metadata_t meta;
		metadata_read(track_path, &meta);
		if (meta.title[0]) {
			copy_capped(title, title_size, meta.title);
		}
		if (meta.artist[0]) {
			copy_capped(artist, artist_size, meta.artist);
		}
	}
	if (!title[0]) {
		const char *slash = strrchr(track_path, '/');
		copy_capped(title, title_size, slash ? slash + 1 : track_path);
	}
}

bool playlists_add_track(const char *name, const char *track_path) {
	if (!name_is_usable(name) || !track_path || !track_path[0]) {
		return false;
	}

	// Title and artist are written down with the entry, so that drawing the
	// playlist later is a read of one table and nothing else.
	library_playlist_row_t row = {{0}, {0}, {0}, -1, true};
	copy_capped(row.path, sizeof(row.path), track_path);
	playlists_track_names(track_path, row.title, sizeof(row.title), row.artist, sizeof(row.artist));
	row.seconds = track_seconds(track_path);

	return library_playlist_append(name, &row);
}

bool playlists_remove_track(const char *name, const char *track_path) {
	if (!name_is_usable(name) || !track_path || !track_path[0]) {
		return false;
	}
	return library_playlist_remove_path(name, track_path);
}

bool playlists_has_track(const char *name, const char *track_path) {
	return name_is_usable(name) && track_path && track_path[0] && library_playlist_contains(name, track_path);
}

int playlists_remove_positions(const char *name, const int *positions, int count) {
	if (!name_is_usable(name)) {
		return 0;
	}
	return library_playlist_remove_positions(name, positions, count);
}

bool playlists_delete(const char *name) { return name_is_usable(name) && library_playlist_drop(name); }

bool playlists_rename(const char *name, const char *new_name) {
	if (!name_is_usable(name) || !name_is_usable(new_name)) {
		return false;
	}
	// No special case for case alone: a table name is compared exactly, so
	// "prova" and "Prova" are two playlists -- unlike the FAT file names the
	// backups take, where they would be one file.
	return library_playlist_rename(name, new_name);
}

// ---------------------------------------------------------------------------
// reading
// ---------------------------------------------------------------------------

static void strip_eol(char *line) {
	size_t len = strlen(line);
	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
		line[--len] = '\0';
	}
}

// One line of a playlist file, without its end of line. A line longer than the
// buffer is cut, and the rest of it read past, so its tail does not come back
// as a line of its own -- a long comment would otherwise turn into an entry.
static bool read_line(FILE *f, char *line, size_t size) {
	if (!fgets(line, (int)size, f)) {
		return false;
	}
	size_t len = strlen(line);
	if (len > 0 && line[len - 1] != '\n') {
		int c;
		while ((c = fgetc(f)) != EOF && c != '\n') {
		}
	}
	strip_eol(line);
	return true;
}

// Where a line's content starts: past a UTF-8 byte order mark, which some
// editors put on the first line, and past the blanks in front. A comment is a
// line whose content starts with '#', wherever the '#' sits.
static char *line_body(char *line) {
	if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) {
		line += 3;
	}
	while (*line == ' ' || *line == '\t') {
		line++;
	}
	return line;
}

// Takes the "." and ".." segments and doubled separators out of an absolute
// path, in place and without touching the disk: the library stores paths in
// this form, and "Playlist/../Music/a.flac" has to match its "Music/a.flac".
// ".." at the root stays at the root.
static void collapse_dots(char *path) {
	if (path[0] != '/') {
		return;
	}
	size_t len = 1; // the result so far, always starting with '/'
	const char *seg = path + 1;
	while (*seg) {
		const char *end = strchr(seg, '/');
		size_t n = end ? (size_t)(end - seg) : strlen(seg);
		bool dot = n == 1 && seg[0] == '.';
		if (n == 2 && seg[0] == '.' && seg[1] == '.') {
			while (len > 1 && path[len - 1] != '/') {
				len--;
			}
			if (len > 1) {
				len--; // the separator in front of the segment dropped
			}
		} else if (n > 0 && !dot) {
			if (len > 1) {
				path[len++] = '/';
			}
			memmove(path + len, seg, n);
			len += n;
		}
		if (!end) {
			break;
		}
		seg = end + 1;
	}
	path[len] = '\0';
}

// Turns one line of a playlist into a path this player can open.
//
// The stock HiBy player writes its own playlists the Windows way, with a drive
// letter for the card and backslashes:
//
//     a:\Music\Coldplay\Parachutes\05 - Yellow.flac
//
// There is no drive `a:` here, so the letter is dropped and the card's mount
// point put in its place; backslashes become separators. A plain relative path
// is resolved against the folder the playlist itself is in, which is what every
// other M3U reader does: `base`, or the Playlist folder when it is NULL. An
// absolute path is already usable and passes through.
static void resolve_entry(const char *line, const char *base, char *out, size_t out_size) {
	// Skip a UTF-8 byte order mark: some editors put one on the first line and
	// it would otherwise become part of the first path.
	if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) {
		line += 3;
	}
	while (*line == ' ' || *line == '\t') {
		line++;
	}

	const char *rest = line;
	bool drive_letter = ((line[0] >= 'a' && line[0] <= 'z') || (line[0] >= 'A' && line[0] <= 'Z')) && line[1] == ':' &&
						(line[2] == '\\' || line[2] == '/');

	if (drive_letter) {
		rest = line + 2; // keep the separator: it starts the path under the card
		copy_capped(out, out_size, card_root[0] ? card_root : "");
		copy_capped(out + strlen(out), out_size - strlen(out), rest);
	} else if (line[0] == '/') {
		copy_capped(out, out_size, line);
	} else if (line[0] == '\\') {
		copy_capped(out, out_size, card_root[0] ? card_root : "");
		copy_capped(out + strlen(out), out_size - strlen(out), line);
	} else {
		if (!base) {
			base = dir_path[0] ? dir_path : ".";
		}
		copy_capped(out, out_size, base);
		copy_capped(out + strlen(out), out_size - strlen(out), "/");
		copy_capped(out + strlen(out), out_size - strlen(out), line);
	}

	for (char *c = out; *c; c++) {
		if (*c == '\\') {
			*c = '/';
		}
	}
	collapse_dots(out);
}

// Whether the file an entry points at is actually there. Asked once, when the
// entry goes in (import, and the move off the card's .m3u files); from then on
// the library says (see library.h).
//
// A CUE track is named by its sheet plus a track number, so it is the sheet
// that has to exist.
static bool entry_is_present(const char *path) {
	char sheet[512];
	if (cue_split_path(path, sheet, sizeof(sheet)) > 0) {
		return access(sheet, F_OK) == 0;
	}
	// The extension too, and by the queue's own list: a playlist made on a
	// computer can name a .wma or a .m4v, which is a row that does nothing for
	// a different reason but looks exactly the same from the outside.
	return playlist_is_playable_file(path) && access(path, F_OK) == 0;
}

// How long a single playlist may spend reading tags off the card for entries the
// library does not know about. See name_entry().
#define PLAYLIST_TAG_BUDGET_MS 250

// A monotonic millisecond reading. Only ever used as a difference, to bound the
// work above.
static uint32_t elapsed_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)(now.tv_sec * 1000u) + (uint32_t)(now.tv_nsec / 1000000);
}

// What a row should be called, best first: the track's own tags, then the
// #EXTINF the playlist carries, then the file name.
//
// The tags come first even when there is an #EXTINF, so a list looks like every
// other list in the player -- title on one line, artist under it -- rather than
// like the single "Artist - Title" string M3U was built to carry. The stock
// player's own playlists have no #EXTINF at all, only paths, so without this
// they are a column of file names.
//
// The library is asked before the file: the scan already put both fields in the
// row, so a 400-entry playlist costs 400 indexed lookups instead of 400 files
// opened off the card. Tracks the scan has never seen fall back to their tags.
static void name_entry(const char *path, const char *extinf, char *title_out, size_t title_size, char *artist_out,
					   size_t artist_size, uint32_t *tags_started_ms, bool *tags_budget_spent) {
	title_out[0] = '\0';
	artist_out[0] = '\0';

	if (!library_track_names(path, title_out, title_size, artist_out, artist_size) || !title_out[0]) {
		// The library did not know this one, so its tags have to be read off the
		// card. That is a file opened per entry, and a playlist whose tracks were
		// never scanned would spend seconds of it before showing anything -- so
		// the whole list gets a fixed slice of time for this, after which the
		// remaining unknown entries take the name below. With a scanned library
		// the lookup above answers and this never runs at all.
		if (!*tags_budget_spent) {
			// The clock starts here, at the first tag this list actually needs.
			if (*tags_started_ms == 0) {
				*tags_started_ms = elapsed_ms();
			}
			song_metadata_t meta;
			metadata_read(path, &meta);
			if (meta.title[0]) {
				copy_capped(title_out, title_size, meta.title);
			}
			if (meta.artist[0]) {
				copy_capped(artist_out, artist_size, meta.artist);
			}
			if ((elapsed_ms() - *tags_started_ms) >= PLAYLIST_TAG_BUDGET_MS) {
				*tags_budget_spent = true;
				fprintf(stderr, "playlists: too many entries the library does not know; the rest keep their file "
								"names (scan the library to name them)\n");
			}
		}
	}

	if (!title_out[0] && extinf && extinf[0]) {
		copy_capped(title_out, title_size, extinf);
	}
	if (!title_out[0]) {
		const char *slash = strrchr(path, '/');
		copy_capped(title_out, title_size, slash ? slash + 1 : path);
	}
}

// Reads a .m3u straight into a playlist of its own: the path each line means,
// the title and artist to show, whether the file is there. One row at a time,
// so a long list costs a row rather than all of them. Returns how many went in,
// 0 if nothing did. Used by the one-time move off the card; Import has its own
// reader, because it also has to look up entries the card no longer has at the
// path they name.
static int rows_from_file(const char *name, const char *file_path) {
	FILE *f = fopen(file_path, "r");
	if (!f) {
		return 0;
	}
	library_playlist_writer_t *writer = library_playlist_write_begin(name);
	if (!writer) {
		fclose(f);
		return 0;
	}

	int count = 0;
	char line[1024];
	char pending_title[512] = {0};
	long pending_seconds = -1;
	uint32_t tags_started_ms = 0;
	bool tags_budget_spent = false;
	bool ok = true;

	while (ok && read_line(f, line, sizeof(line))) {
		char *text = line_body(line);
		if (!text[0]) {
			continue;
		}
		if (text[0] == '#') {
			if (strncmp(text, "#EXTINF:", 8) == 0) {
				pending_seconds = strtol(text + 8, NULL, 10);
				const char *comma = strchr(text + 8, ',');
				snprintf(pending_title, sizeof(pending_title), "%s", comma ? comma + 1 : "");
			}
			continue;
		}
		if (is_marker_line(text)) {
			continue;
		}

		library_playlist_row_t row;
		memset(&row, 0, sizeof(row));
		resolve_entry(text, NULL, row.path, sizeof(row.path));
		row.seconds = pending_seconds;
		row.present = entry_is_present(row.path);
		name_entry(row.path, pending_title, row.title, sizeof(row.title), row.artist, sizeof(row.artist),
				   &tags_started_ms, &tags_budget_spent);
		pending_title[0] = '\0';
		pending_seconds = -1;

		ok = library_playlist_write_row(writer, &row);
		if (ok) {
			count++;
		}
	}
	fclose(f);

	if (!library_playlist_write_end(writer, ok && count > 0)) {
		return 0;
	}
	return count;
}

// A pageful at a time, everywhere. See library.h: a row is near enough two
// kilobytes, so an array of a big playlist's worth of them is megabytes this
// device does not have.
#define PLAYLIST_PAGE 32

// ---------------------------------------------------------------------------
// Opening a playlist, whole
//
// One read of one table, and no card at all: the title and the artist come
// from the library or were written down when the entry was added, and whether
// the file is there is a column. The page itself does not use this -- it opens the playlist's table as
// a windowed list, the same way Tutti i brani opens the library -- but a caller
// that wants the lot in one go has it.
// ---------------------------------------------------------------------------

int playlists_load(const char *name, char ***paths_out, char ***titles_out, char ***artists_out) {
	if (paths_out) {
		*paths_out = NULL;
	}
	if (titles_out) {
		*titles_out = NULL;
	}
	if (artists_out) {
		*artists_out = NULL;
	}
	if (!name_is_usable(name)) {
		return 0;
	}

	int capacity = playlists_count_tracks(name);
	if (capacity <= 0) {
		return 0;
	}

	char **paths = malloc((size_t)capacity * sizeof(*paths));
	char **titles = malloc((size_t)capacity * sizeof(*titles));
	char **artists = malloc((size_t)capacity * sizeof(*artists));
	if (!paths || !titles || !artists) {
		free(paths);
		free(titles);
		free(artists);
		return 0;
	}

	library_playlist_row_t page[PLAYLIST_PAGE];
	int count = 0;
	for (int offset = 0; count < capacity; offset += PLAYLIST_PAGE) {
		int got = library_playlist_page(name, offset, PLAYLIST_PAGE, page);
		if (got <= 0) {
			break;
		}
		for (int i = 0; i < got && count < capacity; i++) {
			if (!page[i].present) {
				continue;
			}
			paths[count] = strdup(page[i].path);
			titles[count] = strdup(page[i].title);
			artists[count] = page[i].artist[0] ? strdup(page[i].artist) : NULL;
			if (!paths[count] || !titles[count]) {
				goto done;
			}
			count++;
		}
		if (got < PLAYLIST_PAGE) {
			break;
		}
	}
done:

	if (count == 0) {
		playlists_free_list(paths, 0);
		playlists_free_list(titles, 0);
		playlists_free_list(artists, 0);
		return 0;
	}

	if (paths_out) {
		*paths_out = paths;
	} else {
		playlists_free_list(paths, count);
	}
	if (titles_out) {
		*titles_out = titles;
	} else {
		playlists_free_list(titles, count);
	}
	if (artists_out) {
		*artists_out = artists;
	} else {
		playlists_free_list(artists, count);
	}
	return count;
}

void playlists_free_list(char **list, int count) {
	if (!list) {
		return;
	}
	for (int i = 0; i < count; i++) {
		free(list[i]);
	}
	free(list);
}

// Both counters are the same question asked of the same column, and the answer
// is a COUNT over one small table: no card, no file, nothing to correct
// afterwards.
int playlists_count_tracks(const char *name) {
	return name_is_usable(name) ? library_playlist_count(name) : 0;
}

int playlists_count_lines(const char *name) { return playlists_count_tracks(name); }

// ---------------------------------------------------------------------------
// import
// ---------------------------------------------------------------------------

static bool has_playlist_extension(const char *file_name) {
	size_t len = strlen(file_name);
	if (len >= 5 && strcasecmp(file_name + len - 4, ".m3u") == 0) {
		return true;
	}
	return len >= 6 && strcasecmp(file_name + len - 5, ".m3u8") == 0;
}

// The playlist name a file name becomes: the stem, with what a card cannot
// carry replaced rather than refused. The source is somebody else's file, so
// its name is not rejected.
static void name_from_file(const char *file_name, char *out, size_t out_size) {
	const char *dot = strrchr(file_name, '.');
	size_t stem = dot ? (size_t)(dot - file_name) : strlen(file_name);
	if (stem >= out_size) {
		stem = out_size - 1;
	}
	memcpy(out, file_name, stem);
	out[stem] = '\0';

	for (char *c = out; *c; c++) {
		if (strchr("/\\:*?\"<>|", *c) != NULL) {
			*c = '_';
		}
	}
	size_t len = strlen(out);
	while (len > 0 && out[len - 1] == ' ') {
		out[--len] = '\0';
	}
	while (out[0] == '.') {
		memmove(out, out + 1, strlen(out));
	}
	if (!out[0]) {
		copy_capped(out, out_size, "Playlist");
	}
}

static int candidate_cmp(const void *a, const void *b) {
	const playlists_candidate_t *x = a;
	const playlists_candidate_t *y = b;
	return strcasecmp(x->name, y->name);
}

static void consider_candidate(const char *folder, const char *file_name, enum PlaylistLocation playlist_location, playlists_candidate_t *out,
							   int max, int *count) {
	if (*count >= max || !has_playlist_extension(file_name)) {
		return;
	}

	char full[512];
	if (snprintf(full, sizeof(full), "%s/%s", folder, file_name) >= (int)sizeof(full)) {
		return;
	}

	struct stat st;
	if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
		return;
	}
	playlists_candidate_t *slot = &out[*count];
	name_from_file(file_name, slot->name, sizeof(slot->name));
	// A file named like a playlist that is already there is offered all the
	// same, marked, since importing it replaces that playlist.
	slot->exists = library_playlist_exists(slot->name);
	copy_capped(slot->path, sizeof(slot->path), full);
	slot->playlist_location = playlist_location;
	(*count)++;
}

int playlists_importable(playlists_candidate_t *out, int max) {
	if (!out || max <= 0 || !card_root[0]) {
		return 0;
	}

	int count = 0;

	// The root of the card, and only the root: a playlist copied over lands
	// there, and walking the whole card looking for more would be the library
	// scan again.
	DIR *dir = opendir(card_root);
	if (dir) {
		struct dirent *de;
		while (count < max && (de = readdir(dir)) != NULL) {
			consider_candidate(card_root, de->d_name, PLAYLIST_LOCATION_SD_ROOT, out, max, &count);
		}
		closedir(dir);
	}

	// And the Playlist folder, which is where Backup writes. That is what brings
	// a deleted playlist back: its backup is still sitting there, and importing
	// it is the way in.
	if (dir_path[0]) {
		dir = opendir(dir_path);
		if (dir) {
			struct dirent *de;
			while (count < max && (de = readdir(dir)) != NULL) {
				consider_candidate(dir_path, de->d_name, PLAYLIST_LOCATION_PLAYLIST, out, max, &count);
			}
			closedir(dir);
		}
	}

	// And the playlist_data folder, which is where the original firmware stores its
	// m3u playlist files
	if (playlist_data_path[0]) {
		dir = opendir(playlist_data_path);
		if (dir) {
			struct dirent *de;
			while (count < max && (de = readdir(dir)) != NULL) {
				consider_candidate(playlist_data_path, de->d_name, PLAYLIST_LOCATION_PLAYLIST_DATA, out, max, &count);
			}
			closedir(dir);
		}
	}

	qsort(out, (size_t)count, sizeof(*out), candidate_cmp);
	return count;
}

// A playlist longer than this is not one somebody wrote. The cap is on what is
// held while importing: a path, a title and a little per entry.
#define IMPORT_MAX_ENTRIES 5000

typedef struct {
	char *path;	  // where the entry points, or where the track was found
	char *title;  // the source's #EXTINF text, NULL when it carried none
	long seconds; // and its duration, -1 for the M3U "unknown"
	bool found;
	bool by_name; // found by file name in the index rather than at its path
} import_entry_t;

typedef struct {
	char **hits;
	int count;
} match_ctx_t;

// One row of the index whose file name an unresolved entry asked for. The first
// match is kept: two tracks can share a file name in different folders, and
// there is nothing in a playlist line to tell them apart.
static void import_match_cb(int slot, const char *path, void *user) {
	match_ctx_t *ctx = user;
	if (slot < 0 || slot >= ctx->count || ctx->hits[slot]) {
		return;
	}
	ctx->hits[slot] = strdup(path);
}

static void import_entries_free(import_entry_t *entries, int count) {
	for (int i = 0; i < count; i++) {
		free(entries[i].path);
		free(entries[i].title);
	}
	free(entries);
}

// What the imported playlist is called. It takes the source file's name; when
// a playlist already answers to it a number is added, so importing a backup of
// something that still exists gives a second list rather than overwriting the
// first.
static bool import_name(const char *stem, char *name_out, size_t name_size) {
	for (int attempt = 1; attempt <= 99; attempt++) {
		char name[201];
		if (attempt == 1) {
			copy_capped(name, sizeof(name), stem);
		} else {
			snprintf(name, sizeof(name), "%.190s (%d)", stem, attempt);
		}
		if (!name_is_usable(name)) {
			return false;
		}
		if (!library_playlist_exists(name)) {
			copy_capped(name_out, name_size, name);
			return true;
		}
	}
	return false;
}

bool playlists_import(const char *source_path, bool overwrite, playlists_import_result_t *out) {
	if (!source_path || !source_path[0] || !out) {
		return false;
	}
	memset(out, 0, sizeof(*out));

	FILE *f = fopen(source_path, "r");
	if (!f) {
		return false;
	}

	import_entry_t *entries = calloc(IMPORT_MAX_ENTRIES, sizeof(*entries));
	if (!entries) {
		fclose(f);
		return false;
	}
	int count = 0;

	// Relative entries are read against the folder the file is in.
	char source_dir[512];
	copy_capped(source_dir, sizeof(source_dir), source_path);
	char *last_slash = strrchr(source_dir, '/');
	if (last_slash && last_slash != source_dir) {
		*last_slash = '\0';
	} else {
		copy_capped(source_dir, sizeof(source_dir), last_slash ? "/" : ".");
	}

	// --- what the file says -------------------------------------------------
	char line[1024];
	char pending_title[512] = {0};
	long pending_seconds = -1;

	while (count < IMPORT_MAX_ENTRIES && read_line(f, line, sizeof(line))) {
		char *text = line_body(line);
		if (!text[0] || is_marker_line(text)) {
			continue;
		}
		if (text[0] == '#') {
			if (strncmp(text, "#EXTINF:", 8) == 0) {
				pending_seconds = strtol(text + 8, NULL, 10);
				const char *comma = strchr(text + 8, ',');
				snprintf(pending_title, sizeof(pending_title), "%s", comma ? comma + 1 : "");
			}
			continue;
		}

		char resolved[1024];
		resolve_entry(text, source_dir, resolved, sizeof(resolved));
		bool found = entry_is_present(resolved);

		// A list copied into the Playlist folder often keeps paths written from
		// the card's root ("Music/..."); one more look there before the entry
		// goes to the by-name search, which cannot tell two 01.flac apart.
		if (!found && text[0] != '/' && text[0] != '\\' && card_root[0] && strcmp(source_dir, card_root) != 0 &&
			!(text[1] == ':' && (text[2] == '\\' || text[2] == '/'))) {
			char from_root[1024];
			resolve_entry(text, card_root, from_root, sizeof(from_root));
			if (entry_is_present(from_root)) {
				copy_capped(resolved, sizeof(resolved), from_root);
				found = true;
			}
		}

		import_entry_t *entry = &entries[count];
		entry->path = strdup(resolved);
		entry->found = found;
		entry->title = pending_title[0] ? strdup(pending_title) : NULL;
		entry->seconds = pending_seconds;
		pending_title[0] = '\0';
		pending_seconds = -1;
		if (!entry->path) {
			break;
		}
		count++;
	}
	fclose(f);

	out->total = count;
	if (count == 0) {
		import_entries_free(entries, count);
		return false;
	}

	// --- the rest, by file name, in one pass over the index -------------------
	const char **names = calloc((size_t)count, sizeof(*names));
	char **hits = calloc((size_t)count, sizeof(*hits));
	if (names && hits) {
		int wanted = 0;
		for (int i = 0; i < count; i++) {
			if (entries[i].found) {
				continue;
			}
			const char *slash = strrchr(entries[i].path, '/');
			names[i] = slash ? slash + 1 : entries[i].path;
			wanted++;
		}
		if (wanted > 0) {
			match_ctx_t ctx = {hits, count};
			library_match_basenames(names, count, import_match_cb, &ctx);
			for (int i = 0; i < count; i++) {
				if (!hits[i]) {
					continue;
				}
				// The index describes the card as it was scanned; the file can
				// have gone since, and a row pointing at nothing is exactly what
				// importing is meant to remove.
				if (entry_is_present(hits[i])) {
					free(entries[i].path);
					entries[i].path = hits[i];
					entries[i].found = true;
					entries[i].by_name = true;
					hits[i] = NULL;
				}
			}
		}
	}
	for (int i = 0; hits && i < count; i++) {
		free(hits[i]);
	}
	free(hits);
	free(names);

	for (int i = 0; i < count; i++) {
		if (entries[i].found) {
			out->found++;
			if (entries[i].by_name) {
				out->by_name++;
			}
		} else {
			out->missing++;
			if (out->missing_listed < PLAYLISTS_IMPORT_MISSING_MAX) {
				const char *slash = strrchr(entries[i].path, '/');
				copy_capped(out->missing_names[out->missing_listed], sizeof(out->missing_names[0]),
							slash ? slash + 1 : entries[i].path);
				out->missing_listed++;
			}
		}
	}

	if (out->found == 0) {
		// Nothing to write, and writing an empty playlist over the source would
		// be the worst possible outcome of pressing Import.
		import_entries_free(entries, count);
		return false;
	}

	// --- write it into the index as a playlist -------------------------------
	char stem[201];
	const char *slash = strrchr(source_path, '/');
	name_from_file(slash ? slash + 1 : source_path, stem, sizeof(stem));

	// Replacing: the rows go into a playlist of their own beside the old one,
	// which is only dropped once they are all in. A leftover from an import cut
	// short is cleared first.
	char name[201];
	bool replacing = overwrite && name_is_usable(stem) && library_playlist_exists(stem);
	if (replacing) {
		snprintf(name, sizeof(name), "%.180s (import)", stem);
		if (library_playlist_exists(name)) {
			library_playlist_drop(name);
		}
	} else if (!import_name(stem, name, sizeof(name))) {
		import_entries_free(entries, count);
		return false;
	}

	// The name each row will show, worked out once and written down: the index
	// first, then the track's own tags, and the source's #EXTINF text only when
	// neither answers -- that text is one string, "Artist - Title", and this
	// player wants the two apart. The tag reading is what name_entry() bounds,
	// which matters here: a list of hundreds the index has never seen would
	// otherwise be hundreds of files opened off the card.
	//
	// Straight into the playlist, a row at a time. Held in an array first, a
	// five-thousand-entry import would be nine megabytes of fixed-width rows.
	library_playlist_writer_t *writer = library_playlist_write_begin(name);
	if (!writer) {
		import_entries_free(entries, count);
		return false;
	}

	uint32_t tags_started_ms = 0;
	bool tags_budget_spent = false;
	int written = 0;
	bool ok = true;

	for (int i = 0; i < count && ok; i++) {
		if (!entries[i].found) {
			continue;
		}
		library_playlist_row_t row;
		memset(&row, 0, sizeof(row));
		copy_capped(row.path, sizeof(row.path), entries[i].path);
		row.seconds = entries[i].seconds;
		row.present = true; // only the entries found on the card are written
		name_entry(entries[i].path, entries[i].title, row.title, sizeof(row.title), row.artist, sizeof(row.artist),
				   &tags_started_ms, &tags_budget_spent);
		ok = library_playlist_write_row(writer, &row);
		if (ok) {
			written++;
		}
	}

	ok = library_playlist_write_end(writer, ok && written > 0);
	import_entries_free(entries, count);
	if (!ok) {
		if (replacing) {
			library_playlist_drop(name); // the old playlist stays as it was
		}
		return false;
	}

	// The new rows are complete: they take the old playlist's place. Should
	// that fail, they stay under the name they were written to rather than
	// being lost.
	if (replacing && library_playlist_drop(stem) && library_playlist_rename(name, stem)) {
		copy_capped(name, sizeof(name), stem);
	}

	copy_capped(out->name, sizeof(out->name), name);
	return true;
}

// ---------------------------------------------------------------------------
// Backup
//
// The playlist written out as an .m3u in the Playlist folder. It is a plain
// extended M3U with the marker on top, so it opens in any music program, and
// Import reads it back -- which is what makes it a way to undo a deletion
// rather than only a way to copy a list off the device.
//
// Written to a temporary and moved into place, so a card pulled out half way
// leaves the previous backup rather than half of a new one.
// ---------------------------------------------------------------------------

bool playlists_backup(const char *name, char *path_out, size_t path_size) {
	if (path_out && path_size) {
		path_out[0] = '\0';
	}
	char target[600];
	if (!file_for(name, target, sizeof(target)) || !ensure_dir()) {
		return false;
	}

	char temp[620];
	snprintf(temp, sizeof(temp), "%s.tmp", target);
	FILE *w = fopen(temp, "w");
	if (!w) {
		fprintf(stderr, "playlists: cannot write '%s': %s\n", temp, strerror(errno));
		return false;
	}
	fputs(MARKER_HEADER, w);

	// A pageful at a time, so a long playlist is not held in memory to be
	// written out, and the index is not locked for the length of a card write.
	library_playlist_row_t page[PLAYLIST_PAGE];
	for (int offset = 0;; offset += PLAYLIST_PAGE) {
		int got = library_playlist_page(name, offset, PLAYLIST_PAGE, page);
		if (got <= 0) {
			break;
		}
		for (int i = 0; i < got; i++) {
			// Every entry, including the ones whose file is missing: a backup
			// is what the playlist holds, and a track that is off the card
			// today may be back on it tomorrow.
			if (page[i].artist[0]) {
				fprintf(w, "#EXTINF:%ld,%s - %s\n", page[i].seconds, page[i].artist, page[i].title);
			} else {
				fprintf(w, "#EXTINF:%ld,%s\n", page[i].seconds, page[i].title);
			}
			fprintf(w, "%s\n", page[i].path);
		}
		if (got < PLAYLIST_PAGE) {
			break;
		}
	}

	if (fclose(w) != 0 || rename(temp, target) != 0) {
		remove(temp);
		return false;
	}
	if (path_out && path_size) {
		copy_capped(path_out, path_size, target);
	}
	return true;
}

bool playlists_file_stamp(const char *name, char *path_out, size_t path_size, long *mtime_out, long *size_out) {
	char path[600];
	if (!file_for(name, path, sizeof(path))) {
		return false;
	}
	struct stat st;
	if (stat(path, &st) != 0) {
		return false;
	}
	if (path_out && path_size > 0) {
		snprintf(path_out, path_size, "%s", path);
	}
	if (mtime_out) {
		*mtime_out = (long)st.st_mtime;
	}
	if (size_out) {
		*size_out = (long)st.st_size;
	}
	return true;
}
