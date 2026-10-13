#include "podcastcard.h"

#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/streaming/podcastpage.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/library/metadata.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/podcastdl.h"
#include "src/system/streaming/podcastsubs.h"

#define CARD_MAX_EPISODES 400
#define FEED_MAX_EPISODES PODCASTPAGE_FEED_MAX
#define HERO_COVER 112

// Below this an episode has not really been started (see podcaststate.c).
#define RESUME_MIN_SECONDS 5.0

lv_obj_t *podcastcard_screen;
lv_obj_t *podcastcard_feed_screen;

static gui_config_t *g_cfg;

// ---------------------------------------------------------------------------
// what the card says about an episode
// ---------------------------------------------------------------------------

static bool is_episode_name(const char *name) { return name[0] != '.' && playlist_is_playable_file(name); }

// Whether a directory entry is a regular file or a folder. The card's file
// system says so in the listing; only when it does not is the entry asked.
static bool entry_is(const struct dirent *de, const char *path, bool folder) {
	if (de->d_type == DT_REG) {
		return !folder;
	}
	if (de->d_type == DT_DIR) {
		return folder;
	}
	struct stat st;
	if (stat(path, &st) != 0) {
		return false;
	}
	return folder ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode);
}

static bool is_sidecar_name(const char *name) {
	size_t n = strlen(name);
	return name[0] != '.' && n > 5 && strcasecmp(name + n - 5, ".tags") == 0;
}

// A date at the start of a file name -- "2026-10-05 Episode", "2026.10.05",
// "20261005_" -- which is how podcast files copied by hand are usually named.
// 0 when there is none.
static time_t date_in_name(const char *name) {
	int y = 0, m = 0, d = 0;
	const char *p = name;
	for (int i = 0; i < 4; i++) {
		if (p[i] < '0' || p[i] > '9') {
			return 0;
		}
		y = y * 10 + (p[i] - '0');
	}
	p += 4;
	bool separated = *p == '-' || *p == '.' || *p == '_';
	if (separated) {
		p++;
	}
	for (int i = 0; i < 2; i++) {
		if (p[i] < '0' || p[i] > '9') {
			return 0;
		}
		m = m * 10 + (p[i] - '0');
	}
	p += 2;
	if (separated) {
		if (*p != '-' && *p != '.' && *p != '_') {
			return 0;
		}
		p++;
	}
	for (int i = 0; i < 2; i++) {
		if (p[i] < '0' || p[i] > '9') {
			return 0;
		}
		d = d * 10 + (p[i] - '0');
	}
	if (y < 1990 || y > 2100 || m < 1 || m > 12 || d < 1 || d > 31) {
		return 0;
	}
	struct tm when = {0};
	when.tm_year = y - 1900;
	when.tm_mon = m - 1;
	when.tm_mday = d;
	when.tm_hour = 12;
	return mktime(&when);
}

// The file name without its folder, its extension or a leading date: the title
// until the tags have been read.
static void name_title(const char *path, char *out, size_t size) {
	const char *slash = strrchr(path, '/');
	const char *name = slash ? slash + 1 : path;
	if (date_in_name(name) > 0) {
		const char *p = name + 8;
		while (*p == '-' || *p == '.' || *p == '_' || (*p >= '0' && *p <= '9')) {
			p++;
		}
		while (*p == ' ' || *p == '-' || *p == '_') {
			p++;
		}
		if (*p && *p != '.') {
			name = p;
		}
	}
	snprintf(out, size, "%.*s", (int)size - 1, name);
	char *dot = strrchr(out, '.');
	if (dot && dot != out) {
		*dot = '\0';
	}
}

// ---------------------------------------------------------------------------
// what has been heard: every row under the Podcast folder, read once per list
// ---------------------------------------------------------------------------

typedef struct {
	char *path;
	double position;
	double duration;
	bool played;
	long long touched;
} heard_t;

static heard_t *heard;
static int heard_count;
static int heard_capacity;

static void heard_free(void) {
	for (int i = 0; i < heard_count; i++) {
		free(heard[i].path);
	}
	heard_count = 0;
}

static void heard_add_cb(const podcaststate_t *state, void *user) {
	(void)user;
	if (!state->path[0]) {
		return;
	}
	if (heard_count == heard_capacity) {
		int grown = heard_capacity ? heard_capacity * 2 : 64;
		heard_t *bigger = realloc(heard, (size_t)grown * sizeof(*heard));
		if (!bigger) {
			return;
		}
		heard = bigger;
		heard_capacity = grown;
	}
	char *copy = strdup(state->path);
	if (!copy) {
		return;
	}
	heard[heard_count].path = copy;
	heard[heard_count].position = state->position;
	heard[heard_count].duration = state->duration;
	heard[heard_count].played = state->played;
	heard[heard_count].touched = state->touched;
	heard_count++;
}

static int heard_cmp(const void *a, const void *b) { return strcmp(((const heard_t *)a)->path, ((const heard_t *)b)->path); }

static void heard_read(const char *folder) {
	heard_free();
	podcaststate_in_folder(folder, heard_add_cb, NULL);
	qsort(heard, (size_t)heard_count, sizeof(*heard), heard_cmp);
}

static const heard_t *heard_find(const char *path) {
	if (heard_count == 0) {
		return NULL;
	}
	heard_t key = {(char *)path, 0, 0, false, 0};
	return bsearch(&key, heard, (size_t)heard_count, sizeof(*heard), heard_cmp);
}

// ---------------------------------------------------------------------------
// the podcasts on the card
// ---------------------------------------------------------------------------

static void count_file(const char *path, podcastcard_show_t *show) {
	show->episodes++;
	if (!show->first[0]) {
		snprintf(show->first, sizeof(show->first), "%.*s", (int)sizeof(show->first) - 1, path);
	}
	const heard_t *h = heard_find(path);
	if (!h || !h->played) {
		show->unheard++;
	}
	if (!h || (!h->played && h->position <= 0)) {
		show->fresh = true;
	}
}

static void count_folder(const char *folder, podcastcard_show_t *show) {
	DIR *dir = opendir(folder);
	if (!dir) {
		return;
	}
	// One pass over the listing: a download from Sonix has its ".tags" beside
	// it, and seeing that name is cheaper than asking after every file.
	char sidecar[900] = "";
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		char path[900];
		snprintf(path, sizeof(path), "%.600s/%.255s", folder, de->d_name);
		if (!sidecar[0] && is_sidecar_name(de->d_name)) {
			snprintf(sidecar, sizeof(sidecar), "%.*s", (int)(strlen(path) - 5), path);
			continue;
		}
		if (!is_episode_name(de->d_name) || !entry_is(de, path, false)) {
			continue;
		}
		count_file(path, show);
	}
	closedir(dir);
	if (sidecar[0]) {
		show->downloaded = true;
		char text[32];
		if (podcastcache_tag(sidecar, "feed_id", text, sizeof(text))) {
			show->feed_id = strtoll(text, NULL, 10);
		}
	}
}

static int show_cmp(const void *a, const void *b) {
	const podcastcard_show_t *x = a, *y = b;
	// The loose files after every podcast.
	if (!x->name[0] != !y->name[0]) {
		return x->name[0] ? -1 : 1;
	}
	return strcasecmp(x->name, y->name);
}

int podcastcard_shows(podcastcard_show_t *out, int max) {
	const char *root = podcastdl_root();
	if (!out || max <= 0 || !root[0]) {
		return 0;
	}
	heard_read(root);
	int count = 0;
	podcastcard_show_t loose;
	memset(&loose, 0, sizeof(loose));
	snprintf(loose.folder, sizeof(loose.folder), "%s", root);

	DIR *dir = opendir(root);
	if (!dir) {
		heard_free();
		return 0;
	}
	struct dirent *de;
	while ((de = readdir(dir)) != NULL && count < max) {
		if (de->d_name[0] == '.' || strlen(de->d_name) >= sizeof(out[0].name)) {
			continue;
		}
		char path[900];
		snprintf(path, sizeof(path), "%s/%s", root, de->d_name);
		if (is_episode_name(de->d_name) && entry_is(de, path, false)) {
			count_file(path, &loose);
			continue;
		}
		if (!entry_is(de, path, true)) {
			continue;
		}
		podcastcard_show_t *show = &out[count];
		memset(show, 0, sizeof(*show));
		snprintf(show->name, sizeof(show->name), "%s", de->d_name);
		snprintf(show->folder, sizeof(show->folder), "%.*s", (int)sizeof(show->folder) - 1, path);
		count_folder(path, show);
		if (show->episodes > 0) {
			count++;
		}
	}
	closedir(dir);
	if (loose.episodes > 0 && count < max) {
		out[count++] = loose;
	}
	heard_free();
	qsort(out, (size_t)count, sizeof(*out), show_cmp);
	return count;
}

const char *podcastcard_show_name(const podcastcard_show_t *show) {
	return show->name[0] ? show->name : tr("podcast_other_episodes");
}

int podcastcard_count_for_title(const char *title) {
	const char *root = podcastdl_root();
	if (!title || !title[0] || !root[0]) {
		return 0;
	}
	char name[256];
	podcastdl_safe_name(title, "Podcast", name, sizeof(name));
	char folder[600];
	snprintf(folder, sizeof(folder), "%s/%s", root, name);
	DIR *dir = opendir(folder);
	if (!dir) {
		return 0;
	}
	int n = 0;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (is_episode_name(de->d_name)) {
			n++;
		}
	}
	closedir(dir);
	return n;
}

// ---------------------------------------------------------------------------
// the episode playing
// ---------------------------------------------------------------------------

static char now_path[512];
static long long now_episode;

// The podcast episode loaded in the player, playing or paused. False when the
// player holds anything else, or nothing.
static bool now_read(bool *playing) {
	static device_state_t state;
	device_state_get(&state);
	if (state.live || state.status == AUDIO_STATUS_STOPPED || !podcastcache_is_episode(state.current_file)) {
		now_path[0] = '\0';
		now_episode = 0;
		return false;
	}
	if (strcmp(now_path, state.current_file) != 0) {
		snprintf(now_path, sizeof(now_path), "%s", state.current_file);
		now_episode = podcastcache_episode_id(now_path);
	}
	if (playing) {
		*playing = state.status == AUDIO_STATUS_PLAYING;
	}
	return true;
}

