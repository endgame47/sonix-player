#include "podcastpage.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <malloc.h>
#include <unistd.h>

#include "src/gui/fonts/fonts.h"
#include "src/system/playback/sleeptimer.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/toast.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/streaming/podcastcard.h"
#include "src/gui/streaming/podcastopml.h"
#include "src/gui/streaming/qobuzart.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/decode/growfile.h"
#include "src/system/playback/device_state.h"
#include "src/system/core/lang.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/podcastdl.h"
#include "src/system/streaming/podcaststate.h"
#include "src/system/streaming/podcastsubs.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/device/power.h"
#include "src/system/device/system.h"
#include "src/system/core/utils.h"
#include "src/system/net/wifi.h"
#include "src/system/net/wifitransfer.h"

// ---------------------------------------------------------------------------
// Podcasts, up front.
//
// The same shape as the Qobuz and Tidal pages, deliberately: opening all three
// must not mean learning three interfaces. The same ground rule therefore
// holds -- everything that talks to the network BLOCKS, so it cannot run on the
// GUI thread: one worker, one slot for the job to do, and results handed back
// with gui_post().
//
// Three things differ here, and every odd choice follows from them:
//
//   * there is no sign-in. Podcast Index is an open catalogue: search and read,
//     that is all. No login page, no token to renew, and "my podcasts" is a
//     list kept on the card (podcastsubs.h) rather than something living on the
//     service;
//   * nothing is fetched ahead. Qobuz and Tidal download the NEXT track while
//     the current one plays, because a track is five megabytes. An episode is
//     sixty and runs an hour: fetching ahead would fill the card with material
//     that may never be heard. What is needed is fetched when it is needed;
//   * episodes come in reverse order. The catalogue lists newest first, which
//     is the order to see them in but not the order to hear them in, so the
//     queue built from a tap holds the chosen episode and the ones AFTER it in
//     the list, that is, going back in time.
// ---------------------------------------------------------------------------

#define PODCAST_LIMIT PODCAST_PAGE_LIMIT
// How many entries are held at most. The catalogue has no offset, so "another
// page" here means asking for the same list with a higher limit.
//
// A podcast's episodes go up to the catalogue's own maximum per request, 1000.
// Search results and the chart stop at four pages, like the radio stations.
// The arrays are static and sized for the larger of the two; their pages only
// take memory once entries are written into them.
#define PODCAST_MAX_HELD PODCASTPAGE_FEED_MAX
#define PODCAST_FEEDS_MAX (4 * PODCAST_LIMIT)

// Episodes are asked for in steps of this many: the first step when a podcast
// is opened, another each time the scroll nears the bottom. Each step repeats
// the whole request with the higher limit.
#define PODCAST_EPISODES_STEP PODCASTPAGE_FEED_STEP

// The longest queue a tapped episode builds: it and the ones after it in the
// list. Each queued episode that is not on the card yet gets its sidecar
// written at the tap.
#define PODCAST_QUEUE_MAX (4 * PODCAST_LIMIT)

lv_obj_t *podcast_screen;
lv_obj_t *podcast_list_screen; // the same object: see podcastpage.h
#define list_screen podcast_screen
static lv_obj_t *search_screen;
static lv_obj_t *settings_screen;

static lv_obj_t *busy_layer;
static lv_obj_t *busy_label;
static lv_obj_t *search_btn;
static lv_obj_t *settings_btn;

// The three pills at the top -- the followed podcasts, the ones on the card and
// the chart -- and the row that holds them. Shown only while one of the lists
// they name is underneath: on search results they would not be telling the
// truth, because what is on screen is none of them.
static lv_obj_t *pill_row;
static lv_obj_t *pill_followed;
static lv_obj_t *pill_card;
static lv_obj_t *pill_trending;

// Without a network, or without the keys, the line that says so above the
// podcasts on the card -- which are all that can be listened to then.
static lv_obj_t *offline_banner;
static lv_obj_t *offline_label;
static bool offline;

// Above the followed podcasts and the ones on the card: "Now listening", the
// episode in the player, and under "Continue listening" the ones left halfway
// most recently. They scroll with the list, in a block above its rows.
#define LISTEN_ROWS 4 // the first for the episode in the player

typedef struct {
	lv_obj_t *button;
	lv_obj_t *thumb;
	lv_obj_t *title;
	lv_obj_t *detail;
	lv_obj_t *bar;
	lv_obj_t *action; // play, or pause on the episode in the player
	lv_obj_t *action_icon;
	cover_image_t image;
	char cover_path[512]; // the file the picture is of; "" for none
	bool cover_waiting;
	podcaststate_t state;
} listen_row_t;

static lv_obj_t *top_block;
static lv_obj_t *now_header;
static lv_obj_t *continue_header;
static listen_row_t listen[LISTEN_ROWS];

// What the "on the card" pill shows when the Podcast folder is empty: how to
// fill it.
static lv_obj_t *card_empty;

// The notice that takes the list's place when the section cannot work at all:
// no card.
static lv_obj_t *note_label;
static lv_obj_t *note_wifi_row; // the "Wi-Fi settings" row under the network notice
static void note_wifi_row_cb(lv_event_t *e);

// ---------------------------------------------------------------------------
// the worker
// ---------------------------------------------------------------------------

typedef enum {
	JOB_NONE = 0,
	JOB_SEARCH,	  // search podcasts by name
	JOB_TRENDING, // the current chart
	JOB_FEED,	  // one podcast's episodes, for its page (podcastcard.c)
	JOB_FOLLOWED, // the followed podcasts, which live on the card
	JOB_PLAY,	  // download an episode and start it
	JOB_CARD,	  // the podcasts in the card's Podcast folder: drawn on the spot, never queued
	JOB_RESUME,	  // an episode of the catalogue left halfway: its podcast's list, then play it
} job_kind_t;

typedef struct {
	job_kind_t kind;
	char text[160];	 // the query, or the title the list will carry
	long long id;	 // the podcast's or the episode's id (64 bit: see podcast.h)
	int index;		 // for JOB_PLAY: which row was tapped; for JOB_FEED: the request's number
	long long feed;	 // for JOB_RESUME: the episode's podcast; for JOB_FEED: whether to ask its record too
	int want;		 // how many entries in all; 0 means one page
	// Whether this is the SAME list continuing rather than a new one. Set only
	// by the paging at the bottom of the list, never inferred from `want`: a
	// list can open asking for more than one page, and a continuation keeps the
	// rows already drawn and appends after them.
	bool more;
} job_t;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_ready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t result_taken = PTHREAD_COND_INITIALIZER;
static job_t pending;
static bool have_pending;
static bool worker_started;

// The results. Two arrays and not one because a podcast and an episode do not
// share fields, and merging them into a single struct would leave half the
// fields empty in every row.
static podcast_feed_t result_feeds[PODCAST_MAX_HELD];
static podcast_episode_t result_episodes[PODCAST_MAX_HELD];
static int result_count;
static int result_from; // from which row the on-screen list is (re)drawn
static int result_want; // how many entries were asked for
static int result_raw;	// how many the catalogue SENT, before the discards: this
						// is what says whether the answer was full (an episode without
						// audio dropped mid-page must not make a full page look
						// short)
static job_kind_t result_kind;
static char result_error[256];
static char result_title[160];

// The worker waits for the GUI to have taken the result before picking up the
// next job: without that, a second job could rewrite the arrays while the first
// was still being drawn.
static bool result_pending;

// Kept for the heading, whose reserved width follows the corner buttons.
static gui_config_t *g_cfg;

// The podcaststate_revision() the page was last drawn with.
static unsigned states_seen;

static void *worker_main(void *arg);
static void refresh_page_state(void);
static bool refresh_notes(void);
static void refresh_corner_buttons(void);
static bool network_up(void);
static void paint_pill(lv_obj_t *btn, bool on);
static void row_clicked_cb(lv_event_t *e);
static bool job_is_list(job_kind_t kind);
static void list_nav_drop(void);
static void list_nav_commit(void);
static void fill_list(int from);
static void busy_hide(void);
static void prefetch_urgent(const char *wanted_path);
static bool show_card_now(void);
static void backfill_start(void);

static void submit(const job_t *job) {
	pthread_mutex_lock(&job_lock);
	// One job waits at a time: a podcast's page whose list is pushed out by
	// another job is told, so that it asks again rather than wait for nothing.
	bool dropped = have_pending && pending.kind == JOB_FEED && job->kind != JOB_FEED;
	unsigned dropped_ticket = (unsigned)pending.index;
	pending = *job;
	have_pending = true;
	pthread_cond_signal(&job_ready);
	pthread_mutex_unlock(&job_lock);
	if (dropped) {
		podcastcard_feed_loaded(dropped_ticket, -1, NULL, NULL);
	}
}

static void job_result_taken(void) {
	pthread_mutex_lock(&job_lock);
	result_pending = false;
	pthread_cond_broadcast(&result_taken);
	pthread_mutex_unlock(&job_lock);
}

// ---------------------------------------------------------------------------
// the waiting veil
// ---------------------------------------------------------------------------

static void busy_show_text(bool with_text) {
	if (!busy_layer) {
		return;
	}
	if (busy_label) {
		if (with_text) {
			lv_obj_set_hidden(busy_label, false);
		} else {
			lv_obj_set_hidden(busy_label, true);
		}
	}
	lv_obj_set_hidden(busy_layer, false);
	lv_obj_move_foreground(busy_layer);
}

// Waiting for a list: the spinner says enough, and the list being replaced is
// what the user is looking at anyway.
static void busy_show(void) { busy_show_text(false); }

static void busy_hide(void) {
	if (busy_layer) {
		lv_obj_set_hidden(busy_layer, true);
	}
}

