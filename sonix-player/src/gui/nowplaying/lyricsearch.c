#include "lyricsearch.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"

lv_obj_t *lyricsearch_screen;

#define ROW_HEIGHT 92
#define ROW_RADIUS 12
#define APPLY_W 230
#define APPLY_H 64

static lv_obj_t *title_label;
static lv_obj_t *list;		   // the records
static lv_obj_t *preview;	   // the words of one of them
static lv_obj_t *preview_text; // inside `preview`
static lv_obj_t *apply_bar;	   // Apply, under the words

static char file_path[1024];
static lrclib_record_t *records;
static int record_count;
static int previewing = -1; // the record whose words are up, -1 on the list

static void format_length(int seconds, char *out, size_t size) {
	if (seconds <= 0) {
		out[0] = '\0';
		return;
	}
	snprintf(out, size, "%d:%02d", seconds / 60, seconds % 60);
}

// The words as they are read: timed lines without their "[01:02.34]" stamps,
// and the tag lines of an LRC file ("[ar:...]") left out. Empty lines are kept,
// since they are where the verses break.
static char *readable_text(const char *text) {
	char *out = malloc(strlen(text) + 1);
	if (!out) {
		return NULL;
	}
	char *w = out;
	const char *line = text;
	while (*line) {
		const char *end = strchr(line, '\n');
		size_t len = end ? (size_t)(end - line) : strlen(line);
		const char *p = line;
		const char *stop = line + len;
		bool stamped = false;
		while (p < stop && *p == '[') {
			const char *close = memchr(p, ']', (size_t)(stop - p));
			if (!close) {
				break;
			}
			p = close + 1;
			stamped = true;
		}
		while (p < stop && (*p == ' ' || *p == '\t')) {
			p++;
		}
		size_t rest = (size_t)(stop - p);
		if (rest > 0 && p[rest - 1] == '\r') {
			rest--;
		}
		if (rest > 0 || !stamped) {
			memcpy(w, p, rest);
			w += rest;
			*w++ = '\n';
		}
		if (!end) {
			break;
		}
		line = end + 1;
	}
	while (w > out && w[-1] == '\n') {
		w--;
	}
	*w = '\0';
	return out;
}

static void show_list(void) {
	previewing = -1;
	lv_label_set_text(title_label, tr("lyrics_results"));
	lv_obj_set_hidden(preview, true);
	lv_obj_set_hidden(apply_bar, true);
	lv_obj_set_hidden(list, false);
}

static void show_preview(int index) {
	if (index < 0 || index >= record_count) {
		return;
	}
	char *text = readable_text(records[index].text);
	if (!text) {
		return;
	}
	previewing = index;
	lv_label_set_text(title_label, records[index].track[0] ? records[index].track : tr("lyrics_results"));
	lv_label_set_text(preview_text, text);
	free(text);
	lv_obj_scroll_to_y(preview, 0, LV_ANIM_OFF);
	lv_obj_set_hidden(list, true);
	lv_obj_set_hidden(preview, false);
	lv_obj_set_hidden(apply_bar, false);
}

// The chevron and the swipe-back from the words go back to the list.
static bool back_guard(void) {
	if (previewing < 0) {
		return false;
	}
	show_list();
	return true;
}

static bool back_peek(void) { return previewing >= 0; }

static void row_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	show_preview((int)(intptr_t)lv_event_get_user_data(e));
}

static void apply_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active() || previewing < 0) {
		return;
	}
	if (!lrclib_save(file_path, records[previewing].text)) {
		toast_error("lyrics_not_saved");
		return;
	}
	// The path outlives the page, which lets go of everything once it is left.
	char path[sizeof(file_path)];
	snprintf(path, sizeof(path), "%s", file_path);
	show_list();
	back_btn_cb(NULL); // back into the player
	player_lyrics_reload(path);
	toast_success("lyrics_downloaded");
}