// Whether this episode -- by its file, or by its id in the catalogue -- is the
// one in the player.
static bool is_now(const char *path, long long episode_id) {
	if (!now_read(NULL)) {
		return false;
	}
	return (path && path[0] && strcmp(path, now_path) == 0) || (episode_id > 0 && episode_id == now_episode);
}

// To the player, without starting the episode again: tapping the one already
// in it would otherwise take it back to the second last written down.
// `go_on` also lets a paused one carry on.
static void show_player(bool go_on) {
	bool playing = false;
	if (go_on && now_read(&playing) && !playing) {
		player_key_play_pause();
	}
	player_refresh_now_playing();
	switch_screen(player_screen);
}

// ---------------------------------------------------------------------------
// one podcast's episodes
// ---------------------------------------------------------------------------

typedef struct {
	char *path;		  // on the card: the file. NULL for an episode of the catalogue
	char *title;	  // on the card: the title shown, which the titles thread fills in
	int item;		  // of the catalogue: its place in podcastpage_feed_episode(); -1 on the card
	long long id;	  // of the catalogue: the episode's id
	time_t when;
	double position;
	double duration;
	bool played;
	bool started;
	long long touched;
} episode_t;

typedef enum {
	FILTER_UNHEARD = 0,
	FILTER_ALL,
	FILTER_HEARD,
} filter_t;

#define EP_ROW_HEIGHT 96
#define EP_ROW_GAP 10
#define EP_ROW_PITCH (EP_ROW_HEIGHT + EP_ROW_GAP)
#define EP_ROW_POOL 10
#define STATUS_BOX 30
#define SEL_FAB_SIZE 72

typedef struct {
	lv_obj_t *button;
	lv_obj_t *dot;
	lv_obj_t *arc;
	lv_obj_t *check;
	lv_obj_t *title;
	lv_obj_t *detail;
	lv_obj_t *bar;
	lv_obj_t *menu_btn;
	lv_obj_t *mark; // while choosing, in the status's place: the tick on a chosen row,
	lv_obj_t *ring; // and an empty ring on the others
	int index; // into shown[]; -1 for none, -2 to force a rebind
} ep_row_t;

// One of the two pages: the same widgets and the same rules, filled from the
// card or from the catalogue.
typedef struct {
	bool feed;
	lv_obj_t *screen;
	lv_obj_t *page_title;
	lv_obj_t *page;
	lv_obj_t *hero_cover;
	lv_obj_t *hero_author; // the catalogue's: who makes it
	lv_obj_t *hero_counts; // how many episodes
	lv_obj_t *hero_left; // the card's: how many, or how long, still to hear
	lv_obj_t *resume_btn;
	lv_obj_t *online_row; // the card's: the same podcast in the catalogue
	lv_obj_t *filter_pills[3];
	lv_obj_t *body;
	lv_obj_t *more_spinner; // the catalogue's: the next step coming
	lv_obj_t *loader;		// the catalogue's: the first step coming
	lv_obj_t *empty_label;
	lv_obj_t *corner_btn;
	lv_obj_t *corner_icon; // sort on the card, follow on the catalogue
	// Choosing several episodes: a long press on a row starts it, a tap then
	// chooses a row or lets it go, and letting go of the last one ends it. The
	// round button in the corner of the screen acts on them -- delete on the
	// card, download on the catalogue -- and the cross closes the mode.
	bool selecting;
	uint8_t *chosen; // one per episode, by its place in `episodes`
	int chosen_count;
	lv_obj_t *sel_close_btn;
	lv_obj_t *sel_action_btn;
	cover_image_t hero_image;
	bool hero_waiting;
	ep_row_t rows[EP_ROW_POOL];
	episode_t *episodes;
	int capacity;
	int count;
	int *shown; // the episodes the filter lets through, in the order shown
	int shown_count;
	filter_t filter;
	int menu_shown;
	unsigned states_seen;
	// "Resume listening": the episode of this podcast left halfway most
	// recently.
	podcaststate_t resume;
	bool has_resume;
} page_t;

static episode_t card_episodes[CARD_MAX_EPISODES];
static int card_shown[CARD_MAX_EPISODES];
static uint8_t card_chosen[CARD_MAX_EPISODES];
static char card_titles[CARD_MAX_EPISODES][PODCAST_TITLE_MAX];
static episode_t feed_episodes[FEED_MAX_EPISODES];
static int feed_shown[FEED_MAX_EPISODES];
static uint8_t feed_chosen[FEED_MAX_EPISODES];

static page_t card_page = {
	.feed = false, .episodes = card_episodes, .capacity = CARD_MAX_EPISODES, .shown = card_shown, .chosen = card_chosen};
static page_t feed_page = {
	.feed = true, .episodes = feed_episodes, .capacity = FEED_MAX_EPISODES, .shown = feed_shown, .chosen = feed_chosen};

// The podcast each page is showing.
static podcastcard_show_t open_show;
static podcast_feed_t open_feed;

// The catalogue's side of the feed page: the request out, whether the list
// has been drawn, and whether there is more of it.
static unsigned feed_ticket;
static bool feed_loading;
static bool feed_loaded;
static bool feed_more;
static char feed_error[256];

// The page whose episode menu is open.
static page_t *menu_page;

// The titles come in from the tags on a thread of their own, a file at a time:
// reading four hundred files' tags before drawing anything would be seconds of
// a blank page. The lock covers the titles only; the rest of the array belongs
// to the GUI thread.
static pthread_mutex_t titles_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned titles_generation;

static lv_timer_t *hero_timer;

bool podcastcard_oldest_first(void) { return config_get_int("podcast", "card_oldest_first", 0) != 0; }

void podcastcard_set_oldest_first(bool on) {
	config_set_int("podcast", "card_oldest_first", on ? 1 : 0);
	config_save();
}

static const char *episode_title(const page_t *p, const episode_t *e) {
	if (!p->feed) {
		return e->title ? e->title : "";
	}
	const podcast_episode_t *item = podcastpage_feed_episode(e->item);
	return item ? item->title : "";
}

static void episodes_free(page_t *p) {
	pthread_mutex_lock(&titles_lock);
	if (!p->feed) {
		titles_generation++;
	}
	for (int i = 0; i < p->count; i++) {
		free(p->episodes[i].path);
		p->episodes[i].path = NULL;
	}
	p->count = 0;
	p->shown_count = 0;
	pthread_mutex_unlock(&titles_lock);
}

static int episode_cmp_new(const void *a, const void *b) {
	time_t x = ((const episode_t *)a)->when, y = ((const episode_t *)b)->when;
	if (x != y) {
		return x < y ? 1 : -1;
	}
	const char *pa = ((const episode_t *)a)->path, *pb = ((const episode_t *)b)->path;
	return strcasecmp(pb ? pb : "", pa ? pa : "");
}

static void take_heard(episode_t *e, const heard_t *h) {
	e->position = h ? h->position : 0;
	e->duration = h ? h->duration : 0;
	e->played = h && h->played;
	e->started = h && (h->position > 0 || h->played);
	e->touched = h ? h->touched : 0;
}

// The episodes in `folder`, newest first, with where each one stands from the
// rows heard_read() has just read. `titles`, when given, receives a title per
// episode from its file name.
static int folder_read(const char *folder, episode_t *out, int max, char (*titles)[PODCAST_TITLE_MAX]) {
	DIR *dir = opendir(folder);
	if (!dir) {
		return 0;
	}
	int n = 0;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL && n < max) {
		if (!is_episode_name(de->d_name)) {
			continue;
		}
		char path[900];
		snprintf(path, sizeof(path), "%.600s/%.255s", folder, de->d_name);
		// The listing says what the entry is and a date in its name says when
		// it is from: only a file that leaves either open is asked.
		time_t when = date_in_name(de->d_name);
		if (de->d_type != DT_REG || when <= 0) {
			struct stat st;
			if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
				continue;
			}
			if (when <= 0) {
				when = st.st_mtime;
			}
		}
		char *copy = strdup(path);
		if (!copy) {
			break;
		}
		episode_t *e = &out[n];
		memset(e, 0, sizeof(*e));
		e->path = copy;
		e->item = -1;
		e->when = when;
		if (titles) {
			e->title = titles[n];
			name_title(path, e->title, PODCAST_TITLE_MAX);
		}
		take_heard(e, heard_find(path));
		n++;
	}
	closedir(dir);
	// Newest first; the other way round is the same array read backwards.
	qsort(out, (size_t)n, sizeof(*out), episode_cmp_new);
	return n;
}

// The episode of the card page left halfway most recently.
static void card_find_resume(page_t *p) {
	p->has_resume = false;
	const episode_t *best = NULL;
	for (int i = 0; i < p->count; i++) {
		const episode_t *e = &p->episodes[i];
		if (!e->played && e->position > RESUME_MIN_SECONDS && (!best || e->touched > best->touched)) {
			best = e;
		}
	}
	if (best) {
		memset(&p->resume, 0, sizeof(p->resume));
		snprintf(p->resume.path, sizeof(p->resume.path), "%.*s", (int)sizeof(p->resume.path) - 1, best->path);
		p->resume.position = best->position;
		p->resume.duration = best->duration;
		p->resume.touched = best->touched;
		p->has_resume = true;
	}
}

static void card_read(void) {
	page_t *p = &card_page;
	episodes_free(p);
	heard_read(open_show.folder);
	pthread_mutex_lock(&titles_lock);
	p->count = folder_read(open_show.folder, p->episodes, p->capacity, card_titles);
	pthread_mutex_unlock(&titles_lock);
	heard_free();
	card_find_resume(p);
}

// Where each episode of the catalogue's list stands, and which was left
// halfway most recently -- also among those the list has not reached yet.
static void feed_state_cb(const podcaststate_t *state, void *user) {
	page_t *p = user;
	if (state->episode_id <= 0) {
		return;
	}
	if (!state->played && state->position > RESUME_MIN_SECONDS &&
		(!p->has_resume || state->touched > p->resume.touched)) {
		p->resume = *state;
		p->has_resume = true;
	}
	for (int i = 0; i < p->count; i++) {
		episode_t *e = &p->episodes[i];
		if (e->id == state->episode_id) {
			e->position = state->played ? 0 : state->position;
			if (state->duration > 0) {
				e->duration = state->duration;
			}
			e->played = state->played;
			e->started = state->played || state->position > 0;
			e->touched = state->touched;
			return;
		}
	}
}

