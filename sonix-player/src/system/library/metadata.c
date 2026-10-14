#include "metadata.h"

#include "src/system/library/cue.h"
#include "src/system/decode/decode.h"
#include "src/system/decode/mp4.h"
#include "src/system/decode/apedec.h"
#include "src/system/library/oggtags.h"
#include "src/system/image/miniz/miniz_tinfl.h"
#include "src/system/core/config.h"
#include "src/system/core/utils.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Standard ID3v1 genre list, used to resolve numeric genre references from
// both ID3v1 tags and ID3v2 TCON frames written in the old "(N)" style. Past
// "Hard Rock" are Winamp's additions, which every tagger since has written.
static const char *id3v1_genres[] = {
	"Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge",
	"Hip-Hop", "Jazz", "Metal", "New Age", "Oldies", "Other", "Pop", "R&B",
	"Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska",
	"Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient",
	"Trip-Hop", "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical",
	"Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise",
	"AlternRock", "Bass", "Soul", "Punk", "Space", "Meditative",
	"Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic", "Darkwave",
	"Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream",
	"Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40", "Christian Rap",
	"Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave",
	"Psychedelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal",
	"Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll",
	"Hard Rock", "Folk", "Folk-Rock", "National Folk", "Swing", "Fast Fusion",
	"Bebop", "Latin", "Revival", "Celtic", "Bluegrass", "Avantgarde",
	"Gothic Rock", "Progressive Rock", "Psychedelic Rock", "Symphonic Rock",
	"Slow Rock", "Big Band", "Chorus", "Easy Listening", "Acoustic", "Humour",
	"Speech", "Chanson", "Opera", "Chamber Music", "Sonata", "Symphony",
	"Booty Bass", "Primus", "Porn Groove", "Satire", "Slow Jam", "Club", "Tango",
	"Samba", "Folklore", "Ballad", "Power Ballad", "Rhythmic Soul", "Freestyle",
	"Duet", "Punk Rock", "Drum Solo", "A Cappella", "Euro-House", "Dance Hall",
	"Goa", "Drum & Bass", "Club-House", "Hardcore", "Terror", "Indie", "BritPop",
	"Negerpunk", "Polsk Punk", "Beat", "Christian Gangsta Rap", "Heavy Metal",
	"Black Metal", "Crossover", "Contemporary Christian", "Christian Rock",
	"Merengue", "Salsa", "Thrash Metal", "Anime", "JPop", "Synthpop", "Abstract",
	"Art Rock", "Baroque", "Bhangra", "Big Beat", "Breakbeat", "Chillout",
	"Downtempo", "Dub", "EBM", "Eclectic", "Electro", "Electroclash", "Emo",
	"Experimental", "Garage", "Global", "IDM", "Illbient", "Industro-Goth",
	"Jam Band", "Krautrock", "Leftfield", "Lounge", "Math Rock", "New Romantic",
	"Nu-Breakz", "Post-Punk", "Post-Rock", "Psytrance", "Shoegaze", "Space Rock",
	"Trop Rock", "World Music", "Neoclassical", "Audiobook", "Audio Theatre",
	"Neue Deutsche Welle", "Podcast", "Indie Rock", "G-Funk", "Dubstep",
	"Garage Rock", "Psybient",
};
#define ID3V1_GENRE_COUNT (sizeof(id3v1_genres) / sizeof(id3v1_genres[0]))

// Copies a null-terminated string into a fixed-size destination, truncating
// safely. Avoids -Wformat-truncation noise from snprintf("%s", ...) when the
// compiler can't prove the source is short enough.
static void copy_bounded(char *dst, size_t dst_size, const char *src) {
	if (dst_size == 0)
		return;
	size_t len = strlen(src);
	size_t copy_len = len < dst_size - 1 ? len : dst_size - 1;
	memcpy(dst, src, copy_len);
	dst[copy_len] = '\0';
}

// One more value of a tag that can hold several -- a second ARTIST comment, the
// next string of an ID3v2.4 TPE1 -- after the ones already in `dst`, joined
// with "; ". A value already there, or one that does not fit whole, is left out.
static void append_value(char *dst, size_t dst_size, const char *value, size_t len) {
	while (len > 0 && (value[len - 1] == ' ' || value[len - 1] == '\0')) {
		len--;
	}
	if (len == 0 || dst_size == 0) {
		return;
	}
	size_t have = strlen(dst);
	if (have == 0) {
		size_t copy_len = len < dst_size - 1 ? len : dst_size - 1;
		memcpy(dst, value, copy_len);
		dst[copy_len] = '\0';
		return;
	}
	// Already among the values: compared one by one, without regard to case.
	const char *p = dst;
	while (*p) {
		const char *end = strstr(p, "; ");
		size_t n = end ? (size_t)(end - p) : strlen(p);
		if (n == len && strncasecmp(p, value, len) == 0) {
			return;
		}
		p = end ? end + 2 : p + n;
	}
	if (have + 2 + len >= dst_size) {
		return;
	}
	memcpy(dst + have, "; ", 2);
	memcpy(dst + have + 2, value, len);
	dst[have + 2 + len] = '\0';
}

// ---------------------------------------------------------------------------
// Vorbis comment parsing, shared by FLAC and OGG Vorbis
// ---------------------------------------------------------------------------

// ReplayGain arrives as text in every format that carries it: "-7.50 dB" for a
// gain, "0.988525" for a peak. One place to turn either into a number, so the
// four parsers below agree about what counts as present.
static bool replaygain_field(song_metadata_t *out, const char *key, const char *value, size_t value_len) {
	char buf[32];
	size_t n = value_len < sizeof(buf) - 1 ? value_len : sizeof(buf) - 1;
	memcpy(buf, value, n);
	buf[n] = '\0';

	char *end = NULL;
	double number = strtod(buf, &end);
	if (end == buf) {
		return false; // not a number at all
	}

	if (strcmp(key, "REPLAYGAIN_TRACK_GAIN") == 0) {
		out->track_gain_db = (float)number;
		out->has_track_gain = true;
	} else if (strcmp(key, "REPLAYGAIN_ALBUM_GAIN") == 0) {
		out->album_gain_db = (float)number;
		out->has_album_gain = true;
	} else if (strcmp(key, "REPLAYGAIN_TRACK_PEAK") == 0) {
		out->track_peak = (float)number;
	} else if (strcmp(key, "REPLAYGAIN_ALBUM_PEAK") == 0) {
		out->album_peak = (float)number;
	} else {
		return false;
	}
	return true;
}

// Applies a single "KEY=VALUE" Vorbis comment string (not null-terminated) to
// the relevant metadata field.
// ---------------------------------------------------------------------------
// Lyrics
//
// Read only by metadata_read_lyrics(), which switches these on for one call on
// its own thread: an ordinary read -- the library scan, the player -- never
// allocates for them.
// ---------------------------------------------------------------------------

#define LYRICS_MAX_BYTES (256u * 1024u)

static __thread bool lyrics_wanted;
static __thread char *lyrics_found;

// Whether the text carries LRC time tags: "[mm:ss" at the start of a line.
static bool lyrics_have_times(const char *text, size_t len) {
	for (size_t i = 0; i + 5 < len; i++) {
		if ((i == 0 || text[i - 1] == '\n') && text[i] == '[' && isdigit((unsigned char)text[i + 1])) {
			size_t j = i + 1;
			while (j < len && isdigit((unsigned char)text[j])) {
				j++;
			}
			if (j + 2 < len && text[j] == ':' && isdigit((unsigned char)text[j + 1])) {
				return true;
			}
		}
	}
	return false;
}

// The first lyrics found are kept, except that timed ones replace untimed.
static void lyrics_offer(const char *text, size_t len) {
	if (!lyrics_wanted || !text || len == 0) {
		return;
	}
	if (len > LYRICS_MAX_BYTES) {
		len = LYRICS_MAX_BYTES;
	}
	if (lyrics_found) {
		if (lyrics_have_times(lyrics_found, strlen(lyrics_found)) || !lyrics_have_times(text, len)) {
			return;
		}
		free(lyrics_found);
		lyrics_found = NULL;
	}
	lyrics_found = malloc(len + 1);
	if (lyrics_found) {
		memcpy(lyrics_found, text, len);
		lyrics_found[len] = '\0';
	}
}

// ---------------------------------------------------------------------------
// The description of an audiobook
//
// The same arrangement as the lyrics: switched on only by
// metadata_read_with_description(), for the audiobook scan.
//
// A file can carry more than one candidate. The ranks pick between them: a
// field named for the purpose (Vorbis or APE DESCRIPTION or SUMMARY, an MP4
// ldes, a TXXX:DESCRIPTION) beats an MP4 desc or an ID3 COMM frame, and a bare
// comment field comes last -- it is as often an encoder's signature as a
// summary. Of equal ranks the first wins.
// ---------------------------------------------------------------------------

#define DESCRIPTION_MAX_BYTES (8u * 1024u)

static __thread bool description_wanted;
static __thread char *description_found;
static __thread int description_found_rank;

// The HTML tags that end a paragraph or a line: <p>, </p>, <br>, <li>, <div>.
static bool description_tag_breaks(const char *tag, size_t len) {
	while (len && isspace((unsigned char)*tag)) {
		tag++;
		len--;
	}
	if (len && *tag == '/') {
		tag++;
		len--;
	}
	if (len >= 1 && tolower((unsigned char)tag[0]) == 'p' && (len == 1 || !isalpha((unsigned char)tag[1]))) {
		return true;
	}
	if (len >= 2 && tolower((unsigned char)tag[0]) == 'b' && tolower((unsigned char)tag[1]) == 'r') {
		return true;
	}
	if (len >= 2 && tolower((unsigned char)tag[0]) == 'l' && tolower((unsigned char)tag[1]) == 'i') {
		return true;
	}
	return len >= 3 && strncasecmp(tag, "div", 3) == 0;
}

// Kept as plain text: publishers paste the store page's HTML into the tag, so
// the markup is dropped, the common entities are turned back into their
// characters, and runs of blank lines become one line break.
static void description_offer(const char *text, size_t len, int rank) {
	if (!description_wanted || !text || len == 0 || (description_found && rank <= description_found_rank)) {
		return;
	}
	if (len > DESCRIPTION_MAX_BYTES) {
		len = DESCRIPTION_MAX_BYTES;
	}
	char *plain = malloc(len + 1);
	if (!plain) {
		return;
	}

	static const struct {
		const char *entity;
		char value;
	} ENTITIES[] = {
		{"&amp;", '&'}, {"&quot;", '"'}, {"&#39;", '\''}, {"&apos;", '\''}, {"&lt;", '<'}, {"&gt;", '>'}, {"&nbsp;", ' '},
	};

	size_t written = 0;
	for (size_t i = 0; i < len && text[i];) {
		if (text[i] == '<') {
			const char *end = memchr(text + i + 1, '>', len - i - 1);
			if (end) {
				if (description_tag_breaks(text + i + 1, (size_t)(end - text - i - 1)) && written && plain[written - 1] != '\n') {
					plain[written++] = '\n';
				}
				i = (size_t)(end - text) + 1;
				continue;
			}
		}
		bool entity = false;
		for (size_t e = 0; e < sizeof(ENTITIES) / sizeof(ENTITIES[0]); e++) {
			size_t n = strlen(ENTITIES[e].entity);
			if (n <= len - i && memcmp(text + i, ENTITIES[e].entity, n) == 0) {
				plain[written++] = ENTITIES[e].value;
				i += n;
				entity = true;
				break;
			}
		}
		if (entity) {
			continue;
		}
		char c = text[i++];
		if (c == '\r') {
			c = '\n';
		}
		if (c == '\n' && (!written || plain[written - 1] == '\n')) {
			continue;
		}
		if (c == '\t') {
			c = ' ';
		}
		if (c == ' ' && (!written || plain[written - 1] == '\n')) {
			continue;
		}
		plain[written++] = c;
	}
	while (written && isspace((unsigned char)plain[written - 1])) {
		written--;
	}
	plain[written] = '\0';

	if (!written) {
		free(plain);
		return;
	}
	free(description_found);
	description_found = plain;
	description_found_rank = rank;
}

// A compilation flag's value: "1", as every tagger writes it, or a word some
// write instead.
static bool flag_is_set(const char *value, size_t len) {
	while (len > 0 && (*value == ' ' || *value == '\0')) {
		value++;
		len--;
	}
	if (len == 0) {
		return false;
	}
	if (*value == '1') {
		return true;
	}
	return (len >= 4 && strncasecmp(value, "true", 4) == 0) || (len >= 3 && strncasecmp(value, "yes", 3) == 0);
}

