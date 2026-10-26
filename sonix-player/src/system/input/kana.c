#include "kana.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
	const char *romaji;
	const char *kana;
} rule_t;

// Every romaji that makes kana, in the spellings an IME accepts. A rule that
// ends in a vowel is complete as soon as it matches: none of these is the
// start of a longer one. "n" and the doubled consonant are handled in code.
static const rule_t RULES[] = {
	{"a", "あ"},
	{"i", "い"},
	{"u", "う"},
	{"e", "え"},
	{"o", "お"},
	{"ka", "か"},
	{"ki", "き"},
	{"ku", "く"},
	{"ke", "け"},
	{"ko", "こ"},
	{"sa", "さ"},
	{"shi", "し"},
	{"si", "し"},
	{"su", "す"},
	{"se", "せ"},
	{"so", "そ"},
	{"ta", "た"},
	{"chi", "ち"},
	{"ti", "ち"},
	{"tsu", "つ"},
	{"tu", "つ"},
	{"te", "て"},
	{"to", "と"},
	{"na", "な"},
	{"ni", "に"},
	{"nu", "ぬ"},
	{"ne", "ね"},
	{"no", "の"},
	{"ha", "は"},
	{"hi", "ひ"},
	{"fu", "ふ"},
	{"hu", "ふ"},
	{"he", "へ"},
	{"ho", "ほ"},
	{"ma", "ま"},
	{"mi", "み"},
	{"mu", "む"},
	{"me", "め"},
	{"mo", "も"},
	{"ya", "や"},
	{"yu", "ゆ"},
	{"yo", "よ"},
	{"ra", "ら"},
	{"ri", "り"},
	{"ru", "る"},
	{"re", "れ"},
	{"ro", "ろ"},
	{"wa", "わ"},
	{"wo", "を"},
	{"ga", "が"},
	{"gi", "ぎ"},
	{"gu", "ぐ"},
	{"ge", "げ"},
	{"go", "ご"},
	{"za", "ざ"},
	{"ji", "じ"},
	{"zi", "じ"},
	{"zu", "ず"},
	{"ze", "ぜ"},
	{"zo", "ぞ"},
	{"da", "だ"},
	{"di", "ぢ"},
	{"du", "づ"},
	{"de", "で"},
	{"do", "ど"},
	{"ba", "ば"},
	{"bi", "び"},
	{"bu", "ぶ"},
	{"be", "べ"},
	{"bo", "ぼ"},
	{"pa", "ぱ"},
	{"pi", "ぴ"},
	{"pu", "ぷ"},
	{"pe", "ぺ"},
	{"po", "ぽ"},
	{"kya", "きゃ"},
	{"kyu", "きゅ"},
	{"kyo", "きょ"},
	{"gya", "ぎゃ"},
	{"gyu", "ぎゅ"},
	{"gyo", "ぎょ"},
	{"sha", "しゃ"},
	{"shu", "しゅ"},
	{"she", "しぇ"},
	{"sho", "しょ"},
	{"sya", "しゃ"},
	{"syu", "しゅ"},
	{"syo", "しょ"},
	{"ja", "じゃ"},
	{"ju", "じゅ"},
	{"je", "じぇ"},
	{"jo", "じょ"},
	{"jya", "じゃ"},
	{"jyu", "じゅ"},
	{"jyo", "じょ"},
	{"zya", "じゃ"},
	{"zyu", "じゅ"},
	{"zyo", "じょ"},
	{"cha", "ちゃ"},
	{"chu", "ちゅ"},
	{"che", "ちぇ"},
	{"cho", "ちょ"},
	{"tya", "ちゃ"},
	{"tyu", "ちゅ"},
	{"tyo", "ちょ"},
	{"dya", "ぢゃ"},
	{"dyu", "ぢゅ"},
	{"dyo", "ぢょ"},
	{"nya", "にゃ"},
	{"nyu", "にゅ"},
	{"nyo", "にょ"},
	{"hya", "ひゃ"},
	{"hyu", "ひゅ"},
	{"hyo", "ひょ"},
	{"bya", "びゃ"},
	{"byu", "びゅ"},
	{"byo", "びょ"},
	{"pya", "ぴゃ"},
	{"pyu", "ぴゅ"},
	{"pyo", "ぴょ"},
	{"mya", "みゃ"},
	{"myu", "みゅ"},
	{"myo", "みょ"},
	{"rya", "りゃ"},
	{"ryu", "りゅ"},
	{"ryo", "りょ"},
	{"fa", "ふぁ"},
	{"fi", "ふぃ"},
	{"fe", "ふぇ"},
	{"fo", "ふぉ"},
	{"va", "ゔぁ"},
	{"vi", "ゔぃ"},
	{"vu", "ゔ"},
	{"ve", "ゔぇ"},
	{"vo", "ゔぉ"},
	// Small kana, typed with x or l in front.
	{"xa", "ぁ"},
	{"xi", "ぃ"},
	{"xu", "ぅ"},
	{"xe", "ぇ"},
	{"xo", "ぉ"},
	{"la", "ぁ"},
	{"li", "ぃ"},
	{"lu", "ぅ"},
	{"le", "ぇ"},
	{"lo", "ぉ"},
	{"xya", "ゃ"},
	{"xyu", "ゅ"},
	{"xyo", "ょ"},
	{"lya", "ゃ"},
	{"lyu", "ゅ"},
	{"lyo", "ょ"},
	{"xtu", "っ"},
	{"xtsu", "っ"},
	{"ltu", "っ"},
	{"ltsu", "っ"},
	{"xwa", "ゎ"},
	{"lwa", "ゎ"},
};