static void feed_read_states(void) {
	page_t *p = &feed_page;
	p->has_resume = false;
	for (int i = 0; i < p->count; i++) {
		episode_t *e = &p->episodes[i];
		const podcast_episode_t *item = podcastpage_feed_episode(e->item);
		e->position = 0;
		e->duration = item ? item->duration_secs : 0;
		e->played = false;
		e->started = false;
		e->touched = 0;
	}
	podcaststate_for_feed(open_feed.id, feed_state_cb, p);
}

// The list the catalogue has just answered with, in place of the one drawn.
static void feed_take_list(void) {
	page_t *p = &feed_page;
	int n = podcastpage_feed_count();
	if (n > p->capacity) {
		n = p->capacity;
	}
	for (int i = 0; i < n; i++) {
		const podcast_episode_t *item = podcastpage_feed_episode(i);
		episode_t *e = &p->episodes[i];
		memset(e, 0, sizeof(*e));
		e->item = i;
		e->id = item ? item->id : 0;
		e->when = item ? item->published : 0;
	}
	p->count = n;
	feed_read_states();
}

static void apply_filter(page_t *p) {
	p->shown_count = 0;
	bool oldest = !p->feed && podcastcard_oldest_first();
	for (int k = 0; k < p->count; k++) {
		int i = oldest ? p->count - 1 - k : k;
		const episode_t *e = &p->episodes[i];
		if ((p->filter == FILTER_UNHEARD && e->played) || (p->filter == FILTER_HEARD && !e->played)) {
			continue;
		}
		p->shown[p->shown_count++] = i;
	}
}

static void window_update(page_t *p);
static void rows_invalidate(page_t *p);

static void titles_done_cb(void *user) {
	unsigned generation = (unsigned)(uintptr_t)user;
	if (generation == titles_generation && lv_screen_active() == podcastcard_screen) {
		// Same entries, new titles: every row is drawn again.
		rows_invalidate(&card_page);
		window_update(&card_page);
	}
}

typedef struct {
	unsigned generation;
	int count;
	char **paths; // copies: the array may be rebuilt meanwhile
} titles_job_t;

static void *titles_main(void *arg) {
	titles_job_t *job = arg;
	thread_be_background("podcast titles");
	song_metadata_t *m = malloc(sizeof(*m));
	for (int i = 0; i < job->count && m; i++) {
		pthread_mutex_lock(&titles_lock);
		bool stale = job->generation != titles_generation;
		pthread_mutex_unlock(&titles_lock);
		if (stale) {
			break;
		}
		char title[PODCAST_TITLE_MAX] = "";
		if (!podcastcache_tag(job->paths[i], "title", title, sizeof(title))) {
			metadata_read(job->paths[i], m);
			snprintf(title, sizeof(title), "%.*s", (int)sizeof(title) - 1, m->title);
		}
		if (!title[0]) {
			continue;
		}
		pthread_mutex_lock(&titles_lock);
		if (job->generation == titles_generation) {
			for (int k = 0; k < card_page.count; k++) {
				episode_t *e = &card_page.episodes[k];
				if (e->path && e->title && strcmp(e->path, job->paths[i]) == 0) {
					snprintf(e->title, PODCAST_TITLE_MAX, "%s", title);
					break;
				}
			}
		}
		pthread_mutex_unlock(&titles_lock);
		if ((i % 8) == 7) {
			gui_post(titles_done_cb, (void *)(uintptr_t)job->generation);
		}
	}
	free(m);
	gui_post(titles_done_cb, (void *)(uintptr_t)job->generation);
	for (int i = 0; i < job->count; i++) {
		free(job->paths[i]);
	}
	free(job->paths);
	free(job);
	return NULL;
}

static void titles_start(void) {
	page_t *p = &card_page;
	titles_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	job->paths = calloc((size_t)(p->count > 0 ? p->count : 1), sizeof(char *));
	if (!job->paths) {
		free(job);
		return;
	}
	// A reader still at work on an older list notices the generation has moved
	// and stops at its next file.
	pthread_mutex_lock(&titles_lock);
	job->generation = titles_generation;
	pthread_mutex_unlock(&titles_lock);
	// The order shown, so the rows on screen get their titles first.
	for (int k = 0; k < p->shown_count; k++) {
		job->paths[job->count] = strdup(p->episodes[p->shown[k]].path);
		if (job->paths[job->count]) {
			job->count++;
		}
	}
	for (int i = 0; i < p->count; i++) {
		bool listed = false;
		for (int k = 0; k < p->shown_count && !listed; k++) {
			listed = p->shown[k] == i;
		}
		if (!listed) {
			job->paths[job->count] = strdup(p->episodes[i].path);
			if (job->paths[job->count]) {
				job->count++;
			}
		}
	}
	pthread_t thread;
	if (pthread_create(&thread, NULL, titles_main, job) == 0) {
		pthread_detach(thread);
	} else {
		for (int i = 0; i < job->count; i++) {
			free(job->paths[i]);
		}
		free(job->paths);
		free(job);
	}
}

// ---------------------------------------------------------------------------
// formatting
// ---------------------------------------------------------------------------

static void format_date(time_t when, char *out, size_t size) {
	static const char *const MONTHS[12] = {"podcast_january", "podcast_february", "podcast_march",
										   "podcast_april",	  "podcast_may",	  "podcast_june",
										   "podcast_july",	  "podcast_august",	  "podcast_september",
										   "podcast_october", "podcast_november", "podcast_december"};
	struct tm tm_when;
	if (when <= 0 || !localtime_r(&when, &tm_when)) {
		out[0] = '\0';
		return;
	}
	snprintf(out, size, "%d %s %d", tm_when.tm_mday, tr(MONTHS[tm_when.tm_mon % 12]), tm_when.tm_year + 1900);
}

static void format_minutes(double seconds, char *out, size_t size) {
	int minutes = (int)((seconds + 30) / 60);
	if (minutes < 1) {
		minutes = 1;
	}
	if (minutes < 60) {
		snprintf(out, size, "%d min", minutes);
	} else {
		snprintf(out, size, "%d h %02d min", minutes / 60, minutes % 60);
	}
}

// The line under an episode: its date, then its length, the time left or
// that it has been heard.
static void episode_detail(const episode_t *e, char *out, size_t size) {
	char when[48];
	format_date(e->when, when, sizeof(when));
	char extra[64] = "";
	if (e->played) {
		snprintf(extra, sizeof(extra), "%s", tr("podcast_heard"));
	} else if (e->position > 0 && e->duration > e->position) {
		char left[32];
		format_minutes(e->duration - e->position, left, sizeof(left));
		snprintf(extra, sizeof(extra), tr("podcast_minutes_left"), left);
	} else if (e->duration > 0) {
		format_minutes(e->duration, extra, sizeof(extra));
	}
	if (when[0] && extra[0]) {
		snprintf(out, size, "%s \xC2\xB7 %s", when, extra);
	} else {
		snprintf(out, size, "%s%s", when, extra);
	}
}

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------

// While choosing, the episode's status gives way to a tick or an empty ring,
// on the left where the round button in the corner cannot cover it, and the
// three dots go.
static void row_paint_choice(page_t *p, ep_row_t *row) {
	int index = row->index;
	bool on = p->selecting && index >= 0 && index < p->shown_count && p->chosen[p->shown[index]];
	if (p->selecting) {
		lv_obj_set_hidden(row->dot, true);
		lv_obj_set_hidden(row->arc, true);
		lv_obj_set_hidden(row->check, true);
		lv_obj_set_style_image_recolor(row->mark, theme()->accent, 0);
		lv_obj_set_style_border_color(row->ring, theme()->text_primary, 0);
	}
	lv_obj_set_hidden(row->mark, !on);
	lv_obj_set_hidden(row->ring, !p->selecting || on);
	lv_obj_set_hidden(row->menu_btn, p->selecting);
}

static void selection_toggle(page_t *p, ep_row_t *row);
static void selection_stop(page_t *p);

static void row_bind(page_t *p, ep_row_t *row, int index) {
	if (row->index == index) {
		return;
	}
	row->index = index;
	if (index < 0 || index >= p->shown_count) {
		lv_obj_set_hidden(row->button, true);
		return;
	}
	const episode_t *e = &p->episodes[p->shown[index]];
	lv_obj_set_hidden(row->button, false);
	lv_obj_set_y(row->button, index * EP_ROW_PITCH);

	pthread_mutex_lock(&titles_lock);
	lv_label_set_text(row->title, episode_title(p, e));
	pthread_mutex_unlock(&titles_lock);

	char detail[160];
	episode_detail(e, detail, sizeof(detail));
	lv_label_set_text(row->detail, detail);

	bool partial = !e->played && e->position > 0 && e->duration > 0;
	lv_obj_set_hidden(row->dot, e->started);
	lv_obj_set_hidden(row->arc, !partial);
	lv_obj_set_hidden(row->check, !e->played);
	lv_obj_set_hidden(row->bar, !partial);
	if (partial) {
		int permille = (int)(e->position * 1000.0 / e->duration);
		if (permille > 1000) {
			permille = 1000;
		}
		lv_arc_set_value(row->arc, permille / 10);
		lv_bar_set_value(row->bar, permille, LV_ANIM_OFF);
	}
	// A heard episode is quieter than one waiting.
	lv_obj_set_style_text_opa(row->title, e->played ? LV_OPA_60 : LV_OPA_COVER, 0);
	row_paint_choice(p, row);
}

static void rows_invalidate(page_t *p) {
	for (int i = 0; i < EP_ROW_POOL; i++) {
		p->rows[i].index = -2;
	}
}

static void feed_request(int want, bool with_info);

// The next step of the catalogue's list, once the scroll nears its end.
static void feed_maybe_more(page_t *p) {
	if (!p->feed || !feed_more || feed_loading || lv_screen_active() != podcastcard_feed_screen) {
		return;
	}
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	if (lv_obj_get_scroll_bottom(p->page) > lv_obj_get_height(p->page)) {
		return;
	}
	feed_request(podcastpage_feed_want() + PODCASTPAGE_FEED_STEP, false);
}

