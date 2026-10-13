#include "podcastopml.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/streaming/podcastpage.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/system.h"
#include "src/system/net/wifi.h"
#include "src/system/streaming/opml.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcastdl.h"
#include "src/system/streaming/podcastsubs.h"

#define FEED_ROW_HEIGHT 80
#define EXPORT_NAME "Sonix.opml"

static gui_config_t *g_cfg;

static lv_obj_t *files_screen;
static lv_obj_t *files_list;
static lv_obj_t *files_empty;

static lv_obj_t *feeds_screen;
static lv_obj_t *feeds_title;
static lv_obj_t *feeds_list;
static lv_obj_t *feeds_file_label;
static lv_obj_t *feeds_count_label;
static lv_obj_t *feeds_status;
static lv_obj_t *feeds_note;
static lv_obj_t *follow_btn;
static lv_obj_t *follow_label;

static lv_obj_t *result_screen;
static lv_obj_t *result_list;

static char files[OPML_MAX_FILES][512];
static int file_count;
static char chosen_file[512];

// ---------------------------------------------------------------------------
// the feeds of the chosen file, and where each one stands
// ---------------------------------------------------------------------------

typedef enum {
	FEED_LOOKING,
	FEED_FOUND,	   // in the catalogue, and not followed yet
	FEED_ALREADY,  // followed already
	FEED_MISSING,  // the catalogue does not have it
	FEED_FAILED,   // the catalogue could not be asked
} feed_state_t;

typedef struct {
	opml_feed_t source;
	podcast_feed_t found;
	feed_state_t state;
	bool checked;
	lv_obj_t *row;
	lv_obj_t *box;
	lv_obj_t *tick;
	lv_obj_t *status;
} feed_t;

static feed_t *feeds;
static int feed_count;
static int looked_at;

// Bumped whenever the list is replaced or left: a lookup still running for the
// old one sees it and stops.
static pthread_mutex_t lookup_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned lookup_generation;

static bool network_up(void) {
#ifdef HOST_BUILD
	return true;
#else
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
#endif
}

