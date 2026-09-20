#include "lang.h"

// LVGL 9.2 made lv_roller_t opaque, and there is no lv_roller_get_mode():
// re-setting the options needs the mode the roller already has.
#include "lvgl/src/widgets/roller/lv_roller_private.h"

#include "src/system/core/config.h"

#include "lvgl/lvgl.h"
#include "lvgl/src/display/lv_display_private.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// A language is a table: one arena holding all the text, one array of entries
// pointing into it, and a hash index over the keys. Two of these exist for a
// moment during a switch, which is why it is a struct rather than a set of
// globals -- what is on screen is in the language being left and can only be
// read with its table, while the new text has to come from the other.
#define ARENA_STEP (32 * 1024)
#define MAX_ENTRIES 2048
#define BUCKETS 1024

typedef struct {
	uint32_t key_at; // offsets into the arena, not pointers: it moves as it grows
	uint32_t value_at;
	int next;		// index of the next entry in this bucket, -1 at the end
	int value_next; // the same chain for the by-value index below
} entry_t;

typedef struct {
	char *arena;
	size_t used, size;
	entry_t entries[MAX_ENTRIES];
	int count;
	int buckets[BUCKETS];
	// The same index the other way round, keyed on the translation rather than
	// on the tag. The re-labelling below asks nine hundred-odd questions of it
	// for every label on every page at once, and walking the whole table for
	// each of them is the language change taking seconds rather than a moment.
	int value_buckets[BUCKETS];
} table_t;

static table_t live;
static char current[64] = "Italiano";
static char dir_path[256] = LANG_DIR;

// ---------------------------------------------------------------------------
// the table
// ---------------------------------------------------------------------------

static unsigned hash_of(const char *s) {
	unsigned h = 2166136261u; // FNV-1a
	while (*s) {
		h ^= (unsigned char)*s++;
		h *= 16777619u;
	}
	return h;
}

static void table_reset(table_t *t) {
	free(t->arena);
	t->arena = NULL;
	t->used = t->size = 0;
	t->count = 0;
	for (int i = 0; i < BUCKETS; i++) {
		t->buckets[i] = -1;
		t->value_buckets[i] = -1;
	}
}

static const char *at(const table_t *t, uint32_t off) { return t->arena + off; }
static const char *entry_key(const table_t *t, int i) { return at(t, t->entries[i].key_at); }
static const char *entry_value(const table_t *t, int i) { return at(t, t->entries[i].value_at); }

// Offsets rather than pointers: realloc moves the arena, and an entry holding
// a pointer into the old block would be left dangling with nothing to say so.
static bool arena_put(table_t *t, const char *s, uint32_t *out) {
	size_t len = strlen(s);
	if (t->used + len + 1 > t->size) {
		size_t want = t->size;
		while (want < t->used + len + 1) {
			want += ARENA_STEP;
		}
		char *grown = realloc(t->arena, want);
		if (!grown) {
			return false;
		}
		t->arena = grown;
		t->size = want;
	}
	*out = (uint32_t)t->used;
	memcpy(t->arena + t->used, s, len + 1);
	t->used += len + 1;
	return true;
}

static void table_add(table_t *t, const char *key, const char *value) {
	// An empty translation means "not done yet", not "show nothing".
	if (t->count >= MAX_ENTRIES || !key[0] || !value[0]) {
		return;
	}

	uint32_t k, v;
	if (!arena_put(t, key, &k) || !arena_put(t, value, &v)) {
		return;
	}

	unsigned b = hash_of(at(t, k)) % BUCKETS;
	unsigned vb = hash_of(at(t, v)) % BUCKETS;
	t->entries[t->count].key_at = k;
	t->entries[t->count].value_at = v;
	t->entries[t->count].next = t->buckets[b];
	t->entries[t->count].value_next = t->value_buckets[vb];
	t->buckets[b] = t->count;
	t->value_buckets[vb] = t->count;
	t->count++;
}

static const char *table_find(const table_t *t, const char *key) {
	if (t->count == 0) {
		return NULL;
	}
	for (int i = t->buckets[hash_of(key) % BUCKETS]; i >= 0; i = t->entries[i].next) {
		if (strcmp(entry_key(t, i), key) == 0) {
			return entry_value(t, i);
		}
	}
	return NULL;
}

// The other direction, for the re-labelling below: what is on screen is the
// translation and the key has to be recovered from it.
//
// Two tags can carry the same text in one language and different text in
// another, so which of them answers has to be settled: the lowest entry, which
// is the one nearest the top of the file, exactly as a walk from the start
// would have found. The chain is built by prepending, so it is walked to the
// end rather than stopped at the first match.
static const char *table_find_by_value(const table_t *t, const char *value) {
	int best = -1;
	for (int i = t->value_buckets[hash_of(value) % BUCKETS]; i >= 0; i = t->entries[i].value_next) {
		if (strcmp(entry_value(t, i), value) == 0 && (best < 0 || i < best)) {
			best = i;
		}
	}
	return best >= 0 ? entry_key(t, best) : NULL;
}