static void build_busy_layer(void) {
	busy_layer = lv_obj_create(lv_layer_sys());
	lv_obj_set_size(busy_layer, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(busy_layer, 0, 0);
	lv_obj_set_style_bg_color(busy_layer, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(busy_layer, LV_OPA_50, 0);
	lv_obj_set_style_border_width(busy_layer, 0, 0);
	lv_obj_set_style_radius(busy_layer, 0, 0);
	lv_obj_set_scrollable(busy_layer, false);
	lv_obj_set_hidden(busy_layer, true);
	lv_obj_set_flex_flow(busy_layer, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(busy_layer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *spin = spinner_create(busy_layer, &icon_loader_big);
	lv_obj_set_style_image_recolor(spin, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(spin, LV_OPA_COVER, 0);

	// Under the spinner, because a spinner alone over a list that is still
	// there reads as "the page is stuck" rather than "your episode is coming".
	busy_label = lv_label_create(busy_layer);
	lv_label_set_text(busy_label, tr("loading"));
	lv_obj_set_style_text_color(busy_label, lv_color_white(), 0);
	lv_obj_set_style_text_font(busy_label, &font_ui_22, 0);
	lv_obj_set_style_margin_top(busy_label, 16, 0);
	lv_obj_set_hidden(busy_label, true);
}

// ---------------------------------------------------------------------------
// the row covers
//
// The loader has twelve slots and there can be sixty rows: they go to the rows
// that are VISIBLE and change hands as the list scrolls. The loader is Qobuz's
// and suits fine -- it names its images after the MD5 of the address, so two
// services cannot tread on each other even by accident (see qobuzart.h).
// ---------------------------------------------------------------------------

#define ROW_THUMB 60

static lv_obj_t *list_container;
static lv_obj_t *list_title;
static lv_obj_t *list_empty;

// What is drawn right now, which is not the same as result_kind: result_* says
// what the worker is working on, and while an episode downloads that job is
// JOB_PLAY -- but the screen still holds the list with its tappable rows.
// Whatever answers a tap has to look at what is visible, not at what is being
// done. (qobuzpage.c does the same, under the same name.)
static job_kind_t list_kind;

// Paging, as in the radio: the last request came back full to the limit, so
// there may be more -- and one request at a time.
static bool page_more;
static bool page_loading;
static int list_count;

// The rows, of which there are twelve however long the list is.
//
// Scrolling a container costs time proportional to how many children it has,
// whether or not they are on screen, so a widget tree per entry -- up to two
// hundred and forty of them, some fifteen hundred LVGL objects -- makes the
// list crawl.
//
// So the widgets are a pool that slides over the data, exactly as the file
// browser and the track list do. Twelve for the six that fit, and the arithmetic
// that makes it work is uniform pitch: row `n` sits at `n * ROW_PITCH`, the
// first one to bind is `scroll / ROW_PITCH`, and the scrollable height is
// `count * ROW_PITCH` on an otherwise empty body object.
//
// Everything ABOUT an entry -- its cover, whether that cover failed, which row
// lent it -- stays indexed by the entry, not by the widget. The widget is only
// what draws it this second.
#define ROW_HEIGHT 88
#define ROW_GAP 12
#define ROW_PITCH (ROW_HEIGHT + ROW_GAP)
#define ROW_POOL 12

typedef struct {
	lv_obj_t *button;
	lv_obj_t *thumb;
	lv_obj_t *title;
	lv_obj_t *detail;
	lv_obj_t *menu_btn;
	lv_obj_t *badge;	  // on the cover of a podcast on the card: where it came from
	lv_obj_t *badge_icon;
	// The entry drawn on it: -1 for a row with nothing to show, and -2 for one
	// that must be redrawn even though its number has not changed -- the list
	// reloaded underneath it to the same length.
	int index;
} row_t;

static row_t rows[ROW_POOL];

// The podcasts on the card, when that is the list on screen.
static podcastcard_show_t card_shows[PODCASTCARD_MAX_SHOWS];

// How many episodes the card holds for each followed podcast, for the line
// under its name.
static int followed_on_card[PODCASTSUBS_MAX];

// The object the rows are positioned inside. It holds no layout of its own: its
// only job is to be as tall as the whole list so the container has something to
// scroll.
static lv_obj_t *list_body;

static char row_cover_url[PODCAST_MAX_HELD][QOBUZART_URL_MAX];
static cover_image_t row_images[PODCAST_MAX_HELD];
static bool row_has_image[PODCAST_MAX_HELD];

// Which row the picture on this one belongs to, or -1 for a row showing the
// microphone. A row that owns its picture points at itself.
//
// Every episode of a podcast carries the same artwork and the catalogue repeats
// that one address on all of them, so asked row by row it would be hundreds of
// downloads and JPEG decodes of identical pictures on a single core. The first
// row to get a picture lends it to every other row asking for the same address.
//
// The rule is the address, not the kind of list: a show that does give its
// episodes their own artwork still gets it, and a search result listing the same
// podcast twice fetches its cover once.
static int row_image_source[PODCAST_MAX_HELD];

// This row has already asked for its cover and did not get one: dead address,
// image that will not decode, host that never answers. Without this memory the
// reload pass -- which restarts at every slot that frees up -- would ask again,
// and again, forever: one broken row would keep the loader busy and the good
// covers would never arrive.
static bool row_art_failed[PODCAST_MAX_HELD];

// The token that says "these slots are mine": only its address counts, never
// its value.
static const char podcast_art_owner;

// The last of the loader's slots is the cover at the head of a podcast's page
// (podcastcard.c); the rows share the others.
#define ROW_ART_SLOTS (QOBUZART_SLOTS - 1)
#define HERO_ART_SLOT (QOBUZART_SLOTS - 1)

static int slot_owner[ROW_ART_SLOTS];
static lv_timer_t *art_timer;

// The podcasts on the card bring their covers from the card, through the
// cover loader: pooled row i asks with slot PODCASTCARD_SLOT_ROWS + i, and this
// is the entry it asked for, or -1.
static int card_slot_entry[ROW_POOL];

static bool art_any_pending(void) {
	for (int i = 0; i < ROW_ART_SLOTS; i++) {
		if (slot_owner[i] >= 0) {
			return true;
		}
	}
	for (int i = 0; i < ROW_POOL; i++) {
		if (card_slot_entry[i] >= 0) {
			return true;
		}
	}
	for (int i = 0; i < LISTEN_ROWS; i++) {
		if (listen[i].cover_waiting) {
			return true;
		}
	}
	return false;
}

static void card_art_release(int pool) {
	if (card_slot_entry[pool] >= 0) {
		coverloader_release(PODCASTCARD_SLOT_ROWS + pool);
		card_slot_entry[pool] = -1;
	}
}

static void art_wake(void) {
	if (art_timer) {
		lv_timer_resume(art_timer);
	}
}

static void art_release_slot(int slot) {
	if (slot < 0 || slot >= ROW_ART_SLOTS) {
		return;
	}
	qobuzart_release(&podcast_art_owner, slot);
	slot_owner[slot] = -1;
}

static void art_request_visible(void);
static void listen_art_poll(void);

// The pooled widget drawing this entry right now, or NULL when the entry is off
// screen. Twelve comparisons, which is cheaper than keeping a map up to date.
static lv_obj_t *thumb_showing(int index) {
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].index == index) {
			return rows[i].thumb;
		}
	}
	return NULL;
}

// Puts the picture, or the microphone when there is none, on one pooled widget.
static void paint_thumb(lv_obj_t *thumb, int index) {
	if (!thumb) {
		return;
	}
	if (index >= 0 && row_has_image[index]) {
		lv_obj_remove_style(thumb, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(thumb, LV_OPA_TRANSP, 0);
		lv_obj_set_style_radius(thumb, 6, 0);
		lv_obj_set_style_clip_corner(thumb, true, 0);
		lv_image_set_src(thumb, &row_images[row_image_source[index]].dsc);
		return;
	}
	// The microphone, not the musical note: a podcast row without a cover is
	// not a track. The icon is the one supplied by the user (podcast-list.svg).
	lv_image_set_src(thumb, &icon_podcast_list);
	lv_obj_add_style(thumb, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(thumb, LV_OPA_COVER, 0);
	lv_obj_set_style_clip_corner(thumb, false, 0);
	lv_obj_set_style_radius(thumb, 0, 0);
}

// Records that the picture belonging to `owner` is the one for `row`, and draws
// it if that row is on screen. The two are the same row when it has just come
// out of the decoder.
static void show_cover(int row, int owner) {
	if (row != owner) {
		// It may already have asked for a copy of its own; that copy is now
		// pointless and the slot is worth more to a row that has nothing.
		for (int slot = 0; slot < ROW_ART_SLOTS; slot++) {
			if (slot_owner[slot] == row) {
				art_release_slot(slot);
			}
		}
	}
	row_has_image[row] = true;
	row_image_source[row] = owner;
	paint_thumb(thumb_showing(row), row);
}

// The row that owns a picture, or -1 when nobody has this address yet.
static int cover_owner_for(const char *url) {
	if (!url[0]) {
		return -1;
	}
	for (int row = 0; row < PODCAST_MAX_HELD; row++) {
		if (row_has_image[row] && row_image_source[row] == row && strcmp(row_cover_url[row], url) == 0) {
			return row;
		}
	}
	return -1;
}

// A picture has just arrived: every row waiting for the same address gets it
// now, including the ones off screen, because the cost of handing over a
// pointer is nothing and it stops them from ever asking.
static void lend_cover(int owner) {
	for (int row = 0; row < PODCAST_MAX_HELD; row++) {
		if (row == owner || row >= list_count || row_has_image[row]) {
			continue;
		}
		if (strcmp(row_cover_url[row], row_cover_url[owner]) == 0) {
			show_cover(row, owner);
		}
	}
}

// And the same for a failure: an address that would not download will not
// download for the other rows either, and without this the one broken cover of
// a podcast is retried once per episode.
static void fail_cover(int owner) {
	row_art_failed[owner] = true;
	for (int row = 0; row < PODCAST_MAX_HELD; row++) {
		if (row != owner && !row_has_image[row] && row_cover_url[row][0] &&
			strcmp(row_cover_url[row], row_cover_url[owner]) == 0) {
			row_art_failed[row] = true;
		}
	}
}

static void art_poll_cb(lv_timer_t *timer) {
	bool freed = false;
	if (!art_any_pending()) {
		lv_timer_pause(timer); // nothing in flight: art_wake() starts it again
		return;
	}

	for (int slot = 0; slot < ROW_ART_SLOTS; slot++) {
		int row = slot_owner[slot];
		if (row < 0 || row >= PODCAST_MAX_HELD) {
			continue;
		}
		cover_image_t image;
		bool finished = false;
		if (!qobuzart_take(&podcast_art_owner, slot, &image, &finished)) {
			if (finished) {
				// Failed: this address is never requested again.
				fail_cover(row);
				freed = true;
				slot_owner[slot] = -1;
			}
			continue;
		}

		cover_free(&row_images[row]);
		row_images[row] = image;
		show_cover(row, row);
		lend_cover(row);

		slot_owner[slot] = -1;
		freed = true;
	}

	// The covers of the podcasts on the card.
	for (int i = 0; i < ROW_POOL; i++) {
		int row = card_slot_entry[i];
		if (row < 0) {
			continue;
		}
		cover_image_t image;
		bool finished = false;
		bool got = coverloader_take(PODCASTCARD_SLOT_ROWS + i, &image, &finished);
		if (!got && !finished) {
			continue;
		}
		card_slot_entry[i] = -1;
		freed = true;
		if (!got) {
			if (row < PODCAST_MAX_HELD) {
				row_art_failed[row] = true;
			}
			continue;
		}
		if (list_kind != JOB_CARD || row >= list_count || row_has_image[row]) {
			cover_free(&image);
			continue;
		}
		cover_free(&row_images[row]);
		row_images[row] = image;
		row_has_image[row] = true;
		row_image_source[row] = row;
		paint_thumb(thumb_showing(row), row);
	}

	listen_art_poll();

	// A slot that frees up goes straight back out to someone.
	//
	// There are more rows than slots, so art_request_visible() hands out what
	// it can and returns; without this pass the rest would only be asked for
	// again on the next scroll, and never at all if the last scroll happened
	// while every slot was busy.
	if (freed) {
		art_request_visible();
	}

}

// The covers wanted right now, which is the same set as the rows that exist:
// the pool IS the visible band, so there is nothing left to test for
// visibility.
static void card_art_request_visible(void) {
	for (int i = 0; i < ROW_POOL; i++) {
		int row = rows[i].index;
		if (row < 0 || row >= list_count || row_has_image[row] || row_art_failed[row] || card_slot_entry[i] == row) {
			continue;
		}
		card_art_release(i);
		const char *first = card_shows[row].first;
		if (!first[0]) {
			row_art_failed[row] = true;
			continue;
		}
		// A thumbnail made before costs a small read; only a podcast seen for
		// the first time waits for the decoder.
		cover_image_t image;
		if (cover_thumb_cached(first, ROW_THUMB, &image)) {
			cover_free(&row_images[row]);
			row_images[row] = image;
			row_has_image[row] = true;
			row_image_source[row] = row;
			paint_thumb(rows[i].thumb, row);
			continue;
		}
		coverloader_request(PODCASTCARD_SLOT_ROWS + i, first, ROW_THUMB);
		card_slot_entry[i] = row;
		art_wake();
	}
}

static void art_request_visible(void) {
	if (list_kind == JOB_CARD) {
		card_art_request_visible();
		return;
	}
	for (int i = 0; i < ROW_POOL; i++) {
		int row = rows[i].index;
		if (row < 0 || row >= list_count || row_has_image[row] || row_art_failed[row] || !row_cover_url[row][0]) {
			continue;
		}

		// Already downloaded for another row of the same podcast: hand it over
		// rather than spend a slot, a download and a decode on a copy of it.
		int owner = cover_owner_for(row_cover_url[row]);
		if (owner >= 0) {
			show_cover(row, owner);
			continue;
		}

		bool queued = false;
		for (int slot = 0; slot < ROW_ART_SLOTS && !queued; slot++) {
			queued = slot_owner[slot] == row;
		}
		if (queued) {
			continue;
		}

		int free_slot = -1;
		for (int slot = 0; slot < ROW_ART_SLOTS && free_slot < 0; slot++) {
			if (slot_owner[slot] < 0) {
				free_slot = slot;
			}
		}
		if (free_slot < 0) {
			return;
		}

		slot_owner[free_slot] = row;
		art_wake();
		qobuzart_request(&podcast_art_owner, free_slot, row_cover_url[row], ROW_THUMB);
	}
}

static void maybe_load_more(void);
static void row_detail(int index, char *out, size_t size);
static bool kind_has_row_menu(job_kind_t kind);

// Puts entry `index` on one pooled widget, or hides it when there is no entry.
// Nothing is created here: the widgets were built once and only ever change
// what they say and where they sit.
static void row_bind(row_t *row, int index) {
	if (row->index == index) {
		return;
	}

	// The slot this widget's old entry was waiting on. Letting it run would
	// fill a cover nobody is looking at while a visible row waits for a slot --
	// and if it is an address some other row also wants, that row asks for it
	// itself.
	int leaving = row->index;
	if (leaving >= 0 && !row_has_image[leaving]) {
		for (int slot = 0; slot < ROW_ART_SLOTS; slot++) {
			if (slot_owner[slot] == leaving) {
				art_release_slot(slot);
			}
		}
	}
	int pool = (int)(row - rows);
	if (card_slot_entry[pool] >= 0 && card_slot_entry[pool] != index) {
		card_art_release(pool);
	}

	row->index = index;

	if (index < 0 || index >= list_count) {
		lv_obj_set_hidden(row->button, true);
		return;
	}

	lv_obj_set_hidden(row->button, false);
	lv_obj_set_y(row->button, index * ROW_PITCH);

	const char *title =
		list_kind == JOB_CARD ? podcastcard_show_name(&card_shows[index]) : result_feeds[index].title;
	lv_label_set_text(row->title, title);

	// Where a podcast on the card came from: downloaded here, or copied in.
	if (list_kind == JOB_CARD) {
		lv_image_set_src(row->badge_icon, card_shows[index].downloaded ? &icon_download_badge : &icon_sd_card_badge);
		lv_obj_set_hidden(row->badge, false);
	} else {
		lv_obj_set_hidden(row->badge, true);
	}

	char detail[200];
	row_detail(index, detail, sizeof(detail));
	if (detail[0]) {
		lv_label_set_text(row->detail, detail);
		lv_obj_set_hidden(row->detail, false);
	} else {
		lv_obj_set_hidden(row->detail, true);
	}

	// The three dots belong to a podcast, not to an episode, and the same pool
	// serves both: built on every row, shown on the lists that have something
	// to put in it.
	if (kind_has_row_menu(list_kind)) {
		lv_obj_set_hidden(row->menu_btn, false);
	} else {
		lv_obj_set_hidden(row->menu_btn, true);
	}

	paint_thumb(row->thumb, index);
}

// Slides the pool to wherever the list has been scrolled to.
static void window_update(void) {
	if (!list_container) {
		return;
	}

	// From the top of the rows, under whatever sits above them.
	int scroll = lv_obj_get_scroll_y(list_container) - lv_obj_get_y(list_body);
	if (scroll < 0) {
		scroll = 0; // the elastic overscroll at the top
	}

	// One row of overdraw above, so a row arriving from the top is already
	// drawn rather than appearing as the gap reaches it.
	int first = (scroll / ROW_PITCH) - 1;
	if (first + ROW_POOL > list_count) {
		first = list_count - ROW_POOL;
	}
	if (first < 0) {
		first = 0;
	}

	// The widget for entry n is always rows[n % ROW_POOL], which is what makes
	// a one-row slide rebind one widget instead of all twelve.
	for (int i = 0; i < ROW_POOL; i++) {
		int index = first + i;
		row_bind(&rows[index % ROW_POOL], index < list_count ? index : -1);
	}

	art_request_visible();
}

static void list_scrolled_cb(lv_event_t *e) {
	(void)e;
	window_update();
	maybe_load_more();
}

static void art_forget_rows(void) {
	for (int slot = 0; slot < ROW_ART_SLOTS; slot++) {
		art_release_slot(slot);
	}
	for (int i = 0; i < ROW_POOL; i++) {
		card_art_release(i);
	}
	// Nothing may still be pointing at the pixels about to go: the widgets stay
	// alive across a list change, so they are put back on the microphone first
	// and freed afterwards.
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].button) {
			lv_obj_set_hidden(rows[i].button, true);
			paint_thumb(rows[i].thumb, -1);
		}
		rows[i].index = -1;
	}
	for (int row = 0; row < PODCAST_MAX_HELD; row++) {
		row_cover_url[row][0] = '\0';
		row_has_image[row] = false;
		row_art_failed[row] = false;
		row_image_source[row] = -1;
		// Only an owner has pixels of its own; a row that borrowed one holds a
		// zeroed image, and freeing that is nothing.
		cover_free(&row_images[row]);
	}
}

// Gives the decoded thumbnails back without taking the list apart: the entries
// stay, so coming back to the page finds the list where it was left and only
// the pictures have to be fetched again.
//
// row_art_failed is deliberately NOT cleared: a cover that would not download
// still will not, and forgetting that would send the same failing requests
// again at every visit.
static void art_drop_pictures(void) {
	for (int slot = 0; slot < ROW_ART_SLOTS; slot++) {
		art_release_slot(slot);
	}
	for (int i = 0; i < ROW_POOL; i++) {
		card_art_release(i);
	}
	for (int row = 0; row < PODCAST_MAX_HELD; row++) {
		row_has_image[row] = false;
		row_image_source[row] = -1;
		cover_free(&row_images[row]);
	}
	// Back to the microphone, after the state says there is no picture and
	// before anything can draw the freed pixels.
	for (int i = 0; i < ROW_POOL; i++) {
		paint_thumb(rows[i].thumb, rows[i].index);
	}
}

// Two hundred and forty rows of 60x60 is 1.7 MB, held until the list is
// rebuilt, so the pictures are given back a while after the page is left. Not
// the moment it is left: tapping an episode goes to the player and coming
// straight back is the ordinary way to use this page, and freeing on the way out
// would re-read and re-decode a dozen pictures every time something is played.
#define ART_DROP_DELAY_MS 20000

static lv_timer_t *art_drop_timer;

static void art_drop_cb(lv_timer_t *timer) {
	lv_timer_pause(timer);
	art_drop_pictures();

	// And back to the kernel, not merely back to the arena. A thumbnail is
	// 7200 bytes, well under the 96 kB mmap threshold main.c sets, so the
	// thumbnails come out of the heap rather than out of their own mappings
	// and freeing them alone leaves the arena holding the space.
	malloc_trim(0);
}

static void list_loaded_cb(lv_event_t *e) {
	(void)e;
	if (art_drop_timer) {
		lv_timer_pause(art_drop_timer); // back before the wait ran out
	}
	if (!list_container) {
		return; // the screen exists before the page inside it does
	}
	// Whatever was given back while the page was away is asked for again. Free
	// when nothing was dropped: every row still holding its picture is skipped.
	art_wake();
	window_update();
}

static void list_unloaded_cb(lv_event_t *e) {
	(void)e;
	if (art_drop_timer) {
		lv_timer_reset(art_drop_timer);
		lv_timer_resume(art_drop_timer);
	}
}

// ---------------------------------------------------------------------------
// the rows
// ---------------------------------------------------------------------------

// The three-dot menu is where following a podcast of the catalogue is chosen;
// the podcasts on the card have none.
static bool kind_has_row_menu(job_kind_t kind) {
	return kind == JOB_SEARCH || kind == JOB_TRENDING || kind == JOB_FOLLOWED;
}

static void row_menu_cb(lv_event_t *e);

// The twelve rows, built once. Everything that can differ between one entry and
// the next is created here and shown or hidden at bind time: a recycled widget
// cannot grow a subtitle it was not born with.
static void build_rows(int width) {
	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &rows[i];

		row->button = lv_btn_create(list_body);
		lv_obj_set_size(row->button, width, ROW_HEIGHT);
		lv_obj_set_x(row->button, 0);
		lv_obj_add_style(row->button, &theme_style_card, 0);
		lv_obj_add_style(row->button, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_radius(row->button, 12, 0);
		lv_obj_set_style_border_width(row->button, 0, 0);
		lv_obj_set_style_shadow_width(row->button, 0, 0);
		lv_obj_set_style_pad_hor(row->button, 16, 0);
		lv_obj_set_style_pad_ver(row->button, 0, 0);
		lv_obj_set_style_pad_column(row->button, 8, 0);
		lv_obj_set_scrollable(row->button, false);
		lv_obj_set_hidden(row->button, true);
		lv_obj_set_flex_flow(row->button, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row->button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		// Gestures (back, and pulling the player in) often start on a row, and a
		// button keeps the press to itself.
		lv_obj_set_event_bubble(row->button, true);
		lv_obj_add_event_cb(row->button, row_clicked_cb, LV_EVENT_CLICKED, NULL);

		// The thumbnail is always there, even when the catalogue gives no
		// address: a row without it comes up with nothing on the left, text
		// against the edge and out of line with every other row. With no
		// address the grey microphone stays, which is the honest answer --
		// there is no cover.
		row->thumb = lv_image_create(row->button);
		lv_obj_set_size(row->thumb, ROW_THUMB, ROW_THUMB);
		lv_image_set_inner_align(row->thumb, LV_IMAGE_ALIGN_CENTER);
		lv_obj_set_event_bubble(row->thumb, true);
		paint_thumb(row->thumb, -1);

		// Over the cover's corner, out of the row's layout.
		row->badge = lv_obj_create(row->button);
		lv_obj_remove_style_all(row->badge);
		lv_obj_set_ignore_layout(row->badge, true);
		lv_obj_set_size(row->badge, 24, 24);
		lv_obj_set_style_radius(row->badge, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(row->badge, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(row->badge, theme()->screen_bg, 0);
		lv_obj_set_pos(row->badge, ROW_THUMB - 10, (ROW_HEIGHT + ROW_THUMB) / 2 - 18);
		lv_obj_set_event_bubble(row->badge, true);
		lv_obj_set_hidden(row->badge, true);
		row->badge_icon = lv_image_create(row->badge);
		lv_image_set_src(row->badge_icon, &icon_sd_card_badge);
		lv_obj_add_style(row->badge_icon, &theme_style_icon, 0);
		lv_obj_center(row->badge_icon);
		lv_obj_set_event_bubble(row->badge_icon, true);

		lv_obj_t *texts = lv_obj_create(row->button);
		lv_obj_set_flex_grow(texts, 1);
		lv_obj_set_height(texts, LV_SIZE_CONTENT);
		lv_obj_set_style_bg_opa(texts, 0, 0);
		lv_obj_set_style_border_width(texts, 0, 0);
		lv_obj_set_style_pad_all(texts, 0, 0);
		lv_obj_set_style_pad_row(texts, 6, 0);
		lv_obj_set_scrollable(texts, false);
		lv_obj_set_event_bubble(texts, true);
		lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(texts, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

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

		row->menu_btn = lv_btn_create(row->button);
		lv_obj_set_size(row->menu_btn, 44, 44);
		lv_obj_set_style_bg_opa(row->menu_btn, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(row->menu_btn, 0, 0);
		lv_obj_set_style_shadow_width(row->menu_btn, 0, 0);
		lv_obj_set_style_pad_all(row->menu_btn, 0, 0);
		lv_obj_set_hidden(row->menu_btn, true);
		lv_obj_add_event_cb(row->menu_btn, row_menu_cb, LV_EVENT_CLICKED, NULL);

		lv_obj_t *dots = lv_image_create(row->menu_btn);
		lv_image_set_src(dots, &icon_ellipsis_vertical);
		lv_obj_add_style(dots, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(dots, LV_OPA_COVER, 0);
		lv_obj_center(dots);

		row->index = -1;
	}
}

// Which entry a pressed widget is showing. The number cannot be carried in the
// callback's user data: the widget that is entry 3 one moment is entry 51 the
// next.
static int row_index_of(const lv_obj_t *button) {
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].button == button) {
			return rows[i].index;
		}
	}
	return -1;
}

static int menu_index_of(const lv_obj_t *menu_btn) {
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].menu_btn == menu_btn) {
			return rows[i].index;
		}
	}
	return -1;
}

// "1 h 12 min", "43 min", "" when the catalogue does not say.
static void format_duration(int seconds, char *out, size_t size) {
	if (seconds <= 0) {
		out[0] = '\0';
		return;
	}
	int minutes = (seconds + 30) / 60;
	if (minutes < 60) {
		snprintf(out, size, "%d min", minutes);
	} else {
		snprintf(out, size, "%d h %02d min", minutes / 60, minutes % 60);
	}
}

// The line under an entry's name: the author and episode count of a podcast, or
// what a podcast on the card holds. Built at bind time rather than kept for every
// entry -- it is twelve short strings on screen, not two hundred and forty in
// memory.
static void row_detail(int index, char *out, size_t size) {
	out[0] = '\0';
	if (index < 0 || index >= list_count) {
		return;
	}
	if (list_kind == JOB_CARD) {
		const podcastcard_show_t *show = &card_shows[index];
		char total[48];
		if (show->episodes == 1) {
			snprintf(total, sizeof(total), "%s", tr("podcast_one_episode"));
		} else {
			snprintf(total, sizeof(total), tr("podcast_episodes_2"), show->episodes);
		}
		if (show->unheard == 0) {
			snprintf(out, size, "%s \xC2\xB7 %s", tr("podcast_all_heard_short"), total);
		} else {
			char waiting[48];
			snprintf(waiting, sizeof(waiting), tr("podcast_to_hear_count"), show->unheard);
			snprintf(out, size, "%s%s \xC2\xB7 %s", show->fresh ? "\xE2\x80\xA2 " : "", waiting, total);
		}
		return;
	}
	const podcast_feed_t *f = &result_feeds[index];
	if (f->episode_count > 0 && f->author[0]) {
		snprintf(out, size, tr("podcast_episodes"), f->author, f->episode_count);
	} else if (f->episode_count > 0) {
		snprintf(out, size, tr("podcast_episodes_2"), f->episode_count);
	} else {
		snprintf(out, size, "%s", f->author);
	}
	// A followed podcast with episodes on the card says so.
	if (list_kind == JOB_FOLLOWED && index < PODCASTSUBS_MAX && followed_on_card[index] > 0) {
		size_t n = strlen(out);
		char on_card[48];
		snprintf(on_card, sizeof(on_card), tr("podcast_on_card_count"), followed_on_card[index]);
		snprintf(out + n, size - n, "%s%s", n ? " \xC2\xB7 " : "", on_card);
	}
}

static void refresh_listening(void);
static void refresh_card_empty(void);

// `from` is the first NEW entry: 0 is a different list, a higher value is
// another page of the same one appended at the end. No widget is created or
// destroyed either way -- the list is only as long as the body object says it
// is, and the pool slides over it.
static void fill_list(int from) {
	if (from == 0) {
		art_forget_rows();
	}

	list_kind = result_kind;
	list_count = result_count;

	// The addresses of every entry, on screen or not, because the lending looks
	// for a row that already holds this address and the row that holds it is
	// usually one nobody can see.
	for (int i = from; i < result_count && i < PODCAST_MAX_HELD && list_kind != JOB_CARD; i++) {
		snprintf(row_cover_url[i], sizeof(row_cover_url[0]), "%s", result_feeds[i].image);
	}
	if (list_kind == JOB_FOLLOWED) {
		for (int i = 0; i < list_count && i < PODCASTSUBS_MAX; i++) {
			followed_on_card[i] = podcastcard_count_for_title(result_feeds[i].title);
		}
	}

	// The entries under the pool have changed even where their numbers have
	// not: a search that returns the same count as the last one would otherwise
	// leave every row saying what it said before.
	for (int i = 0; i < ROW_POOL; i++) {
		rows[i].index = -2;
	}
	lv_obj_set_height(list_body, list_count > 0 ? list_count * ROW_PITCH : ROW_PITCH);

	if (from == 0) {
		lv_obj_scroll_to_y(list_container, 0, LV_ANIM_OFF);
	}
	window_update();

	// The pills only on the three lists they name. The title says "Podcasts"
	// there, because the lit pill already says which one is on screen;
	// elsewhere it says what is on screen, which is the query or the podcast's
	// name.
	bool home_view = list_kind == JOB_FOLLOWED || list_kind == JOB_TRENDING || list_kind == JOB_CARD;
	if (pill_row) {
		if (home_view) {
			lv_obj_set_hidden(pill_row, false);
		} else {
			lv_obj_set_hidden(pill_row, true);
		}
	}
	// The magnifier and the gear belong to the SECTION, not to what is inside
	// it: on search results they go away together with the pills.
	refresh_corner_buttons();
	paint_pill(pill_followed, list_kind == JOB_FOLLOWED);
	paint_pill(pill_card, list_kind == JOB_CARD);
	paint_pill(pill_trending, list_kind == JOB_TRENDING);
	if (list_title) {
		lv_label_set_text(list_title, home_view ? tr("podcasts") : result_title);
	}

	if (list_count == 0 && list_kind != JOB_CARD) {
		// The empty notice says what to do when there is something to do. It is
		// where the hint lives: under the notice it appears only to someone
		// whose list really is empty, rather than as a subtitle that stays over
		// a full one.
		lv_label_set_text(list_empty,
						  list_kind == JOB_FOLLOWED ? tr("podcast_none_followed") : tr("nothing_to_show"));
		lv_obj_set_hidden(list_empty, false);
	} else {
		lv_obj_set_hidden(list_empty, true);
	}
	refresh_card_empty();
	refresh_listening();

	// A result can land after a notice has taken the page over -- the request
	// was already on the wire when the Wi-Fi went. The list is filled anyway,
	// so the page is whole once the notice goes, but it stays out of sight
	// underneath it instead of half showing through.
	if (note_label && !lv_obj_is_hidden(note_label)) {
		lv_obj_set_hidden(pill_row, true);
		lv_obj_set_hidden(list_container, true);
		lv_obj_set_hidden(list_empty, true);
	}
}

// ---------------------------------------------------------------------------
// the list history
//
// Every list lives on ONE screen, so the global screen manager has nothing to
// stack: this page keeps the stack itself. Taken from tidalpage.c: a step is
// committed only once the result has really arrived, since committing it at
// request time would leave the history pointing at a page nobody ever saw.
// ---------------------------------------------------------------------------

#define LIST_HISTORY_DEPTH 8
static job_t list_history[LIST_HISTORY_DEPTH];
static int list_history_len;
static job_t current_list_job;
static bool have_current_list;

static job_t inflight_job;
static bool inflight_valid;
static bool inflight_is_back; // a back step consumes a rung instead of adding one
static bool inflight_is_root; // a pill: the stack restarts from there

static bool job_is_list(job_kind_t kind) {
	return kind != JOB_PLAY && kind != JOB_RESUME && kind != JOB_FEED && kind != JOB_NONE;
}

// The jobs that start playback rather than draw a list.
static bool job_plays(job_kind_t kind) { return kind == JOB_PLAY || kind == JOB_RESUME; }

static bool same_list(const job_t *a, const job_t *b) {
	return a->kind == b->kind && a->id == b->id && strcmp(a->text, b->text) == 0;
}

static void list_nav_drop(void) { inflight_valid = false; }

static void list_nav_commit(void) {
	if (!inflight_valid) {
		return;
	}
	inflight_valid = false;

	if (inflight_is_back) {
		if (list_history_len > 0) {
			list_history_len--;
		}
		current_list_job = inflight_job;
		have_current_list = true;
		return;
	}

	// A pill is the section's root, not a step into something. Going from "My
	// podcasts" to "Trending" and then back must leave the section, not bounce
	// between the two.
	if (inflight_is_root) {
		list_history_len = 0;
		current_list_job = inflight_job;
		have_current_list = true;
		return;
	}

	// Entering from outside: the stack restarts here.
	//
	// The search screen is NOT "outside": it is a keyboard sitting on top of
	// this page, and the results it brings are a step INSIDE the list that was
	// being looked at. Without this second condition a search would clear the
	// stack, and the chevron from the results would leave the section instead
	// of returning to the pills.
	lv_obj_t *active = lv_screen_active();
	if (active != list_screen && active != search_screen) {
		list_history_len = 0;
		current_list_job = inflight_job;
		have_current_list = true;
		return;
	}

	// The same list reloaded is not a step forward.
	if (have_current_list && same_list(&current_list_job, &inflight_job)) {
		return;
	}

	if (have_current_list && list_history_len < LIST_HISTORY_DEPTH) {
		list_history[list_history_len++] = current_list_job;
	}
	current_list_job = inflight_job;
	have_current_list = true;
}

static void run(const job_t *job);
static void start_worker(void);
static void busy_show(void);

// The back step is prepared by hand rather than through run(), so that
// inflight_is_back stays raised until list_nav_commit() reads it. run() only
// hands the job to the worker; the commit happens much later, when the result
// comes back from the network, and a flag lowered straight after run() would
// have the back step stacked as a step forward. tidalpage.c does it the same
// way.
// The guard's peek, for the back swipe: is there a rung to consume inside the
// page? It moves nothing -- it only keeps another screen's page from being
// drawn under the finger.
static bool list_back_has_step(void) { return list_history_len > 0; }

static bool list_back_guard(void) {
	if (list_history_len == 0) {
		have_current_list = false;
		return false; // the stack is empty: leave the page
	}
	// The top step is PEEKED at and redone; the stack only shortens once the
	// result has arrived (list_nav_commit).
	job_t previous = list_history[list_history_len - 1];
	// The card's podcasts are not the worker's: they are drawn on the spot.
	if (previous.kind == JOB_CARD) {
		if (show_card_now()) {
			return true;
		}
	}
	inflight_job = previous;
	inflight_valid = true;
	inflight_is_back = true;
	inflight_is_root = false;
	page_more = false;
	page_loading = false;
	start_worker();
	if (previous.kind != JOB_FOLLOWED) {
		busy_show();
	}
	submit(&previous);
	return true;
}

// ---------------------------------------------------------------------------
// starting a job
// ---------------------------------------------------------------------------

static void start_worker(void) {
	if (worker_started) {
		return;
	}
	pthread_t thread;
	if (pthread_create(&thread, NULL, worker_main, NULL) == 0) {
		pthread_detach(thread);
		worker_started = true;
	}
}

static void run_ex(const job_t *job, bool root) {
	if (job_is_list(job->kind)) {
		inflight_job = *job;
		inflight_valid = true;
		inflight_is_back = false;
		inflight_is_root = root;
		// A new list is starting: the old one's paging must not fire while it is
		// awaited.
		page_more = false;
		page_loading = false;
	}
	start_worker();
	// No veil for "My podcasts": that list is on the card, comes back the
	// instant it is asked for, and a veil that appears and vanishes in the same
	// frame is only a black flash at every entry into the section.
	//
	// The veil DOES cover the wait before an episode starts, and it stays up
	// until job_done_cb takes it down, which is the moment playback has really
	// begun -- a timed toast would leave before the download it is about.
	if (job->kind != JOB_FOLLOWED) {
		busy_show_text(job_plays(job->kind));
	}
	submit(job);
}

static void run(const job_t *job) { run_ex(job, false); }

// A higher limit when the scroll comes near the bottom: like the radio, except
// the catalogue has no offset, so the same request is made again asking for one
// more page. No veil and no history -- it is the SAME list continuing (inflight
// deliberately stays off), and the rows already there stay usable meanwhile.
static void maybe_load_more(void) {
	if (page_loading || !page_more) {
		return;
	}
	if (lv_screen_active() != list_screen || !have_current_list || !job_is_list(current_list_job.kind) ||
		current_list_job.kind == JOB_FOLLOWED || current_list_job.kind == JOB_CARD) {
		return;
	}
	// A sideways drag (back, or pulling the player in) moves this list too:
	// that is not a request for more rows.
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	// Within one screen of the bottom is enough: the request takes a moment,
	// and reaching the last row only to find the list stops there is exactly
	// what this replaces.
	if (lv_obj_get_scroll_bottom(list_container) > lv_obj_get_height(list_container)) {
		return;
	}
	page_loading = true;
	job_t job = current_list_job;
	// From the LIMIT asked for last time, not from the cleaned-up count: the
	// discards (episodes with no audio) must not shift the step of the next
	// request.
	job.want = result_want + PODCAST_LIMIT;
	job.more = true;
	start_worker();
	submit(&job);
}

// ---------------------------------------------------------------------------
// waiting for an episode to come down
//
// Much simpler than on Qobuz and Tidal, for the reason given at the top of the
// file: nothing is fetched ahead. There is one request at a time, and it is
// always "the episode needed NOW".
// ---------------------------------------------------------------------------

static pthread_mutex_t fetch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fetch_ready = PTHREAD_COND_INITIALIZER;
static long long fetch_want_id;
static bool fetch_started;
static bool waiting_for_episode;

// The episodes that are known: they are what the address of the one the queue
// asks for is found from. Filled when a podcast's list arrives, and from the
// list a tap was made on.
static podcast_episode_t queued_episodes[PODCAST_MAX_HELD];
// Whether that array is the whole feed rather than a page of it. Asking its
// length is not the same question: a podcast with twenty episodes has twenty
// and no more, and testing the length against the cap would send the player
// back to the catalogue for the two hundred that do not exist -- once per tap,
// in front of the sound.
static bool queued_complete;
static int queued_count;

static void downloading_note_async(void *user) {
	(void)user;
	gui_notify_popup("downloading");
}

static bool episode_is_playable(const char *path) {
	long long id = podcastcache_episode_id(path);
	if (podcastcache_find(id, NULL, 0)) {
		return true;
	}
	// Or it is coming down right NOW: the decoder can read a growing file. Two
	// questions and not one, for the same reason as the other two services: the
	// comparison by path and the one by id can disagree, and the id cannot be
	// wrong.
	return growfile_is_growing(path) || (id != 0 && podcastcache_downloading_id() == id);
}

static void resume_if_waiting_async(void *user) {
	(void)user;
	if (!waiting_for_episode) {
		return;
	}
	// The Wi-Fi transfer is a file server running as root on the card this
	// episode was just written to. Nothing starts playing under it.
	if (wifitransfer_get_enabled()) {
		return;
	}
	int index = playlist_current_index();
	char path[512];
	if (index < 0 || !playlist_path_at(index, path, sizeof(path))) {
		return;
	}
	if (podcastcache_owns(path) && !episode_is_playable(path)) {
		fprintf(stderr, "podcast: something arrived but the queue is waiting for '%s', which is not ready\n", path);
		return;
	}
	waiting_for_episode = false;
	fprintf(stderr, "podcast: restarting the queue from slot %d\n", index);
	device_state_play_queue_index(index);
	player_refresh_now_playing();
}

// Called by device_state an instant before a file is started.
static bool podcastpage_prepare_track(const char *path) {
	if (!podcastcache_owns(path)) {
		waiting_for_episode = false;
		return true; // not a podcast episode
	}
	if (episode_is_playable(path)) {
		waiting_for_episode = false;
		return true;
	}

	fprintf(stderr, "podcast: '%s' not ready (id=%lld): stopping and requesting it\n", path, podcastcache_episode_id(path));

	// Stop what was playing FIRST: the previous file is still coming down and is
	// exactly what prefetch_urgent is about to abandon. Pulling it out from
	// under the audio thread makes it report "track finished", and the queue
	// advances by itself onto another missing episode.
	podcastcache_set_network_wanted(true);
	waiting_for_episode = true;
	device_state_stop();
	prefetch_urgent(path);
	gui_post(downloading_note_async, NULL);
	return false;
}

static void prefetch_urgent(const char *wanted_path) {
	long long wanted = podcastcache_episode_id(wanted_path ? wanted_path : "");
	if (wanted == 0) {
		return;
	}

	// Is this one already coming down? Asked both ways, and if the answer is yes
	// it is NOT abandoned: throwing away a wait already under way means starting
	// from zero and never reaching the end.
	char stem[512];
	snprintf(stem, sizeof(stem), "%s", wanted_path ? wanted_path : "");
	char *dot = strrchr(stem, '.');
	if (dot) {
		*(dot + 1) = '\0';
	}
	bool already_ours = (stem[0] && growfile_prefix_is_growing(stem)) || podcastcache_downloading_id() == wanted;
	if (!already_ours) {
		streamturn_abandon_all();
	}

	podcastcache_set_network_wanted(true);

	pthread_mutex_lock(&fetch_lock);
	fetch_want_id = wanted;
	pthread_cond_broadcast(&fetch_ready);
	pthread_mutex_unlock(&fetch_lock);
}

// The author of the podcast being browsed. It does not arrive with the episodes
// -- the catalogue's /episodes/byfeedid answer carries the podcast's TITLE and
// not its author -- but the sidecar files need it, because that is where the
// player's star takes what it needs to follow the podcast without asking the
// network. Given by the podcast's page, which has the whole record
// (podcastpage_set_browsing).
static char browsing_author[PODCAST_AUTHOR_MAX];
static char browsing_image[PODCAST_URL_MAX];

// Downloads an episode and writes the sidecars, with the local path in `out`.
static bool fetch_episode(const podcast_episode_t *e, char *out, size_t size) {
	if (!podcastcache_start(e->id, e->enclosure, e->mime, e->duration_secs, out, size)) {
		return false;
	}
	podcastcache_write_sidecars(e->id, e->mime, e->title, e->feed_title[0] ? e->feed_title : NULL, e->feed_id,
								browsing_author, browsing_image[0] ? browsing_image : e->image, e->image, true);
	return true;
}

static void *fetch_main(void *arg) {
	(void)arg;
	thread_be_background("podcast fetch");

	for (;;) {
		pthread_mutex_lock(&fetch_lock);
		while (fetch_want_id == 0) {
			pthread_cond_wait(&fetch_ready, &fetch_lock);
		}
		long long id = fetch_want_id;
		pthread_mutex_unlock(&fetch_lock);

		podcast_episode_t episode;
		bool known = false;
		for (int i = 0; i < queued_count && !known; i++) {
			if (queued_episodes[i].id == id) {
				episode = queued_episodes[i];
				known = true;
			}
		}

		// Not known does not mean lost: ask the catalogue.
		//
		// `queued_episodes` is the last episode list that was looked at, and the
		// queue outlives it: opening another podcast, or returning to a queue
		// resumed after a shutdown, is enough for the episode coming up not to be
		// in that array any more, and stopping here would leave playback stuck
		// on the next episode. One catalogue request by id settles it.
		if (!known) {
			fprintf(stderr, "podcast: %lld is not in the list we know, asking the catalogue\n", id);
			if (!podcast_episode(id, &episode)) {
				fprintf(stderr, "podcast: %lld not found there either: %s\n", id, podcast_last_error());
				pthread_mutex_lock(&fetch_lock);
				if (fetch_want_id == id) {
					fetch_want_id = 0;
				}
				pthread_mutex_unlock(&fetch_lock);
				continue;
			}
		}

		podcastcache_wait_idle();

		// The request may have changed while waiting for a turn.
		pthread_mutex_lock(&fetch_lock);
		bool moved_on = fetch_want_id != id;
		pthread_mutex_unlock(&fetch_lock);
		if (moved_on) {
			continue;
		}

		char path[512];
		bool ok = fetch_episode(&episode, path, sizeof(path));
		if (!ok) {
			fprintf(stderr, "podcast: %lld does not download: %s\n", id, podcastcache_last_error());
		}

		pthread_mutex_lock(&fetch_lock);
		if (fetch_want_id == id) {
			fetch_want_id = 0;
		}
		bool superseded = fetch_want_id != 0 && fetch_want_id != id;
		pthread_mutex_unlock(&fetch_lock);

		if (ok && !superseded) {
			gui_post(resume_if_waiting_async, NULL);
		}
	}
	return NULL;
}

static void fetch_start_thread(void) {
	pthread_mutex_lock(&fetch_lock);
	if (!fetch_started) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, fetch_main, NULL) == 0) {
			pthread_detach(thread);
			fetch_started = true;
		}
	}
	pthread_mutex_unlock(&fetch_lock);
}

