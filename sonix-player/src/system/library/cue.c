#include "cue.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>

// A .cue is a few dozen short lines, so one fixed buffer read with fgets is
// the whole I/O strategy. The two caps below are not format limits, they are
// the answer to a hand-written or corrupt file: a line longer than the buffer
// keeps its first chunk and the rest is dropped, and a file that never stops
// giving lines stops being read.
#define CUE_LINE_MAX 1024
#define CUE_MAX_LINES 20000

// 100000 minutes is ~69 days of audio: past this the timestamp is not a
// timestamp, and stopping here keeps the millisecond maths far from overflow.
#define CUE_MAX_MINUTES 100000

// ---------------------------------------------------------------------------
// small string helpers
// ---------------------------------------------------------------------------

// Truncating copy. Same helper as metadata.c, for the same reason: every field
// here comes from the card and must never be trusted to fit.
static void copy_bounded(char *dst, size_t dst_size, const char *src) {
	if (dst_size == 0) {
		return;
	}
	size_t len = strlen(src);
	size_t copy_len = len < dst_size - 1 ? len : dst_size - 1;
	memcpy(dst, src, copy_len);
	dst[copy_len] = '\0';
}

static const char *skip_ws(const char *s) {
	while (*s == ' ' || *s == '\t') {
		s++;
	}
	return s;
}

// Copies the next whitespace-delimited word, upper-cased, and returns the
// position just after it. Commands and keywords are compared case-insensitively
// because sheets are written by hand as often as by a ripper.
static const char *next_keyword(const char *s, char *out, size_t size) {
	s = skip_ws(s);
	size_t n = 0;
	while (*s && *s != ' ' && *s != '\t') {
		if (n + 1 < size) {
			out[n++] = (char)toupper((unsigned char)*s);
		}
		s++;
	}
	out[n] = '\0';
	return s;
}

// The type token that trails a bare FILE name ("FILE album.wav WAVE"). Only a
// known keyword is dropped, so a file genuinely called "my album WAVE.flac"
// loses its last word but "song.wav" does not lose its name.
static bool is_file_type_word(const char *word) {
	static const char *types[] = {"WAVE", "MP3", "AIFF", "BINARY", "MOTOROLA",
								  "FLAC", "APE", "WV", "OGG", "VORBIS", "AAC",
								  "MP4", "M4A", "OPUS"};
	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		if (strcasecmp(word, types[i]) == 0) {
			return true;
		}
	}
	return false;
}

// Reads a command's argument: `TITLE "Some Song"` and `TITLE Some Song` both
// have to work, and the quoted form is the only one that can hold a trailing
// keyword safely. `drop_type_word` is for FILE, where a bare name is followed
// by the format token.
static void read_value(const char *s, char *out, size_t size, bool drop_type_word) {
	s = skip_ws(s);
	if (size == 0) {
		return;
	}
	out[0] = '\0';

	if (*s == '"') {
		s++;
		size_t n = 0;
		while (*s && *s != '"') {
			if (n + 1 < size) {
				out[n++] = *s;
			}
			s++;
		}
		out[n] = '\0';
		return;
	}

	copy_bounded(out, size, s);

	size_t len = strlen(out);
	while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t')) {
		out[--len] = '\0';
	}

	if (drop_type_word) {
		char *last = strrchr(out, ' ');
		if (last && is_file_type_word(last + 1)) {
			*last = '\0';
			len = strlen(out);
			while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t')) {
				out[--len] = '\0';
			}
		}
	}
}

