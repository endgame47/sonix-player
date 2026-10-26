// The lookup of hiragana readings in jpdict_data.c.
//
// The way a reading is matched (a key exactly, or a stem and one kana of its
// okurigana) follows lv_ime_jp_lite.c (https://github.com/fukuen/lvgl, the commit
// is in tools/gen_jpdict.py), Copyright (c) 2026 fukuen, under the GNU General Public
// License version 3, as are the dictionary and its notices in jpdict_data.c.

#include "jpdict.h"

#include "jpdict_data.h"

#include <stdint.h>
#include <string.h>

// The kana an okurigana consonant stands for, indexed by the letter: the key
// "<kana>k" is that kana followed by one of the k row. Same table as lv_ime_jp_skk_exp
// in lv_ime_jp_lite.c.
static const char *const OKURI[26] = {
	"あ", "ばびぶべぼ", "", "だぢづでど", "え", "", "がぎぐげご", "はひふへほ", "い", "", "かきくけこ", "", "まみむめも", "なにぬねの", "お", "ぱぴぷぺぽ", "", "らりるれろ", "さしすせそ", "たちつてとっ", "う", "", "わ", "", "やゆよ", "ざじずぜぞ",
};

// The code point of the three-byte character at `s`, or 0 when it is not one.
static unsigned head_cp(const char *s) {
	const unsigned char *p = (const unsigned char *)s;
	if ((p[0] & 0xF0) != 0xE0 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) {
		return 0;
	}
	return ((unsigned)(p[0] & 0x0F) << 12) | ((unsigned)(p[1] & 0x3F) << 6) | (unsigned)(p[2] & 0x3F);
}

