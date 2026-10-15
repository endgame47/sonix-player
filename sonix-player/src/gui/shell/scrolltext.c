#include "scrolltext.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/animpark.h"

#include "lvgl/src/misc/lv_text_ap.h"

#include <stdlib.h>
#include <string.h>

// How fast the text moves, and how long it sits still before it starts (and
// again at the end of every pass).
//
// A speed, not a duration, and that distinction is the point of this file.
// LVGL's label scroll takes the `anim_duration` style as the time for one
// whole pass -- `lv_anim_set_duration(&a, anim_time)` in lv_label.c, the pass
// covering the entire width of the text -- so a fixed duration makes longer
// text fly past faster, exactly backwards.
//
// The extreme case is an ICY on-air title, routinely something like "Artist -
// Song (Radio Edit) | Radio XYZ 101.5 - the best hits" and several times the
// length of a track title: under a fixed duration it goes past several times
// as fast and cannot be read at all. 55 px/s is a comfortable reading pace at
// any length.
#define SCROLL_SPEED_PX_S 55

// Floor and ceiling on one pass. The floor stops a barely-overflowing title
// from twitching; the ceiling exists only to bound the pathological case, not
// to shape the normal one, which is why it is so generous.
#define SCROLL_MIN_MS 4000
#define SCROLL_MAX_MS 60000

#define SCROLL_DELAY_MS 2500

// LVGL scrolls the text width plus this many spaces, so there is a visible gap
// between the end of one pass and the start of the next
// (LV_LABEL_WAIT_CHAR_COUNT in lv_conf_internal.h, which is not a public
// header -- hence the copy). Matching it makes the measured distance exactly
// the animated one.
#define SCROLL_WAIT_CHARS 3

// The template every scrolling label points at. It has to outlive them all --
// the style keeps the pointer, not a copy -- so it is a single static one,
// which is fine because every label wants the same timing.
static lv_anim_t scroll_template;
static bool template_ready;

static void build_template(void) {
	if (template_ready) {
		return;
	}
	lv_anim_init(&scroll_template);
	// The pause before the first pass.
	lv_anim_set_delay(&scroll_template, SCROLL_DELAY_MS);
	// The same pause between passes, so it does not become a loop that never
	// lets the beginning of the title be read.
	lv_anim_set_repeat_delay(&scroll_template, SCROLL_DELAY_MS);
	// The label copies the repeat count from the template too; without this it
	// would scroll exactly once and stop.
	lv_anim_set_repeat_count(&scroll_template, LV_ANIM_REPEAT_INFINITE);
	template_ready = true;
}