// ---------------------------------------------------------------------------
// results coming back, on the GUI thread
// ---------------------------------------------------------------------------

static void play_started_async(void *user) {
	(void)user;
	player_refresh_now_playing();
	switch_screen(player_screen);
}

static void job_done_cb(void *user) {
	(void)user;
	busy_hide();

	if (result_error[0]) {
		list_nav_drop();
		gui_notify_popup(result_error);
		page_loading = false;
		job_result_taken();
		refresh_notes();
		return;
	}



	if (job_plays(result_kind)) {
		job_result_taken();
		return;
	}

	fill_list(result_from);
	// The request came back full to the limit asked for: there is probably more.
	// Come back short, the list has ended and nothing more is asked for. "My
	// podcasts" is entirely on the card and never pages.
	page_loading = false;
	page_more = result_kind != JOB_FOLLOWED && result_raw >= result_want && result_count < PODCAST_FEEDS_MAX;
	list_nav_commit();

	// The page only changes if the viewer is still where it expects to be.
	//
	// A job can finish after the user has gone elsewhere -- the real case is
	// opening the magnifier while the list is still loading: the result arrived
	// and forced the list back up, with the keyboard vanishing under the
	// fingers. The search screen counts as "inside the section", because the
	// results are exactly what was asked for from there.
	lv_obj_t *active = lv_screen_active();
	if (active == list_screen || active == search_screen) {
		switch_screen(list_screen);
	}
	job_result_taken();
}