// ---------------------------------------------------------------------------
// files
// ---------------------------------------------------------------------------

static char *trim(char *s) {
	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
		s++;
	}
	char *end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
		end--;
	}
	*end = '\0';
	return s;
}

// The escapes a one-line INI needs: a translation may want a line break, and
// it has to be able to write a backslash.
static void unescape(char *s) {
	char *w = s;
	for (const char *r = s; *r; r++) {
		if (*r != '\\' || !r[1]) {
			*w++ = *r;
			continue;
		}
		r++;
		switch (*r) {
		case 'n':
			*w++ = '\n';
			break;
		case 't':
			*w++ = '\t';
			break;
		default:
			*w++ = *r; // including a doubled backslash
			break;
		}
	}
	*w = '\0';
}

static bool load_file(table_t *t, const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	table_reset(t);

	char line[1024]; // twice the longest sentence in the interface
	while (fgets(line, sizeof(line), f)) {
		char *s = trim(line);
		if (!s[0] || s[0] == '#' || s[0] == ';' || s[0] == '[') {
			continue;
		}
		// The FIRST '=' only: a translation is allowed to contain one, and no
		// key does -- every key here is a C string literal from the source.
		char *eq = strchr(s, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		char *key = trim(s);
		char *value = trim(eq + 1);
		unescape(key);
		unescape(value);
		table_add(t, key, value);
	}

	fclose(f);
	return true;
}

static void path_for(char *out, size_t out_size, const char *name) {
	snprintf(out, out_size, "%s/%s.ini", dir_path, name);
}

// ---------------------------------------------------------------------------
// re-labelling what is already drawn
//
// Every page is built once, at startup, and the text it was given is a copy
// held inside the label. There is therefore nothing to ask the pages to redo
// -- and asking fifty files to each grow a refresh callback would be fifty
// chances to forget one. Instead the object tree is walked and every label
// whose text is recognised is set again.
//
// Recognised means: it is a value in the table being left (so the key can be
// recovered from it), or it is already that key. Anything else -- a song title,
// a formatted count, a file name -- is left alone. A track really called
// "Album" would be re-labelled until the list is next filled from the database,
// and that is the price of recovering keys from the text on screen.
// ---------------------------------------------------------------------------

static const table_t *from_table;
static const table_t *to_table;

static const char *convert_text(const char *shown) {
	const char *key = table_find_by_value(from_table, shown);
	if (!key) {
		key = shown; // already the source language
	}
	const char *now = table_find(to_table, key);
	return now ? now : key;
}

static void relabel_tree(lv_obj_t *obj) {
	if (!obj) {
		return;
	}

	// A roller is not a label: its options are one multi-line string it parses
	// into rows, and it keeps a count and a selection derived from them. The
	// month names live in one, so setting the label a roller draws with would
	// change what is on screen and leave the roller believing something else.
	// It gets the whole option string translated at once -- which is exactly
	// how it is written in the source, and therefore how it is keyed -- and
	// the selected row put back, because set_options resets it to the first.
	if (lv_obj_check_type(obj, &lv_roller_class)) {
		const char *options = lv_roller_get_options(obj);
		if (options && options[0]) {
			const char *now = convert_text(options);
			if (strcmp(now, options) != 0) {
				uint32_t selected = lv_roller_get_selected(obj);
				lv_roller_set_options(obj, now, ((lv_roller_t *)obj)->mode);
				lv_roller_set_selected(obj, selected, LV_ANIM_OFF);
			}
		}
		return; // never descend: the label inside is the roller's own business
	}

	// A text field's placeholder ("Search...", "Name...", "Password") is neither
	// a label nor a child: it is a string inside the widget that the textarea
	// draws itself while the field is empty. The walk below never saw it, so it
	// stayed forever in the language the page was built in at startup.
	//
	// The typed text is not touched: somebody entered it, and a typed word that
	// happens to resemble an interface string is not an interface string.
	if (lv_obj_check_type(obj, &lv_textarea_class)) {
		const char *hint = lv_textarea_get_placeholder_text(obj);
		if (hint && hint[0]) {
			const char *now = convert_text(hint);
			if (strcmp(now, hint) != 0) {
				lv_textarea_set_placeholder_text(obj, now);
			}
		}
		// Never descend: the label inside a textarea holds the typed text, which
		// must never be translated. Searching for "Album" and then changing
		// language must not rewrite what was searched for.
		return;
	}

	if (lv_obj_check_type(obj, &lv_label_class)) {
		// A label set to LV_LABEL_LONG_DOT does not keep the text it was
		// given: once LVGL has decided the text is too long it overwrites the
		// tail with dots, remembers the characters it covered, and
		// lv_label_get_text() then hands back the shortened form. A page built
		// at startup has never been laid out at its real width, so the dots
		// start at the first character and the whole string reads as "..." --
		// which matches nothing, and the heading stays in the old language.
		//
		// lv_label_set_text(obj, NULL) is LVGL's own "put the characters back
		// and refresh": it reverts the dots in place and leaves the text
		// alone. Changing the long mode does not, whatever it once did --
		// lv_label_set_long_mode() only marks the label for a refresh that has
		// not happened yet by the time the text is read back.
		//
		// Asked only of the labels that can carry dots at all: this walk sees
		// every label on every page, and the call reallocates the string.
		if (lv_label_get_long_mode(obj) == LV_LABEL_LONG_DOT) {
			lv_label_set_text(obj, NULL);
		}

		const char *text = lv_label_get_text(obj);
		if (text && text[0]) {
			const char *now = convert_text(text);
			if (strcmp(now, text) != 0) {
				lv_label_set_text(obj, now);
			}
		}
	}
	uint32_t n = lv_obj_get_child_count(obj);
	for (uint32_t i = 0; i < n; i++) {
		relabel_tree(lv_obj_get_child(obj, i));
	}
}

static void relabel_everything(const table_t *from, const table_t *to) {
	from_table = from;
	to_table = to;

	lv_display_t *disp = lv_display_get_default();
	if (!disp) {
		return;
	}
	for (uint32_t i = 0; i < disp->screen_cnt; i++) {
		relabel_tree(disp->screens[i]);
	}
	// The overlays do not live on a screen: the top bar, the quick panel, the
	// pop-overs and the toasts are all on the layers.
	relabel_tree(lv_display_get_layer_bottom(disp));
	relabel_tree(lv_display_get_layer_top(disp));
	relabel_tree(lv_display_get_layer_sys(disp));

	from_table = to_table = NULL;
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

void lang_init(const char *dir) {
	if (dir && dir[0]) {
		snprintf(dir_path, sizeof(dir_path), "%s", dir);
	}
	table_reset(&live);

	// With no line in the config nobody has chosen yet: this is the first boot,
	// and it starts in English -- not because English is better, but because it
	// is the one language whoever meets the welcome screen has some chance of
	// reading whatever theirs is, and the first thing that screen asks is which
	// language to continue in.
	const char *chosen = config_get("ui", "language", NULL);
	snprintf(current, sizeof(current), "%s", chosen ? chosen : LANG_FIRST_BOOT);

	char path[512];
	path_for(path, sizeof(path), current);
	if (load_file(&live, path)) {
		printf("lang: %s, %d texts from %s\n", current, live.count, path);
	} else {
		// Since the move to tags the source holds no Italian: it holds tags
		// ("podcast_search_for_a_podcast"), and every language, Italian included, is a
		// file to load. Without its file the interface shows bare tags --
		// deliberately ugly, because that is the unmistakable sign the .ini
		// files were not installed, not a case to paper over. Italian is still
		// tried as a fallback, being the file most likely to be present.
		fprintf(stderr, "lang: no %s.ini in %s: THE TAGS WILL SHOW. Install the language files.\n", current,
				dir_path);
		if (strcmp(current, "Italiano") != 0) {
			path_for(path, sizeof(path), "Italiano");
			if (load_file(&live, path)) {
				snprintf(current, sizeof(current), "Italiano");
				printf("lang: falling back to Italiano, %d texts\n", live.count);
			}
		}
	}
}

const char *tr(const char *text) {
	if (!text) {
		return "";
	}
	const char *found = table_find(&live, text);
	return found ? found : text;
}

const char *lang_current(void) { return current; }

static bool read_declared_name(const char *name, char *out, size_t size);

int lang_list(const char **names, int max) {
	// Whatever .ini files are in the folder, and nothing else. Dropping a new
	// one in is all it takes to add a language: the list is the directory.
	// Nothing is offered that has no file behind it -- the keys in the source
	// are tags, so a language with no file would show those tags rather than
	// any readable text.
	static char found[16][64];
	int count = 0;

	DIR *d = opendir(dir_path);
	if (!d) {
		return count;
	}
	struct dirent *e;
	while ((e = readdir(d)) != NULL && count < max && count < 16) {
		const char *dot = strrchr(e->d_name, '.');
		if (!dot || strcmp(dot, ".ini") != 0) {
			continue;
		}
		size_t len = (size_t)(dot - e->d_name);
		if (len == 0 || len >= sizeof(found[0])) {
			continue;
		}
		char name[64];
		memcpy(name, e->d_name, len);
		name[len] = '\0';

		// Two files can name the same language -- the shipped set carries both
		// `Francais.ini` and its `#U00e7` spelling -- and the list must not show
		// it twice. They are compared by what they declare, not by file name.
		char label[64];
		if (!read_declared_name(name, label, sizeof(label))) {
			snprintf(label, sizeof(label), "%s", name);
		}

		bool already = false;
		for (int i = 0; i < count; i++) {
			char other[64];
			if (!read_declared_name(names[i], other, sizeof(other))) {
				snprintf(other, sizeof(other), "%s", names[i]);
			}
			if (strcmp(other, label) == 0) {
				already = true;
				break;
			}
		}
		if (already) {
			continue;
		}
		snprintf(found[count], sizeof(found[0]), "%s", name);
		names[count] = found[count];
		count++;
	}
	closedir(d);
	return count;
}

static int hex_digit(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

// The name a file gives for itself, from its header line:
//
//     [Language: English]
//
// Read straight off the file rather than derived from its name, so the list
// shows what the translator wrote. Returns false when the file has no such
// line, and the caller falls back to the file name.
static bool read_declared_name(const char *name, char *out, size_t size) {
	char path[512];
	if ((size_t)snprintf(path, sizeof(path), "%s/%s.ini", dir_path, name) >= sizeof(path)) {
		return false;
	}
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}

	bool got = false;
	char line[256];
	while (!got && fgets(line, sizeof(line), f)) {
		char *s = trim(line);
		if (s[0] != '[') {
			continue;
		}
		char *close = strchr(s, ']');
		if (!close) {
			continue;
		}
		*close = '\0';
		if (strncasecmp(s + 1, "language", 8) != 0) {
			continue; // some other section
		}
		char *colon = strchr(s + 1, ':');
		if (!colon) {
			continue;
		}
		char *value = trim(colon + 1);
		if (value[0]) {
			snprintf(out, size, "%s", value);
			got = true;
		}
	}

	fclose(f);
	return got;
}

void lang_display_name(const char *name, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	if (!name) {
		out[0] = '\0';
		return;
	}

	if (read_declared_name(name, out, size)) {
		return;
	}

	// No header: fall back to the file name, decoding the Windows-style
	// `#Uxxxx` escapes some of the shipped files use. See lang.h -- basic
	// multilingual plane only, since `#Uxxxx` is four hex digits and no
	// existing language name needs more.

	size_t w = 0;
	for (const char *p = name; *p && w + 1 < size;) {
		int d0, d1, d2, d3;
		if (p[0] == '#' && (p[1] == 'U' || p[1] == 'u') && (d0 = hex_digit(p[2])) >= 0 &&
			(d1 = hex_digit(p[3])) >= 0 && (d2 = hex_digit(p[4])) >= 0 && (d3 = hex_digit(p[5])) >= 0) {
			unsigned cp = (unsigned)((d0 << 12) | (d1 << 8) | (d2 << 4) | d3);

			// Code point to UTF-8, and only if the whole sequence fits: half a
			// sequence would be worse than leaving it undecoded.
			char enc[4];
			size_t n = 0;
			if (cp < 0x80) {
				enc[n++] = (char)cp;
			} else if (cp < 0x800) {
				enc[n++] = (char)(0xC0 | (cp >> 6));
				enc[n++] = (char)(0x80 | (cp & 0x3F));
			} else {
				enc[n++] = (char)(0xE0 | (cp >> 12));
				enc[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
				enc[n++] = (char)(0x80 | (cp & 0x3F));
			}
			if (w + n + 1 > size) {
				break;
			}
			for (size_t i = 0; i < n; i++) {
				out[w++] = enc[i];
			}
			p += 6;
			continue;
		}
		out[w++] = *p++;
	}
	out[w] = '\0';
}

bool lang_set(const char *name) {
	if (!name || !name[0] || strcmp(name, current) == 0) {
		return true;
	}

	// Static rather than on the stack: the table carries two indexes now and is
	// forty kilobytes, which is more than a page's worth of stack to take for
	// the length of one call.
	static table_t next;
	memset(&next, 0, sizeof(next));
	table_reset(&next);

	char path[512];
	path_for(path, sizeof(path), name);
	if (!load_file(&next, path)) {
		// With tags in the source there is no free language: Italian is a file
		// too, and an unreadable file is a failure for any language, since an
		// empty table would show bare tags.
		table_reset(&next);
		free(next.arena);
		fprintf(stderr, "lang: %s not readable\n", path);
		return false;
	}

	relabel_everything(&live, &next);

	free(live.arena);
	live = next;

	snprintf(current, sizeof(current), "%s", name);
	config_set("ui", "language", current);
	config_save();
	printf("lang: switched to %s (%d texts)\n", current, live.count);
	return true;
}