#define RULE_COUNT (sizeof(RULES) / sizeof(RULES[0]))

static bool is_vowel(char c) { return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o'; }

static const char *exact(const char *p) {
	for (size_t i = 0; i < RULE_COUNT; i++) {
		if (strcmp(RULES[i].romaji, p) == 0) {
			return RULES[i].kana;
		}
	}
	return NULL;
}

// True when `p` is the start of a longer rule.
static bool has_longer(const char *p) {
	size_t len = strlen(p);
	for (size_t i = 0; i < RULE_COUNT; i++) {
		if (strlen(RULES[i].romaji) > len && strncmp(RULES[i].romaji, p, len) == 0) {
			return true;
		}
	}
	return false;
}

static size_t put(char *out, size_t cap, size_t at, const char *text) {
	size_t len = strlen(text);
	if (at + len < cap) {
		memcpy(out + at, text, len + 1);
		return at + len;
	}
	return at;
}

static void drop_first(kana_t *k, size_t count) {
	size_t len = strlen(k->pending);
	if (count >= len) {
		k->pending[0] = '\0';
		return;
	}
	memmove(k->pending, k->pending + count, len - count + 1);
}

void kana_reset(kana_t *k) { k->pending[0] = '\0'; }

const char *kana_pending(const kana_t *k) { return k->pending; }

bool kana_backspace(kana_t *k) {
	size_t len = strlen(k->pending);
	if (len == 0) {
		return false;
	}
	k->pending[len - 1] = '\0';
	return true;
}

// Turn as much of the waiting letters into kana as can be told for certain.
static size_t resolve(kana_t *k, char *out, size_t cap, size_t at) {
	for (;;) {
		char *p = k->pending;
		size_t len = strlen(p);
		if (len == 0) {
			return at;
		}

		if (p[0] == 'n' && len >= 2) {
			// "nn" is the kana n only when no vowel or y follows: in "konnichi" the
			// second n opens the "ni", and "kannji" has one kana n for both.
			if (p[1] == 'n') {
				if (len == 2) {
					return at; // wait for the next letter
				}
				at = put(out, cap, at, "ん");
				drop_first(k, is_vowel(p[2]) || p[2] == 'y' ? 1 : 2);
				continue;
			}
			// n before anything that cannot follow it in a kana: the kana n.
			if (!is_vowel(p[1]) && p[1] != 'y') {
				at = put(out, cap, at, "ん");
				drop_first(k, 1);
				continue;
			}
		}

		const char *kana = exact(p);
		if (kana) {
			at = put(out, cap, at, kana);
			p[0] = '\0';
			return at;
		}
		if (has_longer(p)) {
			return at;
		}

		// A dead end. A doubled consonant is a small tsu in front of the
		// second ("kitte"), and "tch" does the same ("matcha").
		if (len >= 2 && ((p[0] == p[1] && !is_vowel(p[0]) && p[0] != 'n') || (p[0] == 't' && p[1] == 'c'))) {
			at = put(out, cap, at, "っ");
			drop_first(k, 1);
			continue;
		}

		// Not romaji at all ("qz"): the first letter goes through as typed.
		char one[2] = {p[0], '\0'};
		at = put(out, cap, at, one);
		drop_first(k, 1);
	}
}

size_t kana_flush(kana_t *k, char *out, size_t cap) {
	size_t at = 0;
	if (cap) {
		out[0] = '\0';
	}
	if (strcmp(k->pending, "n") == 0 || strcmp(k->pending, "nn") == 0) {
		at = put(out, cap, at, "ん");
	} else {
		at = put(out, cap, at, k->pending);
	}
	k->pending[0] = '\0';
	return at;
}

size_t kana_feed(kana_t *k, int c, char *out, size_t cap) {
	size_t at = 0;
	if (cap) {
		out[0] = '\0';
	}

	if (c >= 0 && c < 0x80 && isalpha(c)) {
		size_t len = strlen(k->pending);
		if (len >= KANA_PENDING_MAX) { // cannot happen: resolve() keeps it short
			kana_reset(k);
			len = 0;
		}
		k->pending[len] = (char)tolower(c);
		k->pending[len + 1] = '\0';
		return resolve(k, out, cap, at);
	}

	if (c == '\'' && strcmp(k->pending, "n") == 0) { // n' is the kana n, so "hon'ya" is hon + ya
		k->pending[0] = '\0';
		return put(out, cap, at, "ん");
	}

	at = kana_flush(k, out, cap);
	if (c == '-') {
		return put(out, cap, at, "ー");
	}
	if (c > 0 && c < 0x80) {
		char one[2] = {(char)c, '\0'};
		at = put(out, cap, at, one);
	}
	return at;
}

void kana_to_katakana(const char *in, char *out, size_t cap) {
	size_t at = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p && at + 4 < cap;) {
		if (p[0] == 0xE3 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
			uint32_t cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (uint32_t)(p[2] & 0x3F);
			if (cp >= 0x3041 && cp <= 0x3096) {
				cp += 0x60;
			}
			out[at++] = (char)(0xE0 | (cp >> 12));
			out[at++] = (char)(0x80 | ((cp >> 6) & 0x3F));
			out[at++] = (char)(0x80 | (cp & 0x3F));
			p += 3;
		} else {
			out[at++] = (char)*p++;
		}
	}
	if (cap) {
		out[at < cap ? at : cap - 1] = '\0';
	}
}

