#include "keyboard.h"

#include "src/gui/shell/popover.h"
#include "src/system/input/kblayout.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

#include <string.h>

// ---------------------------------------------------------------------------
// The keyboard, built by hand out of real buttons: the stock LVGL keyboard
// could neither carry icon keys nor follow the theme. Shift and the accept
// key wear the accent; 123 swaps the letter caps for digits and symbols.
//
// One instance per page that needs one. The only per-page differences are the
// textarea being typed into and what the accent key does.
// ---------------------------------------------------------------------------

#define KB_MAX_KEYS KB_LAYOUT_MAX_KEYS

// A letter key's event user data is the instance and the index packed
// together, because LVGL gives a callback exactly one pointer. One set per
// keyboard, living inside the instance.
//
// They must belong to the instance and not to a shared table: several keyboards
// exist at once (Wi-Fi password, MSEB preset name, EQ preset name, search,
// playlist name), and a shared slot would let one keyboard's keys type into
// another's field.
typedef struct {
	struct keyboard_s *kb;
	int index;
} key_ref_t;

// The symbol page. Kept here and not in the layout table because it does not
// change with the alphabet: the digits and the punctuation are the same
// whether the letters around them are Latin or Cyrillic, which is how a phone
// behaves too. Three rows, so it drops into the same three rows the letters
// use.
static const char *const KB_SYM_ROWS[KB_LAYOUT_ROWS] = {
	"1234567890",
	"-_\'!?()&@",
	".,:;\"+%",
};

// The second symbol page, behind shift: the ASCII punctuation the first one
// has no room for, with the commonest marks repeated so the rows keep their
// length.
static const char *const KB_SYM2_ROWS[KB_LAYOUT_ROWS] = {
	"[]{}#$*=^~",
	"/\\|<>`_-+",
	".,?!\'\"%",
};

// ---------------------------------------------------------------------------
// T9: the nine-key phone keypad, multitap. Repeated taps on the same key walk
// its cycle by replacing the last character, and the character commits after a
// short pause (or when a different key is tapped).
//
// Two modes, switched by the "123"/"abc" key that always sits first on the
// bottom row: letters (keys 2-9 carry only letters, 1 carries punctuation) and
// numbers, laid out like the letters -- 123 / 456 / 789 -- each with its share
// of the QWERTY symbols at the tail of its cycle, and the 0 after the 9. Space
// exists in both modes; in number mode shift turns to the second symbol set.
// ---------------------------------------------------------------------------
#define KB_T9_KEYS KB_LAYOUT_T9_KEYS
#define KB_T9_COMMIT_MS 800

// Number mode: the digit first, then the sixteen symbols of the first QWERTY
// symbol page shared out at the tail of each cycle (.,:; -_ '" !? () &@ + %),
// none left out.
static const char *const KB_T9_NUM[KB_T9_KEYS] = {
	"1.,:;", "2-_", "3'\"", "4!?", "5()", "6&@", "7+", "8%", "90",
};

// Number mode with shift: the same digits over the second symbol page's
// sixteen marks.
static const char *const KB_T9_NUM2[KB_T9_KEYS] = {
	"1[]", "2{}", "3#$", "4*=", "5^~", "6/\\", "7|", "8<>", "90`",
};

struct keyboard_s {
	lv_obj_t *tray;
	lv_obj_t *field;

	// The two panels inside the tray: only one is visible, per
	// [other] keyboard_t9 (see keyboard_refresh_type).
	lv_obj_t *qwerty;
	lv_obj_t *t9;

	// The letter keys, and where each one sits on the panel now. There are as
	// many as the widest alphabet needs; the ones a narrower one does not use
	// are hidden rather than destroyed, and change rows when the layout does.
	lv_obj_t *rows[KB_LAYOUT_ROWS];
	lv_obj_t *letter_btn[KB_MAX_KEYS];
	lv_obj_t *letter_label[KB_MAX_KEYS];
	uint8_t key_row[KB_MAX_KEYS];
	uint8_t key_col[KB_MAX_KEYS];
	int key_count;
	lv_obj_t *delete_btn;

	// The alphabet this keyboard is typing in. It opens in the default one and
	// goes back to it at every reset; the "123" key's long press moves it to
	// another of the ones in use, for this keyboard alone.
	kblayout_t layout;

	lv_obj_t *shift_btn;
	lv_obj_t *shift_icon;
	lv_obj_t *mode_label; // "123" / "abc"
	lv_obj_t *accept_btn;

	// The T9 panel: the nine letter keys, its own shift and accept keys (the
	// theme repaints both), and the multitap state.
	lv_obj_t *t9_label[KB_T9_KEYS];
	lv_obj_t *t9_mode_label;  // the mode-switch key: "123" / "abc"
	lv_obj_t *t9_shift_btn;
	lv_obj_t *t9_shift_icon;
	lv_obj_t *t9_accept_btn;
	key_ref_t t9_refs[KB_T9_KEYS];
	int t9_last_key; // -1 = no pending character
	int t9_tap;		 // position in the key's cycle
	lv_timer_t *t9_timer; // the pause that commits the character
	bool t9_numbers;	  // T9 mode: false = letters, true = numbers

	// The bubble that pops above the key under the finger, the way a phone
	// keyboard shows what is being typed while the key itself is covered.
	lv_obj_t *preview;
	lv_obj_t *preview_label;
	lv_obj_t *host; // what the bubble is positioned within

	// The strip of accented letters a held key opens (kblayout_variant), and
	// which of them the finger is on. `variant_key` is the cap the strip was
	// opened for; `variant_swallow` eats the click that ends the hold, since
	// the letter is written on release instead.
	lv_obj_t *variants;
	lv_obj_t *variant_cell[KB_LAYOUT_MAX_VARIANTS];
	lv_obj_t *variant_label[KB_LAYOUT_MAX_VARIANTS];
	char variant_key[8];
	lv_obj_t *variant_btn;
	int variant_count;
	int variant_sel; // -1 while the strip is closed
	bool variant_swallow;

	bool mode_long_pressed; // the "123" key opened the layout menu; eat its click
	bool shift;
	bool caps; // set by a shift double-tap: upper case until shift is pressed again
	uint32_t shift_tap_ms; // last shift tap, to recognise the double
	bool symbols;
	bool symbols2;	  // the second symbol page, in the QWERTY panel and on the keypad

	key_ref_t refs[KB_MAX_KEYS];

	struct keyboard_s *next; // the chain below
};

// Every live keyboard, chained, so one theme-refresh callback can repaint them
// all (theme_register_refresh carries no user pointer). A list rather than an
// array: there is no number of keyboards at which silently dropping one is the
// right answer.
static keyboard_t *kb_list;
static bool refresh_registered;

// Every textarea whose caret has been styled. The colour is the accent, and
// the accent changes at runtime (Appearance > Accent colour), so the fields
// have to be reachable when it does -- LVGL repaints styles it owns, but this
// caret is a set of local style properties on each field.
#define KB_MAX_CARETS 12
static lv_obj_t *caret_fields[KB_MAX_CARETS];
static int caret_field_count;

static void kb_refresh_theme(void);

static void kb_register_refresh(void) {
	if (!refresh_registered) {
		refresh_registered = true;
		theme_register_refresh(kb_refresh_theme);
	}
}

