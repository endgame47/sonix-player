#include "metadata.h"

#include "src/system/library/cue.h"
#include "src/system/decode/decode.h"
#include "src/system/decode/dr_flac.h"
#include "src/system/decode/mp4.h"
#include "src/system/decode/apedec.h"
#include "src/system/library/oggtags.h"
#include "src/system/core/config.h"
#include "src/system/core/utils.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Standard ID3v1 genre list, used to resolve numeric genre references from
// both ID3v1 tags and ID3v2 TCON frames written in the old "(N)" style.
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
	"Hard Rock",
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
	} else if (strcmp(key, "ALBUMARTIST") == 0 || strcmp(key, "ALBUM ARTIST") == 0) {
		dst = out->album_artist;
		dst_size = sizeof(out->album_artist);
	} else if (strcmp(key, "ALBUM") == 0) {
		dst = out->album;
		dst_size = sizeof(out->album);
	} else if (strcmp(key, "TRACKNUMBER") == 0) {
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
	} else if (strcmp(key, "DATE") == 0) {
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

static void flac_meta_callback(void *pUserData, drflac_metadata *pMetadata) {
	if (pMetadata->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT)
		return;

	song_metadata_t *out = (song_metadata_t *)pUserData;

	drflac_vorbis_comment_iterator iter;
	drflac_init_vorbis_comment_iterator(&iter, pMetadata->data.vorbis_comment.commentCount,
										 pMetadata->data.vorbis_comment.pComments);

	drflac_uint32 comment_len;
	const char *comment;
	while ((comment = drflac_next_vorbis_comment(&iter, &comment_len)) != NULL) {
		apply_vorbis_comment(out, comment, comment_len);
	}
}

static void read_flac_metadata(const char *filepath, song_metadata_t *out) {
	drflac *flac = drflac_open_file_with_metadata(filepath, flac_meta_callback, out, NULL);
	if (flac) {
		drflac_close(flac);
	}
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
	case 0x00: { // ISO-8859-1 (Latin-1), or Windows-1251 pretending to be it
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

// ID3v2 TCON content is either plain text, or the old-style "(N)" / "(N)Text"
// format referencing an ID3v1 genre index.
static void resolve_tcon_genre(const char *raw, char *out, size_t out_size) {
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

// TXXX:SERIES and TXXX:SERIES-PART, in any of the four encodings. The
// description ends at a NUL, which in UTF-16 is two zero bytes on an even
// offset; the value follows it. These win over MVNM / MVIN.
static void txxx_series(song_metadata_t *out, uint8_t encoding, const uint8_t *data, size_t len) {
	bool wide = encoding == 0x01 || encoding == 0x02;
	size_t end = 0;
	if (wide) {
		while (end + 1 < len && (data[end] || data[end + 1])) {
			end += 2;
		}
	} else {
		while (end < len && data[end]) {
			end++;
		}
	}
	size_t value_at = end + (wide ? 2 : 1);
	if (end >= len || value_at > len) {
		return;
	}

	char key[48];
	id3_decode_text(encoding, data, end, key, sizeof(key));
	for (char *c = key; *c; c++) {
		*c = (char)toupper((unsigned char)*c);
	}
	bool series = strcmp(key, "SERIES") == 0;
	bool part = strcmp(key, "SERIES-PART") == 0 || strcmp(key, "SERIES_PART") == 0 || strcmp(key, "SERIESPART") == 0;
	if (description_wanted && (strcmp(key, "DESCRIPTION") == 0 || strcmp(key, "SUMMARY") == 0 || strcmp(key, "COMMENT") == 0)) {
		size_t size = (len - value_at) * 2 + 16;
		char *text = malloc(size);
		if (text) {
			id3_decode_text(encoding, data + value_at, len - value_at, text, size);
			description_offer(text, strlen(text), strcmp(key, "COMMENT") == 0 ? 1 : 3);
			free(text);
		}
		return;
	}
	if (!series && !part) {
		return;
	}
	char value[256];
	id3_decode_text(encoding, data + value_at, len - value_at, value, sizeof(value));
	if (series) {
		copy_bounded(out->series, sizeof(out->series), value);
	} else {
		copy_bounded(out->series_part, sizeof(out->series_part), value);
	}
}

// The largest text frame worth reading. A very long title of a few thousand
// characters fits comfortably; cover art does not, but it is not wanted here
// (APIC frames are skipped without being read), and a size larger than this
// only means the file declares something it is not.
#define ID3_MAX_TEXT_FRAME (512u * 1024u)

// How many frames to look at before giving up. A tag holds a dozen; thousands
// mean the walk has wandered into random data.
#define ID3_MAX_FRAMES 256

// Reads an ID3v2 tag at the current position of `f` -- the start of an MP3, or
// wherever a .dsf, a WAV or an AIFF keeps it. Returns true if a tag was found
// (regardless of whether any wanted frames were in it).
//
// Positions are off_t: in a .dsf, a WAV and an AIFF the tag sits after the
// audio, which in a long hi-res recording is past two gigabytes, where a
// 32-bit ftell() has nothing to return.
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

	if (flags & 0x40) { // extended header present
		uint8_t ext_size_bytes[4];
		if (fread(ext_size_bytes, 1, 4, f) != 4)
			return true;
		uint32_t ext_size = (major_version >= 4) ? syncsafe_to_uint32(ext_size_bytes) : plain_to_uint32(ext_size_bytes);
		off_t remaining = (major_version >= 4) ? (off_t)ext_size - 4 : (off_t)ext_size;
		if (remaining > 0)
			fseeko(f, remaining, SEEK_CUR);
	}

	// ID3v2.2 is a different shape: six-byte frame headers with three-character
	// identifiers and a three-byte size, where 2.3 and 2.4 have ten and four.
	// Walking a 2.2 tag with the ten-byte reader takes the size from the middle
	// of the next frame's id, which stops the loop at the first frame.
	bool v22 = major_version == 2;
	int header_size = v22 ? 6 : 10;

	for (int frames = 0; frames < ID3_MAX_FRAMES && ftello(f) < tag_end - header_size; frames++) {
		off_t frame_at = ftello(f);
		uint8_t frame_header[10];
		if (fread(frame_header, 1, (size_t)header_size, f) != (size_t)header_size)
			break;
		if (frame_header[0] == 0) // padding reached
			break;

		char frame_id[5] = {0};
		uint32_t frame_size;
		if (v22) {
			memcpy(frame_id, frame_header, 3);
			frame_size = ((uint32_t)frame_header[3] << 16) | ((uint32_t)frame_header[4] << 8) | frame_header[5];
		} else {
			memcpy(frame_id, frame_header, 4);
			frame_size =
				(major_version >= 4) ? syncsafe_to_uint32(&frame_header[4]) : plain_to_uint32(&frame_header[4]);
		}

		if (frame_size == 0 || (off_t)frame_size > tag_end - ftello(f))
			break;

		// The allocation cap. The length field is 28 bits, so a damaged (or
		// merely badly written) file can declare a frame of two hundred and
		// fifty megabytes, and until here the only bound was the tag size,
		// which the same file declares. No title, artist or ReplayGain reaches
		// half a megabyte: past that the frame is skipped instead of allocated
		// for. The jump to the next frame uses an absolute position computed
		// from the start of this one, so the position always moves forward
		// whatever happened in between.
		off_t next_frame = frame_at + header_size + (off_t)frame_size;
		if (next_frame <= frame_at) {
			break; // arithmetic that does not add up: stop instead of spinning
		}

		if (frame_size > ID3_MAX_TEXT_FRAME) {
			fseeko(f, next_frame, SEEK_SET);
			continue;
		}

		// The 2.2 identifiers are the same fields under their older names; they
		// are translated here so that everything below reads one set.
		if (v22) {
			static const char *const V22_MAP[][2] = {{"TT2", "TIT2"}, {"TP1", "TPE1"}, {"TP2", "TPE2"},
													 {"TAL", "TALB"}, {"TCO", "TCON"}, {"TRK", "TRCK"},
													 {"TPA", "TPOS"}, {"TYE", "TYER"}, {"TXX", "TXXX"},
													 {"ULT", "USLT"}, {"SLT", "SYLT"}, {"TCP", "TCMP"}, {"COM", "COMM"}};
			for (size_t i = 0; i < sizeof(V22_MAP) / sizeof(V22_MAP[0]); i++) {
				if (strcmp(frame_id, V22_MAP[i][0]) == 0) {
					snprintf(frame_id, sizeof(frame_id), "%s", V22_MAP[i][1]);
					break;
				}
			}
		}

		bool wanted = strcmp(frame_id, "MVNM") == 0 || strcmp(frame_id, "MVIN") == 0 || strcmp(frame_id, "TIT2") == 0 || strcmp(frame_id, "TPE1") == 0 || strcmp(frame_id, "TPE2") == 0 || strcmp(frame_id, "TALB") == 0 || strcmp(frame_id, "TCON") == 0 || strcmp(frame_id, "TRCK") == 0 || strcmp(frame_id, "TPOS") == 0 || strcmp(frame_id, "TYER") == 0 || strcmp(frame_id, "TDRC") == 0 || strcmp(frame_id, "TXXX") == 0 || strcmp(frame_id, "TCMP") == 0;

		bool lyrics_frame = lyrics_wanted && (strcmp(frame_id, "USLT") == 0 || strcmp(frame_id, "SYLT") == 0);
		bool comment_frame = description_wanted && strcmp(frame_id, "COMM") == 0;

		if (!wanted && !lyrics_frame && !comment_frame) {
			fseeko(f, next_frame, SEEK_SET);
			continue;
		}

		uint8_t *buf = malloc(frame_size);
		if (!buf)
			break;
		if (fread(buf, 1, frame_size, f) != frame_size) {
			free(buf);
			break;
		}

		if (comment_frame) {
			id3_description_comm(buf, frame_size);
			free(buf);
			continue;
		}

		if (lyrics_frame) {
			if (strcmp(frame_id, "USLT") == 0) {
				id3_lyrics_uslt(buf, frame_size);
			} else {
				id3_lyrics_sylt(buf, frame_size);
			}
			free(buf);
			continue;
		}

		uint8_t encoding = buf[0];

		// TXXX is a pair, not a value: an encoded description, a terminator,
		// then the text. It is where every tagger writes ReplayGain on an MP3,
		// and where Mp3tag and its kin write an audiobook's series.
		if (strcmp(frame_id, "TXXX") == 0) {
			txxx_series(out, encoding, buf + 1, frame_size - 1);
			// ReplayGain, and a compilation flag, in the single-byte encodings
			// only.
			if (encoding == 0 || encoding == 3) {
				const char *text = (const char *)buf + 1;
				size_t left = frame_size - 1;
				size_t desc_len = strnlen(text, left);
				if (desc_len < left) {
					char key[40];
					size_t k = desc_len < sizeof(key) - 1 ? desc_len : sizeof(key) - 1;
					for (size_t i = 0; i < k; i++) {
						key[i] = (char)toupper((unsigned char)text[i]);
					}
					key[k] = '\0';
					replaygain_field(out, key, text + desc_len + 1, left - desc_len - 1);
					// Where ffmpeg puts the compilation flag in an MP3.
					if (strcmp(key, "COMPILATION") == 0) {
						out->compilation = flag_is_set(text + desc_len + 1, left - desc_len - 1);
					}
				}
			}
			free(buf);
			continue;
		}

		// In ID3v2.4 a text frame may hold several strings, each ended by a
		// terminator: several artists, several genres.
		if (major_version >= 4 && (strcmp(frame_id, "TPE1") == 0 || strcmp(frame_id, "TCON") == 0)) {
			bool genre = frame_id[1] == 'C';
			char *dst = genre ? out->genre : out->artist;
			dst[0] = '\0';
			id3_values(encoding, buf + 1, frame_size - 1, dst, genre ? sizeof(out->genre) : sizeof(out->artist), genre);
			free(buf);
			continue;
		}

		char decoded[512];
		id3_decode_text(encoding, buf + 1, frame_size - 1, decoded, sizeof(decoded));
		free(buf);

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
		} else if (strcmp(frame_id, "MVNM") == 0) {
			if (!out->series[0])
				copy_bounded(out->series, sizeof(out->series), decoded);
		} else if (strcmp(frame_id, "MVIN") == 0) {
			if (!out->series_part[0])
				copy_bounded(out->series_part, sizeof(out->series_part), decoded);
		}
	}

	return true;
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
	if (fseek(f, -128, SEEK_END) != 0)
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

	if (out->genre[0] == '\0') {
		uint8_t genre_idx = tag[127];
		if (genre_idx < ID3V1_GENRE_COUNT) {
			snprintf(out->genre, sizeof(out->genre), "%s", id3v1_genres[genre_idx]);
		}
	}
}

static void read_mp3_metadata(const char *filepath, song_metadata_t *out) {
	FILE *f = fopen(filepath, "rb");
	if (!f)
		return;

	read_id3v2(f, out);
	read_id3v1(f, out);

	fclose(f);
}

// ---------------------------------------------------------------------------
// DSD
//
// Neither container invents a tag format of its own. A .dsf points at an
// ordinary ID3v2 tag with the eight bytes at offset 20 of its header -- the
// same tag an MP3 carries, so the same reader handles it -- and a .dff spells
// artist and title out in DIIN sub-chunks.
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

static void read_dff_metadata(FILE *f, song_metadata_t *out) {
	unsigned char head[16];
	if (fread(head, 1, 16, f) != 16 || memcmp(head, "FRM8", 4) != 0) {
		return;
	}

	// The declared lengths are 64-bit and come off a card, so they are kept
	// 64-bit here and checked against the real end of the file: truncated into
	// a 32-bit position they can turn negative and send a walk backwards.
	if (fseeko(f, 0, SEEK_END) != 0) {
		return;
	}
	off_t file_end = ftello(f);
	if (file_end < 16) {
		return;
	}

	// DIIN sits at the top level after the audio, so the walk is flat: step
	// over every chunk (each padded to an even length, which the size field
	// does not include) and descend only into DIIN itself.
	off_t pos = 16;
	for (int guard = 0; guard < 64; guard++) {
		unsigned char ch[12];
		if (fseeko(f, pos, SEEK_SET) != 0 || fread(ch, 1, 12, f) != 12) {
			return;
		}
		unsigned long long size = 0;
		for (int i = 0; i < 8; i++) {
			size = (size << 8) | ch[4 + i];
		}
		off_t body = pos + 12;
		if (size > (unsigned long long)(file_end - body)) {
			return;
		}

		if (memcmp(ch, "DIIN", 4) == 0) {
			off_t sub = body;
			off_t end = body + (off_t)size;
			for (int sub_guard = 0; sub_guard < 64 && sub + 12 <= end; sub_guard++) {
				unsigned char sh[12];
				if (fseeko(f, sub, SEEK_SET) != 0 || fread(sh, 1, 12, f) != 12) {
					return;
				}
				unsigned long long ssize = 0;
				for (int i = 0; i < 8; i++) {
					ssize = (ssize << 8) | sh[4 + i];
				}
				if (ssize > (unsigned long long)(end - (sub + 12))) {
					return;
				}
				bool artist = memcmp(sh, "DIAR", 4) == 0;
				bool title = memcmp(sh, "DITI", 4) == 0;
				if ((artist || title) && ssize >= 4 && ssize < 4096) {
					// Four bytes of length, then the text, not terminated.
					unsigned char len_be[4];
					if (fread(len_be, 1, 4, f) == 4) {
						unsigned long len = ((unsigned long)len_be[0] << 24) | ((unsigned long)len_be[1] << 16) |
											((unsigned long)len_be[2] << 8) | len_be[3];
						if (len > ssize - 4) {
							len = (unsigned long)ssize - 4;
						}
						char text[256] = {0};
						if (len >= sizeof(text)) {
							len = sizeof(text) - 1;
						}
						if (fread(text, 1, len, f) == len) {
							copy_bounded(artist ? out->artist : out->title,
										 artist ? sizeof(out->artist) : sizeof(out->title), text);
						}
					}
				}
				sub += 12 + (off_t)ssize + (off_t)(ssize & 1);
			}
			return;
		}

		pos = body + (off_t)size + (off_t)(size & 1);
	}
}

static void read_dsd_metadata(const char *filepath, song_metadata_t *out) {
	FILE *f = fopen(filepath, "rb");
	if (!f) {
		return;
	}
	if (has_extension(filepath, ".dsf")) {
		read_dsf_metadata(f, out);
	} else {
		read_dff_metadata(f, out);
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
// INFO text is taken as UTF-8 when it is well-formed UTF-8, and otherwise as
// the single-byte text an ID3v1 tag holds (Latin-1, or Windows-1251 -- see
// id3_decode_text).
//
// AIFF has no tags of its own that anybody writes, only the same ID3 chunk.
// ---------------------------------------------------------------------------

// Whether `len` bytes, up to the first NUL, are well-formed UTF-8. Plain ASCII
// is. A Latin-1 accent on its own is not: a high byte has to open a sequence of
// the right length, and in Latin-1 text the byte after it is a letter.
static bool text_is_utf8(const uint8_t *p, size_t len) {
	size_t i = 0;
	while (i < len && p[i]) {
		uint8_t c = p[i];
		size_t follow;
		if (c < 0x80) {
			follow = 0;
		} else if (c >= 0xC2 && c <= 0xDF) {
			follow = 1;
		} else if (c >= 0xE0 && c <= 0xEF) {
			follow = 2;
		} else if (c >= 0xF0 && c <= 0xF4) {
			follow = 3;
		} else {
			return false;
		}
		i++;
		for (size_t k = 0; k < follow; k++, i++) {
			if (i >= len || (p[i] & 0xC0) != 0x80) {
				return false;
			}
		}
	}
	return true;
}

// One INFO value into `out` as UTF-8. The value is usually NUL-terminated and
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

// The fields of one INFO list, from just after its "INFO" to `list_end`.
static void read_wav_info(FILE *f, off_t list_end, song_metadata_t *info) {
	struct {
		char id[4];
		uint32_t size;
	} sub;
	while (ftello(f) + (off_t)sizeof(sub) <= list_end && fread(&sub, 1, sizeof(sub), f) == sizeof(sub)) {
		char value[1024];
		uint32_t read_size = sub.size < sizeof(value) - 1 ? sub.size : sizeof(value) - 1;
		if (fread(value, 1, read_size, f) != read_size)
			break;
		value[read_size] = '\0';

		if (memcmp(sub.id, "INAM", 4) == 0) {
			info_text(value, read_size, info->title, sizeof(info->title));
		} else if (memcmp(sub.id, "IART", 4) == 0) {
			info_text(value, read_size, info->artist, sizeof(info->artist));
		} else if (memcmp(sub.id, "IPRD", 4) == 0) {
			info_text(value, read_size, info->album, sizeof(info->album));
		} else if (memcmp(sub.id, "IGNR", 4) == 0) {
			info_text(value, read_size, info->genre, sizeof(info->genre));
		} else if (memcmp(sub.id, "ICRD", 4) == 0) {
			info->year = (int)strtol(value, NULL, 10);
		} else if (memcmp(sub.id, "ITRK", 4) == 0 || memcmp(sub.id, "IPRT", 4) == 0) {
			// "3" or "3/12"; strtol stops at the slash.
			if (info->track_number == 0) {
				info->track_number = (int)strtol(value, NULL, 10);
			}
		}

		off_t sub_data_start = ftello(f) - (off_t)read_size;
		off_t next_sub_pos = sub_data_start + (off_t)sub.size + (sub.size & 1);
		// Same rule as the outer walk: a sub-chunk that does not move the
		// position forward would be read again forever.
		if (next_sub_pos <= sub_data_start - (off_t)sizeof(sub) || next_sub_pos > list_end) {
			break;
		}
		if (fseeko(f, next_sub_pos, SEEK_SET) != 0) {
			break;
		}
	}
}

// What the ID3 tag left empty, from INFO.
static void fill_from_info(song_metadata_t *out, const song_metadata_t *info) {
	if (!out->title[0])
		copy_bounded(out->title, sizeof(out->title), info->title);
	if (!out->artist[0])
		copy_bounded(out->artist, sizeof(out->artist), info->artist);
	if (!out->album[0])
		copy_bounded(out->album, sizeof(out->album), info->album);
	if (!out->genre[0])
		copy_bounded(out->genre, sizeof(out->genre), info->genre);
	if (out->year == 0)
		out->year = info->year;
	if (out->track_number == 0)
		out->track_number = info->track_number;
}

static void read_wav_metadata(const char *filepath, song_metadata_t *out) {
	FILE *f = fopen(filepath, "rb");
	if (!f)
		return;

	char riff_header[12];
	if (fread(riff_header, 1, 12, f) != 12 || memcmp(riff_header, "RIFF", 4) != 0 ||
		memcmp(riff_header + 8, "WAVE", 4) != 0) {
		fclose(f);
		return;
	}

	// The real end of the file. Chunk lengths are declared by the file and are
	// unsigned 32-bit, while `long` here is signed 32-bit: a length past two
	// gigabytes turns negative, so the next position lands before the current
	// one and the loop rereads the same bytes forever.
	if (fseeko(f, 0, SEEK_END) != 0) {
		fclose(f);
		return;
	}
	off_t file_end = ftello(f);
	if (fseeko(f, 12, SEEK_SET) != 0) {
		fclose(f);
		return;
	}

	struct {
		char id[4];
		uint32_t size;
	} chunk;

	song_metadata_t info;
	memset(&info, 0, sizeof(info));
	off_t id3_at = -1;

	while (fread(&chunk, 1, sizeof(chunk), f) == sizeof(chunk)) {
		off_t chunk_data_start = ftello(f);
		if (chunk_data_start < 0) {
			break;
		}

		// The declared length has to fit inside the file: if it does not, the
		// file is lying and there is nothing further on to read.
		if ((off_t)chunk_data_start + (off_t)chunk.size > file_end) {
			break;
		}

		if (memcmp(chunk.id, "LIST", 4) == 0 && chunk.size >= 4) {
			char list_type[4];
			if (fread(list_type, 1, 4, f) == 4 && memcmp(list_type, "INFO", 4) == 0) {
				read_wav_info(f, chunk_data_start + (off_t)chunk.size, &info);
			}
		} else if ((memcmp(chunk.id, "id3 ", 4) == 0 || memcmp(chunk.id, "ID3 ", 4) == 0) && id3_at < 0) {
			id3_at = chunk_data_start;
		}

		off_t next_chunk_pos = (off_t)chunk_data_start + (off_t)chunk.size + (chunk.size & 1);
		// Always forward: a chunk that does not advance the position is the one
		// way this loop can fail to end.
		if (next_chunk_pos <= (off_t)chunk_data_start - (off_t)sizeof(chunk) || next_chunk_pos > file_end) {
			break;
		}
		if (fseeko(f, next_chunk_pos, SEEK_SET) != 0) {
			break;
		}
	}

	if (id3_at >= 0 && fseeko(f, id3_at, SEEK_SET) == 0) {
		read_id3v2(f, out);
	}
	fill_from_info(out, &info);

	fclose(f);
}

static void read_aiff_metadata(const char *filepath, song_metadata_t *out) {
	FILE *f = fopen(filepath, "rb");
	if (!f)
		return;

	uint8_t form[12];
	if (fread(form, 1, sizeof(form), f) != sizeof(form) || memcmp(form, "FORM", 4) != 0 || fseeko(f, 0, SEEK_END) != 0) {
		fclose(f);
		return;
	}
	off_t file_end = ftello(f);

	// Sizes are big-endian here, and a chunk body is padded to an even length
	// the size does not count. The guard stops a damaged file walking for ever.
	off_t pos = 12;
	for (int guard = 0; guard < 64 && pos + 8 <= file_end; guard++) {
		uint8_t ch[8];
		if (fseeko(f, pos, SEEK_SET) != 0 || fread(ch, 1, sizeof(ch), f) != sizeof(ch)) {
			break;
		}
		uint32_t size = ((uint32_t)ch[4] << 24) | ((uint32_t)ch[5] << 16) | ((uint32_t)ch[6] << 8) | ch[7];
		if (memcmp(ch, "ID3 ", 4) == 0 || memcmp(ch, "id3 ", 4) == 0) {
			read_id3v2(f, out);
			break;
		}
		pos += 8 + (off_t)size + (off_t)(size & 1);
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
// tag after them, which fills whatever the APEv2 one left empty. Binary items,
// the cover among them, are skipped unread.
static void read_ape_metadata(const char *filepath, song_metadata_t *out) {
	apedec_tags(filepath, apev2_tag_cb, out);

	FILE *f = fopen(filepath, "rb");
	if (f) {
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

void metadata_read(const char *filepath, song_metadata_t *out) {
	memset(out, 0, sizeof(*out));

	if (read_cue_metadata(filepath, out)) {
		return;
	}

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
		read_ogg_metadata(filepath, out);
		break;
	case DECODE_FORMAT_AAC_MP4:
		read_mp4_metadata(filepath, out);
		break;
	case DECODE_FORMAT_DSD:
		read_dsd_metadata(filepath, out);
		break;
	case DECODE_FORMAT_OPUS:
		read_ogg_metadata(filepath, out);
		break;
	case DECODE_FORMAT_WAVPACK:
		read_ape_metadata(filepath, out);
		break;
	case DECODE_FORMAT_APE:
		read_ape_metadata(filepath, out);
		break;
	default:
		if (has_extension(filepath, ".wav")) {
			read_wav_metadata(filepath, out);
		} else if (has_extension(filepath, ".aif") || has_extension(filepath, ".aiff") || has_extension(filepath, ".aifc")) {
			read_aiff_metadata(filepath, out);
		}
		break;
	}

	read_sidecar_tags(filepath, out);

	out->has_tags = out->title[0] != '\0' || out->artist[0] != '\0' || out->album[0] != '\0' || out->genre[0] != '\0';
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
