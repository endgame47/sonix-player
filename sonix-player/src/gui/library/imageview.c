#include "imageview.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

lv_obj_t *imageview_screen;

static lv_obj_t *title_label;
static lv_obj_t *picture;
static lv_obj_t *busy;
static lv_obj_t *failed; // the file is not a picture this player can decode
static int box_w, box_h;

static cover_image_t shown; // what `picture` draws
static unsigned generation; // a decode for a picture no longer wanted is thrown away

typedef struct {
	unsigned generation;
	char path[1024];
	int w, h;
	cover_image_t image;
	bool ok;
} decode_job_t;

static void drop_picture(void) {
	lv_image_set_src(picture, NULL);
	cover_free(&shown);
}

static void decoded(void *user) {
	decode_job_t *job = user;
	if (job->generation != generation) {
		cover_free(&job->image);
		free(job);
		return;
	}
	lv_obj_set_hidden(busy, true);
	if (!job->ok) {
		lv_obj_set_hidden(failed, false);
		free(job);
		return;
	}
	drop_picture();
	shown = job->image;
	lv_image_set_src(picture, &shown.dsc);
	lv_obj_set_hidden(picture, false);
	free(job);
}

static void *decode_worker(void *arg) {
	decode_job_t *job = arg;
	job->ok = cover_load_image_file(job->path, job->w, job->h, COVER_FIT_CONTAIN, &job->image);
	if (!gui_post(decoded, job)) {
		cover_free(&job->image);
		free(job);
	}
	return NULL;
}

void imageview_open(const char *path) {
	generation++;
	drop_picture();
	lv_obj_set_hidden(picture, true);
	lv_obj_set_hidden(failed, true);
	lv_obj_set_hidden(busy, false);

	const char *slash = strrchr(path, '/');
	lv_label_set_text(title_label, slash ? slash + 1 : path);
	switch_screen(imageview_screen);

	decode_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	job->generation = generation;
	job->w = box_w;
	job->h = box_h;
	snprintf(job->path, sizeof(job->path), "%s", path);
	pthread_t thread;
	if (pthread_create(&thread, NULL, decode_worker, job) != 0) {
		free(job);
		return;
	}
	pthread_detach(thread);
}

// Leaving the page lets the picture go: a photo the size of the screen is the
// better part of a megabyte, and nothing else needs it.
static void unloaded_cb(lv_event_t *e) {
	(void)e;
	generation++;
	drop_picture();
}

void imageview_init(gui_config_t *cfg) {
	lv_obj_add_style(imageview_screen, &theme_style_screen, 0);
	title_label = settingsrow_title(imageview_screen, cfg, "");
	settingsrow_title_corner_slots(title_label, cfg, 0);

	int content_top = settingsrow_content_top(cfg);
	box_w = cfg->screen_width - 2 * cfg->padding;
	box_h = cfg->screen_height - content_top - 2 * cfg->padding;

	lv_obj_t *area = lv_obj_create(imageview_screen);
	lv_obj_remove_style_all(area);
	lv_obj_set_size(area, cfg->screen_width, cfg->screen_height - content_top);
	lv_obj_align(area, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_scrollable(area, false);
	lv_obj_set_event_bubble(area, true);

	picture = lv_image_create(area);
	lv_obj_center(picture);
	lv_obj_set_event_bubble(picture, true);
	lv_obj_set_hidden(picture, true);

	busy = spinner_create(area, &icon_loader_big);
	lv_obj_center(busy);
	lv_obj_set_hidden(busy, true);

	failed = lv_label_create(area);
	lv_label_set_text(failed, tr("files_image_unreadable"));
	lv_label_set_long_mode(failed, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(failed, box_w);
	lv_obj_set_style_text_align(failed, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(failed, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(failed, &font_ui_22, 0);
	lv_obj_center(failed);
	lv_obj_set_hidden(failed, true);

	lv_obj_add_event_cb(imageview_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(imageview_screen);
}