static void window_update(page_t *p) {
	if (!p->page || !p->body) {
		return;
	}
	int scroll = lv_obj_get_scroll_y(p->page) - lv_obj_get_y(p->body);
	if (scroll < 0) {
		scroll = 0;
	}
	int first = scroll / EP_ROW_PITCH - 1;
	if (first + EP_ROW_POOL > p->shown_count) {
		first = p->shown_count - EP_ROW_POOL;
	}
	if (first < 0) {
		first = 0;
	}
	for (int i = 0; i < EP_ROW_POOL; i++) {
		int index = first + i;
		row_bind(p, &p->rows[index % EP_ROW_POOL], index < p->shown_count ? index : -1);
	}
}

static void page_scrolled_cb(lv_event_t *e) {
	page_t *p = lv_event_get_user_data(e);
	window_update(p);
	feed_maybe_more(p);
}

static int count_heard(const page_t *p, bool heard_ones) {
	int n = 0;
	for (int i = 0; i < p->count; i++) {
		if (p->episodes[i].played == heard_ones) {
			n++;
		}
	}
	return n;
}

static void paint_pills(page_t *p) {
	const char *const TAGS[3] = {"podcast_filter_unheard", "podcast_filter_all", "podcast_filter_heard"};
	const int counts[3] = {count_heard(p, false), p->count, count_heard(p, true)};
	for (int i = 0; i < 3; i++) {
		if (!p->filter_pills[i]) {
			continue;
		}
		lv_obj_t *label = lv_obj_get_child(p->filter_pills[i], 0);
		// The catalogue's list arrives a step at a time: a count would be the
		// count of what has arrived so far.
		if (p->feed) {
			lv_label_set_text(label, tr(TAGS[i]));
		} else {
			lv_label_set_text_fmt(label, "%s  %d", tr(TAGS[i]), counts[i]);
		}
		settingsrow_pill_active(p->filter_pills[i], (int)p->filter == i);
	}
}

static void refresh_counts_card(page_t *p) {
	double waiting = 0;
	bool all_known = true;
	for (int i = 0; i < p->count; i++) {
		const episode_t *e = &p->episodes[i];
		if (e->played) {
			continue;
		}
		if (e->duration > 0) {
			waiting += e->duration - e->position;
		} else {
			all_known = false;
		}
	}
	if (p->count == 1) {
		lv_label_set_text(p->hero_counts, tr("podcast_one_episode"));
	} else {
		lv_label_set_text_fmt(p->hero_counts, tr("podcast_episodes_2"), p->count);
	}
	// Under it, what is still to hear: how long when every length is known,
	// how many otherwise.
	int unheard = count_heard(p, false);
	char more[96] = "";
	if (unheard > 0 && all_known && waiting > 0) {
		char left[32];
		format_minutes(waiting, left, sizeof(left));
		snprintf(more, sizeof(more), tr("podcast_to_hear_time"), left);
	} else if (unheard > 0) {
		snprintf(more, sizeof(more), tr("podcast_to_hear_count"), unheard);
	}
	lv_label_set_text(p->hero_left, more);
	lv_obj_set_hidden(p->hero_left, !more[0]);
}

static void refresh_counts_feed(page_t *p) {
	int total = open_feed.episode_count > p->count ? open_feed.episode_count : p->count;
	if (total <= 0) {
		lv_label_set_text(p->hero_counts, "");
	} else if (total == 1) {
		lv_label_set_text(p->hero_counts, tr("podcast_one_episode"));
	} else {
		lv_label_set_text_fmt(p->hero_counts, tr("podcast_episodes_2"), total);
	}
}

static void refresh_hero(page_t *p) {
	if (p->feed) {
		refresh_counts_feed(p);
		lv_label_set_text(p->hero_author, open_feed.author);
		lv_obj_set_hidden(p->hero_author, !open_feed.author[0]);
	} else {
		refresh_counts_card(p);
		bool online = open_show.feed_id > 0 && podcast_configured();
		lv_obj_set_hidden(p->online_row, !online);
	}
	lv_obj_set_hidden(p->resume_btn, !p->has_resume);
}

static void refresh_corner(page_t *p) {
	if (p->feed) {
		bool followed = open_feed.id > 0 && podcastsubs_is_followed(open_feed.id);
		lv_image_set_src(p->corner_icon, followed ? &icon_star_filled : &icon_star);
	} else {
		lv_image_set_src(p->corner_icon, podcastcard_oldest_first() ? &icon_sort_date_old : &icon_sort_date_new);
	}
}

static void refill(page_t *p, bool keep_scroll) {
	apply_filter(p);
	paint_pills(p);
	refresh_hero(p);
	rows_invalidate(p);
	lv_obj_set_height(p->body, p->shown_count > 0 ? p->shown_count * EP_ROW_PITCH : 1);

	const char *empty = NULL;
	if (p->feed && p->count == 0) {
		empty = feed_loading ? NULL : feed_error[0] ? feed_error : feed_loaded ? tr("nothing_to_show") : NULL;
	} else if (p->shown_count == 0) {
		empty = tr(p->filter == FILTER_HEARD ? "podcast_none_heard" : "podcast_all_heard");
	}
	if (empty) {
		lv_label_set_text(p->empty_label, empty);
	}
	lv_obj_set_hidden(p->empty_label, empty == NULL);
	if (p->feed) {
		lv_obj_set_hidden(p->loader, !(feed_loading && p->count == 0));
		lv_obj_set_hidden(p->more_spinner, !(feed_loading && p->count > 0));
	}

	if (!keep_scroll) {
		lv_obj_scroll_to_y(p->page, 0, LV_ANIM_OFF);
	}
	lv_obj_update_layout(p->page);
	window_update(p);
	refresh_corner(p);
}

// ---------------------------------------------------------------------------
// the cover at the head
// ---------------------------------------------------------------------------

static void hero_placeholder(page_t *p) {
	lv_image_set_src(p->hero_cover, &icon_podcast_list);
	lv_obj_add_style(p->hero_cover, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(p->hero_cover, LV_OPA_COVER, 0);
}

static void hero_show(page_t *p, const cover_image_t *image) {
	// Off the old pixels before they go.
	hero_placeholder(p);
	cover_free(&p->hero_image);
	p->hero_image = *image;
	lv_obj_remove_style(p->hero_cover, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(p->hero_cover, LV_OPA_TRANSP, 0);
	lv_image_set_src(p->hero_cover, &p->hero_image.dsc);
}

static void hero_clear(page_t *p) {
	hero_placeholder(p);
	cover_free(&p->hero_image);
	p->hero_waiting = false;
}

static void hero_poll_cb(lv_timer_t *timer) {
	bool waiting = false;
	if (card_page.hero_waiting) {
		cover_image_t image;
		bool finished = false;
		if (coverloader_take(PODCASTCARD_SLOT_HERO, &image, &finished)) {
			hero_show(&card_page, &image);
			card_page.hero_waiting = false;
		} else if (finished) {
			card_page.hero_waiting = false;
		}
		waiting = waiting || card_page.hero_waiting;
	}
	if (feed_page.hero_waiting) {
		cover_image_t image;
		bool finished = false;
		if (podcastpage_hero_take(&image, &finished)) {
			hero_show(&feed_page, &image);
			feed_page.hero_waiting = false;
		} else if (finished) {
			feed_page.hero_waiting = false;
		}
		waiting = waiting || feed_page.hero_waiting;
	}
	if (!waiting) {
		lv_timer_pause(timer);
	}
}

static void hero_wake(void) {
	if (hero_timer) {
		lv_timer_resume(hero_timer);
	}
}

static void card_hero_load(void) {
	page_t *p = &card_page;
	hero_clear(p);
	coverloader_release(PODCASTCARD_SLOT_HERO);
	if (!open_show.first[0]) {
		return;
	}
	// The thumbnail already made costs a small read; only a first visit waits
	// for the decoder.
	cover_image_t image;
	if (cover_thumb_cached(open_show.first, HERO_COVER, &image)) {
		hero_show(p, &image);
		return;
	}
	coverloader_request(PODCASTCARD_SLOT_HERO, open_show.first, HERO_COVER);
	p->hero_waiting = true;
	hero_wake();
}

static void feed_hero_load(void) {
	page_t *p = &feed_page;
	hero_clear(p);
	podcastpage_hero_release();
	if (!open_feed.image[0]) {
		return;
	}
	podcastpage_hero_request(open_feed.image, HERO_COVER);
	p->hero_waiting = true;
	hero_wake();
}

// ---------------------------------------------------------------------------
// playing
// ---------------------------------------------------------------------------

static void play_paths(const char *const *paths, int count) {
	if (count <= 0) {
		return;
	}
	device_state_play_list_ordered(paths, count, 0);
	player_refresh_now_playing();
	switch_screen(player_screen);
}

static int page_row_index(page_t *p, lv_obj_t *button) {
	for (int i = 0; i < EP_ROW_POOL; i++) {
		if (p->rows[i].button == button) {
			return p->rows[i].index;
		}
	}
	return -1;
}

// From the row tapped, on down the list as it is shown.
static void row_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	page_t *p = lv_event_get_user_data(e);
	lv_obj_t *button = lv_event_get_current_target(e);
	if (p->selecting) {
		for (int i = 0; i < EP_ROW_POOL; i++) {
			if (p->rows[i].button == button) {
				selection_toggle(p, &p->rows[i]);
			}
		}
		return;
	}
	int index = page_row_index(p, button);
	if (index < 0 || index >= p->shown_count) {
		return;
	}
	episode_t *ep = &p->episodes[p->shown[index]];
	if (is_now(ep->path, ep->id)) {
		show_player(false);
		return;
	}
	if (p->feed) {
		podcastpage_feed_play(ep->item);
		return;
	}
	static const char *paths[CARD_MAX_EPISODES];
	int n = 0;
	for (int k = index; k < p->shown_count; k++) {
		paths[n++] = p->episodes[p->shown[k]].path;
	}
	play_paths(paths, n);
}

// The folder of an episode on the card, from it onwards, oldest first: what
// follows it is what comes after it in the podcast.
static episode_t play_scratch[CARD_MAX_EPISODES];

bool podcastcard_play(const podcaststate_t *state) {
	if (!state || !state->path[0] || access(state->path, F_OK) != 0) {
		return false;
	}
	if (is_now(state->path, state->episode_id)) {
		show_player(true);
		return true;
	}
	if (!podcastdl_owns(state->path)) {
		// In the cache: no folder to follow, only the episode itself.
		const char *one[1] = {state->path};
		play_paths(one, 1);
		return true;
	}
	char folder[600];
	snprintf(folder, sizeof(folder), "%.*s", (int)sizeof(folder) - 1, state->path);
	char *slash = strrchr(folder, '/');
	if (!slash) {
		return false;
	}
	*slash = '\0';
	heard_read(folder);
	int count = folder_read(folder, play_scratch, CARD_MAX_EPISODES, NULL);
	heard_free();
	static const char *paths[CARD_MAX_EPISODES];
	int n = 0;
	bool reached = false;
	for (int i = count - 1; i >= 0; i--) {
		if (strcmp(play_scratch[i].path, state->path) == 0) {
			reached = true;
		}
		if (reached && (n == 0 || !play_scratch[i].played)) {
			paths[n++] = play_scratch[i].path;
		}
	}
	if (n == 0) {
		paths[n++] = state->path;
	}
	play_paths(paths, n);
	for (int i = 0; i < count; i++) {
		free(play_scratch[i].path);
		play_scratch[i].path = NULL;
	}
	return true;
}

// "Resume listening": the episode of this podcast left halfway most recently,
// from where it was left.
static void resume_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	page_t *p = lv_event_get_user_data(e);
	if (!p->has_resume) {
		return;
	}
	podcaststate_t *st = &p->resume;
	if (is_now(st->path, st->episode_id)) {
		show_player(true);
		return;
	}
	if (!p->feed) {
		if (!podcastcard_play(st)) {
			toast_error(tr("podcast_that_episode_is_gone"));
		}
		return;
	}
	// Kept on the card: from there, with the network or without.
	if (st->path[0] && podcastdl_owns(st->path) && access(st->path, F_OK) == 0 && podcastcard_play(st)) {
		return;
	}
	// In the list drawn: as a tap on its row, with the episodes after it.
	for (int i = 0; i < p->count; i++) {
		if (p->episodes[i].id == st->episode_id) {
			podcastpage_feed_play(p->episodes[i].item);
			return;
		}
	}
	podcastpage_resume(st);
}

