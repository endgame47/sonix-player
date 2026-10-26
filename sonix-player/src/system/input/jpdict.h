#ifndef JPDICT_H
#define JPDICT_H

#include <stdbool.h>
#include <stddef.h>

// Hiragana to kanji candidates, with no LVGL and no allocation, so it can be
// tested on a PC like kana.c. The dictionary is the default one of
// lv_ime_jp_lite (see jpdict_data.c), kept in flash as plain const arrays made by
// tools/gen_jpdict.py: nothing is built at start-up and no RAM is used.
//
// It is a reading-to-kanji list, not a sentence converter: it answers for the
// whole reading it is given, not for the best split of a longer text.

// The longest candidate that can be handed out, in bytes with the NUL. Candidates
// are copied out into a buffer of this size or more.
#define JPDICT_CAND_MAX 64

// The most kana that may follow the okurigana of a verb or adjective, as its
// ending (a few in "tabenai": the stem, the okurigana and the ending "nai").
#define JPDICT_TAIL_MAX 3

typedef struct {
	const char *stem;					// internal: candidates of the verb or adjective entry, "/" between them
	const char *exact;					// internal: candidates of the entry whose key is the reading
	char okuri[4];						// the kana of the reading that follows the stem, as typed
	char tail[3 * JPDICT_TAIL_MAX + 1]; // the ending after the okurigana, as typed (see jpdict_lookup_before)
	int stem_count;
	int count; // all of them, the stem's first
} jpdict_hit_t;

// Look up a reading in hiragana, UTF-8. False when there is nothing for it.
//
// Two kinds of entry can match, and both are offered:
//   - a verb or adjective stem followed by one kana of its okurigana: the reading
//     "kaku" (in hiragana) matches the key "ka" + "k", so the candidates of the stem
//     come out with the kana put back (the stem's candidate plus the final "ku"),
//     and they come first;
//   - exactly the key: the reading itself.
// Every candidate stands for the whole reading. (jpdict_lookup_before can also
// put an ending after the okurigana: see there.)
bool jpdict_lookup(const char *reading, jpdict_hit_t *hit);

// Candidate `index` (0 .. hit->count-1) as NUL-terminated UTF-8 in `out`. The
// length is returned; 0, and an empty string, for a bad index or a `cap` that is
// too small.
size_t jpdict_candidate(const jpdict_hit_t *hit, int index, char *out, size_t cap);

// The hiragana just before the caret, looked up the way a phone does: the
// longest run of up to `max_chars` characters that has an entry. `text` is the
// whole field and `cursor` the caret counted in characters, as
// lv_textarea_get_cursor_pos() gives it. On a hit, `reading` receives the run that
// matched (NUL-terminated, `cap` bytes, which must hold 3 * max_chars + 1) and
// `chars` how many characters of the field it is: the caller replaces those
// characters with a candidate. False when the text before the caret does not end
// in hiragana, or nothing is known for it.
//
// A verb or adjective keeps its ending as typed: of the run "taberu" (in hiragana)
// the stem and its okurigana "tabe" are looked up and the final "ru" is left as it
// is, so the candidate offered is the whole word. Up to JPDICT_TAIL_MAX kana of
// ending are allowed, and only for a stem; a noun is not offered with kana after it.
bool jpdict_lookup_before(const char *text, unsigned cursor, unsigned max_chars, jpdict_hit_t *hit, char *reading, size_t cap, unsigned *chars);

#endif /* JPDICT_H */