// The tray the keys sit on. In the light theme both the page and the cards are
// near-white, so keys built straight out of those styles would be white on
// white -- they have to sit on something darker.
static lv_color_t kb_tray_color(void) {
	return theme()->dark ? lv_color_make(22, 22, 22) : lv_color_make(206, 209, 216);
}

// How many keys a row of the panel carries now, and what the one at `col`
// says. The symbol page and the alphabets are asked in the same shape, so
// everything above them treats a key as a place on a grid rather than as a
// letter.
static int kb_row_len(const keyboard_t *kb, int row) {
	if (row < 0 || row >= KB_LAYOUT_ROWS) {
		return 0;
	}
	return kb->symbols ? (int)strlen((kb->symbols2 ? KB_SYM2_ROWS : KB_SYM_ROWS)[row])
					   : kblayout_row_len(kb->layout, row);
}

static const char *kb_key_text(const keyboard_t *kb, int row, int col) {
	if (row < 0 || row >= KB_LAYOUT_ROWS || col < 0 || col >= kb_row_len(kb, row)) {
		return "";
	}
	if (kb->symbols) {
		static char one[2];
		one[0] = (kb->symbols2 ? KB_SYM2_ROWS : KB_SYM_ROWS)[row][col];
		one[1] = '\0';
		return one;
	}
	return kblayout_letter(kb->layout, row, col, kb->shift);
}

static void kb_refresh_caps(keyboard_t *kb); // just below, once the keys are placed

// One step of a keypad key's cycle, and how many steps it has. The number page
// is the same everywhere and stays a plain byte table; the letters come from
// the alphabet in use, which may be Cyrillic and so cannot be indexed by byte.
static int kb_t9_len(const keyboard_t *kb, int key) {
	if (key < 0 || key >= KB_T9_KEYS) {
		return 0;
	}
	return kb->t9_numbers ? (int)strlen((kb->symbols2 ? KB_T9_NUM2 : KB_T9_NUM)[key])
						  : kblayout_t9_len(kb->layout, key);
}

static const char *kb_t9_step(const keyboard_t *kb, int key, int tap) {
	if (kb->t9_numbers) {
		static char one[2];
		one[0] = (kb->symbols2 ? KB_T9_NUM2 : KB_T9_NUM)[key][tap];
		one[1] = '\0';
		return one;
	}
	return kblayout_t9_letter(kb->layout, key, tap, kb->shift);
}

// Puts every key into the row the current panel wants it in, and hides the
// ones this alphabet does not need.
//
// The keys are re-parented rather than rebuilt: a change of layout or a tap on
// "123" would otherwise destroy and recreate thirty buttons, and destroying a
// widget while a finger is on it is how a keyboard crashes.
static void kb_apply_layout(keyboard_t *kb) {
	if (!kb->rows[0]) {
		return;
	}

	int index = 0;
	for (int row = 0; row < KB_LAYOUT_ROWS; row++) {
		int len = kb_row_len(kb, row);
		for (int col = 0; col < len && index < KB_MAX_KEYS; col++, index++) {
			kb->key_row[index] = (uint8_t)row;
			kb->key_col[index] = (uint8_t)col;
			// set_parent appends, so working through the rows in order is what
			// puts the letters in the right order inside each of them.
			lv_obj_set_parent(kb->letter_btn[index], kb->rows[row]);
			lv_obj_set_hidden(kb->letter_btn[index], false);
		}
	}
	kb->key_count = index;

	for (int i = index; i < KB_MAX_KEYS; i++) {
		lv_obj_set_hidden(kb->letter_btn[i], true);
	}

	// The order inside each row, set key by key. lv_obj_set_parent() leaves a
	// key that is already in the row it is given where it is, and the keys that
	// move in go after it: from an alphabet with longer rows (Russian) back to a
	// shorter one, the letters come out shuffled. Shift opens the bottom row and
	// delete closes it.
	lv_obj_move_to_index(kb->shift_btn, 0);
	for (int i = 0; i < index; i++) {
		int first = kb->key_row[i] == 2 ? 1 : 0; // after shift
		lv_obj_move_to_index(kb->letter_btn[i], first + kb->key_col[i]);
	}
	lv_obj_move_to_index(kb->delete_btn, (int32_t)lv_obj_get_child_count(kb->rows[2]) - 1);

	kb_refresh_caps(kb);
}

// ---------------------------------------------------------------------------
// Choosing an alphabet from the "123" key
//
// A long press offers the other layouts in use. The choice is this keyboard's
// alone and lasts until it is reset, which every page does when it opens one:
// a Cyrillic search does not leave the playlist naming field in Cyrillic.
// ---------------------------------------------------------------------------

// One popover is open at a time, so one set of these is enough.
static struct {
	keyboard_t *kb;
	kblayout_t layout;
} layout_choices[KB_LAYOUT_COUNT];

static void kb_layout_pick(void *user) {
	int i = (int)(intptr_t)user;
	if (i < 0 || i >= KB_LAYOUT_COUNT || !layout_choices[i].kb) {
		return;
	}
	keyboard_t *kb = layout_choices[i].kb;
	kb->layout = layout_choices[i].layout;
	kb->symbols = false;	 // the letters are the point of the choice
	kb->symbols2 = false;
	kb->t9_numbers = false;	 // and the same on the keypad
	kb->t9_last_key = -1;
	kb_apply_layout(kb); // repaints both panels' caps
}

// The page carrying this keyboard has been left.
static void kb_screen_left_cb(lv_event_t *e) {
	keyboard_t *kb = lv_event_get_user_data(e);
	if (!kb || kb->layout == kblayout_default()) {
		return;
	}
	kb->layout = kblayout_default();
	kb->symbols = false;
	kb->symbols2 = false;
	kb_apply_layout(kb);
}

static void kb_layouts_cb(lv_event_t *e) {
	keyboard_t *kb = lv_event_get_user_data(e);
	if (!kb) {
		return;
	}

	popover_item_t items[KB_LAYOUT_COUNT];
	int count = 0;
	for (int i = 0; i < kblayout_in_use_count() && count < KB_LAYOUT_COUNT; i++) {
		kblayout_t layout = kblayout_in_use_at(i);
		layout_choices[count].kb = kb;
		layout_choices[count].layout = layout;
		items[count].label = kblayout_name(layout);
		items[count].action = kb_layout_pick;
		items[count].user = (void *)(intptr_t)count;
		items[count].checked = layout == kb->layout;
		count++;
	}

	// One alphabet in use is not a choice; the press does nothing rather than
	// opening a menu with a single entry that changes nothing -- and the click
	// that follows is then the plain switch to the symbol page, as always.
	if (count > 1) {
		kb->mode_long_pressed = true;
		popover_show(lv_event_get_current_target(e), items, count);
	}
}