// ---------------------------------------------------------------------------
// an episode's menu
// ---------------------------------------------------------------------------

static episode_t *menu_episode(void) {
	page_t *p = menu_page;
	if (!p || p->menu_shown < 0 || p->menu_shown >= p->shown_count) {
		return NULL;
	}
	return &p->episodes[p->shown[p->menu_shown]];
}

static void mark_cb(void *user) {
	episode_t *e = menu_episode();
	if (!e) {
		return;
	}
	page_t *p = menu_page;
	bool heard_now = user != NULL;
	if (p->feed) {
		const podcast_episode_t *item = podcastpage_feed_episode(e->item);
		if (!item) {
			return;
		}
		podcaststate_set_played_episode(item, open_feed.image, heard_now);
	} else {
		podcaststate_set_played(e->path, heard_now);
	}
	e->played = heard_now;
	if (heard_now) {
		e->position = 0;
	}
	e->started = e->started || heard_now;
	p->states_seen = podcaststate_revision();
	if (p->feed) {
		feed_read_states();
	} else {
		card_find_resume(p);
	}
	refill(p, true);
}

static void play_next_cb(void *user) {
	(void)user;
	episode_t *e = menu_episode();
	if (!e) {
		return;
	}
	bool ok = menu_page->feed ? podcastpage_feed_play_next(e->item) : playlist_insert_next(e->path);
	if (ok) {
		device_state_queue_changed();
		toast_success(tr("added_to_the_queue"));
	} else {
		toast_error(tr("playlist_cannot_add_the_tracks"));
	}
}

static void download_cb(void *user) {
	(void)user;
	episode_t *e = menu_episode();
	if (!e || !menu_page->feed) {
		return;
	}
	podcastpage_feed_download(e->item, open_feed.title);
}

static void remove_folder_if_empty(const char *folder) {
	if (strcmp(folder, podcastdl_root()) == 0) {
		return; // the Podcast folder itself stays
	}
	DIR *dir = opendir(folder);
	if (!dir) {
		return;
	}
	bool any = false;
	struct dirent *de;
	while (!any && (de = readdir(dir)) != NULL) {
		any = is_episode_name(de->d_name);
	}
	closedir(dir);
	if (any) {
		return;
	}
	char cover[700];
	snprintf(cover, sizeof(cover), "%s/cover.jpg", folder);
	remove(cover);
	snprintf(cover, sizeof(cover), "%s/cover.png", folder);
	remove(cover);
	rmdir(folder);
}

static void delete_confirmed(void *user) {
	(void)user;
	episode_t *e = menu_episode();
	if (!e || menu_page->feed) {
		return;
	}
	page_t *p = menu_page;
	if (remove(e->path) != 0) {
		toast_error(tr("card_write_failed"));
		return;
	}
	char side[900];
	snprintf(side, sizeof(side), "%s.tags", e->path);
	remove(side);
	snprintf(side, sizeof(side), "%s.jpg", e->path);
	remove(side);
	podcaststate_forget(e->path);
	sync();
	remove_folder_if_empty(open_show.folder);
	p->menu_shown = -1;
	card_read();
	if (p->count == 0) {
		switch_screen_return_to(podcast_screen);
		return;
	}
	refill(p, true);
	titles_start();
}

static void delete_cb(void *user) {
	(void)user;
	episode_t *e = menu_episode();
	if (!e) {
		return;
	}
	confirm_show("podcast_delete_from_card", e->title, "delete", delete_confirmed, NULL);
}

static void open_menu(page_t *p, lv_obj_t *anchor, int index) {
	menu_page = p;
	p->menu_shown = index;
	episode_t *e = menu_episode();
	if (!e) {
		return;
	}
	// Separate tables for the two states rather than a label chosen with a
	// ternary: the string collector reads initialisers.
	static const popover_item_t CARD_UNHEARD[] = {
		{"podcast_mark_heard", mark_cb, (void *)1},
		{"podcast_play_next", play_next_cb, NULL},
		{"podcast_delete_from_card_short", delete_cb, NULL},
	};
	static const popover_item_t CARD_HEARD[] = {
		{"podcast_mark_unheard", mark_cb, NULL},
		{"podcast_play_next", play_next_cb, NULL},
		{"podcast_delete_from_card_short", delete_cb, NULL},
	};
	static const popover_item_t FEED_UNHEARD[] = {
		{"podcast_mark_heard", mark_cb, (void *)1},
		{"podcast_play_next", play_next_cb, NULL},
		{"podcast_download", download_cb, NULL},
	};
	static const popover_item_t FEED_HEARD[] = {
		{"podcast_mark_unheard", mark_cb, NULL},
		{"podcast_play_next", play_next_cb, NULL},
		{"podcast_download", download_cb, NULL},
	};
	const popover_item_t *items = p->feed ? (e->played ? FEED_HEARD : FEED_UNHEARD) : (e->played ? CARD_HEARD : CARD_UNHEARD);
	popover_show(anchor, items, 3);
}

static void row_menu_cb(lv_event_t *e) {
	page_t *p = lv_event_get_user_data(e);
	lv_obj_t *target = lv_event_get_current_target(e);
	for (int i = 0; i < EP_ROW_POOL; i++) {
		if (p->rows[i].menu_btn == target) {
			open_menu(p, target, p->rows[i].index);
			return;
		}
	}
}

// ---------------------------------------------------------------------------
// choosing several episodes
// ---------------------------------------------------------------------------

static void selection_show(page_t *p) {
	lv_obj_set_hidden(p->corner_btn, p->selecting);
	lv_obj_set_hidden(p->sel_close_btn, !p->selecting);
	lv_obj_set_hidden(p->sel_action_btn, !p->selecting);
	if (p->selecting) {
		lv_obj_move_foreground(p->sel_action_btn);
	}
	// Drawn again: leaving the mode puts each row's status back.
	rows_invalidate(p);
	window_update(p);
}

static void selection_stop(page_t *p) {
	if (!p->selecting) {
		return;
	}
	p->selecting = false;
	memset(p->chosen, 0, (size_t)p->capacity);
	p->chosen_count = 0;
	selection_show(p);
}

static void selection_start(page_t *p) {
	if (p->selecting) {
		return;
	}
	memset(p->chosen, 0, (size_t)p->capacity);
	p->chosen_count = 0;
	p->selecting = true;
	selection_show(p);
}

// Chooses the row, or lets it go. Letting go of the last one leaves the mode.
static void selection_toggle(page_t *p, ep_row_t *row) {
	int index = row->index;
	if (!p->selecting || index < 0 || index >= p->shown_count) {
		return;
	}
	int i = p->shown[index];
	p->chosen[i] = !p->chosen[i];
	if (p->chosen[i]) {
		p->chosen_count++;
	} else if (--p->chosen_count == 0) {
		selection_stop(p);
		return;
	}
	row_paint_choice(p, row);
}

// The chosen files, kept between the question and the answer: the selection
// is gone by then.
static char **delete_pending;
static int delete_pending_count;

static void delete_pending_free(void) {
	for (int i = 0; i < delete_pending_count; i++) {
		free(delete_pending[i]);
	}
	free(delete_pending);
	delete_pending = NULL;
	delete_pending_count = 0;
}

static void delete_many_confirmed(void *user) {
	(void)user;
	page_t *p = &card_page;
	int deleted = 0;
	for (int i = 0; i < delete_pending_count; i++) {
		const char *path = delete_pending[i];
		if (remove(path) != 0) {
			continue;
		}
		char side[900];
		snprintf(side, sizeof(side), "%s.tags", path);
		remove(side);
		snprintf(side, sizeof(side), "%s.jpg", path);
		remove(side);
		podcaststate_forget(path);
		deleted++;
	}
	delete_pending_free();
	if (deleted == 0) {
		toast_error(tr("card_write_failed"));
		return;
	}
	sync();
	remove_folder_if_empty(open_show.folder);
	p->menu_shown = -1;
	card_read();
	if (p->count == 0) {
		switch_screen_return_to(podcast_screen);
		return;
	}
	refill(p, true);
	titles_start();
}