// ---------------------------------------------------------------------------
// a podcast's list, for its page
//
// The worker writes the answer into the downloader's array, and the GUI copies
// it into the page's one when it is taken: the page reads its array while it
// scrolls, and never while it is being written.
// ---------------------------------------------------------------------------

// The answer, written by the worker and read once it has been posted.
static unsigned feed_answer_ticket;
static int feed_answer_count; // -1 on failure
static int feed_answer_want;
static bool feed_answer_full; // as many came back as were asked for
static char feed_answer_error[256];
static podcast_feed_t feed_answer_info;
static bool feed_answer_has_info;

// The list the page draws: result_episodes, this long. GUI thread.
static int feed_count;
static int feed_want;
static bool feed_more_flag;
static unsigned feed_last_ticket;

static void feed_done_cb(void *user) {
	(void)user;
	unsigned ticket = feed_answer_ticket;
	int n = feed_answer_count;
	// Only the latest request's: an older one is about a podcast no longer
	// open, or an earlier step of the same list.
	bool latest = ticket == feed_last_ticket;
	if (latest && n >= 0) {
		memcpy(result_episodes, queued_episodes, sizeof(result_episodes[0]) * (size_t)n);
		feed_count = n;
		feed_want = feed_answer_want;
		feed_more_flag = feed_answer_full && n < PODCAST_MAX_HELD;
	}
	static char error[256];
	static podcast_feed_t info;
	snprintf(error, sizeof(error), "%s", feed_answer_error);
	bool has_info = feed_answer_has_info;
	if (has_info) {
		info = feed_answer_info;
	}
	job_result_taken();
	if (latest) {
		podcastcard_feed_loaded(ticket, n, n < 0 ? error : NULL, has_info ? &info : NULL);
	}
}

// On the worker.
static void run_feed_job(const job_t *job) {
	int want = job->want > 0 ? job->want : PODCAST_EPISODES_STEP;
	if (want > PODCAST_MAX_HELD) {
		want = PODCAST_MAX_HELD;
	}
	feed_answer_error[0] = '\0';
	feed_answer_has_info = job->feed != 0 && podcast_feed(job->id, &feed_answer_info);
	int n = podcast_episodes(job->id, queued_episodes, want);
	if (n < 0) {
		snprintf(feed_answer_error, sizeof(feed_answer_error), "%s", podcast_last_error());
		// The array may hold part of an answer: nothing in it is to be trusted.
		queued_count = 0;
		feed_answer_full = false;
	} else {
		int raw = podcast_last_raw_count();
		queued_count = n;
		queued_complete = want >= PODCAST_MAX_HELD || raw < want;
		feed_answer_full = raw >= want;
	}
	feed_answer_ticket = (unsigned)job->index;
	feed_answer_count = n;
	feed_answer_want = want;
	pthread_mutex_lock(&job_lock);
	result_pending = true;
	pthread_mutex_unlock(&job_lock);
	gui_post(feed_done_cb, NULL);
}

unsigned podcastpage_feed_load(long long feed_id, int want, bool with_info) {
	job_t job = {0};
	job.kind = JOB_FEED;
	job.id = feed_id;
	job.want = want;
	job.feed = with_info ? 1 : 0;
	feed_last_ticket++;
	if (feed_last_ticket == 0) {
		feed_last_ticket = 1;
	}
	job.index = (int)feed_last_ticket;
	start_worker();
	submit(&job);
	return feed_last_ticket;
}

int podcastpage_feed_count(void) { return feed_count; }

const podcast_episode_t *podcastpage_feed_episode(int index) {
	return index >= 0 && index < feed_count ? &result_episodes[index] : NULL;
}

int podcastpage_feed_want(void) { return feed_want; }

bool podcastpage_feed_more(void) { return feed_more_flag; }

// ---------------------------------------------------------------------------
// the thread
// ---------------------------------------------------------------------------

// Waits, on the worker, until the point an episode was left at has come down:
// a seek past what has arrived is refused rather than waited for, so starting a
// growing file in the middle needs the bytes there first. Gives up -- and the
// episode starts from the top -- after two minutes, when the server did not
// say how long the file is, or when another job has been asked for.
static double wait_for_resume_point(const char *path, double resume, double duration) {
	if (resume <= 0 || !growfile_is_growing(path)) {
		return resume;
	}
	growfile_t *g = growfile_open(path);
	if (!g) {
		return 0;
	}
	double reached = 0;
	for (int tick = 0; tick < 600; tick++) {
		long done = 0, total = 0;
		if (!growfile_span(g, &done, &total)) {
			// Finished meanwhile, or no length announced.
			reached = growfile_is_growing(path) ? 0 : resume;
			break;
		}
		if (duration <= 0) {
			break;
		}
		double needed = (double)total * (resume / duration) * 1.02 + 65536.0;
		if ((double)done >= needed) {
			reached = resume;
			break;
		}
		pthread_mutex_lock(&job_lock);
		bool superseded = have_pending;
		pthread_mutex_unlock(&job_lock);
		if (superseded) {
			break;
		}
		usleep(200000);
	}
	growfile_close(g);
	if (reached <= 0) {
		fprintf(stderr, "podcast: '%s' did not come down as far as %.0f s, starting it from the top\n", path, resume);
	}
	return reached;
}

// Downloads queued_episodes[index], queues it and the ones after it, and
// starts it -- from where it was left, if it was. False, with result_error
// set, when the download cannot start.
static bool play_queued_from(int index) {
	// Drop whatever was coming down: this is the user's order, not a fetch
	// ahead.
	streamturn_abandon_all();
	podcastcache_set_network_wanted(true);

	char path[512];
	if (!fetch_episode(&queued_episodes[index], path, sizeof(path))) {
		snprintf(result_error, sizeof(result_error), "%s", podcastcache_last_error());
		return false;
	}

	// Where it was left, by its id: the same whether it was streamed or kept.
	double resume = 0;
	podcaststate_t state;
	if (podcaststate_get_episode(queued_episodes[index].id, &state) && !state.played && state.position > 5.0) {
		double duration = state.duration > 0 ? state.duration : queued_episodes[index].duration_secs;
		resume = wait_for_resume_point(path, state.position - 3.0, duration);
	}

	// The queue: this episode and the ones AFTER it in the list, that is,
	// going back in time. The others are not downloaded now -- they come
	// down when their turn arrives (see podcastpage_prepare_track).
	static char paths[PODCAST_QUEUE_MAX][512];
	static const char *path_ptr[PODCAST_QUEUE_MAX];
	int count = 0;
	for (int i = index; i < queued_count && count < PODCAST_QUEUE_MAX; i++) {
		if (i == index) {
			snprintf(paths[count], sizeof(paths[0]), "%s", path);
		} else if (!podcastcache_find(queued_episodes[i].id, paths[count], sizeof(paths[0]))) {
			podcastcache_path(queued_episodes[i].id, queued_episodes[i].mime, paths[count],
							  sizeof(paths[0]));

			// The tags for the whole queue at once, as Qobuz does and
			// for the same reason: they are text files of a few dozen
			// bytes and cost nothing, and without them the Queue page
			// would show the file name, which is the id, for every
			// episode not yet downloaded. Not the cover: that is an
			// image to download, and it is fetched when the episode's
			// turn comes.
			const podcast_episode_t *q = &queued_episodes[i];
			podcastcache_write_sidecars(q->id, q->mime, q->title,
										q->feed_title[0] ? q->feed_title : NULL, q->feed_id,
										browsing_author, browsing_image, q->image, false);
		}
		path_ptr[count] = paths[count];
		count++;
	}

	// _ordered: a podcast queue already has its order, newest first
	// going back. Plain device_state_play_list would shuffle the
	// episodes whenever the play mode is random.
	device_state_play_list_ordered_at(path_ptr, count, 0, resume > 0 ? resume : -1.0);
	gui_post(play_started_async, NULL);
	return true;
}