// Repaints the letter caps and the shift key for the current mode.
static void kb_refresh_caps(keyboard_t *kb) {
	for (int i = 0; i < kb->key_count; i++) {
		lv_label_set_text(kb->letter_label[i], kb_key_text(kb, kb->key_row[i], kb->key_col[i]));
	}
	lv_label_set_text(kb->mode_label, kb->symbols ? "abc" : "123");

	// Shift wears the caps-lock icon while caps lock is on (set by a fast
	// double tap), on both keyboards.
	lv_image_set_src(kb->shift_icon, kb->caps ? &icon_caps_lock : &icon_shift);
	if (kb->symbols ? kb->symbols2 : kb->shift) {
		lv_obj_set_style_bg_color(kb->shift_btn, theme()->accent, 0);
		lv_obj_set_style_bg_opa(kb->shift_btn, LV_OPA_COVER, 0);
		lv_obj_set_style_image_recolor(kb->shift_icon, lv_color_white(), 0);
	} else {
		lv_obj_remove_local_style_prop(kb->shift_btn, LV_STYLE_BG_COLOR, 0);
		lv_obj_remove_local_style_prop(kb->shift_btn, LV_STYLE_BG_OPA, 0);
		lv_obj_set_style_image_recolor(kb->shift_icon, theme()->text_primary, 0);
	}
	lv_obj_set_style_image_recolor_opa(kb->shift_icon, LV_OPA_COVER, 0);

	// The T9 caps, from the current mode's table: in letter mode the case
	// follows shift, in number mode the digit sits above its share of symbols.
	for (int i = 0; i < KB_T9_KEYS; i++) {
		if (!kb->t9_label[i]) {
			continue;
		}
		char cap[16];
		if (kb->t9_numbers) {
			const char *cycle = (kb->symbols2 ? KB_T9_NUM2 : KB_T9_NUM)[i];
			if (cycle[1] != '\0') {
				snprintf(cap, sizeof(cap), "%c\n%s", cycle[0], cycle + 1);
			} else {
				snprintf(cap, sizeof(cap), "%c", cycle[0]);
			}
		} else {
			cap[0] = '\0';
			int steps = kb_t9_len(kb, i);
			for (int c = 0; c < steps; c++) {
				size_t at = strlen(cap);
				snprintf(cap + at, sizeof(cap) - at, "%s", kb_t9_step(kb, i, c));
			}
		}
		lv_label_set_text(kb->t9_label[i], cap);
	}
	if (kb->t9_mode_label) {
		lv_label_set_text(kb->t9_mode_label, kb->t9_numbers ? "abc" : "123");
	}
	if (kb->t9_shift_btn && kb->t9_shift_icon) {
		lv_image_set_src(kb->t9_shift_icon, kb->caps ? &icon_caps_lock : &icon_shift);
		// In number mode shift turns to the second symbol set; the 0 is on 9.
		if (kb->t9_numbers ? kb->symbols2 : kb->shift) {
			lv_obj_set_style_bg_color(kb->t9_shift_btn, theme()->accent, 0);
			lv_obj_set_style_bg_opa(kb->t9_shift_btn, LV_OPA_COVER, 0);
			lv_obj_set_style_image_recolor(kb->t9_shift_icon, lv_color_white(), 0);
		} else {
			lv_obj_remove_local_style_prop(kb->t9_shift_btn, LV_STYLE_BG_COLOR, 0);
			lv_obj_remove_local_style_prop(kb->t9_shift_btn, LV_STYLE_BG_OPA, 0);
			lv_obj_set_style_image_recolor(kb->t9_shift_icon, theme()->text_primary, 0);
		}
		lv_obj_set_style_image_recolor_opa(kb->t9_shift_icon, LV_OPA_COVER, 0);
	}
}

// ---------------------------------------------------------------------------
// T9 multitap
// ---------------------------------------------------------------------------

static void kb_shift_press(keyboard_t *kb); // defined with the shift key, below

// Commits the pending character: the multitap state resets and shift (one-shot
// as on QWERTY, unless caps lock is on) is consumed by it. The character is
// already in the field; committing only means it stops being replaced. With no
// character pending there is nothing for shift to have applied to, and it
// stays armed for the next one.
static void kb_t9_commit(keyboard_t *kb) {
	bool pending = kb->t9_last_key >= 0;
	kb->t9_last_key = -1;
	kb->t9_tap = 0;
	if (kb->t9_timer) {
		lv_timer_pause(kb->t9_timer);
	}
	if (pending && kb->shift && !kb->caps) {
		kb->shift = false;
		kb_refresh_caps(kb);
	}
}

static void kb_t9_timer_cb(lv_timer_t *t) {
	keyboard_t *kb = lv_timer_get_user_data(t);
	if (kb) {
		kb_t9_commit(kb);
	}
}

static void kb_t9_key_cb(lv_event_t *e) {
	key_ref_t *ref = lv_event_get_user_data(e);
	if (!ref || !ref->kb || !ref->kb->field) {
		return;
	}
	keyboard_t *kb = ref->kb;
	int k = ref->index;
	if (k < 0 || k >= KB_T9_KEYS) {
		return;
	}

	// The current mode's cycle: letters from the alphabet in use, or digits
	// with symbols at the tail.
	int len = kb_t9_len(kb, k);
	if (len == 0) {
		return;
	}

	if (kb->t9_last_key == k) {
		// Same key within the pause: advance the cycle, replacing the last
		// character written.
		kb->t9_tap = (kb->t9_tap + 1) % len;
		lv_textarea_delete_char(kb->field);
	} else {
		// Different key: the previous character commits, this one begins.
		kb_t9_commit(kb);
		kb->t9_last_key = k;
		kb->t9_tap = 0;
	}

	lv_textarea_add_text(kb->field, kb_t9_step(kb, k, kb->t9_tap));

	if (kb->t9_timer) {
		lv_timer_reset(kb->t9_timer);
		lv_timer_resume(kb->t9_timer);
	}
}

// The T9 space key: commits the pending character and writes the space. It
// exists in both modes.
static void kb_t9_space_cb(lv_event_t *e) {
	keyboard_t *kb = lv_event_get_user_data(e);
	if (!kb || !kb->field) {
		return;
	}
	kb_t9_commit(kb);
	lv_textarea_add_text(kb->field, " ");
}

// The letters <-> numbers switch: the key that always sits first on the bottom
// row.
static void kb_t9_mode_cb(lv_event_t *e) {
	keyboard_t *kb = lv_event_get_user_data(e);
	if (!kb) {
		return;
	}
	if (kb->mode_long_pressed) {
		kb->mode_long_pressed = false;
		return; // the click that ends the press that opened the layout menu
	}
	kb_t9_commit(kb);
	kb->t9_last_key = -1; // a cycle does not carry across a table change
	kb->t9_numbers = !kb->t9_numbers;
	kb->symbols2 = false;
	kb_refresh_caps(kb);
}

// The T9 shift key: shift/caps in letter mode, the second symbol set in number
// mode, where case means nothing.
static void kb_t9_shift_cb(lv_event_t *e) {
	keyboard_t *kb = lv_event_get_user_data(e);
	if (!kb) {
		return;
	}
	if (kb->t9_numbers) {
		kb_t9_commit(kb);
		kb->t9_last_key = -1; // a cycle does not carry across a table change
		kb->symbols2 = !kb->symbols2;
		kb_refresh_caps(kb);
		return;
	}
	// The character still cycling is done: shift is for the next one. Without
	// this the next key's commit would spend the new shift on the old letter.
	kb_t9_commit(kb);
	kb_shift_press(kb);
}