static void sel_action_cb(lv_event_t *e) {
	page_t *p = lv_event_get_user_data(e);
	if (!p->selecting || p->chosen_count == 0) {
		return;
	}
	if (p->feed) {
		int *items = malloc((size_t)p->chosen_count * sizeof(*items));
		int n = 0;
		for (int i = 0; items && i < p->count && n < p->chosen_count; i++) {
			if (p->chosen[i]) {
				items[n++] = p->episodes[i].item;
			}
		}
		selection_stop(p);
		if (n > 0) {
			podcastpage_feed_download_many(items, n, open_feed.title);
		}
		free(items);
		return;
	}
	delete_pending_free();
	delete_pending = calloc((size_t)p->chosen_count, sizeof(*delete_pending));
	for (int i = 0; delete_pending && i < p->count && delete_pending_count < p->chosen_count; i++) {
		if (p->chosen[i] && p->episodes[i].path) {
			delete_pending[delete_pending_count] = strdup(p->episodes[i].path);
			if (delete_pending[delete_pending_count]) {
				delete_pending_count++;
			}
		}
	}
	selection_stop(p);
	if (delete_pending_count == 0) {
		delete_pending_free();
		return;
	}
	char message[96];
	if (delete_pending_count == 1) {
		snprintf(message, sizeof(message), "%s", tr("podcast_one_episode"));
	} else {
		snprintf(message, sizeof(message), tr("podcast_episodes_2"), delete_pending_count);
	}
	confirm_show("podcast_delete_episodes_question", message, "delete", delete_many_confirmed, NULL);
}

static void sel_close_cb(lv_event_t *e) { selection_stop(lv_event_get_user_data(e)); }

static void row_long_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	page_t *p = lv_event_get_user_data(e);
	lv_obj_t *target = lv_event_get_current_target(e);
	for (int i = 0; i < EP_ROW_POOL; i++) {
		if (p->rows[i].button == target) {
			// The lift that ends this press would otherwise arrive as a click
			// and start the episode.
			lv_indev_t *indev = lv_indev_active();
			if (indev) {
				lv_indev_wait_release(indev);
			}
			ep_row_t *row = &p->rows[i];
			if (row->index < 0 || row->index >= p->shown_count) {
				return;
			}
			selection_start(p);
			if (!p->chosen[p->shown[row->index]]) {
				selection_toggle(p, row);
			}
			return;
		}
	}
}

// ---------------------------------------------------------------------------
// the catalogue's list
// ---------------------------------------------------------------------------

static void feed_request(int want, bool with_info) {
	feed_ticket = podcastpage_feed_load(open_feed.id, want, with_info);
	feed_loading = true;
	feed_error[0] = '\0';
	if (feed_page.page) {
		lv_obj_set_hidden(feed_page.loader, feed_page.count > 0);
		lv_obj_set_hidden(feed_page.more_spinner, feed_page.count == 0);
		lv_obj_set_hidden(feed_page.empty_label, true);
	}
}

void podcastcard_feed_loaded(unsigned ticket, int count, const char *error, const podcast_feed_t *info) {
	if (ticket != feed_ticket) {
		return; // an answer about another podcast, or an older step
	}
	page_t *p = &feed_page;
	feed_loading = false;
	if (count < 0) {
		if (error && error[0]) {
			snprintf(feed_error, sizeof(feed_error), "%s", error);
			if (p->count > 0 && lv_screen_active() == podcastcard_feed_screen) {
				toast_error(error);
			}
		}
		// Without a reason the request was dropped for another job: asked again
		// when the page is next opened.
		refill(p, true);
		return;
	}
	if (info) {
		if (!open_feed.author[0]) {
			snprintf(open_feed.author, sizeof(open_feed.author), "%s", info->author);
		}
		if (!open_feed.url[0]) {
			snprintf(open_feed.url, sizeof(open_feed.url), "%s", info->url);
		}
		if (info->episode_count > 0) {
			open_feed.episode_count = info->episode_count;
		}
		if (!open_feed.title[0] && info->title[0]) {
			snprintf(open_feed.title, sizeof(open_feed.title), "%s", info->title);
			lv_label_set_text(p->page_title, open_feed.title);
		}
		if (!open_feed.image[0] && info->image[0]) {
			snprintf(open_feed.image, sizeof(open_feed.image), "%s", info->image);
			feed_hero_load();
		}
		podcastpage_set_browsing(&open_feed);
	}
	bool first = !feed_loaded;
	feed_loaded = true;
	feed_more = podcastpage_feed_more();
	feed_take_list();
	p->states_seen = podcaststate_revision();
	refill(p, !first);
}

void podcastcard_open_feed(const podcast_feed_t *feed) {
	if (!feed || feed->id <= 0) {
		return;
	}
	page_t *p = &feed_page;
	if (feed->id != open_feed.id || (!feed_loaded && !feed_loading)) {
		bool same = feed->id == open_feed.id;
		open_feed = *feed;
		if (!same) {
			selection_stop(p);
			episodes_free(p);
			p->filter = FILTER_ALL;
			p->menu_shown = -1;
			p->has_resume = false;
			feed_loaded = false;
			feed_more = false;
			feed_hero_load();
		}
		lv_label_set_text(p->page_title, open_feed.title[0] ? open_feed.title : tr("podcasts"));
		podcastpage_set_browsing(&open_feed);
		feed_request(PODCASTPAGE_FEED_STEP, !open_feed.author[0] || !open_feed.image[0]);
		refill(p, false);
	} else {
		// The same podcast again: whatever this record knows that the page
		// does not.
		if (!open_feed.author[0] && feed->author[0]) {
			snprintf(open_feed.author, sizeof(open_feed.author), "%s", feed->author);
		}
		if (!open_feed.image[0] && feed->image[0]) {
			snprintf(open_feed.image, sizeof(open_feed.image), "%s", feed->image);
			feed_hero_load();
		}
		if (feed->episode_count > 0) {
			open_feed.episode_count = feed->episode_count;
		}
		podcastpage_set_browsing(&open_feed);
		// Opened again, from its top.
		lv_obj_scroll_to_y(p->page, 0, LV_ANIM_OFF);
		window_update(p);
	}
	switch_screen(podcastcard_feed_screen);
}

// ---------------------------------------------------------------------------
// building
// ---------------------------------------------------------------------------

static void filter_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	lv_obj_t *pill = lv_event_get_current_target(e);
	page_t *p = lv_obj_get_parent(lv_obj_get_parent(pill)) == card_page.page ? &card_page : &feed_page;
	p->filter = (filter_t)(intptr_t)lv_event_get_user_data(e);
	refill(p, false);
	if (!p->feed) {
		titles_start();
	}
}

static void corner_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	page_t *p = lv_event_get_user_data(e);
	if (!p->feed) {
		podcastcard_set_oldest_first(!podcastcard_oldest_first());
		refill(p, false);
		return;
	}
	if (open_feed.id <= 0) {
		return;
	}
	if (podcastsubs_is_followed(open_feed.id)) {
		podcastsubs_unfollow(open_feed.id);
		gui_notify_popup(tr("podcast_no_longer_following"));
	} else {
		podcastsubs_follow(&open_feed);
		gui_notify_popup(tr("podcast_now_following"));
	}
	refresh_corner(p);
}

static void online_cb(lv_event_t *e) {
	(void)e;
	if (open_show.feed_id <= 0) {
		return;
	}
	podcast_feed_t feed;
	memset(&feed, 0, sizeof(feed));
	feed.id = open_show.feed_id;
	snprintf(feed.title, sizeof(feed.title), "%.*s", (int)sizeof(feed.title) - 1, podcastcard_show_name(&open_show));
	podcastcard_open_feed(&feed);
}

