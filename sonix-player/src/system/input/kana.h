#ifndef KANA_H
#define KANA_H

#include <stdbool.h>
#include <stddef.h>

// Romaji to hiragana, one key at a time, with no LVGL and no allocation, so it
// can be tested on a PC. The keyboard feeds it the letters typed on the QWERTY
// rows; what comes back is the text to insert, and what is still waiting (the
// "k" of a "ka" not finished) is kept in the converter to be shown as pre-edit.
//
// Katakana is the same kana 0x60 higher: kana_to_katakana() converts a string.
// There is no kanji conversion; that needs a dictionary the player has no room
// for.

#define KANA_PENDING_MAX 4 // the longest romaji here is "xtsu"

typedef struct {
	char pending[KANA_PENDING_MAX + 1];
} kana_t;

void kana_reset(kana_t *k);

// One typed character. Any text that is now final is written to `out` as
// NUL-terminated UTF-8 (possibly nothing); the length is returned. Letters
// build up kana; "-" gives the long-vowel mark; anything else is passed
// through after finishing whatever was waiting.
size_t kana_feed(kana_t *k, int c, char *out, size_t cap);

// Finish what is waiting: a lone "n" becomes the kana n, other leftovers stay as typed.
size_t kana_flush(kana_t *k, char *out, size_t cap);

// Remove the last waiting letter. False when nothing was waiting, in which
// case the keyboard deletes from the text instead.
bool kana_backspace(kana_t *k);

// What is waiting, for display ("" when nothing).
const char *kana_pending(const kana_t *k);

// Hiragana to katakana, every other letter unchanged. `out` may not alias `in`.
void kana_to_katakana(const char *in, char *out, size_t cap);

// ---------------------------------------------------------------------------
// Typing into a text field
//
// A field has no room for a pre-edit string, so what is still waiting ("k" of a
// "ka") is written into the field as typed and replaced when the next letter
// settles it. The field is reached through two callbacks, so this part is
// tested on a PC against a plain string.
// ---------------------------------------------------------------------------

typedef struct {
	void (*erase)(void *ctx, int count); // delete `count` characters before the caret
	void (*insert)(void *ctx, const char *text);
} kana_sink_t;

// One letter typed: the waiting letters are taken back, and what the letter
// settles is written, followed by what is still waiting.
void kana_edit_key(kana_t *k, bool katakana, int c, const kana_sink_t *sink, void *ctx);

// Settle what is waiting (a lone "n" becomes the kana n) and write it.
void kana_edit_flush(kana_t *k, bool katakana, const kana_sink_t *sink, void *ctx);

// Delete key: true when it took back a waiting letter, false when the field
// should delete a character as usual.
bool kana_edit_backspace(kana_t *k, const kana_sink_t *sink, void *ctx);

// ---------------------------------------------------------------------------
// The ten-key keypad's dakuten key
// ---------------------------------------------------------------------------

// The character just before the caret: `cursor` counts characters, not bytes,
// as lv_textarea_get_cursor_pos() does. False at the start of the text.
bool kana_prev_char(const char *text, unsigned cursor, char *out, size_t cap);

// The next form of a kana: ka -> ga -> ka, ha -> ba -> pa -> ha, tsu -> small tsu
// -> dzu -> tsu, and the same in katakana. False for a letter that has none.
bool kana_cycle_mark(const char *prev, char *out, size_t cap);

// ---------------------------------------------------------------------------
// The kana pad
//
// The pad has seventeen keys in four rows (the controls are built in keyboard.c):
//
//     a   ka  sa  delete             keys 0  1  2
//     ta  na  ha  123                keys 3  4  5
//     ma  ya  ra  space              keys 6  7  8
//     marks  wa  mode  punct  search keys 10 11 .  9  .
//
// Twelve of them are handled here, with the numbers above: the kana keys 0 to 8
// and 11, the punctuation key 9, and the marks key 10, which is not a cycle (see
// kana_cycle_mark). The other five (delete, 123, space, the mode key and search)
// are plain controls.
//
// A key is a cycle: tapping it again steps through its row (a, i, u, e, o), the
// way a Japanese phone does. The key's cap is the first kana of the row.
// ---------------------------------------------------------------------------

#define KANA_KEY_COUNT 12
#define KANA_KEY_MARK 10 // the dakuten / small-kana key: it changes the kana before the caret

// How many steps key `key` has: 0 for the mark key and for a bad index.
int kana_key_len(int key);

// Step `tap` of a key's cycle, as hiragana or katakana. The text is in a buffer
// that the next call overwrites.
const char *kana_key_step(int key, int tap, bool katakana);

// What the key is labelled with: its first kana, or its first two marks for the
// punctuation key.
const char *kana_key_cap(int key, bool katakana);

// ---------------------------------------------------------------------------
// Flick on the kana pad
//
// A finger that goes down on a key and travels across it before it lifts picks
// a kana of the key's row by the direction: for the a row, centre a, left i, up u,
// right e, down o. That is the order of the row (see KEY_ROWS), so the other keys
// follow it: wa, wo, n and the long-vowel mark on the wa key, and the comma, the
// full stop, "?", "!" and the middle dot on the punctuation key. The ya key has
// three kana, which are centre ya, up yu, down yo. A tap, with no travel, is not
// a flick.
// ---------------------------------------------------------------------------

typedef enum {
	KANA_DIR_CENTER = 0,
	KANA_DIR_LEFT,
	KANA_DIR_UP,
	KANA_DIR_RIGHT,
	KANA_DIR_DOWN,
} kana_dir_t;

// The direction of a drag of (dx, dy) pixels, y growing downwards as on a
// screen: the longer of the two axes wins (vertical on a tie), and a drag that
// is shorter than `threshold` on both is no direction at all (CENTER).
kana_dir_t kana_flick_dir(int dx, int dy, int threshold);

// The kana a flick of that direction writes on key `key`, as hiragana or
// katakana; CENTER is the first kana, the same as a tap. "" when the key has
// nothing in that direction (and for the marks key). The text is in a buffer
// that the next call overwrites.
const char *kana_flick_step(int key, kana_dir_t dir, bool katakana);

#endif /* KANA_H */