// ---------------------------------------------------------------------------
// keys
// ---------------------------------------------------------------------------

// Writes one letter and consumes a one-shot shift.
static void kb_type(keyboard_t *kb, const char *text) {
	lv_textarea_add_text(kb->field, text);
	if (kb->symbols) {
		return; // on the symbol pages shift picks the page and stays
	}
	if (kb->shift && !kb->caps) {
		kb->shift = false; // one-shot, like every phone keyboard, unless caps lock
		kb_refresh_caps(kb);
	}
}

static void kb_letter_cb(lv_event_t *e) {
	key_ref_t *ref = lv_event_get_user_data(e);
	if (!ref || !ref->kb->field) {
		return;
	}
	keyboard_t *kb = ref->kb;
	int index = ref->index;
	if (index < 0 || index >= kb->key_count) {
		return;
	}

	if (kb->variant_swallow) {
		kb->variant_swallow = false; // the hold wrote its letter on release
		return;
	}
	kb_type(kb, kb_key_text(kb, kb->key_row[index], kb->key_col[index]));
}

// A second shift tap within this window turns caps lock on.
#define KB_CAPS_DOUBLE_TAP_MS 350

// Shift behaves as on iOS: one tap arms a one-shot capital (another disarms
// it); two fast taps turn caps lock on -- the icon changes and upper case
// stays until shift is pressed again, which returns to lower case.
static void kb_shift_press(keyboard_t *kb) {
	uint32_t now = lv_tick_get();
	if (kb->caps) {
		kb->caps = false; // one tap leaves caps lock, back to lower case
		kb->shift = false;
	} else if (kb->shift && now - kb->shift_tap_ms <= KB_CAPS_DOUBLE_TAP_MS) {
		kb->caps = true; // the fast second tap: caps lock
	} else {
		kb->shift = !kb->shift;
	}
	kb->shift_tap_ms = now;
	kb_refresh_caps(kb);
}

static void kb_shift_cb(lv_event_t *e) {
	keyboard_t *kb = lv_event_get_user_data(e);
	if (!kb) {
		return;
	}
	if (kb->symbols) {
		kb->symbols2 = !kb->symbols2;
		kb_apply_layout(kb);
		return;
	}
	kb_shift_press(kb);
}

static void kb_delete_cb(lv_event_t *e) {
	keyboard_t *kb = lv_event_get_user_data(e);
	if (kb && kb->field) {
		// A pending T9 character is exactly the one being deleted, so the
		// cycle resets without consuming shift.
		kb->t9_last_key = -1;
		kb->t9_tap = 0;
		if (kb->t9_timer) {
			lv_timer_pause(kb->t9_timer);
		}
		lv_textarea_delete_char(kb->field);
	}
}

static void kb_mode_cb(lv_event_t *e) {
	keyboard_t *kb = lv_event_get_user_data(e);
	if (!kb) {
		return;
	}
	// A long press on this key opens the layout menu, and LVGL sends the click
	// that ends the same press straight after it. Without this the menu comes up
	// over a keyboard that has just switched to its symbol page.
	if (kb->mode_long_pressed) {
		kb->mode_long_pressed = false;
		return;
	}
	kb->symbols = !kb->symbols;
	kb->symbols2 = false;
	kb->shift = false;
	// Not just a repaint: the symbol page and the alphabet do not have the same
	// number of keys per row, so the keys have to be dealt out again.
	kb_apply_layout(kb);
}

static void kb_space_cb(lv_event_t *e) {
	keyboard_t *kb = lv_event_get_user_data(e);
	if (kb && kb->field) {
		lv_textarea_add_text(kb->field, " ");
	}
}

// ---------------------------------------------------------------------------
// the key preview
// ---------------------------------------------------------------------------

static void kb_preview_hide(keyboard_t *kb) {
	if (kb->preview) {
		lv_obj_set_hidden(kb->preview, true);
	}
}

static void kb_preview_show(keyboard_t *kb, lv_obj_t *key, const char *text) {
	if (!kb->preview || !key || !text || !text[0]) {
		return;
	}

	lv_label_set_text(kb->preview_label, text);
	lv_obj_set_hidden(kb->preview, false);
	lv_obj_move_foreground(kb->preview);

	// Centred over the key and sitting just above it, clamped to the host so
	// the outermost keys' bubbles stay fully on screen.
	lv_obj_update_layout(kb->preview);
	int w = lv_obj_get_width(kb->preview);
	int h = lv_obj_get_height(kb->preview);

	lv_area_t coords;
	lv_obj_get_coords(key, &coords);

	lv_area_t host_coords;
	lv_obj_get_coords(kb->host, &host_coords);

	int x = (coords.x1 + coords.x2) / 2 - w / 2 - host_coords.x1;
	int right_limit = lv_obj_get_width(kb->host) - w - 2;
	if (x > right_limit) {
		x = right_limit;
	}
	if (x < 2) {
		x = 2;
	}
	lv_obj_set_pos(kb->preview, x, coords.y1 - host_coords.y1 - h - 6);
}

static void kb_preview_cb(lv_event_t *e) {
	key_ref_t *ref = lv_event_get_user_data(e);
	if (!ref) {
		return;
	}
	keyboard_t *kb = ref->kb;

	if (lv_event_get_code(e) != LV_EVENT_PRESSED) {
		kb_preview_hide(kb);
		return;
	}
	if (ref->index < 0 || ref->index >= ref->kb->key_count) {
		return;
	}
	kb_preview_show(kb, kb->letter_btn[ref->index], lv_label_get_text(kb->letter_label[ref->index]));
}

// ---------------------------------------------------------------------------
// accented letters
//
// Holding a letter opens a strip of its accented forms above the key, as on a
// phone: the finger slides along it, the cell under it lights up, and letting
// go writes that letter. The strip opens with the cell nearest the finger lit,
// so a hold and release without sliding writes the first form offered there.
// ---------------------------------------------------------------------------

#define KB_VARIANT_PAD 6

// The key the strip was opened from. While it is open a slide along the strip
// must not scroll whatever page holds the keyboard.
static void kb_variant_hold(lv_obj_t *key, bool holding) {
	lv_obj_set_scroll_chain_hor(key, !holding);
	lv_obj_set_scroll_chain_ver(key, !holding);
}

static void kb_variants_close(keyboard_t *kb) {
	if (kb->variants) {
		lv_obj_set_hidden(kb->variants, true);
	}
	if (kb->variant_sel >= 0 && kb->variant_btn) {
		kb_variant_hold(kb->variant_btn, false);
	}
	kb->variant_btn = NULL;
	kb->variant_sel = -1;
}

static void kb_variants_paint(keyboard_t *kb) {
	for (int i = 0; i < kb->variant_count; i++) {
		bool on = i == kb->variant_sel;
		if (on) {
			lv_obj_set_style_bg_color(kb->variant_cell[i], theme()->accent, 0);
			lv_obj_set_style_bg_opa(kb->variant_cell[i], LV_OPA_COVER, 0);
			lv_obj_set_style_text_color(kb->variant_label[i], lv_color_white(), 0);
		} else {
			lv_obj_set_style_bg_opa(kb->variant_cell[i], LV_OPA_TRANSP, 0);
			lv_obj_set_style_text_color(kb->variant_label[i], theme()->text_primary, 0);
		}
	}
}