// mm:ss:ff, 75 frames to the second. Also accepts mm:ss, which hand-written
// sheets do produce. Anything out of range is refused rather than clamped: a
// wrong offset would send the decoder into the middle of another track.
static bool parse_msf(const char *s, uint64_t *ms_out) {
	unsigned long field[3] = {0, 0, 0};
	int count = 0;

	s = skip_ws(s);
	while (count < 3) {
		if (!isdigit((unsigned char)*s)) {
			return false;
		}
		unsigned long value = 0;
		int digits = 0;
		while (isdigit((unsigned char)*s)) {
			if (digits < 9) { // no overflow, and nothing sane is this long
				value = value * 10 + (unsigned long)(*s - '0');
			}
			digits++;
			s++;
		}
		field[count++] = value;
		if (*s != ':') {
			break;
		}
		s++;
	}

	if (count < 2) {
		return false;
	}
	if (field[0] > CUE_MAX_MINUTES || field[1] > 59 || field[2] > 74) {
		return false;
	}

	*ms_out = ((uint64_t)field[0] * 60 + field[1]) * 1000 + field[2] * 1000 / 75;
	return true;
}

// ---------------------------------------------------------------------------
// locating the audio file
// ---------------------------------------------------------------------------

// Last path component of a FILE field. Sheets written on a PC carry a whole
// Windows path ("D:\rips\album.wav"); the name at the end is the only part
// that means anything on the card.
static const char *basename_of(const char *path) {
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static bool file_exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// FAT is case-insensitive but the sheet and the directory entry rarely agree
// on case ("Album.WAV" against album.wav). The kernel's vfat mount is not
// always forgiving, so the directory is scanned for a name that differs only
// in case.
static bool match_in_dir(const char *dir, const char *name, char *out, size_t size) {
	DIR *d = opendir(dir);
	if (!d) {
		return false;
	}

	bool found = false;
	struct dirent *de;
	while (!found && (de = readdir(d)) != NULL) {
		if (strcasecmp(de->d_name, name) != 0) {
			continue;
		}
		if ((size_t)snprintf(out, size, "%s/%s", dir, de->d_name) < size && file_exists(out)) {
			found = true;
		}
	}
	closedir(d);
	return found;
}

// Turns the FILE field into an absolute path on the card, or gives up.
static bool resolve_audio(const char *cue_path, const char *field, char *out, size_t size) {
	if (!field[0]) {
		return false;
	}

	// Backslashes only ever arrive from a PC-written sheet, where they are
	// separators; treating them as such is what makes basename_of work.
	char rel[CUE_LINE_MAX];
	copy_bounded(rel, sizeof(rel), field);
	for (char *c = rel; *c; c++) {
		if (*c == '\\') {
			*c = '/';
		}
	}

	char dir[512];
	copy_bounded(dir, sizeof(dir), cue_path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		if (dir[0] == '\0') {
			dir[0] = '/'; // the sheet sits in the root
			dir[1] = '\0';
		}
	} else {
		copy_bounded(dir, sizeof(dir), ".");
	}

	// As written, relative to the sheet unless the sheet gives an absolute path.
	if (rel[0] == '/') {
		copy_bounded(out, size, rel);
	} else if ((size_t)snprintf(out, size, "%s/%s", dir, rel) >= size) {
		out[0] = '\0'; // did not fit: only the bare name can still work
	}
	if (out[0] && file_exists(out)) {
		return true;
	}

	// Same folder as the name asked for, ignoring case.
	const char *base = basename_of(rel);
	if (out[0]) {
		char sub[512];
		copy_bounded(sub, sizeof(sub), out);
		char *sub_slash = strrchr(sub, '/');
		if (sub_slash) {
			*sub_slash = '\0';
			if (sub[0] == '\0') {
				sub[0] = '/';
				sub[1] = '\0';
			}
			if (match_in_dir(sub, sub_slash + 1, out, size)) {
				return true;
			}
			out[0] = '\0'; // match_in_dir may have left a partial path behind
		}
	}

	// Last resort: the bare name next to the sheet. Covers the stale absolute
	// path a PC left behind, and sub-folders that no longer exist.
	if ((size_t)snprintf(out, size, "%s/%s", dir, base) < size && file_exists(out)) {
		return true;
	}
	if (match_in_dir(dir, base, out, size)) {
		return true;
	}

	out[0] = '\0';
	return false;
}

// ---------------------------------------------------------------------------
// parsing
// ---------------------------------------------------------------------------

static void parse_rem(const char *args, cue_sheet_t *out) {
	char key[16];
	const char *rest = next_keyword(args, key, sizeof(key));

	if (strcmp(key, "GENRE") == 0) {
		read_value(rest, out->genre, sizeof(out->genre), false);
	} else if (strcmp(key, "DISCNUMBER") == 0) {
		char value[32];
		read_value(rest, value, sizeof(value), false);
		long disc = strtol(value, NULL, 10);
		if (disc >= 1 && disc <= 999) {
			out->disc = (int)disc;
		}
	} else if (strcmp(key, "DATE") == 0 || strcmp(key, "YEAR") == 0) {
		char value[32];
		read_value(rest, value, sizeof(value), false);
		// Taggers write "1973", "1973-05-01" and "May 1973"; the first run of
		// four digits is the only part worth keeping.
		for (const char *c = value; *c; c++) {
			if (!isdigit((unsigned char)*c)) {
				continue;
			}
			long year = strtol(c, NULL, 10);
			if (year >= 1000 && year <= 9999) {
				out->year = (int)year;
			}
			break;
		}
	}
}

// ---------------------------------------------------------------------------
// Markers inside a WAV
//
// A WAV can carry its own track list and needs no sheet beside it: the RIFF
// "cue " chunk is a list of points into the audio, and the "LIST"/"adtl" chunk
// that usually follows gives each one a label. It is what a vinyl rip made in
// Audition or Audacity looks like -- one long file, a marker where each track
// starts -- and without reading them the whole side is one row an hour long,
// which is exactly what a CUE sheet exists to avoid.
//
// The markers are turned into the same cue_sheet_t a sheet produces, with the
// WAV standing in for the sheet, so everything downstream -- the scanner, the
// decoder's window, the metadata -- carries on believing it is a sheet.
// ---------------------------------------------------------------------------

#define WAV_MAX_CHUNK_WALK 4096 // chunks to look at before calling it a loop

// The shortest stretch between two markers that is still a track. Editors
// leave stray points behind -- a loop, a note to self, a cut that was undone
// -- and without a floor one of them turns a recording into a list of
// fragments. Four seconds is the Red Book minimum for a CD track.
#define WAV_MIN_TRACK_MS 4000

static uint32_t le32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)); }