// How wide the text is, laid out on one line, in the label's own font.
static int32_t text_width(lv_obj_t *label, const char *text) {
	const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
	if (!font || !text || !text[0]) {
		return 0;
	}

	lv_point_t size;
	lv_text_get_size(&size, text, font, lv_obj_get_style_text_letter_space(label, LV_PART_MAIN),
					 lv_obj_get_style_text_line_space(label, LV_PART_MAIN), LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
	return size.x;
}

// ---------------------------------------------------------------------------
// The fade at the ends
//
// A scrolling label cuts its text at the box edge, mid-letter, and the letter
// left half-drawn reads as a rendering fault rather than as "there is more".
// Fading the last few pixels out says the same thing without the broken glyph.
//
// It is done with LVGL's bitmap mask: an A8 image whose alpha ramps down at the
// edge, hung on the label as its `bitmap_mask_src`. LVGL draws the label into
// its own layer and blends that layer through the mask, so what fades is the
// text over whatever is behind it -- which is what the player needs, where the
// title sits on blurred artwork and a gradient strip of background colour would
// be a smear of the wrong colour.
//
// Two masks per size. The right end is always a cut and always fades. The left
// one is a cut only while the text is moving; standing still, the first letter
// of the title is at that edge and has to be read, so it stays solid.
//
// Which of the two is up follows the label's own scroll animation: lv_anim
// keeps a negative act_time for the whole of a delay, so an animation that is
// absent or waiting means the text is parked. It is polled rather than hooked
// because LVGL offers no callback at the ends of a repetition, and a style must
// not be written from inside a draw.
//
// The mask is centred on the object, so it has to be exactly the label's size.
// Masks are built on demand and never freed: a label's style holds the pointer,
// and freeing one still in use would fault at the next redraw. The cache simply
// stops handing out fades once it is full.
// ---------------------------------------------------------------------------

#define FADE_EDGE_PX 24
#define FADE_CACHE_MAX 8
#define FADE_LABELS_MAX 16
#define FADE_POLL_MS 80

// One size, and the two masks for it: `both` while the text moves, `tail` while
// it stands still.
typedef struct {
	int32_t w, h;
	lv_image_dsc_t both;
	lv_image_dsc_t tail;
	bool ready;
} fade_masks_t;

static fade_masks_t fade_cache[FADE_CACHE_MAX];
static int fade_cache_used;

// Fills one A8 image of w x h whose rows are all `row`.
static bool fill_mask(lv_image_dsc_t *dsc, const uint8_t *row, int32_t w, int32_t h) {
	uint8_t *data = malloc((size_t)w * (size_t)h);
	if (!data) {
		return false;
	}
	for (int32_t y = 0; y < h; y++) {
		memcpy(data + (size_t)y * (size_t)w, row, (size_t)w);
	}

	memset(dsc, 0, sizeof(*dsc));
	dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
	dsc->header.cf = LV_COLOR_FORMAT_A8;
	dsc->header.w = (uint32_t)w;
	dsc->header.h = (uint32_t)h;
	dsc->header.stride = (uint32_t)w;
	dsc->data = data;
	dsc->data_size = (uint32_t)((size_t)w * (size_t)h);
	return true;
}

static const fade_masks_t *fade_masks(int32_t w, int32_t h) {
	// Narrower than three ramps and there is no middle left at full strength:
	// the whole label would be a fade, which is worse than a clean cut.
	if (w < 3 * FADE_EDGE_PX || h <= 0 || w > UINT16_MAX || h > UINT16_MAX) {
		return NULL;
	}

	for (int i = 0; i < fade_cache_used; i++) {
		if (fade_cache[i].w == w && fade_cache[i].h == h) {
			return &fade_cache[i];
		}
	}
	if (fade_cache_used >= FADE_CACHE_MAX) {
		return NULL;
	}

	uint8_t *row = malloc((size_t)w);
	if (!row) {
		return NULL;
	}

	// The right ramp first, which both masks share.
	for (int32_t x = 0; x < w; x++) {
		row[x] = (uint8_t)(x >= w - FADE_EDGE_PX ? ((w - 1 - x) * 255) / FADE_EDGE_PX : 255);
	}
	fade_masks_t *entry = &fade_cache[fade_cache_used];
	bool ok = fill_mask(&entry->tail, row, w, h);

	// Then the left one on top of it, for the moving mask.
	for (int32_t x = 0; ok && x < FADE_EDGE_PX; x++) {
		row[x] = (uint8_t)((x * 255) / FADE_EDGE_PX);
	}
	ok = ok && fill_mask(&entry->both, row, w, h);
	free(row);

	if (!ok) {
		free((void *)entry->tail.data);
		memset(entry, 0, sizeof(*entry));
		return NULL;
	}

	entry->w = w;
	entry->h = h;
	entry->ready = true;
	fade_cache_used++;
	return entry;
}

// The labels a mask has been worked out for, and which pair it is.
typedef struct {
	lv_obj_t *label;
	const fade_masks_t *masks; // NULL while the text fits
	// Where the scroll stood when the label was parked (see scroll_park), as
	// the animation's own clock; `resume` while it is still to be put back.
	int32_t resume_time;
	bool resume;
} fade_label_t;

static fade_label_t fade_labels[FADE_LABELS_MAX];
static int fade_label_count;
static lv_timer_t *fade_timer;

static fade_label_t *fade_entry(lv_obj_t *label) {
	for (int i = 0; i < fade_label_count; i++) {
		if (fade_labels[i].label == label) {
			return &fade_labels[i];
		}
	}
	if (fade_label_count >= FADE_LABELS_MAX) {
		return NULL;
	}
	fade_labels[fade_label_count].label = label;
	fade_labels[fade_label_count].masks = NULL;
	fade_labels[fade_label_count].resume = false;
	return &fade_labels[fade_label_count++];
}

// True while the label's text is actually travelling. lv_anim expresses a delay
// as a negative act_time, so the pause before a pass and the pause between
// passes both read as "not moving"; no animation at all means the text fits or
// has just been replaced.
static bool text_is_moving(lv_obj_t *label) {
	const lv_anim_t *anim = lv_anim_get(label, NULL);
	return anim && anim->act_time > 0;
}

// Puts the right mask of the pair on the label, or none at all.
static void fade_pick(fade_label_t *entry) {
	const lv_image_dsc_t *want = NULL;
	if (entry->masks) {
		want = text_is_moving(entry->label) ? &entry->masks->both : &entry->masks->tail;
	}

	// Compared before writing: the property is one LVGL rebuilds the object's
	// layer type from, and writing it unchanged would invalidate the label
	// twelve times a second for nothing.
	if (lv_obj_get_style_bitmap_mask_src(entry->label, LV_PART_MAIN) != want) {
		lv_obj_set_style_bitmap_mask_src(entry->label, want, 0);
	}
}

// A parked label (see the end of this file) is not moving and keeps the mask
// it was parked with, so only the others are looked at.
static void fade_poll_cb(lv_timer_t *timer) {
	(void)timer;
	bool any = false;
	for (int i = 0; i < fade_label_count; i++) {
		if (!fade_labels[i].masks || animpark_is_parked(fade_labels[i].label)) {
			continue;
		}
		any = true;
		fade_pick(&fade_labels[i]);
	}
	if (!any) {
		lv_timer_pause(fade_timer);
	}
}

// Works out whether this label overflows at all, and which pair of masks its
// size wants. The choice between the two is left to fade_pick().
static void fade_update(lv_obj_t *label) {
	fade_label_t *entry = fade_entry(label);
	if (!entry) {
		return;
	}

	int32_t w = lv_obj_get_width(label);
	int32_t h = lv_obj_get_height(label);
	entry->masks = NULL;

	if (w > 0 && text_width(label, lv_label_get_text(label)) > lv_obj_get_content_width(label)) {
		entry->masks = fade_masks(w, h);
	}

	fade_pick(entry);

	// The one timer that decides, for every scrolling label, whether its left
	// end is a cut or the start of the title. Made when the first overflowing
	// label turns up, and paused again by the poll once none is left.
	if (entry->masks && !animpark_is_parked(label)) {
		if (!fade_timer) {
			fade_timer = lv_timer_create(fade_poll_cb, FADE_POLL_MS, NULL);
		} else {
			lv_timer_resume(fade_timer);
		}
	}
}

// The label's width is not known when scrolltext_apply() runs -- the layout
// has not happened yet -- and it changes again whenever the box around it
// does, so the mask is decided here as well as at every text change.
static void fade_size_cb(lv_event_t *e) { fade_update(lv_event_get_target(e)); }

static void resume_cb(lv_event_t *e);

// A label that goes away must leave the list with it: the next one allocated
// could land on the same address and inherit its state.
static void fade_delete_cb(lv_event_t *e) {
	lv_obj_t *label = lv_event_get_target(e);
	lv_display_remove_event_cb_with_user_data(lv_obj_get_display(label), resume_cb, label);
	for (int i = 0; i < fade_label_count; i++) {
		if (fade_labels[i].label != label) {
			continue;
		}
		fade_labels[i] = fade_labels[fade_label_count - 1];
		fade_label_count--;
		return;
	}
}

// Measures what LVGL is about to animate and turns it into a duration at
// SCROLL_SPEED_PX_S.
//
// LVGL does offer a speed instead of a duration -- lv_anim_speed_clamped()
// encodes px/s into the same field -- but it packs the speed and both bounds
// into ten bits each, which caps the resulting pass at 10230 ms. A long radio
// title needs more than that at a readable pace, so it would hit that ceiling
// and start speeding up again. Sizing the text and setting a plain duration has
// no such limit.
static void apply_duration(lv_obj_t *label, const char *text) {
	const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
	if (!font || !text || !text[0]) {
		return;
	}

	// Exactly the distance lv_label.c animates over: the text, plus the gap it
	// leaves before the text comes round again.
	int32_t distance = text_width(label, text) + lv_font_get_glyph_width(font, ' ', ' ') * SCROLL_WAIT_CHARS;
	if (distance <= 0) {
		return;
	}

	uint32_t ms = (uint32_t)(((int64_t)distance * 1000) / SCROLL_SPEED_PX_S);
	if (ms < SCROLL_MIN_MS) {
		ms = SCROLL_MIN_MS;
	} else if (ms > SCROLL_MAX_MS) {
		ms = SCROLL_MAX_MS;
	}

	lv_obj_set_style_anim_duration(label, ms, 0);
}

// A change of text size: every scrolling label's pass is measured again, at
// the new width of its text, and its fade follows whether it still overflows.
static void fonts_changed(void) {
	for (int i = 0; i < fade_label_count; i++) {
		apply_duration(fade_labels[i].label, lv_label_get_text(fade_labels[i].label));
		fade_update(fade_labels[i].label);
	}
}

// Out of sight the label stops scrolling: clipped, with no animation left for
// LVGL to run, and the point the scroll had reached kept. Given back before it
// is drawn again (animpark.h), it carries on from there; a new text in the
// meantime starts from its pause instead. The text, the duration and the fade
// all go on being kept up to date while parked.
static void scroll_park(lv_obj_t *label) {
	// No animation: the text fits, or the label was given back so recently
	// that LVGL has not built it yet -- then the point still to be put back
	// is the one to keep.
	fade_label_t *entry = fade_entry(label);
	const lv_anim_t *anim = lv_anim_get(label, NULL);
	if (entry && anim) {
		entry->resume = true;
		entry->resume_time = anim->act_time;
	}
	lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
	fade_update(label);
}

// Puts the scroll back where it was parked. LVGL builds the scroll animation
// when the label is next laid out, which is still before the frame is drawn;
// this runs right after, at the same layout pass, so the frame already shows
// the text where it stopped.
static void resume_cb(lv_event_t *e) {
	lv_obj_t *label = lv_event_get_user_data(e);
	lv_display_remove_event_cb_with_user_data(lv_obj_get_display(label), resume_cb, label);
	fade_label_t *entry = fade_entry(label);
	if (!entry || !entry->resume || animpark_is_parked(label)) {
		return;
	}
	entry->resume = false;
	lv_anim_t *anim = lv_anim_get(label, NULL);
	if (!anim || !anim->exec_cb || !anim->path_cb || anim->duration <= 0) {
		return;
	}
	// The pass may have got shorter (a smaller font): no further than its end.
	anim->act_time = entry->resume_time < anim->duration ? entry->resume_time : anim->duration;
	if (anim->act_time > 0) {
		anim->current_value = anim->path_cb(anim);
		anim->exec_cb(anim->var, anim->current_value);
	}
	fade_pick(entry);
}

static void scroll_unpark(lv_obj_t *label) {
	lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
	fade_label_t *entry = fade_entry(label);
	if (entry && entry->resume) {
		// Added after the label's own handler, which set_long_mode has just
		// queued for the same event: the animation exists by the time this runs.
		lv_display_add_event_cb(lv_obj_get_display(label), resume_cb, LV_EVENT_UPDATE_LAYOUT_COMPLETED, label);
	}
	fade_update(label);
}

void scrolltext_apply(lv_obj_t *label) {
	if (!label) {
		return;
	}
	build_template();
	static bool registered;
	if (!registered) {
		fonts_register_change(fonts_changed);
		registered = true;
	}

	lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
	lv_obj_set_style_anim(label, &scroll_template, LV_PART_MAIN);
	apply_duration(label, lv_label_get_text(label));

	lv_obj_add_event_cb(label, fade_size_cb, LV_EVENT_SIZE_CHANGED, NULL);
	lv_obj_add_event_cb(label, fade_delete_cb, LV_EVENT_DELETE, NULL);
	fade_update(label);
	animpark_watch(label, scroll_park, scroll_unpark);
}

// `text` on one line: line breaks, tabs and other control characters become
// one space. A break would make the label two lines tall, and the row it sits
// in would spill over whatever is above it. Returns `text` itself when there
// is nothing to change, otherwise a copy the caller frees.
static char *one_line(const char *text) {
	const char *r = text;
	while (*r && (unsigned char)*r >= 0x20 && *r != 0x7F) {
		r++;
	}
	if (!*r) {
		return (char *)text;
	}

	char *copy = malloc(strlen(text) + 1);
	if (!copy) {
		return (char *)text;
	}
	char *w = copy;
	bool space = false;
	for (r = text; *r; r++) {
		unsigned char c = (unsigned char)*r;
		if (c < 0x20 || c == 0x7F) {
			if (!space && w > copy) {
				*w++ = ' ';
			}
			space = true;
			continue;
		}
		*w++ = (char)c;
		space = c == ' ';
	}
	while (w > copy && w[-1] == ' ') {
		w--;
	}
	*w = '\0';
	return copy;
}

// Whether `label` already shows `text`. A label keeps Arabic in its joined
// forms (LV_USE_ARABIC_PERSIAN_CHARS), never as it was set, so a line with
// Arabic in it is put in those forms before the two are compared. Otherwise
// an Arabic title would look new every time the same words came in, and its
// scroll would start again from the beginning each time.
static bool label_shows(lv_obj_t *label, const char *text) {
	const char *current = lv_label_get_text(label);
	if (!current) {
		return false;
	}
	if (strcmp(current, text) == 0) {
		return true;
	}
#if LV_USE_ARABIC_PERSIAN_CHARS
	// The Arabic block, U+0600 to U+06FF, is the code points whose UTF-8
	// starts with 0xD8 to 0xDB.
	const unsigned char *p = (const unsigned char *)text;
	while (*p && (*p < 0xD8 || *p > 0xDB)) {
		p++;
	}
	if (!*p) {
		return false;
	}
	char *joined = malloc(lv_text_ap_strlen(text) + 1);
	if (!joined) {
		return false;
	}
	lv_text_ap_proc(text, joined);
	bool same = strcmp(current, joined) == 0;
	free(joined);
	return same;
#else
	return false;
#endif
}

void scrolltext_set(lv_obj_t *label, const char *text) {
	if (!label) {
		return;
	}
	if (!text) {
		text = "";
	}
	char *line = one_line(text);
	if (label_shows(label, line)) {
		if (line != text) {
			free(line);
		}
		return; // same words: leave the animation where it is
	}

	// A new text scrolls from its start, wherever the old one had got to.
	fade_label_t *entry = fade_entry(label);
	if (entry) {
		entry->resume = false;
	}

	// The duration first: changing it rebuilds the scroll animation, so doing
	// it before the text means the text change is the last word and the
	// animation it builds is the one that stays.
	apply_duration(label, line);
	lv_label_set_text(label, line);
	fade_update(label);
	if (line != text) {
		free(line);
	}
}