// The album artists a compilation is credited to when its tracks carry no
// flag: still a compilation, and still a name that says nothing about who is
// singing.
static bool various_artists(const char *name) {
	return strcasecmp(name, "Various Artists") == 0 || strcasecmp(name, "Various") == 0 ||
		   strcasecmp(name, "VA") == 0 || strcasecmp(name, "V.A.") == 0;
}

// -1 until the configuration is first asked.
static int track_artist = -1;

bool metadata_track_artist(void) {
	if (track_artist < 0) {
		track_artist = config_get_bool("music", "track_artist", false) ? 1 : 0;
	}
	return track_artist != 0;
}

void metadata_set_track_artist(bool on) {
	track_artist = on ? 1 : 0;
	config_set_bool("music", "track_artist", on);
	config_save();
}

const char *metadata_shown_artist(const song_metadata_t *m) {
	if (!m) {
		return "";
	}
	if (metadata_track_artist()) {
		return m->artist[0] ? m->artist : m->album_artist;
	}
	if (!m->album_artist[0] || (m->artist[0] && (m->compilation || various_artists(m->album_artist)))) {
		return m->artist;
	}
	return m->album_artist;
}

static void apply_vorbis_comment(song_metadata_t *out, const char *comment, size_t len) {
	const char *eq = memchr(comment, '=', len);
	if (!eq)
		return;

	size_t key_len = (size_t)(eq - comment);
	size_t value_len = len - key_len - 1;
	const char *value = eq + 1;

	char key[32];
	if (key_len >= sizeof(key))
		return;
	for (size_t i = 0; i < key_len; i++) {
		key[i] = (char)toupper((unsigned char)comment[i]);
	}
	key[key_len] = '\0';

	if (strcmp(key, "LYRICS") == 0 || strcmp(key, "UNSYNCEDLYRICS") == 0 || strcmp(key, "UNSYNCED LYRICS") == 0 ||
		strcmp(key, "SYNCEDLYRICS") == 0) {
		lyrics_offer(value, value_len);
		return;
	}

	if (strcmp(key, "COMPILATION") == 0 || strcmp(key, "ITUNESCOMPILATION") == 0) {
		out->compilation = flag_is_set(value, value_len);
		return;
	}

	if (strcmp(key, "DESCRIPTION") == 0 || strcmp(key, "SUMMARY") == 0) {
		description_offer(value, value_len, 3);
		return;
	}
	if (strcmp(key, "COMMENT") == 0) {
		description_offer(value, value_len, 1);
		return;
	}

	// A repeated ARTIST or GENRE comment is another value of the same tag.
	if (strcmp(key, "ARTIST") == 0) {
		append_value(out->artist, sizeof(out->artist), value, value_len);
		return;
	}
	if (strcmp(key, "GENRE") == 0) {
		append_value(out->genre, sizeof(out->genre), value, value_len);
		return;
	}

	char *dst = NULL;
	size_t dst_size = 0;
	if (strcmp(key, "TITLE") == 0) {
		dst = out->title;
		dst_size = sizeof(out->title);
	} else if (strcmp(key, "ALBUMARTIST") == 0 || strcmp(key, "ALBUM ARTIST") == 0 || strcmp(key, "ALBUM_ARTIST") == 0) {
		dst = out->album_artist;
		dst_size = sizeof(out->album_artist);
	} else if (strcmp(key, "ALBUM") == 0) {
		dst = out->album;
		dst_size = sizeof(out->album);
	} else if (strcmp(key, "TRACKNUMBER") == 0 || (strcmp(key, "TRACK") == 0 && out->track_number == 0)) {
		// TRACK is what a few taggers write instead; TRACKNUMBER wins when a
		// file has both.
		char num_buf[16];
		size_t n = value_len < sizeof(num_buf) - 1 ? value_len : sizeof(num_buf) - 1;
		memcpy(num_buf, value, n);
		num_buf[n] = '\0';
		out->track_number = (int)strtol(num_buf, NULL, 10);
		return;
	} else if (strcmp(key, "DISCNUMBER") == 0 || strcmp(key, "DISC") == 0) {
		// DISC is what some older taggers write instead; both carry "2" or
		// "2/3", and strtol stops at the slash either way.
		char num_buf[16];
		size_t n = value_len < sizeof(num_buf) - 1 ? value_len : sizeof(num_buf) - 1;
		memcpy(num_buf, value, n);
		num_buf[n] = '\0';
		out->disc_number = (int)strtol(num_buf, NULL, 10);
		return;
	} else if (strcmp(key, "DATE") == 0 || (strcmp(key, "YEAR") == 0 && out->year == 0)) {
		// YEAR, likewise, only where there is no DATE.
		char num_buf[16];
		size_t n = value_len < sizeof(num_buf) - 1 ? value_len : sizeof(num_buf) - 1;
		memcpy(num_buf, value, n);
		num_buf[n] = '\0';
		out->year = (int)strtol(num_buf, NULL, 10);
		return;
	} else if (strncmp(key, "REPLAYGAIN_", 11) == 0) {
		replaygain_field(out, key, value, value_len);
		return;
	} else if (strcmp(key, "SERIES") == 0 || (strcmp(key, "MOVEMENTNAME") == 0 && !out->series[0])) {
		dst = out->series;
		dst_size = sizeof(out->series);
	} else if (strcmp(key, "SERIES-PART") == 0 || strcmp(key, "SERIES_PART") == 0 || strcmp(key, "SERIESPART") == 0 || ((strcmp(key, "MOVEMENT") == 0 || strcmp(key, "MOVEMENTNUMBER") == 0) && !out->series_part[0])) {
		dst = out->series_part;
		dst_size = sizeof(out->series_part);
	} else {
		return;
	}

	size_t copy_len = value_len < dst_size - 1 ? value_len : dst_size - 1;
	memcpy(dst, value, copy_len);
	dst[copy_len] = '\0';
}

// Ogg Vorbis and Opus: the same Vorbis comments, in the OpusTags packet or the
// Vorbis comment header. Pictures are skipped unread; any other comment is
// read whole, up to OGG_TEXT_LONG_MAX (long lyrics).
#define OGG_TEXT_MAX (64 * 1024)
#define OGG_TEXT_LONG_MAX (1024 * 1024)

static bool ogg_comment(void *user, const char *comment, size_t len) {
	apply_vorbis_comment((song_metadata_t *)user, comment, len);
	return true;
}

static bool ogg_long_comment(void *user, const char *key, size_t key_len, uint32_t value_len,
							 oggtags_value_t *value) {
	if (oggtags_key_is_picture(key, key_len) || value_len > OGG_TEXT_LONG_MAX) {
		return true;
	}
	size_t len = key_len + 1 + value_len;
	char *comment = malloc(len + 1);
	if (!comment) {
		return true;
	}
	memcpy(comment, key, key_len);
	comment[key_len] = '=';
	if (oggtags_value_read(value, comment + key_len + 1, value_len) == value_len) {
		comment[len] = '\0';
		apply_vorbis_comment((song_metadata_t *)user, comment, len);
	}
	free(comment);
	return true;
}

static void read_ogg_metadata(const char *filepath, song_metadata_t *out) {
	oggtags_walk(filepath, OGG_TEXT_MAX, ogg_comment, ogg_long_comment, out);
}

// ---------------------------------------------------------------------------
// MP3 (ID3v1 / ID3v2) parsing
// ---------------------------------------------------------------------------

static uint32_t syncsafe_to_uint32(const uint8_t b[4]) {
	return ((uint32_t)(b[0] & 0x7F) << 21) | ((uint32_t)(b[1] & 0x7F) << 14) |
		   ((uint32_t)(b[2] & 0x7F) << 7) | (uint32_t)(b[3] & 0x7F);
}

static uint32_t plain_to_uint32(const uint8_t b[4]) {
	return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

// Converts UTF-16 (LE or BE) text to UTF-8, stopping at a null terminator.
static void utf16_to_utf8(const uint8_t *data, size_t len, bool big_endian, char *out, size_t out_size) {
	size_t oi = 0;
	size_t i = 0;
	while (i + 1 < len && oi + 4 < out_size) {
		uint32_t unit = big_endian ? ((uint32_t)data[i] << 8 | data[i + 1]) : ((uint32_t)data[i + 1] << 8 | data[i]);
		i += 2;
		if (unit == 0)
			break;

		uint32_t cp = unit;
		if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < len) {
			uint32_t unit2 = big_endian ? ((uint32_t)data[i] << 8 | data[i + 1]) : ((uint32_t)data[i + 1] << 8 | data[i]);
			if (unit2 >= 0xDC00 && unit2 <= 0xDFFF) {
				cp = 0x10000 + ((unit - 0xD800) << 10) + (unit2 - 0xDC00);
				i += 2;
			}
		}

		if (cp < 0x80) {
			out[oi++] = (char)cp;
		} else if (cp < 0x800) {
			out[oi++] = (char)(0xC0 | (cp >> 6));
			out[oi++] = (char)(0x80 | (cp & 0x3F));
		} else if (cp < 0x10000) {
			out[oi++] = (char)(0xE0 | (cp >> 12));
			out[oi++] = (char)(0x80 | ((cp >> 6) & 0x3F));
			out[oi++] = (char)(0x80 | (cp & 0x3F));
		} else {
			out[oi++] = (char)(0xF0 | (cp >> 18));
			out[oi++] = (char)(0x80 | ((cp >> 12) & 0x3F));
			out[oi++] = (char)(0x80 | ((cp >> 6) & 0x3F));
			out[oi++] = (char)(0x80 | (cp & 0x3F));
		}
	}
	out[oi] = '\0';
}

// ---------------------------------------------------------------------------
// UTF-8 where it was not declared
//
// Text in a field that says Latin-1 (an ID3v2 frame with encoding 0x00, an
// ID3v1 tag) or says nothing (RIFF INFO, the text of a .dff) is quite often
// UTF-8: taggers write what the system gives them. Read as Latin-1, every
// accented letter comes out as two wrong ones. Well-formed UTF-8 with at least
// one sequence of two or more bytes is taken as UTF-8; real Latin-1 almost
// never is, since there the byte after an accented letter is another letter,
// not a continuation.
// ---------------------------------------------------------------------------

// The length of the UTF-8 sequence opened by `c`, or 0 when `c` cannot open
// one.
static size_t utf8_sequence_length(uint8_t c) {
	if (c < 0x80) {
		return 1;
	}
	if (c >= 0xC2 && c <= 0xDF) {
		return 2;
	}
	if (c >= 0xE0 && c <= 0xEF) {
		return 3;
	}
	if (c >= 0xF0 && c <= 0xF4) {
		return 4;
	}
	return 0;
}

// Whether `len` bytes, up to the first NUL, are well-formed UTF-8. Plain ASCII
// is. A Latin-1 accent on its own is not: a high byte has to open a sequence of
// the right length, and in Latin-1 text the byte after it is a letter.
static bool text_is_utf8(const uint8_t *p, size_t len) {
	size_t i = 0;
	while (i < len && p[i]) {
		size_t follow = utf8_sequence_length(p[i]);
		if (follow == 0) {
			return false;
		}
		i++;
		for (size_t k = 1; k < follow; k++, i++) {
			if (i >= len || (p[i] & 0xC0) != 0x80) {
				return false;
			}
		}
	}
	return true;
}

// Well-formed UTF-8 that is not plain ASCII.
static bool text_is_wide_utf8(const uint8_t *p, size_t len) {
	bool wide = false;
	for (size_t i = 0; i < len && p[i] && !wide; i++) {
		wide = p[i] >= 0x80;
	}
	return wide && text_is_utf8(p, len);
}

// ---------------------------------------------------------------------------
// Windows-1251 in a Latin-1 frame
//
// An ID3v2 frame that declares encoding 0x00 says its bytes are ISO-8859-1, and
// most taggers tell the truth. Russian ones often do not: they write
// Windows-1251 and leave the encoding byte at zero, as does every ID3v1 tag,
// which has no encoding byte at all. Read as Latin-1, "Алиса" comes out as
// "Àëèñà" -- still in the database and still sorting, but under A, in
// characters nobody is looking for.
//
// The two encodings cannot be told apart by the bytes alone, so this goes by
// how they are arranged. A Latin-1 name spends its high bytes one at a time --
// the e in "Café", the u in "Müller" -- with letters either side. Cyrillic
// written as 1251 is high bytes all the way through a word. So a run of two or
// more Cyrillic-range letters with no ASCII letter or digit touching either end
// is 1251; anything else is left as Latin-1.
// ---------------------------------------------------------------------------