static void build_rows(page_t *p, int width) {
	for (int i = 0; i < EP_ROW_POOL; i++) {
		ep_row_t *row = &p->rows[i];
		row->button = lv_btn_create(p->body);
		lv_obj_set_size(row->button, width, EP_ROW_HEIGHT);
		lv_obj_add_style(row->button, &theme_style_card, 0);
		lv_obj_add_style(row->button, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_radius(row->button, 12, 0);
		lv_obj_set_style_border_width(row->button, 0, 0);
		lv_obj_set_style_shadow_width(row->button, 0, 0);
		lv_obj_set_style_pad_left(row->button, 12, 0);
		lv_obj_set_style_pad_right(row->button, 4, 0);
		lv_obj_set_style_pad_ver(row->button, 0, 0);
		lv_obj_set_style_pad_column(row->button, 10, 0);
		lv_obj_set_scrollable(row->button, false);
		lv_obj_set_hidden(row->button, true);
		lv_obj_set_flex_flow(row->button, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row->button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_event_bubble(row->button, true);
		lv_obj_add_event_cb(row->button, row_clicked_cb, LV_EVENT_CLICKED, p);
		lv_obj_add_event_cb(row->button, row_long_cb, LV_EVENT_LONG_PRESSED, p);

		// Where the episode stands: a dot never started, a ring part heard, a
		// tick heard to the end.
		lv_obj_t *status = lv_obj_create(row->button);
		lv_obj_remove_style_all(status);
		lv_obj_set_size(status, STATUS_BOX, STATUS_BOX);
		lv_obj_set_scrollable(status, false);
		lv_obj_set_event_bubble(status, true);

		row->dot = lv_obj_create(status);
		lv_obj_remove_style_all(row->dot);
		lv_obj_set_size(row->dot, 12, 12);
		lv_obj_set_style_radius(row->dot, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(row->dot, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(row->dot, theme()->accent, 0);
		lv_obj_center(row->dot);
		lv_obj_set_event_bubble(row->dot, true);

		row->arc = lv_arc_create(status);
		lv_obj_set_size(row->arc, 24, 24);
		lv_obj_center(row->arc);
		lv_arc_set_rotation(row->arc, 270);
		lv_arc_set_bg_angles(row->arc, 0, 360);
		lv_arc_set_range(row->arc, 0, 100);
		lv_obj_remove_style(row->arc, NULL, LV_PART_KNOB);
		lv_obj_set_style_arc_width(row->arc, 4, LV_PART_MAIN);
		lv_obj_set_style_arc_width(row->arc, 4, LV_PART_INDICATOR);
		lv_obj_set_style_arc_color(row->arc, theme()->surface_pressed, LV_PART_MAIN);
		lv_obj_set_style_arc_color(row->arc, theme()->accent, LV_PART_INDICATOR);
		lv_obj_set_style_pad_all(row->arc, 0, 0);
		lv_obj_set_clickable(row->arc, false);
		lv_obj_set_event_bubble(row->arc, true);

		row->ring = lv_obj_create(status);
		lv_obj_remove_style_all(row->ring);
		lv_obj_set_size(row->ring, 24, 24);
		lv_obj_set_style_radius(row->ring, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_border_width(row->ring, 2, 0);
		lv_obj_set_style_border_opa(row->ring, LV_OPA_50, 0);
		lv_obj_center(row->ring);
		lv_obj_set_event_bubble(row->ring, true);
		lv_obj_set_hidden(row->ring, true);

		row->mark = lv_image_create(status);
		lv_image_set_src(row->mark, &icon_check_small);
		lv_obj_set_style_image_recolor_opa(row->mark, LV_OPA_COVER, 0);
		lv_obj_center(row->mark);
		lv_obj_set_event_bubble(row->mark, true);
		lv_obj_set_hidden(row->mark, true);

		row->check = lv_image_create(status);
		lv_image_set_src(row->check, &icon_check_small);
		lv_obj_set_style_image_recolor(row->check, lv_color_hex(0x57c26a), 0);
		lv_obj_set_style_image_recolor_opa(row->check, LV_OPA_COVER, 0);
		lv_obj_center(row->check);
		lv_obj_set_event_bubble(row->check, true);

		lv_obj_t *texts = lv_obj_create(row->button);
		lv_obj_remove_style_all(texts);
		lv_obj_set_flex_grow(texts, 1);
		lv_obj_set_height(texts, LV_SIZE_CONTENT);
		lv_obj_set_style_pad_row(texts, 6, 0);
		lv_obj_set_scrollable(texts, false);
		lv_obj_set_event_bubble(texts, true);
		lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);

		row->title = lv_label_create(texts);
		lv_label_set_long_mode(row->title, LV_LABEL_LONG_DOT);
		lv_obj_set_width(row->title, lv_pct(100));
		lv_obj_set_height(row->title, lv_font_get_line_height(&font_ui_22));
		lv_obj_add_style(row->title, &theme_style_text, 0);
		lv_obj_set_style_text_font(row->title, &font_ui_22, 0);

		row->detail = lv_label_create(texts);
		lv_label_set_long_mode(row->detail, LV_LABEL_LONG_DOT);
		lv_obj_set_width(row->detail, lv_pct(100));
		lv_obj_set_height(row->detail, lv_font_get_line_height(&font_ui_18));
		lv_obj_add_style(row->detail, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(row->detail, &font_ui_18, 0);

		row->bar = lv_bar_create(texts);
		lv_obj_set_size(row->bar, lv_pct(100), 5);
		lv_bar_set_range(row->bar, 0, 1000);
		lv_obj_set_style_bg_color(row->bar, theme()->surface_pressed, LV_PART_MAIN);
		lv_obj_set_style_bg_opa(row->bar, LV_OPA_COVER, LV_PART_MAIN);
		lv_obj_set_style_bg_color(row->bar, theme()->accent, LV_PART_INDICATOR);
		lv_obj_set_style_radius(row->bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
		lv_obj_set_style_radius(row->bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
		lv_obj_set_event_bubble(row->bar, true);

		row->menu_btn = lv_btn_create(row->button);
		lv_obj_set_size(row->menu_btn, 44, 44);
		lv_obj_set_style_bg_opa(row->menu_btn, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(row->menu_btn, 0, 0);
		lv_obj_set_style_shadow_width(row->menu_btn, 0, 0);
		lv_obj_set_style_pad_all(row->menu_btn, 0, 0);
		lv_obj_add_event_cb(row->menu_btn, row_menu_cb, LV_EVENT_CLICKED, p);
		lv_obj_t *dots = lv_image_create(row->menu_btn);
		lv_image_set_src(dots, &icon_ellipsis_vertical);
		lv_obj_add_style(dots, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(dots, LV_OPA_COVER, 0);
		lv_obj_center(dots);


		row->index = -1;
	}
}

// The colours set by hand, again after a theme or accent change.
static void repaint_page(page_t *p) {
	for (int i = 0; i < EP_ROW_POOL; i++) {
		if (!p->rows[i].button) {
			continue;
		}
		lv_obj_set_style_bg_color(p->rows[i].dot, theme()->accent, 0);
		lv_obj_set_style_arc_color(p->rows[i].arc, theme()->surface_pressed, LV_PART_MAIN);
		lv_obj_set_style_arc_color(p->rows[i].arc, theme()->accent, LV_PART_INDICATOR);
		lv_obj_set_style_bg_color(p->rows[i].bar, theme()->surface_pressed, LV_PART_MAIN);
		lv_obj_set_style_bg_color(p->rows[i].bar, theme()->accent, LV_PART_INDICATOR);
	}
	if (p->resume_btn) {
		lv_obj_set_style_bg_color(p->resume_btn, theme()->accent, 0);
	}
	if (p->page) {
		paint_pills(p);
	}
}

static void repaint(void) {
	repaint_page(&card_page);
	repaint_page(&feed_page);
}

// The states again, after the player has moved one: the list itself, its
// order and its titles stay as they are.
static void states_refresh(page_t *p) {
	p->states_seen = podcaststate_revision();
	if (p->feed) {
		feed_read_states();
	} else {
		heard_read(open_show.folder);
		for (int i = 0; i < p->count; i++) {
			take_heard(&p->episodes[i], heard_find(p->episodes[i].path));
		}
		heard_free();
		card_find_resume(p);
	}
	refill(p, true);
}

// Once a second while a page is in sight: the player sits over it as a sheet,
// and closing the sheet does not load the page again.
static void states_watch_cb(lv_timer_t *timer) {
	(void)timer;
	lv_obj_t *active = lv_screen_active();
	page_t *p = active == podcastcard_screen ? &card_page : active == podcastcard_feed_screen ? &feed_page : NULL;
	if (!p || player_sheet_is_open() || p->count == 0) {
		return;
	}
	if (podcaststate_revision() != p->states_seen) {
		states_refresh(p);
	}
}

static void card_loaded_cb(lv_event_t *e) {
	(void)e;
	page_t *p = &card_page;
	selection_stop(p);
	p->states_seen = podcaststate_revision();
	// Read again on every visit: an episode heard in the player meanwhile has
	// moved on.
	card_read();
	if (p->count == 0) {
		switch_screen_return_to(podcast_screen);
		return;
	}
	refill(p, true);
	titles_start();
}

static void card_unloaded_cb(lv_event_t *e) {
	(void)e;
	selection_stop(&card_page);
	pthread_mutex_lock(&titles_lock);
	titles_generation++;
	pthread_mutex_unlock(&titles_lock);
}

static void feed_unloaded_cb(lv_event_t *e) {
	(void)e;
	selection_stop(&feed_page);
}

static void feed_loaded_cb(lv_event_t *e) {
	(void)e;
	page_t *p = &feed_page;
	if (!feed_loaded && !feed_loading && open_feed.id > 0) {
		feed_request(PODCASTPAGE_FEED_STEP, !open_feed.author[0] || !open_feed.image[0]);
		refill(p, false);
		return;
	}
	if (p->count > 0) {
		states_refresh(p);
	} else {
		refresh_corner(p);
	}
}

void podcastcard_open(const podcastcard_show_t *show) {
	if (!show) {
		return;
	}
	page_t *p = &card_page;
	bool same = strcmp(open_show.folder, show->folder) == 0;
	if (!same) {
		p->filter = FILTER_UNHEARD;
		lv_obj_scroll_to_y(p->page, 0, LV_ANIM_OFF);
	}
	bool same_cover = same && strcmp(open_show.first, show->first) == 0 && p->hero_image.pixels;
	open_show = *show;
	lv_label_set_text(p->page_title, podcastcard_show_name(show));
	if (!same_cover) {
		card_hero_load();
	}
	switch_screen(podcastcard_screen);
}

static lv_obj_t *make_filter_pill(page_t *p, lv_obj_t *parent, filter_t which) {
	(void)p;
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 48);
	lv_obj_set_style_pad_hor(btn, 16, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, filter_cb, LV_EVENT_CLICKED, (void *)(intptr_t)which);
	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, "");
	lv_obj_set_style_text_font(label, &font_ui_20, 0);
	lv_obj_center(label);
	return btn;
}

static void build_page(page_t *p, gui_config_t *cfg) {
	p->screen = lv_obj_create(NULL);
	lv_obj_add_style(p->screen, &theme_style_screen, 0);
	p->page_title = settingsrow_title(p->screen, cfg, "podcasts");
	settingsrow_title_corner_slots(p->page_title, cfg, 1);
	p->menu_shown = -1;

	// Sort on the card; follow on the catalogue.
	lv_obj_t *corner = lv_btn_create(p->screen);
	lv_obj_set_size(corner, 56, 56);
	lv_obj_set_style_bg_opa(corner, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(corner, 0, 0);
	lv_obj_set_style_shadow_width(corner, 0, 0);
	lv_obj_set_style_pad_all(corner, 0, 0);
	lv_obj_align(corner, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_add_event_cb(corner, corner_cb, LV_EVENT_CLICKED, p);
	p->corner_btn = corner;
	p->corner_icon = lv_image_create(corner);
	lv_image_set_src(p->corner_icon, p->feed ? &icon_star : &icon_sort_date_new);
	lv_obj_add_style(p->corner_icon, &theme_style_icon, 0);
	lv_obj_center(p->corner_icon);

	// In the same place while choosing: the cross that ends it.
	p->sel_close_btn = lv_btn_create(p->screen);
	lv_obj_set_size(p->sel_close_btn, 56, 56);
	lv_obj_set_style_bg_opa(p->sel_close_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(p->sel_close_btn, 0, 0);
	lv_obj_set_style_shadow_width(p->sel_close_btn, 0, 0);
	lv_obj_set_style_pad_all(p->sel_close_btn, 0, 0);
	lv_obj_align(p->sel_close_btn, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_add_event_cb(p->sel_close_btn, sel_close_cb, LV_EVENT_CLICKED, p);
	lv_obj_t *close_icon = lv_image_create(p->sel_close_btn);
	lv_image_set_src(close_icon, &icon_close);
	lv_obj_add_style(close_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(close_icon, LV_OPA_COVER, 0);
	lv_obj_center(close_icon);
	lv_obj_set_hidden(p->sel_close_btn, true);

	int top = settingsrow_content_top(cfg);
	int width = cfg->screen_width - 2 * cfg->padding;

	p->page = lv_obj_create(p->screen);
	lv_obj_set_size(p->page, lv_pct(100), cfg->screen_height - top);
	lv_obj_align(p->page, LV_ALIGN_TOP_LEFT, 0, top);
	lv_obj_set_style_bg_opa(p->page, 0, 0);
	lv_obj_set_style_border_width(p->page, 0, 0);
	lv_obj_set_style_radius(p->page, 0, 0);
	lv_obj_set_style_pad_hor(p->page, cfg->padding, 0);
	lv_obj_set_style_pad_top(p->page, 0, 0);
	lv_obj_set_style_pad_bottom(p->page, cfg->padding, 0);
	lv_obj_set_style_pad_gap(p->page, 14, 0);
	lv_obj_set_scroll_dir(p->page, LV_DIR_VER);
	lv_obj_set_flex_flow(p->page, LV_FLEX_FLOW_COLUMN);
	lv_obj_add_event_cb(p->page, page_scrolled_cb, LV_EVENT_SCROLL, p);

	// The cover, and what the podcast holds.
	lv_obj_t *hero = lv_obj_create(p->page);
	lv_obj_remove_style_all(hero);
	lv_obj_set_size(hero, width, HERO_COVER);
	lv_obj_set_style_pad_column(hero, 16, 0);
	lv_obj_set_scrollable(hero, false);
	lv_obj_set_event_bubble(hero, true);
	lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	p->hero_cover = lv_image_create(hero);
	lv_obj_set_size(p->hero_cover, HERO_COVER, HERO_COVER);
	lv_image_set_inner_align(p->hero_cover, LV_IMAGE_ALIGN_CENTER);
	lv_obj_set_style_radius(p->hero_cover, 10, 0);
	lv_obj_set_style_clip_corner(p->hero_cover, true, 0);
	lv_obj_set_event_bubble(p->hero_cover, true);
	hero_placeholder(p);

	lv_obj_t *texts = lv_obj_create(hero);
	lv_obj_remove_style_all(texts);
	lv_obj_set_flex_grow(texts, 1);
	lv_obj_set_height(texts, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_row(texts, 8, 0);
	lv_obj_set_scrollable(texts, false);
	lv_obj_set_event_bubble(texts, true);
	lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);

	if (p->feed) {
		p->hero_author = lv_label_create(texts);
		lv_label_set_long_mode(p->hero_author, LV_LABEL_LONG_DOT);
		lv_obj_set_width(p->hero_author, lv_pct(100));
		lv_obj_set_style_max_height(p->hero_author, 2 * lv_font_get_line_height(&font_ui_18), 0);
		lv_obj_add_style(p->hero_author, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(p->hero_author, &font_ui_18, 0);
		lv_label_set_text(p->hero_author, "");
	}

	p->hero_counts = lv_label_create(texts);
	lv_label_set_long_mode(p->hero_counts, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(p->hero_counts, lv_pct(100));
	lv_obj_add_style(p->hero_counts, &theme_style_text, 0);
	lv_obj_set_style_text_font(p->hero_counts, &font_ui_20, 0);
	lv_label_set_text(p->hero_counts, "");

	if (!p->feed) {
		p->hero_left = lv_label_create(texts);
		lv_label_set_long_mode(p->hero_left, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(p->hero_left, lv_pct(100));
		lv_obj_add_style(p->hero_left, &theme_style_text, 0);
		lv_obj_set_style_text_font(p->hero_left, &font_ui_20, 0);
		lv_label_set_text(p->hero_left, "");
		lv_obj_set_hidden(p->hero_left, true);
	}

	// The big button: the episode left halfway most recently.
	p->resume_btn = lv_btn_create(p->page);
	lv_obj_set_size(p->resume_btn, width, 56);
	lv_obj_set_style_radius(p->resume_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(p->resume_btn, 0, 0);
	lv_obj_set_style_border_width(p->resume_btn, 0, 0);
	lv_obj_set_style_bg_color(p->resume_btn, theme()->accent, 0);
	lv_obj_set_style_pad_column(p->resume_btn, 8, 0);
	lv_obj_set_flex_flow(p->resume_btn, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(p->resume_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_event_cb(p->resume_btn, resume_cb, LV_EVENT_CLICKED, p);
	lv_obj_t *play_icon = lv_image_create(p->resume_btn);
	lv_image_set_src(play_icon, &icon_play_small);
	lv_obj_set_style_image_recolor(play_icon, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(play_icon, LV_OPA_COVER, 0);
	lv_obj_t *play_label = lv_label_create(p->resume_btn);
	lv_label_set_text(play_label, tr("podcast_resume_listening"));
	lv_obj_set_style_text_font(play_label, &font_ui_22, 0);
	lv_obj_set_style_text_color(play_label, lv_color_white(), 0);
	lv_obj_set_hidden(p->resume_btn, true);

	if (!p->feed) {
		// The same podcast in the catalogue, when its downloads say which it is.
		p->online_row = settingsrow_add(p->page, "podcast_all_episodes_online", NULL, online_cb, NULL);
		lv_obj_set_width(p->online_row, width);
		lv_obj_set_hidden(p->online_row, true);
	}

	lv_obj_t *pills = lv_obj_create(p->page);
	lv_obj_remove_style_all(pills);
	lv_obj_set_size(pills, width, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_column(pills, 8, 0);
	lv_obj_set_event_bubble(pills, true);
	lv_obj_set_scroll_dir(pills, LV_DIR_HOR);
	lv_obj_set_scrollbar_mode(pills, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW);
	p->filter_pills[0] = make_filter_pill(p, pills, FILTER_UNHEARD);
	p->filter_pills[1] = make_filter_pill(p, pills, FILTER_ALL);
	p->filter_pills[2] = make_filter_pill(p, pills, FILTER_HEARD);

	// The rows slide over this, which is exactly as tall as the list.
	p->body = lv_obj_create(p->page);
	lv_obj_remove_style_all(p->body);
	lv_obj_set_size(p->body, width, 1);
	lv_obj_set_scrollable(p->body, false);
	lv_obj_set_event_bubble(p->body, true);
	build_rows(p, width);

	if (p->feed) {
		// Under the rows, in the middle.
		p->more_spinner = lv_obj_create(p->page);
		lv_obj_remove_style_all(p->more_spinner);
		lv_obj_set_size(p->more_spinner, width, LV_SIZE_CONTENT);
		lv_obj_set_event_bubble(p->more_spinner, true);
		lv_obj_set_flex_flow(p->more_spinner, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(p->more_spinner, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		spinner_create(p->more_spinner, &icon_loader_small);
		lv_obj_set_hidden(p->more_spinner, true);

		p->loader = spinner_create(p->screen, &icon_loader_big);
		lv_obj_align(p->loader, LV_ALIGN_TOP_MID, 0, top + HERO_COVER + 180);
		lv_obj_set_hidden(p->loader, true);
	}

	p->empty_label = lv_label_create(p->page);
	lv_label_set_long_mode(p->empty_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(p->empty_label, width);
	lv_obj_set_style_text_align(p->empty_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(p->empty_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(p->empty_label, &font_ui_22, 0);
	lv_obj_set_hidden(p->empty_label, true);

	// What acts on the chosen episodes, a disc in the bottom right corner as on
	// the library lists: red for deleting them from the card, the accent for
	// downloading them.
	p->sel_action_btn = lv_btn_create(p->screen);
	lv_obj_set_size(p->sel_action_btn, SEL_FAB_SIZE, SEL_FAB_SIZE);
	lv_obj_set_style_radius(p->sel_action_btn, LV_RADIUS_CIRCLE, 0);
	if (p->feed) {
		lv_obj_add_style(p->sel_action_btn, &theme_style_accent_bg, 0);
	} else {
		lv_obj_set_style_bg_color(p->sel_action_btn, lv_color_make(224, 27, 36), 0);
		lv_obj_set_style_bg_color(p->sel_action_btn, lv_color_make(192, 28, 40), LV_STATE_PRESSED);
	}
	lv_obj_set_style_bg_opa(p->sel_action_btn, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(p->sel_action_btn, 0, 0);
	lv_obj_set_style_shadow_width(p->sel_action_btn, 0, 0);
	lv_obj_set_style_pad_all(p->sel_action_btn, 0, 0);
	lv_obj_align(p->sel_action_btn, LV_ALIGN_BOTTOM_RIGHT, -cfg->padding - 6, -cfg->padding - 6);
	lv_obj_add_event_cb(p->sel_action_btn, sel_action_cb, LV_EVENT_CLICKED, p);
	lv_obj_t *action_icon = lv_image_create(p->sel_action_btn);
	lv_image_set_src(action_icon, p->feed ? &icon_download : &icon_trash);
	lv_obj_set_style_image_recolor(action_icon, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(action_icon, LV_OPA_COVER, 0);
	lv_obj_center(action_icon);
	lv_obj_set_hidden(p->sel_action_btn, true);

	switcher_attach_back_gesture(p->screen);
	switcher_attach_back_gesture(p->page);
	player_sheet_attach_drag(p->page, true);
	player_sheet_attach_drag(p->screen, true);
}

void podcastcard_init(gui_config_t *cfg) {
	g_cfg = cfg;
	build_page(&card_page, cfg);
	podcastcard_screen = card_page.screen;
	lv_obj_add_event_cb(podcastcard_screen, card_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(podcastcard_screen, card_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	build_page(&feed_page, cfg);
	podcastcard_feed_screen = feed_page.screen;
	lv_obj_add_event_cb(podcastcard_feed_screen, feed_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(podcastcard_feed_screen, feed_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	theme_register_refresh(repaint);
	lv_timer_create(states_watch_cb, 1000, NULL);
	hero_timer = lv_timer_create(hero_poll_cb, 100, NULL);
	lv_timer_pause(hero_timer);
}