// The cell under screen x, clamped to the ends of the strip.
static int kb_variant_at(keyboard_t *kb, int32_t x) {
	lv_area_t first, last;
	lv_obj_get_coords(kb->variant_cell[0], &first);
	lv_obj_get_coords(kb->variant_cell[kb->variant_count - 1], &last);
	if (x <= first.x2) {
		return 0;
	}
	if (x >= last.x1) {
		return kb->variant_count - 1;
	}
	int pitch = lv_area_get_width(&first) + KB_VARIANT_PAD;
	int at = (int)(x - first.x1) / pitch;
	return at < 0 ? 0 : (at >= kb->variant_count ? kb->variant_count - 1 : at);
}

static void kb_variants_open(keyboard_t *kb, lv_obj_t *key, const char *cap) {
	int count = kblayout_variant_count(cap);
	if (!kb->variants || count <= 0) {
		return;
	}
	if (count > KB_LAYOUT_MAX_VARIANTS) {
		count = KB_LAYOUT_MAX_VARIANTS;
	}
	snprintf(kb->variant_key, sizeof(kb->variant_key), "%s", cap);
	kb->variant_count = count;
	for (int i = 0; i < KB_LAYOUT_MAX_VARIANTS; i++) {
		if (i < count) {
			lv_label_set_text(kb->variant_label[i], kblayout_variant(cap, i));
			lv_obj_set_hidden(kb->variant_cell[i], false);
		} else {
			lv_obj_set_hidden(kb->variant_cell[i], true);
		}
	}

	kb_preview_hide(kb);
	lv_obj_set_hidden(kb->variants, false);
	lv_obj_move_foreground(kb->variants);
	lv_obj_update_layout(kb->variants);

	// The first cell over the key, the strip running on towards the middle of
	// the screen, and the whole of it kept inside the host.
	int w = lv_obj_get_width(kb->variants);
	int h = lv_obj_get_height(kb->variants);
	int cell_w = lv_obj_get_width(kb->variant_cell[0]);

	lv_area_t coords;
	lv_obj_get_coords(key, &coords);
	lv_area_t host_coords;
	lv_obj_get_coords(kb->host, &host_coords);
	int host_w = lv_obj_get_width(kb->host);
	int key_x = (coords.x1 + coords.x2) / 2 - host_coords.x1;

	int x;
	if (key_x < host_w / 2) {
		x = key_x - cell_w / 2 - KB_VARIANT_PAD;
	} else {
		x = key_x + cell_w / 2 + KB_VARIANT_PAD - w;
	}
	if (x > host_w - w - 2) {
		x = host_w - w - 2;
	}
	if (x < 2) {
		x = 2;
	}
	lv_obj_set_pos(kb->variants, x, coords.y1 - host_coords.y1 - h - 6);
	lv_obj_update_layout(kb->variants);

	kb->variant_sel = kb_variant_at(kb, (coords.x1 + coords.x2) / 2);
	kb->variant_btn = key;
	kb_variant_hold(key, true);
	kb->variant_swallow = true;
	kb_variants_paint(kb);
}

static void kb_variants_cb(lv_event_t *e) {
	key_ref_t *ref = lv_event_get_user_data(e);
	if (!ref || !ref->kb) {
		return;
	}
	keyboard_t *kb = ref->kb;
	lv_event_code_t code = lv_event_get_code(e);

	if (code == LV_EVENT_PRESSED) {
		kb->variant_swallow = false; // a fresh press, whatever the last one left
		return;
	}
	if (code == LV_EVENT_LONG_PRESSED) {
		if (kb->symbols || ref->index < 0 || ref->index >= kb->key_count) {
			return;
		}
		kb_variants_open(kb, kb->letter_btn[ref->index], lv_label_get_text(kb->letter_label[ref->index]));
		return;
	}
	if (kb->variant_sel < 0) {
		return;
	}

	lv_indev_t *indev = lv_indev_active();
	if (code == LV_EVENT_PRESSING) {
		if (indev) {
			lv_point_t point;
			lv_indev_get_point(indev, &point);
			int sel = kb_variant_at(kb, point.x);
			if (sel != kb->variant_sel) {
				kb->variant_sel = sel;
				kb_variants_paint(kb);
			}
		}
		return;
	}
	if (code == LV_EVENT_RELEASED && kb->field) {
		kb_type(kb, kblayout_variant(kb->variant_key, kb->variant_sel));
	}
	kb_variants_close(kb); // RELEASED or PRESS_LOST
}

// ---------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------