static const char *file_name(const char *path) {
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

// "Podcast/" or "" -- where on the card the file is, as the user put it there.
static void file_place(const char *path, char *out, size_t size) {
	const char *root = storage_sd_root();
	size_t n = root ? strlen(root) : 0;
	const char *rest = n && strncmp(path, root, n) == 0 ? path + n + 1 : path;
	const char *slash = strrchr(rest, '/');
	if (slash) {
		snprintf(out, size, "%.*s/", (int)(slash - rest), rest);
	} else {
		snprintf(out, size, "%s", tr("podcast_card_root"));
	}
}

static int count_checked(void) {
	int n = 0;
	for (int i = 0; i < feed_count; i++) {
		if (feeds[i].checked && feeds[i].state == FEED_FOUND) {
			n++;
		}
	}
	return n;
}

static void paint_feed(feed_t *f) {
	static const char *const STATUS[] = {"podcast_opml_looking", "podcast_opml_found", "podcast_opml_already",
										 "podcast_opml_missing", "podcast_opml_failed"};
	lv_label_set_text(f->status, tr(STATUS[f->state]));
	lv_color_t color = theme()->text_secondary;
	if (f->state == FEED_FOUND) {
		color = lv_color_hex(0x57c26a);
	} else if (f->state == FEED_MISSING) {
		color = lv_color_hex(0xe8a33d);
	} else if (f->state == FEED_FAILED) {
		color = lv_color_hex(0xff7b72);
	}
	lv_obj_set_style_text_color(f->status, color, 0);

	bool usable = f->state == FEED_FOUND;
	bool ticked = f->checked && usable;
	lv_obj_set_style_bg_opa(f->box, ticked || f->state == FEED_ALREADY ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_color(f->box, f->state == FEED_ALREADY ? theme()->surface_pressed : theme()->accent, 0);
	lv_obj_set_style_border_width(f->box, ticked || f->state == FEED_ALREADY ? 0 : 2, 0);
	lv_obj_set_hidden(f->tick, !(ticked || f->state == FEED_ALREADY));
	lv_obj_set_style_opa(f->row, usable || f->state == FEED_LOOKING ? LV_OPA_COVER : LV_OPA_60, 0);
}

static void paint_totals(void) {
	int checked = count_checked();
	if (looked_at < feed_count) {
		lv_label_set_text_fmt(feeds_status, tr("podcast_opml_looking_count"), looked_at, feed_count);
	} else {
		lv_label_set_text_fmt(feeds_status, tr("podcast_opml_to_follow"), checked, feed_count);
	}
	if (checked == 1) {
		lv_label_set_text(follow_label, tr("podcast_opml_follow_one"));
	} else {
		lv_label_set_text_fmt(follow_label, tr("podcast_opml_follow_n"), checked);
	}
	if (checked > 0) {
		lv_obj_remove_state(follow_btn, LV_STATE_DISABLED);
		lv_obj_set_style_bg_opa(follow_btn, LV_OPA_COVER, 0);
	} else {
		lv_obj_add_state(follow_btn, LV_STATE_DISABLED);
		lv_obj_set_style_bg_opa(follow_btn, LV_OPA_40, 0);
	}
}

// ---------------------------------------------------------------------------
// asking the catalogue, one feed at a time
// ---------------------------------------------------------------------------

typedef struct {
	unsigned generation;
	int index;
	int result; // podcast_feed_by_url's answer
	podcast_feed_t found;
} answer_t;

static void answer_cb(void *user) {
	answer_t *a = user;
	pthread_mutex_lock(&lookup_lock);
	bool current = a->generation == lookup_generation;
	pthread_mutex_unlock(&lookup_lock);
	if (current && a->index >= 0 && a->index < feed_count) {
		feed_t *f = &feeds[a->index];
		if (a->result > 0) {
			f->found = a->found;
			if (podcastsubs_is_followed(a->found.id)) {
				f->state = FEED_ALREADY;
				f->checked = false;
			} else {
				f->state = FEED_FOUND;
				f->checked = true;
			}
		} else {
			f->state = a->result == 0 ? FEED_MISSING : FEED_FAILED;
			f->checked = false;
		}
		looked_at++;
		paint_feed(f);
		paint_totals();
	}
	free(a);
}

typedef struct {
	unsigned generation;
	int count;
	char (*urls)[PODCAST_FEED_URL_MAX];
	int *indexes;
} lookup_job_t;

static void *lookup_main(void *arg) {
	lookup_job_t *job = arg;
	thread_be_background("podcast opml");
	for (int i = 0; i < job->count; i++) {
		pthread_mutex_lock(&lookup_lock);
		bool stale = job->generation != lookup_generation;
		pthread_mutex_unlock(&lookup_lock);
		if (stale) {
			break;
		}
		answer_t *a = calloc(1, sizeof(*a));
		if (!a) {
			break;
		}
		a->generation = job->generation;
		a->index = job->indexes[i];
		a->result = podcast_feed_by_url(job->urls[i], &a->found);
		if (!gui_post(answer_cb, a)) {
			free(a);
		}
	}
	free(job->urls);
	free(job->indexes);
	free(job);
	return NULL;
}

static void lookup_start(void) {
	lookup_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	job->urls = calloc((size_t)(feed_count > 0 ? feed_count : 1), sizeof(*job->urls));
	job->indexes = calloc((size_t)(feed_count > 0 ? feed_count : 1), sizeof(int));
	if (!job->urls || !job->indexes) {
		free(job->urls);
		free(job->indexes);
		free(job);
		return;
	}
	for (int i = 0; i < feed_count; i++) {
		if (feeds[i].state != FEED_LOOKING) {
			continue;
		}
		snprintf(job->urls[job->count], PODCAST_FEED_URL_MAX, "%s", feeds[i].source.url);
		job->indexes[job->count] = i;
		job->count++;
	}
	pthread_mutex_lock(&lookup_lock);
	job->generation = lookup_generation;
	pthread_mutex_unlock(&lookup_lock);
	pthread_t thread;
	if (pthread_create(&thread, NULL, lookup_main, job) == 0) {
		pthread_detach(thread);
	} else {
		free(job->urls);
		free(job->indexes);
		free(job);
	}
}

static void lookup_stop(void) {
	pthread_mutex_lock(&lookup_lock);
	lookup_generation++;
	pthread_mutex_unlock(&lookup_lock);
}

// ---------------------------------------------------------------------------
// the feeds page
// ---------------------------------------------------------------------------

static void feed_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i < 0 || i >= feed_count || feeds[i].state != FEED_FOUND) {
		return;
	}
	feeds[i].checked = !feeds[i].checked;
	paint_feed(&feeds[i]);
	paint_totals();
}