typedef struct {
	uint32_t id;	   // the cue point's own name, which the labels refer to
	uint64_t frame;	   // where it sits, in frames from the start of the audio
	char label[160];
} wav_marker_t;

static int marker_cmp(const void *a, const void *b) {
	const wav_marker_t *ma = a;
	const wav_marker_t *mb = b;
	if (ma->frame < mb->frame) {
		return -1;
	}
	return ma->frame > mb->frame ? 1 : 0;
}

// Reads the "cue " points and their labels. Returns how many were kept.
static int wav_read_markers(FILE *f, wav_marker_t *marks, int max_marks, uint32_t *rate_out,
							uint64_t *total_frames_out) {
	uint8_t header[12];
	if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
		return 0;
	}
	if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
		return 0;
	}

	int count = 0;
	uint32_t rate = 0;
	uint16_t channels = 0;
	uint16_t bits = 0;
	uint64_t data_bytes = 0;

	for (int chunk = 0; chunk < WAV_MAX_CHUNK_WALK; chunk++) {
		uint8_t head[8];
		if (fread(head, 1, sizeof(head), f) != sizeof(head)) {
			break;
		}
		uint32_t size = le32(head + 4);
		// off_t, not long: the sizes come off the disk as unsigned 32-bit and
		// this is a 32-bit target, so a two-gigabyte side -- 24/192 is one --
		// turns a long negative and sends the walk backwards. The markers sit
		// after the data chunk, so a walk that stops there finds none.
		off_t body = ftello(f);
		if (body < 0) {
			break;
		}
		// Chunks are padded to an even length, and the pad byte is not counted.
		off_t next = body + (off_t)size + (size & 1);

		if (memcmp(head, "fmt ", 4) == 0 && size >= 16) {
			uint8_t fmt[16];
			if (fread(fmt, 1, sizeof(fmt), f) == sizeof(fmt)) {
				channels = le16(fmt + 2);
				rate = le32(fmt + 4);
				bits = le16(fmt + 14);
			}
		} else if (memcmp(head, "data", 4) == 0) {
			data_bytes = size;
		} else if (memcmp(head, "cue ", 4) == 0 && size >= 4) {
			uint8_t points[4];
			if (fread(points, 1, sizeof(points), f) == sizeof(points)) {
				uint32_t declared = le32(points);
				// The count is the file's word against the chunk's own length;
				// believe whichever is smaller, and never more than fits.
				uint32_t by_size = (size - 4) / 24;
				if (declared > by_size) {
					declared = by_size;
				}
				for (uint32_t i = 0; i < declared && count < max_marks; i++) {
					uint8_t point[24];
					if (fread(point, 1, sizeof(point), f) != sizeof(point)) {
						break;
					}
					marks[count].id = le32(point);
					// Byte 20 is the sample offset inside the data chunk, which
					// is what every writer of these files actually fills in;
					// dwPosition (byte 4) is play-order and often zero.
					marks[count].frame = le32(point + 20);
					marks[count].label[0] = '\0';
					count++;
				}
			}
		} else if (memcmp(head, "LIST", 4) == 0 && size >= 4) {
			uint8_t type[4];
			if (fread(type, 1, sizeof(type), f) == sizeof(type) && memcmp(type, "adtl", 4) == 0) {
				off_t list_end = body + (off_t)size;
				for (int sub = 0; sub < WAV_MAX_CHUNK_WALK && ftello(f) + 8 <= list_end; sub++) {
					uint8_t sub_head[8];
					if (fread(sub_head, 1, sizeof(sub_head), f) != sizeof(sub_head)) {
						break;
					}
					uint32_t sub_size = le32(sub_head + 4);
					off_t sub_body = ftello(f);
					off_t sub_next = sub_body + (off_t)sub_size + (sub_size & 1);

					// "labl" is the marker's name, "note" a longer comment. The
					// name is what a track list wants.
					if (memcmp(sub_head, "labl", 4) == 0 && sub_size >= 5 && sub_size < 4096) {
						uint8_t id_bytes[4];
						if (fread(id_bytes, 1, sizeof(id_bytes), f) == sizeof(id_bytes)) {
							uint32_t id = le32(id_bytes);
							size_t text_len = sub_size - 4;
							char text[160];
							size_t take = text_len < sizeof(text) - 1 ? text_len : sizeof(text) - 1;
							if (fread(text, 1, take, f) == take) {
								text[take] = '\0';
								text[strcspn(text, "\r\n")] = '\0';
								for (int m = 0; m < count; m++) {
									if (marks[m].id == id) {
										copy_bounded(marks[m].label, sizeof(marks[m].label), text);
										break;
									}
								}
							}
						}
					}

					// A zero-size sub-chunk is legal and advances by its own
					// header; only a backwards step is a malformed file.
					if (sub_next < sub_body || sub_next > list_end || fseeko(f, sub_next, SEEK_SET) != 0) {
						break;
					}
				}
			}
		}

		// A zero-size chunk is legal -- "fact", an empty "LIST" -- and the walk
		// still advances by the eight bytes of the next header. Stopping on
		// one would abandon the markers that come after it.
		if (next < body || fseeko(f, next, SEEK_SET) != 0) {
			break;
		}
	}

	if (rate == 0 || channels == 0 || bits == 0) {
		return 0; // no format: nothing to turn frames into time with
	}

	if (rate_out) {
		*rate_out = rate;
	}
	if (total_frames_out) {
		uint64_t frame_bytes = (uint64_t)channels * (bits / 8);
		*total_frames_out = frame_bytes ? data_bytes / frame_bytes : 0;
	}
	return count;
}