static void add_row(int index) {
	const lrclib_record_t *r = &records[index];

	lv_obj_t *row = lv_btn_create(list);
	lv_obj_set_size(row, lv_pct(100), ROW_HEIGHT);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, ROW_RADIUS, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 16, 0);
	lv_obj_set_style_pad_ver(row, 10, 0);
	lv_obj_set_style_pad_column(row, 12, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_event_bubble(row, true);
	lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// Title over artist and album.
	lv_obj_t *names = lv_obj_create(row);
	lv_obj_remove_style_all(names);
	lv_obj_set_height(names, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(names, 1);
	lv_obj_set_flex_flow(names, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(names, 4, 0);
	lv_obj_set_scrollable(names, false);
	lv_obj_set_event_bubble(names, true);

	lv_obj_t *track = lv_label_create(names);
	lv_label_set_text(track, r->track[0] ? r->track : "-");
	lv_label_set_long_mode(track, LV_LABEL_LONG_DOT);
	lv_obj_set_width(track, lv_pct(100));
	lv_obj_set_height(track, lv_font_get_line_height(&font_ui_24));
	lv_obj_add_style(track, &theme_style_text, 0);
	lv_obj_set_style_text_font(track, &font_ui_24, 0);

	char who[340];
	if (r->album[0]) {
		snprintf(who, sizeof(who), "%s - %s", r->artist, r->album);
	} else {
		snprintf(who, sizeof(who), "%s", r->artist);
	}
	lv_obj_t *sub = lv_label_create(names);
	lv_label_set_text(sub, who);
	lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
	lv_obj_set_width(sub, lv_pct(100));
	lv_obj_set_height(sub, lv_font_get_line_height(&font_ui_20));
	lv_obj_add_style(sub, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(sub, &font_ui_20, 0);

	// Length over kind, on the right.
	lv_obj_t *facts = lv_obj_create(row);
	lv_obj_remove_style_all(facts);
	lv_obj_set_size(facts, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(facts, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(facts, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	lv_obj_set_style_pad_row(facts, 4, 0);
	lv_obj_set_scrollable(facts, false);
	lv_obj_set_event_bubble(facts, true);

	char length[16];
	format_length(r->duration, length, sizeof(length));
	lv_obj_t *len_label = lv_label_create(facts);
	lv_label_set_text(len_label, length);
	lv_obj_add_style(len_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(len_label, &font_ui_20, 0);

	lv_obj_t *kind = lv_label_create(facts);
	lv_label_set_text(kind, tr(r->synced ? "lyrics_synced" : "lyrics_plain"));
	lv_obj_set_style_text_font(kind, &font_ui_20, 0);
	if (r->synced) {
		lv_obj_set_style_text_color(kind, theme()->accent, 0);
	} else {
		lv_obj_add_style(kind, &theme_style_text_dim, 0);
	}
}

void lyricsearch_open(const char *path, lrclib_record_t *records_in, int count) {
	lrclib_free(records, record_count);
	records = records_in;
	record_count = count;
	snprintf(file_path, sizeof(file_path), "%s", path);

	lv_obj_clean(list);
	for (int i = 0; i < record_count; i++) {
		add_row(i);
	}
	lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
	show_list();

	player_sheet_close(false);
	switch_screen(lyricsearch_screen);
	switcher_set_player_return(lyricsearch_screen);
}

// Left, whichever way: the records go, they are a few hundred kilobytes at most
// but nothing needs them any more.
static void unloaded_cb(lv_event_t *e) {
	(void)e;
	show_list();
	lv_obj_clean(list);
	lrclib_free(records, record_count);
	records = NULL;
	record_count = 0;
}

void lyricsearch_init(gui_config_t *cfg) {
	lv_obj_add_style(lyricsearch_screen, &theme_style_screen, 0);
	title_label = settingsrow_title(lyricsearch_screen, cfg, "lyrics_results");
	settingsrow_title_corner_slots(title_label, cfg, 0);

	int content_top = settingsrow_content_top(cfg);
	int height = cfg->screen_height - content_top;

	list = lv_obj_create(lyricsearch_screen);
	lv_obj_set_size(list, lv_pct(100), height);
	lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(list, 0, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_style_radius(list, 0, 0);
	lv_obj_set_style_pad_hor(list, cfg->padding, 0);
	lv_obj_set_style_pad_top(list, 0, 0);
	lv_obj_set_style_pad_bottom(list, cfg->padding, 0);
	lv_obj_set_style_pad_gap(list, 8, 0);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_event_bubble(list, true);
	switcher_attach_back_gesture(list);

	// The words, above a bar holding Apply.
	int bar_h = APPLY_H + 2 * cfg->padding;
	preview = lv_obj_create(lyricsearch_screen);
	lv_obj_set_size(preview, lv_pct(100), height - bar_h);
	lv_obj_align(preview, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(preview, 0, 0);
	lv_obj_set_style_border_width(preview, 0, 0);
	lv_obj_set_style_radius(preview, 0, 0);
	lv_obj_set_style_pad_hor(preview, cfg->padding, 0);
	lv_obj_set_style_pad_ver(preview, 0, 0);
	lv_obj_set_scroll_dir(preview, LV_DIR_VER);
	lv_obj_set_event_bubble(preview, true);
	switcher_attach_back_gesture(preview);

	preview_text = lv_label_create(preview);
	lv_label_set_long_mode(preview_text, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(preview_text, lv_pct(100));
	lv_obj_set_style_text_align(preview_text, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_line_space(preview_text, 8, 0);
	lv_obj_add_style(preview_text, &theme_style_text, 0);
	lv_obj_set_style_text_font(preview_text, &font_ui_24, 0);
	lv_label_set_text(preview_text, "");

	apply_bar = lv_obj_create(lyricsearch_screen);
	lv_obj_remove_style_all(apply_bar);
	lv_obj_set_size(apply_bar, lv_pct(100), bar_h);
	lv_obj_align(apply_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_scrollable(apply_bar, false);

	lv_obj_t *apply = lv_btn_create(apply_bar);
	lv_obj_set_size(apply, APPLY_W, APPLY_H);
	lv_obj_center(apply);
	lv_obj_add_style(apply, &theme_style_card, 0);
	lv_obj_add_style(apply, &theme_style_accent_bg, 0);
	lv_obj_add_style(apply, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(apply, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(apply, 0, 0);
	lv_obj_set_style_shadow_width(apply, 0, 0);
	lv_obj_add_event_cb(apply, apply_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *apply_label = lv_label_create(apply);
	lv_label_set_text(apply_label, tr("lyrics_apply"));
	lv_obj_set_style_text_font(apply_label, &font_ui_24, 0);
	lv_obj_set_style_text_color(apply_label, lv_color_white(), 0);
	lv_obj_center(apply_label);

	show_list();

	switcher_set_back_guard(lyricsearch_screen, back_guard);
	switcher_set_back_guard_peek(lyricsearch_screen, back_peek);
	lv_obj_add_event_cb(lyricsearch_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}