// ---------------------------------------------------------------------------
// Typing into a text field
// ---------------------------------------------------------------------------

static void write_out(const char *kana, bool katakana, const kana_sink_t *sink, void *ctx) {
	if (!kana[0]) {
		return;
	}
	if (katakana) {
		char converted[48];
		kana_to_katakana(kana, converted, sizeof(converted));
		sink->insert(ctx, converted);
	} else {
		sink->insert(ctx, kana);
	}
}

void kana_edit_key(kana_t *k, bool katakana, int c, const kana_sink_t *sink, void *ctx) {
	int waiting = (int)strlen(k->pending); // ASCII, one character each
	if (waiting) {
		sink->erase(ctx, waiting);
	}
	char out[48];
	kana_feed(k, c, out, sizeof(out));
	write_out(out, katakana, sink, ctx);
	if (k->pending[0]) {
		sink->insert(ctx, k->pending);
	}
}

void kana_edit_flush(kana_t *k, bool katakana, const kana_sink_t *sink, void *ctx) {
	int waiting = (int)strlen(k->pending);
	if (!waiting) {
		return;
	}
	sink->erase(ctx, waiting);
	char out[48];
	kana_flush(k, out, sizeof(out));
	write_out(out, katakana, sink, ctx);
}

bool kana_edit_backspace(kana_t *k, const kana_sink_t *sink, void *ctx) {
	if (!kana_backspace(k)) {
		return false;
	}
	sink->erase(ctx, 1);
	return true;
}

// ---------------------------------------------------------------------------
// The dakuten key
// ---------------------------------------------------------------------------

static int utf8_step(const char *p) {
	unsigned char c = (unsigned char)*p;
	if (c < 0x80) {
		return 1;
	}
	if ((c & 0xE0) == 0xC0) {
		return 2;
	}
	if ((c & 0xF0) == 0xE0) {
		return 3;
	}
	return 4;
}

bool kana_prev_char(const char *text, unsigned cursor, char *out, size_t cap) {
	if (!text || cursor == 0) {
		return false;
	}
	size_t at = 0, start = 0;
	unsigned seen = 0;
	while (text[at] && seen < cursor) {
		start = at;
		int step = utf8_step(text + at);
		for (int i = 1; i < step; i++) { // a truncated sequence must not run past the NUL
			if (!text[at + (size_t)i]) {
				return false;
			}
		}
		at += (size_t)step;
		seen++;
	}
	size_t len = at - start;
	if (seen != cursor || len == 0 || len + 1 > cap) {
		return false;
	}
	memcpy(out, text + start, len);
	out[len] = '\0';
	return true;
}

// Each string is one cycle of hiragana, the plain letter first.
static const char *const MARK_CYCLES[] = {
	"かが", "きぎ", "くぐ", "けげ", "こご", "さざ", "しじ", "すず", "せぜ", "そぞ", "ただ", "ちぢ", "つっづ", "てで", "とど", "はばぱ", "ひびぴ", "ふぶぷ", "へべぺ", "ほぼぽ", "あぁ", "いぃ", "うぅゔ", "えぇ", "おぉ", "やゃ", "ゆゅ", "よょ", "わゎ",
};