// True when the name ends in .wav or .wave.
static bool is_wav_name(const char *path) {
	if (!path) {
		return false;
	}
	size_t len = strlen(path);
	return (len > 4 && strcasecmp(path + len - 4, ".wav") == 0) ||
		   (len > 5 && strcasecmp(path + len - 5, ".wave") == 0);
}

static bool cue_parse_wav(const char *wav_path, cue_sheet_t *out) {
	FILE *f = fopen(wav_path, "rb");
	if (!f) {
		return false;
	}

	// Ninety-nine markers are seventeen kilobytes. This is reached from the
	// playback thread under a decoder open and from the scan thread a dozen
	// directories deep, which is the same reason the sheet itself is not on
	// the stack.
	wav_marker_t *marks = malloc(sizeof(*marks) * CUE_MAX_TRACKS);
	if (!marks) {
		fclose(f);
		return false;
	}

	uint32_t rate = 0;
	uint64_t total_frames = 0;
	int count = wav_read_markers(f, marks, CUE_MAX_TRACKS, &rate, &total_frames);
	fclose(f);

	if (count <= 0 || rate == 0) {
		free(marks);
		return false;
	}

	qsort(marks, (size_t)count, sizeof(marks[0]), marker_cmp);

	// Markers past the end of the audio describe a file this is not, and one
	// that lands a moment before it would open a track with nothing in it.
	uint64_t min_frames = (uint64_t)rate * WAV_MIN_TRACK_MS / 1000;
	uint64_t last_usable = total_frames > min_frames ? total_frames - min_frames : 0;
	while (count > 0 && total_frames > 0 && marks[count - 1].frame >= last_usable) {
		count--;
	}
	// Markers closer together than a track can be are one boundary: the same
	// point written twice, or an editor's leftovers around it.
	int kept = 0;
	for (int i = 0; i < count; i++) {
		if (kept > 0 && marks[i].frame < marks[kept - 1].frame + min_frames) {
			continue;
		}
		marks[kept++] = marks[i];
	}
	count = kept;

	// One marker is not a track list. It is a loop point, a note, a cut -- and
	// splitting a recording in two on the strength of it would take the file
	// out of the library and put two halves of it in. Two or more, and the
	// file is describing its own tracks.
	if (count < 2) {
		free(marks);
		return false;
	}

	// A marker at the very start is the start of track one; markers that begin
	// further in leave a first track before them, which is what a rip with a
	// marker only at each boundary means.
	uint64_t first_ms = marks[0].frame * 1000ull / rate;
	bool implicit_first = first_ms >= WAV_MIN_TRACK_MS;

	int tracks = count + (implicit_first ? 1 : 0);
	if (tracks > CUE_MAX_TRACKS) {
		// A sheet holds ninety-nine tracks. Trimming loses the tail of a side
		// no real rip has; refusing would lose the whole side.
		tracks = CUE_MAX_TRACKS;
	}

	memset(out, 0, sizeof(*out));
	copy_bounded(out->audio_path, sizeof(out->audio_path), wav_path);
	out->track_count = tracks;

	for (int i = 0; i < tracks; i++) {
		cue_track_t *t = &out->tracks[i];
		t->number = i + 1;

		int mark = implicit_first ? i - 1 : i;
		t->begin_ms = (mark < 0) ? 0 : marks[mark].frame * 1000ull / rate;
		if (mark >= 0) {
			copy_bounded(t->title, sizeof(t->title), marks[mark].label);
		}
	}

	for (int i = 0; i + 1 < tracks; i++) {
		out->tracks[i].end_ms = out->tracks[i + 1].begin_ms;
	}

	free(marks);
	return true;
}

