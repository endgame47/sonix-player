#include "kblayout.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "src/system/core/config.h"
#include "src/system/core/lang.h"

// One alphabet. The rows are written out in both cases rather than folded from
// one to the other: the letters are UTF-8 and the mapping is not arithmetic --
// Cyrillic sits a whole block away from its capitals, and a rule that works for
// ASCII would silently mangle everything else. Two strings per row and the
// question does not arise.
//
// The strings are split into letters at the code point, so every letter of a
// row must be one code point, which all of these are.
typedef struct {
	const char *name;
	const char *lower[KB_LAYOUT_ROWS];
	const char *upper[KB_LAYOUT_ROWS];
	// The same alphabet shared over the nine keys of a phone keypad. Key 0 is
	// the punctuation one and is the same everywhere.
	const char *t9_lower[KB_LAYOUT_T9_KEYS];
	const char *t9_upper[KB_LAYOUT_T9_KEYS];
	const char *language; // the interface language whose default this is
} layout_info_t;

// The plain Latin keypad, for the layouts whose keypad adds no letters: T9
// groups letters by alphabet, and AZERTY rearranges keys rather than letters.
#define T9_LATIN_LOWER ".,?!\'", "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz"
#define T9_LATIN_UPPER ".,?!\'", "ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ"

static const layout_info_t layouts[KB_LAYOUT_COUNT] = {
	[KB_LAYOUT_ENGLISH] = {"English",
						   {"qwertyuiop", "asdfghjkl", "zxcvbnm"},
						   {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"},
						   {T9_LATIN_LOWER},
						   {T9_LATIN_UPPER},
						   "English"},
	// The same letters as English, which is what an Italian phone keyboard
	// has: the accented vowels are not on the letter rows there either.
	[KB_LAYOUT_ITALIAN] = {"Italiano",
						   {"qwertyuiop", "asdfghjkl", "zxcvbnm"},
						   {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"},
						   {T9_LATIN_LOWER},
						   {T9_LATIN_UPPER},
						   "Italiano"},
	// QWERTZ, with the umlauts on the rows where a German keyboard has them and
	// on the keys of the letters they are: a phone keypad shares them out that
	// way rather than giving them keys of their own.
	[KB_LAYOUT_GERMAN] = {"Deutsch",
						  {"qwertzuiop\xc3\xbc", "asdfghjkl\xc3\xb6\xc3\xa4", "yxcvbnm"},
						  {"QWERTZUIOP\xc3\x9c", "ASDFGHJKL\xc3\x96\xc3\x84", "YXCVBNM"},
						  {".,?!\'", "abc\xc3\xa4", "def", "ghi", "jkl", "mno\xc3\xb6", "pqrs\xc3\x9f", "tuv\xc3\xbc",
						   "wxyz"},
						  {".,?!\'", "ABC\xc3\x84", "DEF", "GHI", "JKL", "MNO\xc3\x96", "PQRS\xc3\x9f", "TUV\xc3\x9c",
						   "WXYZ"},
						  "Deutsch"},
	[KB_LAYOUT_SPANISH] = {"Espa\xc3\xb1ol",
						   {"qwertyuiop", "asdfghjkl\xc3\xb1", "zxcvbnm"},
						   {"QWERTYUIOP", "ASDFGHJKL\xc3\x91", "ZXCVBNM"},
						   // The enye rides the key its neighbours are on.
						   {".,?!\'", "abc", "def", "ghi", "jkl", "mno\xc3\xb1", "pqrs", "tuv", "wxyz"},
						   {".,?!\'", "ABC", "DEF", "GHI", "JKL", "MNO\xc3\x91", "PQRS", "TUV", "WXYZ"},
						   "Espa\xc3\xb1ol"},
	[KB_LAYOUT_FRENCH] = {"Fran\xc3\xa7\x61is",
						  {"azertyuiop", "qsdfghjklm", "wxcvbn"},
						  {"AZERTYUIOP", "QSDFGHJKLM", "WXCVBN"},
						  {T9_LATIN_LOWER},
						  {T9_LATIN_UPPER},
						  "Fran\xc3\xa7\x61is"},
	[KB_LAYOUT_RUSSIAN] = {"\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9",
						   {"\xd0\xb9\xd1\x86\xd1\x83\xd0\xba\xd0\xb5\xd0\xbd\xd0\xb3\xd1\x88\xd1\x89\xd0\xb7"
							"\xd1\x85\xd1\x8a",
							"\xd1\x84\xd1\x8b\xd0\xb2\xd0\xb0\xd0\xbf\xd1\x80\xd0\xbe\xd0\xbb\xd0\xb4\xd0\xb6"
							"\xd1\x8d",
							"\xd1\x8f\xd1\x87\xd1\x81\xd0\xbc\xd0\xb8\xd1\x82\xd1\x8c\xd0\xb1\xd1\x8e"},
						   {"\xd0\x99\xd0\xa6\xd0\xa3\xd0\x9a\xd0\x95\xd0\x9d\xd0\x93\xd0\xa8\xd0\xa9\xd0\x97"
							"\xd0\xa5\xd0\xaa",
							"\xd0\xa4\xd0\xab\xd0\x92\xd0\x90\xd0\x9f\xd0\xa0\xd0\x9e\xd0\x9b\xd0\x94\xd0\x96"
							"\xd0\xad",
							"\xd0\xaf\xd0\xa7\xd0\xa1\xd0\x9c\xd0\x98\xd0\xa2\xd0\xac\xd0\x91\xd0\xae"},
						   // The Cyrillic keypad: thirty-two letters four to a key,
						   // in alphabetical order, which is how a Russian phone
						   // shares them out.
						   {".,?!\'",
							"\xd0\xb0\xd0\xb1\xd0\xb2\xd0\xb3", "\xd0\xb4\xd0\xb5\xd0\xb6\xd0\xb7",
							"\xd0\xb8\xd0\xb9\xd0\xba\xd0\xbb", "\xd0\xbc\xd0\xbd\xd0\xbe\xd0\xbf",
							"\xd1\x80\xd1\x81\xd1\x82\xd1\x83", "\xd1\x84\xd1\x85\xd1\x86\xd1\x87",
							"\xd1\x88\xd1\x89\xd1\x8a\xd1\x8b", "\xd1\x8c\xd1\x8d\xd1\x8e\xd1\x8f"},
						   {".,?!\'",
							"\xd0\x90\xd0\x91\xd0\x92\xd0\x93", "\xd0\x94\xd0\x95\xd0\x96\xd0\x97",
							"\xd0\x98\xd0\x99\xd0\x9a\xd0\x9b", "\xd0\x9c\xd0\x9d\xd0\x9e\xd0\x9f",
							"\xd0\xa0\xd0\xa1\xd0\xa2\xd0\xa3", "\xd0\xa4\xd0\xa5\xd0\xa6\xd0\xa7",
							"\xd0\xa8\xd0\xa9\xd0\xaa\xd0\xab", "\xd0\xac\xd0\xad\xd0\xae\xd0\xaf"},
						   "Russian"},
	[KB_LAYOUT_DUTCH] = {"Nederlands",
						 {"qwertyuiop", "asdfghjkl", "zxcvbnm"},
						 {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"},
						 {T9_LATIN_LOWER},
						 {T9_LATIN_UPPER},
						 "Nederlands"},
	// The letter rows of a Polish keyboard, which is QWERTY with the
	// diacritics behind AltGr; here they come from holding the base letter. A
	// Polish phone keypad puts each of them on the key of its base letter.
	[KB_LAYOUT_POLISH] = {"Polski",
						  {"qwertyuiop", "asdfghjkl", "zxcvbnm"},
						  {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"},
						  {".,?!\'", "abc\xc4\x85\xc4\x87", "def\xc4\x99", "ghi", "jkl\xc5\x82", "mno\xc5\x84\xc3\xb3",
						   "pqrs\xc5\x9b", "tuv", "wxyz\xc5\xba\xc5\xbc"},
						  {".,?!\'", "ABC\xc4\x84\xc4\x86", "DEF\xc4\x98", "GHI", "JKL\xc5\x81", "MNO\xc5\x83\xc3\x93",
						   "PQRS\xc5\x9a", "TUV", "WXYZ\xc5\xb9\xc5\xbb"},
						  "Polski"},
	// ABNT2, the Brazilian keyboard: the c-cedilla has its own key after L, and
	// the accented vowels, dead keys there, come from holding the vowel.
	[KB_LAYOUT_PORTUGUESE] = {"Portugu\xc3\xaas",
							  {"qwertyuiop", "asdfghjkl\xc3\xa7", "zxcvbnm"},
							  {"QWERTYUIOP", "ASDFGHJKL\xc3\x87", "ZXCVBNM"},
							  {".,?!\'", "abc\xc3\xa7", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz"},
							  {".,?!\'", "ABC\xc3\x87", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ"},
							  "Portugu\xc3\xaas BR"},
	// QWERTZ, as Czech keyboards are. The diacritics come from holding the base
	// letter, and a Czech phone keypad puts each of them on its base letter's
	// key.
	[KB_LAYOUT_CZECH] = {"\xc4\x8c"
						 "e\xc5\xa1tina",
						 {"qwertzuiop", "asdfghjkl", "yxcvbnm"},
						 {"QWERTZUIOP", "ASDFGHJKL", "YXCVBNM"},
						 {".,?!\'", "abc\xc3\xa1\xc4\x8d", "def\xc4\x8f\xc3\xa9\xc4\x9b", "ghi\xc3\xad", "jkl",
						  "mno\xc5\x88\xc3\xb3", "pqrs\xc5\x99\xc5\xa1", "tuv\xc5\xa5\xc3\xba\xc5\xaf",
						  "wxyz\xc3\xbd\xc5\xbe"},
						 {".,?!\'", "ABC\xc3\x81\xc4\x8c", "DEF\xc4\x8e\xc3\x89\xc4\x9a", "GHI\xc3\x8d", "JKL",
						  "MNO\xc5\x87\xc3\x93", "PQRS\xc5\x98\xc5\xa0", "TUV\xc5\xa4\xc3\x9a\xc5\xae",
						  "WXYZ\xc3\x9d\xc5\xbd"},
						 "\xc4\x8c"
						 "e\xc5\xa1tina"},
	// Japanese has no letters of its own here: the kana are typed through the
	// Latin rows as romaji, and through the ten-key keypad of kana.c. Both are
	// switched on by the layout in keyboard.c, and what is below is what they
	// fall back to: the Latin letters, for the mode that types them as they are
	// (the "A" of the key beside "123"). The "-" at the end of the third row is
	// the long-vowel mark while romaji is being turned into kana ("ra-men" is the
	// word for ramen), a hyphen in Latin mode; with shift it is "_". The name is the
	// layout's name in its own language.
	[KB_LAYOUT_JAPANESE] = {"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e",
						   {"qwertyuiop", "asdfghjkl", "zxcvbnm-"},
						   {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM_"},
						   {T9_LATIN_LOWER},
						   {T9_LATIN_UPPER},
						   "Japanese"},
};

// The letters a held key offers, the way a phone keyboard does: one entry per
// key cap, upper and lower case written out separately for the same reason the
// rows are. The same list for every Latin layout, so a word in another
// language can be typed on any of them.
static const struct {
	const char *key;
	const char *variants;
} VARIANTS[] = {
	{"a", "\xc3\xa0\xc3\xa1\xc3\xa2\xc3\xa4\xc3\xa6\xc3\xa3\xc3\xa5\xc4\x81\xc4\x85"}, // à á â ä æ ã å ā ą
	{"A", "\xc3\x80\xc3\x81\xc3\x82\xc3\x84\xc3\x86\xc3\x83\xc3\x85\xc4\x80\xc4\x84"}, // À Á Â Ä Æ Ã Å Ā Ą
	{"c", "\xc3\xa7\xc4\x87\xc4\x8d"},												   // ç ć č
	{"C", "\xc3\x87\xc4\x86\xc4\x8c"},												   // Ç Ć Č
	{"d", "\xc4\x8f"},																   // ď
	{"D", "\xc4\x8e"},																   // Ď
	{"e", "\xc3\xa8\xc3\xa9\xc3\xaa\xc3\xab\xc4\x93\xc4\x97\xc4\x99\xc4\x9b"},		   // è é ê ë ē ė ę ě
	{"E", "\xc3\x88\xc3\x89\xc3\x8a\xc3\x8b\xc4\x92\xc4\x96\xc4\x98\xc4\x9a"},		   // È É Ê Ë Ē Ė Ę Ě
	{"i", "\xc3\xac\xc3\xad\xc3\xae\xc3\xaf\xc4\xab\xc4\xaf"},						   // ì í î ï ī į
	{"I", "\xc3\x8c\xc3\x8d\xc3\x8e\xc3\x8f\xc4\xaa\xc4\xae"},						   // Ì Í Î Ï Ī Į
	{"l", "\xc5\x82"},																   // ł
	{"L", "\xc5\x81"},																   // Ł
	{"n", "\xc3\xb1\xc5\x84\xc5\x88"},												   // ñ ń ň
	{"N", "\xc3\x91\xc5\x83\xc5\x87"},												   // Ñ Ń Ň
	{"o", "\xc3\xb2\xc3\xb3\xc3\xb4\xc3\xb6\xc3\xb5\xc3\xb8\xc5\x8d\xc5\x93"},		   // ò ó ô ö õ ø ō œ
	{"O", "\xc3\x92\xc3\x93\xc3\x94\xc3\x96\xc3\x95\xc3\x98\xc5\x8c\xc5\x92"},		   // Ò Ó Ô Ö Õ Ø Ō Œ
	{"r", "\xc5\x99"},																   // ř
	{"R", "\xc5\x98"},																   // Ř
	{"s", "\xc3\x9f\xc5\x9b\xc5\xa1"},												   // ß ś š
	{"S", "\xc5\x9a\xc5\xa0"},														   // Ś Š
	{"t", "\xc5\xa5"},																   // ť
	{"T", "\xc5\xa4"},																   // Ť
	{"u", "\xc3\xb9\xc3\xba\xc3\xbb\xc3\xbc\xc5\xab\xc5\xaf"},						   // ù ú û ü ū ů
	{"U", "\xc3\x99\xc3\x9a\xc3\x9b\xc3\x9c\xc5\xaa\xc5\xae"},						   // Ù Ú Û Ü Ū Ů
	{"y", "\xc3\xbf\xc3\xbd"},														   // ÿ ý
	{"Y", "\xc5\xb8\xc3\x9d"},														   // Ÿ Ý
	{"z", "\xc5\xbe\xc5\xba\xc5\xbc"},												   // ž ź ż
	{"Z", "\xc5\xbd\xc5\xb9\xc5\xbb"},												   // Ž Ź Ż
	{"\xd0\xb5", "\xd1\x91"},														   // е: ё
	{"\xd0\x95", "\xd0\x81"},														   // Е: Ё
};

// The config file keeps names, not indices.
static const char *const layout_keys[KB_LAYOUT_COUNT] = {"english", "italian", "german", "spanish",	 "french", "russian",
														 "dutch",   "polish",  "portuguese", "czech", "japanese"};

static uint8_t in_use[KB_LAYOUT_COUNT];
static uint8_t in_use_n;
static uint8_t others[KB_LAYOUT_COUNT];
static uint8_t others_n;
static bool loaded;

// ---------------------------------------------------------------------------
// The letters
// ---------------------------------------------------------------------------

// How many bytes the UTF-8 code point starting at `p` occupies.
static int utf8_len(const char *p) {
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

static const char *row_string(kblayout_t layout, int row, bool upper) {
	if (layout < 0 || layout >= KB_LAYOUT_COUNT || row < 0 || row >= KB_LAYOUT_ROWS) {
		return "";
	}
	return upper ? layouts[layout].upper[row] : layouts[layout].lower[row];
}

int kblayout_row_len(kblayout_t layout, int row) {
	const char *p = row_string(layout, row, false);
	int n = 0;
	while (*p) {
		p += utf8_len(p);
		n++;
	}
	return n;
}

const char *kblayout_letter(kblayout_t layout, int row, int index, bool upper) {
	// Copied out into a small rotation of buffers rather than returned as a
	// pointer into the table: the caller wants one letter, and the table holds
	// a whole row with no terminator between its letters.
	static char slots[4][8];
	static int at;

	const char *p = row_string(layout, row, upper);
	for (int i = 0; i < index && *p; i++) {
		p += utf8_len(p);
	}
	if (!*p) {
		return "";
	}

	int len = utf8_len(p);
	char *out = slots[at];
	at = (at + 1) % 4;
	memcpy(out, p, (size_t)len);
	out[len] = '\0';
	return out;
}

static const char *t9_string(kblayout_t layout, int key, bool upper) {
	if (layout < 0 || layout >= KB_LAYOUT_COUNT || key < 0 || key >= KB_LAYOUT_T9_KEYS) {
		return "";
	}
	return upper ? layouts[layout].t9_upper[key] : layouts[layout].t9_lower[key];
}

int kblayout_t9_len(kblayout_t layout, int key) {
	const char *p = t9_string(layout, key, false);
	int n = 0;
	while (*p) {
		p += utf8_len(p);
		n++;
	}
	return n;
}

const char *kblayout_t9_letter(kblayout_t layout, int key, int index, bool upper) {
	static char slots[4][8];
	static int at;

	const char *p = t9_string(layout, key, upper);
	for (int i = 0; i < index && *p; i++) {
		p += utf8_len(p);
	}
	if (!*p) {
		return "";
	}

	int len = utf8_len(p);
	char *out = slots[at];
	at = (at + 1) % 4;
	memcpy(out, p, (size_t)len);
	out[len] = '\0';
	return out;
}

static const char *variants_of(const char *key) {
	for (size_t i = 0; key && i < sizeof(VARIANTS) / sizeof(VARIANTS[0]); i++) {
		if (strcmp(VARIANTS[i].key, key) == 0) {
			return VARIANTS[i].variants;
		}
	}
	return "";
}

int kblayout_variant_count(const char *key) {
	const char *p = variants_of(key);
	int n = 0;
	while (*p) {
		p += utf8_len(p);
		n++;
	}
	return n;
}

const char *kblayout_variant(const char *key, int index) {
	static char slots[4][8];
	static int at;

	const char *p = variants_of(key);
	for (int i = 0; i < index && *p; i++) {
		p += utf8_len(p);
	}
	if (index < 0 || !*p) {
		return "";
	}

	int len = utf8_len(p);
	char *out = slots[at];
	at = (at + 1) % 4;
	memcpy(out, p, (size_t)len);
	out[len] = '\0';
	return out;
}

const char *kblayout_name(kblayout_t layout) {
	if (layout < 0 || layout >= KB_LAYOUT_COUNT) {
		return "";
	}
	return layouts[layout].name;
}

// ---------------------------------------------------------------------------
// The two lists
// ---------------------------------------------------------------------------

static uint8_t parse_list(const char *saved, uint8_t *out, bool *placed) {
	uint8_t n = 0;
	const char *p = saved ? saved : "";

	while (*p && n < KB_LAYOUT_COUNT) {
		while (*p == ',' || *p == ' ') {
			p++;
		}
		const char *start = p;
		while (*p && *p != ',') {
			p++;
		}
		size_t len = (size_t)(p - start);
		while (len > 0 && start[len - 1] == ' ') {
			len--;
		}
		for (int i = 0; i < KB_LAYOUT_COUNT && len; i++) {
			if (placed[i] || strlen(layout_keys[i]) != len || strncmp(layout_keys[i], start, len) != 0) {
				continue;
			}
			placed[i] = true;
			out[n++] = (uint8_t)i;
			break;
		}
	}
	return n;
}

// The layout the interface language asks for, which is the one a player that
// has never been told otherwise types in.
static kblayout_t layout_for_language(void) {
	const char *now = lang_current();
	for (int i = 0; i < KB_LAYOUT_COUNT && now; i++) {
		if (strcmp(layouts[i].language, now) == 0) {
			return (kblayout_t)i;
		}
	}
	return KB_LAYOUT_ENGLISH;
}

static void load(void) {
	if (loaded) {
		return;
	}
	loaded = true;

	bool placed[KB_LAYOUT_COUNT] = {false};
	in_use_n = parse_list(config_get("keyboard", "layouts", ""), in_use, placed);
	others_n = parse_list(config_get("keyboard", "layouts_off", ""), others, placed);

	// Nothing saved, or a layout that did not exist when it was saved. The
	// interface language's own goes in front of the ones in use; everything
	// else waits in the other list, because a page of alphabets is a list to
	// choose from, not a default.
	kblayout_t mine = layout_for_language();
	if (!placed[mine] && in_use_n == 0) {
		placed[mine] = true;
		in_use[in_use_n++] = (uint8_t)mine;
	}
	for (int i = 0; i < KB_LAYOUT_COUNT; i++) {
		if (!placed[i]) {
			others[others_n++] = (uint8_t)i;
		}
	}

	// A hand-edited file that emptied the first list: something has to be
	// typeable, so the first of the others is drafted.
	if (in_use_n == 0 && others_n > 0) {
		in_use[in_use_n++] = others[0];
		memmove(&others[0], &others[1], (size_t)(others_n - 1));
		others_n--;
	}
}

static void save(void) {
	char text[KB_LAYOUT_COUNT * 10];

	for (int list = 0; list < 2; list++) {
		const uint8_t *items = list ? others : in_use;
		int count = list ? others_n : in_use_n;
		size_t used = 0;
		text[0] = '\0';
		for (int i = 0; i < count; i++) {
			int wrote = snprintf(text + used, sizeof(text) - used, "%s%s", i ? "," : "", layout_keys[items[i]]);
			if (wrote <= 0 || (size_t)wrote >= sizeof(text) - used) {
				break;
			}
			used += (size_t)wrote;
		}
		config_set("keyboard", list ? "layouts_off" : "layouts", text);
	}
	config_save();
}

int kblayout_in_use_count(void) {
	load();
	return in_use_n;
}

kblayout_t kblayout_in_use_at(int position) {
	load();
	if (position < 0 || position >= in_use_n) {
		return KB_LAYOUT_ENGLISH;
	}
	return (kblayout_t)in_use[position];
}

int kblayout_others_count(void) {
	load();
	return others_n;
}

kblayout_t kblayout_others_at(int position) {
	load();
	if (position < 0 || position >= others_n) {
		return KB_LAYOUT_ENGLISH;
	}
	return (kblayout_t)others[position];
}

kblayout_t kblayout_default(void) {
	load();
	return in_use_n ? (kblayout_t)in_use[0] : KB_LAYOUT_ENGLISH;
}

void kblayout_language_changed(void) {
	const char *on = config_get("keyboard", "layouts", "");
	const char *off = config_get("keyboard", "layouts_off", "");
	if ((on && on[0]) || (off && off[0])) {
		return;
	}
	loaded = false;
}

bool kblayout_is_in_use(kblayout_t layout) {
	load();
	for (int i = 0; i < in_use_n; i++) {
		if (in_use[i] == layout) {
			return true;
		}
	}
	return false;
}

static void list_remove(uint8_t *items, uint8_t *count, kblayout_t layout) {
	for (int i = 0; i < *count; i++) {
		if (items[i] != layout) {
			continue;
		}
		memmove(&items[i], &items[i + 1], (size_t)(*count - i - 1));
		(*count)--;
		return;
	}
}

static void list_insert(uint8_t *items, uint8_t *count, kblayout_t layout, int position) {
	if (position < 0 || position > *count) {
		position = *count;
	}
	memmove(&items[position + 1], &items[position], (size_t)(*count - position));
	items[position] = (uint8_t)layout;
	(*count)++;
}

void kblayout_move_in_use(kblayout_t layout, int position) {
	load();
	if (layout < 0 || layout >= KB_LAYOUT_COUNT) {
		return;
	}
	if (kblayout_is_in_use(layout)) {
		list_remove(in_use, &in_use_n, layout);
	} else {
		list_remove(others, &others_n, layout);
	}
	list_insert(in_use, &in_use_n, layout, position);
	save();
}

void kblayout_move_others(kblayout_t layout, int position) {
	load();
	if (layout < 0 || layout >= KB_LAYOUT_COUNT) {
		return;
	}
	// The last one in use cannot be sent away: the keyboard would have no
	// letters to draw.
	if (kblayout_is_in_use(layout)) {
		if (in_use_n <= 1) {
			return;
		}
		list_remove(in_use, &in_use_n, layout);
	} else {
		list_remove(others, &others_n, layout);
	}
	list_insert(others, &others_n, layout, position);
	save();
}