static lv_obj_t *kb_make_row(lv_obj_t *parent, int height) {
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_set_size(row, lv_pct(100), height);
	lv_obj_set_style_bg_opa(row, 0, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_gap(row, 6, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	return row;
}

static lv_obj_t *kb_make_key(lv_obj_t *row, int width, lv_event_cb_t cb, void *user) {
	lv_obj_t *btn = lv_btn_create(row);
	if (width > 0) {
		lv_obj_set_size(btn, width, lv_pct(100));
	} else {
		lv_obj_set_height(btn, lv_pct(100));
		lv_obj_set_flex_grow(btn, 1);
	}
	lv_obj_add_style(btn, &theme_style_card, 0);
	lv_obj_add_style(btn, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(btn, 10, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_shadow_width(btn, 2, 0);
	lv_obj_set_style_shadow_opa(btn, LV_OPA_20, 0);
	lv_obj_set_style_shadow_color(btn, lv_color_black(), 0);
	lv_obj_set_style_shadow_offset_y(btn, 1, 0);
	lv_obj_set_style_pad_all(btn, 0, 0);
	if (cb) {
		lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
	}
	return btn;
}

static lv_obj_t *kb_key_label(lv_obj_t *btn) {
	lv_obj_t *label = lv_label_create(btn);
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_26, 0);
	lv_obj_center(label);
	return label;
}

static lv_obj_t *kb_key_icon(lv_obj_t *btn, const lv_image_dsc_t *glyph) {
	lv_obj_t *icon = lv_image_create(btn);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
	lv_obj_center(icon);
	return icon;
}

// Letter keys carry the preview bubble; the control keys (shift, delete, 123,
// space, accept) do not, exactly like a phone keyboard.
static void kb_attach_preview(lv_obj_t *btn, key_ref_t *ref) {
	lv_obj_add_event_cb(btn, kb_preview_cb, LV_EVENT_PRESSED, ref);
	lv_obj_add_event_cb(btn, kb_preview_cb, LV_EVENT_RELEASED, ref);
	lv_obj_add_event_cb(btn, kb_preview_cb, LV_EVENT_PRESS_LOST, ref);
}

// The accent bits, the tray and every caret re-read the palette after a
// theme/colour switch, for every keyboard and every field that exists.
static void kb_refresh_theme(void) {
	for (keyboard_t *kb = kb_list; kb; kb = kb->next) {
		if (kb->accept_btn) {
			lv_obj_set_style_bg_color(kb->accept_btn, theme()->accent, 0);
		}
		if (kb->t9_accept_btn) {
			lv_obj_set_style_bg_color(kb->t9_accept_btn, theme()->accent, 0);
		}
		if (kb->tray) {
			lv_obj_set_style_bg_color(kb->tray, kb_tray_color(), 0);
			lv_obj_set_style_bg_opa(kb->tray, LV_OPA_COVER, 0);
		}
		kb_refresh_caps(kb);
	}

	for (int i = 0; i < caret_field_count; i++) {
		keyboard_style_caret(caret_fields[i]);
	}
}

keyboard_t *keyboard_create(lv_obj_t *parent, int width, int height, lv_obj_t *field,
							const lv_image_dsc_t *accept_icon, const char *accept_text, lv_event_cb_t on_accept,
							void *user) {
	keyboard_t *kb = calloc(1, sizeof(*kb));
	if (!kb) {
		return NULL;
	}
	kb->field = field;
	kb->host = parent;

	kb->tray = lv_obj_create(parent);
	lv_obj_set_size(kb->tray, width, height);
	lv_obj_align(kb->tray, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_bg_color(kb->tray, kb_tray_color(), 0);
	lv_obj_set_style_bg_opa(kb->tray, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(kb->tray, 0, 0);
	lv_obj_set_style_radius(kb->tray, 0, 0);
	lv_obj_set_style_pad_all(kb->tray, 8, 0);
	lv_obj_set_style_pad_gap(kb->tray, 7, 0);
	lv_obj_set_scrollable(kb->tray, false);
	lv_obj_set_flex_flow(kb->tray, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(kb->tray, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	int row_h = (height - 16 - 3 * 7) / 4;
	int key_w = (width - 16 - 9 * 6) / 10;
	int index = 0;

	// The two panels, stacked inside the tray: QWERTY and T9. Only one stays
	// visible (keyboard_refresh_type), and every keyboard in the player
	// switches together when the setting changes.
	kb->qwerty = lv_obj_create(kb->tray);
	lv_obj_set_size(kb->qwerty, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_opa(kb->qwerty, 0, 0);
	lv_obj_set_style_border_width(kb->qwerty, 0, 0);
	lv_obj_set_style_pad_all(kb->qwerty, 0, 0);
	lv_obj_set_style_pad_gap(kb->qwerty, 7, 0);
	lv_obj_set_scrollable(kb->qwerty, false);
	lv_obj_set_flex_flow(kb->qwerty, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(kb->qwerty, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// One key_ref per letter, inside this instance: they live exactly as long
	// as the keyboard they belong to, and no two keyboards can share them.
	//
	// The keys are made once, as many as the widest alphabet needs, and every
	// one of them grows to fill its row: a row of twelve Cyrillic keys and a
	// row of ten Latin ones then come out at the widths their own rows allow,
	// with nothing to compute. kb_apply_layout() below is what puts each key
	// into a row and gives it its letter.
	kb->rows[0] = kb_make_row(kb->qwerty, row_h);
	kb->rows[1] = kb_make_row(kb->qwerty, row_h);
	kb->rows[2] = kb_make_row(kb->qwerty, row_h);

	kb->shift_btn = kb_make_key(kb->rows[2], key_w + 14, kb_shift_cb, kb);
	kb->shift_icon = kb_key_icon(kb->shift_btn, &icon_shift);

	for (index = 0; index < KB_MAX_KEYS; index++) {
		kb->refs[index].kb = kb;
		kb->refs[index].index = index;
		kb->letter_btn[index] = kb_make_key(kb->rows[0], 0, kb_letter_cb, &kb->refs[index]);
		kb->letter_label[index] = kb_key_label(kb->letter_btn[index]);
		kb_attach_preview(kb->letter_btn[index], &kb->refs[index]);
		lv_obj_add_event_cb(kb->letter_btn[index], kb_variants_cb, LV_EVENT_PRESSED, &kb->refs[index]);
		lv_obj_add_event_cb(kb->letter_btn[index], kb_variants_cb, LV_EVENT_LONG_PRESSED, &kb->refs[index]);
		lv_obj_add_event_cb(kb->letter_btn[index], kb_variants_cb, LV_EVENT_PRESSING, &kb->refs[index]);
		lv_obj_add_event_cb(kb->letter_btn[index], kb_variants_cb, LV_EVENT_RELEASED, &kb->refs[index]);
		lv_obj_add_event_cb(kb->letter_btn[index], kb_variants_cb, LV_EVENT_PRESS_LOST, &kb->refs[index]);
	}

	kb->delete_btn = kb_make_key(kb->rows[2], key_w + 14, kb_delete_cb, kb);
	kb_key_icon(kb->delete_btn, &icon_delete);
	// Holding delete keeps deleting.
	lv_obj_add_event_cb(kb->delete_btn, kb_delete_cb, LV_EVENT_LONG_PRESSED_REPEAT, kb);

	lv_obj_t *row4 = kb_make_row(kb->qwerty, row_h);
	lv_obj_t *mode = kb_make_key(row4, 84, kb_mode_cb, kb);
	kb->mode_label = kb_key_label(mode);
	// Held rather than tapped, the same key offers the other alphabets in use.
	lv_obj_add_event_cb(mode, kb_layouts_cb, LV_EVENT_LONG_PRESSED, kb);
	lv_obj_t *space = kb_make_key(row4, 0, kb_space_cb, kb); // grows
	kb_key_icon(space, &icon_space);
	kb->accept_btn = kb_make_key(row4, 84, on_accept, user);
	lv_obj_set_style_bg_color(kb->accept_btn, theme()->accent, 0);
	if (accept_icon) {
		lv_obj_t *glyph = kb_key_icon(kb->accept_btn, accept_icon);
		lv_obj_set_style_image_recolor(glyph, lv_color_white(), 0);
	} else {
		lv_obj_t *label = kb_key_label(kb->accept_btn);
		lv_label_set_text(label, tr(accept_text ? accept_text : "ok"));
		lv_obj_set_style_text_color(label, lv_color_white(), 0);
	}

	// --- the T9 panel ---
	// Four rows like the QWERTY: 1-2-3 / 4-5-6 / 7-8-9 / mode, shift, space,
	// delete, accept. In number mode the digit is the first step of its key's
	// cycle; in letter mode punctuation lives on the 1.
	kb->t9 = lv_obj_create(kb->tray);
	lv_obj_set_size(kb->t9, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_opa(kb->t9, 0, 0);
	lv_obj_set_style_border_width(kb->t9, 0, 0);
	lv_obj_set_style_pad_all(kb->t9, 0, 0);
	lv_obj_set_style_pad_gap(kb->t9, 7, 0);
	lv_obj_set_scrollable(kb->t9, false);
	lv_obj_set_flex_flow(kb->t9, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(kb->t9, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	kb->t9_last_key = -1;
	int t9_key_w = (width - 16 - 2 * 6) / 3;
	for (int r = 0; r < 3; r++) {
		lv_obj_t *trow = kb_make_row(kb->t9, row_h);
		for (int c = 0; c < 3; c++) {
			int k = r * 3 + c;
			kb->t9_refs[k].kb = kb;
			kb->t9_refs[k].index = k;
			lv_obj_t *btn = kb_make_key(trow, t9_key_w, kb_t9_key_cb, &kb->t9_refs[k]);
			kb->t9_label[k] = lv_label_create(btn);
			lv_obj_add_style(kb->t9_label[k], &theme_style_text, 0);
			lv_obj_set_style_text_font(kb->t9_label[k], &font_ui_20, 0);
			lv_obj_set_style_text_align(kb->t9_label[k], LV_TEXT_ALIGN_CENTER, 0);
			lv_obj_center(kb->t9_label[k]);
		}
	}
	lv_obj_t *t9_row4 = kb_make_row(kb->t9, row_h);
	// The mode switch, always first: "123" in letter mode, "abc" in number mode.
	lv_obj_t *t9_mode = kb_make_key(t9_row4, 84, kb_t9_mode_cb, kb);
	kb->t9_mode_label = kb_key_label(t9_mode);
	lv_obj_add_event_cb(t9_mode, kb_layouts_cb, LV_EVENT_LONG_PRESSED, kb);
	kb->t9_shift_btn = kb_make_key(t9_row4, key_w + 14, kb_t9_shift_cb, kb);
	kb->t9_shift_icon = kb_key_icon(kb->t9_shift_btn, &icon_shift);
	// The "0" that takes the shift icon's place in number mode.
	lv_obj_t *t9_space = kb_make_key(t9_row4, 0, kb_t9_space_cb, kb); // grows
	kb_key_icon(t9_space, &icon_space);
	lv_obj_t *t9_del = kb_make_key(t9_row4, key_w + 14, kb_delete_cb, kb);
	kb_key_icon(t9_del, &icon_delete);
	lv_obj_add_event_cb(t9_del, kb_delete_cb, LV_EVENT_LONG_PRESSED_REPEAT, kb);
	kb->t9_accept_btn = kb_make_key(t9_row4, 84, on_accept, user);
	lv_obj_set_style_bg_color(kb->t9_accept_btn, theme()->accent, 0);
	if (accept_icon) {
		lv_obj_t *glyph = kb_key_icon(kb->t9_accept_btn, accept_icon);
		lv_obj_set_style_image_recolor(glyph, lv_color_white(), 0);
	} else {
		lv_obj_t *label = kb_key_label(kb->t9_accept_btn);
		lv_label_set_text(label, tr(accept_text ? accept_text : "ok"));
		lv_obj_set_style_text_color(label, lv_color_white(), 0);
	}

	// The pause that commits a multitap character. Paused until something is
	// typed.
	kb->t9_timer = lv_timer_create(kb_t9_timer_cb, KB_T9_COMMIT_MS, kb);
	lv_timer_pause(kb->t9_timer);

	// One of the two panels is hidden, per the setting.
	if (config_get_int("other", "keyboard_t9", 0) != 0) {
		lv_obj_set_hidden(kb->qwerty, true);
	} else {
		lv_obj_set_hidden(kb->t9, true);
	}

	// The preview bubble lives above the keyboard, on the page itself, so it
	// can overhang the top row.
	kb->preview = lv_obj_create(parent);
	lv_obj_set_size(kb->preview, key_w + 22, row_h + 12);
	lv_obj_add_style(kb->preview, &theme_style_card, 0);
	lv_obj_set_style_radius(kb->preview, 12, 0);
	lv_obj_set_style_border_width(kb->preview, 0, 0);
	lv_obj_set_style_shadow_width(kb->preview, 14, 0);
	lv_obj_set_style_shadow_opa(kb->preview, LV_OPA_30, 0);
	lv_obj_set_style_shadow_color(kb->preview, lv_color_black(), 0);
	lv_obj_set_style_shadow_offset_y(kb->preview, 3, 0);
	lv_obj_set_style_pad_all(kb->preview, 0, 0);
	lv_obj_set_scrollable(kb->preview, false);
	lv_obj_set_clickable(kb->preview, false);
	lv_obj_set_hidden(kb->preview, true);

	kb->preview_label = lv_label_create(kb->preview);
	lv_obj_add_style(kb->preview_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(kb->preview_label, &font_ui_32, 0);
	lv_obj_center(kb->preview_label);

	// The accented-letter strip, beside the bubble and in the same style: one
	// cell per letter, as wide as a key.
	kb->variant_sel = -1;
	kb->variants = lv_obj_create(parent);
	lv_obj_set_size(kb->variants, LV_SIZE_CONTENT, row_h + 12);
	lv_obj_add_style(kb->variants, &theme_style_card, 0);
	lv_obj_set_style_radius(kb->variants, 12, 0);
	lv_obj_set_style_border_width(kb->variants, 0, 0);
	lv_obj_set_style_shadow_width(kb->variants, 14, 0);
	lv_obj_set_style_shadow_opa(kb->variants, LV_OPA_30, 0);
	lv_obj_set_style_shadow_color(kb->variants, lv_color_black(), 0);
	lv_obj_set_style_shadow_offset_y(kb->variants, 3, 0);
	lv_obj_set_style_pad_all(kb->variants, KB_VARIANT_PAD, 0);
	lv_obj_set_style_pad_gap(kb->variants, KB_VARIANT_PAD, 0);
	lv_obj_set_flex_flow(kb->variants, LV_FLEX_FLOW_ROW);
	lv_obj_set_scrollable(kb->variants, false);
	lv_obj_set_clickable(kb->variants, false);
	lv_obj_set_hidden(kb->variants, true);
	for (int i = 0; i < KB_LAYOUT_MAX_VARIANTS; i++) {
		kb->variant_cell[i] = lv_obj_create(kb->variants);
		lv_obj_set_size(kb->variant_cell[i], key_w, lv_pct(100));
		lv_obj_set_style_radius(kb->variant_cell[i], 8, 0);
		lv_obj_set_style_border_width(kb->variant_cell[i], 0, 0);
		lv_obj_set_style_bg_opa(kb->variant_cell[i], LV_OPA_TRANSP, 0);
		lv_obj_set_style_pad_all(kb->variant_cell[i], 0, 0);
		lv_obj_set_scrollable(kb->variant_cell[i], false);
		lv_obj_set_clickable(kb->variant_cell[i], false);

		kb->variant_label[i] = lv_label_create(kb->variant_cell[i]);
		lv_obj_add_style(kb->variant_label[i], &theme_style_text, 0);
		lv_obj_set_style_text_font(kb->variant_label[i], &font_ui_28, 0);
		lv_obj_center(kb->variant_label[i]);
	}

	kb->next = kb_list;
	kb_list = kb;
	kb_register_refresh();

	kb->layout = kblayout_default();
	kb_apply_layout(kb);

	// Leaving the page puts the alphabet back to the default. That is what
	// makes a layout picked from the "123" key temporary: it lasts as long as
	// the typing that asked for it and no longer, even on the pages that keep
	// their text between visits.
	lv_obj_t *screen = lv_obj_get_screen(kb->tray);
	if (screen) {
		lv_obj_add_event_cb(screen, kb_screen_left_cb, LV_EVENT_SCREEN_UNLOADED, kb);
	}
	return kb;
}

lv_obj_t *keyboard_obj(keyboard_t *kb) { return kb ? kb->tray : NULL; }

void keyboard_set_visible(keyboard_t *kb, bool visible) {
	if (!kb || !kb->tray) {
		return;
	}
	if (visible) {
		lv_obj_set_hidden(kb->tray, false);
	} else {
		lv_obj_set_hidden(kb->tray, true);
		kb_preview_hide(kb);
		kb_variants_close(kb);
	}
}

bool keyboard_is_visible(keyboard_t *kb) { return kb && kb->tray && !lv_obj_is_hidden(kb->tray); }

void keyboard_set_field(keyboard_t *kb, lv_obj_t *field) {
	if (kb) {
		kb->field = field;
	}
}

// The caret is the left border of the CURSOR part, not a block: three pixels
// of accent, like a system text field's cursor.
#define KB_CARET_WIDTH 3

void keyboard_style_caret(lv_obj_t *field) {
	if (!field) {
		return;
	}

	// Remembered, so the caret can be repainted when the accent changes. The
	// colour below is a local style property on the field, and nothing in
	// LVGL's own repaint knows to come back for it, so without this list the
	// caret keeps the colour it was born with.
	bool known = false;
	for (int i = 0; i < caret_field_count; i++) {
		if (caret_fields[i] == field) {
			known = true;
			break;
		}
	}
	if (!known && caret_field_count < KB_MAX_CARETS) {
		caret_fields[caret_field_count++] = field;
		kb_register_refresh();
	}

	const lv_style_selector_t parts[] = {LV_PART_CURSOR, LV_PART_CURSOR | LV_STATE_FOCUSED};
	for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		lv_obj_set_style_bg_opa(field, LV_OPA_TRANSP, parts[i]);
		lv_obj_set_style_border_side(field, LV_BORDER_SIDE_LEFT, parts[i]);
		lv_obj_set_style_border_color(field, theme()->accent, parts[i]);
		lv_obj_set_style_border_opa(field, LV_OPA_COVER, parts[i]);
		lv_obj_set_style_border_width(field, KB_CARET_WIDTH, parts[i]);
		lv_obj_set_style_anim_duration(field, 900, parts[i]); // a calm blink
	}
}

// ---------------------------------------------------------------------------
// password bullets
//
// MiSans gives U+2022 -- the bullet LVGL substitutes for each character -- the
// width of an ideograph: 1000 units out of 1000 em, holding a dot of 290. At
// 24 px that is a 7 px bullet every 24, so a password reads as a row of
// scattered dots rather than a hidden word.
//
// LVGL draws the bullets as ordinary text, so the fix is negative letter
// spacing on the field: -10 px brings the pitch to 14 and leaves 7 px between
// bullets, the proportions of a system password field.
//
// It cannot be left on permanently: the same style also applies to the
// placeholder, to the plain text shown when the eye is tapped, and to the
// character LVGL leaves readable for a moment after it is typed. So it is
// enabled only while the field really is showing bullets.
#define KB_PASSWORD_LETTER_SPACE (-10)

// The matching caret shift, which would otherwise land on top of the last
// bullet.
//
// LVGL puts the caret at the next character's pitch (lv_label_get_letter_pos,
// which does account for letter spacing) minus its own border. That is right
// for normal text, where a letter's ink fills most of its advance, but a bullet
// is a 7 px dot centred in a 24 px advance, so with spacing at -10 the point
// LVGL picks falls in the middle of the previous bullet's ink.
//
// The gap between two bullets runs from +1.5 to +8.5 of that point, so its
// centre is at -spacing/2. The CURSOR part's left padding is the only lever
// that moves the caret (x1 = position - padding - border), hence the negative
// value: -spacing/2 to reach the centre of the gap, -KB_CARET_WIDTH+1 because
// what must end up centred is the three-pixel strip, not its left edge.
//
// Applies only while the bullets are visible, exactly like the letter spacing.
#define KB_PASSWORD_CARET_PAD (KB_PASSWORD_LETTER_SPACE / 2 - KB_CARET_WIDTH + 1)

void keyboard_refresh_password(lv_obj_t *field) {
	if (!field) {
		return;
	}
	const char *text = lv_textarea_get_text(field);
	bool dots = lv_textarea_get_password_mode(field) && text && *text;
	lv_obj_set_style_text_letter_space(field, dots ? KB_PASSWORD_LETTER_SPACE : 0, 0);

	const lv_style_selector_t parts[] = {LV_PART_CURSOR, LV_PART_CURSOR | LV_STATE_FOCUSED};
	for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		lv_obj_set_style_pad_left(field, dots ? KB_PASSWORD_CARET_PAD : 0, parts[i]);
	}
}

static void kb_password_changed_cb(lv_event_t *e) { keyboard_refresh_password(lv_event_get_target(e)); }

void keyboard_style_password(lv_obj_t *field, uint32_t show_ms) {
	if (!field) {
		return;
	}
	lv_textarea_set_password_mode(field, true);
	lv_textarea_set_password_show_time(field, show_ms);
	// The field goes between empty and non-empty on every key, which is where
	// the letter spacing has to be switched on or off.
	lv_obj_add_event_cb(field, kb_password_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
	keyboard_refresh_password(field);
}

void keyboard_show_caret(lv_obj_t *field, bool on) {
	if (!field) {
		return;
	}
	// The cursor is the CURSOR part's left border (see keyboard_style_caret),
	// so turning it off means taking that border's opacity to zero.
	const lv_style_selector_t parts[] = {LV_PART_CURSOR, LV_PART_CURSOR | LV_STATE_FOCUSED};
	for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		lv_obj_set_style_border_opa(field, on ? LV_OPA_COVER : LV_OPA_TRANSP, parts[i]);
	}
}

void keyboard_reset(keyboard_t *kb) {
	if (!kb) {
		return;
	}
	kb->shift = false;
	kb->caps = false;
	kb->symbols = false;
	kb->symbols2 = false;
	kb->t9_last_key = -1;
	kb->t9_tap = 0;
	kb->t9_numbers = false;
	if (kb->t9_timer) {
		lv_timer_pause(kb->t9_timer);
	}
	// Back to the default alphabet as well: a layout picked from the "123" key
	// is meant to last as long as the typing that asked for it.
	kb->layout = kblayout_default();
	kb_apply_layout(kb);
}

// Applies the layout lists to every live keyboard, after the settings page has
// changed them: pages build their keyboards once at startup, so a change has to
// walk the list rather than wait for a rebuild that never comes.
//
// Every keyboard goes to the default, not only the ones whose layout has left
// the list: the default is the first of the ones in use, so moving another
// layout to the top is a change of default even though nothing left.
void keyboard_refresh_layout(void) {
	kblayout_t now = kblayout_default();
	for (keyboard_t *kb = kb_list; kb; kb = kb->next) {
		if (!kb->rows[0]) {
			continue;
		}
		kb->layout = now;
		kb_apply_layout(kb);
	}
}

// Applies [other] keyboard_t9 to every live keyboard, for the same reason: both
// panels already exist inside each keyboard, so only their visibility changes.
void keyboard_refresh_type(void) {
	bool t9 = config_get_int("other", "keyboard_t9", 0) != 0;
	for (keyboard_t *kb = kb_list; kb; kb = kb->next) {
		if (!kb->qwerty || !kb->t9) {
			continue;
		}
		if (t9) {
			lv_obj_set_hidden(kb->qwerty, true);
			lv_obj_set_hidden(kb->t9, false);
		} else {
			lv_obj_set_hidden(kb->t9, true);
			lv_obj_set_hidden(kb->qwerty, false);
		}
		keyboard_reset(kb);
	}
}