bool cue_parse(const char *cue_path, cue_sheet_t *out) {
	if (!cue_path || !cue_path[0] || !out) {
		return false;
	}

	// A WAV is its own sheet when it carries markers.
	if (is_wav_name(cue_path)) {
		return cue_parse_wav(cue_path, out);
	}

	memset(out, 0, sizeof(*out));

	FILE *f = fopen(cue_path, "rb");
	if (!f) {
		return false;
	}

	char file_field[CUE_LINE_MAX];
	file_field[0] = '\0';
	bool have_file = false;

	// A track only counts once an INDEX 01 has placed it; TRACK on its own is
	// a heading, and a heading with no offset cannot be played.
	bool placed[CUE_MAX_TRACKS];
	memset(placed, 0, sizeof(placed));

	int count = 0;	  // tracks kept, placed or not
	int current = -1; // index into out->tracks, -1 when the track was skipped
	// TITLE and PERFORMER mean the album before the first TRACK and the track
	// after it. A skipped track (data, over the cap, unreadable number) still
	// opens a track scope: its title belongs to it, not to the album.
	bool in_track = false;

	char line[CUE_LINE_MAX];
	bool first_line = true;
	bool discard_rest_of_line = false;
	long lines = 0;

	while (fgets(line, sizeof(line), f)) {
		if (++lines > CUE_MAX_LINES) {
			break;
		}

		size_t len = strlen(line);
		bool complete = (len > 0 && line[len - 1] == '\n');

		// An over-long line is read in chunks: the first chunk is parsed (a
		// truncated TITLE is better than a lost one), the rest is thrown away
		// so its tail is never mistaken for a command of its own.
		if (discard_rest_of_line) {
			discard_rest_of_line = !complete;
			continue;
		}
		discard_rest_of_line = !complete;

		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
			line[--len] = '\0';
		}

		const char *p = line;
		if (first_line) {
			first_line = false;
			if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
				p += 3; // UTF-8 BOM
			}
		}

		char command[16];
		const char *args = next_keyword(p, command, sizeof(command));
		if (!command[0]) {
			continue;
		}

		if (strcmp(command, "REM") == 0) {
			parse_rem(args, out);
		} else if (strcmp(command, "FILE") == 0) {
			// One file per sheet is what a CD rip looks like. A second FILE
			// means every offset after it is measured from a different zero,
			// so parsing stops and what is already correct is kept. At most
			// one track placed in the first file makes it a sheet over files
			// split one per track (see cue_sheet_t.split).
			if (have_file) {
				int placed_count = 0;
				for (int i = 0; i < count; i++) {
					placed_count += placed[i] ? 1 : 0;
				}
				out->split = placed_count <= 1;
				break;
			}
			read_value(args, file_field, sizeof(file_field), true);
			have_file = true;
		} else if (strcmp(command, "TITLE") == 0) {
			if (current >= 0) {
				read_value(args, out->tracks[current].title, sizeof(out->tracks[current].title), false);
			} else if (!in_track) {
				read_value(args, out->album, sizeof(out->album), false);
			}
		} else if (strcmp(command, "PERFORMER") == 0) {
			if (current >= 0) {
				read_value(args, out->tracks[current].performer, sizeof(out->tracks[current].performer), false);
			} else if (!in_track) {
				read_value(args, out->performer, sizeof(out->performer), false);
			}
		} else if (strcmp(command, "TRACK") == 0) {
			current = -1;
			in_track = true;

			char number[16], type[16];
			const char *rest = next_keyword(args, number, sizeof(number));
			next_keyword(rest, type, sizeof(type));

			// A data track on a mixed-mode disc is not in the audio file, so
			// it is skipped; a missing type is taken as audio, since sheets
			// that omit it are audio-only rips.
			if (type[0] && strcmp(type, "AUDIO") != 0) {
				continue;
			}
			char *end = NULL;
			long value = strtol(number, &end, 10);
			if (end == number || value < 1 || value > 9999) {
				continue;
			}
			if (count >= CUE_MAX_TRACKS) {
				continue; // a sheet claiming hundreds of tracks: keep the first 99
			}

			current = count++;
			out->tracks[current].number = (int)value;
		} else if (strcmp(command, "INDEX") == 0) {
			if (current < 0) {
				continue;
			}
			char number[16];
			const char *rest = next_keyword(args, number, sizeof(number));
			if (strtol(number, NULL, 10) != 1) {
				continue; // INDEX 00 is the pre-gap, higher indexes are sub-cues
			}
			uint64_t begin = 0;
			if (parse_msf(rest, &begin)) {
				out->tracks[current].begin_ms = begin;
				placed[current] = true;
			}
		}
		// Anything else (FLAGS, ISRC, SONGWRITER, CATALOG, PREGAP, POSTGAP,
		// or a command from a newer ripper) is not needed here.
	}

	fclose(f);

	if (!have_file || count == 0) {
		return false;
	}

	// Drop the headings that never got an INDEX 01, closing the gaps so the
	// array stays contiguous. Track numbers keep what the sheet wrote.
	int kept = 0;
	for (int i = 0; i < count; i++) {
		if (!placed[i]) {
			continue;
		}
		if (kept != i) {
			out->tracks[kept] = out->tracks[i];
		}
		kept++;
	}
	memset(&out->tracks[kept], 0, sizeof(out->tracks[0]) * (size_t)(CUE_MAX_TRACKS - kept));
	out->track_count = kept;
	if (kept == 0) {
		return false;
	}

	for (int i = 0; i < kept; i++) {
		if (!out->tracks[i].performer[0]) {
			copy_bounded(out->tracks[i].performer, sizeof(out->tracks[i].performer), out->performer);
		}
		// The last track runs to the end of the file, and so does any track a
		// scrambled sheet placed after the one that follows it.
		out->tracks[i].end_ms = 0;
		if (i + 1 < kept && out->tracks[i + 1].begin_ms > out->tracks[i].begin_ms) {
			out->tracks[i].end_ms = out->tracks[i + 1].begin_ms;
		}
	}

	if (!resolve_audio(cue_path, file_field, out->audio_path, sizeof(out->audio_path))) {
		return false; // the sheet describes a file that is not on the card
	}
	return true;
}