static void *worker_main(void *arg) {
	(void)arg;
	thread_be_background("podcast worker");

	for (;;) {
		pthread_mutex_lock(&job_lock);
		while (!have_pending) {
			pthread_cond_wait(&job_ready, &job_lock);
		}
		// The previous result must have been taken, or it would be rewritten
		// under whoever is drawing it. With a cap: if the GUI has fallen behind
		// through a fault of its own, an overwritten list beats a worker stuck
		// forever.
		struct timespec deadline;
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec += 2;
		while (result_pending) {
			if (pthread_cond_timedwait(&result_taken, &job_lock, &deadline) != 0) {
				break;
			}
		}

		job_t job = pending;
		have_pending = false;
		pthread_mutex_unlock(&job_lock);

		// A podcast's page: its own answer, which leaves the list on this page
		// as it is.
		if (job.kind == JOB_FEED) {
			run_feed_job(&job);
			continue;
		}

		// What is on screen stays there if the job fails: without this snapshot,
		// an error emptied the list being looked at.
		int kept_count = result_count;
		job_kind_t kept_kind = result_kind;
		char kept_title[160];
		snprintf(kept_title, sizeof(kept_title), "%s", result_title);

		result_error[0] = '\0';

		// Playback does not clear the list on this page: it stays on screen
		// with its tappable rows. Same rule as qobuzpage.c.
		result_kind = job.kind;

		// How many entries in all. The catalogue does not page with an offset, so
		// continuing a list is the same request with a higher limit: the arrays
		// are rewritten whole, and the GUI draws only the new rows (from
		// result_from on).
		int want = job.want;
		if (want <= 0) {
			want = PODCAST_LIMIT;
		}
		int cap = job_plays(job.kind) ? PODCAST_MAX_HELD : PODCAST_FEEDS_MAX;
		if (want > cap) {
			want = cap;
		}
		bool grow = job.more;
		int kept = result_count;

		if (!job_plays(job.kind) && !grow) {
			result_count = 0;
			snprintf(result_title, sizeof(result_title), "%s", job.text);
		}
		if (!job_plays(job.kind)) {
			result_from = grow ? kept : 0;
			result_want = want;
		}

		switch (job.kind) {
		case JOB_SEARCH: {
			int n = podcast_search(job.text, result_feeds, want);
			if (n < 0) {
				snprintf(result_error, sizeof(result_error), "%s", podcast_last_error());
			} else {
				result_count = n;
				result_raw = podcast_last_raw_count();
			}
			break;
		}

		case JOB_TRENDING: {
			int n = podcast_trending(result_feeds, want);
			if (n < 0) {
				snprintf(result_error, sizeof(result_error), "%s", podcast_last_error());
			} else {
				result_count = n;
				result_raw = podcast_last_raw_count();
			}
			break;
		}

		case JOB_FOLLOWED:
			// No network: the list is on the card, all of it at once.
			result_count = podcastsubs_list(result_feeds, PODCAST_MAX_HELD);
			result_raw = result_count;
			break;

		case JOB_PLAY: {
			// The list the tap was made on is the podcast page's: the episode
			// tapped and the ones after it are taken from there. The id says it
			// is still the same list.
			int index = job.index;
			if (index < 0 || index >= feed_count || result_episodes[index].id != job.id) {
				snprintf(result_error, sizeof(result_error), "%s", tr("podcast_that_episode_is_gone"));
				break;
			}
			memcpy(queued_episodes, result_episodes, sizeof(result_episodes[0]) * (size_t)feed_count);
			queued_count = feed_count;
			queued_complete = !feed_more_flag;
			play_queued_from(index);
			break;
		}

		case JOB_RESUME: {
			// The podcast's list first, so the episodes after this one follow it
			// as they would from a tap; the one episode alone when it is older
			// than the first step of the list, or the podcast is not known.
			int n = job.feed > 0 ? podcast_episodes(job.feed, queued_episodes, PODCAST_EPISODES_STEP) : -1;
			int index = -1;
			if (n > 0) {
				queued_count = n;
				queued_complete = podcast_last_raw_count() < PODCAST_EPISODES_STEP;
				for (int i = 0; i < n && index < 0; i++) {
					if (queued_episodes[i].id == job.id) {
						index = i;
					}
				}
			}
			if (index < 0) {
				podcast_episode_t one;
				if (!podcast_episode(job.id, &one)) {
					snprintf(result_error, sizeof(result_error), "%s", podcast_last_error());
					break;
				}
				queued_episodes[0] = one;
				queued_count = 1;
				queued_complete = true;
				index = 0;
			}
			play_queued_from(index);
			break;
		}

		case JOB_NONE:
		default:
			break;
		}

		// The job failed: put back what was there.
		if (result_error[0]) {
			result_count = kept_count;
			result_kind = kept_kind;
			snprintf(result_title, sizeof(result_title), "%s", kept_title);
		}

		// Repeating with a higher limit can -- rarely -- bring back FEWER entries
		// than before (the chart has moved): redraw from where the list really
		// ends, never past it.
		if (result_from > result_count) {
			result_from = result_count;
		}

		pthread_mutex_lock(&job_lock);
		result_pending = true;
		pthread_mutex_unlock(&job_lock);
		gui_post(job_done_cb, NULL);
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// tapping a row
// ---------------------------------------------------------------------------

static void row_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = row_index_of(lv_event_get_current_target(e));
	if (index < 0 || index >= list_count) {
		return;
	}

	if (list_kind == JOB_CARD) {
		podcastcard_open(&card_shows[index]);
		return;
	}

	// A podcast: its page, with its record as the list has it.
	podcastcard_open_feed(&result_feeds[index]);
}

// ---------------------------------------------------------------------------
// the three-dot menu: follow, or stop
// ---------------------------------------------------------------------------

static int menu_row_index;

static void follow_cb(void *user) {
	(void)user;
	if (menu_row_index < 0 || menu_row_index >= list_count) {
		return;
	}
	podcastsubs_follow(&result_feeds[menu_row_index]);
	gui_notify_popup(tr("podcast_now_following"));
}

static void unfollow_cb(void *user) {
	(void)user;
	if (menu_row_index < 0 || menu_row_index >= list_count) {
		return;
	}
	podcastsubs_unfollow(result_feeds[menu_row_index].id);
	gui_notify_popup(tr("podcast_no_longer_following"));

	// If the followed list itself is on screen, the row has to go at once:
	// leaving it there would mean a list that is no longer true.
	if (list_kind == JOB_FOLLOWED) {
		result_kind = JOB_FOLLOWED;
		result_count = podcastsubs_list(result_feeds, PODCAST_MAX_HELD);
		fill_list(0);
	}
}

static void row_menu_cb(lv_event_t *e) {
	menu_row_index = menu_index_of(lv_event_get_current_target(e));
	if (menu_row_index < 0 || menu_row_index >= list_count) {
		return;
	}

	// Two one-entry tables instead of one label picked with a ternary: the tool
	// that collects the strings to translate reads popover_item_t INITIALISERS,
	// not assignments.
	static const popover_item_t FOLLOW[] = {{"podcast_follow", follow_cb, NULL}};
	static const popover_item_t UNFOLLOW[] = {{"podcast_unfollow", unfollow_cb, NULL}};

	bool followed = podcastsubs_is_followed(result_feeds[menu_row_index].id);
	popover_show(lv_event_get_current_target(e), followed ? UNFOLLOW : FOLLOW, 1);
}

// ---------------------------------------------------------------------------
// keeping an episode: the card that follows the download
// ---------------------------------------------------------------------------

#define DL_POLL_MS 250

static lv_obj_t *dl_veil;
static lv_obj_t *dl_name;
static lv_obj_t *dl_bar;
static lv_obj_t *dl_amount;
static lv_obj_t *dl_title;
static lv_timer_t *dl_timer;

// The episodes waiting their turn: podcastdl.h keeps one at a time, so the
// ones chosen together go down one after the other behind the same card.
static podcast_episode_t *dl_queue;
static int dl_queue_count;
static int dl_queue_next;
static int dl_queue_done;
static int dl_queue_failed;
static bool dl_queue_cancelled;
static char dl_feed_title[PODCAST_TITLE_MAX];

static void dl_close(void) {
	lv_timer_pause(dl_timer);
	lv_obj_set_hidden(dl_veil, true);
	power_hold_screen_on(false);
}

// "12.4 / 58.0 MB", or the bytes alone when the server gave no length.
static void dl_paint(long long done, long long total) {
	double done_mb = (double)done / (1024.0 * 1024.0);
	if (total > 0) {
		int permille = (int)(done * 1000 / total);
		lv_bar_set_value(dl_bar, permille, LV_ANIM_OFF);
		lv_label_set_text_fmt(dl_amount, "%.1f / %.1f MB \xC2\xB7 %d%%", done_mb, (double)total / (1024.0 * 1024.0),
							  permille / 10);
	} else {
		lv_bar_set_value(dl_bar, 0, LV_ANIM_OFF);
		lv_label_set_text_fmt(dl_amount, "%.1f MB", done_mb);
	}
}

static void dl_queue_free(void) {
	free(dl_queue);
	dl_queue = NULL;
	dl_queue_count = 0;
	dl_queue_next = 0;
}

// The next episode of the queue that is not on the card yet. False when none
// is left to start.
static bool dl_start_next(void) {
	while (!dl_queue_cancelled && dl_queue_next < dl_queue_count) {
		const podcast_episode_t *episode = &dl_queue[dl_queue_next++];
		if (podcastdl_exists(episode, dl_feed_title)) {
			continue;
		}
		if (!podcastdl_start(episode, dl_feed_title, browsing_author, browsing_image)) {
			dl_queue_failed++;
			continue;
		}
		lv_label_set_text(dl_name, episode->title);
		if (dl_queue_count > 1) {
			char count[48];
			snprintf(count, sizeof(count), tr("podcast_download_count"), dl_queue_next, dl_queue_count);
			lv_label_set_text_fmt(dl_title, "%s \xC2\xB7 %s", tr("podcast_downloading"), count);
		} else {
			lv_label_set_text(dl_title, tr("podcast_downloading"));
		}
		dl_paint(0, 0);
		return true;
	}
	return false;
}

// What the queue came to, once it is over.
static void dl_report(void) {
	if (dl_queue_cancelled) {
		return;
	}
	if (dl_queue_done == 0 && dl_queue_failed > 0) {
		toast_error(tr("podcast_download_failed"));
	} else if (dl_queue_done == 1 && dl_queue_failed == 0) {
		toast_success(tr("podcast_download_done"));
	} else if (dl_queue_done > 0) {
		char text[96];
		snprintf(text, sizeof(text), tr("podcast_downloads_done"), dl_queue_done);
		if (dl_queue_failed > 0) {
			toast_error(text);
		} else {
			toast_success(text);
		}
	}
}

static void dl_poll_cb(lv_timer_t *timer) {
	(void)timer;
	long long done = 0, total = 0;
	podcastdl_state_t state = podcastdl_state(&done, &total);
	dl_paint(done, total);
	if (state == PODCASTDL_RUNNING) {
		return;
	}
	podcastdl_acknowledge();
	if (state == PODCASTDL_DONE) {
		dl_queue_done++;
	} else if (state == PODCASTDL_FAILED) {
		dl_queue_failed++;
	} else if (state == PODCASTDL_CANCELLED) {
		dl_queue_cancelled = true;
	}
	if (dl_start_next()) {
		return;
	}
	dl_close();
	dl_report();
	dl_queue_free();
}

static void dl_cancel_cb(lv_event_t *e) {
	(void)e;
	dl_queue_cancelled = true;
	podcastdl_cancel();
	lv_label_set_text(dl_amount, tr("podcast_download_cancelling"));
}

static void dl_build(gui_config_t *cfg) {
	dl_veil = lv_obj_create(lv_layer_top());
	lv_obj_set_size(dl_veil, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_color(dl_veil, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(dl_veil, LV_OPA_60, 0);
	lv_obj_set_style_border_width(dl_veil, 0, 0);
	lv_obj_set_style_radius(dl_veil, 0, 0);
	lv_obj_set_scrollable(dl_veil, false);
	lv_obj_set_clickable(dl_veil, true);
	lv_obj_set_hidden(dl_veil, true);

	lv_obj_t *card = lv_obj_create(dl_veil);
	lv_obj_set_size(card, cfg->screen_width - 2 * cfg->padding, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 16, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 20, 0);
	lv_obj_set_style_pad_row(card, 14, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_align(card, LV_ALIGN_CENTER, 0, cfg->top_bar_height / 2);

	dl_title = lv_label_create(card);
	lv_label_set_text(dl_title, tr("podcast_downloading"));
	lv_obj_add_style(dl_title, &theme_style_text, 0);
	lv_obj_set_style_text_font(dl_title, &font_ui_24, 0);

	dl_name = lv_label_create(card);
	lv_label_set_long_mode(dl_name, LV_LABEL_LONG_DOT);
	lv_obj_set_width(dl_name, lv_pct(100));
	lv_obj_set_style_max_height(dl_name, 2 * lv_font_get_line_height(&font_ui_20), 0);
	lv_obj_add_style(dl_name, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(dl_name, &font_ui_20, 0);

	dl_bar = lv_bar_create(card);
	lv_obj_set_size(dl_bar, lv_pct(100), 12);
	lv_bar_set_range(dl_bar, 0, 1000);
	lv_obj_set_style_bg_color(dl_bar, theme()->surface_pressed, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(dl_bar, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_bg_color(dl_bar, theme()->accent, LV_PART_INDICATOR);
	lv_obj_set_style_radius(dl_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_radius(dl_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

	dl_amount = lv_label_create(card);
	lv_obj_add_style(dl_amount, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(dl_amount, &font_ui_18, 0);

	lv_obj_t *cancel = lv_btn_create(card);
	lv_obj_set_size(cancel, lv_pct(100), 56);
	lv_obj_set_style_radius(cancel, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(cancel, 0, 0);
	lv_obj_set_style_border_width(cancel, 0, 0);
	lv_obj_set_style_bg_color(cancel, theme()->surface_pressed, 0);
	lv_obj_add_event_cb(cancel, dl_cancel_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *cancel_label = lv_label_create(cancel);
	lv_label_set_text(cancel_label, tr("cancel"));
	lv_obj_set_style_text_font(cancel_label, &font_ui_22, 0);
	lv_obj_set_style_text_color(cancel_label, theme()->text_primary, 0);
	lv_obj_center(cancel_label);

	dl_timer = lv_timer_create(dl_poll_cb, DL_POLL_MS, NULL);
	lv_timer_pause(dl_timer);
}

void podcastpage_feed_download_many(const int *indices, int count, const char *feed_title) {
	if (!indices || count <= 0) {
		return;
	}
	// One queue at a time: while it runs, its card covers the page.
	if (dl_queue || podcastdl_busy()) {
		toast_error(tr("podcast_download_failed"));
		return;
	}
	dl_queue = calloc((size_t)count, sizeof(*dl_queue));
	if (!dl_queue) {
		toast_error(tr("podcast_download_failed"));
		return;
	}
	dl_queue_count = 0;
	bool all_there = true;
	for (int i = 0; i < count; i++) {
		if (indices[i] < 0 || indices[i] >= feed_count) {
			continue;
		}
		dl_queue[dl_queue_count] = result_episodes[indices[i]];
		all_there = all_there && podcastdl_exists(&dl_queue[dl_queue_count], feed_title);
		dl_queue_count++;
	}
	snprintf(dl_feed_title, sizeof(dl_feed_title), "%s", feed_title ? feed_title : "");
	dl_queue_next = 0;
	dl_queue_done = 0;
	dl_queue_failed = 0;
	dl_queue_cancelled = false;
	if (dl_queue_count > 0 && all_there) {
		toast_plain(tr(dl_queue_count == 1 ? "podcast_already_downloaded" : "podcast_all_already_downloaded"));
		dl_queue_free();
		return;
	}
	if (!dl_start_next()) {
		toast_error(tr("podcast_download_failed"));
		dl_queue_free();
		return;
	}

	// Theme colours are read at every opening: the card is built once.
	lv_obj_set_style_bg_color(dl_bar, theme()->accent, LV_PART_INDICATOR);
	lv_obj_set_hidden(dl_veil, false);
	lv_obj_move_foreground(dl_veil);
	power_hold_screen_on(true);
	lv_timer_reset(dl_timer);
	lv_timer_resume(dl_timer);
}

void podcastpage_feed_download(int index, const char *feed_title) {
	podcastpage_feed_download_many(&index, 1, feed_title);
}

// ---------------------------------------------------------------------------
// the rest of what a podcast's page asks for
// ---------------------------------------------------------------------------

void podcastpage_set_browsing(const podcast_feed_t *feed) {
	if (!feed) {
		return;
	}
	snprintf(browsing_author, sizeof(browsing_author), "%s", feed->author);
	snprintf(browsing_image, sizeof(browsing_image), "%s", feed->image);
}

void podcastpage_feed_play(int index) {
	if (index < 0 || index >= feed_count) {
		return;
	}
	// No toast: a timed one would leave while the download it is about
	// carries on. run_ex() puts the veil up instead, with the same words on
	// it, and takes it down when the episode actually starts.
	job_t job = {0};
	job.kind = JOB_PLAY;
	job.index = index;
	job.id = result_episodes[index].id;
	run(&job);
}

bool podcastpage_feed_play_next(int index) {
	if (index < 0 || index >= feed_count) {
		return false;
	}
	const podcast_episode_t *e = &result_episodes[index];
	char path[512];
	// Kept on the card, or already in the cache: that file. Otherwise the
	// cache's name for it, with the tags written now, and the episode comes
	// down when its turn arrives (podcastpage_prepare_track).
	if (!podcastdl_path(e, e->feed_title, path, sizeof(path)) || access(path, F_OK) != 0) {
		if (!podcastcache_find(e->id, path, sizeof(path))) {
			podcastcache_path(e->id, e->mime, path, sizeof(path));
			podcastcache_write_sidecars(e->id, e->mime, e->title, e->feed_title[0] ? e->feed_title : NULL,
										e->feed_id, browsing_author, browsing_image, e->image, false);
		}
	}
	if (!path[0]) {
		return false;
	}
	return playlist_insert_next(path);
}

void podcastpage_resume(const podcaststate_t *state) {
	if (!state || state->episode_id <= 0) {
		return;
	}
	if (offline) {
		toast_error(tr("enable_wi_fi_first"));
		return;
	}
	job_t job = {0};
	job.kind = JOB_RESUME;
	job.id = state->episode_id;
	job.feed = state->feed_id;
	run(&job);
}

void podcastpage_hero_request(const char *url, int size) {
	if (!url || !url[0]) {
		return;
	}
	qobuzart_request(&podcast_art_owner, HERO_ART_SLOT, url, size);
}

bool podcastpage_hero_take(cover_image_t *out, bool *finished) {
	return qobuzart_take(&podcast_art_owner, HERO_ART_SLOT, out, finished);
}

void podcastpage_hero_release(void) { qobuzart_release(&podcast_art_owner, HERO_ART_SLOT); }

// ---------------------------------------------------------------------------
// the page: pills on top, list below
//
// ONE screen. The followed podcasts, the ones on the card and the chart are not
// different places: they are three ways of looking at the same thing, exactly like All / Recent / Finished in
// the audiobooks. That is what pills are for -- they change what is underneath
// without navigating away -- so the list sits on the same screen.
//
// The pills go away when a search is underneath: there they would no longer
// describe what is on screen, and a lit pill over a list that is not its own
// is a lie.
// ---------------------------------------------------------------------------

static void pill_clicked_cb(lv_event_t *e);

static lv_obj_t *make_pill(lv_obj_t *parent, const char *text, job_kind_t kind) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 52);
	lv_obj_set_style_pad_hor(btn, 18, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0); // Adwaita-style pill
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, pill_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)kind);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_20, 0);
	lv_obj_center(label);

	return btn;
}

static void paint_pill(lv_obj_t *btn, bool on) {
	if (!btn) {
		return;
	}
	lv_obj_set_style_bg_color(btn, on ? theme()->accent : theme()->surface_pressed, 0);
	lv_obj_set_style_text_color(lv_obj_get_child(btn, 0), on ? lv_color_white() : theme()->text_primary, 0);
	// Greyed while it cannot be used: the two pills that need the network.
	lv_obj_set_style_opa(btn, lv_obj_has_state(btn, LV_STATE_DISABLED) ? LV_OPA_40 : LV_OPA_COVER, 0);
}

// The corner buttons' visibility has a single owner, because two owners of one
// flag means whichever runs last wins. The magnifier needs the keys, a network
// and a home view (one of the pills). The gear needs none of that and stays up
// over the notices too: the podcast settings apply to the episodes on the card
// as well. From the edge: gear, magnifier.
static void refresh_corner_buttons(void) {
	bool home_view = list_kind == JOB_FOLLOWED || list_kind == JOB_TRENDING || list_kind == JOB_CARD ||
					 list_kind == JOB_NONE;
	bool online = podcast_configured() && network_up();
	bool search = online && home_view;
	bool local = home_view || !online;
	if (list_title && g_cfg) {
		// The heading gets the width the buttons are not using.
		settingsrow_title_corner_slots(list_title, g_cfg, (search ? 1 : 0) + (local ? 1 : 0));
	}
	lv_obj_t *const buttons[] = {search_btn, settings_btn};
	const bool shown[] = {search, local};
	for (int i = 0; i < 2; i++) {
		if (!buttons[i]) {
			continue;
		}
		if (shown[i]) {
			lv_obj_set_hidden(buttons[i], false);
		} else {
			lv_obj_set_hidden(buttons[i], true);
		}
	}
}

// The pill colours are set by hand, so a theme change does not touch them on
// its own: without this repaint the lit pill keeps the previous accent.
static void refresh_pills(void) {
	paint_pill(pill_followed, list_kind == JOB_FOLLOWED);
	paint_pill(pill_card, list_kind == JOB_CARD);
	paint_pill(pill_trending, list_kind == JOB_TRENDING);
	for (int k = 0; k < LISTEN_ROWS; k++) {
		if (listen[k].bar) {
			lv_obj_set_style_bg_color(listen[k].bar, theme()->accent, LV_PART_INDICATOR);
		}
	}
}

// ---------------------------------------------------------------------------
// the line about the network, "Continue listening", and the empty card
// ---------------------------------------------------------------------------

static void offline_banner_cb(lv_event_t *e) {
	(void)e;
	if (podcast_configured() && !network_up()) {
		switch_screen(wifisettings_screen);
	}
}

static void build_offline_banner(lv_obj_t *column, gui_config_t *cfg) {
	offline_banner = lv_btn_create(column);
	lv_obj_set_width(offline_banner, cfg->screen_width - 2 * cfg->padding);
	lv_obj_set_height(offline_banner, LV_SIZE_CONTENT);
	lv_obj_set_style_margin_left(offline_banner, cfg->padding, 0);
	lv_obj_add_style(offline_banner, &theme_style_card, 0);
	lv_obj_set_style_radius(offline_banner, 12, 0);
	lv_obj_set_style_border_width(offline_banner, 0, 0);
	lv_obj_set_style_shadow_width(offline_banner, 0, 0);
	lv_obj_set_style_pad_all(offline_banner, 12, 0);
	lv_obj_set_style_pad_column(offline_banner, 12, 0);
	lv_obj_set_scrollable(offline_banner, false);
	lv_obj_set_flex_flow(offline_banner, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(offline_banner, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_event_bubble(offline_banner, true);
	lv_obj_add_event_cb(offline_banner, offline_banner_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *icon = lv_image_create(offline_banner);
	lv_image_set_src(icon, &icon_wifi_off);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_set_event_bubble(icon, true);

	offline_label = lv_label_create(offline_banner);
	lv_label_set_long_mode(offline_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_flex_grow(offline_label, 1);
	lv_obj_add_style(offline_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(offline_label, &font_ui_18, 0);
	lv_label_set_text(offline_label, "");
	lv_obj_set_hidden(offline_banner, true);
}

// ---------------------------------------------------------------------------
// "Now listening" and "Continue listening"
// ---------------------------------------------------------------------------

#define LISTEN_ROW_HEIGHT 96

// The episode the first row shows: "" while it is hidden.
static char listen_now_path[512];

static bool listen_home(void) {
	return (list_kind == JOB_FOLLOWED || list_kind == JOB_CARD) && (!note_label || lv_obj_is_hidden(note_label));
}

static void listen_thumb(listen_row_t *row, bool pictured) {
	if (pictured) {
		lv_obj_remove_style(row->thumb, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(row->thumb, LV_OPA_TRANSP, 0);
		lv_obj_set_style_radius(row->thumb, 6, 0);
		lv_obj_set_style_clip_corner(row->thumb, true, 0);
		lv_image_set_src(row->thumb, &row->image.dsc);
	} else {
		lv_image_set_src(row->thumb, &icon_podcast_list);
		lv_obj_add_style(row->thumb, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(row->thumb, LV_OPA_COVER, 0);
		lv_obj_set_style_clip_corner(row->thumb, false, 0);
	}
}

// The picture of row `k`: the cover of the file at `path`, or the microphone
// when there is no file to take it from.
static void listen_cover(int k, const char *path) {
	listen_row_t *row = &listen[k];
	char want[512] = "";
	if (path && path[0] && access(path, F_OK) == 0 && !growfile_is_growing(path)) {
		snprintf(want, sizeof(want), "%s", path);
	}
	if (strcmp(want, row->cover_path) == 0) {
		return;
	}
	coverloader_release(PODCASTCARD_SLOT_LISTENING + k);
	row->cover_waiting = false;
	listen_thumb(row, false);
	cover_free(&row->image);
	snprintf(row->cover_path, sizeof(row->cover_path), "%s", want);
	if (!want[0]) {
		return;
	}
	cover_image_t image;
	if (cover_thumb_cached(want, ROW_THUMB, &image)) {
		row->image = image;
		listen_thumb(row, true);
		return;
	}
	coverloader_request(PODCASTCARD_SLOT_LISTENING + k, want, ROW_THUMB);
	row->cover_waiting = true;
	art_wake();
}

static void listen_art_poll(void) {
	for (int k = 0; k < LISTEN_ROWS; k++) {
		listen_row_t *row = &listen[k];
		if (!row->cover_waiting) {
			continue;
		}
		cover_image_t image;
		bool finished = false;
		if (coverloader_take(PODCASTCARD_SLOT_LISTENING + k, &image, &finished)) {
			listen_thumb(row, false);
			cover_free(&row->image);
			row->image = image;
			listen_thumb(row, true);
			row->cover_waiting = false;
		} else if (finished) {
			row->cover_waiting = false;
		}
	}
}

// The title, the podcast and how much is left, on one row.
static void listen_fill(listen_row_t *row, const char *title, const char *show, double position, double duration) {
	lv_label_set_text(row->title, title);
	char left[32] = "";
	if (duration > position) {
		format_duration((int)(duration - position), left, sizeof(left));
	}
	char detail[256];
	if (left[0]) {
		char rest[64];
		snprintf(rest, sizeof(rest), tr("podcast_minutes_left"), left);
		snprintf(detail, sizeof(detail), "%.*s%s%s", 160, show, show[0] ? " \xC2\xB7 " : "", rest);
	} else {
		snprintf(detail, sizeof(detail), "%s", show);
	}
	lv_label_set_text(row->detail, detail);
	int permille = duration > 0 ? (int)(position * 1000.0 / duration) : 0;
	if (permille > 1000) {
		permille = 1000;
	}
	lv_bar_set_value(row->bar, permille, LV_ANIM_OFF);
}

// The list's rows start under the block above them, whatever it holds.
static void place_list_body(void) {
	if (!top_block || !list_body) {
		return;
	}
	bool any = !lv_obj_is_hidden(now_header) || !lv_obj_is_hidden(continue_header) ||
			   (card_empty && !lv_obj_is_hidden(card_empty));
	lv_obj_set_hidden(top_block, !any);
	int y = 0;
	if (any) {
		lv_obj_update_layout(top_block);
		y = lv_obj_get_height(top_block) + ROW_GAP;
	}
	if (lv_obj_get_y(list_body) != y) {
		lv_obj_set_y(list_body, y);
		window_update();
	}
	// The empty notice sits under whatever is above the list.
	if (list_empty && list_container) {
		lv_obj_update_layout(list_container);
		lv_obj_align_to(list_empty, list_container, LV_ALIGN_TOP_MID, 0, y + 60);
	}
}

static void refresh_listening(void) {
	if (!top_block) {
		return;
	}
	bool home = listen_home();

	static device_state_t now;
	device_state_get(&now);
	bool playing_one = home && !now.live && now.status != AUDIO_STATUS_STOPPED && podcastcache_is_episode(now.current_file);
	static char now_id_path[512];
	static long long now_id;
	if (playing_one) {
		if (strcmp(now_id_path, now.current_file) != 0) {
			snprintf(now_id_path, sizeof(now_id_path), "%s", now.current_file);
			now_id = podcastcache_episode_id(now.current_file);
		}
		listen_row_t *row = &listen[0];
		podcaststate_t known;
		bool described = podcaststate_get(now.current_file, &known) && known.title[0];
		memset(&row->state, 0, sizeof(row->state));
		snprintf(row->state.title, sizeof(row->state.title), "%.*s", (int)sizeof(row->state.title) - 1,
				 described ? known.title : now.metadata.title);
		snprintf(row->state.show, sizeof(row->state.show), "%.*s", (int)sizeof(row->state.show) - 1,
				 described ? known.show : now.metadata.album);
		listen_fill(row, row->state.title, row->state.show, now.progress_current_secs, now.progress_total_secs);
		lv_image_set_src(row->action_icon, now.status == AUDIO_STATUS_PLAYING ? &icon_pause_small : &icon_play_small);
		listen_cover(0, now.current_file);
		snprintf(listen_now_path, sizeof(listen_now_path), "%s", now.current_file);
	} else {
		listen_now_path[0] = '\0';
		listen_cover(0, NULL);
	}
	lv_obj_set_hidden(now_header, !playing_one);
	lv_obj_set_hidden(listen[0].button, !playing_one);

	int shown = 0;
	if (home) {
		static podcaststate_t found[LISTEN_ROWS * 2];
		int n = podcaststate_in_progress(found, LISTEN_ROWS * 2);
		for (int i = 0; i < n && shown < LISTEN_ROWS - 1; i++) {
			const podcaststate_t *st = &found[i];
			// The episode in the player is the row above.
			if (playing_one && ((st->path[0] && strcmp(st->path, now.current_file) == 0) ||
								(now_id > 0 && st->episode_id == now_id))) {
				continue;
			}
			bool on_card = st->path[0] && access(st->path, F_OK) == 0 && !growfile_is_growing(st->path);
			bool reachable = st->episode_id > 0 && !offline;
			if (!on_card && !reachable) {
				continue;
			}
			listen_row_t *row = &listen[1 + shown];
			row->state = *st;
			listen_fill(row, st->title, st->show, st->position, st->duration);
			listen_cover(1 + shown, on_card ? st->path : NULL);
			lv_obj_set_hidden(row->button, false);
			shown++;
		}
	}
	for (int k = 1 + shown; k < LISTEN_ROWS; k++) {
		lv_obj_set_hidden(listen[k].button, true);
		listen_cover(k, NULL);
	}
	lv_obj_set_hidden(continue_header, shown == 0);
	place_list_body();
}

// Once a second: how far the episode in the player has got, and whether it is
// playing. Anything more than that -- another episode, or none -- draws the
// block again.
static void listen_tick(void) {
	if (!top_block) {
		return;
	}
	static device_state_t now;
	device_state_get(&now);
	bool playing_one = listen_home() && !now.live && now.status != AUDIO_STATUS_STOPPED &&
					   podcastcache_is_episode(now.current_file);
	if (strcmp(playing_one ? now.current_file : "", listen_now_path) != 0) {
		refresh_listening();
		return;
	}
	if (!playing_one) {
		return;
	}
	listen_row_t *row = &listen[0];
	listen_fill(row, row->state.title, row->state.show, now.progress_current_secs, now.progress_total_secs);
	lv_image_set_src(row->action_icon, now.status == AUDIO_STATUS_PLAYING ? &icon_pause_small : &icon_play_small);
}

static void continue_play(const podcaststate_t *st) {
	// An episode of the catalogue goes back through its podcast's list when
	// the network allows, so the episodes after it follow it; a file on the
	// card, or anything without the network, plays from where it is.
	bool kept = st->path[0] && podcastdl_owns(st->path);
	if (st->episode_id > 0 && !offline && !kept) {
		browsing_author[0] = '\0';
		snprintf(browsing_image, sizeof(browsing_image), "%.*s", (int)sizeof(browsing_image) - 1, st->image);
		podcastpage_resume(st);
		return;
	}
	if (!podcastcard_play(st)) {
		toast_error(tr("podcast_that_episode_is_gone"));
	}
}

static void listen_row_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	listen_row_t *row = lv_event_get_user_data(e);
	// The round button on the row answers for itself.
	lv_obj_t *target = lv_event_get_target(e);
	if (target == row->action || target == row->action_icon) {
		return;
	}
	if (row == &listen[0]) {
		player_refresh_now_playing();
		switch_screen(player_screen);
		return;
	}
	continue_play(&row->state);
}

static void listen_action_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	listen_row_t *row = lv_event_get_user_data(e);
	if (row == &listen[0]) {
		player_key_play_pause();
		listen_tick();
		return;
	}
	continue_play(&row->state);
}

static lv_obj_t *make_header(lv_obj_t *parent, const char *text) {
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, tr(text));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_22_bold, 0);
	lv_obj_set_hidden(label, true);
	return label;
}

static void build_listen_row(listen_row_t *row, lv_obj_t *parent) {
	row->button = lv_btn_create(parent);
	lv_obj_set_size(row->button, lv_pct(100), LISTEN_ROW_HEIGHT);
	lv_obj_add_style(row->button, &theme_style_card, 0);
	lv_obj_add_style(row->button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row->button, 12, 0);
	lv_obj_set_style_border_width(row->button, 0, 0);
	lv_obj_set_style_shadow_width(row->button, 0, 0);
	lv_obj_set_style_pad_hor(row->button, 14, 0);
	lv_obj_set_style_pad_column(row->button, 12, 0);
	lv_obj_set_scrollable(row->button, false);
	lv_obj_set_flex_flow(row->button, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row->button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_event_bubble(row->button, true);
	lv_obj_add_event_cb(row->button, listen_row_cb, LV_EVENT_CLICKED, row);

	row->thumb = lv_image_create(row->button);
	lv_obj_set_size(row->thumb, ROW_THUMB, ROW_THUMB);
	lv_image_set_inner_align(row->thumb, LV_IMAGE_ALIGN_CENTER);
	lv_obj_set_event_bubble(row->thumb, true);
	listen_thumb(row, false);

	lv_obj_t *texts = lv_obj_create(row->button);
	lv_obj_remove_style_all(texts);
	lv_obj_set_flex_grow(texts, 1);
	lv_obj_set_height(texts, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_row(texts, 5, 0);
	lv_obj_set_event_bubble(texts, true);
	lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);

	row->title = lv_label_create(texts);
	lv_label_set_long_mode(row->title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(row->title, lv_pct(100));
	lv_obj_set_height(row->title, lv_font_get_line_height(&font_ui_20));
	lv_obj_add_style(row->title, &theme_style_text, 0);
	lv_obj_set_style_text_font(row->title, &font_ui_20, 0);

	row->detail = lv_label_create(texts);
	lv_label_set_long_mode(row->detail, LV_LABEL_LONG_DOT);
	lv_obj_set_width(row->detail, lv_pct(100));
	lv_obj_set_height(row->detail, lv_font_get_line_height(&font_ui_16));
	lv_obj_add_style(row->detail, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(row->detail, &font_ui_16, 0);

	row->bar = lv_bar_create(texts);
	lv_obj_set_size(row->bar, lv_pct(100), 5);
	lv_bar_set_range(row->bar, 0, 1000);
	lv_obj_set_style_bg_color(row->bar, theme()->surface_pressed, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(row->bar, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_bg_color(row->bar, theme()->accent, LV_PART_INDICATOR);
	lv_obj_set_style_radius(row->bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_radius(row->bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_set_event_bubble(row->bar, true);

	// Bubbling, so a swipe that starts on it still reaches the page.
	row->action = lv_btn_create(row->button);
	lv_obj_remove_style_all(row->action);
	lv_obj_set_size(row->action, 52, 52);
	lv_obj_set_style_radius(row->action, LV_RADIUS_CIRCLE, 0);
	lv_obj_add_style(row->action, &theme_style_accent_bg, 0);
	lv_obj_set_style_bg_opa(row->action, LV_OPA_COVER, 0);
	lv_obj_set_event_bubble(row->action, true);
	lv_obj_add_event_cb(row->action, listen_action_cb, LV_EVENT_CLICKED, row);
	row->action_icon = lv_image_create(row->action);
	lv_image_set_src(row->action_icon, &icon_play_small);
	lv_obj_set_style_image_recolor(row->action_icon, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(row->action_icon, LV_OPA_COVER, 0);
	lv_obj_center(row->action_icon);
	lv_obj_set_event_bubble(row->action_icon, true);

	lv_obj_set_hidden(row->button, true);
}

static void build_top_block(int width) {
	top_block = lv_obj_create(list_container);
	lv_obj_remove_style_all(top_block);
	lv_obj_set_size(top_block, width, LV_SIZE_CONTENT);
	lv_obj_set_pos(top_block, 0, 0);
	lv_obj_set_style_pad_row(top_block, 10, 0);
	lv_obj_set_scrollable(top_block, false);
	lv_obj_set_event_bubble(top_block, true);
	lv_obj_set_flex_flow(top_block, LV_FLEX_FLOW_COLUMN);

	now_header = make_header(top_block, "podcast_now_listening");
	build_listen_row(&listen[0], top_block);
	continue_header = make_header(top_block, "podcast_continue_listening");
	for (int k = 1; k < LISTEN_ROWS; k++) {
		build_listen_row(&listen[k], top_block);
	}
	lv_obj_set_hidden(top_block, true);
}

static void build_card_empty(lv_obj_t *parent) {
	card_empty = lv_obj_create(parent);
	lv_obj_remove_style_all(card_empty);
	lv_obj_set_size(card_empty, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_row(card_empty, 10, 0);
	lv_obj_set_scrollable(card_empty, false);
	lv_obj_set_event_bubble(card_empty, true);
	lv_obj_set_flex_flow(card_empty, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card_empty, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *icon = lv_image_create(card_empty);
	lv_image_set_src(icon, &icon_folder_open);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(icon, LV_OPA_60, 0);
	lv_obj_set_event_bubble(icon, true);

	lv_obj_t *title = lv_label_create(card_empty);
	lv_label_set_text(title, tr("podcast_card_empty_title"));
	lv_obj_add_style(title, &theme_style_text, 0);
	lv_obj_set_style_text_font(title, &font_ui_22, 0);

	lv_obj_t *text = lv_label_create(card_empty);
	lv_label_set_text(text, tr("podcast_card_empty_text"));
	lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(text, lv_pct(100));
	lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(text, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(text, &font_ui_18, 0);

	// What a filled folder looks like.
	lv_obj_t *tree = lv_obj_create(card_empty);
	lv_obj_set_size(tree, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_add_style(tree, &theme_style_card, 0);
	lv_obj_set_style_radius(tree, 12, 0);
	lv_obj_set_style_border_width(tree, 0, 0);
	lv_obj_set_style_shadow_width(tree, 0, 0);
	lv_obj_set_style_pad_all(tree, 14, 0);
	lv_obj_set_scrollable(tree, false);
	lv_obj_set_event_bubble(tree, true);
	lv_obj_t *tree_text = lv_label_create(tree);
	lv_label_set_text_fmt(tree_text, "%s/\n    %s/\n        cover.jpg\n        2026-10-05 %s.mp3", PODCASTDL_FOLDER,
						  tr("podcast_tree_show"), tr("podcast_tree_episode"));
	lv_obj_add_style(tree_text, &theme_style_text, 0);
	lv_obj_set_style_text_font(tree_text, &font_ui_18, 0);
	lv_obj_set_style_text_line_space(tree_text, 4, 0);

	lv_obj_set_hidden(card_empty, true);
}

// The empty card's explanation, in the list's place, only on that pill and
// only while the Podcast folder holds nothing.
static void refresh_card_empty(void) {
	if (!card_empty) {
		return;
	}
	bool show = list_kind == JOB_CARD && list_count == 0 && (!note_label || lv_obj_is_hidden(note_label));
	lv_obj_set_hidden(card_empty, !show);
}

// The line about the network, and which pills can be used.
static void refresh_offline(void) {
	if (!offline_banner) {
		return;
	}
	if (offline) {
		lv_label_set_text(offline_label,
						  tr(podcast_configured() ? "podcast_offline_no_wifi" : "podcast_offline_no_keys"));
		lv_obj_set_hidden(offline_banner, false);
		lv_obj_add_state(pill_followed, LV_STATE_DISABLED);
		lv_obj_add_state(pill_trending, LV_STATE_DISABLED);
	} else {
		lv_obj_set_hidden(offline_banner, true);
		lv_obj_remove_state(pill_followed, LV_STATE_DISABLED);
		lv_obj_remove_state(pill_trending, LV_STATE_DISABLED);
	}
	refresh_pills();
}

static void build_page(gui_config_t *cfg) {
	g_cfg = cfg;
	podcast_screen = lv_obj_create(NULL);
	lv_obj_add_event_cb(podcast_screen, list_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(podcast_screen, list_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	podcast_list_screen = podcast_screen;
	lv_obj_add_style(podcast_screen, &theme_style_screen, 0);

	// The title follows what is on screen: "Podcasts" on the three home lists, the
	// query on a search.
	list_title = settingsrow_title(podcast_screen, cfg, "podcasts");
	settingsrow_title_corner_slots(list_title, cfg, 2);

	int content_top = settingsrow_content_top(cfg);

	// The column: the pills and the list, one above the other. Not
	// settingsrow_page(), because that gives a single container and fill_list()
	// starts it with an lv_obj_clean() -- which would take the pills with it at
	// every fill.
	lv_obj_t *column = lv_obj_create(podcast_screen);
	lv_obj_set_size(column, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(column, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(column, 0, 0);
	lv_obj_set_style_border_width(column, 0, 0);
	lv_obj_set_style_radius(column, 0, 0);
	lv_obj_set_style_pad_all(column, 0, 0);
	lv_obj_set_style_pad_gap(column, 12, 0);
	lv_obj_set_scrollable(column, false);
	lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(column, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	pill_row = lv_obj_create(column);
	lv_obj_set_size(pill_row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(pill_row, 0, 0);
	lv_obj_set_style_border_width(pill_row, 0, 0);
	lv_obj_set_style_pad_ver(pill_row, 0, 0);
	lv_obj_set_style_pad_hor(pill_row, cfg->padding, 0);
	lv_obj_set_style_pad_gap(pill_row, 10, 0);
	// Sideways when a language's three names do not fit the width.
	lv_obj_set_scroll_dir(pill_row, LV_DIR_HOR);
	lv_obj_set_scrollbar_mode(pill_row, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_flex_flow(pill_row, LV_FLEX_FLOW_ROW);
	// Presses that miss a pill belong to the column below, which carries the
	// swipe-back and the player sheet.
	lv_obj_set_event_bubble(pill_row, true);

	pill_followed = make_pill(pill_row, "podcast_pill_followed", JOB_FOLLOWED);
	pill_card = make_pill(pill_row, "podcast_pill_card", JOB_CARD);
	pill_trending = make_pill(pill_row, "podcast_pill_trending", JOB_TRENDING);

	build_offline_banner(column, cfg);
	refresh_pills();
	theme_register_refresh(refresh_pills);

	list_container = lv_obj_create(column);
	lv_obj_set_width(list_container, lv_pct(100));
	lv_obj_set_flex_grow(list_container, 1);
	lv_obj_set_style_bg_opa(list_container, 0, 0);
	lv_obj_set_style_border_width(list_container, 0, 0);
	lv_obj_set_style_radius(list_container, 0, 0);
	lv_obj_set_style_pad_hor(list_container, cfg->padding, 0);
	lv_obj_set_style_pad_ver(list_container, 0, 0);
	lv_obj_set_scroll_dir(list_container, LV_DIR_VER);

	// The body: no layout, no background, and exactly as tall as the whole list.
	// It is the only child the container has, and the twelve rows are placed
	// inside it by hand -- flex and lv_obj_set_y() cannot both decide where a
	// row goes.
	int row_width = cfg->screen_width - 2 * cfg->padding;
	list_body = lv_obj_create(list_container);
	lv_obj_set_width(list_body, row_width);
	lv_obj_set_height(list_body, ROW_PITCH);
	lv_obj_set_pos(list_body, 0, 0);
	lv_obj_set_style_bg_opa(list_body, 0, 0);
	lv_obj_set_style_border_width(list_body, 0, 0);
	lv_obj_set_style_pad_all(list_body, 0, 0);
	lv_obj_set_scrollable(list_body, false);
	// Presses go row -> body -> container; without this they stop here and the
	// swipes never see a gesture that began on a row.
	lv_obj_set_event_bubble(list_body, true);
	build_rows(row_width);

	// Above the rows, scrolling with them: the episodes being listened to, and
	// what to do with an empty Podcast folder.
	build_top_block(row_width);
	build_card_empty(top_block);

	// On the screen and not inside the container: it has to sit in the middle of
	// the page, and inside the body it would have to be positioned against a
	// height that changes with the length of a list that, in this case, has no
	// entries at all.
	list_empty = lv_label_create(podcast_screen);
	lv_label_set_text(list_empty, tr("nothing_to_show"));
	lv_label_set_long_mode(list_empty, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(list_empty, cfg->screen_width - 4 * cfg->padding);
	lv_obj_set_style_text_align(list_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(list_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(list_empty, &font_ui_24, 0);
	lv_obj_align(list_empty, LV_ALIGN_TOP_MID, 0, content_top + 140);
	lv_obj_set_hidden(list_empty, true);

	// The same spot, for when the section cannot work at all.
	note_label = lv_label_create(podcast_screen);
	lv_label_set_text(note_label, "");
	lv_label_set_long_mode(note_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note_label, cfg->screen_width - 2 * cfg->padding);
	lv_obj_add_style(note_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note_label, &font_ui_22, 0);
	lv_obj_set_style_text_align(note_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(note_label, LV_ALIGN_TOP_LEFT, cfg->padding, content_top + 14);
	lv_obj_set_hidden(note_label, true);

	// The "Wi-Fi settings" row under the network notice, as on the transfer
	// page. Created hidden; show_note puts it under the notice.
	note_wifi_row = settingsrow_add(podcast_screen, "wi_fi_settings", NULL, note_wifi_row_cb, NULL);
	lv_obj_set_width(note_wifi_row, cfg->screen_width - 2 * cfg->padding);
	lv_obj_set_hidden(note_wifi_row, true);

	lv_obj_add_event_cb(list_container, list_scrolled_cb, LV_EVENT_SCROLL, NULL);
	switcher_attach_back_gesture(podcast_screen);
	switcher_attach_back_gesture(list_container);
	// The column covers the whole page and is clickable, as every LVGL object
	// is: without its own handlers it swallows both gestures, and with the
	// notice up -- where the list is hidden and the column is all there is --
	// the swipe back would do nothing at all.
	switcher_attach_back_gesture(column);
	player_sheet_attach_drag(column, true);
	switcher_set_back_guard(podcast_screen, list_back_guard);
	switcher_set_back_guard_peek(podcast_screen, list_back_has_step);
	player_sheet_attach_drag(podcast_screen, true);
	player_sheet_attach_drag(list_container, true);
}

// ---------------------------------------------------------------------------
// the search
// ---------------------------------------------------------------------------

static lv_obj_t *search_field;
static lv_obj_t *search_clear_btn;
static keyboard_t *search_keyboard;

static void refresh_search_clear(void) {
	if (!search_clear_btn) {
		return;
	}
	const char *text = lv_textarea_get_text(search_field);
	if (text && text[0]) {
		lv_obj_set_hidden(search_clear_btn, false);
	} else {
		lv_obj_set_hidden(search_clear_btn, true);
	}
}

static void search_field_changed_cb(lv_event_t *e) {
	(void)e;
	refresh_search_clear();
}

static void search_clear_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(search_field, "");
	keyboard_reset(search_keyboard);
	lv_obj_add_state(search_field, LV_STATE_FOCUSED);
	lv_obj_send_event(search_field, LV_EVENT_FOCUSED, NULL);
	refresh_search_clear();
}

static void search_accept_cb(lv_event_t *e) {
	(void)e;
	const char *text = lv_textarea_get_text(search_field);
	if (!text || !text[0]) {
		return;
	}
	job_t job = {0};
	job.kind = JOB_SEARCH;
	snprintf(job.text, sizeof(job.text), "%s", text);
	run(&job);
}

// Copied line for line from Tidal's, deliberately: search in this player has
// one shape -- the 62-high field with 12 radius, the x on the right edge, the
// 316-high keyboard below -- and a page that invents another is instantly not
// of this house.
static void build_search_page(gui_config_t *cfg) {
	search_screen = lv_obj_create(NULL);
	lv_obj_add_style(search_screen, &theme_style_screen, 0);

	settingsrow_title(search_screen, cfg, "podcast_search_for_a_podcast");
	int top = settingsrow_content_top(cfg);

	search_field = lv_textarea_create(search_screen);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_placeholder_text(search_field, tr("podcast_name"));
	lv_obj_set_size(search_field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(search_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(search_field, LV_ALIGN_TOP_LEFT, cfg->padding, top);
	lv_obj_add_style(search_field, &theme_style_card, 0);
	lv_obj_set_style_radius(search_field, 12, 0);
	lv_obj_set_style_border_width(search_field, 0, 0);
	lv_obj_set_style_shadow_width(search_field, 0, 0);
	lv_obj_set_style_pad_all(search_field, 14, 0);
	lv_obj_set_style_text_font(search_field, &font_ui_24, 0);
	keyboard_style_caret(search_field);
	lv_obj_add_event_cb(search_field, search_field_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

	// The x on the field's right edge: clears the query with one tap. Hidden
	// while the field is empty.
	search_clear_btn = lv_btn_create(search_screen);
	lv_obj_set_size(search_clear_btn, 56, 56);
	lv_obj_align(search_clear_btn, LV_ALIGN_TOP_RIGHT, -cfg->padding - 4, top + (62 - 56) / 2);
	lv_obj_set_style_bg_opa(search_clear_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(search_clear_btn, 0, 0);
	lv_obj_set_style_border_width(search_clear_btn, 0, 0);
	lv_obj_set_style_pad_all(search_clear_btn, 0, 0);
	lv_obj_set_hidden(search_clear_btn, true);
	lv_obj_add_event_cb(search_clear_btn, search_clear_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *search_clear_icon = lv_image_create(search_clear_btn);
	lv_image_set_src(search_clear_icon, &icon_clear);
	lv_obj_add_style(search_clear_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(search_clear_icon, LV_OPA_70, 0);
	lv_obj_center(search_clear_icon);

	// Room for it, so a long query does not run underneath.
	lv_obj_set_style_pad_right(search_field, 60, 0);

	// No search-type pills, unlike Qobuz and Tidal: there the search runs over
	// tracks, albums and artists, here there is one thing only to search for.
	search_keyboard =
		keyboard_create(search_screen, cfg->screen_width, 316, search_field, NULL, "ok", search_accept_cb, NULL);
	switcher_attach_back_gesture(search_screen);
}

// ---------------------------------------------------------------------------
// the pills, and the page state
// ---------------------------------------------------------------------------

// "My podcasts" does NOT go through the worker.
//
// The followed list is in memory (podcastsubs.h reads it from the card once):
// queueing it on a thread means a veil that flashes, a screen change when the
// result comes back, and -- since the page refreshes it at every entry -- a loop
// that bites its own tail: the result changed screen, the screen change started
// the refresh again. There is nothing to wait for here: fill and be done.
//
// Not done while the worker is working: the result arrays are the same ones, and
// rewriting them under it is the one way to get this wrong.
static bool show_followed_now(void) {
	pthread_mutex_lock(&job_lock);
	bool busy = have_pending || result_pending;
	pthread_mutex_unlock(&job_lock);
	if (busy || inflight_valid) {
		return false;
	}

	result_error[0] = '\0';
	result_kind = JOB_FOLLOWED;
	result_count = podcastsubs_list(result_feeds, PODCAST_MAX_HELD);
	snprintf(result_title, sizeof(result_title), "%s", tr("podcast_my_podcasts"));
	fill_list(0);

	// This is the section's root: the list stack restarts here.
	job_t root = {0};
	root.kind = JOB_FOLLOWED;
	snprintf(root.text, sizeof(root.text), "%s", result_title);
	current_list_job = root;
	have_current_list = true;
	list_history_len = 0;
	return true;
}

// The podcasts on the card, the same way: they are read from the card here and
// now, with nothing to wait for.
static bool show_card_now(void) {
	pthread_mutex_lock(&job_lock);
	bool busy = have_pending || result_pending;
	pthread_mutex_unlock(&job_lock);
	if (busy || inflight_valid) {
		return false;
	}

	result_error[0] = '\0';
	result_kind = JOB_CARD;
	result_count = podcastcard_shows(card_shows, PODCASTCARD_MAX_SHOWS);
	snprintf(result_title, sizeof(result_title), "%s", tr("podcast_pill_card"));
	fill_list(0);

	job_t root = {0};
	root.kind = JOB_CARD;
	snprintf(root.text, sizeof(root.text), "%s", result_title);
	current_list_job = root;
	have_current_list = true;
	list_history_len = 0;
	return true;
}

static void run_home_view(job_kind_t kind) {
	if (kind == JOB_FOLLOWED && show_followed_now()) {
		return;
	}
	if (kind == JOB_CARD) {
		show_card_now();
		return;
	}
	job_t job = {0};
	job.kind = kind;
	switch (kind) {
	case JOB_TRENDING:
		snprintf(job.text, sizeof(job.text), "%s", tr("podcast_trending"));
		break;
	case JOB_FOLLOWED:
		snprintf(job.text, sizeof(job.text), "%s", tr("podcast_my_podcasts"));
		break;
	default:
		return;
	}
	run_ex(&job, true); // a pill is the root: it clears the stack
}

static void pill_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	run_home_view((job_kind_t)(intptr_t)lv_event_get_user_data(e));
}

static void open_search_cb(lv_event_t *e) {
	(void)e;
	switch_screen(search_screen);
}

static void open_settings_cb(lv_event_t *e) {
	(void)e;
	switch_screen(settings_screen);
}

static lv_obj_t *corner_button(gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph, lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(podcast_screen);
	lv_obj_set_size(button, 56, 56);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * (56 + 6), cfg->padding + cfg->top_bar_height);

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);

	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
	return button;
}

// Written out rather than borrowed: the function of this name in tidalpage.c is
// a static of that file. Calling it from here compiles on the host, where the
// preprocessor removes the branch that uses it, and does not link on the
// device.
static bool network_up(void) {
#ifdef HOST_BUILD
	return true; // the simulator runs on a machine with a network and no wlan0
#else
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
#endif
}

// The row under the network notice: it leads to the Wi-Fi settings.
static void note_wifi_row_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

// Shows the notice in place of everything else, or takes it away. The keys and
// the Wi-Fi can change while the user is elsewhere, so this is rechecked at
// every entry into the page.
static void show_note(const char *text) {
	if (!note_label) {
		return;
	}
	if (text) {
		bool wifi_note = strcmp(text, "enable_wi_fi_first") == 0;
		lv_label_set_text(note_label, tr(text));
		// The Wi-Fi notice uses the NORMAL text colour (as on the other pages);
		// the other notices stay quiet.
		if (wifi_note) {
			lv_obj_set_style_text_color(note_label, theme()->text_primary, 0);
		} else {
			lv_obj_remove_local_style_prop(note_label, LV_STYLE_TEXT_COLOR, 0);
		}
		lv_obj_set_hidden(note_label, false);
		lv_obj_set_hidden(pill_row, true);
		lv_obj_set_hidden(list_container, true);
		lv_obj_set_hidden(list_empty, true);
		lv_obj_set_hidden(offline_banner, true);
		lv_obj_set_hidden(card_empty, true);
		// The NETWORK notice brings the "Wi-Fi settings" row with it, as on the
		// transfer page: it says what is missing AND where it is fixed. The other
		// notices (the keys) do not.
		if (note_wifi_row) {
			if (wifi_note) {
				lv_obj_set_hidden(note_wifi_row, false);
				lv_obj_update_layout(note_label); // the notice's real height
				lv_obj_align_to(note_wifi_row, note_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);
			} else {
				lv_obj_set_hidden(note_wifi_row, true);
			}
		}
	} else {
		lv_obj_set_hidden(note_label, true);
		if (note_wifi_row) {
			lv_obj_set_hidden(note_wifi_row, true);
		}
		lv_obj_set_hidden(list_container, false);
	}
}

// The network state the page is currently drawn for, so the watcher below can
// tell a change from a poll that found nothing new.
static bool note_network_up;

// Only the notices and the corner buttons: it starts nothing. It is also called
// from the return of a failed job, where starting a job would be an endless
// loop.
static bool refresh_notes(void) {
	if (!note_label) {
		return false;
	}

	// Recorded before anything can return: the watcher below compares against
	// it, and leaving it behind on the path where the keys are missing would
	// have it find a difference on every tick.
	note_network_up = network_up();

	// Without the keys or the network the catalogue is out of reach, but the
	// podcasts on the card are not: the page stays, on those, with a line
	// saying what is missing.
	offline = !podcast_configured() || !note_network_up;
	refresh_corner_buttons();

	// Without a card there is nothing at all: even an episode streamed from the
	// catalogue is written to the card first.
	if (!gui_card_available()) {
		show_note("sd_card_missing");
		return false;
	}

	show_note(NULL);
	refresh_offline();
	return true;
}

static void refresh_page_state(void) {
	if (!refresh_notes()) {
		return;
	}

	if (offline) {
		run_home_view(JOB_CARD);
		return;
	}
	backfill_start();

	if (!have_current_list) {
		// First entry: start from the followed list, which asks nothing of the
		// network and is there anyway.
		run_home_view(JOB_FOLLOWED);
		return;
	}

	// Re-entering the section rereads the followed list: a podcast may have been
	// followed, or unfollowed, from the player's star while the user was
	// elsewhere.
	if (current_list_job.kind == JOB_FOLLOWED || current_list_job.kind == JOB_CARD) {
		run_home_view(current_list_job.kind);
		return;
	}
	// Anything else is a list from the catalogue, still on screen: only what
	// has been heard meanwhile changes.
	refresh_listening();
}

static void page_loaded_cb(lv_event_t *e) {
	(void)e;
	states_seen = podcaststate_revision();
	refresh_page_state();
}

// What has been heard changes under the page while the player sits over it as
// a sheet, and closing the sheet does not load the page again: the states are
// followed once a second instead, while the page is in sight.

static void states_follow(void) {
	if (lv_screen_active() != podcast_screen || player_sheet_is_open()) {
		return;
	}
	unsigned now = podcaststate_revision();
	if (now == states_seen) {
		listen_tick();
		return;
	}
	states_seen = now;
	if (list_kind == JOB_CARD && current_list_job.kind == JOB_CARD) {
		// The same podcasts with new counts keep their rows, their pictures
		// and the scroll; anything else is drawn again.
		static podcastcard_show_t fresh[PODCASTCARD_MAX_SHOWS];
		int n = podcastcard_shows(fresh, PODCASTCARD_MAX_SHOWS);
		bool same = n == list_count;
		for (int i = 0; i < n && same; i++) {
			same = strcmp(fresh[i].folder, card_shows[i].folder) == 0;
		}
		if (same) {
			memcpy(card_shows, fresh, sizeof(fresh[0]) * (size_t)n);
			for (int i = 0; i < ROW_POOL; i++) {
				rows[i].index = -2;
			}
			window_update();
		} else {
			show_card_now();
		}
	}
	refresh_listening();
}

// The Wi-Fi can be switched on while this page is already open, so the page
// follows it once a second, as the radio hub does; otherwise the notice stays
// up until the section is left and entered again.
static void wifi_watch_cb(lv_timer_t *timer) {
	(void)timer;
	if (lv_screen_active() != podcast_screen) {
		return;
	}
	states_follow();
	bool up = network_up();
	if (up == note_network_up) {
		return;
	}

	bool was_offline = offline;
	if (!refresh_notes()) {
		return;
	}
	if (offline && !was_offline) {
		// The network went: the card is what is left.
		run_home_view(JOB_CARD);
	} else if (!offline && was_offline) {
		// Back: the catalogue's pills work again, and the card stays where it
		// was until another is picked.
		refresh_listening();
	}
}

// ---------------------------------------------------------------------------
// the podcast settings
//
// One page, with the two skip amounts. SEPARATE from the audiobooks' ones and
// not a link to their page: the two are alike but are not listened to the same
// way, and wanting -10/+30 on a podcast and -30/+10 on a book must not be a
// choice between them. See podcast.h.
// ---------------------------------------------------------------------------

#define SKIP_CHOICES 3

static lv_obj_t *skip_back_choice[SKIP_CHOICES];
static lv_obj_t *skip_forward_choice[SKIP_CHOICES];
static const int SKIP_VALUES[SKIP_CHOICES] = {PODCAST_SKIP_SHORT, PODCAST_SKIP_LONG, PODCAST_SKIP_HUGE};

static void refresh_skip_buttons(void) {
	int back = podcast_skip_back();
	int forward = podcast_skip_forward();
	for (int i = 0; i < SKIP_CHOICES; i++) {
		paint_pill(skip_back_choice[i], SKIP_VALUES[i] == back);
		paint_pill(skip_forward_choice[i], SKIP_VALUES[i] == forward);
	}
}

static void pick_skip_back_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	podcast_set_skip_back((int)(intptr_t)lv_event_get_user_data(e));
	refresh_skip_buttons();
	player_refresh_now_playing(); // the button it names changes its glyph
}

static void pick_skip_forward_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	podcast_set_skip_forward((int)(intptr_t)lv_event_get_user_data(e));
	refresh_skip_buttons();
	player_refresh_now_playing();
}

static lv_obj_t *make_skip_choice(lv_obj_t *parent, const char *text, int seconds, lv_event_cb_t cb) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 64);
	lv_obj_set_style_pad_hor(btn, 22, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)seconds);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_center(label);
	return btn;
}

// One card per button: its name, then its three pills. The same shape as the
// audiobooks' "Change controls" page, because it is the same choice.
static lv_obj_t *make_skip_card(lv_obj_t *parent, const char *title) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 20, 0);
	lv_obj_set_style_pad_gap(card, 18, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_event_bubble(card, true);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *name = lv_label_create(card);
	lv_label_set_text(name, tr(title));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);

	lv_obj_t *row = lv_obj_create(card);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, 0, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_gap(row, 14, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_event_bubble(row, true);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	return row;
}

static void settings_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_skip_buttons();
}

// The two skip buttons, on a page of their own. Same arrangement as the
// audiobooks': the settings page lists what the section does, and how the two
// side buttons are laid out is one thing among those rather than the first
// thing anybody reads.
static lv_obj_t *controls_screen;

static void build_controls_page(gui_config_t *cfg) {
	controls_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(controls_screen, cfg, "change_controls");

	lv_obj_t *back_row = make_skip_card(container, "back_button");
	skip_back_choice[0] = make_skip_choice(back_row, "-10", PODCAST_SKIP_SHORT, pick_skip_back_cb);
	skip_back_choice[1] = make_skip_choice(back_row, "-30", PODCAST_SKIP_LONG, pick_skip_back_cb);
	skip_back_choice[2] = make_skip_choice(back_row, "-60", PODCAST_SKIP_HUGE, pick_skip_back_cb);

	lv_obj_t *forward_row = make_skip_card(container, "forward_button");
	skip_forward_choice[0] = make_skip_choice(forward_row, "+10", PODCAST_SKIP_SHORT, pick_skip_forward_cb);
	skip_forward_choice[1] = make_skip_choice(forward_row, "+30", PODCAST_SKIP_LONG, pick_skip_forward_cb);
	skip_forward_choice[2] = make_skip_choice(forward_row, "+60", PODCAST_SKIP_HUGE, pick_skip_forward_cb);

	refresh_skip_buttons();
	theme_register_refresh(refresh_skip_buttons);
	lv_obj_add_event_cb(controls_screen, settings_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(controls_screen);
}

static lv_obj_t *stop_episode_switch;
static settingsrow_duration_t sleep_row;
static lv_obj_t *order_pill[2];
static lv_obj_t *margin_pill[3];
static const int MARGIN_VALUES[3] = {30, 60, 120};

static void refresh_settings_rows(void) {
	if (stop_episode_switch) {
		if (podcast_stop_at_episode_end()) {
			lv_obj_add_state(stop_episode_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(stop_episode_switch, LV_STATE_CHECKED);
		}
	}
	// The control centre switches this timer too, so the switch and the
	// wheels are read again rather than trusted from the last visit.
	settingsrow_duration_set_minutes(&sleep_row, sleeptimer_minutes(SLEEPTIMER_PODCAST));
	settingsrow_duration_expanded(&sleep_row, sleeptimer_enabled(SLEEPTIMER_PODCAST));
	settingsrow_duration_repaint(&sleep_row);
	bool oldest = podcastcard_oldest_first();
	settingsrow_pill_active(order_pill[0], !oldest);
	settingsrow_pill_active(order_pill[1], oldest);
	int margin = podcaststate_margin();
	for (int i = 0; i < 3; i++) {
		settingsrow_pill_active(margin_pill[i], MARGIN_VALUES[i] == margin);
	}
}

static void order_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	podcastcard_set_oldest_first((int)(intptr_t)lv_event_get_user_data(e) != 0);
	refresh_settings_rows();
}

static void margin_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	podcaststate_set_margin((int)(intptr_t)lv_event_get_user_data(e));
	refresh_settings_rows();
}

static void import_opml_row_cb(lv_event_t *e) {
	(void)e;
	podcastopml_open_import();
}

static void export_opml_cb(lv_event_t *e) {
	(void)e;
	podcastopml_export();
}

static void settings_rows_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_settings_rows();
}

static void stop_episode_cb(lv_event_t *e) {
	podcast_set_stop_at_episode_end(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void sleep_toggle_cb(lv_event_t *e) {
	(void)e;
	sleeptimer_set_enabled(SLEEPTIMER_PODCAST, lv_obj_has_state(sleep_row.toggle, LV_STATE_CHECKED));
	refresh_settings_rows();
}

static void sleep_wheel_cb(lv_event_t *e) {
	(void)e;
	sleeptimer_set_minutes(SLEEPTIMER_PODCAST, settingsrow_duration_minutes(&sleep_row));
}

static void build_settings_page(gui_config_t *cfg) {
	build_controls_page(cfg);

	settings_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(settings_screen, cfg, "podcast_settings");

	settingsrow_add(container, "change_controls", NULL, switch_screen_cb, controls_screen);
	settingsrow_toggle(container, "podcast_stop_at_end_of_episode", &stop_episode_switch, stop_episode_cb);
	// Its own, and not the music one: a stretch set for falling asleep to a
	// podcast should not be counting down over an album the next morning.
	settingsrow_toggle_duration(container, "sleep_timer", sleep_toggle_cb, sleep_wheel_cb, &sleep_row);
	settingsrow_duration_set_minutes(&sleep_row, sleeptimer_minutes(SLEEPTIMER_PODCAST));

	// The subscriptions, in and out of other apps.
	settingsrow_add(container, "podcast_opml_import", NULL, import_opml_row_cb, NULL);
	settingsrow_action(container, "podcast_opml_export", export_opml_cb, NULL);

	// The podcasts on the card.
	lv_obj_t *pills = NULL;
	settingsrow_pills(container, "podcast_episode_order", &pills);
	order_pill[0] = settingsrow_pill(pills, "podcast_order_newest", 0, order_cb);
	order_pill[1] = settingsrow_pill(pills, "podcast_order_oldest", 1, order_cb);
	settingsrow_pills(container, "podcast_mark_heard_at", &pills);
	margin_pill[0] = settingsrow_pill_text(pills, "30 s", 30, margin_cb);
	margin_pill[1] = settingsrow_pill_text(pills, "60 s", 60, margin_cb);
	margin_pill[2] = settingsrow_pill_text(pills, "2 min", 120, margin_cb);

	refresh_settings_rows();
	theme_register_refresh(refresh_settings_rows);
	lv_obj_add_event_cb(settings_screen, settings_rows_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	// And when the control centre switches the timer with the page under it.
	lv_obj_add_event_cb(settings_screen, settings_rows_loaded_cb, LV_EVENT_REFRESH, NULL);
	switcher_attach_back_gesture(settings_screen);
}

// ---------------------------------------------------------------------------
// from outside
// ---------------------------------------------------------------------------

lv_obj_t *podcastpage_settings_screen(void) { return settings_screen; }

void podcastpage_open_followed(void) {
	switch_screen_return_to(podcast_screen);
	if (!offline) {
		run_home_view(JOB_FOLLOWED);
	}
}

// ---------------------------------------------------------------------------
// the feed addresses of podcasts followed before they were kept
//
// Asked of the catalogue once per run, in the background, the first time the
// page opens with a network: one request per podcast missing it.
// ---------------------------------------------------------------------------

typedef struct {
	long long id;
	char url[PODCAST_FEED_URL_MAX];
} backfill_t;

static bool backfill_done;

static void backfill_apply_cb(void *user) {
	backfill_t *b = user;
	podcastsubs_set_url(b->id, b->url);
	free(b);
}

static void *backfill_main(void *arg) {
	long long *ids = arg;
	thread_be_background("podcast urls");
	for (int i = 0; ids[i] > 0; i++) {
		podcast_feed_t feed;
		if (!podcast_feed(ids[i], &feed) || !feed.url[0]) {
			continue;
		}
		backfill_t *b = calloc(1, sizeof(*b));
		if (!b) {
			break;
		}
		b->id = ids[i];
		snprintf(b->url, sizeof(b->url), "%s", feed.url);
		if (!gui_post(backfill_apply_cb, b)) {
			free(b);
		}
	}
	free(ids);
	return NULL;
}

static void backfill_start(void) {
	if (backfill_done || offline) {
		return;
	}
	backfill_done = true;
	long long *ids = calloc(PODCASTSUBS_MAX + 1, sizeof(*ids));
	if (!ids) {
		return;
	}
	int n = podcastsubs_missing_urls(ids, PODCASTSUBS_MAX);
	if (n == 0) {
		free(ids);
		return;
	}
	pthread_t thread;
	if (pthread_create(&thread, NULL, backfill_main, ids) == 0) {
		pthread_detach(thread);
	} else {
		free(ids);
	}
}

bool podcastpage_open_feed(long long feed_id, const char *title) {
	if (feed_id <= 0 || !podcast_configured()) {
		return false;
	}
	// Arriving from the player's "Show podcast", only the id and the name are
	// known: the page asks the catalogue for the rest.
	podcast_feed_t feed;
	memset(&feed, 0, sizeof(feed));
	feed.id = feed_id;
	snprintf(feed.title, sizeof(feed.title), "%s", title && title[0] ? title : tr("podcasts"));
	podcastcard_open_feed(&feed);
	return true;
}

// Once a second: protects from pruning the episodes the queue still has ahead,
// and clears the waiting flag when the queue has moved on to something that is
// not a podcast.
static void queue_watch_cb(lv_timer_t *timer) {
	(void)timer;
	// A queue that is a library list holds no cache files, and walking it here
	// would be a database read per entry, once a second, for nothing. The
	// bookkeeping below still runs: a download already in flight keeps the
	// radio awake whether or not the queue moved off it.
	bool library_queue = playlist_is_library_backed();
	if (library_queue) {
		podcastcache_set_protected(NULL, 0);
		waiting_for_episode = false;
	}

	int count = playlist_count();
	int current = playlist_current_index();

	static char queued[PODCASTCACHE_PROTECTED_MAX][512];
	static const char *queued_ptr[PODCASTCACHE_PROTECTED_MAX];
	int protect = 0;
	for (int i = current; !library_queue && i >= 0 && i < count && protect < PODCASTCACHE_PROTECTED_MAX; i++) {
		if (playlist_path_at(i, queued[protect], sizeof(queued[0]))) {
			queued_ptr[protect] = queued[protect];
			protect++;
		}
	}
	if (!library_queue) {
		podcastcache_set_protected(queued_ptr, protect);
	}

	if (waiting_for_episode) {
		char wanted[512];
		if (current < 0 || !playlist_path_at(current, wanted, sizeof(wanted)) || !podcastcache_owns(wanted)) {
			waiting_for_episode = false;
		}
	}

	// Nothing further is asked of the network while the Wi-Fi transfer is on:
	// the episode would land on a card a server outside this process is
	// writing, and the wait it feeds is a wait to start playing, which is what
	// switching the transfer on put a stop to. The queued files stay protected
	// above either way -- they are still the queue.
	pthread_mutex_lock(&fetch_lock);
	if (wifitransfer_get_enabled()) {
		waiting_for_episode = false;
		fetch_want_id = 0;
	}
	bool downloading = fetch_want_id != 0;
	pthread_mutex_unlock(&fetch_lock);
	podcastcache_set_network_wanted(downloading || waiting_for_episode);
}

void podcastpage_init(gui_config_t *cfg) {
	for (int i = 0; i < ROW_ART_SLOTS; i++) {
		slot_owner[i] = -1;
	}
	for (int i = 0; i < ROW_POOL; i++) {
		card_slot_entry[i] = -1;
	}
	for (int i = 0; i < PODCAST_MAX_HELD; i++) {
		row_image_source[i] = -1;
	}

	build_page(cfg);

	// Slots are counted from the edge leftwards: the gear against it, the
	// magnifier beside it. The downloaded episodes are the "on the card" pill.
	settings_btn = corner_button(cfg, 0, &icon_music_settings, open_settings_cb);
	search_btn = corner_button(cfg, 1, &icon_search, open_search_cb);

	build_search_page(cfg);
	build_settings_page(cfg);
	podcastcard_init(cfg);
	podcastopml_init(cfg);
	dl_build(cfg);
	build_busy_layer();
	podcastcache_set_slow_start_cb(NULL);

	device_state_add_prepare_cb(podcastpage_prepare_track);
	fetch_start_thread();

	lv_timer_create(queue_watch_cb, 1000, NULL);
	lv_timer_create(wifi_watch_cb, 1000, NULL);
	art_drop_timer = lv_timer_create(art_drop_cb, ART_DROP_DELAY_MS, NULL);
	lv_timer_pause(art_drop_timer);
	art_timer = lv_timer_create(art_poll_cb, 120, NULL);
	lv_timer_pause(art_timer);
	qobuzart_set_root(storage_sd_root());

	podcastcache_set_root(storage_sd_root());
	podcastsubs_set_root(storage_sd_root());

	// Notices only: the first list is asked for by the page when it really
	// opens, not at player startup -- at startup the card may not have been read
	// yet and the Wi-Fi may not be up.
	refresh_notes();

	lv_obj_add_event_cb(podcast_screen, page_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}
