#include "bootlogopage.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/bootlogo.h"

#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

lv_obj_t *bootlogopage_screen;

// The folder on the microSD the pictures are listed from.
#define FOLDER_NAME "Bootlogo"

// Rows built once at startup and reused; pictures past this many are not
// listed.
#define MAX_PICTURES 48
#define NAME_MAX_LEN 256

// The same green the language page marks its choice with.
#define CHECK_GREEN lv_color_make(46, 194, 126)

static int logo_w, logo_h;
static const char *card_root;

static lv_obj_t *default_check;
static lv_obj_t *rows[MAX_PICTURES];
static lv_obj_t *checks[MAX_PICTURES];
static char names[MAX_PICTURES][NAME_MAX_LEN];
static int count;
static lv_obj_t *empty_label;

// True while a picture is being turned into the logo; other picks wait.
static bool busy;

typedef struct {
	char path[PATH_MAX];
	char name[NAME_MAX_LEN];
	bool ok;
} job_t;

static bool is_picture(const char *name) {
	return name[0] != '.' && (has_extension(name, ".jpg") || has_extension(name, ".jpeg") ||
							  has_extension(name, ".png"));
}

static bool folder_path(char *out, size_t size) {
	return card_root && card_root[0] && (size_t)snprintf(out, size, "%s/%s", card_root, FOLDER_NAME) < size;
}

static int compare_names(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

static void scan(void) {
	count = 0;
	char dir[PATH_MAX];
	DIR *d = folder_path(dir, sizeof(dir)) ? opendir(dir) : NULL;
	if (!d) {
		return;
	}
	struct dirent *e;
	while (count < MAX_PICTURES && (e = readdir(d)) != NULL) {
		if (is_picture(e->d_name) && strlen(e->d_name) < NAME_MAX_LEN) {
			snprintf(names[count], NAME_MAX_LEN, "%s", e->d_name);
			count++;
		}
	}
	closedir(d);
	qsort(names, (size_t)count, NAME_MAX_LEN, compare_names);
}

static void paint_checks(void) {
	bool custom = bootlogo_has_custom();
	const char *chosen = config_get("ui", "bootlogo_file", "");
	lv_obj_set_hidden(default_check, custom);
	for (int i = 0; i < count; i++) {
		lv_obj_set_hidden(checks[i], !custom || strcmp(names[i], chosen) != 0);
	}
}

static void refresh(void) {
	scan();
	for (int i = 0; i < MAX_PICTURES; i++) {
		if (i < count) {
			lv_label_set_text(settingsrow_name_label(rows[i]), names[i]);
		}
		lv_obj_set_hidden(rows[i], i >= count);
	}
	lv_obj_set_hidden(empty_label, count > 0);
	paint_checks();
}

static void job_done(void *user) {
	job_t *job = user;
	busy = false;
	toast_busy_end();
	if (job->ok) {
		config_set("ui", "bootlogo_file", job->name);
		config_save();
		toast_success("bootlogo_changed");
	} else {
		toast_error("bootlogo_failed");
	}
	paint_checks();
	free(job);
}

static void *job_main(void *arg) {
	job_t *job = arg;
	thread_be_low_priority("boot logo");
	job->ok = bootlogo_make_custom(job->path, logo_w, logo_h);
	// The page stays busy until the answer arrives, so it has to arrive.
	while (!gui_post(job_done, job)) {
		usleep(50 * 1000);
	}
	return NULL;
}

static void pick_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (busy || switcher_back_drag_active() || index < 0 || index >= count) {
		return;
	}

	job_t *job = calloc(1, sizeof(*job));
	char dir[PATH_MAX];
	if (!job || !folder_path(dir, sizeof(dir)) ||
		(size_t)snprintf(job->path, sizeof(job->path), "%s/%s", dir, names[index]) >= sizeof(job->path)) {
		free(job);
		toast_error("bootlogo_failed");
		return;
	}
	snprintf(job->name, sizeof(job->name), "%s", names[index]);

	pthread_t thread;
	if (pthread_create(&thread, NULL, job_main, job) != 0) {
		free(job);
		toast_error("bootlogo_failed");
		return;
	}
	pthread_detach(thread);
	busy = true;
	toast_busy("bootlogo_working");
}

static void default_cb(lv_event_t *e) {
	(void)e;
	if (busy || switcher_back_drag_active() || !bootlogo_has_custom()) {
		return;
	}
	bootlogo_clear_custom();
	config_set("ui", "bootlogo_file", "");
	config_save();
	paint_checks();
}

static lv_obj_t *add_check(lv_obj_t *row) {
	lv_obj_t *mark = lv_image_create(row);
	lv_image_set_src(mark, &icon_check);
	lv_obj_set_style_image_recolor(mark, CHECK_GREEN, 0);
	lv_obj_set_style_image_recolor_opa(mark, LV_OPA_COVER, 0);
	lv_obj_align(mark, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_hidden(mark, true);
	return mark;
}

static lv_obj_t *add_note(lv_obj_t *parent, const char *tag) {
	lv_obj_t *note = lv_label_create(parent);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr(tag));
	return note;
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	refresh();
}

void bootlogopage_init(gui_config_t *cfg) {
	logo_w = cfg->screen_width;
	logo_h = cfg->screen_height;
	card_root = cfg->sd_root_path;

	bootlogopage_screen = lv_obj_create(NULL);
	lv_obj_add_style(bootlogopage_screen, &theme_style_screen, 0);

	lv_obj_t *container = settingsrow_page(bootlogopage_screen, cfg, "bootlogo");

	lv_obj_t *default_row = settingsrow_action(container, "bootlogo_default", default_cb, NULL);
	default_check = add_check(default_row);
	add_note(container, "bootlogo_note");

	for (int i = 0; i < MAX_PICTURES; i++) {
		rows[i] = settingsrow_action(container, "", pick_cb, (void *)(intptr_t)i);
		checks[i] = add_check(rows[i]);
		settingsrow_name_lines(rows[i], 1);
		lv_obj_set_hidden(rows[i], true);
	}
	empty_label = add_note(container, "bootlogo_empty");

	lv_obj_add_event_cb(bootlogopage_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(bootlogopage_screen);
}