// ---------------------------------------------------------------------------
// paths
// ---------------------------------------------------------------------------

bool cue_wav_has_markers(const char *path) { return is_wav_name(path); }

bool cue_is_sheet(const char *path) {
	if (!path) {
		return false;
	}
	size_t len = strlen(path);
	return len > 4 && strcasecmp(path + len - 4, ".cue") == 0;
}

void cue_virtual_path(const char *cue_path, int track_number, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	if (!cue_path || track_number < 1) {
		out[0] = '\0';
		return;
	}
	snprintf(out, size, "%s?track=%d", cue_path, track_number);
}

bool cue_order_tracks(const char *a, const char *b, int *out) {
	if (!a || !b || !out) {
		return false;
	}
	char sheet_a[512], sheet_b[512];
	int ta = cue_split_path(a, sheet_a, sizeof(sheet_a));
	int tb = cue_split_path(b, sheet_b, sizeof(sheet_b));
	if (ta <= 0 || tb <= 0 || strcmp(sheet_a, sheet_b) != 0) {
		return false;
	}
	*out = ta - tb;
	return true;
}

int cue_split_path(const char *path, char *cue_out, size_t size) {
	if (cue_out && size > 0) {
		cue_out[0] = '\0';
	}
	if (!path) {
		return 0;
	}

	// A real file name may well contain '?', so the suffix only counts when it
	// is the last one and the whole tail is "?track=<digits>".
	const char *mark = strrchr(path, '?');
	int number = 0;
	if (mark && strncmp(mark, "?track=", 7) == 0 && isdigit((unsigned char)mark[7])) {
		char *end = NULL;
		long value = strtol(mark + 7, &end, 10);
		if (end && *end == '\0' && value >= 1 && value <= CUE_MAX_TRACKS) {
			number = (int)value;
		}
	}

	if (cue_out && size > 0) {
		if (number > 0) {
			size_t len = (size_t)(mark - path);
			if (len >= size) {
				len = size - 1;
			}
			memcpy(cue_out, path, len);
			cue_out[len] = '\0';
		} else {
			copy_bounded(cue_out, size, path);
		}
	}
	return number;
}