// The half of the code page that is not simply А-я: punctuation, the Ukrainian
// and Belarusian letters, and the currency signs. 0 means "not in the page".
static const uint16_t CP1251_HIGH[64] = {
	0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, // 80-87
	0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F, // 88-8F
	0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, // 90-97
	0x0000, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F, // 98-9F
	0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, // A0-A7
	0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407, // A8-AF
	0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, // B0-B7
	0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457, // B8-BF
};

// One byte of the code page as a Unicode code point.
static uint32_t cp1251_code(uint8_t c) {
	if (c < 0x80) {
		return c;
	}
	if (c >= 0xC0) {
		// А-Я then а-я, in one contiguous stretch each.
		return (c < 0xE0) ? (uint32_t)(0x0410 + (c - 0xC0)) : (uint32_t)(0x0430 + (c - 0xE0));
	}
	uint16_t mapped = CP1251_HIGH[c - 0x80];
	return mapped ? mapped : (uint32_t)c;
}

// A Cyrillic LETTER in the code page -- what a word is made of, as opposed to
// the quotes and the currency signs that share the upper half.
static bool cp1251_is_letter(uint8_t c) { return c >= 0xC0 || c == 0xA8 || c == 0xB8; }

static bool ascii_alnum(uint8_t c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// A whole word of high bytes, with no ASCII letter touching either end, is the
// tell. An accent inside a Latin word always has letters beside it -- the u in
// "Müller", the o in "Motörhead", the three umlauts in "ÄÖÜX" -- while a
// Cyrillic word written as 1251 runs from one space to the next without an
// ASCII letter in sight. Two letters is enough of a word; the high bytes that
// are punctuation in both code pages are neither evidence nor objection.
static bool looks_like_cp1251(const uint8_t *data, size_t len) {
	size_t i = 0;
	while (i < len && data[i]) {
		if (!cp1251_is_letter(data[i])) {
			i++;
			continue;
		}

		size_t start = i;
		while (i < len && data[i] && cp1251_is_letter(data[i])) {
			i++;
		}

		bool free_left = start == 0 || !ascii_alnum(data[start - 1]);
		bool free_right = i >= len || data[i] == 0 || !ascii_alnum(data[i]);
		if (i - start >= 2 && free_left && free_right) {
			return true;
		}
	}
	return false;
}

// The same bytes as UTF-8, read through the code page.
static void cp1251_to_utf8(const uint8_t *data, size_t len, char *out, size_t out_size) {
	size_t oi = 0;
	for (size_t i = 0; i < len && data[i]; i++) {
		uint32_t code = cp1251_code(data[i]);
		if (code < 0x80) {
			if (oi + 1 >= out_size) {
				break;
			}
			out[oi++] = (char)code;
		} else if (code < 0x800) {
			if (oi + 2 >= out_size) {
				break;
			}
			out[oi++] = (char)(0xC0 | (code >> 6));
			out[oi++] = (char)(0x80 | (code & 0x3F));
		} else {
			if (oi + 3 >= out_size) {
				break;
			}
			out[oi++] = (char)(0xE0 | (code >> 12));
			out[oi++] = (char)(0x80 | ((code >> 6) & 0x3F));
			out[oi++] = (char)(0x80 | (code & 0x3F));
		}
	}
	out[oi] = '\0';
}

// Decodes ID3v2 frame text (encoding byte 0x00-0x03) into UTF-8.
static void id3_decode_text(uint8_t encoding, const uint8_t *data, size_t len, char *out, size_t out_size) {
	if (out_size == 0)
		return;
	out[0] = '\0';
	if (len == 0)
		return;

	switch (encoding) {
	case 0x00: { // ISO-8859-1 (Latin-1), or UTF-8 or Windows-1251 pretending to be it
		if (text_is_wide_utf8(data, len)) {
			size_t copy_len = strnlen((const char *)data, len);
			if (copy_len > out_size - 1) {
				copy_len = out_size - 1;
			}
			memcpy(out, data, copy_len);
			out[copy_len] = '\0';
			break;
		}
		if (looks_like_cp1251(data, len)) {
			cp1251_to_utf8(data, len, out, out_size);
			break;
		}
		size_t oi = 0;
		for (size_t i = 0; i < len && oi + 2 < out_size; i++) {
			uint8_t c = data[i];
			if (c == 0)
				break;
			if (c < 0x80) {
				out[oi++] = (char)c;
			} else {
				out[oi++] = (char)(0xC0 | (c >> 6));
				out[oi++] = (char)(0x80 | (c & 0x3F));
			}
		}
		out[oi] = '\0';
		break;
	}
	case 0x01: { // UTF-16 with BOM
		bool big_endian = false;
		size_t offset = 0;
		if (len >= 2) {
			if (data[0] == 0xFE && data[1] == 0xFF) {
				big_endian = true;
				offset = 2;
			} else if (data[0] == 0xFF && data[1] == 0xFE) {
				big_endian = false;
				offset = 2;
			}
		}
		utf16_to_utf8(data + offset, len - offset, big_endian, out, out_size);
		break;
	}
	case 0x02: // UTF-16BE, no BOM
		utf16_to_utf8(data, len, true, out, out_size);
		break;
	case 0x03: // UTF-8
	default: {
		size_t copy_len = len < out_size - 1 ? len : out_size - 1;
		size_t actual = 0;
		while (actual < copy_len && data[actual] != 0)
			actual++;
		memcpy(out, data, actual);
		out[actual] = '\0';
		break;
	}
	}
}

// ID3v2 TCON content is either plain text, the old-style "(N)" / "(N)Text"
// format referencing an ID3v1 genre index, or in v2.4 the bare number, along
// with "RX" and "CR" for a remix and a cover.
static void resolve_tcon_genre(const char *raw, char *out, size_t out_size) {
	if (raw[0] >= '0' && raw[0] <= '9') {
		char *endptr;
		long idx = strtol(raw, &endptr, 10);
		if (*endptr == '\0' && idx >= 0 && (size_t)idx < ID3V1_GENRE_COUNT) {
			copy_bounded(out, out_size, id3v1_genres[idx]);
			return;
		}
	}
	if (strcmp(raw, "RX") == 0 || strcmp(raw, "(RX)") == 0) {
		copy_bounded(out, out_size, "Remix");
		return;
	}
	if (strcmp(raw, "CR") == 0 || strcmp(raw, "(CR)") == 0) {
		copy_bounded(out, out_size, "Cover");
		return;
	}
	if (raw[0] == '(') {
		const char *end = strchr(raw, ')');
		if (end) {
			char num_buf[8];
			size_t num_len = (size_t)(end - raw - 1);
			if (num_len > 0 && num_len < sizeof(num_buf)) {
				memcpy(num_buf, raw + 1, num_len);
				num_buf[num_len] = '\0';

				char *endptr;
				long idx = strtol(num_buf, &endptr, 10);
				if (*endptr == '\0' && idx >= 0) {
					const char *rest = end + 1;
					if (*rest != '\0') {
						copy_bounded(out, out_size, rest);
						return;
					}
					if ((size_t)idx < ID3V1_GENRE_COUNT) {
						copy_bounded(out, out_size, id3v1_genres[idx]);
						return;
					}
				}
			}
		}
	}
	copy_bounded(out, out_size, raw);
}

// Every string of a text frame, joined with "; " (append_value). A string ends
// at a NUL, which in UTF-16 is two zero bytes on an even offset. A genre may be
// an ID3v1 number, resolved one string at a time.
static void id3_values(uint8_t encoding, const uint8_t *data, size_t len, char *out, size_t out_size, bool genre) {
	bool wide = encoding == 0x01 || encoding == 0x02;
	size_t start = 0;
	while (start < len) {
		size_t end = start;
		if (wide) {
			while (end + 1 < len && (data[end] || data[end + 1])) {
				end += 2;
			}
			if (end + 1 >= len) {
				end = len;
			}
		} else {
			while (end < len && data[end]) {
				end++;
			}
		}
		if (end > start) {
			char value[512];
			char resolved[256];
			id3_decode_text(encoding, data + start, end - start, value, sizeof(value));
			const char *v = value;
			if (genre) {
				resolve_tcon_genre(value, resolved, sizeof(resolved));
				v = resolved;
			}
			append_value(out, out_size, v, strlen(v));
		}
		start = end + (wide ? 2 : 1);
	}
}

// The largest text frame worth reading. A very long title of a few thousand
// characters fits comfortably; cover art does not, but it is not wanted here
// (APIC frames are skipped without being read), and a size larger than this
// only means the file declares something it is not.
#define ID3_MAX_TEXT_FRAME (512u * 1024u)

// How many frames to look at before giving up. A tag holds a few dozen;
// thousands mean the walk has wandered into random data.
#define ID3_MAX_FRAMES 1024

// How many ID3v2 tags in a row are read at the start of a file.
#define ID3_MAX_TAGS 4

// Past the encoded, terminated string at the start of `data`: one zero byte in
// the single-byte encodings, two on an even offset in the UTF-16 ones.
static size_t id3_skip_string(uint8_t encoding, const uint8_t *data, size_t len) {
	if (encoding == 1 || encoding == 2) {
		for (size_t i = 0; i + 1 < len; i += 2) {
			if (data[i] == 0 && data[i + 1] == 0) {
				return i + 2;
			}
		}
		return len;
	}
	const uint8_t *zero = memchr(data, 0, len);
	return zero ? (size_t)(zero - data) + 1 : len;
}

// USLT: encoding, language, a description, then the text.
static void id3_lyrics_uslt(const uint8_t *buf, size_t len) {
	if (len < 5) {
		return;
	}
	uint8_t encoding = buf[0];
	size_t pos = 4 + id3_skip_string(encoding, buf + 4, len - 4);
	if (pos >= len) {
		return;
	}
	size_t out_size = (len - pos) * 2 + 16;
	char *text = malloc(out_size);
	if (!text) {
		return;
	}
	id3_decode_text(encoding, buf + pos, len - pos, text, out_size);
	lyrics_offer(text, strlen(text));
	free(text);
}

// COMM: encoding, language, a short description, then the comment. Where
// Mp3tag and most audiobook tools put the publisher's summary.
static void id3_description_comm(const uint8_t *buf, size_t len) {
	if (len < 5) {
		return;
	}
	uint8_t encoding = buf[0];
	size_t pos = 4 + id3_skip_string(encoding, buf + 4, len - 4);
	if (pos >= len) {
		return;
	}
	size_t out_size = (len - pos) * 2 + 16;
	char *text = malloc(out_size);
	if (!text) {
		return;
	}
	id3_decode_text(encoding, buf + pos, len - pos, text, out_size);
	description_offer(text, strlen(text), 2);
	free(text);
}

// SYLT with millisecond stamps, written out as LRC so there is one timed form
// to read: encoding, language, stamp format, content type, a description,
// then pairs of text and a 32-bit time.
static void id3_lyrics_sylt(const uint8_t *buf, size_t len) {
	if (len < 7 || buf[4] != 2) {
		return; // stamps in MPEG frames are not supported
	}
	uint8_t encoding = buf[0];
	size_t pos = 6 + id3_skip_string(encoding, buf + 6, len - 6);
	size_t cap = len * 2 + 64;
	char *lrc = malloc(cap);
	char *line = malloc(len * 2 + 16);
	if (!lrc || !line) {
		free(lrc);
		free(line);
		return;
	}
	size_t used = 0;
	lrc[0] = '\0';
	while (pos < len) {
		size_t text_len = id3_skip_string(encoding, buf + pos, len - pos);
		if (pos + text_len + 4 > len) {
			break;
		}
		id3_decode_text(encoding, buf + pos, text_len, line, len * 2 + 16);
		const uint8_t *t = buf + pos + text_len;
		uint32_t ms = ((uint32_t)t[0] << 24) | ((uint32_t)t[1] << 16) | ((uint32_t)t[2] << 8) | t[3];
		pos += text_len + 4;
		char *nl = line;
		while (*nl == '\n' || *nl == '\r') {
			nl++; // some taggers start each entry with its line break
		}
		int n = snprintf(lrc + used, cap - used, "[%02u:%02u.%02u]%s\n", ms / 60000, (ms / 1000) % 60, (ms % 1000) / 10, nl);
		if (n < 0 || (size_t)n >= cap - used) {
			break;
		}
		used += (size_t)n;
	}
	lyrics_offer(lrc, used);
	free(lrc);
	free(line);
}

// TXXX: an encoded description, its terminator, then the value, in any of the
// four encodings. The pairs looked at: SERIES and SERIES-PART, which win over
// MVNM / MVIN; ReplayGain, which is where every tagger puts it on an MP3; the
// compilation flag, where ffmpeg writes it; the album artist, from the taggers
// that do not use TPE2; and the description, when it is wanted.
static void id3_txxx(song_metadata_t *out, uint8_t encoding, const uint8_t *data, size_t len) {
	bool wide = encoding == 0x01 || encoding == 0x02;
	size_t value_at = id3_skip_string(encoding, data, len);
	if (value_at >= len) {
		return; // a description and no value
	}
	size_t key_len = value_at - (wide ? 2 : 1);
	const uint8_t *value = data + value_at;
	size_t value_len = len - value_at;

	char key[48];
	id3_decode_text(encoding, data, key_len, key, sizeof(key));
	for (char *c = key; *c; c++) {
		*c = (char)toupper((unsigned char)*c);
	}

	if (description_wanted && (strcmp(key, "DESCRIPTION") == 0 || strcmp(key, "SUMMARY") == 0 || strcmp(key, "COMMENT") == 0)) {
		size_t size = value_len * 2 + 16;
		char *text = malloc(size);
		if (text) {
			id3_decode_text(encoding, value, value_len, text, size);
			description_offer(text, strlen(text), strcmp(key, "COMMENT") == 0 ? 1 : 3);
			free(text);
		}
		return;
	}

	char text[256];
	id3_decode_text(encoding, value, value_len, text, sizeof(text));
	if (strcmp(key, "SERIES") == 0) {
		copy_bounded(out->series, sizeof(out->series), text);
	} else if (strcmp(key, "SERIES-PART") == 0 || strcmp(key, "SERIES_PART") == 0 || strcmp(key, "SERIESPART") == 0) {
		copy_bounded(out->series_part, sizeof(out->series_part), text);
	} else if (strncmp(key, "REPLAYGAIN_", 11) == 0) {
		replaygain_field(out, key, text, strlen(text));
	} else if (strcmp(key, "COMPILATION") == 0) {
		out->compilation = flag_is_set(text, strlen(text));
	} else if ((strcmp(key, "ALBUM ARTIST") == 0 || strcmp(key, "ALBUMARTIST") == 0 || strcmp(key, "ALBUM_ARTIST") == 0) &&
			   !out->album_artist[0]) {
		// TPE2, when the tag has one too, overwrites this wherever it comes.
		copy_bounded(out->album_artist, sizeof(out->album_artist), text);
	}
}

// ---------------------------------------------------------------------------
// Reading a tag's bytes
//
// A tag is read where it lies, frame by frame, and a picture is stepped over
// with a seek rather than read. One kind cannot be: a v2.2 or v2.3 tag with
// the unsynchronisation flag in its header, which old versions of iTunes
// wrote. Every 0xFF in it that could be mistaken for the start of an MPEG
// frame is followed by a stuffed 0x00 that is not data, and the frame sizes
// count the data without it, so such a tag is read a byte at a time and the
// stuffing dropped on the way.
// ---------------------------------------------------------------------------

typedef struct {
	FILE *f;
	off_t pos;	   // in the file
	off_t end;	   // where the tag ends in the file
	bool unsync;   // the whole tag is unsynchronised
	bool after_ff; // the last byte read was 0xFF
} id3_stream_t;

// Reads `n` bytes of the tag into `buf`, or skips them when `buf` is NULL.
static bool id3s_read(id3_stream_t *s, uint8_t *buf, size_t n) {
	if (!s->unsync) {
		if ((off_t)n > s->end - s->pos) {
			return false;
		}
		bool ok = buf ? fread(buf, 1, n, s->f) == n : fseeko(s->f, (off_t)n, SEEK_CUR) == 0;
		s->pos += (off_t)n;
		return ok;
	}
	for (size_t got = 0; got < n;) {
		if (s->pos >= s->end) {
			return false;
		}
		int c = getc_unlocked(s->f);
		if (c == EOF) {
			return false;
		}
		s->pos++;
		if (s->after_ff && c == 0) {
			s->after_ff = false;
			continue;
		}
		s->after_ff = c == 0xFF;
		if (buf) {
			buf[got] = (uint8_t)c;
		}
		got++;
	}
	return true;
}

// Undoes the unsynchronisation of one v2.4 frame in place: every 0xFF 0x00
// becomes 0xFF. Returns the new length.
static size_t id3_resync(uint8_t *buf, size_t len) {
	size_t o = 0;
	for (size_t i = 0; i < len; i++) {
		buf[o++] = buf[i];
		if (buf[i] == 0xFF && i + 1 < len && buf[i + 1] == 0) {
			i++;
		}
	}
	return o;
}

// A frame identifier: capital letters and digits only.
static bool id3_frame_id_ok(const uint8_t *id, int len) {
	for (int i = 0; i < len; i++) {
		if (!((id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= '0' && id[i] <= '9'))) {
			return false;
		}
	}
	return true;
}

// Whether a frame can end at `at`: the end of the tag, the padding, or the
// header of another frame. Leaves the read position where it was.
static bool id3_frame_ends_at(id3_stream_t *s, off_t at) {
	if (at == s->end) {
		return true;
	}
	if (at > s->end) {
		return false;
	}
	uint8_t id[4];
	bool ok = s->end - at < 4 || (fseeko(s->f, at, SEEK_SET) == 0 && fread(id, 1, 4, s->f) == 4 &&
								  (id[0] == 0 || id3_frame_id_ok(id, 4)));
	fseeko(s->f, s->pos, SEEK_SET);
	return ok;
}

// The data of a compressed frame, inflated into a buffer of the size the frame
// declares. malloc'd, NULL when it does not inflate.
static uint8_t *id3_inflate(const uint8_t *data, size_t len, uint32_t size, size_t *out_len) {
	if (size == 0 || size > ID3_MAX_TEXT_FRAME) {
		return NULL;
	}
	uint8_t *out = malloc(size);
	if (!out) {
		return NULL;
	}
	size_t got = tinfl_decompress_mem_to_mem(out, size, data, len,
											 TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
	if (got == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || got == 0) {
		free(out);
		return NULL;
	}
	*out_len = got;
	return out;
}

// Every field `from` has that `out` lacks.
static void fill_missing(song_metadata_t *out, const song_metadata_t *from) {
	if (!out->title[0])
		copy_bounded(out->title, sizeof(out->title), from->title);
	if (!out->artist[0])
		copy_bounded(out->artist, sizeof(out->artist), from->artist);
	if (!out->album_artist[0])
		copy_bounded(out->album_artist, sizeof(out->album_artist), from->album_artist);
	if (!out->album[0])
		copy_bounded(out->album, sizeof(out->album), from->album);
	if (!out->genre[0])
		copy_bounded(out->genre, sizeof(out->genre), from->genre);
	if (!out->series[0])
		copy_bounded(out->series, sizeof(out->series), from->series);
	if (!out->series_part[0])
		copy_bounded(out->series_part, sizeof(out->series_part), from->series_part);
	if (out->track_number == 0)
		out->track_number = from->track_number;
	if (out->disc_number == 0)
		out->disc_number = from->disc_number;
	if (out->year == 0)
		out->year = from->year;
	out->compilation = out->compilation || from->compilation;
	if (!out->has_track_gain && from->has_track_gain) {
		out->has_track_gain = true;
		out->track_gain_db = from->track_gain_db;
	}
	if (!out->has_album_gain && from->has_album_gain) {
		out->has_album_gain = true;
		out->album_gain_db = from->album_gain_db;
	}
	if (out->track_peak == 0)
		out->track_peak = from->track_peak;
	if (out->album_peak == 0)
		out->album_peak = from->album_peak;
}

// Reads an ID3v2 tag at the current position of `f` -- the start of an MP3, or
// wherever a .dsf, a WAV or an AIFF keeps it. Returns true if a tag was found
// (regardless of whether any wanted frames were in it), and leaves `f` at its
// end.
//
// Positions are off_t: in a .dsf, a WAV and an AIFF the tag sits after the
// audio, which in a long hi-res recording is past two gigabytes, where a
// 32-bit ftell() has nothing to return.
static bool read_id3v2(FILE *f, song_metadata_t *out) {
	uint8_t header[10];
	if (fread(header, 1, 10, f) != 10)
		return false;
	if (memcmp(header, "ID3", 3) != 0)
		return false;

	uint8_t major_version = header[3];
	uint8_t flags = header[5];
	uint32_t tag_size = syncsafe_to_uint32(&header[6]);

	off_t tag_data_start = ftello(f);
	if (tag_data_start < 0)
		return true;
	off_t tag_end = tag_data_start + (off_t)tag_size;
	// A footer, ten bytes, follows a v2.4 tag that says it has one.
	off_t after_tag = tag_end + ((major_version >= 4 && (flags & 0x10)) ? 10 : 0);

	bool v22 = major_version == 2;
	bool v24 = major_version >= 4;
	// v2.2 compression was never given a scheme: there is nothing to read.
	if ((v22 && (flags & 0x40)) || major_version < 2 || major_version > 4) {
		fseeko(f, after_tag, SEEK_SET);
		return true;
	}

	// In v2.4 the header flag only says that every frame carries its own.
	id3_stream_t s = {f, tag_data_start, tag_end, !v24 && (flags & 0x80), false};

	if (!v22 && (flags & 0x40)) { // extended header present
		uint8_t ext_size_bytes[4];
		if (!id3s_read(&s, ext_size_bytes, 4)) {
			fseeko(f, after_tag, SEEK_SET);
			return true;
		}
		uint32_t ext_size = v24 ? syncsafe_to_uint32(ext_size_bytes) : plain_to_uint32(ext_size_bytes);
		uint32_t remaining = v24 ? (ext_size > 4 ? ext_size - 4 : 0) : ext_size;
		if (remaining > 0 && !id3s_read(&s, NULL, remaining)) {
			fseeko(f, after_tag, SEEK_SET);
			return true;
		}
	}

	// ID3v2.2 is a different shape: six-byte frame headers with three-character
	// identifiers and a three-byte size, where 2.3 and 2.4 have ten and four.
	// Walking a 2.2 tag with the ten-byte reader takes the size from the middle
	// of the next frame's id, which stops the loop at the first frame.
	int header_size = v22 ? 6 : 10;
	int original_year = 0;

	for (int frames = 0; frames < ID3_MAX_FRAMES && s.end - s.pos > header_size; frames++) {
		uint8_t frame_header[10];
		if (!id3s_read(&s, frame_header, (size_t)header_size))
			break;
		if (frame_header[0] == 0) // padding reached
			break;

		char frame_id[5] = {0};
		uint32_t frame_size;
		uint16_t frame_flags = 0;
		if (v22) {
			memcpy(frame_id, frame_header, 3);
			frame_size = ((uint32_t)frame_header[3] << 16) | ((uint32_t)frame_header[4] << 8) | frame_header[5];
		} else {
			memcpy(frame_id, frame_header, 4);
			frame_flags = (uint16_t)((frame_header[8] << 8) | frame_header[9]);
			uint32_t plain = plain_to_uint32(&frame_header[4]);
			frame_size = v24 ? syncsafe_to_uint32(&frame_header[4]) : plain;
			// Old versions of iTunes wrote v2.4 tags with the sizes as plain
			// numbers. They read the same as long as a frame is under 128
			// bytes, and a picture never is. A size with a high bit set cannot
			// be syncsafe at all; otherwise the plain reading is taken when it
			// lands on the next frame and the syncsafe one does not.
			if (v24 && plain != frame_size) {
				bool high = (frame_header[4] | frame_header[5] | frame_header[6] | frame_header[7]) & 0x80;
				if (high || (!id3_frame_ends_at(&s, s.pos + (off_t)frame_size) &&
							 id3_frame_ends_at(&s, s.pos + (off_t)plain))) {
					frame_size = plain;
				}
			}
		}

		// A frame of size zero is empty, not the end: some taggers write
		// an empty genre or comment like that, and the frames after it are
		// still there.
		if (frame_size == 0)
			continue;
		if ((off_t)frame_size > s.end - s.pos)
			break;

		// The extra bytes a flag puts in front of the data, the frames that
		// cannot be read, and the ones that are compressed.
		bool compressed = false;
		bool encrypted = false;
		bool frame_unsync = false;
		bool has_dli = false;
		size_t group_bytes = 0;
		if (major_version == 3) {
			compressed = frame_flags & 0x0080;
			encrypted = frame_flags & 0x0040;
			group_bytes = (frame_flags & 0x0020) ? 1 : 0;
		} else if (v24) {
			group_bytes = (frame_flags & 0x0040) ? 1 : 0;
			compressed = frame_flags & 0x0008;
			encrypted = frame_flags & 0x0004;
			frame_unsync = (frame_flags & 0x0002) || (flags & 0x80);
			has_dli = frame_flags & 0x0001;
		}

		// The allocation cap. The length field is 28 bits, so a damaged (or
		// merely badly written) file can declare a frame of two hundred and
		// fifty megabytes, and until here the only bound was the tag size,
		// which the same file declares. No title, artist or ReplayGain reaches
		// half a megabyte: past that the frame is skipped instead of allocated
		// for.
		if (frame_size > ID3_MAX_TEXT_FRAME || encrypted) {
			if (!id3s_read(&s, NULL, frame_size))
				break;
			continue;
		}

		// The 2.2 identifiers are the same fields under their older names; they
		// are translated here so that everything below reads one set.
		if (v22) {
			static const char *const V22_MAP[][2] = {{"TT2", "TIT2"}, {"TP1", "TPE1"}, {"TP2", "TPE2"},
													 {"TAL", "TALB"}, {"TCO", "TCON"}, {"TRK", "TRCK"},
													 {"TPA", "TPOS"}, {"TYE", "TYER"}, {"TXX", "TXXX"},
													 {"ULT", "USLT"}, {"SLT", "SYLT"}, {"TCP", "TCMP"},
													 {"COM", "COMM"}, {"TOR", "TORY"}};
			for (size_t i = 0; i < sizeof(V22_MAP) / sizeof(V22_MAP[0]); i++) {
				if (strcmp(frame_id, V22_MAP[i][0]) == 0) {
					snprintf(frame_id, sizeof(frame_id), "%s", V22_MAP[i][1]);
					break;
				}
			}
		}

		bool wanted = strcmp(frame_id, "MVNM") == 0 || strcmp(frame_id, "MVIN") == 0 || strcmp(frame_id, "TIT2") == 0 ||
					  strcmp(frame_id, "TPE1") == 0 || strcmp(frame_id, "TPE2") == 0 || strcmp(frame_id, "TALB") == 0 ||
					  strcmp(frame_id, "TCON") == 0 || strcmp(frame_id, "TRCK") == 0 || strcmp(frame_id, "TPOS") == 0 ||
					  strcmp(frame_id, "TYER") == 0 || strcmp(frame_id, "TDRC") == 0 || strcmp(frame_id, "TORY") == 0 ||
					  strcmp(frame_id, "TDOR") == 0 || strcmp(frame_id, "TXXX") == 0 || strcmp(frame_id, "TCMP") == 0;

		bool lyrics_frame = lyrics_wanted && (strcmp(frame_id, "USLT") == 0 || strcmp(frame_id, "SYLT") == 0);
		bool comment_frame = description_wanted && strcmp(frame_id, "COMM") == 0;

		if (!wanted && !lyrics_frame && !comment_frame) {
			if (!id3s_read(&s, NULL, frame_size))
				break;
			continue;
		}

		uint8_t *buf = malloc(frame_size);
		if (!buf)
			break;
		if (!id3s_read(&s, buf, frame_size)) {
			free(buf);
			break;
		}

		// v2.3 puts the inflated size first and the group after it; v2.4 the
		// group first and the data length indicator after it.
		uint8_t *data = buf;
		size_t len = frame_size;
		uint32_t inflated_size = 0;
		if (major_version == 3 && compressed && len >= 4) {
			inflated_size = plain_to_uint32(data);
			data += 4;
			len -= 4;
		}
		if (group_bytes > len) {
			free(buf);
			continue;
		}
		data += group_bytes;
		len -= group_bytes;
		if (has_dli) {
			if (len < 4) {
				free(buf);
				continue;
			}
			inflated_size = syncsafe_to_uint32(data);
			data += 4;
			len -= 4;
		}
		if (frame_unsync) {
			len = id3_resync(data, len);
		}
		uint8_t *inflated = NULL;
		if (compressed) {
			inflated = id3_inflate(data, len, inflated_size, &len);
			if (!inflated) {
				free(buf);
				continue;
			}
			data = inflated;
		}
		if (len == 0) {
			free(inflated);
			free(buf);
			continue;
		}

		if (comment_frame) {
			id3_description_comm(data, len);
		} else if (lyrics_frame) {
			if (strcmp(frame_id, "USLT") == 0) {
				id3_lyrics_uslt(data, len);
			} else {
				id3_lyrics_sylt(data, len);
			}
		} else if (strcmp(frame_id, "TXXX") == 0) {
			id3_txxx(out, data[0], data + 1, len - 1);
		} else if (v24 && (strcmp(frame_id, "TPE1") == 0 || strcmp(frame_id, "TCON") == 0)) {
			// In ID3v2.4 a text frame may hold several strings, each ended by
			// a terminator: several artists, several genres.
			bool genre = frame_id[1] == 'C';
			char *dst = genre ? out->genre : out->artist;
			dst[0] = '\0';
			id3_values(data[0], data + 1, len - 1, dst, genre ? sizeof(out->genre) : sizeof(out->artist), genre);
		} else {
			char decoded[512];
			id3_decode_text(data[0], data + 1, len - 1, decoded, sizeof(decoded));

			if (strcmp(frame_id, "TIT2") == 0) {
				copy_bounded(out->title, sizeof(out->title), decoded);
			} else if (strcmp(frame_id, "TPE1") == 0) {
				copy_bounded(out->artist, sizeof(out->artist), decoded);
			} else if (strcmp(frame_id, "TPE2") == 0) {
				copy_bounded(out->album_artist, sizeof(out->album_artist), decoded);
			} else if (strcmp(frame_id, "TCMP") == 0) {
				out->compilation = flag_is_set(decoded, strlen(decoded));
			} else if (strcmp(frame_id, "TALB") == 0) {
				copy_bounded(out->album, sizeof(out->album), decoded);
			} else if (strcmp(frame_id, "TCON") == 0) {
				resolve_tcon_genre(decoded, out->genre, sizeof(out->genre));
			} else if (strcmp(frame_id, "TRCK") == 0) {
				out->track_number = (int)strtol(decoded, NULL, 10);
			} else if (strcmp(frame_id, "TPOS") == 0) {
				// "2" or "2/3"; strtol stops at the slash.
				out->disc_number = (int)strtol(decoded, NULL, 10);
			} else if (strcmp(frame_id, "TYER") == 0 || strcmp(frame_id, "TDRC") == 0) {
				if (out->year == 0)
					out->year = (int)strtol(decoded, NULL, 10);
			} else if (strcmp(frame_id, "TORY") == 0 || strcmp(frame_id, "TDOR") == 0) {
				// The original release, for a tag that has no other year.
				original_year = (int)strtol(decoded, NULL, 10);
			} else if (strcmp(frame_id, "MVNM") == 0) {
				if (!out->series[0])
					copy_bounded(out->series, sizeof(out->series), decoded);
			} else if (strcmp(frame_id, "MVIN") == 0) {
				if (!out->series_part[0])
					copy_bounded(out->series_part, sizeof(out->series_part), decoded);
			}
		}
		free(inflated);
		free(buf);
	}

	if (out->year == 0) {
		out->year = original_year;
	}
	fseeko(f, after_tag, SEEK_SET);
	return true;
}

// Every ID3v2 tag at the current position of `f`, one after the other. A file
// can carry two: a tagger that put its own in front without removing the old
// one, which is quite often empty. The first tag's fields win and the later
// ones fill the gaps. Leaves `f` after the last tag, or where it was when there
// is none, past any zero bytes that follow. Returns how many were read.
static int read_id3v2_chain(FILE *f, song_metadata_t *out) {
	int tags = 0;
	song_metadata_t *more = NULL;
	for (; tags < ID3_MAX_TAGS; tags++) {
		off_t at = ftello(f);
		if (at < 0) {
			break;
		}
		if (tags == 0) {
			if (!read_id3v2(f, out)) {
				fseeko(f, at, SEEK_SET);
				break;
			}
		} else {
			if (!more && !(more = malloc(sizeof(*more)))) {
				fseeko(f, at, SEEK_SET);
				break;
			}
			memset(more, 0, sizeof(*more));
			if (!read_id3v2(f, more)) {
				fseeko(f, at, SEEK_SET);
				break;
			}
			fill_missing(out, more);
		}
		// Some taggers leave padding behind a tag's end, outside its size.
		int c = EOF;
		for (int zeros = 0; zeros < 4096 && (c = getc(f)) == 0; zeros++) {
		}
		if (c != EOF) {
			ungetc(c, f);
		}
	}
	free(more);
	return tags;
}

// Copies up to `max_src_len` raw ISO-8859-1 bytes from `src`, trimming
// trailing spaces, and decodes them into `dst` as UTF-8.
static void set_field_bounded(char *dst, size_t dst_size, const char *src, size_t max_src_len) {
	size_t len = 0;
	while (len < max_src_len && src[len] != '\0')
		len++;
	while (len > 0 && src[len - 1] == ' ')
		len--;
	id3_decode_text(0x00, (const uint8_t *)src, len, dst, dst_size);
}

// Reads the trailing 128-byte ID3v1 tag, filling only fields left empty by
// a preceding ID3v2 pass (if any).
static void read_id3v1(FILE *f, song_metadata_t *out) {
	if (fseeko(f, -128, SEEK_END) != 0)
		return;

	uint8_t tag[128];
	if (fread(tag, 1, 128, f) != 128)
		return;
	if (memcmp(tag, "TAG", 3) != 0)
		return;

	if (out->title[0] == '\0')
		set_field_bounded(out->title, sizeof(out->title), (const char *)tag + 3, 30);
	if (out->artist[0] == '\0')
		set_field_bounded(out->artist, sizeof(out->artist), (const char *)tag + 33, 30);
	if (out->album[0] == '\0')
		set_field_bounded(out->album, sizeof(out->album), (const char *)tag + 63, 30);

	if (out->year == 0) {
		char year_buf[5] = {0};
		memcpy(year_buf, tag + 93, 4);
		out->year = (int)strtol(year_buf, NULL, 10);
	}

	// ID3v1.1: a zero at the end of the comment, then the track number.
	if (out->track_number == 0 && tag[125] == 0 && tag[126] != 0) {
		out->track_number = tag[126];
	}

	if (out->genre[0] == '\0') {
		uint8_t genre_idx = tag[127];
		if (genre_idx < ID3V1_GENRE_COUNT) {
			snprintf(out->genre, sizeof(out->genre), "%s", id3v1_genres[genre_idx]);
		}
	}
}

// APEv2 items under their Vorbis comment names (see apev2_tag_cb).
static void apev2_tag_cb(void *user, const char *key, const char *value);

// The APEv2 tag at the end of a file, filling what `out` lacks.
static void fill_from_ape(const char *filepath, song_metadata_t *out) {
	song_metadata_t *ape = calloc(1, sizeof(*ape));
	if (!ape) {
		return;
	}
	apedec_tags(filepath, apev2_tag_cb, ape);
	fill_missing(out, ape);
	free(ape);
}

// An MP3 (and a raw .aac) carries ID3v2 in front, and may carry an APEv2 tag
// and an ID3v1 tag at the end: foobar2000 and Mp3tag can be set to write APE
// alone. They are read in that order, each filling what the ones before left.
static void read_mp3_metadata(const char *filepath, song_metadata_t *out) {
	FILE *f = fopen(filepath, "rb");
	if (!f)
		return;

	read_id3v2_chain(f, out);
	fill_from_ape(filepath, out);
	read_id3v1(f, out);

	fclose(f);
}

// ---------------------------------------------------------------------------
// FLAC
//
// The metadata blocks are walked here rather than by dr_flac, which gives up
// on the whole file -- tags and all -- at the first block it does not like: a
// picture whose lengths disagree, a comment count larger than the comments.
// Here a damaged block ends the walk and keeps what was read before it, and
// nothing past the metadata is looked at, so the audio can be in any state.
//
// Taggers that do not know FLAC put an ID3v2 tag in front of "fLaC", and a few
// put ID3v1 at the end. Both are read too, and fill in what the Vorbis
// comments, the format's own tags, leave empty.
// ---------------------------------------------------------------------------

#define FLAC_MAX_BLOCKS 1024
#define FLAC_BLOCK_VORBIS_COMMENT 4
#define FLAC_BLOCK_INVALID 127

// Comments up to this long are read in one piece. A longer one is a cover
// stored as text, skipped unread, or long lyrics, read up to
// FLAC_TEXT_LONG_MAX when they are wanted.
#define FLAC_TEXT_MAX (64 * 1024)
#define FLAC_TEXT_LONG_MAX (1024 * 1024)
#define FLAC_KEY_MAX 64

static uint32_t le32_bytes(const uint8_t *b) {
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

// One VORBIS_COMMENT block of `size` bytes at the current position of `f`.
// Each comment is checked against what is left of the block, and the first
// that does not fit ends the reading.
static void flac_read_comments(FILE *f, uint32_t size, song_metadata_t *out) {
	uint8_t b[4];
	uint32_t left = size;
	if (left < 8 || fread(b, 1, 4, f) != 4) {
		return;
	}
	left -= 4;
	uint32_t vendor = le32_bytes(b);
	if (vendor > left - 4 || fseeko(f, (off_t)vendor, SEEK_CUR) != 0 || fread(b, 1, 4, f) != 4) {
		return;
	}
	left -= vendor + 4;
	uint32_t count = le32_bytes(b);

	for (uint32_t i = 0; i < count && left >= 4; i++) {
		if (fread(b, 1, 4, f) != 4) {
			return;
		}
		left -= 4;
		uint32_t len = le32_bytes(b);
		if (len > left) {
			return;
		}
		left -= len;

		if (len <= FLAC_TEXT_MAX) {
			char *text = malloc((size_t)len + 1);
			if (!text) {
				return;
			}
			if (fread(text, 1, len, f) != len) {
				free(text);
				return;
			}
			text[len] = '\0';
			apply_vorbis_comment(out, text, len);
			free(text);
			continue;
		}

		// A long one: the key first, a byte at a time up to the '='.
		char key[FLAC_KEY_MAX];
		size_t key_len = 0;
		uint32_t read = 0;
		bool has_key = false;
		while (read < len && key_len < sizeof(key)) {
			int c = getc(f);
			if (c == EOF) {
				return;
			}
			read++;
			if (c == '=') {
				has_key = true;
				break;
			}
			key[key_len++] = (char)c;
		}
		uint32_t rest = len - read;
		if (has_key && (lyrics_wanted || description_wanted) && !oggtags_key_is_picture(key, key_len) &&
			rest <= FLAC_TEXT_LONG_MAX) {
			size_t whole = key_len + 1 + rest;
			char *comment = malloc(whole + 1);
			if (!comment) {
				return;
			}
			memcpy(comment, key, key_len);
			comment[key_len] = '=';
			if (fread(comment + key_len + 1, 1, rest, f) != rest) {
				free(comment);
				return;
			}
			comment[whole] = '\0';
			apply_vorbis_comment(out, comment, whole);
			free(comment);
		} else if (fseeko(f, (off_t)rest, SEEK_CUR) != 0) {
			return;
		}
	}
}

static void read_flac_metadata(const char *filepath, song_metadata_t *out) {
	FILE *f = fopen(filepath, "rb");
	if (!f) {
		return;
	}
	struct stat st;
	off_t file_end = fstat(fileno(f), &st) == 0 ? st.st_size : 0;

	song_metadata_t *id3 = calloc(1, sizeof(*id3));
	if (id3) {
		read_id3v2_chain(f, id3);
	}

	uint8_t magic[4];
	if (fread(magic, 1, 4, f) == 4 && memcmp(magic, "fLaC", 4) == 0) {
		for (int blocks = 0; blocks < FLAC_MAX_BLOCKS; blocks++) {
			uint8_t h[4];
			if (fread(h, 1, 4, f) != 4) {
				break;
			}
			int type = h[0] & 0x7F;
			uint32_t size = ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
			off_t body = ftello(f);
			if (type == FLAC_BLOCK_INVALID || body < 0) {
				break;
			}
			// A file cut short keeps the comments that made it.
			if ((off_t)size > file_end - body) {
				size = file_end > body ? (uint32_t)(file_end - body) : 0;
			}
			if (type == FLAC_BLOCK_VORBIS_COMMENT) {
				flac_read_comments(f, size, out);
			}
			if ((h[0] & 0x80) || fseeko(f, body + (off_t)size, SEEK_SET) != 0) {
				break;
			}
		}
	}

	if (id3) {
		fill_missing(out, id3);
		free(id3);
	}
	read_id3v1(f, out);
	fclose(f);
}

// ---------------------------------------------------------------------------
// Text with no declared encoding
//
// RIFF INFO, the native AIFF and .dff text: taken as UTF-8 when it is
// well-formed UTF-8, and otherwise as the single-byte text an ID3v1 tag holds
// (Latin-1, or Windows-1251 -- see id3_decode_text).
// ---------------------------------------------------------------------------

// One value into `out` as UTF-8. The value is usually NUL-terminated and
// sometimes padded with spaces; both are dropped.
static void info_text(const char *raw, size_t len, char *out, size_t out_size) {
	len = strnlen(raw, len);
	while (len > 0 && raw[len - 1] == ' ') {
		len--;
	}
	if (text_is_utf8((const uint8_t *)raw, len)) {
		size_t copy_len = len < out_size - 1 ? len : out_size - 1;
		memcpy(out, raw, copy_len);
		out[copy_len] = '\0';
	} else {
		id3_decode_text(0x00, (const uint8_t *)raw, len, out, out_size);
	}
}

// Whether a chunk header at `at` makes sense: an identifier of letters, digits
// and spaces, as every RIFF and IFF one is, and a size that fits before
// `limit`.
static bool chunk_fits(FILE *f, off_t at, off_t limit, bool big_endian) {
	uint8_t h[8];
	if (at + 8 > limit || fseeko(f, at, SEEK_SET) != 0 || fread(h, 1, 8, f) != 8) {
		return false;
	}
	for (int i = 0; i < 4; i++) {
		if (!isalnum(h[i]) && h[i] != ' ') {
			return false;
		}
	}
	uint32_t size = big_endian ? ((uint32_t)h[4] << 24) | ((uint32_t)h[5] << 16) | ((uint32_t)h[6] << 8) | h[7]
							   : le32_bytes(h + 4);
	return (off_t)size <= limit - (at + 8);
}

// Where the chunk after one of `size` bytes from `body` starts. RIFF and IFF
// pad an odd chunk to an even length, and some writers forget: when the padded
// position holds no sensible chunk header and the unpadded one does, the pad
// byte was never written.
static off_t chunk_next(FILE *f, off_t body, uint64_t size, off_t limit, bool big_endian) {
	off_t next = body + (off_t)size + (off_t)(size & 1);
	if ((size & 1) && next + 8 <= limit && !chunk_fits(f, next, limit, big_endian) &&
		chunk_fits(f, next - 1, limit, big_endian)) {
		next--;
	}
	return next;
}

static uint32_t be32_bytes(const uint8_t *b) {
	return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static uint64_t be64_bytes(const uint8_t *b) { return ((uint64_t)be32_bytes(b) << 32) | be32_bytes(b + 4); }

static off_t file_size_of(FILE *f) {
	struct stat st;
	return fstat(fileno(f), &st) == 0 ? st.st_size : 0;
}

// ---------------------------------------------------------------------------
// DSD
//
// Neither container invents a tag format of its own. A .dsf points at an
// ordinary ID3v2 tag with the eight bytes at offset 20 of its header -- the
// same tag an MP3 carries, so the same reader handles it. A .dff spells artist
// and title out in DIIN sub-chunks, and the taggers that write more (Mp3tag,
// foobar2000) add an "ID3 " chunk at the top level, which wins.
// ---------------------------------------------------------------------------

static void read_dsf_metadata(FILE *f, song_metadata_t *out) {
	unsigned char head[28];
	if (fread(head, 1, 28, f) != 28 || memcmp(head, "DSD ", 4) != 0) {
		return;
	}
	unsigned long long tag_at = 0;
	for (int i = 7; i >= 0; i--) {
		tag_at = (tag_at << 8) | head[20 + i];
	}
	// Zero means the file simply has no tag, which is common and not an error.
	if (tag_at == 0) {
		return;
	}
	// The offset is 64-bit and comes from the file, while `long` here is 32-bit.
	// Unchecked, a monstrous value truncates and sends the read to a random
	// point in the file, where anything resembling "ID3" is taken for a tag.
	if (fseeko(f, 0, SEEK_END) != 0) {
		return;
	}
	off_t size = ftello(f);
	if (size <= 0 || tag_at >= (unsigned long long)size) {
		return;
	}
	if (fseeko(f, (off_t)tag_at, SEEK_SET) != 0) {
		return;
	}
	read_id3v2(f, out);
}

// The DIAR and DITI sub-chunks of a DIIN chunk, from `sub` to `end`.
static void read_dff_diin(FILE *f, off_t sub, off_t end, song_metadata_t *out) {
	for (int sub_guard = 0; sub_guard < 64 && sub + 12 <= end; sub_guard++) {
		unsigned char sh[12];
		if (fseeko(f, sub, SEEK_SET) != 0 || fread(sh, 1, 12, f) != 12) {
			return;
		}
		uint64_t ssize = be64_bytes(sh + 4);
		if (ssize > (uint64_t)(end - (sub + 12))) {
			return;
		}
		bool artist = memcmp(sh, "DIAR", 4) == 0;
		bool title = memcmp(sh, "DITI", 4) == 0;
		if ((artist || title) && ssize >= 4 && ssize < 4096) {
			// Four bytes of length, then the text, not terminated.
			unsigned char len_be[4];
			if (fread(len_be, 1, 4, f) == 4) {
				uint32_t len = be32_bytes(len_be);
				if (len > ssize - 4) {
					len = (uint32_t)ssize - 4;
				}
				char text[256] = {0};
				if (len >= sizeof(text)) {
					len = sizeof(text) - 1;
				}
				if (fread(text, 1, len, f) == len) {
					info_text(text, len, artist ? out->artist : out->title,
							  artist ? sizeof(out->artist) : sizeof(out->title));
				}
			}
		}
		sub += 12 + (off_t)ssize + (off_t)(ssize & 1);
	}
}

static void read_dff_metadata(FILE *f, song_metadata_t *out) {
	unsigned char head[16];
	if (fread(head, 1, 16, f) != 16 || memcmp(head, "FRM8", 4) != 0) {
		return;
	}

	// The declared lengths are 64-bit and come off a card, so they are kept
	// 64-bit here and checked against the real end of the file: truncated into
	// a 32-bit position they can turn negative and send a walk backwards.
	off_t file_end = file_size_of(f);
	if (file_end < 16) {
		return;
	}

	song_metadata_t *diin = calloc(1, sizeof(*diin));
	if (!diin) {
		return;
	}

	// DIIN and ID3 sit at the top level after the audio, so the walk is flat:
	// step over every chunk (each padded to an even length, which the size
	// field does not include).
	off_t pos = 16;
	bool id3_read = false;
	for (int guard = 0; guard < 64; guard++) {
		unsigned char ch[12];
		if (fseeko(f, pos, SEEK_SET) != 0 || fread(ch, 1, 12, f) != 12) {
			break;
		}
		uint64_t size = be64_bytes(ch + 4);
		off_t body = pos + 12;
		if (size > (uint64_t)(file_end - body)) {
			break;
		}
		if (memcmp(ch, "DIIN", 4) == 0) {
			read_dff_diin(f, body, body + (off_t)size, diin);
		} else if (memcmp(ch, "ID3 ", 4) == 0 && !id3_read && fseeko(f, body, SEEK_SET) == 0) {
			id3_read = read_id3v2(f, out);
		}
		pos = body + (off_t)size + (off_t)(size & 1);
	}

	fill_missing(out, diin);
	free(diin);
}

// The container is told by its first bytes: the name is not always right.
static void read_dsd_metadata(const char *filepath, song_metadata_t *out) {
	FILE *f = fopen(filepath, "rb");
	if (!f) {
		return;
	}
	unsigned char magic[4];
	if (fread(magic, 1, 4, f) == 4 && fseeko(f, 0, SEEK_SET) == 0) {
		if (memcmp(magic, "DSD ", 4) == 0) {
			read_dsf_metadata(f, out);
		} else if (memcmp(magic, "FRM8", 4) == 0) {
			read_dff_metadata(f, out);
		}
	}
	fclose(f);
}

// ---------------------------------------------------------------------------
// WAV and AIFF
//
// WAV's own tags are the RIFF LIST/INFO chunk, and its text has no declared
// encoding. Windows taggers write it in the machine's ANSI code page, and a
// character that code page does not have -- every Chinese one on a Western
// machine, most of the Vietnamese letters with a tone mark on any machine -- is
// written as a literal "?". The bytes in the file are the question marks; no
// font can bring the letters back.
//
// So the same taggers (Mp3tag, foobar2000, Picard, dBpoweramp) also keep a
// whole ID3v2 tag in an "id3 " chunk, in UTF-16 or UTF-8, with every letter
// intact. That tag is read first and INFO only fills in what it left empty.
// Taggers that do not know WAV leave an ID3v2 tag after the RIFF, outside it,
// or an ID3v1 tag at the end of the file; those fill in last.
//
// AIFF has the same ID3 chunk, and NAME and AUTH chunks of its own for the
// title and the author, which fill in what the ID3 tag left empty.
// ---------------------------------------------------------------------------

// The fields of one INFO list, from `pos` (just after its "INFO") to
// `list_end`.
static void read_wav_info(FILE *f, off_t pos, off_t list_end, song_metadata_t *info) {
	for (int guard = 0; guard < 256 && pos + 8 <= list_end; guard++) {
		uint8_t h[8];
		if (fseeko(f, pos, SEEK_SET) != 0 || fread(h, 1, 8, f) != 8) {
			break;
		}
		uint32_t size = le32_bytes(h + 4);
		off_t body = pos + 8;
		// A value that runs past the list is read as far as the list goes.
		if ((off_t)size > list_end - body) {
			size = (uint32_t)(list_end - body);
		}

		char value[1024];
		uint32_t read_size = size < sizeof(value) - 1 ? size : sizeof(value) - 1;
		if (fread(value, 1, read_size, f) != read_size) {
			break;
		}
		value[read_size] = '\0';

		if (memcmp(h, "INAM", 4) == 0) {
			info_text(value, read_size, info->title, sizeof(info->title));
		} else if (memcmp(h, "IART", 4) == 0) {
			info_text(value, read_size, info->artist, sizeof(info->artist));
		} else if (memcmp(h, "IPRD", 4) == 0) {
			info_text(value, read_size, info->album, sizeof(info->album));
		} else if (memcmp(h, "IGNR", 4) == 0) {
			info_text(value, read_size, info->genre, sizeof(info->genre));
		} else if (memcmp(h, "ICRD", 4) == 0) {
			info->year = (int)strtol(value, NULL, 10);
		} else if (memcmp(h, "ITRK", 4) == 0 || memcmp(h, "IPRT", 4) == 0) {
			// "3" or "3/12"; strtol stops at the slash.
			if (info->track_number == 0) {
				info->track_number = (int)strtol(value, NULL, 10);
			}
		}

		pos = chunk_next(f, body, size, list_end, false);
	}
}

static void read_wav_metadata(const char *filepath, song_metadata_t *out) {
	FILE *f = fopen(filepath, "rb");
	if (!f)
		return;

	uint8_t riff_header[12];
	if (fread(riff_header, 1, 12, f) != 12 || memcmp(riff_header, "RIFF", 4) != 0 ||
		memcmp(riff_header + 8, "WAVE", 4) != 0) {
		fclose(f);
		return;
	}

	// The real end of the file. Chunk lengths are declared by the file and are
	// unsigned 32-bit, while `long` here is signed 32-bit: a length past two
	// gigabytes turns negative, so the next position lands before the current
	// one and the loop rereads the same bytes forever. Positions are off_t and
	// every one is checked against this.
	off_t file_end = file_size_of(f);
	off_t riff_end = 8 + (off_t)le32_bytes(riff_header + 4);

	song_metadata_t *info = calloc(1, sizeof(*info));
	if (!info) {
		fclose(f);
		return;
	}
	off_t id3_at = -1;

	off_t pos = 12;
	for (int guard = 0; guard < 1024 && pos + 8 <= file_end; guard++) {
		uint8_t h[8];
		if (fseeko(f, pos, SEEK_SET) != 0 || fread(h, 1, 8, f) != 8) {
			break;
		}
		uint32_t size = le32_bytes(h + 4);
		off_t body = pos + 8;

		// The declared length has to fit inside the file: if it does not, the
		// file is lying and there is nothing further on to read.
		if ((off_t)size > file_end - body) {
			break;
		}

		if (memcmp(h, "LIST", 4) == 0 && size >= 4) {
			char list_type[4];
			if (fread(list_type, 1, 4, f) == 4 && memcmp(list_type, "INFO", 4) == 0) {
				read_wav_info(f, body + 4, body + (off_t)size, info);
			}
		} else if ((memcmp(h, "id3 ", 4) == 0 || memcmp(h, "ID3 ", 4) == 0) && id3_at < 0) {
			id3_at = body;
		}

		pos = chunk_next(f, body, size, file_end, false);
	}

	if (id3_at >= 0 && fseeko(f, id3_at, SEEK_SET) == 0) {
		read_id3v2(f, out);
	}
	// An ID3v2 tag after the RIFF, outside it.
	if (riff_end >= 12 && riff_end + 10 <= file_end && fseeko(f, riff_end + (riff_end & 1), SEEK_SET) == 0) {
		song_metadata_t *after = calloc(1, sizeof(*after));
		if (after) {
			if (read_id3v2(f, after)) {
				fill_missing(out, after);
			}
			free(after);
		}
	}
	fill_missing(out, info);
	free(info);
	read_id3v1(f, out);

	fclose(f);
}

static void read_aiff_metadata(const char *filepath, song_metadata_t *out) {
	FILE *f = fopen(filepath, "rb");
	if (!f)
		return;

	uint8_t form[12];
	if (fread(form, 1, sizeof(form), f) != sizeof(form) || memcmp(form, "FORM", 4) != 0) {
		fclose(f);
		return;
	}
	off_t file_end = file_size_of(f);

	song_metadata_t *native = calloc(1, sizeof(*native));
	if (!native) {
		fclose(f);
		return;
	}

	// Sizes are big-endian here, and a chunk body is padded to an even length
	// the size does not count. The guard stops a damaged file walking for ever.
	off_t pos = 12;
	bool id3_read = false;
	for (int guard = 0; guard < 256 && pos + 8 <= file_end; guard++) {
		uint8_t ch[8];
		if (fseeko(f, pos, SEEK_SET) != 0 || fread(ch, 1, sizeof(ch), f) != sizeof(ch)) {
			break;
		}
		uint32_t size = be32_bytes(ch + 4);
		off_t body = pos + 8;
		if ((off_t)size > file_end - body) {
			break;
		}
		if ((memcmp(ch, "ID3 ", 4) == 0 || memcmp(ch, "id3 ", 4) == 0) && !id3_read) {
			id3_read = read_id3v2(f, out);
		} else if (memcmp(ch, "NAME", 4) == 0 || memcmp(ch, "AUTH", 4) == 0) {
			char text[256];
			size_t len = size < sizeof(text) - 1 ? size : sizeof(text) - 1;
			if (fread(text, 1, len, f) == len) {
				text[len] = '\0';
				bool name = ch[0] == 'N';
				info_text(text, len, name ? native->title : native->artist,
						  name ? sizeof(native->title) : sizeof(native->artist));
			}
		}
		pos = chunk_next(f, body, size, file_end, true);
	}

	fill_missing(out, native);
	free(native);
	fclose(f);
}

// ---------------------------------------------------------------------------
// CAF
//
// Apple's Core Audio Format keeps its tags in an "info" chunk: a count, then
// that many pairs of NUL-terminated UTF-8 strings, the key first ("title",
// "artist", "album", "track number", "year", "genre", ...).
// ---------------------------------------------------------------------------

#define CAF_INFO_MAX (64 * 1024)

static void caf_info_entry(song_metadata_t *out, const char *key, const char *value) {
	size_t len = strlen(value);
	if (strcasecmp(key, "title") == 0) {
		info_text(value, len, out->title, sizeof(out->title));
	} else if (strcasecmp(key, "artist") == 0) {
		info_text(value, len, out->artist, sizeof(out->artist));
	} else if (strcasecmp(key, "album") == 0) {
		info_text(value, len, out->album, sizeof(out->album));
	} else if (strcasecmp(key, "album artist") == 0 || strcasecmp(key, "album_artist") == 0) {
		info_text(value, len, out->album_artist, sizeof(out->album_artist));
	} else if (strcasecmp(key, "genre") == 0) {
		info_text(value, len, out->genre, sizeof(out->genre));
	} else if (strcasecmp(key, "track number") == 0 || strcasecmp(key, "track") == 0) {
		out->track_number = (int)strtol(value, NULL, 10);
	} else if (strcasecmp(key, "year") == 0 || (strcasecmp(key, "recorded date") == 0 && out->year == 0)) {
		out->year = (int)strtol(value, NULL, 10);
	}
}

static void read_caf_metadata(const char *filepath, song_metadata_t *out) {
	FILE *f = fopen(filepath, "rb");
	if (!f) {
		return;
	}
	uint8_t head[8];
	if (fread(head, 1, 8, f) != 8 || memcmp(head, "caff", 4) != 0) {
		fclose(f);
		return;
	}
	off_t file_end = file_size_of(f);

	// Chunk sizes are signed 64-bit, big-endian; -1 is the audio running to
	// the end of the file, and nothing comes after it.
	off_t pos = 8;
	for (int guard = 0; guard < 256 && pos + 12 <= file_end; guard++) {
		uint8_t ch[12];
		if (fseeko(f, pos, SEEK_SET) != 0 || fread(ch, 1, 12, f) != 12) {
			break;
		}
		uint64_t size = be64_bytes(ch + 4);
		off_t body = pos + 12;
		if (size > (uint64_t)(file_end - body)) {
			break;
		}
		if (memcmp(ch, "info", 4) == 0 && size >= 4 && size <= CAF_INFO_MAX) {
			char *buf = malloc((size_t)size + 1);
			if (buf && fread(buf, 1, (size_t)size, f) == size) {
				buf[size] = '\0';
				uint32_t count = be32_bytes((const uint8_t *)buf);
				size_t p = 4;
				for (uint32_t i = 0; i < count && p < size; i++) {
					const char *key = buf + p;
					p += strnlen(key, (size_t)size - p) + 1;
					if (p >= size) {
						break;
					}
					const char *value = buf + p;
					p += strnlen(value, (size_t)size - p) + 1;
					caf_info_entry(out, key, value);
				}
			}
			free(buf);
		}
		pos = body + (off_t)size;
	}
	fclose(f);
}

// ---------------------------------------------------------------------------
// MP4 / M4A / M4B: iTunes-style atoms, read by the container parser
// ---------------------------------------------------------------------------

static void read_mp4_metadata(const char *filepath, song_metadata_t *out) {
	mp4_file_t *m = mp4_open(filepath);
	if (!m) {
		return;
	}

	snprintf(out->title, sizeof(out->title), "%s", mp4_tag_title(m));
	snprintf(out->artist, sizeof(out->artist), "%s", mp4_tag_artist(m));
	snprintf(out->album_artist, sizeof(out->album_artist), "%s", mp4_tag_album_artist(m));
	out->compilation = mp4_tag_compilation(m);
	snprintf(out->album, sizeof(out->album), "%s", mp4_tag_album(m));
	snprintf(out->genre, sizeof(out->genre), "%s", mp4_tag_genre(m));
	out->year = mp4_tag_year(m);
	snprintf(out->series, sizeof(out->series), "%s", mp4_tag_series(m));
	snprintf(out->series_part, sizeof(out->series_part), "%s", mp4_tag_series_part(m));
	const char *lyrics = mp4_tag_lyrics(m);
	if (lyrics) {
		lyrics_offer(lyrics, strlen(lyrics));
	}
	const char *description = mp4_tag_description(m);
	if (description) {
		description_offer(description, strlen(description), mp4_tag_description_rank(m));
	}
	out->track_number = mp4_tag_track_number(m);
	out->disc_number = mp4_tag_disc_number(m);

	// The freeform "----" atoms, where iTunes-style ReplayGain lives.
	const char *value;
	if ((value = mp4_tag_freeform(m, "replaygain_track_gain")) != NULL && value[0]) {
		replaygain_field(out, "REPLAYGAIN_TRACK_GAIN", value, strlen(value));
	}
	if ((value = mp4_tag_freeform(m, "replaygain_album_gain")) != NULL && value[0]) {
		replaygain_field(out, "REPLAYGAIN_ALBUM_GAIN", value, strlen(value));
	}
	if ((value = mp4_tag_freeform(m, "replaygain_track_peak")) != NULL && value[0]) {
		replaygain_field(out, "REPLAYGAIN_TRACK_PEAK", value, strlen(value));
	}
	if ((value = mp4_tag_freeform(m, "replaygain_album_peak")) != NULL && value[0]) {
		replaygain_field(out, "REPLAYGAIN_ALBUM_PEAK", value, strlen(value));
	}

	mp4_close(m);
}

// ---------------------------------------------------------------------------
// Opus and WavPack
// ---------------------------------------------------------------------------


// APEv2 tags (.wv, .ape). The names are not the Vorbis comment ones --
// "Track" instead of "TRACKNUMBER", "Year" instead of "DATE" -- but the rest
// matches, so the few that differ are renamed and the same reader is reused.
// The ReplayGain entries already share their names.
static void apev2_tag_cb(void *user, const char *key, const char *value) {
	song_metadata_t *out = (song_metadata_t *)user;

	if (strcasecmp(key, "Lyrics") == 0 || strcasecmp(key, "UnsyncedLyrics") == 0) {
		lyrics_offer(value, strlen(value));
		return;
	}
	if (strcasecmp(key, "Description") == 0 || strcasecmp(key, "Summary") == 0 || strcasecmp(key, "Comment") == 0) {
		description_offer(value, strlen(value), strcasecmp(key, "Comment") == 0 ? 1 : 3);
		return;
	}

	const char *vorbis = key;
	if (strcasecmp(key, "Track") == 0) {
		vorbis = "TRACKNUMBER";
	} else if (strcasecmp(key, "Year") == 0 || strcasecmp(key, "date") == 0) {
		vorbis = "DATE";
	} else if (strcasecmp(key, "Album Artist") == 0 || strcasecmp(key, "AlbumArtist") == 0 ||
			   strcasecmp(key, "album_artist") == 0) {
		vorbis = "ALBUMARTIST";
	}

	char line[320];
	int n = snprintf(line, sizeof(line), "%s=%s", vorbis, value);
	if (n > 0) {
		size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
		apply_vorbis_comment(out, line, len);
	}
}

// An .ape or a .wv: APEv2 tags at the end of the file, and sometimes an ID3v1
// tag after them, or an ID3v2 tag in front, both of which fill whatever the
// APEv2 one left empty. Binary items, the cover among them, are skipped unread.
static void read_ape_metadata(const char *filepath, song_metadata_t *out) {
	apedec_tags(filepath, apev2_tag_cb, out);

	FILE *f = fopen(filepath, "rb");
	if (f) {
		song_metadata_t *id3 = calloc(1, sizeof(*id3));
		if (id3) {
			if (read_id3v2_chain(f, id3) > 0) {
				fill_missing(out, id3);
			}
			free(id3);
		}
		read_id3v1(f, out);
		fclose(f);
	}
}

// ---------------------------------------------------------------------------
// Sidecar files
//
// A track downloaded from a streaming service can arrive with no tags at all:
// Qobuz FLACs are often bare. What the file does not say the API does, and the
// downloader writes it alongside, in "<file>.tags":
//
//     title=...
//     artist=...
//     album=...
//     track=3
//
// It is read after the real tags and overrides whatever they said (see the
// note in read_sidecar_tags). The same function then serves the player, the
// details page, the favourites and the library scan without any of them
// knowing that streaming exists.
// ---------------------------------------------------------------------------

static void read_sidecar_tags(const char *filepath, song_metadata_t *out) {
	char path[600];
	if ((size_t)snprintf(path, sizeof(path), "%.560s.tags", filepath) >= sizeof(path)) {
		return;
	}

	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}

	char line[512];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		char *equals = strchr(line, '=');
		if (!equals) {
			continue;
		}
		*equals = '\0';
		const char *key = line;
		const char *value = equals + 1;
		if (!value[0]) {
			continue;
		}

		// The sidecar wins over the file's own tags. It exists only beside a
		// cached track (Qobuz, Tidal, a podcast episode) and was written from
		// what the service says, while the file's tags are whatever someone else
		// published -- a podcast episode's MP3 very often carries the tags of the
		// show, or of the most recent episode, or none at all.
		//
		// Music on the card is unaffected: no .tags file is there.
		if (strcmp(key, "title") == 0) {
			snprintf(out->title, sizeof(out->title), "%s", value);
		} else if (strcmp(key, "artist") == 0) {
			snprintf(out->artist, sizeof(out->artist), "%s", value);
		} else if (strcmp(key, "album") == 0) {
			snprintf(out->album, sizeof(out->album), "%s", value);
		} else if (strcmp(key, "album_artist") == 0) {
			snprintf(out->album_artist, sizeof(out->album_artist), "%s", value);
		} else if (strcmp(key, "track") == 0) {
			out->track_number = atoi(value);
		} else if (strcmp(key, "disc") == 0) {
			out->disc_number = atoi(value);
		} else if (strcmp(key, "year") == 0) {
			out->year = atoi(value);
		}
	}
	fclose(f);
}

// One track of a CUE sheet: the tags are in the sheet, not in the audio file.
// Reading the file would give the whole disc's tags for every track.
static bool read_cue_metadata(const char *filepath, song_metadata_t *out) {
	char sheet_path[512];
	int track = cue_split_path(filepath, sheet_path, sizeof(sheet_path));
	if (track <= 0) {
		return false;
	}

	// A sheet is thirty-five kilobytes, and tags are read from threads that
	// already carry deep call stacks.
	cue_sheet_t *cue = malloc(sizeof(*cue));
	if (!cue) {
		return false;
	}
	if (!cue_parse(sheet_path, cue) || track > cue->track_count) {
		free(cue);
		return false;
	}
	const cue_track_t *t = &cue->tracks[track - 1];

	copy_bounded(out->title, sizeof(out->title), t->title);
	copy_bounded(out->artist, sizeof(out->artist), t->performer[0] ? t->performer : cue->performer);
	copy_bounded(out->album, sizeof(out->album), cue->album);
	copy_bounded(out->album_artist, sizeof(out->album_artist), cue->performer);
	copy_bounded(out->genre, sizeof(out->genre), cue->genre);
	out->year = cue->year;
	out->track_number = t->number;
	out->disc_number = cue->disc;
	out->has_tags = out->title[0] || out->artist[0] || out->album[0] || out->genre[0];
	free(cue);
	return true;
}

// ---------------------------------------------------------------------------
// The text as it is kept
//
// Whatever the format, a field ends up as clean UTF-8 on one line: the list
// rows, the sort keys and the font all expect that. Text that is not UTF-8 at
// all -- a Vorbis comment or an APE item written in Latin-1, against what the
// format says -- is read as the single-byte text of an ID3v1 tag. A sequence
// cut in half at the end of a field too short for it is dropped. Line breaks
// and tabs become spaces, and the spaces at either end go, so a field of
// nothing but spaces is empty.
// ---------------------------------------------------------------------------

static void clean_text(char *s, size_t size) {
	size_t len = strlen(s);
	bool bad = false;
	for (size_t i = 0; i < len && !bad;) {
		size_t n = utf8_sequence_length((uint8_t)s[i]);
		if (n == 0) {
			bad = true;
			break;
		}
		size_t have = i + n <= len ? n : len - i;
		for (size_t k = 1; k < have; k++) {
			if (((uint8_t)s[i + k] & 0xC0) != 0x80) {
				bad = true;
				break;
			}
		}
		if (!bad && have < n) {
			// Cut short at the end -- but only a field filled to the brim was
			// cut; anywhere else this is a Latin-1 letter at the end of a word.
			if (len + 4 < size) {
				bad = true;
				break;
			}
			s[i] = '\0';
			len = i;
		}
		i += have;
	}
	if (bad) {
		char raw[512];
		size_t raw_len = len < sizeof(raw) - 1 ? len : sizeof(raw) - 1;
		memcpy(raw, s, raw_len);
		raw[raw_len] = '\0';
		id3_decode_text(0x00, (const uint8_t *)raw, raw_len, s, size);
	}

	for (char *c = s; *c; c++) {
		if ((unsigned char)*c < 0x20 || *c == 0x7F) {
			*c = ' ';
		}
	}
	len = strlen(s);
	while (len > 0 && s[len - 1] == ' ') {
		s[--len] = '\0';
	}
	size_t lead = 0;
	while (s[lead] == ' ') {
		lead++;
	}
	if (lead > 0) {
		memmove(s, s + lead, len - lead + 1);
	}
}

static void clean_fields(song_metadata_t *out) {
	clean_text(out->title, sizeof(out->title));
	clean_text(out->artist, sizeof(out->artist));
	clean_text(out->album_artist, sizeof(out->album_artist));
	clean_text(out->album, sizeof(out->album));
	clean_text(out->genre, sizeof(out->genre));
	clean_text(out->series, sizeof(out->series));
	clean_text(out->series_part, sizeof(out->series_part));
	out->has_tags = out->title[0] != '\0' || out->artist[0] != '\0' || out->album[0] != '\0' || out->genre[0] != '\0';
}

void metadata_read(const char *filepath, song_metadata_t *out) {
	memset(out, 0, sizeof(*out));

	if (read_cue_metadata(filepath, out)) {
		clean_fields(out);
		return;
	}

	// The format by the file's first bytes where they disagree with its name
	// (see decode_detect_format).
	decode_format_t format = decode_detect_format(filepath);
	switch (format) {
	case DECODE_FORMAT_MP3:
	// A raw .aac has no container to put tags in, so it carries them up front
	// the same way an mp3 does: an ID3v2 before the first frame, which is
	// exactly what the decoder skips on open.
	case DECODE_FORMAT_AAC_ADTS:
		read_mp3_metadata(filepath, out);
		break;
	case DECODE_FORMAT_FLAC:
		read_flac_metadata(filepath, out);
		break;
	case DECODE_FORMAT_OGG_VORBIS:
	case DECODE_FORMAT_OPUS:
		read_ogg_metadata(filepath, out);
		break;
	case DECODE_FORMAT_AAC_MP4:
		read_mp4_metadata(filepath, out);
		break;
	case DECODE_FORMAT_DSD:
		read_dsd_metadata(filepath, out);
		break;
	case DECODE_FORMAT_WAVPACK:
	case DECODE_FORMAT_APE:
		read_ape_metadata(filepath, out);
		break;
	default: {
		// WAV, which audio.c plays itself, and what libsndfile reads.
		unsigned char magic[4] = {0};
		FILE *f = fopen(filepath, "rb");
		if (f) {
			if (fread(magic, 1, 4, f) != 4) {
				memset(magic, 0, sizeof(magic));
			}
			fclose(f);
		}
		if (memcmp(magic, "RIFF", 4) == 0) {
			read_wav_metadata(filepath, out);
		} else if (memcmp(magic, "FORM", 4) == 0) {
			read_aiff_metadata(filepath, out);
		} else if (memcmp(magic, "caff", 4) == 0) {
			read_caf_metadata(filepath, out);
		}
		break;
	}
	}

	read_sidecar_tags(filepath, out);
	clean_fields(out);
}

char *metadata_read_lyrics(const char *filepath) {
	song_metadata_t *scratch = malloc(sizeof(*scratch));
	if (!scratch) {
		return NULL;
	}
	lyrics_wanted = true;
	lyrics_found = NULL;
	metadata_read(filepath, scratch);
	lyrics_wanted = false;
	free(scratch);

	char *found = lyrics_found;
	lyrics_found = NULL;
	return found;
}

char *metadata_read_with_description(const char *filepath, song_metadata_t *out) {
	description_wanted = true;
	description_found = NULL;
	description_found_rank = 0;
	metadata_read(filepath, out);
	description_wanted = false;

	char *found = description_found;
	description_found = NULL;
	return found;
}