// The run of entries that start with this kana, found by bisection.
static const jpdict_head_t *find_head(unsigned cp) {
	int lo = 0, hi = JPDICT_HEAD_COUNT - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (JPDICT_HEADS[mid].cp == cp) {
			return &JPDICT_HEADS[mid];
		}
		if (JPDICT_HEADS[mid].cp < cp) {
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	return NULL;
}

// True when the three bytes at `tail` are one of the kana in `set`.
static bool in_set(const char *tail, const char *set) {
	for (; *set; set += 3) {
		if (memcmp(tail, set, 3) == 0) {
			return true;
		}
	}
	return false;
}

static int count_cands(const char *cands) {
	int count = 1;
	for (; *cands; cands++) {
		if (*cands == '/') {
			count++;
		}
	}
	return count;
}

bool jpdict_lookup(const char *reading, jpdict_hit_t *hit) {
	if (!reading || !hit) {
		return false;
	}
	size_t len = strlen(reading);
	if (len < 3) {
		return false;
	}
	unsigned cp = head_cp(reading);
	const jpdict_head_t *head = cp ? find_head(cp) : NULL;
	if (!head) {
		return false;
	}

	hit->stem = NULL;
	hit->exact = NULL;
	hit->tail[0] = '\0';
	hit->stem_count = 0;
	hit->count = 0;

	// From the last entry back, where two entries of one kind match, the later
	// one is the one that is answered.
	for (int i = head->first + head->count - 1; i >= head->first; i--) {
		const char *key = JPDICT_KEYS + JPDICT_ENTRIES[i].key;
		size_t klen = strlen(key);

		if (len == klen) {
			if (!hit->exact && memcmp(reading, key, len) == 0) {
				hit->exact = JPDICT_VALS + JPDICT_ENTRIES[i].val;
			}
		} else if (!hit->stem && len == klen + 2 && key[klen - 1] >= 'a' && key[klen - 1] <= 'z') {
			// A stem and one kana: the key's last letter is a consonant (3 bytes of
			// kana in the reading take the place of its 1 byte).
			if (memcmp(reading, key, klen - 1) == 0 && in_set(reading + klen - 1, OKURI[key[klen - 1] - 'a'])) {
				hit->stem = JPDICT_VALS + JPDICT_ENTRIES[i].val;
				memcpy(hit->okuri, reading + klen - 1, 3);
				hit->okuri[3] = '\0';
			}
		}
	}
	if (!hit->stem && !hit->exact) {
		return false;
	}
	hit->stem_count = hit->stem ? count_cands(hit->stem) : 0;
	hit->count = hit->stem_count + (hit->exact ? count_cands(hit->exact) : 0);
	return true;
}

size_t jpdict_candidate(const jpdict_hit_t *hit, int index, char *out, size_t cap) {
	if (cap) {
		out[0] = '\0';
	}
	if (!hit || index < 0 || index >= hit->count || !cap) {
		return 0;
	}
	bool from_stem = index < hit->stem_count;
	const char *p = from_stem ? hit->stem : hit->exact;
	for (int i = from_stem ? index : index - hit->stem_count; i > 0; i--) {
		p = strchr(p, '/') + 1; // the index is below the count, so there is a "/" to find
	}
	const char *end = strchr(p, '/');
	size_t len = end ? (size_t)(end - p) : strlen(p);
	size_t okuri = from_stem ? strlen(hit->okuri) : 0; // the kana after the stem is put back
	size_t tail = from_stem ? strlen(hit->tail) : 0;   // and the ending that follows it
	if (len + okuri + tail + 1 > cap) {
		return 0;
	}
	memcpy(out, p, len);
	memcpy(out + len, hit->okuri, okuri);
	memcpy(out + len + okuri, hit->tail, tail);
	out[len + okuri + tail] = '\0';
	return len + okuri + tail;
}

// True for the 3-byte hiragana at `s` (U+3041 .. U+3096), the letters a reading
// is made of.
static bool is_hiragana(const char *s) {
	unsigned cp = head_cp(s);
	return cp >= 0x3041 && cp <= 0x3096;
}

bool jpdict_lookup_before(const char *text, unsigned cursor, unsigned max_chars, jpdict_hit_t *hit, char *reading, size_t cap, unsigned *chars) {
	if (!text || !hit || !reading || !chars || max_chars == 0 || cap < 3 * (size_t)max_chars + 1) {
		return false;
	}

	// The byte at which the caret stands, found by stepping over `cursor`
	// characters. A cut-off sequence ends the walk rather than running on.
	size_t at = 0;
	for (unsigned seen = 0; seen < cursor; seen++) {
		unsigned char c = (unsigned char)text[at];
		if (!c) {
			return false; // the caret is past the end
		}
		size_t step = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
		for (size_t i = 1; i < step; i++) {
			if (!text[at + i]) {
				return false;
			}
		}
		at += step;
	}

	// The run of hiragana before it, three bytes a character, newest last.
	unsigned run = 0;
	while (run < max_chars && at >= 3 * (size_t)(run + 1) && is_hiragana(text + at - 3 * (run + 1))) {
		run++;
	}

	// The longest piece of it that is in the dictionary: the whole run first,
	// then without its first character, and so on down to the last one.
	for (unsigned n = run; n >= 1; n--) {
		memcpy(reading, text + at - 3 * (size_t)n, 3 * (size_t)n);
		reading[3 * (size_t)n] = '\0';
		if (jpdict_lookup(reading, hit)) {
			*chars = n;
			return true;
		}

		// A verb or adjective with an ending: the stem and its okurigana first,
		// the ending left as typed.
		for (unsigned t = 1; t <= JPDICT_TAIL_MAX && t < n; t++) {
			size_t cut = 3 * (size_t)(n - t);
			char saved = reading[cut];
			reading[cut] = '\0';
			bool found = jpdict_lookup(reading, hit) && hit->stem_count > 0;
			reading[cut] = saved;
			if (found) {
				hit->exact = NULL; // a noun with kana after it is not a word
				hit->count = hit->stem_count;
				memcpy(hit->tail, reading + cut, 3 * (size_t)t);
				hit->tail[3 * (size_t)t] = '\0';
				*chars = n;
				return true;
			}
		}
	}
	return false;
}