static uint32_t decode3(const char *s) {
	const unsigned char *p = (const unsigned char *)s;
	return ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (uint32_t)(p[2] & 0x3F);
}

static void encode3(uint32_t cp, char *out) {
	out[0] = (char)(0xE0 | (cp >> 12));
	out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[2] = (char)(0x80 | (cp & 0x3F));
}

bool kana_cycle_mark(const char *prev, char *out, size_t cap) {
	if (!prev || strlen(prev) != 3 || (unsigned char)prev[0] != 0xE3 || cap < 4) {
		return false;
	}
	uint32_t cp = decode3(prev);
	bool katakana = cp >= 0x30A1 && cp <= 0x30F6;
	if (katakana) {
		cp -= 0x60; // looked up as hiragana, answered in katakana
	}
	char key[4];
	encode3(cp, key);
	key[3] = '\0';

	for (size_t i = 0; i < sizeof(MARK_CYCLES) / sizeof(MARK_CYCLES[0]); i++) {
		const char *cycle = MARK_CYCLES[i];
		size_t len = strlen(cycle);
		for (size_t at = 0; at + 3 <= len; at += 3) {
			if (memcmp(cycle + at, key, 3) != 0) {
				continue;
			}
			size_t next = at + 3 < len ? at + 3 : 0;
			uint32_t answer = decode3(cycle + next);
			if (katakana) {
				answer += 0x60;
			}
			encode3(answer, out);
			out[3] = '\0';
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// The ten-key kana keypad
// ---------------------------------------------------------------------------

// Hiragana; katakana is the same kana 0x60 higher. The long-vowel mark is on the wa key and the
// rest of the punctuation on the first, as on a Japanese phone.
static const char *const KEY_ROWS[KANA_KEY_COUNT] = {
	"あいうえお", "かきくけこ", "さしすせそ", "たちつてと", "なにぬねの", "はひふへほ", "まみむめも", "やゆよ", "らりるれろ", "、。?!・", "", "わをんー",
};

int kana_key_len(int key) {
	if (key < 0 || key >= KANA_KEY_COUNT) {
		return 0;
	}
	int count = 0;
	for (const char *p = KEY_ROWS[key]; *p; p += utf8_step(p)) {
		count++;
	}
	return count;
}

static void copy_char(const char *row, int index, char *out) {
	const char *p = row;
	for (int i = 0; i < index && *p; i++) {
		p += utf8_step(p);
	}
	int len = *p ? utf8_step(p) : 0;
	memcpy(out, p, (size_t)len);
	out[len] = '\0';
}

const char *kana_key_step(int key, int tap, bool katakana) {
	static char step[8];
	step[0] = '\0';
	if (tap < 0 || tap >= kana_key_len(key)) {
		return step;
	}
	copy_char(KEY_ROWS[key], tap, step);
	if (katakana) {
		char converted[8];
		kana_to_katakana(step, converted, sizeof(converted));
		memcpy(step, converted, sizeof(converted));
	}
	return step;
}

const char *kana_key_cap(int key, bool katakana) {
	static char cap[16];
	cap[0] = '\0';
	if (key == 9) { // the punctuation key shows its first two marks
		snprintf(cap, sizeof(cap), "%s", KEY_ROWS[9]);
		char *cut = cap;
		for (int i = 0; i < 2 && *cut; i++) {
			cut += utf8_step(cut);
		}
		*cut = '\0';
		return cap;
	}
	snprintf(cap, sizeof(cap), "%s", kana_key_step(key, 0, katakana));
	return cap;
}

// ---------------------------------------------------------------------------
// Flick on the kana pad
// ---------------------------------------------------------------------------

kana_dir_t kana_flick_dir(int dx, int dy, int threshold) {
	int ax = dx < 0 ? -dx : dx;
	int ay = dy < 0 ? -dy : dy;
	if ((ax == 0 && ay == 0) || (ax < threshold && ay < threshold)) {
		return KANA_DIR_CENTER;
	}
	if (ax > ay) {
		return dx < 0 ? KANA_DIR_LEFT : KANA_DIR_RIGHT;
	}
	return dy < 0 ? KANA_DIR_UP : KANA_DIR_DOWN;
}

const char *kana_flick_step(int key, kana_dir_t dir, bool katakana) {
	// The ya row has three kana, which take the vertical directions.
	static const signed char YA_ROW[5] = {0, -1, 1, -1, 2};
	if ((unsigned)dir > (unsigned)KANA_DIR_DOWN) {
		return "";
	}
	int tap = key == 7 ? YA_ROW[dir] : (int)dir;
	return kana_key_step(key, tap, katakana); // "" for a step the row does not have
}