static void make_feed_row(int i) {
	feed_t *f = &feeds[i];
	f->row = lv_btn_create(feeds_list);
	lv_obj_set_size(f->row, lv_pct(100), FEED_ROW_HEIGHT);
	lv_obj_add_style(f->row, &theme_style_card, 0);
	lv_obj_add_style(f->row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(f->row, 12, 0);
	lv_obj_set_style_border_width(f->row, 0, 0);
	lv_obj_set_style_shadow_width(f->row, 0, 0);
	lv_obj_set_style_pad_hor(f->row, 16, 0);
	lv_obj_set_style_pad_column(f->row, 14, 0);
	lv_obj_set_scrollable(f->row, false);
	lv_obj_set_flex_flow(f->row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(f->row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_event_bubble(f->row, true);
	lv_obj_add_event_cb(f->row, feed_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

	f->box = lv_obj_create(f->row);
	lv_obj_remove_style_all(f->box);
	lv_obj_set_size(f->box, 30, 30);
	lv_obj_set_style_radius(f->box, 8, 0);
	lv_obj_set_style_border_color(f->box, theme()->text_secondary, 0);
	lv_obj_set_style_border_width(f->box, 2, 0);
	lv_obj_set_scrollable(f->box, false);
	lv_obj_set_event_bubble(f->box, true);
	f->tick = lv_image_create(f->box);
	lv_image_set_src(f->tick, &icon_check_small);
	lv_obj_set_style_image_recolor(f->tick, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(f->tick, LV_OPA_COVER, 0);
	lv_obj_center(f->tick);
	lv_obj_set_event_bubble(f->tick, true);

	lv_obj_t *texts = lv_obj_create(f->row);
	lv_obj_remove_style_all(texts);
	lv_obj_set_flex_grow(texts, 1);
	lv_obj_set_height(texts, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_row(texts, 4, 0);
	lv_obj_set_event_bubble(texts, true);
	lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);

	lv_obj_t *title = lv_label_create(texts);
	lv_label_set_text(title, f->source.title);
	lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(title, lv_pct(100));
	lv_obj_set_height(title, lv_font_get_line_height(&font_ui_22));
	lv_obj_add_style(title, &theme_style_text, 0);
	lv_obj_set_style_text_font(title, &font_ui_22, 0);

	f->status = lv_label_create(texts);
	lv_label_set_long_mode(f->status, LV_LABEL_LONG_DOT);
	lv_obj_set_width(f->status, lv_pct(100));
	lv_obj_set_style_text_font(f->status, &font_ui_18, 0);

	paint_feed(f);
}

static void feeds_free(void) {
	lookup_stop();
	free(feeds);
	feeds = NULL;
	feed_count = 0;
	looked_at = 0;
}

static void feeds_fill(void) {
	lv_obj_clean(feeds_list);
	feeds_free();

	lv_label_set_text(feeds_file_label, file_name(chosen_file));
	char place[600];
	file_place(chosen_file, place, sizeof(place));

	opml_feed_t *read = calloc(OPML_MAX_FEEDS, sizeof(*read));
	int n = read ? opml_read(chosen_file, read, OPML_MAX_FEEDS) : -1;
	if (n > 0) {
		feeds = calloc((size_t)n, sizeof(*feeds));
	}
	if (n > 0 && feeds) {
		for (int i = 0; i < n; i++) {
			feeds[i].source = read[i];
			// Followed already by this address: no need to ask.
			feeds[i].state = podcastsubs_is_followed_url(read[i].url) ? FEED_ALREADY : FEED_LOOKING;
		}
		feed_count = n;
	}
	free(read);
	lv_label_set_text_fmt(feeds_count_label, tr("podcast_opml_place_count"), place, feed_count);

	bool online = podcast_configured() && network_up();
	if (n < 0 || feed_count == 0 || !online) {
		const char *why = n < 0 ? "podcast_opml_unreadable"
					   : feed_count == 0 ? "podcast_opml_no_feeds"
					   : !podcast_configured() ? "api_keys_unavailable"
											: "enable_wi_fi_first";
		lv_label_set_text(feeds_note, tr(why));
		lv_obj_set_hidden(feeds_note, false);
		lv_obj_set_hidden(feeds_status, true);
		lv_obj_set_hidden(follow_btn, true);
		return;
	}
	lv_obj_set_hidden(feeds_note, true);
	lv_obj_set_hidden(feeds_status, false);
	lv_obj_set_hidden(follow_btn, false);

	looked_at = 0;
	for (int i = 0; i < feed_count; i++) {
		make_feed_row(i);
		if (feeds[i].state != FEED_LOOKING) {
			looked_at++;
		}
	}
	// Room under the last row for the button floating over it.
	lv_obj_t *spacer = lv_obj_create(feeds_list);
	lv_obj_remove_style_all(spacer);
	lv_obj_set_size(spacer, 1, 70);
	paint_totals();
	lookup_start();
}

// ---------------------------------------------------------------------------
// following, and the report
// ---------------------------------------------------------------------------

// A card with a name and a number. Built here rather than with
// settingsrow_add(), whose rows are meant to live for the whole run: this list
// is rebuilt at every import.
static lv_obj_t *result_line(const char *name, int count, lv_color_t color, bool show_zero) {
	if (count == 0 && !show_zero) {
		return NULL;
	}
	lv_obj_t *row = lv_obj_create(result_list);
	lv_obj_set_size(row, lv_pct(100), 72);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_set_style_radius(row, 12, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 20, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_event_bubble(row, true);
	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text(label, tr(name));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_t *value = lv_label_create(row);
	lv_label_set_text_fmt(value, "%d", count);
	lv_obj_set_style_text_color(value, color, 0);
	lv_obj_set_style_text_font(value, &font_ui_22, 0);
	lv_obj_align(value, LV_ALIGN_RIGHT_MID, 0, 0);
	return row;
}

static void open_followed_cb(lv_event_t *e) {
	(void)e;
	podcastpage_open_followed();
}

static void follow_cb(lv_event_t *e) {
	(void)e;
	int checked = count_checked();
	if (checked <= 0) {
		return;
	}
	lookup_stop();
	podcast_feed_t *chosen = calloc((size_t)checked, sizeof(*chosen));
	if (!chosen) {
		toast_error(tr("podcast_opml_failed"));
		return;
	}
	int n = 0;
	int already = 0, missing = 0, failed = 0, skipped = 0;
	char missing_names[600] = "";
	for (int i = 0; i < feed_count; i++) {
		feed_t *f = &feeds[i];
		switch (f->state) {
		case FEED_FOUND:
			if (f->checked) {
				chosen[n] = f->found;
				// The address the user subscribed with, when the catalogue had
				// none of its own to give.
				if (!chosen[n].url[0]) {
					snprintf(chosen[n].url, sizeof(chosen[n].url), "%s", f->source.url);
				}
				n++;
			} else {
				skipped++;
			}
			break;
		case FEED_ALREADY:
			already++;
			break;
		case FEED_MISSING:
			missing++;
			if (strlen(missing_names) < 400) {
				size_t len = strlen(missing_names);
				snprintf(missing_names + len, sizeof(missing_names) - len, "%s%s", len ? ", " : "", f->source.title);
			}
			break;
		default:
			failed++;
			break;
		}
	}
	// The oldest first, so the list reads in the file's order once each lands on
	// top of the one before.
	for (int a = 0, b = n - 1; a < b; a++, b--) {
		podcast_feed_t t = chosen[a];
		chosen[a] = chosen[b];
		chosen[b] = t;
	}
	int added = podcastsubs_follow_batch(chosen, n);
	free(chosen);

	lv_obj_clean(result_list);
	lv_obj_t *head = lv_obj_create(result_list);
	lv_obj_remove_style_all(head);
	lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_ver(head, 10, 0);
	lv_obj_set_style_pad_row(head, 10, 0);
	lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_t *icon = lv_image_create(head);
	lv_image_set_src(icon, added > 0 ? &icon_circle_check : &icon_circle_alert);
	lv_obj_set_style_image_recolor(icon, added > 0 ? lv_color_hex(0x57c26a) : lv_color_hex(0xe8a33d), 0);
	lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
	lv_obj_t *big = lv_label_create(head);
	if (added == 1) {
		lv_label_set_text(big, tr("podcast_opml_followed_one"));
	} else {
		lv_label_set_text_fmt(big, tr("podcast_opml_followed_n"), added);
	}
	lv_obj_add_style(big, &theme_style_text, 0);
	lv_obj_set_style_text_font(big, &font_ui_28, 0);
	lv_obj_t *from = lv_label_create(head);
	lv_label_set_text_fmt(from, tr("podcast_opml_from_file"), file_name(chosen_file));
	lv_obj_add_style(from, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(from, &font_ui_20, 0);

	result_line("podcast_opml_result_already", already, theme()->text_secondary, false);
	lv_obj_t *missing_row = result_line("podcast_opml_result_missing", missing, lv_color_hex(0xe8a33d), false);
	if (missing_row && missing_names[0]) {
		lv_obj_t *names = lv_label_create(result_list);
		lv_label_set_text(names, missing_names);
		lv_label_set_long_mode(names, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(names, lv_pct(100));
		lv_obj_add_style(names, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(names, &font_ui_18, 0);
	}
	result_line("podcast_opml_result_failed", failed, lv_color_hex(0xff7b72), false);
	result_line("podcast_opml_result_skipped", skipped, theme()->text_secondary, false);
	if (added < n) {
		// The list was full: PODCASTSUBS_MAX.
		lv_obj_t *full = lv_label_create(result_list);
		lv_label_set_text_fmt(full, tr("podcast_opml_list_full"), n - added, PODCASTSUBS_MAX);
		lv_label_set_long_mode(full, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(full, lv_pct(100));
		lv_obj_add_style(full, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(full, &font_ui_18, 0);
	}
	lv_obj_t *open = lv_btn_create(result_list);
	lv_obj_set_size(open, lv_pct(100), 60);
	lv_obj_set_style_radius(open, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(open, 0, 0);
	lv_obj_set_style_border_width(open, 0, 0);
	lv_obj_add_style(open, &theme_style_accent_bg, 0);
	lv_obj_set_style_margin_top(open, 10, 0);
	lv_obj_add_event_cb(open, open_followed_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *open_label = lv_label_create(open);
	lv_label_set_text(open_label, tr("podcast_opml_open_followed"));
	lv_obj_set_style_text_font(open_label, &font_ui_22, 0);
	lv_obj_set_style_text_color(open_label, lv_color_white(), 0);
	lv_obj_center(open_label);

	switch_screen(result_screen);
}

// ---------------------------------------------------------------------------
// the files
// ---------------------------------------------------------------------------

static void open_file(const char *path) {
	snprintf(chosen_file, sizeof(chosen_file), "%s", path);
	switch_screen(feeds_screen);
}

static void file_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i >= 0 && i < file_count) {
		open_file(files[i]);
	}
}

static void files_fill(void) {
	lv_obj_clean(files_list);
	file_count = opml_find_files(storage_sd_root(), files, OPML_MAX_FILES);
	for (int i = 0; i < file_count; i++) {
		lv_obj_t *row = lv_btn_create(files_list);
		lv_obj_set_size(row, lv_pct(100), FEED_ROW_HEIGHT);
		lv_obj_add_style(row, &theme_style_card, 0);
		lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_radius(row, 12, 0);
		lv_obj_set_style_border_width(row, 0, 0);
		lv_obj_set_style_shadow_width(row, 0, 0);
		lv_obj_set_style_pad_hor(row, 16, 0);
		lv_obj_set_style_pad_column(row, 14, 0);
		lv_obj_set_scrollable(row, false);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_event_bubble(row, true);
		lv_obj_add_event_cb(row, file_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		lv_obj_t *icon = lv_image_create(row);
		lv_image_set_src(icon, &icon_file_down);
		lv_obj_add_style(icon, &theme_style_icon, 0);
		lv_obj_set_event_bubble(icon, true);

		lv_obj_t *texts = lv_obj_create(row);
		lv_obj_remove_style_all(texts);
		lv_obj_set_flex_grow(texts, 1);
		lv_obj_set_height(texts, LV_SIZE_CONTENT);
		lv_obj_set_style_pad_row(texts, 4, 0);
		lv_obj_set_event_bubble(texts, true);
		lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
		lv_obj_t *name = lv_label_create(texts);
		lv_label_set_text(name, file_name(files[i]));
		lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
		lv_obj_set_width(name, lv_pct(100));
		lv_obj_add_style(name, &theme_style_text, 0);
		lv_obj_set_style_text_font(name, &font_ui_22, 0);
		char place[600];
		file_place(files[i], place, sizeof(place));
		lv_obj_t *where = lv_label_create(texts);
		lv_label_set_text(where, place);
		lv_obj_add_style(where, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(where, &font_ui_18, 0);
	}
	lv_obj_set_hidden(files_empty, file_count != 0);
}

void podcastopml_open_import(void) {
	file_count = opml_find_files(storage_sd_root(), files, OPML_MAX_FILES);
	if (file_count == 1) {
		open_file(files[0]);
		return;
	}
	switch_screen(files_screen);
}

static void files_loaded_cb(lv_event_t *e) {
	(void)e;
	files_fill();
}

static void feeds_loaded_cb(lv_event_t *e) {
	(void)e;
	feeds_fill();
}

static void feeds_unloaded_cb(lv_event_t *e) {
	(void)e;
	// Going on to the report keeps nothing running: the answers are in.
	lookup_stop();
}

// ---------------------------------------------------------------------------
// export
// ---------------------------------------------------------------------------

// The followed podcasts are written as they are on the card. Those followed
// before Sonix kept their feed's address are asked of the catalogue first,
// when it can be reached, one request each, with a busy toast meanwhile.
static bool export_busy;

static void export_write(void) {
	const char *root = podcastdl_root();
	if (!root[0]) {
		toast_error(tr("sd_card_missing"));
		return;
	}
	int total = podcastsubs_count();
	if (total == 0) {
		confirm_notice("podcast_opml_export", "podcast_none_followed");
		return;
	}
	podcast_feed_t *subs = calloc((size_t)total, sizeof(*subs));
	opml_feed_t *out = calloc((size_t)total, sizeof(*out));
	if (!subs || !out) {
		free(subs);
		free(out);
		toast_error(tr("podcast_opml_failed"));
		return;
	}
	total = podcastsubs_list(subs, total);
	int n = 0;
	for (int i = 0; i < total; i++) {
		if (!subs[i].url[0]) {
			continue;
		}
		snprintf(out[n].title, sizeof(out[n].title), "%s", subs[i].title);
		snprintf(out[n].url, sizeof(out[n].url), "%s", subs[i].url);
		n++;
	}
	free(subs);

	mkdir(root, 0777);
	char path[600];
	snprintf(path, sizeof(path), "%s/%s", root, EXPORT_NAME);
	bool ok = n > 0 && opml_write(path, out, n);
	free(out);
	if (n == 0) {
		confirm_notice("podcast_opml_export", "podcast_opml_export_no_urls");
		return;
	}
	if (!ok) {
		toast_error(tr("card_write_failed"));
		return;
	}
	char message[400];
	if (n == total && n == 1) {
		snprintf(message, sizeof(message), tr("podcast_opml_exported_one"), PODCASTDL_FOLDER "/" EXPORT_NAME);
	} else if (n == total) {
		snprintf(message, sizeof(message), tr("podcast_opml_exported"), n, PODCASTDL_FOLDER "/" EXPORT_NAME);
	} else {
		snprintf(message, sizeof(message), tr("podcast_opml_exported_some"), n, PODCASTDL_FOLDER "/" EXPORT_NAME,
				 total - n);
	}
	confirm_notice("podcast_opml_export", message);
}

typedef struct {
	long long id;
	char url[PODCAST_FEED_URL_MAX];
} export_url_t;

static void export_url_cb(void *user) {
	export_url_t *u = user;
	podcastsubs_set_url(u->id, u->url);
	free(u);
}

static void export_done_cb(void *user) {
	(void)user;
	export_busy = false;
	toast_busy_end();
	export_write();
}

static void *export_lookup_main(void *arg) {
	long long *ids = arg;
	thread_be_background("opml export");
	for (int i = 0; ids[i] > 0; i++) {
		podcast_feed_t feed;
		if (!podcast_feed(ids[i], &feed) || !feed.url[0]) {
			continue;
		}
		export_url_t *u = calloc(1, sizeof(*u));
		if (!u) {
			break;
		}
		u->id = ids[i];
		snprintf(u->url, sizeof(u->url), "%s", feed.url);
		if (!gui_post(export_url_cb, u)) {
			free(u);
		}
	}
	free(ids);
	// After the addresses: the posts are taken in the order they were made.
	gui_post(export_done_cb, NULL);
	return NULL;
}

void podcastopml_export(void) {
	if (export_busy) {
		return;
	}
	if (podcastdl_root()[0] && podcastsubs_count() > 0 && podcast_configured() && network_up()) {
		long long *ids = calloc(PODCASTSUBS_MAX + 1, sizeof(*ids));
		if (ids && podcastsubs_missing_urls(ids, PODCASTSUBS_MAX) > 0) {
			pthread_t thread;
			if (pthread_create(&thread, NULL, export_lookup_main, ids) == 0) {
				pthread_detach(thread);
				export_busy = true;
				toast_busy("loading");
				return;
			}
		}
		free(ids);
	}
	export_write();
}

// ---------------------------------------------------------------------------
// building
// ---------------------------------------------------------------------------

static lv_obj_t *centred_note(lv_obj_t *parent) {
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_set_style_pad_top(label, 40, 0);
	return label;
}

void podcastopml_init(gui_config_t *cfg) {
	g_cfg = cfg;

	files_screen = lv_obj_create(NULL);
	files_list = settingsrow_page(files_screen, cfg, "podcast_opml_import");
	lv_obj_set_flex_align(files_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	files_empty = lv_label_create(files_screen);
	lv_label_set_text(files_empty, tr("podcast_opml_no_files"));
	lv_label_set_long_mode(files_empty, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(files_empty, cfg->screen_width - 4 * cfg->padding);
	lv_obj_set_style_text_align(files_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(files_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(files_empty, &font_ui_22, 0);
	lv_obj_align(files_empty, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(cfg) + 100);
	lv_obj_set_hidden(files_empty, true);
	switcher_attach_back_gesture(files_screen);
	lv_obj_add_event_cb(files_screen, files_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);

	feeds_screen = lv_obj_create(NULL);
	feeds_list = settingsrow_page(feeds_screen, cfg, "podcast_opml_import");
	feeds_title = settingsrow_page_title(feeds_screen);
	(void)feeds_title;
	lv_obj_set_flex_align(feeds_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	// The file, as a card above the list. Built in the screen rather than the
	// list, which is emptied at every fill.
	int top = settingsrow_content_top(cfg);
	lv_obj_t *chip = lv_obj_create(feeds_screen);
	lv_obj_set_size(chip, cfg->screen_width - 2 * cfg->padding, 74);
	lv_obj_align(chip, LV_ALIGN_TOP_LEFT, cfg->padding, top);
	lv_obj_add_style(chip, &theme_style_card, 0);
	lv_obj_set_style_radius(chip, 12, 0);
	lv_obj_set_style_border_width(chip, 0, 0);
	lv_obj_set_style_shadow_width(chip, 0, 0);
	lv_obj_set_style_pad_hor(chip, 16, 0);
	lv_obj_set_style_pad_column(chip, 12, 0);
	lv_obj_set_scrollable(chip, false);
	lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_t *chip_icon = lv_image_create(chip);
	lv_image_set_src(chip_icon, &icon_file_down);
	lv_obj_add_style(chip_icon, &theme_style_icon, 0);
	lv_obj_t *chip_texts = lv_obj_create(chip);
	lv_obj_remove_style_all(chip_texts);
	lv_obj_set_flex_grow(chip_texts, 1);
	lv_obj_set_height(chip_texts, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_row(chip_texts, 2, 0);
	lv_obj_set_flex_flow(chip_texts, LV_FLEX_FLOW_COLUMN);
	feeds_file_label = lv_label_create(chip_texts);
	lv_label_set_long_mode(feeds_file_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(feeds_file_label, lv_pct(100));
	lv_obj_add_style(feeds_file_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(feeds_file_label, &font_ui_22, 0);
	feeds_count_label = lv_label_create(chip_texts);
	lv_obj_add_style(feeds_count_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(feeds_count_label, &font_ui_18, 0);

	feeds_status = lv_label_create(feeds_screen);
	lv_obj_add_style(feeds_status, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(feeds_status, &font_ui_18, 0);
	lv_obj_align(feeds_status, LV_ALIGN_TOP_LEFT, cfg->padding + 4, top + 84);

	// The list starts under the file and the count.
	int list_top = top + 116;
	lv_obj_set_size(feeds_list, lv_pct(100), cfg->screen_height - list_top);
	lv_obj_align(feeds_list, LV_ALIGN_TOP_LEFT, 0, list_top);

	feeds_note = centred_note(feeds_screen);
	lv_obj_set_width(feeds_note, cfg->screen_width - 4 * cfg->padding);
	lv_obj_align(feeds_note, LV_ALIGN_TOP_MID, 0, top + 90);
	lv_obj_set_hidden(feeds_note, true);

	follow_btn = lv_btn_create(feeds_screen);
	lv_obj_set_size(follow_btn, cfg->screen_width - 2 * cfg->padding, 60);
	lv_obj_align(follow_btn, LV_ALIGN_BOTTOM_MID, 0, -cfg->padding);
	lv_obj_set_style_radius(follow_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(follow_btn, 0, 0);
	lv_obj_set_style_border_width(follow_btn, 0, 0);
	lv_obj_add_style(follow_btn, &theme_style_accent_bg, 0);
	lv_obj_add_event_cb(follow_btn, follow_cb, LV_EVENT_CLICKED, NULL);
	follow_label = lv_label_create(follow_btn);
	lv_label_set_text(follow_label, "");
	lv_obj_set_style_text_font(follow_label, &font_ui_22, 0);
	lv_obj_set_style_text_color(follow_label, lv_color_white(), 0);
	lv_obj_center(follow_label);

	switcher_attach_back_gesture(feeds_screen);
	lv_obj_add_event_cb(feeds_screen, feeds_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(feeds_screen, feeds_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	result_screen = lv_obj_create(NULL);
	result_list = settingsrow_page(result_screen, cfg, "podcast_opml_import");
	lv_obj_set_flex_align(result_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	switcher_attach_back_gesture(result_screen);
}
