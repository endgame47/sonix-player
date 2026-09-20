#include "player.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "src/core/lv_obj.h"
#include "src/core/lv_obj_pos.h"
#include "src/gui/library/browser.h"
#include "src/gui/nowplaying/chapters.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/library/medialist.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/quickpanel.h"
#include "src/gui/shell/scrolltext.h"
#include "src/gui/nowplaying/trackmenu.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/misc/lv_area.h"
#include "src/system/device/power.h"
#include "src/system/playback/sleeptimer.h"
#include "src/system/gearboy/gearboy.h"
#include "src/system/streaming/radio.h"
#include "src/system/audio/audio.h"
#include "src/system/playback/audiobook.h"
#include "src/system/playback/device_state.h"
#include "src/system/remote/airplay.h"
#include "src/system/remote/dlna.h"
#include "src/system/bluetooth/btreceiver.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/device/led.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/podcastsubs.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/qobuzsync.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/streaming/tidalsync.h"
#include "src/system/core/utils.h"
#include "src/system/audio/waveform.h"

#include "lvgl/lvgl.h"

lv_obj_t *player_screen;

static lv_obj_t *player_menu;
static lv_obj_t *play_btn;
static lv_obj_t *play_btn_icon;
static lv_obj_t *song_title_label;
static lv_obj_t *song_artist_label;
static lv_obj_t *format_label; // "16/44.1 FLAC", right of the artist line
static lv_obj_t *fav_btn_obj;  // the button it sits on, hidden on a book
static lv_obj_t *controls_row; // transport row: where the ellipsis lives, and the star in Studio
static lv_obj_t *fav_btn_icon; // the star above it

static void update_fav_button(void);
static void update_speed_button(void);
static void update_format_label(const device_state_t *state);
static void update_repeat_button(void);

// The colour the chrome on this page is drawn in: the theme's accent normally,
// and the sleeve's own while the alternative layout is up. Everything else on
// that layout takes its colour from the record -- the pills, the star's disc,
// the shape of the track -- and a blue play button in the middle of it was the
// one thing that still belonged to some other page.
static lv_color_t chrome_accent(void);
static lv_obj_t *progress_slider;
static lv_obj_t *elapsed_label; // start of the track, under the left end of the bar
static lv_obj_t *remaining_label; // end of the track, under the right end
// Where this track sits in the queue, between the two clocks: the row under the
// bar had an empty middle and the question -- how much of this is left -- is the
// same one the clocks answer.
static lv_obj_t *queue_position_label;
static lv_obj_t *repeat_btn_icon;
static lv_obj_t *prev_icon; // static so the over-cover recolor can reach them
static lv_obj_t *next_icon;
static lv_timer_t *progress_slider_timer;

// Runs only while a sleep timer is counting, which is the only time there is
// anything to ask. Paused otherwise, so an option nobody switched on does not
// wake the main loop once a second all night.
static lv_timer_t *sleep_timer;

// Where playback was when the poll last asked, and when that was. The tick is
// lv_tick_get()'s, which is the same clock the timer is driven by.
static lv_timer_t *smooth_timer;
static bool sheet_open; // the player panel is the page on show
static double progress_anchor_secs;
static uint32_t progress_anchor_tick;
static bool progress_running;

// How often the UI reconciles itself against the playback thread. It polls
// slowly rather than not at all while stopped: the audio thread can report
// STOPPED for a moment in the middle of a track change, and a UI that stopped
// polling on that reading would stay frozen until the next touch.
#define POLL_PERIOD_PLAYING_MS 500
#define POLL_PERIOD_IDLE_MS 2000

// How often the progress display is carried forward between polls.
//
// The poll asks the playback thread where it is, and half a second is as often
// as that is worth asking. But half a second is also how long the bar and the
// shape of the track stood still before jumping, and a bar that moves twice a
// second reads as a bar that is broken.
//
// So the position between polls is worked out from the clock instead of being
// asked for -- playback runs in real time, so the arithmetic is exact -- and
// the drawing happens only when it would actually differ by a pixel. On a
// four-minute track that is about twice a second for each of the two, out of
// sixteen chances; the rest of the ticks cost a subtraction and a comparison.
#define SMOOTH_PERIOD_MS 60

// How many fast polls to keep running after playback stops, so the queue gets
// its chance to start the next track before the timer relaxes.
#define POLLS_FAST_AFTER_STOP 4

static int polls_since_stop = POLLS_FAST_AFTER_STOP;

// Height the controls block needs for its two lines of text, the bar, the
// clocks and the buttons. Only used as a floor on screens too short to fit a
// full-width square cover on top of it.
#define PLAYER_MENU_MIN_HEIGHT 210

// Album art: an image on top of a placeholder panel. The panel is always
// there, so the layout doesn't jump between a track that has a cover and one
// that doesn't -- the image is simply hidden in the latter case.
static lv_obj_t *cover_panel;
static lv_obj_t *cover_img;
static lv_obj_t *cover_placeholder_icon;

// Live mode (internet radio). A stream is not a track: it cannot be paused,
// skipped or seeked, and it has no position and no length. Rather than leave
// controls on screen that would do nothing, the ones that make no sense are
// hidden and the play button becomes a stop button. These are kept so
// apply_live_mode() can reach them.
static lv_obj_t *prev_btn_obj;
static lv_obj_t *next_btn_obj;
static lv_obj_t *more_btn_obj;
static lv_obj_t *more_btn_icon; // ellipsis on a track, chapters on a book
static lv_obj_t *repeat_btn_obj;
static lv_obj_t *speed_btn_obj;	 // stands in the repeat button's place on a book
static lv_obj_t *speed_btn_icon;
static lv_obj_t *below_slider_obj; // the two clocks under the bar
// How far the four source marks keep from the corner of the artwork they
// share. One number, because they replace each other in that corner and a
// difference between them would read as the mark jumping.
#define BADGE_INSET 14

static lv_obj_t *live_badge;	   // live indicator, top right over the artwork
static lv_obj_t *qobuz_badge;	   // the Qobuz mark, in the same corner
static lv_obj_t *tidal_badge;	   // and the Tidal one, over it: only one ever shows
static lv_obj_t *podcast_badge;	   // and the podcast one, third in the same place
static bool live_mode;
static bool live_transport_hidden; // prev/next taken away while a stream is playing

// Audiobook mode. A book is one long file, not a queue of songs: previous and
// next have nothing to move to, and what a listener wants from those two
// buttons is to go back ten seconds because they missed a sentence. So the
// transport becomes a pair of ten-second jumps, the overflow menu becomes the
// chapter list, and the star and the repeat mode -- both of which belong to
// the music library and its queue -- go away rather than sit there doing
// something invisible to a book.
static bool audiobook_mode;
// A podcast is not an audiobook, but the two skip buttons are the same. See
// apply_audiobook_mode(): the rest of the book dressing -- chapters, speed,
// book icon -- stays with books only.
static bool podcast_mode;
static bool audiobook_has_chapters;		 // whether the loaded book is marked up
static int audiobook_skip_shown_back;	 // the two jump sizes the buttons are drawn
static int audiobook_skip_shown_forward; // for, so changing the setting repaints
static bool audiobook_mode_valid;   // false until the first apply, so it paints once

static cover_image_t current_cover;	   // pixels currently referenced by cover_img
static cover_image_t current_backdrop; // upside-down blurred copy behind the controls
static int cover_box_w, cover_box_h;   // the artwork spans the full screen width

// ---------------------------------------------------------------------------
// The two arrangements
//
// Standard is sleeve on top, and under it a block with the title, the star, the
// format, a bar and the transport.
//
// Alternative moves the track's own things onto the sleeve -- the title and
// artist in a pill in one bottom corner, the star in a disc in the other -- and
// turns the progress bar into the shape of the track. Everything it draws takes
// its colour from the sleeve rather than from the theme, which is why it only
// looks like anything at all over artwork.
//
// One set of widgets, moved between parents, and not two pages: the title, the
// star and the transport all have behaviour behind them, and a second copy of
// each would be a second place for that behaviour to be wrong.
// ---------------------------------------------------------------------------

// The progress bar: a thin track, and a knob that is a white ring around a
// smaller circle of the accent colour. The ring is a wide white border over an
// accent-coloured body rather than a second object, so it costs nothing and
// follows the knob on its own.
#define PROGRESS_TRACK_HEIGHT 10 // as thick as every other slider
#define PROGRESS_KNOB_GROW 9	 // how far the knob grows past the track
#define PROGRESS_KNOB_RING 5

// How the shape of the track is drawn, and how much room it gets. The bars are
// wide and well apart with rounded ends rather than a comb of hairlines: at
// this size a hairline is one pixel of a colour that is half background, and
// the whole thing reads as noise.
#define WAVE_HEIGHT 64
#define WAVE_BAR_GAP 3
#define WAVE_MIN_BAR 3	  // a silent column is still a mark, not a hole
#define WAVE_PAST_OPA 255 // the part already played
#define WAVE_TODO_OPA 80  // and the part still to come

// How far the pills and the disc sit in from the corner of the sleeve, and how
// big the disc is.
#define ALT_PAD 16
#define ALT_FAV_SIZE 64
#define ALT_PILL_PAD_H 18 // what a pill keeps to the left and right of its text
#define ALT_PILL_PAD_V 8

// What the sleeve's colour becomes once it has to be painted rather than
// thrown.
//
// cover_dominant_tone() answers with a colour stretched to full strength,
// because in Cover Flow it is a light behind the record and a light that is not
// strong is not seen. Poured into a solid pill the same number is a poster
// colour -- which is the complaint. So the hue is kept and everything else is
// pinned: the surface keeps a third of its distance from grey and sits at a
// fixed low brightness, the ink on it is always the same white, and the mark
// the waveform draws keeps rather more colour and a fixed high brightness so it
// still reads over a dark blurred sleeve. Two roles, one hue -- every album
// then looks like the same player rather than like a different skin.
#define ALT_SURFACE_SAT 34	// per cent of the distance from grey kept
#define ALT_SURFACE_LUMA 58 // and where it sits on 0..255
#define ALT_MARK_SAT 72
#define ALT_MARK_LUMA 196

// The setting, and whether it is in force right now.
//
// They are two different things because the alternative arrangement is built
// around a file on the card: the shape of the track comes from decoding it, the
// colour from its sleeve, and the pill in the corner from tags. A radio station
// has no length to draw, a stream from a phone has no file to read ahead of,
// and a Qobuz track is a cache entry that will be gone tomorrow. So the setting
// says what the user wants for their music, and `layout_alt_now` says whether
// what is playing is music of that kind. Everything that draws asks the second.
static player_layout_t layout_choice; // the setting, saved in the config
static bool layout_alt_now;		// and whether the current source can use it
static bool layout_studio_now;	// likewise for the third arrangement

static uint32_t album_tone; // the sleeve's colour, 0 when there is no sleeve

static lv_obj_t *song_text_obj; // the title and artist, wherever they live now
static lv_obj_t *song_side_obj; // the star and the format, likewise
static lv_obj_t *alt_text_col;	// the two pills, stacked at the foot of the sleeve
static lv_obj_t *alt_title_pill;
static lv_obj_t *alt_artist_pill;
static lv_obj_t *alt_fav_circle;
static lv_obj_t *wave_box;
static lv_obj_t *wave_canvas;
static uint8_t *wave_buf;
static int wave_w; // the buffer's width, which is what the painting must use
static uint8_t wave_bars[WAVEFORM_BARS];
static bool wave_have;		// the shape of this track is known
static int wave_drawn = -1; // where the playhead was, in pixels, when it was drawn
static uint32_t wave_drawn_tone = 1;
static int backdrop_w, backdrop_h;	   // size of the controls block it sits behind
// Studio shows the same blurred copy behind the whole screen, so there it is
// asked for at that shape instead: a picture made for the block behind the
// controls is a third of the height and stretching it up is both distorted and
// coarse. `backdrop_is_studio` is the shape the picture on hand was made at.
static int backdrop_studio_h;
static bool backdrop_is_studio;

static double current_total_length = 0; // cached from the last device_state snapshot, so slider math works between polls
static char progress_label_text[32];

// True while the controls sit on a loaded cover's dark backdrop. The play
// button wears its white disc there (it has to stand out against artwork);
// on the plain no-cover panel it is flat, like prev and next.
static bool chrome_over_cover;

// Switches the transport between a track and a live stream. Cheap and
// idempotent: called from every now-playing refresh, and does nothing at all
// unless the mode actually changed.
static void apply_live_mode(bool live) {
	// Previous and next go away while a station is actually playing, and come
	// back when it is stopped. The distinction is not cosmetic: on a running
	// stream those buttons have nothing to move to, so a tap starts a local
	// track underneath the radio -- which is exactly the surprise being
	// removed. With the station stopped there is nothing to interrupt, and
	// they mean what they always meant.
	bool transport_hidden = live && radio_is_playing();

	if (live == live_mode && transport_hidden == live_transport_hidden && cover_placeholder_icon) {
		return;
	}
	live_mode = live;
	live_transport_hidden = transport_hidden;

	// A stream has no queue to look at, no length to scrub and no repeat mode
	// that means anything.
	//
	// Repeat/shuffle goes for a reason worth stating: it is not merely useless
	// on a stream, it is misleading. The mode it cycles belongs to the track
	// queue, so a tap there would silently change what happens to the music
	// the radio interrupted.
	lv_obj_t *const hidden_when_live[] = {more_btn_obj, repeat_btn_obj, progress_slider, below_slider_obj};
	for (size_t i = 0; i < sizeof(hidden_when_live) / sizeof(hidden_when_live[0]); i++) {
		if (!hidden_when_live[i]) {
			continue;
		}
		if (live) {
			lv_obj_add_flag(hidden_when_live[i], LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_remove_flag(hidden_when_live[i], LV_OBJ_FLAG_HIDDEN);
		}
	}

	// The shape of the track goes with the bar, and comes back only if the
	// alternative layout is the one up -- it is not in the list above because
	// that list has no idea which arrangement is on screen, and unhiding it in
	// the standard one would leave an empty strip under the title.
	if (wave_box) {
		if (live || !layout_alt_now) {
			lv_obj_add_flag(wave_box, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_remove_flag(wave_box, LV_OBJ_FLAG_HIDDEN);
		}
	}

	lv_obj_t *const transport[] = {prev_btn_obj, next_btn_obj};
	for (size_t i = 0; i < sizeof(transport) / sizeof(transport[0]); i++) {
		if (!transport[i]) {
			continue;
		}
		if (transport_hidden) {
			lv_obj_add_flag(transport[i], LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_remove_flag(transport[i], LV_OBJ_FLAG_HIDDEN);
		}
	}

	if (live_badge) {
		if (live) {
			lv_obj_remove_flag(live_badge, LV_OBJ_FLAG_HIDDEN);
			lv_obj_move_foreground(live_badge);
		} else {
			lv_obj_add_flag(live_badge, LV_OBJ_FLAG_HIDDEN);
		}
	}
	// The marks share one corner and can never go there together: a live
	// stream does not come from Qobuz.
	if (tidal_badge && live) {
		lv_obj_add_flag(tidal_badge, LV_OBJ_FLAG_HIDDEN);
	}
	if (podcast_badge && live) {
		lv_obj_add_flag(podcast_badge, LV_OBJ_FLAG_HIDDEN);
	}
	if (qobuz_badge && live) {
		lv_obj_add_flag(qobuz_badge, LV_OBJ_FLAG_HIDDEN);
	}

	// A note means "track", a book means a book, and a station with no artwork
	// gets the radio. Coming back off a stream has to restore whichever of the
	// first two is right, not assume the note.
	if (cover_placeholder_icon) {
		lv_image_set_src(cover_placeholder_icon, live ? &icon_radio_player
									: (audiobook_mode ? &icon_book_headphones : &icon_music_note));
	}
}

// Dresses the player for a book or for a track. Cheap and idempotent, like
// apply_live_mode(): called from every now-playing refresh and from the poll,
// and does nothing at all unless the answer changed.
static void apply_audiobook_mode(bool book, bool podcast) {
	// Whether this book is marked up decides what the chapter button does, not
	// whether it is there.
	bool chapters = book && audiobook_chapter_count() > 0;

	// Books and podcasts both get the second-jump buttons, but not the same
	// pair of sizes: a book's forward jump steps over a pause, a podcast's
	// over a half-minute sponsor read. They are two separate settings on two
	// separate pages (podcast settings, and Audiobooks -> change controls).
	//
	// Nothing else carries over: a podcast has no chapters, is not a single
	// file, and sits in a real queue of episodes. Chapters, speed and the book
	// icon stay with books.
	bool skips = book || podcast;
	int back = podcast && !book ? podcast_skip_back() : audiobook_skip_back();
	int forward = podcast && !book ? podcast_skip_forward() : audiobook_skip_forward();

	if (audiobook_mode_valid && book == audiobook_mode && podcast == podcast_mode &&
		chapters == audiobook_has_chapters && back == audiobook_skip_shown_back &&
		forward == audiobook_skip_shown_forward) {
		return;
	}
	audiobook_mode = book;
	podcast_mode = podcast;
	audiobook_has_chapters = chapters;
	audiobook_skip_shown_back = back;
	audiobook_skip_shown_forward = forward;
	audiobook_mode_valid = true;

	if (prev_icon) {
		lv_image_set_src(prev_icon, !skips                            ? &icon_skip_back
									: back == AUDIOBOOK_SKIP_HUGE ? &icon_prev_60
									: back == AUDIOBOOK_SKIP_LONG ? &icon_prev_30
																  : &icon_prev_10);
	}
	if (next_icon) {
		lv_image_set_src(next_icon, !skips                               ? &icon_skip_forward
									: forward == AUDIOBOOK_SKIP_HUGE ? &icon_next_60
									: forward == AUDIOBOOK_SKIP_LONG ? &icon_next_30
																	 : &icon_next_10);
	}
	// The chapter glyph goes on for every book, marked up or not: one that has
	// no chapters says so when the button is pressed, which is a plainer
	// answer than a button that quietly means something else on some books.
	//
	// On a podcast the same button goes straight to the episodes, the only
	// thing wanted from it, rather than to a menu whose one useful entry would
	// always be picked.
	if (more_btn_icon) {
		lv_image_set_src(more_btn_icon, book			 ? &icon_chapter
										: podcast ? &icon_podcast_episodes
												  : &icon_ellipsis_vertical);
	}

	// The star and the repeat mode are about the music library and its queue.
	// On a book they are not merely useless: starring would write a row into
	// the music favourites for a file the music library does not index, and
	// the repeat mode would silently change what happens to the queue the book
	// interrupted.
	//
	// On a podcast the star stays but means something else: it follows the
	// podcast, putting it under the subscribed feeds -- the same promise the
	// star makes everywhere, "find this again without searching". Repeat still
	// goes: a queue of episodes is not an album, and looping the last twenty
	// instalments is nothing anybody asks for.
	if (fav_btn_obj) {
		if (book) {
			lv_obj_add_flag(fav_btn_obj, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_remove_flag(fav_btn_obj, LV_OBJ_FLAG_HIDDEN);
		}
	}
	if (repeat_btn_obj) {
		if (book || podcast) {
			lv_obj_add_flag(repeat_btn_obj, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_remove_flag(repeat_btn_obj, LV_OBJ_FLAG_HIDDEN);
		}
	}

	if (speed_btn_obj) {
		if (book) {
			lv_obj_remove_flag(speed_btn_obj, LV_OBJ_FLAG_HIDDEN);
			update_speed_button();
		} else {
			lv_obj_add_flag(speed_btn_obj, LV_OBJ_FLAG_HIDDEN);
		}
	}

	// A book with no artwork gets the book and an episode the microphone, not
	// a musical note -- each the glyph its own page uses, drawn at the size
	// this one is, which is why the podcast row's 40 px copy is not the one
	// named here. Only touched when a station is not up: live mode owns this
	// icon while it is on.
	if (cover_placeholder_icon && !live_mode) {
		const lv_image_dsc_t *mark = book ? &icon_book_headphones : podcast ? &icon_podcast_cover : &icon_music_note;
		lv_image_set_src(cover_placeholder_icon, mark);
	}
}

// ---------------------------------------------------------------------------
// Stopping on its own: at the end of a chapter, or after a while
//
// Both live here, in the poll that already runs twice a second, because both
// are questions about where playback has got to -- and neither wants a timer
// of its own that could fire while the book is paused, or after it has been
// swapped for a song.
// ---------------------------------------------------------------------------

// Where the chapter being listened to ends, or 0 when the book has no chapters
// or is on its last one. Worked out per poll rather than remembered, so a jump
// to another chapter is picked up without anything having to be told.
static double current_chapter_end(double position) {
	int index = audiobook_chapter_at(position);
	if (index < 0) {
		return 0;
	}
	double start = 0;
	if (!audiobook_chapter(index + 1, NULL, 0, &start)) {
		return 0; // the last chapter ends where the file does
	}
	return start;
}

static void pause_book_at(double seconds) {
	device_state_seek(seconds);
	if (audio_get_status() == AUDIO_STATUS_PLAYING) {
		audiobook_suppress_rewind_once();
		device_state_toggle_play_pause();
	}
	// Written through: stopping on purpose is exactly the moment somebody puts
	// the player down.
	audiobook_note_position(seconds, 0, true);
}

// Runs on every poll while a book is loaded.
//
// Only the chapter boundary: the sleep timers live in sleeptimer.c, where all
// three behave the same way.
static bool audiobook_auto_stop(const device_state_t *state) {
	if (state->status != AUDIO_STATUS_PLAYING || !audiobook_stop_at_chapter_end()) {
		return false;
	}
	double end = current_chapter_end(state->progress_current_secs);
	if (end > 0 && state->progress_current_secs >= end - 0.5) {
		pause_book_at(end);
		return true;
	}
	return false;
}

// The jump both the on-screen buttons and the side keys make. Clamped at both
// ends: asking for -10 s in the first second means the beginning, and running
// off the end of the file would stop the book instead of moving in it.
static void audiobook_seek_by(double delta, const device_state_t *state) {
	double target = state->progress_current_secs + delta;
	if (target < 0) {
		target = 0;
	}
	double length = state->progress_total_secs > 0 ? state->progress_total_secs : current_total_length;
	if (length > 1.0 && target > length - 1.0) {
		target = length - 1.0;
	}

	device_state_seek(target);
	// Written through immediately: a jump is a deliberate move, and the ten
	// second throttle would lose it if the player were switched off next.
	audiobook_note_position(target, length, true);
}

// Applies a playback status to the play/pause button and the progress timer.
// Called both optimistically (right after a button press, before the audio
// thread has caught up) and from update_progress() every poll, which is what
// keeps the UI from going stale if the track stops on its own.
// Which sleep timer, if any, owns what is playing.
//
// Books and podcasts have their own because they are listened to differently: a
// stretch that is right for falling asleep to music is not the one somebody
// chose for a chapter, and a podcast timer set at bedtime should not be counting
// down over an album the next morning. Everything else the transport plays --
// the card, Tidal, Qobuz -- is one thing and gets one timer.
//
// Three sources are none of them, and for the same reason: they do not go
// through the player's transport at all. The radio, the Game Boy and the
// Bluetooth receiver push their sound out through the external path, and there
// is no pause to give them -- stopping a station is a disconnection, stopping
// the emulator is stopping the game, and stopping the receiver is silence in a
// room where somebody else's phone is still playing. Only the radio ever
// reports playing here; the other two are written down so that it stays true if
// they ever start to.
static sleeptimer_kind_t sleeptimer_kind_playing(void) {
	if (radio_is_active() || gearboy_running() || btreceiver_is_active()) {
		return SLEEPTIMER_COUNT;
	}
	if (audiobook_is_playing()) {
		return SLEEPTIMER_AUDIOBOOK;
	}
	char path[512];
	audio_get_current_file(path, sizeof(path));
	if (podcastcache_owns(path)) {
		return SLEEPTIMER_PODCAST;
	}
	return SLEEPTIMER_MUSIC;
}

static void apply_playback_status(audio_status_t status) {
	bool playing = (status == AUDIO_STATUS_PLAYING);

	// The glyph is the accent colour, always: play and pause are the same
	// control in two states. Only the disc under it changes (see
	// set_over_cover): white over artwork and in the dark theme, dark in the
	// light theme when there is no cover to stand out against.
	//
	// On a live stream there is no pause -- stopping and starting again does
	// not resume, it reconnects -- so the button says stop while it is on.
	// Stopped, it goes back to play: the station stays loaded, and pressing
	// it opens a fresh connection to the same one.
	lv_image_set_src(play_btn_icon,
					 live_mode ? (playing ? &icon_stop : &icon_play) : (playing ? &icon_pause : &icon_play));
	lv_obj_set_style_image_recolor(play_btn_icon, chrome_accent(), 0);
	lv_obj_set_style_image_recolor_opa(play_btn_icon, LV_OPA_COVER, 0);

	// Keep polling either way, just less often when nothing is playing. The
	// timer must never be paused here: a track change is briefly reported as
	// STOPPED, and a timer paused on that reading leaves nothing running to
	// correct the progress bar and the play/pause icon.
	//
	// Dropping straight to the slow period on STOPPED is wrong too: the end of
	// a track is reported as STOPPED and the queue only advances on the next
	// poll, so the gap between two tracks would become the slow period, seconds
	// of silence reading as a player that stopped instead of going on. Stay on
	// the fast period for a moment after playback stops, which is exactly the
	// window the handover needs.
	if (playing) {
		polls_since_stop = 0;
	} else if (polls_since_stop < POLLS_FAST_AFTER_STOP) {
		polls_since_stop++;
	}

	bool settling = polls_since_stop < POLLS_FAST_AFTER_STOP;
	lv_timer_set_period(progress_slider_timer, (playing || settling) ? POLL_PERIOD_PLAYING_MS : POLL_PERIOD_IDLE_MS);
	lv_timer_resume(progress_slider_timer);

	// The status bar shows the same state, and told here rather than left to
	// wait for its own five-second poll.
	topbar_refresh_playback();

	// Every route into and out of playing comes through here -- the button, the
	// side keys, the end of a track, a remote, the poll putting the screen back
	// in step with the engine -- so it is the one place the sleep timers can be
	// told without being told twice. It is also the one place that knows what
	// is playing well enough to say which of the three it belongs to.
	sleeptimer_note_playing(playing ? sleeptimer_kind_playing() : SLEEPTIMER_COUNT);
	if (sleep_timer) {
		if (sleeptimer_any_running()) {
			lv_timer_resume(sleep_timer);
		} else {
			lv_timer_pause(sleep_timer);
		}
	}
}

// "3/24": which track of the queue is playing.
//
// It reads the queue itself, so it says the same thing whatever filled it --
// tracks off the card, a podcast's episodes, a Tidal or Qobuz album.
//
// The number is the track's place in the LIST, not in the dealt order: under
// shuffle those are two different things, and the list is the one that means
// something. Counting the deal gives a number that climbs by one at every
// track, which reads as a position in an album but is not one, since the
// tracks are picked at random from all over the card. Outside shuffle the two
// orders are the same array and the reading is identical.
//
// Blank rather than "1/1" when there is nothing to be lost in: a single track,
// a book (one long file), a stream (no queue at all), or playback that belongs
// to a phone over DLNA.
static void update_queue_position(void) {
	if (!queue_position_label) {
		return;
	}

	int count = playlist_count();
	int index = playlist_current_index();
	bool worth_saying = count > 1 && index >= 0 && !audiobook_mode && !live_mode && !dlna_owns_playback();

	if (!worth_saying) {
		lv_label_set_text(queue_position_label, "");
		return;
	}
	lv_label_set_text_fmt(queue_position_label, "%d/%d", (int)playlist_current_entry() + 1, count);
}

// Writes the two clocks that sit under the ends of the progress bar: elapsed
// on the left, total length on the right.
static void set_progress_label(double current_secs, double total_secs) {
	if (total_secs <= 0) {
		current_secs = 0;
		total_secs = 0;
	} else {
		int value = (current_secs / total_secs) * 1000;
		lv_slider_set_value(progress_slider, value, LV_ANIM_OFF);
	}

	formatDoubleSeconds(current_secs, progress_label_text, sizeof(progress_label_text));
	lv_label_set_text(elapsed_label, progress_label_text);

	formatDoubleSeconds(total_secs, progress_label_text, sizeof(progress_label_text));
	lv_label_set_text(remaining_label, progress_label_text);

	// Written from here because this is the one place that runs on every poll
	// and on every track change, which is exactly when the answer can move.
	update_queue_position();
}

// True while a decode is in flight at the worker.
static bool cover_request_outstanding;

// What the artwork on screen IS, as cover_source_id() reports it, so the next
// track can be asked "the same picture?" before anything is thrown away. 0 when
// it is not known -- the answer has not come back yet, or there is no artwork.
static uint64_t cover_shown_id;

// The id of the picture now being decoded, when it was already known before the
// decode was asked for. Saves hashing the same file twice.
static uint64_t cover_incoming_id;

// Hashing never happens on this thread: it reads the picture out of the file,
// which is the work the loader exists to keep off the interface. So there is a
// question outstanding, and two reasons to have asked it.
static enum {
	COVER_ID_IDLE,
	COVER_ID_NEW_TRACK, // is the next track's picture the one already up?
	COVER_ID_MEASURE,   // what is the picture that just went up?
} cover_id_state;

// With artwork loaded the controls sit on the always-dark blurred backdrop,
// so the chrome -- titles, clocks, transport glyphs, the chevron -- must be
// light in both themes. Local style props are laid over the theme styles
// while the cover is up, and removed again when it goes, so the plain
// no-cover panel keeps following the theme.
// The picture at the width of the screen, and the note that stands in for it.
//
// One place, because Studio hides both -- the sleeve there is a smaller picture
// of its own over the blurred copy -- and the artwork arrives on a worker, so
// whatever the arrangement did when it was set up would be undone the moment a
// cover landed.
static void cover_show(bool have_cover) {
	if (!cover_img) {
		return;
	}
	bool studio = layout_studio_now;
	if (have_cover && !studio) {
		lv_obj_remove_flag(cover_img, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(cover_img, LV_OBJ_FLAG_HIDDEN);
	}
	if (!cover_placeholder_icon) {
		return;
	}
	if (!have_cover && !studio) {
		lv_obj_remove_flag(cover_placeholder_icon, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(cover_placeholder_icon, LV_OBJ_FLAG_HIDDEN);
	}
}

static void set_over_cover(bool on) {
	chrome_over_cover = on;

	lv_obj_t *const text_objs[] = {song_title_label, elapsed_label, remaining_label};
	lv_obj_t *const dim_objs[] = {song_artist_label, format_label, queue_position_label};
	lv_obj_t *const icon_objs[] = {prev_icon, next_icon};

	for (size_t i = 0; i < sizeof(text_objs) / sizeof(text_objs[0]); i++) {
		if (!text_objs[i]) {
			continue;
		}
		if (on) {
			lv_obj_set_style_text_color(text_objs[i], lv_color_white(), 0);
		} else {
			lv_obj_remove_local_style_prop(text_objs[i], LV_STYLE_TEXT_COLOR, 0);
		}
	}
	for (size_t i = 0; i < sizeof(dim_objs) / sizeof(dim_objs[0]); i++) {
		if (!dim_objs[i]) {
			continue;
		}
		if (on) {
			lv_obj_set_style_text_color(dim_objs[i], lv_color_make(200, 200, 200), 0);
		} else {
			lv_obj_remove_local_style_prop(dim_objs[i], LV_STYLE_TEXT_COLOR, 0);
		}
	}
	for (size_t i = 0; i < sizeof(icon_objs) / sizeof(icon_objs[0]); i++) {
		if (!icon_objs[i]) {
			continue;
		}
		if (on) {
			lv_obj_set_style_image_recolor(icon_objs[i], lv_color_white(), 0);
			lv_obj_set_style_image_recolor_opa(icon_objs[i], LV_OPA_COVER, 0);
		} else {
			lv_obj_remove_local_style_prop(icon_objs[i], LV_STYLE_IMAGE_RECOLOR, 0);
			lv_obj_remove_local_style_prop(icon_objs[i], LV_STYLE_IMAGE_RECOLOR_OPA, 0);
		}
	}

	// The disc: white over artwork and in the dark theme, where white reads
	// as the brightest thing on screen. The one case it has to go dark is the
	// light theme with no cover -- a white disc on a near-white panel is
	// invisible.
	if (play_btn) {
		bool white_disc = on || theme()->dark;
		lv_obj_set_style_bg_color(play_btn, white_disc ? lv_color_white() : theme()->surface_pressed, 0);
		lv_obj_set_style_bg_opa(play_btn, LV_OPA_COVER, 0);
	}
	if (play_btn_icon) {
		lv_obj_set_style_image_recolor(play_btn_icon, chrome_accent(), 0);
		lv_obj_set_style_image_recolor_opa(play_btn_icon, LV_OPA_COVER, 0);
	}

	// The floating chevron parks over the artwork's top-left corner.
	back_btn_over_cover(on);
}

// Told before the loaded track's artwork is freed. The screensaver borrows
// those pixels and can still be up behind a blanked panel when a track
// changes, so it has to let go first.
static void (*cover_release_cb)(void);

void player_set_cover_release_cb(void (*cb)(void)) { cover_release_cb = cb; }

// The path whose artwork is currently up. A live stream refreshes its text
// several times a minute (every new on-air title), and re-decoding the same
// station icon each time would be pointless work on the UI thread.
static char cover_shown_path[512];

static void reload_cover(const char *filepath);

// Shows the placeholder and asks the worker for the album art of `filepath`.
// Nothing is read or decoded here: on the X1600E that work is seconds long,
// and doing it on this thread at every track change -- including the automatic
// one when a song ends -- froze whatever the user was doing at that moment,
// the file browser most of all. The finished pictures are collected by
// apply_cover_result() from the progress timer tick.
// Rec. 601 luma, which is what "how bright does this look" means to an eye and
// not to a sum of three channels.
static int luma_of(int r, int g, int b) { return (r * 299 + g * 587 + b * 114) / 1000; }

// The sleeve's colour with its strength cut to `sat` per cent of the distance
// from grey and its brightness moved to `want`. Darkening is a plain ratio, so
// no channel can clip; brightening walks each channel the same fraction of the
// way to white, which keeps the hue where it was.
static lv_color_t tone_shaped(int sat, int want) {
	uint32_t raw = album_tone;
	if (!raw) {
		lv_color_t accent = theme()->accent;
		raw = ((uint32_t)accent.red << 16) | ((uint32_t)accent.green << 8) | accent.blue;
	}
	int r = (int)((raw >> 16) & 0xFF), g = (int)((raw >> 8) & 0xFF), b = (int)(raw & 0xFF);

	int grey = luma_of(r, g, b);
	r = grey + (r - grey) * sat / 100;
	g = grey + (g - grey) * sat / 100;
	b = grey + (b - grey) * sat / 100;

	int luma = luma_of(r, g, b);
	if (luma > want && luma > 0) {
		r = r * want / luma;
		g = g * want / luma;
		b = b * want / luma;
	} else if (luma < want && luma < 255) {
		int step = (want - luma) * 255 / (255 - luma);
		r += (255 - r) * step / 255;
		g += (255 - g) * step / 255;
		b += (255 - b) * step / 255;
	}
	return lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

// What the pills and the disc are filled with, and what the waveform is drawn
// in. Cover Flow throws the same hue under the record in the middle, which is
// what makes the two pages look like they belong to the same player.
static lv_color_t alt_surface(void) { return tone_shaped(ALT_SURFACE_SAT, ALT_SURFACE_LUMA); }
static lv_color_t alt_mark(void) { return tone_shaped(ALT_MARK_SAT, ALT_MARK_LUMA); }

static lv_color_t chrome_accent(void) { return layout_alt_now ? alt_mark() : theme()->accent; }

// The type on that surface. Worked out rather than fixed so that a change to
// ALT_SURFACE_LUMA cannot quietly leave the title unreadable.
static lv_color_t alt_ink(void) {
	lv_color_t s = alt_surface();
	return luma_of(s.red, s.green, s.blue) > 140 ? lv_color_hex(0x1A1A1A) : lv_color_white();
}

// The two glyphs that wear the accent, repainted whenever the accent of the
// moment can have moved: the play/pause disc and the repeat/shuffle mode.
static void paint_transport_tint(void) {
	if (play_btn_icon) {
		lv_obj_set_style_image_recolor(play_btn_icon, chrome_accent(), 0);
		lv_obj_set_style_image_recolor_opa(play_btn_icon, LV_OPA_COVER, 0);
	}
	if (repeat_btn_icon) {
		update_repeat_button();
	}
}

// Hides the artist's pill when there is no artist. An empty pill is a blank
// lozenge sitting on the sleeve for no reason, and a stream between titles has
// no artist for seconds at a time.
static void alt_pills_sync(void) {
	if (!alt_artist_pill || !song_artist_label) {
		return;
	}
	const char *artist = lv_label_get_text(song_artist_label);
	if (artist && artist[0]) {
		lv_obj_remove_flag(alt_artist_pill, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(alt_artist_pill, LV_OBJ_FLAG_HIDDEN);
	}
}

// Puts the sleeve's colour on everything the alternative layout tints -- and,
// in the standard layout, takes it off again. That second half is the point:
// these are local style properties on widgets the two arrangements share, so
// leaving them behind painted the standard page in the last sleeve's ink.
static void paint_alt_tint(void) {
	if (!alt_title_pill) {
		return;
	}

	if (!layout_alt_now) {
		if (song_artist_label) {
			lv_obj_remove_local_style_prop(song_artist_label, LV_STYLE_TEXT_OPA, 0);
		}
		// And the title and the artist go back to being whatever the page they
		// are on says they are. Asked of the one function that knows -- the
		// colour depends on whether there is artwork behind them, not on the
		// theme alone, and a second copy of that rule here would be a second
		// place for it to go out of date.
		set_over_cover(chrome_over_cover);
		update_fav_button(); // which owns the star's colour in both arrangements
		paint_transport_tint();
		wave_drawn = -1;
		return;
	}

	lv_color_t surface = alt_surface();
	lv_color_t ink = alt_ink();

	lv_obj_set_style_bg_color(alt_title_pill, surface, 0);
	lv_obj_set_style_bg_color(alt_artist_pill, surface, 0);
	lv_obj_set_style_bg_color(alt_fav_circle, surface, 0);

	if (song_title_label) {
		lv_obj_set_style_text_color(song_title_label, ink, 0);
	}
	if (song_artist_label) {
		lv_obj_set_style_text_color(song_artist_label, ink, 0);
		lv_obj_set_style_text_opa(song_artist_label, LV_OPA_80, 0);
	}
	update_fav_button();
	alt_pills_sync();
	paint_transport_tint();

	wave_drawn = -1; // the bars are the old colour
}

// Whether what is playing is an ordinary file on the card.
//
// Asked of the state and the caches rather than of a flag raised at the start,
// for the reason written at apply_audiobook_mode(): a flag has to be cleared on
// every route by which the track can change, and the first route forgotten
// leaves the player dressed as the wrong thing.
//
// Everything that is not a file on the card is somebody else's audio passing
// through: the radio has no length, Qobuz, Tidal, a podcast and a phone pushing
// over DLNA all play out of a cache folder that is emptied behind them, and
// AirPlay does not go through the decoder at all -- during an AirPlay session
// `current_file` still names whatever local track was loaded last, so it has to
// be asked separately rather than inferred from the path.
//
// An audiobook is the one exception that IS a file on the card. It is left out
// anyway: the page is already dressed as a book -- chapters instead of a queue,
// skips instead of tracks, no star -- and none of what the alternative
// arrangement adds means anything to one. A shape drawn across nine hours of
// speech is a flat stripe, and the nine hours would have to be decoded to find
// that out.
static bool playing_local_file(const device_state_t *state) {
	if (state->live || !state->current_file[0] || audiobook_is_playing()) {
		return false;
	}
	airplay_state_t air;
	airplay_get_state(&air);
	if (air.playing) {
		return false;
	}
	const char *f = state->current_file;
	return !qobuzcache_owns(f) && !tidalcache_owns(f) && !podcastcache_owns(f) && !dlna_owns_path(f) &&
		   !dlna_owns_playback();
}

// Puts the progress slider over the waveform, or back where it belongs.
//
// The slider is the only thing on the page that knows how to turn a finger into
// a seek -- the waveform is a picture and nothing more -- so the alternative
// layout must not hide it. It moves inside the waveform's box instead,
// stretched over the whole of it so that a press anywhere on the bars is a
// press on the slider, with every part of it drawn as nothing. What is seen is
// the waveform; what is dragged is the slider.
static void slider_over_waveform(bool over) {
	if (!progress_slider) {
		return;
	}

	if (over && wave_box) {
		lv_obj_set_parent(progress_slider, wave_box);
		lv_obj_set_size(progress_slider, lv_pct(100), WAVE_HEIGHT);
		lv_obj_align(progress_slider, LV_ALIGN_CENTER, 0, 0);
		lv_obj_set_style_bg_opa(progress_slider, LV_OPA_TRANSP, LV_PART_MAIN);
		lv_obj_set_style_bg_opa(progress_slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
		lv_obj_set_style_opa(progress_slider, LV_OPA_TRANSP, LV_PART_KNOB);
		lv_obj_move_foreground(progress_slider); // over the canvas, which takes no presses anyway
		return;
	}

	if (lv_obj_get_parent(progress_slider) == wave_box) {
		lv_obj_set_parent(progress_slider, player_menu);
		lv_obj_move_to_index(progress_slider, lv_obj_get_index(wave_box) + 1);
	}
	lv_obj_set_size(progress_slider, lv_pct(100), PROGRESS_TRACK_HEIGHT);
	lv_obj_remove_local_style_prop(progress_slider, LV_STYLE_BG_OPA, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(progress_slider, LV_OPA_30, LV_PART_MAIN);
	lv_obj_remove_local_style_prop(progress_slider, LV_STYLE_BG_OPA, LV_PART_INDICATOR);
	lv_obj_remove_local_style_prop(progress_slider, LV_STYLE_OPA, LV_PART_KNOB);
}

// ---------------------------------------------------------------------------
// Studio: the sleeve in the middle, the blurred artwork behind the whole screen
//
// Nothing here is drawn by hand. The sleeve is the picture the page already
// decoded, shown at the size this arrangement wants, and the background is the
// blurred copy the page already makes for behind the controls, asked for at the
// size of the whole screen instead. What this section owns is where they go and
// when.
// ---------------------------------------------------------------------------

#define STUDIO_MARGIN 14	  // what the sleeve keeps to the side edges
#define STUDIO_HEAD_H 78	  // the title, the artist and the ellipsis
#define STUDIO_COVER_GAP 24	  // between the head and the top of the sleeve
#define STUDIO_BADGE_GAP 6	  // between the head and the source mark under the ellipsis
#define STUDIO_QUALITY_GAP 10 // between the sleeve and the line under it
#define STUDIO_QUALITY_H 30
#define STUDIO_BOTTOM 10 // under that line, before the controls begin

static lv_obj_t *studio_bg;		  // the blurred sleeve, the size of the screen
static lv_obj_t *studio_box;	  // what everything else is laid out on
static lv_obj_t *studio_head;	  // title and artist across the top
static lv_obj_t *studio_text_col; // the two of them, stacked and centred
static lv_obj_t *studio_cover;
static lv_obj_t *studio_empty;		  // the square shown where a track has no artwork
static lv_obj_t *studio_empty_icon;
static lv_obj_t *studio_quality;	  // the icon and the format line under the sleeve
static lv_obj_t *studio_quality_icon;
static bool studio_up;
static int studio_box_w, studio_box_h; // the panel this arrangement is laid out on
static int studio_cover_size;

// Which of the four quality marks belongs to what is playing.
//
// Asked of the stream and not of the library, so a radio station and a track
// still downloading are answered as well as a file that has been indexed.
//
// A podcast gets none of them. The marks rank an encoding against what music
// needs, and an episode is speech at whatever bitrate the publisher chose:
// "lossy" beside it is true and says nothing anyone would act on.
static const lv_image_dsc_t *studio_quality_mark(const device_state_t *state) {
	if (!state->live && podcastcache_owns(state->current_file)) {
		return NULL;
	}
	if (audio_get_dsd_multiple() > 0) {
		return &icon_quality_dsd;
	}
	if (state->live || audio_stream_is_lossy()) {
		return &icon_quality_lossy;
	}
	int rate = 0, channels = 0;
	audio_get_stream_info(&rate, &channels);
	int bits = audio_get_stream_bits();
	if (rate > 48000 || bits > 16) {
		return &icon_quality_hifi;
	}
	if (rate > 0) {
		return &icon_quality_cd;
	}
	return NULL;
}

// The top of the sleeve, and how large it is.
//
// The sleeve is as large as the narrower of the two constraints allows: the
// width left between the margins, and the height left between the head and the
// line under it, which on this panel is what decides.
//
// Worked out from the numbers rather than from the widgets, so it can be asked
// before the panel has been laid out -- which is where the artwork is ordered
// at the size this arrangement will draw it.
static int studio_cover_geometry(int *top_out) {
	int head_top = back_btn_centre_y() - STUDIO_HEAD_H / 2;
	if (head_top < 2) {
		head_top = 2;
	}

	int top = head_top + STUDIO_HEAD_H + STUDIO_COVER_GAP;
	int size = studio_box_w - 2 * STUDIO_MARGIN;
	int room = studio_box_h - top - STUDIO_QUALITY_GAP - STUDIO_QUALITY_H - STUDIO_BOTTOM;
	if (size > room) {
		size = room;
	}
	if (size < 64) {
		size = 64;
	}
	if (top_out) {
		*top_out = top + (room - size) / 2;
	}
	return size;
}

// The four marks that say where a track comes from -- Qobuz, Tidal, podcast,
// and the red one for a live stream -- share the top right corner of the
// artwork, which in this arrangement is where the ellipsis went. They move
// down under it and keep its right edge, so the two read as one column.
//
// `parent` of NULL leaves them where they are and only re-places them, which
// is what studio_place() wants on a relayout.
static void studio_badges_move(lv_obj_t *parent, int y) {
	lv_obj_t *const marks[] = {live_badge, qobuz_badge, tidal_badge, podcast_badge};
	for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); i++) {
		if (!marks[i]) {
			continue;
		}
		if (parent) {
			lv_obj_set_parent(marks[i], parent);
		}
		lv_obj_align(marks[i], LV_ALIGN_TOP_RIGHT, -STUDIO_MARGIN, y);
	}
}

// Where everything goes.
static void studio_place(void) {
	if (!studio_box) {
		return;
	}

	int screen_w = studio_box_w;
	int head_top = back_btn_centre_y() - STUDIO_HEAD_H / 2;
	if (head_top < 2) {
		head_top = 2;
	}

	int cover_y = 0;
	int size = studio_cover_geometry(&cover_y);
	studio_cover_size = size;

	int cover_x = (screen_w - size) / 2;

	lv_obj_set_pos(studio_head, STUDIO_MARGIN, head_top);
	lv_obj_set_size(studio_head, screen_w - 2 * STUDIO_MARGIN, STUDIO_HEAD_H);

	studio_badges_move(NULL, head_top + STUDIO_HEAD_H + STUDIO_BADGE_GAP);

	lv_obj_set_size(studio_cover, size, size);
	lv_obj_set_pos(studio_cover, cover_x, cover_y);

	// The same square, so a track with no artwork keeps the arrangement rather
	// than leaving a bare screen.
	lv_obj_set_size(studio_empty, size, size);
	lv_obj_set_pos(studio_empty, cover_x, cover_y);

	lv_obj_set_size(studio_quality, size, STUDIO_QUALITY_H);
	lv_obj_set_pos(studio_quality, cover_x, cover_y + size + STUDIO_QUALITY_GAP);
}

// Points the sleeve and the background at the pictures the page already holds.
static void studio_refresh_cover(void) {
	if (!studio_cover) {
		return;
	}
	bool have = current_cover.pixels != NULL;
	lv_image_set_src(studio_cover, have ? &current_cover.dsc : NULL);
	if (have) {
		lv_obj_remove_flag(studio_cover, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(studio_cover, LV_OBJ_FLAG_HIDDEN);
	}

	// One of the two squares is always up. Without this a track with no artwork
	// left the arrangement with nothing in it at all: no sleeve, no blurred
	// background behind it, and the page's own colour across the whole screen.
	if (studio_empty) {
		if (have) {
			lv_obj_add_flag(studio_empty, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_remove_flag(studio_empty, LV_OBJ_FLAG_HIDDEN);
		}
	}
	if (studio_empty_icon) {
		// The same mark the full-width cover panel stands in with: the note, the
		// headphones for a book, the aerial for a station.
		lv_image_set_src(studio_empty_icon, lv_image_get_src(cover_placeholder_icon));
		lv_obj_set_style_image_recolor(studio_empty_icon, theme()->text_secondary, 0);
	}

	if (studio_bg) {
		lv_image_set_src(studio_bg, current_backdrop.pixels ? &current_backdrop.dsc : NULL);
		if (current_backdrop.pixels) {
			lv_obj_remove_flag(studio_bg, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(studio_bg, LV_OBJ_FLAG_HIDDEN);
		}
	}
}

// Puts the ellipsis and the star back where every other arrangement keeps them,
// and takes the panel down. Called before each arrangement is laid out, so the
// two that know nothing about Studio find the page as they left it.
static void studio_take_back(void) {
	if (!studio_box) {
		return;
	}
	lv_obj_add_flag(studio_box, LV_OBJ_FLAG_HIDDEN);
	if (studio_bg) {
		lv_obj_add_flag(studio_bg, LV_OBJ_FLAG_HIDDEN);
	}
	if (studio_empty) {
		lv_obj_add_flag(studio_empty, LV_OBJ_FLAG_HIDDEN);
	}

	// The surfaces this arrangement made transparent, and the light chrome that
	// goes with the blurred sleeve. Only put back by the arrangement that took
	// them away: every other path leaves the cover to decide, as it always has.
	if (studio_up) {
		studio_up = false;
		lv_obj_remove_local_style_prop(cover_panel, LV_STYLE_BG_OPA, 0);
		if (player_menu) {
			lv_obj_remove_local_style_prop(player_menu, LV_STYLE_BG_OPA, 0);
			lv_obj_remove_local_style_prop(player_menu, LV_STYLE_BG_IMAGE_OPA, 0);
		}
		// The picture at full width comes back, and the note with it when there
		// is no picture. studio_up is already down, so cover_show() agrees.
		cover_show(current_cover.pixels != NULL);
		if (format_label && song_side_obj) {
			lv_obj_set_parent(format_label, song_side_obj);
			lv_obj_set_style_text_align(format_label, LV_TEXT_ALIGN_RIGHT, 0);
		}
		// The marks go back to the corner of the artwork they share with every
		// other arrangement.
		lv_obj_t *const marks[] = {live_badge, qobuz_badge, tidal_badge, podcast_badge};
		for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); i++) {
			if (marks[i]) {
				lv_obj_set_parent(marks[i], cover_panel);
				lv_obj_align(marks[i], LV_ALIGN_TOP_RIGHT, -BADGE_INSET, BADGE_INSET);
			}
		}
		set_over_cover(current_cover.pixels != NULL);
	}

	if (more_btn_obj && controls_row) {
		lv_obj_set_parent(more_btn_obj, controls_row);
		lv_obj_add_flag(more_btn_obj, LV_OBJ_FLAG_IGNORE_LAYOUT);
		lv_obj_align(more_btn_obj, LV_ALIGN_RIGHT_MID, 0, 0);
	}
	if (fav_btn_obj) {
		// Laid out by its row again: the next step decides which row that is.
		lv_obj_remove_flag(fav_btn_obj, LV_OBJ_FLAG_IGNORE_LAYOUT);
	}
}

static void studio_put(void) {
	if (!studio_box) {
		return;
	}

	if (song_title_label) {
		lv_obj_set_parent(song_title_label, studio_text_col);
		lv_obj_move_to_index(song_title_label, 0);
		lv_obj_set_width(song_title_label, lv_pct(100));
		lv_obj_set_style_max_width(song_title_label, LV_COORD_MAX, 0);
	}
	if (song_artist_label) {
		lv_obj_set_parent(song_artist_label, studio_text_col);
		lv_obj_move_to_index(song_artist_label, 1);
		lv_obj_set_width(song_artist_label, lv_pct(100));
		lv_obj_set_style_max_width(song_artist_label, LV_COORD_MAX, 0);
	}
	lv_obj_add_flag(song_text_obj, LV_OBJ_FLAG_HIDDEN);
	if (song_side_obj) {
		lv_obj_add_flag(song_side_obj, LV_OBJ_FLAG_HIDDEN);
	}

	// The line under the sleeve is the same label the standard arrangement
	// keeps beside the artist -- it already knows how to say "16/44.1 FLAC",
	// "320 kbps MP3" and "DSD256", a station included. Moved, not copied.
	if (format_label) {
		lv_obj_set_parent(format_label, studio_quality);
		lv_obj_remove_flag(format_label, LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_width(format_label, LV_SIZE_CONTENT);
		lv_obj_set_style_text_align(format_label, LV_TEXT_ALIGN_LEFT, 0);
	}

	// The two buttons change places: the ellipsis belongs beside the title it
	// is about, and the star takes the corner of the transport row it left.
	if (more_btn_obj && studio_head) {
		lv_obj_set_parent(more_btn_obj, studio_head);
		lv_obj_add_flag(more_btn_obj, LV_OBJ_FLAG_IGNORE_LAYOUT);
		lv_obj_align(more_btn_obj, LV_ALIGN_RIGHT_MID, 0, 0);
	}
	if (fav_btn_obj && controls_row) {
		lv_obj_set_parent(fav_btn_obj, controls_row);
		lv_obj_add_flag(fav_btn_obj, LV_OBJ_FLAG_IGNORE_LAYOUT);
		lv_obj_align(fav_btn_obj, LV_ALIGN_RIGHT_MID, 0, 0);
	}

	// Onto the panel, so they draw over the sleeve rather than under it, and
	// below the ellipsis. studio_place() settles where.
	studio_badges_move(studio_box, 0);

	// The blurred sleeve is the whole background here, so everything that would
	// otherwise cover it gets out of the way: the two panels' own fills, the
	// picture at full width, and the note that stands in for it.
	studio_up = true;
	lv_obj_set_style_bg_opa(cover_panel, LV_OPA_TRANSP, 0);
	if (player_menu) {
		lv_obj_set_style_bg_opa(player_menu, LV_OPA_TRANSP, 0);
		// Its own blurred copy too: it is the same picture at a different crop,
		// and two of them meeting at the controls is a seam across the screen.
		lv_obj_set_style_bg_image_opa(player_menu, LV_OPA_TRANSP, 0);
	}
	cover_show(current_cover.pixels != NULL);

	lv_obj_remove_flag(studio_box, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(studio_box);
	if (studio_bg) {
		lv_obj_move_background(studio_bg);
	}
	studio_place();
	studio_refresh_cover();
}

// Moves the pieces between the two arrangements.
//
// Reparenting and not rebuilding: the title label carries its scrolling, the
// star carries what it does when pressed, and copying either of them would be
// copying behaviour.
static void apply_layout(void) {
	if (!alt_title_pill || !song_text_obj) {
		return;
	}

	// Always first: the two arrangements below know nothing about Studio, so
	// they have to find the ellipsis and the star where they left them.
	studio_take_back();

	if (layout_alt_now) {
		// A pill each, and not one pill with two lines in it: the title and the
		// album's artist are two different things, a single box around both
		// pads the shorter one out to the length of the longer, and each has to
		// be able to scroll on its own.
		//
		// The labels are a percentage of their parent in the standard layout,
		// and a percentage of a parent that is itself sized by its contents is
		// nothing at all. Here they size themselves -- so each pill ends where
		// its own text ends -- up to what is left of the sleeve once the star's
		// disc and the margins are taken off.
		int room = cover_box_w - 2 * ALT_PAD - ALT_FAV_SIZE - 2 * ALT_PILL_PAD_H - 24;
		if (song_title_label) {
			lv_obj_set_parent(song_title_label, alt_title_pill);
			lv_obj_set_width(song_title_label, LV_SIZE_CONTENT);
			lv_obj_set_style_max_width(song_title_label, room, 0);
		}
		if (song_artist_label) {
			lv_obj_set_parent(song_artist_label, alt_artist_pill);
			lv_obj_set_width(song_artist_label, LV_SIZE_CONTENT);
			lv_obj_set_style_max_width(song_artist_label, room, 0);
		}

		if (fav_btn_obj && alt_fav_circle) {
			lv_obj_set_parent(fav_btn_obj, alt_fav_circle);
			lv_obj_center(fav_btn_obj);
		}
		if (format_label) {
			lv_obj_add_flag(format_label, LV_OBJ_FLAG_HIDDEN); // the sleeve is not the place for a bitrate
		}
		if (song_side_obj) {
			lv_obj_add_flag(song_side_obj, LV_OBJ_FLAG_HIDDEN);
		}
		lv_obj_add_flag(song_text_obj, LV_OBJ_FLAG_HIDDEN); // empty now, and it would still take a row

		lv_obj_remove_flag(alt_text_col, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(alt_fav_circle, LV_OBJ_FLAG_HIDDEN);
		// Over the sleeve and not under it: both were built before the picture
		// so that the picture would not have to be rebuilt around them.
		lv_obj_move_foreground(alt_text_col);
		lv_obj_move_foreground(alt_fav_circle);
		if (wave_canvas) {
			lv_obj_remove_flag(wave_box, LV_OBJ_FLAG_HIDDEN);
			lv_obj_move_to_index(wave_box, lv_obj_get_index(progress_slider));
			slider_over_waveform(true);
		}
	} else {
		if (song_title_label) {
			lv_obj_set_parent(song_title_label, song_text_obj);
			lv_obj_move_to_index(song_title_label, 0);
			lv_obj_set_width(song_title_label, lv_pct(100));
			lv_obj_set_style_max_width(song_title_label, LV_COORD_MAX, 0);
		}
		if (song_artist_label) {
			lv_obj_set_parent(song_artist_label, song_text_obj);
			lv_obj_move_to_index(song_artist_label, 1);
			lv_obj_set_width(song_artist_label, lv_pct(100));
			lv_obj_set_style_max_width(song_artist_label, LV_COORD_MAX, 0);
		}
		lv_obj_remove_flag(song_text_obj, LV_OBJ_FLAG_HIDDEN);

		if (fav_btn_obj && song_side_obj) {
			lv_obj_set_parent(fav_btn_obj, song_side_obj);
			lv_obj_move_to_index(fav_btn_obj, 0);
		}
		if (format_label) {
			lv_obj_remove_flag(format_label, LV_OBJ_FLAG_HIDDEN);
		}
		if (song_side_obj) {
			lv_obj_remove_flag(song_side_obj, LV_OBJ_FLAG_HIDDEN);
		}

		lv_obj_add_flag(alt_text_col, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(alt_fav_circle, LV_OBJ_FLAG_HIDDEN);
		slider_over_waveform(false);
		if (wave_box) {
			lv_obj_add_flag(wave_box, LV_OBJ_FLAG_HIDDEN);
		}
	}

	if (layout_studio_now) {
		studio_put();
	}

	paint_alt_tint();
}

// Decides which arrangement the thing now playing gets, and moves the pieces
// only if the answer changed.
//
// Called from every now-playing refresh and from the live-mode watch in the
// poll, which is what makes the arrangement follow the source instead of being
// settled once at startup. It has to run on the way out of a stream as well as
// on the way in, or the ordinary progress bar is left behind on top of the
// waveform: the bar's parent, size and transparency are decided here and
// nowhere else.
static void update_layout(const device_state_t *state) {
	// Studio only wants a picture and a name, and takes whatever is playing:
	// a file, a cached Qobuz or Tidal track, a podcast, a book, a station.
	bool studio = layout_choice == PLAYER_LAYOUT_STUDIO;
	bool alt = layout_choice == PLAYER_LAYOUT_ALTERNATIVE && playing_local_file(state);

	if (studio == layout_studio_now && alt == layout_alt_now && alt_title_pill) {
		return;
	}
	layout_studio_now = studio;
	layout_alt_now = alt;
	apply_layout();

	// The blurred copy is made at the shape of whatever shows it, so moving in
	// or out of Studio means the one on hand is the wrong shape and the picture
	// is asked for again. Only on a change of arrangement, which is a setting
	// the user has just touched.
	if (studio != backdrop_is_studio && cover_shown_path[0]) {
		reload_cover(cover_shown_path);
	}
}

// ---------------------------------------------------------------------------
// the shape of the track
// ---------------------------------------------------------------------------

// Paints the bars into the canvas buffer by hand.
//
// RGB565A8, and written directly rather than through the drawing pipeline: the
// gaps between the bars have to let the blurred sleeve through, an RGB565
// canvas would lay an opaque rectangle over it, and one LVGL object per bar is
// one more thing to lay out on every track change.
//
// The buffer is the colour plane followed by the alpha plane, which is what
// LVGL means by RGB565A8.
//
// Each bar is a capsule rather than a rectangle: `cap` is how far the drawn
// column is pulled in at that horizontal offset, from the circle of diameter
// `solid` that rounds the ends. A silent stretch is then a row of dots rather
// than a row of ticks, which is the shape this player's buttons and pills are
// already drawn in.
// The bars all start at the same place and are all the same width, and two
// callers besides the painter need to know where: one to work out how far along
// the playhead is in pixels, the other to compare that with what is on screen.
static void wave_geometry(int *bar_w, int *left) {
	int w = wave_w;
	int b = w / WAVEFORM_BARS;
	if (b < 1) {
		b = 1;
	}
	*bar_w = b;
	*left = (w - b * WAVEFORM_BARS) / 2;
}

// How far along the strip the playhead is, in pixels of the canvas.
//
// Pixels and not columns: a column is a forty-eighth of the track, so a column
// boundary stands still for several seconds and then jumps. In pixels the
// playhead moves by one every half second or so, and the bar it is inside is
// lit up to the playhead rather than all at once.
static int wave_played_px(double position, double total) {
	int bar_w, left;
	wave_geometry(&bar_w, &left);
	double fraction = total > 0.1 ? position / total : 0.0;
	if (fraction < 0.0) {
		fraction = 0.0;
	}
	if (fraction > 1.0) {
		fraction = 1.0;
	}
	return left + (int)(fraction * bar_w * WAVEFORM_BARS);
}

static void wave_paint(int played_px) {
	if (!wave_buf || !wave_canvas) {
		return;
	}

	// The buffer's own width, and never the object's: an object that has not
	// been laid out yet answers with something else, and painting to that
	// number writes past the end of the buffer.
	int w = wave_w;
	int h = WAVE_HEIGHT;
	if (w <= 0) {
		return;
	}

	uint16_t *colour = (uint16_t *)wave_buf;
	uint8_t *alpha = wave_buf + (size_t)w * h * 2;
	memset(alpha, 0, (size_t)w * h);

	lv_color_t tint = alt_mark();
	uint16_t packed = (uint16_t)(((tint.red & 0xF8) << 8) | ((tint.green & 0xFC) << 3) | (tint.blue >> 3));

	int bar_w, left;
	wave_geometry(&bar_w, &left);
	int solid = bar_w - WAVE_BAR_GAP;
	if (solid < 1) {
		solid = 1;
	}
	int middle = h / 2;

	// The tallest a bar may be drawn, with the cap and a pixel of air left over
	// so that a loud passage does not end flat against the top edge -- which is
	// exactly what reads as clipped audio.
	int reach = h / 2 - 1 - solid / 2;
	if (reach < WAVE_MIN_BAR) {
		reach = WAVE_MIN_BAR;
	}

	for (int i = 0; i < WAVEFORM_BARS; i++) {
		int value = wave_have ? wave_bars[i] : 0;
		int half = value * reach / 255;
		if (half < WAVE_MIN_BAR) {
			half = WAVE_MIN_BAR;
		}
		int x0 = left + i * bar_w;
		for (int dx = 0; dx < solid; dx++) {
			int x = x0 + dx;
			if (x < 0 || x >= w) {
				continue;
			}
			// Decided per column of pixels rather than per bar, which is what
			// lets the playhead stand in the middle of a bar.
			uint8_t opa = x < played_px ? WAVE_PAST_OPA : WAVE_TODO_OPA;
			// Doubled coordinates, so that an even-width bar can have its
			// centre between two pixels without any rounding.
			int d2 = 2 * dx - (solid - 1);
			int cap = (solid - (int)utils_isqrt32((uint32_t)(solid * solid - d2 * d2))) / 2;

			for (int y = middle - half + cap; y <= middle + half - cap; y++) {
				if (y < 0 || y >= h) {
					continue;
				}
				size_t at = (size_t)y * w + x;
				colour[at] = packed;
				alpha[at] = opa;
			}
		}
	}

	lv_obj_invalidate(wave_canvas);
}

// Moves the played part of the shape to `seconds`, if that has moved it at all.
//
// The guard is what keeps this cheap enough to call at fifteen frames a second:
// repainting is eighty kilobytes of buffer and an invalidate, and on a track of
// any length the answer changes about twice a second.
static void wave_preview(double seconds) {
	if (!layout_alt_now || !wave_canvas || current_total_length <= 0.1) {
		return;
	}
	int px = wave_played_px(seconds, current_total_length);
	if (px == wave_drawn) {
		return;
	}
	wave_drawn = px;
	wave_paint(px);
}

// Asks for this track's shape and repaints when either the shape or the
// playhead has moved. Called from the progress poll, so it runs twice a second
// while something is playing and once every two seconds otherwise.
static void wave_refresh(const char *path, double position, double total) {
	if (!layout_alt_now || !wave_canvas) {
		return;
	}

	bool had = wave_have;
	if (path && path[0]) {
		wave_have = waveform_get(path, wave_bars);
	} else {
		wave_have = false;
	}

	int px = wave_played_px(position, total);
	uint32_t tone = album_tone;
	if (px == wave_drawn && wave_have == had && tone == wave_drawn_tone) {
		return;
	}
	wave_drawn = px;
	wave_drawn_tone = tone;
	wave_paint(px);
}

// Throws away what is on screen and asks for the artwork of `filepath`. The
// half of refresh_cover() that runs once it is known that the picture really is
// changing.
static void reload_cover(const char *filepath) {
	if (cover_release_cb) {
		cover_release_cb();
	}

	// Detach the old pixels before freeing them, so nothing can draw from a
	// buffer that's already gone. The placeholder covers the gap while the
	// worker decodes.
	lv_image_set_src(cover_img, NULL);
	lv_obj_set_style_bg_image_src(player_menu, NULL, 0);
	cover_free(&current_cover);
	cover_free(&current_backdrop);

	cover_show(false);
	// Studio holds pointers to both of those, so it is told in the same breath:
	// its sleeve and its background come down and the empty square goes up for
	// as long as the decode takes.
	if (layout_studio_now) {
		studio_refresh_cover();
	}

	// Backdrop gone: the chrome goes back to following the theme.
	set_over_cover(false);
	album_tone = 0;
	paint_alt_tint();

	lv_obj_invalidate(cover_panel);
	lv_obj_invalidate(player_menu);

	if (filepath && filepath[0]) {
		backdrop_is_studio = layout_studio_now;
		cover_set_backdrop_upright(backdrop_is_studio);

		// The sleeve is asked for at the size the arrangement in force will
		// draw it. A picture that does not match its widget is resampled by
		// LVGL on every frame it is drawn, and every frame is what a sheet
		// being dragged, or the control centre coming down over it, means: in
		// Studio the picture is decoded at the width of the screen and shown
		// at two thirds of it, and that resampling is the interface's core
		// being spent again and again on an answer that never changes.
		int cover_w = backdrop_is_studio ? studio_cover_geometry(NULL) : cover_box_w;
		int cover_h = backdrop_is_studio ? cover_w : cover_box_h;
		coverloader_request_player(filepath, cover_w, cover_h, backdrop_w,
								   backdrop_is_studio ? backdrop_studio_h : backdrop_h);
		cover_request_outstanding = true;

		// The collector rides on the progress timer; make sure it is ticking
		// even when nothing is playing (paused player, theme change).
		lv_timer_resume(progress_slider_timer);
	} else {
		cover_request_outstanding = false;
	}
}

// A track change.
//
// The next track of a record carries the record's cover, and so does the next
// file in a folder that keeps its picture beside the music. Decoding it again
// is a second of this processor's time for a picture that does not change, and
// the cover blinking out to the placeholder and back is the part the user sees.
//
// So nothing is thrown away yet: the loader is asked what the new track's
// artwork is -- which it answers by reading the picture, not by decoding it --
// and only an answer that differs from what is on screen costs anything. When
// it does differ the old cover stays up for the one poll that takes, which
// still reads better than a placeholder in the gap.
static void refresh_cover(const char *filepath) {
	if (!cover_img)
		return;

	snprintf(cover_shown_path, sizeof(cover_shown_path), "%s", filepath ? filepath : "");

	if (cover_shown_id != 0 && current_cover.pixels && filepath && filepath[0]) {
		cover_id_state = COVER_ID_NEW_TRACK;
		coverloader_request_player_id(filepath);
		lv_timer_resume(progress_slider_timer);
		return;
	}

	cover_id_state = COVER_ID_IDLE;
	cover_incoming_id = 0;
	reload_cover(filepath);
}

// The answer to whichever question was asked, picked up on the progress poll.
static void apply_cover_id_result(void) {
	if (cover_id_state == COVER_ID_IDLE) {
		return;
	}
	uint64_t id = 0;
	bool finished = false;
	coverloader_take_player_id(&id, &finished);
	if (!finished) {
		return;
	}

	if (cover_id_state == COVER_ID_MEASURE) {
		cover_id_state = COVER_ID_IDLE;
		cover_shown_id = id;
		return;
	}

	cover_id_state = COVER_ID_IDLE;

	// The same picture: the cover, the backdrop, the sleeve colour and the
	// chrome all stay exactly as they are. This is the whole point.
	if (id != 0 && id == cover_shown_id) {
		return;
	}

	// Different, and its id is already known: the decode does not have to be
	// asked what it produced.
	cover_incoming_id = id;
	reload_cover(cover_shown_path);
}

// Picks up the worker's finished artwork, if any. Runs on the UI thread from
// the progress timer, so touching widgets here is allowed.
static void apply_cover_result(void) {
	if (!cover_request_outstanding) {
		return;
	}

	cover_image_t new_cover, new_backdrop;
	bool finished = false;
	bool has_cover = coverloader_take_player(&new_cover, &new_backdrop, &finished);

	if (!finished) {
		return; // still decoding; the placeholder stays up
	}
	cover_request_outstanding = false;

	if (has_cover) {
		// What is now on screen, so the next track can be asked whether its
		// artwork is this same picture. Usually known already; when it is not
		// -- the first cover after the player opens, a station's icon -- the
		// loader is asked, because working the id out reads the file and this
		// is the interface thread.
		cover_shown_id = cover_incoming_id;
		if (cover_shown_id == 0 && cover_shown_path[0]) {
			cover_id_state = COVER_ID_MEASURE;
			coverloader_request_player_id(cover_shown_path);
		}

		current_cover = new_cover;
		current_cover.dsc.data = current_cover.pixels; // struct moved, buffer didn't

		lv_image_set_src(cover_img, &current_cover.dsc);
		lv_obj_set_size(cover_img, current_cover.dsc.header.w, current_cover.dsc.header.h);
		lv_obj_center(cover_img);

		cover_show(true);

		if (new_backdrop.pixels) {
			current_backdrop = new_backdrop;
			current_backdrop.dsc.data = current_backdrop.pixels;
			lv_obj_set_style_bg_image_src(player_menu, &current_backdrop.dsc, 0);
		}

		// The dark backdrop is up: light chrome, in both themes.
		set_over_cover(true);

		// And the sleeve's own colour, which is what the alternative layout is
		// drawn in.
		album_tone = cover_dominant_tone(&current_cover);
		paint_alt_tint();

		lv_obj_invalidate(cover_panel);
		lv_obj_invalidate(player_menu);
	}
	if (!has_cover) {
		cover_shown_id = 0; // nothing to compare the next track against
	}
	cover_incoming_id = 0;

	// Studio draws both of these itself -- the sleeve smaller and the blurred
	// copy across the whole screen -- so it is told here, where they land,
	// rather than asked on a timer.
	if (layout_studio_now) {
		studio_refresh_cover();
	}
	// No artwork: the placeholder is already showing.
}

// The artwork the player has already decoded for the loaded track, lent to
// anything else that wants to draw it (the screensaver). NULL while a track
// has no cover or its decode is still in flight -- never a stale pointer:
// refresh_cover() zeroes both before the buffers are freed.
const lv_image_dsc_t *player_cover_image(void) { return current_cover.pixels ? &current_cover.dsc : NULL; }

const lv_image_dsc_t *player_backdrop_image(void) {
	return current_backdrop.pixels ? &current_backdrop.dsc : NULL;
}

static void update_repeat_button(void);

// Repaints the bits that are coloured by hand, and rebuilds the blurred
// artwork behind the controls -- it is dimmed towards the page background, so
// it has to be regenerated when that background changes.
static void player_refresh_theme(void) {
	if (!cover_panel)
		return;

	if (studio_box) {
		lv_obj_set_style_bg_color(studio_box, theme()->screen_bg, 0);
	}

	lv_obj_set_style_bg_color(cover_panel, theme()->cover_bg, 0);
	lv_obj_set_style_image_recolor(cover_placeholder_icon, theme()->text_secondary, 0);
	// The backdrop over a loaded cover is always the dark treatment -- same
	// look in both themes; only the no-cover panel follows the theme.
	cover_set_backdrop_light(false);
	lv_obj_set_style_bg_color(progress_slider, theme()->accent, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(progress_slider, theme()->accent, LV_PART_KNOB);
	set_over_cover(chrome_over_cover); // repaints the play disc for the new palette
	update_repeat_button();
	update_speed_button();

	device_state_t state;
	device_state_get(&state);
	refresh_cover(state.current_file);
}

static void update_qobuz_badge(void);

// Refreshes the now-playing info (title + album artist) from the current
// device state and resets the per-track length cache. Used both when the user
// picks a file and when playback auto-advances to a new track.
static void refresh_now_playing(void) {
	current_total_length = 0; // unknown until the playback thread reports it

	// The library lists carry an accent mark on the playing track (and on the
	// album and artist it belongs to). This is the one place every track
	// change goes through, so it is where they are told.
	medialist_notify_now_playing();
	browser_notify_now_playing();
	trackmenu_notify_now_playing();

	// Wind the progress display back to the start straight away, so the bar
	// never sits at the previous track's position while the new one loads.
	lv_slider_set_value(progress_slider, 0, LV_ANIM_OFF);
	set_progress_label(0, 0);

	device_state_t state;
	device_state_get(&state);

	apply_live_mode(state.live);
	// Before anything is drawn: which arrangement this source gets decides
	// where the title goes and what colour everything is.
	update_layout(&state);
	// Decided from the path every time, as for books: a flag raised at start
	// would have to be cleared on every route by which the track can change,
	// and the first one forgotten leaves the player dressed as a podcast over
	// a song.
	apply_audiobook_mode(!state.live && audiobook_is_playing(),
						 !state.live && podcastcache_owns(state.current_file));

	if (state.live) {
		// The station on top, what it says is on air underneath -- the same
		// two lines as a track, carrying the two things a radio knows.
		scrolltext_set(song_title_label, state.metadata.title);
		scrolltext_set(song_artist_label, state.metadata.artist);
		alt_pills_sync();

		update_fav_button();
		update_format_label(&state);

		// The artwork is the station's own icon, downloaded when it started.
		// An empty path simply leaves the radio placeholder up.
		refresh_cover(state.live_cover);
		return;
	}

	const char *file = state.current_file;
	const char *slash = strrchr(file, '/');
	scrolltext_set(song_title_label, state.metadata.title[0] ? state.metadata.title : (slash ? slash + 1 : file));

	// The album's artist, not this track's: on a compilation every track has
	// its own performer, but the album is credited to one name. Fall back to
	// the track artist when the file has no album artist tag.
	const char *artist = state.metadata.album_artist[0] ? state.metadata.album_artist : state.metadata.artist;
	scrolltext_set(song_artist_label, artist);
	alt_pills_sync();

	update_fav_button();
	update_format_label(&state);
	update_qobuz_badge();

	refresh_cover(file);
}

// The parts of the now-playing display a live stream changes while it runs:
// the station name (the stream's own name arrives with the connection), what
// is on air (an ICY title, every few minutes), the bitrate, and the artwork
// once the icon has been downloaded. Deliberately not a full
// refresh_now_playing(): that throws the decoded cover away and asks for it
// again, which for a station whose icon has not changed is work for nothing.
static void refresh_live_texts(const device_state_t *state) {
	scrolltext_set(song_title_label, state->metadata.title);
	scrolltext_set(song_artist_label, state->metadata.artist);
	alt_pills_sync();
	update_fav_button();
	update_format_label(state);

	if (strcmp(cover_shown_path, state->live_cover) != 0) {
		refresh_cover(state->live_cover);
	}
}

// The mirror of the Qobuz favourites has arrived, so the star may need to
// change face. It comes from the qobuzsync worker, hence the bounce onto the
// UI thread.
static void update_fav_button(void);

static void fav_refresh_async(void *user) {
	(void)user;
	update_fav_button();
}

static void qobuz_favorites_changed(void) { gui_post(fav_refresh_async, NULL); }
static void tidal_favorites_changed(void) { gui_post(fav_refresh_async, NULL); }

// The Qobuz id of the loaded track, 0 when what is playing does not come from
// Qobuz. This is the question that splits the star and "add to playlist" in
// two: a user's own file is written to the local database, a Qobuz track to
// the account.
static long current_qobuz_track(void) {
	device_state_t state;
	device_state_get(&state);
	return state.current_file[0] ? qobuzcache_track_id(state.current_file) : 0;
}

// The podcast the playing episode belongs to, plus what following it needs:
// title, author and cover. All from the sidecar written when the episode was
// downloaded -- no network, which listening to an already cached episode may
// well not have.
static bool podcast_current_feed(long long *id_out, podcast_feed_t *feed_out) {
	device_state_t state;
	device_state_get(&state);
	if (state.live || !state.current_file[0]) {
		return false;
	}
	long long id = 0;
	if (!podcastcache_feed_id(state.current_file, &id) || id <= 0) {
		return false;
	}
	if (id_out) {
		*id_out = id;
	}
	if (feed_out) {
		memset(feed_out, 0, sizeof(*feed_out));
		feed_out->id = id;
		// The podcast name is also the episode's album tag: see
		// podcastcache_write_sidecars(), which writes it to both places on
		// purpose.
		snprintf(feed_out->title, sizeof(feed_out->title), "%.*s", (int)sizeof(feed_out->title) - 1,
				 state.metadata.album);
		podcastcache_tag(state.current_file, "feed_author", feed_out->author, sizeof(feed_out->author));
		podcastcache_tag(state.current_file, "feed_image", feed_out->image, sizeof(feed_out->image));
	}
	return true;
}

static long long current_podcast_episode(void) {
	device_state_t state;
	device_state_get(&state);
	return state.current_file[0] ? podcastcache_episode_id(state.current_file) : 0;
}

// The playing episode's podcast, for callers outside the player (the control
// centre): the same lookup the player uses for its own star, exposed so both
// stars say the same thing.
bool player_current_podcast_feed(long long *id_out, podcast_feed_t *feed_out) {
	return podcast_current_feed(id_out, feed_out);
}

// The same for Tidal. Two separate questions rather than one returning "which
// service": the two caches live in different directories, so a path can answer
// yes to at most one, which makes it impossible by construction for the player
// to believe a track comes from both.
static long current_tidal_track(void) {
	device_state_t state;
	device_state_get(&state);
	return state.current_file[0] ? tidalcache_track_id(state.current_file) : 0;
}

// The Qobuz mark, top right, when what is playing comes from there. Same place
// and same purpose as the live badge for radio: say where the music comes from
// without taking space from anything else.
//
// The question is the star's own -- is the file in the Qobuz cache? -- so
// there is no extra state to keep in step.
static void update_qobuz_badge(void) {
	// The marks share one corner and can never appear together: a track comes
	// from one service only. So which one shows is decided here, rather than
	// leaving each badge to hide itself -- which is how they would eventually
	// have overlapped.
	bool from_qobuz = !live_mode && current_qobuz_track() != 0;
	bool from_tidal = !live_mode && !from_qobuz && current_tidal_track() != 0;
	bool from_podcast = !live_mode && !from_qobuz && !from_tidal && current_podcast_episode() != 0;

	if (qobuz_badge) {
		if (from_qobuz) {
			lv_obj_remove_flag(qobuz_badge, LV_OBJ_FLAG_HIDDEN);
			lv_obj_move_foreground(qobuz_badge);
		} else {
			lv_obj_add_flag(qobuz_badge, LV_OBJ_FLAG_HIDDEN);
		}
	}
	if (tidal_badge) {
		if (from_tidal) {
			lv_obj_remove_flag(tidal_badge, LV_OBJ_FLAG_HIDDEN);
			lv_obj_move_foreground(tidal_badge);
		} else {
			lv_obj_add_flag(tidal_badge, LV_OBJ_FLAG_HIDDEN);
		}
	}
	if (podcast_badge) {
		if (from_podcast) {
			lv_obj_remove_flag(podcast_badge, LV_OBJ_FLAG_HIDDEN);
			lv_obj_move_foreground(podcast_badge);
		} else {
			lv_obj_add_flag(podcast_badge, LV_OBJ_FLAG_HIDDEN);
		}
	}
}

// Repaints the star from the database: filled yellow when the loaded track
// is a favourite, quiet outline otherwise.
static void update_fav_button(void) {
	if (!fav_btn_icon) {
		return;
	}

	// A station is starred in the radio favourites, not the music library:
	// there is no file to put in the database, and the list the star feeds is
	// the one on the Radio page.
	radio_station_t station;
	bool starred = false;
	long qobuz_id = 0;
	long tidal_id = 0;
	long long feed_id = 0;
	if (radio_current_station(&station)) {
		starred = radio_fav_contains(station.uuid);
	} else if (podcast_current_feed(&feed_id, NULL)) {
		// On a podcast the star does not mark a liked track: it follows the
		// podcast, so it turns up in the followed list without searching. The
		// list is on the card, so the answer is immediate and nothing has to
		// be asked of the network.
		starred = podcastsubs_is_followed(feed_id);
	} else if ((qobuz_id = current_qobuz_track()) > 0) {
		// A Qobuz track: the star reports what is on the account, not what is
		// in the local database. Until the mirror arrives it is drawn empty
		// and asked for -- one request per session, not one per song.
		starred = qobuzsync_is_favorite(qobuz_id);
		qobuzsync_favorites_refresh(qobuz_favorites_changed);
	} else if ((tidal_id = current_tidal_track()) > 0) {
		starred = tidalsync_is_favorite(tidal_id);
		tidalsync_favorites_refresh(tidal_favorites_changed);
	} else {
		// device_state, not audio_get_current_file(): the star is drawn from
		// one source and toggled from another, and when the two disagreed the
		// star never filled in -- the tap wrote the right track to the
		// database while the drawing looked up a different one.
		device_state_t state;
		device_state_get(&state);
		starred = state.current_file[0] && library_fav_contains(state.current_file);
	}

	// The star's colour is decided here and nowhere else. This function runs
	// last on every track change, so anything that tints the star elsewhere is
	// overwritten.
	//
	// An empty star is drawn in the disc's own ink so that it reads on it; a
	// filled one keeps the yellow, which is the same on every page of the
	// player and is what says "favourite" rather than "here is the album's
	// colour again".
	if (starred) {
		lv_image_set_src(fav_btn_icon, &icon_star_filled);
		lv_obj_set_style_image_recolor(fav_btn_icon, lv_color_make(246, 211, 45), 0); // Adwaita yellow
	} else {
		lv_image_set_src(fav_btn_icon, &icon_star);
		lv_obj_set_style_image_recolor(fav_btn_icon, layout_alt_now ? alt_ink() : lv_color_make(150, 150, 150), 0);
	}
	lv_obj_set_style_image_recolor_opa(fav_btn_icon, LV_OPA_COVER, 0);
}

// The outcome of a Qobuz star, from the qobuzsync worker. Success says
// nothing -- the filled star already did; failure has to be said, because the
// star had filled in optimistically and is about to empty again.
static char fav_error[192];

static void fav_error_async(void *user) {
	(void)user;
	if (fav_error[0]) {
		gui_notify_popup(fav_error);
	}
	update_fav_button();
}

static void qobuz_fav_done(bool ok, const char *error, void *user) {
	(void)user;
	snprintf(fav_error, sizeof(fav_error), "%s", ok ? "" : (error && error[0] ? error : tr("qobuz_favourites_failed")));
	gui_post(fav_error_async, NULL);
}

static void tidal_fav_done(bool ok, const char *error, void *user) {
	(void)user;
	snprintf(fav_error, sizeof(fav_error), "%s", ok ? "" : (error && error[0] ? error : tr("tidal_favourites_failed")));
	gui_post(fav_error_async, NULL);
}

static void fav_btn_event_cb(lv_event_t *e) {
	(void)e;

	radio_station_t station;
	if (radio_current_station(&station)) {
		if (radio_fav_contains(station.uuid)) {
			radio_fav_remove(station.uuid);
		} else {
			radio_fav_add(&station);
		}
		update_fav_button();
		return;
	}

	// A podcast: the star follows or unfollows it. Written to the card at
	// once, so switching the player off a moment later does not lose it.
	long long feed_id = 0;
	podcast_feed_t feed;
	if (podcast_current_feed(&feed_id, &feed)) {
		if (podcastsubs_is_followed(feed_id)) {
			podcastsubs_unfollow(feed_id);
		} else {
			podcastsubs_follow(&feed);
		}
		update_fav_button();
		return;
	}

	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0]) {
		return;
	}

	// A Qobuz track goes into the account's favourites. Saving it in the local
	// database would mean keeping the path of a cache file that will be gone
	// twenty tracks from now, and invisible from any other device.
	long qobuz_id = qobuzcache_track_id(state.current_file);
	if (qobuz_id > 0) {
		bool want = !qobuzsync_is_favorite(qobuz_id);
		qobuzsync_favorite_toggle(qobuz_id, want, qobuz_fav_done, NULL);
		update_fav_button(); // the mirror has already flipped: shows at once
		return;
	}

	long tidal_id = tidalcache_track_id(state.current_file);
	if (tidal_id > 0) {
		bool want = !tidalsync_is_favorite(tidal_id);
		tidalsync_favorite_toggle(tidal_id, want, tidal_fav_done, NULL);
		update_fav_button();
		return;
	}

	const char *title = state.metadata.title[0] ? state.metadata.title : strrchr(state.current_file, '/') + 1;
	// No notification popup: the star filling in is the whole feedback.
	library_fav_toggle(state.current_file, title, state.metadata.artist);
	update_fav_button();
}

// The format line, the way the stock player writes it: "16/44.1 FLAC".
static void update_format_label(const device_state_t *state) {
	// The mark beside it in Studio, which is the same question asked of the
	// stream rather than of the library. Here because this is where the format
	// line is worked out, and the two say one thing between them.
	if (layout_studio_now && studio_quality_icon) {
		const lv_image_dsc_t *mark = studio_quality_mark(state);
		if (mark) {
			lv_image_set_src(studio_quality_icon, mark);
			lv_obj_remove_flag(studio_quality_icon, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(studio_quality_icon, LV_OBJ_FLAG_HIDDEN);
		}
	}

	if (!format_label) {
		return;
	}

	if (state->live) {
		// No bit depth to quote on a stream: what a station is, is its
		// bitrate. Nothing at all until the first frame has been decoded.
		radio_now_t now;
		radio_get_now(&now);
		// The format was written "MP3" because that was the only thing the
		// stream path could decode. It reads AAC too now, and a station that
		// says MP3 while an AAC decoder is running on it is a lie the log
		// cannot correct.
		if (now.bitrate > 0) {
			lv_label_set_text_fmt(format_label, "%d kbps %s", now.bitrate, now.codec[0] ? now.codec : "MP3");
		} else if (now.codec[0]) {
			lv_label_set_text(format_label, now.codec);
		} else {
			lv_label_set_text(format_label, "");
		}
		return;
	}

	if (state->stream_sample_rate <= 0 || !state->current_file[0]) {
		lv_label_set_text(format_label, "");
		return;
	}

	// Nothing on a podcast. "16/44.1 MP3" is true and useless: an episode is
	// not chosen for its audio quality, there is no better version to buy, and
	// the number takes the line under the title without saying anything worth
	// knowing.
	if (podcastcache_owns(state->current_file)) {
		lv_label_set_text(format_label, "");
		return;
	}

	const char *dot = strrchr(state->current_file, '.');
	char kind[8] = "";
	if (dot) {
		size_t k = 0;
		for (const char *c = dot + 1; *c && k < sizeof(kind) - 1; c++) {
			kind[k++] = (char)((*c >= 'a' && *c <= 'z') ? *c - 32 : *c);
		}
		kind[k] = '\0';
	}

	// A DSD track says what it is rather than what carries it: "24/176.4 DSF"
	// is true of a DSD64 file and of a DSD256 one, and tells the listener
	// nothing. How it gets there is not said: DoP is the only way it does.
	int dsd = audio_get_dsd_multiple();
	if (dsd > 0) {
		lv_label_set_text_fmt(format_label, "DSD%d", dsd);
		return;
	}

	// The codec name comes from the decoder, not the extension: an .m4a holds
	// either AAC or ALAC, and those read as two different things under a
	// cover. When the decoder has no name to give -- the libsndfile formats --
	// the extension still serves.
	char codec[16] = "";
	audio_get_stream_codec(codec, sizeof(codec));
	const char *name = codec[0] ? codec : kind;

	// A lossy format has no bit depth of its own: "16/44.1 MP3" describes the
	// PCM leaving the decoder, not the file, and is equally true of a 96 kbps
	// MP3 and a 320 kbps one. What an MP3, an AAC or an Opus is -- exactly as
	// for a radio station -- is its bitrate.
	if (audio_stream_is_lossy()) {
		// The nominal figure first, the one written in the file. It is the
		// only one showable on a Qobuz or Tidal track: the cache file is still
		// growing while it plays, so the bytes-over-seconds average climbed on
		// screen until the download finished and it settled.
		int kbps = audio_get_stream_bitrate_kbps();

		// When the file does not declare one (a variable-bitrate MP3, a silent
		// AAC) the real average stands in -- but only once the download has
		// finished, for the reason just given.
		struct stat st;
		if (kbps <= 0 && !qobuzcache_owns(state->current_file) && !tidalcache_owns(state->current_file) &&
			state->progress_total_secs > 0.5 && stat(state->current_file, &st) == 0 && st.st_size > 0) {
			kbps = (int)((double)st.st_size * 8.0 / state->progress_total_secs / 1000.0 + 0.5);
		}

		if (kbps > 0) {
			lv_label_set_text_fmt(format_label, "%d kbps %s", kbps, name);
			return;
		}
		// Neither one: the format name alone beats quoting a bit depth the
		// file does not have.
		lv_label_set_text(format_label, name);
		return;
	}

	int bits = audio_get_stream_bits();
	lv_label_set_text_fmt(format_label, "%d/%g %s", bits > 0 ? bits : 16, state->stream_sample_rate / 1000.0, name);
}

// The gauge wears the accent colour whenever the book is not at normal speed,
// the way the repeat glyph it stands in for marks a mode that is on. That is
// the only question the button has to answer at a glance: is this playing at
// the speed it was read at?
static void update_speed_button(void) {
	if (!speed_btn_icon) {
		return;
	}
	bool normal = audiobook_speed_permille() == AUDIOBOOK_SPEED_NORMAL;
	lv_obj_set_style_image_recolor(speed_btn_icon, normal ? lv_color_make(100, 100, 100) : theme()->accent, 0);
	lv_obj_set_style_image_recolor_opa(speed_btn_icon, LV_OPA_COVER, 0);
}

static void speed_pick(void *user) {
	int permille = (int)(intptr_t)user;
	audiobook_set_speed_permille(permille);
	// Straight through to the playback thread: it picks the factor up on the
	// next block, so the change is heard within a period rather than at the
	// next track.
	audio_set_speed(audiobook_speed());
	update_speed_button();
}

static void speed_btn_event_cb(lv_event_t *e) {
	static const int CHOICES[] = {500, 1000, 1500, 2000};
	static const char *const LABELS[] = {"0.5x", "1.0x", "1.5x", "2.0x"};

	// Zeroed, not just filled field by field: popover_item_t also has
	// `checked`, and stack garbage in it puts a tick on a random entry -- along
	// with the different row layout a tick brings.
	//
	// The tick belongs here for the same reason it does in the Qobuz quality
	// menu: this is a choice between alternatives, not a list of actions, so
	// the current one has to be visible.
	popover_item_t items[4] = {0};
	int current = audiobook_speed_permille();
	for (int i = 0; i < 4; i++) {
		items[i].label = LABELS[i];
		items[i].action = speed_pick;
		items[i].user = (void *)(intptr_t)CHOICES[i];
		items[i].checked = CHOICES[i] == current;
	}
	popover_show(lv_event_get_current_target(e), items, 4);
}

// Updates the repeat-mode button's icon and colour to reflect the active mode.
// The icons are the white ARGB bitmaps generated from assets/icons/*.svg;
// recolouring them is what marks the mode as active or not.
static void update_repeat_button(void) {
	// The active mode wears the accent colour of the moment, read each time
	// rather than frozen, so it follows a change of accent.
	lv_color_t active = chrome_accent();
	lv_color_t inactive = lv_color_make(100, 100, 100);

	const lv_image_dsc_t *icon;
	lv_color_t color;

	switch (playlist_get_mode()) {
	case PLAYBACK_MODE_REPEAT_ONE:
		icon = &icon_repeat_one;
		color = active;
		break;
	case PLAYBACK_MODE_REPEAT_ALL:
		icon = &icon_repeat_all;
		color = active;
		break;
	case PLAYBACK_MODE_SHUFFLE:
		icon = &icon_shuffle;
		color = active;
		break;
	case PLAYBACK_MODE_SHUFFLE_REPEAT:
		icon = &icon_shuffle_repeat;
		color = active;
		break;
	case PLAYBACK_MODE_NORMAL:
	default:
		icon = &icon_repeat_off;
		color = inactive;
		break;
	}

	lv_image_set_src(repeat_btn_icon, icon);
	lv_obj_set_style_image_recolor(repeat_btn_icon, color, 0);
	lv_obj_set_style_image_recolor_opa(repeat_btn_icon, LV_OPA_COVER, 0);
}

// Called when the current track finished on its own: advances the folder queue
// per the active playback mode. If a next track starts, refresh the UI to it;
// otherwise playback simply stays stopped (end of folder in normal mode).
static void handle_track_finished(void) {
	// What is playing came from DLNA: the queue belongs to the phone, not to
	// the player. Tell it the track finished and wait for the next URL,
	// instead of drawing from a local queue one entry long.
	if (dlna_owns_playback()) {
		dlna_notify_finished();
		return;
	}

	// "Stop at the end of the episode": the queue is left exactly where it is,
	// which is what the player already does when a queue simply runs out. Asked
	// here and not at a boundary inside the file, because for a podcast the end
	// of the episode IS the end of the track -- there is no chapter to watch
	// for, which is what makes this the cheaper half of the audiobook's version.
	char path[512];
	audio_get_current_file(path, sizeof(path));
	if (podcast_stop_at_episode_end() && podcastcache_owns(path)) {
		fprintf(stderr, "player: end of the episode; not starting the next\n");
		refresh_now_playing();
		return;
	}

	if (device_state_advance_auto(NULL, 0)) {
		refresh_now_playing();
	}
}

// Reads the current device state and reconciles the UI against it. This is the
// single point that keeps the play/pause button and the progress bar from
// going stale once a track finishes on its own.
static void update_progress(void) {
	// Auto-advance or loop the folder when the track has played through.
	if (device_state_take_completion()) {
		handle_track_finished();
	}

	device_state_t state;
	device_state_get(&state);

	// The live source moves under the player: the station's own name comes
	// with the connection, the on-air title arrives later and keeps changing,
	// the artwork lands when its download finishes -- and the whole thing
	// ends by itself when a station drops for good. None of that goes through
	// a track change, so it is followed here.
	{
		static bool was_live;
		static unsigned last_serial;
		unsigned serial = radio_now_serial();

		if (state.live != was_live) {
			was_live = state.live;
			last_serial = serial;
			// In or out of live: the whole display changes hands.
			refresh_now_playing();
			device_state_get(&state);
		} else if (state.live && serial != last_serial) {
			last_serial = serial;
			refresh_live_texts(&state);
		}
	}

	// Asked on every poll and not only at a track change, for the routes that
	// are not one: a phone claiming the output over AirPlay does not change the
	// track -- it takes the DAC away under the one that is loaded -- and the
	// answer here has to follow it. Two string compares and a struct copy,
	// twice a second.
	update_layout(&state);

	if (state.progress_total_secs > 0) {
		int value = (state.progress_current_secs / state.progress_total_secs) * 1000;
		lv_slider_set_value(progress_slider, value, LV_ANIM_OFF);

		current_total_length = state.progress_total_secs;
	}

	// The truth the smoothing works from, and the moment it was true. Taken
	// here and not in the smoothing itself, so the two can never drift: every
	// poll puts the interpolation back on the playback thread's own figure.
	progress_anchor_secs = state.progress_current_secs;
	progress_anchor_tick = lv_tick_get();
	progress_running = state.status == AUDIO_STATUS_PLAYING && state.progress_total_secs > 0;
	if (smooth_timer) {
		if (progress_running) {
			lv_timer_resume(smooth_timer);
		} else {
			lv_timer_pause(smooth_timer);
		}
	}

	set_progress_label(state.progress_current_secs, current_total_length);
	wave_refresh(cover_shown_path, state.progress_current_secs, current_total_length);
	apply_playback_status(state.status);
	// Asked every poll rather than only on a track change: the answer comes
	// from the audiobook index, which a scan finishing can change under a
	// track that is already playing. Both answers come from the path every
	// time -- a flag raised at start would have to be cleared on every route
	// by which the track can change, and the first one forgotten leaves the
	// player dressed as a podcast over a song.
	apply_audiobook_mode(!state.live && audiobook_is_playing(),
						 !state.live && podcastcache_owns(state.current_file));
	update_format_label(&state); // the rate/bits settle shortly after the start

	// The status LED follows playback promptly from here (the battery poll
	// only comes round once a minute). Only writes when the colour changes.
	// A podcast lights purple instead of the sample-rate colour.
	led_update_playback(state.status == AUDIO_STATUS_PLAYING, state.stream_sample_rate,
						!state.live && podcastcache_owns(state.current_file));

	// Where the book got to. Not the same thing as "remember track" below and
	// not subject to its switch: that setting is about which track comes back
	// on the next boot, while this is the whole point of a book -- it is
	// remembered per book, it survives listening to something else in between,
	// and turning it off would make the feature meaningless. Throttled inside
	// audiobook_note_position(); forced whenever playback stops or pauses, so
	// a shutdown loses at most a few seconds.
	if (!state.live && audiobook_mode) {
		bool stopped_itself = audiobook_auto_stop(&state);

		static audio_status_t book_last_status = AUDIO_STATUS_STOPPED;
		bool changed = state.status != book_last_status;
		book_last_status = state.status;
		// Not when it has just stopped itself: that path already wrote the
		// position it stopped at, and this snapshot is from before the seek.
		if (!stopped_itself) {
			audiobook_note_position(state.progress_current_secs, state.progress_total_secs,
									changed || state.status != AUDIO_STATUS_PLAYING);
		}
	}

	// "Remember track". The decision, and the writing, live in device_state.c:
	// the power menu and the automatic shutdown have to do the same thing on
	// their way out, and neither of them comes through here.
	device_state_remember_note();
}

static void play_btn_event_cb(lv_event_t *e) {
	(void)e;

	// A live stream has no position to come back to, so this is a stop, not a
	// pause: the connection goes, the station stays on screen, and pressing
	// play again reconnects to it.
	if (radio_is_active()) {
		if (radio_is_playing()) {
			radio_stop();
		} else {
			radio_resume();
		}
		player_refresh_now_playing();
		return;
	}

	audio_status_t new_status = device_state_toggle_play_pause();
	apply_playback_status(new_status); // optimistic UI update; next poll reconciles against the real status
}

// What the on-screen prev/next buttons do. On a book or a podcast they are
// jumps of seconds; on a track they are previous/next, Spotify style -- within
// the first few seconds of a track prev moves to the previous one, later it
// restarts the current one. Public so the control centre can do exactly this:
// the same buttons cannot mean two different things in the two places. (The
// physical side keys stay episode changes on a podcast: see
// player_key_prev/next.)
void player_screen_prev(void) {
	device_state_t state;
	device_state_get(&state);

	// On a book these two are not "previous chapter" and "next chapter": they
	// are ten seconds back and ten seconds on, which is what somebody who
	// missed a sentence reaches for. Chapters have their own list.
	if (audiobook_mode || podcast_mode) {
		int back = podcast_mode && !audiobook_mode ? podcast_skip_back() : audiobook_skip_back();
		audiobook_seek_by(-(double)back, &state);
		update_progress();
		return;
	}

	// Past the first three seconds, prev restarts the track instead of leaving
	// it: the same rule every player follows, so a double press is what goes
	// back.
	if (state.progress_current_secs > 3.0) {
		device_state_seek(0); // far enough in -> restart current track
	} else if (device_state_prev(NULL, 0)) {
		refresh_now_playing(); // near the start -> go to previous track
	} else {
		device_state_seek(0); // no queue -> just restart
	}

	update_progress();
}

void player_screen_next(void) {
	if (audiobook_mode || podcast_mode) {
		device_state_t state;
		device_state_get(&state);
		int fwd = podcast_mode && !audiobook_mode ? podcast_skip_forward() : audiobook_skip_forward();
		audiobook_seek_by((double)fwd, &state);
		update_progress();
		return;
	}

	if (device_state_next(NULL, 0)) {
		refresh_now_playing();
	}
	update_progress();
}

static void prev_btn_event_cb(lv_event_t *e) {
	(void)e;
	player_screen_prev();
}

static void next_btn_event_cb(lv_event_t *e) {
	(void)e;
	player_screen_next();
}

// --- hardware transport keys ---
//
// They do exactly what the on-screen buttons do, whichever page is showing:
// the player does not have to be open for the keys on the side of the device
// to work.

void player_key_play_pause(void) {
	apply_playback_status(device_state_toggle_play_pause());
}

void player_key_next(void) {
	// On a track coming from DLNA, "next" is a question for the phone: the
	// queue is its own.
	if (dlna_owns_playback()) {
		dlna_request_next();
		return;
	}

	// The side keys stay previous/next track on a podcast, deliberately. A
	// book is a single file, so "next track" means nothing there and the keys
	// become jumps. A podcast sits in a real queue of episodes: if the side
	// keys also jumped thirty seconds, there would be no way to reach the next
	// episode without opening the queue. This way both exist -- jumps on
	// screen, episode change under the finger.
	if (audiobook_mode) {
		device_state_t state;
		device_state_get(&state);
		audiobook_seek_by((double)audiobook_skip_forward(), &state);
		update_progress();
		return;
	}

	if (device_state_next(NULL, 0)) {
		refresh_now_playing();
	}
	update_progress();
}

void player_key_prev(void) {
	device_state_t state;
	device_state_get(&state);

	if (dlna_owns_playback()) {
		// As elsewhere: within the first few seconds go back to the start,
		// later ask for the previous track -- only here the phone is asked.
		if (state.progress_current_secs > 3.0) {
			device_state_seek(0);
		} else {
			dlna_request_prev();
		}
		return;
	}

	if (audiobook_mode) {
		audiobook_seek_by(-(double)audiobook_skip_back(), &state);
		update_progress();
		return;
	}

	if (state.progress_current_secs > 3.0) {
		device_state_seek(0);
	} else if (device_state_prev(NULL, 0)) {
		refresh_now_playing();
	} else {
		device_state_seek(0);
	}
	update_progress();
}

// The overflow button: the chapter list on a book, the episode queue on a
// podcast, the track menu on anything else.
static void more_btn_event_cb(lv_event_t *e) {
	if (audiobook_mode) {
		if (audiobook_has_chapters) {
			chapters_open();
		} else {
			gui_notify_popup("player_this_audiobook_has_no_chapters_2");
		}
		return;
	}
	// A podcast has no chapters: it has episodes, and the episodes are already
	// the queue. The button goes straight there.
	if (podcast_mode) {
		trackmenu_open_queue();
		return;
	}
	trackmenu_open(lv_event_get_current_target(e));
}

// Steps to the next playback mode: normal, loop the queue, loop the track,
// shuffle once through, shuffle round and round.
static void repeat_btn_event_cb(lv_event_t *e) {
	(void)e;
	playlist_cycle_mode();
	update_repeat_button();
}

static void progress_slider_event_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);

	if (code == LV_EVENT_PRESSED) {
		lv_timer_pause(progress_slider_timer); // the poll must not fight the finger
		if (smooth_timer) {
			lv_timer_pause(smooth_timer); // nor the smoothing, which would drag it back
		}
	}

	if (code == LV_EVENT_RELEASED) {
		lv_timer_resume(progress_slider_timer);

		int value = lv_slider_get_value(progress_slider);

		double seconds = current_total_length * value / 1000.0;

		device_state_seek(seconds);

		lv_timer_resume(progress_slider_timer);
	}

	if (code == LV_EVENT_VALUE_CHANGED) {
		// While dragging, the left clock previews where the knob would land.
		int value = lv_slider_get_value(progress_slider);

		double seconds = current_total_length * value / 1000.0;

		formatDoubleSeconds(seconds, progress_label_text, sizeof(progress_label_text));
		lv_label_set_text(elapsed_label, progress_label_text);

		// And so does the waveform, when that is what is being dragged. The
		// poll is paused for the duration of the press, so without this the
		// bars would stand still under the finger and only jump at the release.
		wave_preview(seconds);
	}
}

// The poll: collects finished artwork and reconciles the UI with playback.
// Carries the bar and the shape of the track forward between polls.
//
// Draws nothing unless the picture would change: the slider is told a value
// only when its own scale says a different one, and the waveform only when the
// playhead has crossed into another pixel. Both are compared against what was
// last drawn rather than against what was last computed, so a tick that changes
// nothing costs nothing.
static void smooth_timer_cb(lv_timer_t *timer) {
	(void)timer;
	if (!progress_running || current_total_length <= 0.1) {
		return;
	}
	// Nothing is drawn for a screen nobody is looking at, or for a page that is
	// not the one on show. Drawing a dark panel is work this device cannot
	// spare and has paid for before.
	if (!sheet_open || !power_screen_is_on()) {
		return;
	}

	double seconds = progress_anchor_secs + (double)lv_tick_elaps(progress_anchor_tick) / 1000.0;
	if (seconds > current_total_length) {
		seconds = current_total_length; // the poll will say what happens next
	}

	int value = (int)(seconds / current_total_length * 1000);
	if (value != lv_slider_get_value(progress_slider)) {
		lv_slider_set_value(progress_slider, value, LV_ANIM_OFF);
	}
	wave_preview(seconds);
}

static void progress_slider_timer_cb(lv_timer_t *timer) {
	(void)timer;
	apply_cover_id_result(); // "is the new track's picture the one already up?"
	apply_cover_result();	 // artwork the worker has finished since last tick
	update_progress();
}

// The boot-time restore: the remembered track, paused at its position.
void player_restore_track(const char *filepath, double position) {
	device_state_restore_file(filepath, position);
	player_refresh_now_playing();
}

// The same, with the queue that was saved alongside it.
void player_restore_index(library_index_t *ix, int start_index, const char *const *extra, const int *extra_slots,
						  int extra_count, const char *filepath, double position) {
	device_state_restore_index(ix, start_index, extra, extra_slots, extra_count, filepath, position);
	player_refresh_now_playing();
}

void player_restore_list(const char *const *list, int count, int start_index, bool custom, const char *filepath,
						 double position) {
	device_state_restore_list(list, count, start_index, custom, filepath, position);
	player_refresh_now_playing();
}

// Loads and starts a new file, updating the now-playing info. Also rebuilds
// the folder queue inside device_state, so playback carries on to the
// following tracks when this one finishes.
void player_play_file(const char *filepath) {
	device_state_play_file(filepath); // builds folder queue, loads metadata, starts playback

	player_refresh_now_playing();
}

// audio_pause() rather than device_state_toggle_play_pause(): this is a pause,
// not a toggle, and the toggle would start a stopped radio station playing
// again. The button glyph is corrected here rather than left to the next poll
// because half a second of a pause button over a paused track reads as a
// control that did not take.
player_layout_t player_layout_get(void) { return layout_choice; }

void player_layout_set(player_layout_t layout) {
	if (layout == layout_choice) {
		return;
	}
	layout_choice = layout;
	config_set_int("screen", "player_layout_alt", (int)layout);
	config_save();

	// Not apply_layout() directly: choosing Alternative while a radio station
	// is on must not put the alternative arrangement up. What the setting does
	// is change the answer update_layout() will give, so it is asked again.
	device_state_t state;
	device_state_get(&state);
	update_layout(&state);
}

void player_output_unplugged(const char *what) {
	if (audio_get_status() != AUDIO_STATUS_PLAYING) {
		return;
	}
	printf("player: %s unplugged; pausing\n", what ? what : "the output");
	audio_pause();
	apply_playback_status(AUDIO_STATUS_PAUSED);
}

// A pause and not a stop, and for the same reason as the one above: the point
// of falling asleep to something is finding it where it was in the morning.
static void sleep_timer_cb(lv_timer_t *timer) {
	(void)timer;
	bool expired = false;
	for (int i = 0; i < SLEEPTIMER_COUNT; i++) {
		expired = sleeptimer_expired((sleeptimer_kind_t)i) || expired;
	}
	if (!expired || audio_get_status() != AUDIO_STATUS_PLAYING) {
		return;
	}

	// A book being put down has two things to settle that a track does not: the
	// rewind that the next play would otherwise add -- it is for a sentence
	// somebody interrupted, not for one the player stopped on purpose -- and
	// the position, written through rather than left to the ten second throttle.
	if (audiobook_is_playing()) {
		device_state_t state;
		device_state_get(&state);
		audiobook_suppress_rewind_once();
		audio_pause();
		apply_playback_status(AUDIO_STATUS_PAUSED);
		audiobook_note_position(state.progress_current_secs, 0, true);
		return;
	}

	audio_pause();
	apply_playback_status(AUDIO_STATUS_PAUSED);
}

// Public face of refresh_now_playing, for callers that started playback
// through device_state themselves (the library lists hand over a whole
// queue rather than a single file).
void player_refresh_now_playing(void) {
	refresh_now_playing();

	// Playback may have been started by something that also set the mode --
	// the favourites shuffle button forces shuffle on its way in -- so the
	// repeat/shuffle glyph is refreshed here too, not only when it is tapped.
	update_repeat_button();

	device_state_t state;
	device_state_get(&state);
	apply_playback_status(state.status);
}


// ---------------------------------------------------------------------------
// the sheet: dragging the player in and out
// ---------------------------------------------------------------------------

// How far across the screen the drag has to get before letting go opens (or
// closes) it rather than snapping back. A third is enough to feel deliberate
// without demanding the whole width.
#define SHEET_COMMIT_FRACTION 3

#define SHEET_ANIM_MS 220

// Sideways movement past this is a drag, not a tap.
#define DRAG_COMMIT_PX 10

static int sheet_width;
static lv_obj_t *sheet_under; // the page the sheet first slid over

// Where the sheet was when the finger went down, and where the finger was.
static int drag_origin_x;
static lv_point_t drag_start_point;
static bool dragging;

// True once a gesture has moved far enough sideways to count as a drag. Read
// by the tap handlers on the pages behind, so that swiping across a menu tile
// does not also select it.
static bool drag_committed;

static void sheet_anim_x_cb(void *obj, int32_t value) {
	lv_obj_set_x((lv_obj_t *)obj, (int)value);
	// The chevron belongs to the sheet while it moves: ride its edge, and
	// snap back into the page's corner once the sheet is fully away.
	back_btn_translate(value >= sheet_width ? 0 : value);
}

// Moves the sheet to `x` (0 = fully open, sheet_width = fully out of the way).
static void sheet_move_to(int x, bool animate) {
	if (x < 0) {
		x = 0;
	}
	if (x > sheet_width) {
		x = sheet_width;
	}

	// Never race a slide that is still running. A slide left to finish after a
	// new one has started parks the sheet off screen while everything else
	// believes it is open -- the page underneath showing, no status bar, and no
	// way back to the player.
	lv_anim_delete(player_screen, sheet_anim_x_cb);

	if (!animate) {
		lv_obj_set_x(player_screen, x);
		back_btn_translate(x >= sheet_width ? 0 : x);
		return;
	}

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, player_screen);
	lv_anim_set_exec_cb(&a, sheet_anim_x_cb);
	lv_anim_set_values(&a, lv_obj_get_x(player_screen), x);
	lv_anim_set_duration(&a, SHEET_ANIM_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
}

bool player_sheet_is_open(void) { return sheet_open; }

bool player_sheet_drag_active(void) { return drag_committed; }

void player_sheet_open(bool animate) {
	// Nothing to show while another mode owns the sound: the sheet is about a
	// track this player is playing, and in Bluetooth receiver mode the track is
	// on somebody else's machine. Refusing to open is the honest answer -- an
	// opened sheet would show the last thing played here, paused, over music
	// that is coming out of the jack right now.
	if (btreceiver_is_active()) {
		return;
	}

	if (!sheet_open) {
		// Remember the opener: leaving the player must always land back here,
		// whatever the pages under the sheet did in the meantime.
		sheet_under = lv_screen_active();
	}
	sheet_open = true;
	back_btn_player_mode(true);

	// Explicitly the bottom-most thing on the top layer: it covers the page it
	// slides over, while the status bar and the floating back button stay drawn
	// above it, exactly where the page underneath put them. Nothing shared is
	// moved or hidden, so nothing shared slides around during the drag.
	lv_obj_remove_flag(player_screen, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_background(player_screen);

	// Artwork runs to the top edge here, so the bar steps aside. It is hidden
	// rather than moved, which is why nothing slides during the drag.
	topbar_set_hidden(true);

	sheet_move_to(0, animate);
}

static void sheet_hidden_anim_ready_cb(lv_anim_t *a) {
	(void)a;
	if (!sheet_open) {
		lv_obj_add_flag(player_screen, LV_OBJ_FLAG_HIDDEN);
	}
	// Whatever page the close landed on, the chevron's visibility is settled
	// once more now that the last animation frame has run -- the fix for the
	// chevron flashing on the main menu for a frame on the way back to it.
	back_btn_sync_visibility();
}

void player_sheet_close(bool animate) {
	bool was_open = sheet_open;
	sheet_open = false;
	back_btn_player_mode(false);
	topbar_set_hidden(false);
	// The tab does not travel with the page: coming back to a player with a
	// panel hanging open is not what leaving it looked like.

	// Back where the player was opened from, always: if anything switched the
	// page underneath while the sheet was up, put the opener back.
	if (was_open && sheet_under && lv_screen_active() != sheet_under) {
		switch_screen_no_history(sheet_under);
	}

	if (!animate) {
		lv_obj_set_x(player_screen, sheet_width);
		back_btn_translate(0);
		lv_obj_add_flag(player_screen, LV_OBJ_FLAG_HIDDEN);
		back_btn_sync_visibility();
		return;
	}

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, player_screen);
	lv_anim_set_exec_cb(&a, sheet_anim_x_cb);
	lv_anim_set_values(&a, lv_obj_get_x(player_screen), sheet_width);
	lv_anim_set_duration(&a, SHEET_ANIM_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, sheet_hidden_anim_ready_cb);
	lv_anim_start(&a);
}

// Runs from lv_async_call, one tick after the gesture's own events (RELEASED,
// then CLICKED) have all been delivered: the suppression the flag exists for
// has happened by then, and nothing later may still be blocked by it.
static void drag_committed_clear_cb(void *unused) {
	(void)unused;
	drag_committed = false;
}

// One handler for both directions. `opening` is passed as the user data: true
// when the drag starts on something behind the sheet, false when it starts on
// the sheet itself.
static void sheet_drag_cb(lv_event_t *e) {
	bool opening = (bool)(uintptr_t)lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}

	if (code == LV_EVENT_PRESSED) {
		if (opening == sheet_open) {
			return; // dragging the wrong way for the current state
		}

		lv_indev_get_point(indev, &drag_start_point);
		drag_origin_x = opening ? sheet_width : 0;
		dragging = true;

		drag_committed = false;

		if (opening) {
			// It has to be on screen to be dragged, even if only by a pixel.
			lv_obj_remove_flag(player_screen, LV_OBJ_FLAG_HIDDEN);
			lv_obj_move_background(player_screen);
			lv_obj_set_x(player_screen, sheet_width);
		}
		return;
	}

	if (!dragging) {
		return;
	}

	lv_point_t point;
	lv_indev_get_point(indev, &point);
	int dx = point.x - drag_start_point.x;

	if (code == LV_EVENT_PRESSING) {
		// Vertical scrolling must still work: the moment LVGL is scrolling
		// something this press is a scroll, and once the movement reads as
		// vertical it stays a scroll -- no more sideways shimmy of the page
		// during a fast flick.
		if (!drag_committed) {
			if (lv_indev_get_scroll_obj(indev)) {
				dragging = false;
				return;
			}
			int dy = point.y - drag_start_point.y;
			if (LV_ABS(dy) >= DRAG_COMMIT_PX && LV_ABS(dy) > LV_ABS(dx)) {
				dragging = false;
				return;
			}
			if (LV_ABS(dx) < DRAG_COMMIT_PX || LV_ABS(dx) < 2 * LV_ABS(dy)) {
				return;
			}
			drag_committed = true;
		}

		sheet_move_to(drag_origin_x + dx, false);
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		dragging = false;

		int travelled = opening ? -dx : dx;
		bool commit = travelled > (sheet_width / SHEET_COMMIT_FRACTION);

		if (opening) {
			commit ? player_sheet_open(true) : player_sheet_close(true);
		} else {
			commit ? player_sheet_close(true) : player_sheet_open(true);
		}

		// The flag must not outlive the gesture. Waiting for the next PRESSED on
		// an attached object is not enough -- the browser list is not one, so
		// every tap on a row after a swipe would be discarded as part of a drag.
		// Deferred rather than cleared here, because the CLICKED event of this
		// same gesture still has to see it.
		lv_async_call(drag_committed_clear_cb, NULL);
	}
}

// The control centre, from the player too.
//
// Every other page pulls it down from the status bar, and this page hides the
// status bar, so there is nothing to pull. This strip stands in for it:
// the same gesture, in the same place, on a piece of the artwork that has
// nothing else to do. The bar comes back for as long as the sheet is up (see
// quickpanel.c), so what the finger lands on afterwards is what it expected.
//
// Vertical dominance is required before it engages: a sideways drag across the
// top of the artwork is somebody putting the player away, not somebody
// reaching for the brightness.
static void panel_edge_drag_cb(lv_event_t *e) {
	static lv_point_t start;
	static bool tracking;
	static bool engaged;

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}

	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_PRESSED) {
		lv_indev_get_point(indev, &start);
		tracking = !quickpanel_is_open();
		engaged = false;
		return;
	}
	if (!tracking) {
		return;
	}

	lv_point_t p;
	lv_indev_get_point(indev, &p);
	int dy = p.y - start.y;
	int dx = p.x - start.x;

	if (code == LV_EVENT_PRESSING) {
		if (!engaged) {
			int adx = dx < 0 ? -dx : dx;
			if (dy < 8 || dy <= adx) {
				return; // still just a press, or a sideways one
			}
			engaged = true;
			quickpanel_drag_begin();
		}
		quickpanel_drag_update(dy);
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		tracking = false;
		if (engaged) {
			engaged = false;
			quickpanel_drag_end();
		}
	}
}

void player_sheet_attach_drag(lv_obj_t *obj, bool opening) {
	if (!obj) {
		return;
	}

	lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(obj, sheet_drag_cb, LV_EVENT_PRESSED, (void *)(uintptr_t)opening);
	lv_obj_add_event_cb(obj, sheet_drag_cb, LV_EVENT_PRESSING, (void *)(uintptr_t)opening);
	lv_obj_add_event_cb(obj, sheet_drag_cb, LV_EVENT_RELEASED, (void *)(uintptr_t)opening);
	lv_obj_add_event_cb(obj, sheet_drag_cb, LV_EVENT_PRESS_LOST, (void *)(uintptr_t)opening);
}

// Builds the player sheet: artwork, controls and the poll that drives them.
void player_init(gui_config_t *cfg) {
	// The artwork decodes on the shared worker; make sure it exists before
	// the first track can be started. Safe to call more than once.
	coverloader_start();

	lv_obj_add_style(player_screen, &theme_style_screen, 0);

	// The player is a panel parked just off the right edge (see player.h).
	sheet_width = (int)cfg->screen_width;
	lv_obj_set_size(player_screen, cfg->screen_width, cfg->screen_height);
	lv_obj_set_style_pad_all(player_screen, 0, 0);
	lv_obj_set_style_border_width(player_screen, 0, 0);
	lv_obj_set_style_radius(player_screen, 0, 0);
	lv_obj_remove_flag(player_screen, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_pos(player_screen, sheet_width, 0);
	lv_obj_add_flag(player_screen, LV_OBJ_FLAG_HIDDEN);


	// Geometry first, because everything else hangs off it: the artwork is a
	// square as wide as the screen, sitting flush at the top (this page hides
	// the status bar), and the controls get whatever height is left. On a panel
	// too short for that, the artwork gives up height rather than the controls.
	int cover_size = (int)cfg->screen_width;
	int menu_height = (int)cfg->screen_height - cover_size;
	if (menu_height < PLAYER_MENU_MIN_HEIGHT) {
		menu_height = PLAYER_MENU_MIN_HEIGHT;
		cover_size = (int)cfg->screen_height - menu_height;
		if (cover_size < 64)
			cover_size = 64;
	}

	cover_box_w = cover_size;
	cover_box_h = cover_size;
	backdrop_w = (int)cfg->screen_width;
	backdrop_h = menu_height;
	backdrop_studio_h = (int)cfg->screen_height;

	// The controls block. Its background is the current track's artwork,
	// flipped and blurred (set per track in refresh_cover); the panel colour is
	// what shows through when there is no artwork.
	player_menu = lv_obj_create(player_screen);
	lv_obj_set_size(player_menu, cfg->screen_width, menu_height);
	lv_obj_align(player_menu, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_add_style(player_menu, &theme_style_panel, 0);
	lv_obj_set_flex_flow(player_menu, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(player_menu, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_border_width(player_menu, 0, 0);
	lv_obj_set_style_radius(player_menu, 0, 0);
	lv_obj_set_style_pad_hor(player_menu, cfg->padding, 0);
	lv_obj_set_style_pad_ver(player_menu, 10, 0);
	lv_obj_set_style_pad_gap(player_menu, 12, 0);
	lv_obj_remove_flag(player_menu, LV_OBJ_FLAG_SCROLLABLE);

	// Track info: the text on the left, the star and the format on the right.
	lv_obj_t *song_info = lv_obj_create(player_menu);
	lv_obj_set_size(song_info, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(song_info, 0, 0);
	lv_obj_set_style_border_width(song_info, 0, 0);
	lv_obj_set_style_radius(song_info, 0, 0);
	lv_obj_set_style_pad_all(song_info, 0, 0);
	lv_obj_set_style_pad_column(song_info, 10, 0);
	lv_obj_set_flex_flow(song_info, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(song_info, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_remove_flag(song_info, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *song_text = lv_obj_create(song_info);
	song_text_obj = song_text;
	lv_obj_set_height(song_text, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(song_text, 1);
	lv_obj_set_style_bg_opa(song_text, 0, 0);
	lv_obj_set_style_border_width(song_text, 0, 0);
	lv_obj_set_style_pad_all(song_text, 0, 0);
	lv_obj_set_flex_flow(song_text, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(song_text, LV_OBJ_FLAG_SCROLLABLE);

	// Long titles scroll rather than end in an ellipsis: on a 480 px panel a
	// good half of real album titles do not fit, and the tail is usually the
	// part that tells them apart.
	song_title_label = lv_label_create(song_text);
	lv_label_set_text(song_title_label, tr("player_no_track_loaded"));
	lv_obj_set_width(song_title_label, lv_pct(100));
	lv_obj_add_style(song_title_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(song_title_label, &font_ui_26, 0);
	scrolltext_apply(song_title_label);

	// Only the album's artist goes here -- see refresh_now_playing().
	song_artist_label = lv_label_create(song_text);
	lv_label_set_text(song_artist_label, "");
	lv_obj_set_width(song_artist_label, lv_pct(100));
	lv_obj_add_style(song_artist_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(song_artist_label, &font_ui_24, 0);
	scrolltext_apply(song_artist_label);

	// Right side: the favourite star over the stream format.
	lv_obj_t *song_side = lv_obj_create(song_info);
	song_side_obj = song_side;
	lv_obj_set_size(song_side, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(song_side, 0, 0);
	lv_obj_set_style_border_width(song_side, 0, 0);
	lv_obj_set_style_pad_all(song_side, 0, 0);
	lv_obj_set_style_pad_gap(song_side, 2, 0);
	lv_obj_set_flex_flow(song_side, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(song_side, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
	lv_obj_remove_flag(song_side, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *fav_btn = lv_btn_create(song_side);
	fav_btn_obj = fav_btn;
	lv_obj_set_size(fav_btn, 48, 44);
	lv_obj_set_style_bg_opa(fav_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(fav_btn, 0, 0);
	lv_obj_set_style_shadow_width(fav_btn, 0, 0);
	lv_obj_set_style_pad_all(fav_btn, 0, 0);
	lv_obj_add_event_cb(fav_btn, fav_btn_event_cb, LV_EVENT_CLICKED, NULL);

	fav_btn_icon = lv_image_create(fav_btn);
	lv_obj_center(fav_btn_icon);
	update_fav_button();

	format_label = lv_label_create(song_side);
	lv_label_set_text(format_label, "");
	lv_obj_add_style(format_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(format_label, &font_ui_18, 0);

	// The shape of the track, and the slider laid over it that still does the
	// seeking. Two things in the same place rather than one: drawing a
	// waveform is one job and reading a drag is another, and the slider
	// already knows how to do the second.
	wave_box = lv_obj_create(player_menu);
	lv_obj_remove_style_all(wave_box);
	lv_obj_set_size(wave_box, lv_pct(100), WAVE_HEIGHT);
	lv_obj_remove_flag(wave_box, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(wave_box, LV_OBJ_FLAG_HIDDEN);

	{
		wave_w = (int)cfg->screen_width - 2 * cfg->padding;
		if (wave_w < WAVEFORM_BARS) {
			wave_w = WAVEFORM_BARS;
		}
		wave_buf = malloc((size_t)wave_w * WAVE_HEIGHT * 3);
		if (wave_buf) {
			memset(wave_buf, 0, (size_t)wave_w * WAVE_HEIGHT * 3);
			wave_canvas = lv_canvas_create(wave_box);
			lv_canvas_set_buffer(wave_canvas, wave_buf, wave_w, WAVE_HEIGHT, LV_COLOR_FORMAT_RGB565A8);
			lv_obj_set_size(wave_canvas, wave_w, WAVE_HEIGHT);
			lv_obj_align(wave_canvas, LV_ALIGN_CENTER, 0, 0);
			lv_obj_remove_flag(wave_canvas, LV_OBJ_FLAG_CLICKABLE);
		} else {
			fprintf(stderr, "player: no room for the waveform; the alternative layout keeps the bar\n");
		}
	}

	// Playback progress, drawn as described at PROGRESS_TRACK_HEIGHT.
	progress_slider = lv_slider_create(player_menu);
	lv_obj_set_width(progress_slider, lv_pct(100));
	lv_obj_set_height(progress_slider, PROGRESS_TRACK_HEIGHT);
	lv_slider_set_range(progress_slider, 0, 1000);

	lv_obj_set_style_radius(progress_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_bg_color(progress_slider, lv_color_make(255, 255, 255), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(progress_slider, LV_OPA_30, LV_PART_MAIN);

	lv_obj_set_style_radius(progress_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(progress_slider, theme()->accent, LV_PART_INDICATOR);

	lv_obj_set_style_radius(progress_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
	lv_obj_set_style_bg_color(progress_slider, theme()->accent, LV_PART_KNOB);
	lv_obj_set_style_border_color(progress_slider, lv_color_white(), LV_PART_KNOB);
	lv_obj_set_style_border_opa(progress_slider, LV_OPA_COVER, LV_PART_KNOB);
	lv_obj_set_style_border_width(progress_slider, PROGRESS_KNOB_RING, LV_PART_KNOB);
	lv_obj_set_style_pad_all(progress_slider, PROGRESS_KNOB_GROW, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(progress_slider, 8, LV_PART_KNOB);
	lv_obj_set_style_shadow_opa(progress_slider, LV_OPA_40, LV_PART_KNOB);
	lv_obj_set_style_shadow_color(progress_slider, lv_color_black(), LV_PART_KNOB);
	lv_obj_set_style_shadow_offset_y(progress_slider, 1, LV_PART_KNOB);

	lv_obj_add_event_cb(progress_slider, progress_slider_event_cb, LV_EVENT_ALL, NULL);

	// Polls playback to keep the slider, the clock and the play/pause icon in
	// sync. It runs from here on, slowing down rather than stopping when
	// nothing is playing (see apply_playback_status).
	smooth_timer = lv_timer_create(smooth_timer_cb, SMOOTH_PERIOD_MS, NULL);
	lv_timer_pause(smooth_timer); // the poll starts it when something is playing

	progress_slider_timer = lv_timer_create(progress_slider_timer_cb, POLL_PERIOD_IDLE_MS, NULL);
	// Slowed in standby, never stopped: this timer does more than paint --
	// update_progress() calls device_state_take_completion(), the engine that
	// advances the queue when a track ends on its own. Stopping it when the
	// screen goes off (on the grounds that nobody can see the slider) meant no
	// next track ever started with the screen dark, local files included,
	// until the screen came back on and consumed the completion left waiting.
	// The period is still apply_playback_status's to decide (fast while
	// playing, slow when stopped), in the dark too.
	power_slow_in_standby(progress_slider_timer, POLL_PERIOD_PLAYING_MS);

	// The sleep timer's own tick. Slowed in standby and not stopped, because
	// the screen being dark is the normal case for it -- and slowing it costs
	// nothing in accuracy: the countdown is against the clock, so a late tick
	// stops the music late by however late it was, not by a tick.
	sleep_timer = lv_timer_create(sleep_timer_cb, 1000, NULL);
	lv_timer_pause(sleep_timer); // apply_playback_status starts it when there is a stretch to count
	power_slow_in_standby(sleep_timer, 5000);

	lv_obj_t *below_slider_group = lv_obj_create(player_menu);
	below_slider_obj = below_slider_group;
	lv_obj_set_size(below_slider_group, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(below_slider_group, 0, 0);
	lv_obj_set_style_border_width(below_slider_group, 0, 0);
	lv_obj_set_style_radius(below_slider_group, 0, 0);
	lv_obj_set_style_pad_all(below_slider_group, 0, 0);
	lv_obj_remove_flag(below_slider_group, LV_OBJ_FLAG_SCROLLABLE);

	// Elapsed and total time, tucked under the two ends of the bar.
	elapsed_label = lv_label_create(below_slider_group);
	lv_label_set_text(elapsed_label, "0:00");
	lv_obj_add_style(elapsed_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(elapsed_label, &font_ui_22, 0);
	lv_obj_set_align(elapsed_label, LV_ALIGN_LEFT_MID);

	remaining_label = lv_label_create(below_slider_group);
	lv_label_set_text(remaining_label, "0:00");
	lv_obj_add_style(remaining_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(remaining_label, &font_ui_22, 0);
	lv_obj_set_align(remaining_label, LV_ALIGN_RIGHT_MID);

	// Between them, in the middle the two clocks left empty. Dimmer than they
	// are: it is a bearing, not a reading.
	queue_position_label = lv_label_create(below_slider_group);
	lv_label_set_text(queue_position_label, "");
	lv_obj_add_style(queue_position_label, &theme_style_text_dim, 0);
	// A step below the clocks either side of it, as well as dimmer: it is a
	// bearing and they are the reading, and at the same size the three read as
	// one row of equals.
	lv_obj_set_style_text_font(queue_position_label, &font_ui_18, 0);
	lv_obj_set_align(queue_position_label, LV_ALIGN_CENTER);

	// Transport: prev, play/pause, next -- with the repeat toggle parked on
	// the left, out of the way of the transport controls.
	lv_obj_t *player_controls_buttons = lv_obj_create(player_menu);
	controls_row = player_controls_buttons;
	lv_obj_set_size(player_controls_buttons, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(player_controls_buttons, 0, 0);
	lv_obj_set_style_border_width(player_controls_buttons, 0, 0);
	lv_obj_set_style_radius(player_controls_buttons, 0, 0);
	lv_obj_set_style_pad_all(player_controls_buttons, 0, 0);
	lv_obj_set_flex_flow(player_controls_buttons, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(player_controls_buttons, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	// Lifted a touch off the screen edge.
	lv_obj_set_style_translate_y(player_controls_buttons, -8, 0);

	// The repeat/shuffle button, kept out of the flex row so the transport
	// stays centred on the screen.
	lv_obj_t *repeat_btn = lv_btn_create(player_controls_buttons);
	repeat_btn_obj = repeat_btn;
	lv_obj_add_flag(repeat_btn, LV_OBJ_FLAG_IGNORE_LAYOUT);
	lv_obj_set_size(repeat_btn, 56, 56);
	lv_obj_set_style_bg_opa(repeat_btn, 0, 0);
	lv_obj_set_style_shadow_width(repeat_btn, 0, 0);
	lv_obj_add_event_cb(repeat_btn, repeat_btn_event_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_align(repeat_btn, LV_ALIGN_LEFT_MID, 0, 0);

	repeat_btn_icon = lv_image_create(repeat_btn);
	lv_obj_center(repeat_btn_icon);
	update_repeat_button(); // the icon for the mode already in force

	// The playback-speed gauge, in exactly the same corner: on a book the
	// repeat mode is meaningless and the speed is the thing worth reaching,
	// so one hides and the other appears in its place.
	lv_obj_t *speed_btn = lv_btn_create(player_controls_buttons);
	speed_btn_obj = speed_btn;
	lv_obj_add_flag(speed_btn, LV_OBJ_FLAG_IGNORE_LAYOUT);
	lv_obj_set_size(speed_btn, 56, 56);
	lv_obj_set_style_bg_opa(speed_btn, 0, 0);
	lv_obj_set_style_shadow_width(speed_btn, 0, 0);
	lv_obj_add_event_cb(speed_btn, speed_btn_event_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_align(speed_btn, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_add_flag(speed_btn, LV_OBJ_FLAG_HIDDEN);

	speed_btn_icon = lv_image_create(speed_btn);
	lv_image_set_src(speed_btn_icon, &icon_play_speed);
	lv_obj_add_style(speed_btn_icon, &theme_style_icon, 0);
	lv_obj_center(speed_btn_icon);
	update_speed_button();

	// The overflow menu, mirroring the repeat button on the right: queue and
	// details in an iOS-style popover next to the button.
	lv_obj_t *more_btn = lv_btn_create(player_controls_buttons);
	more_btn_obj = more_btn;
	lv_obj_add_flag(more_btn, LV_OBJ_FLAG_IGNORE_LAYOUT);
	lv_obj_set_size(more_btn, 56, 56);
	lv_obj_set_style_bg_opa(more_btn, 0, 0);
	lv_obj_set_style_shadow_width(more_btn, 0, 0);
	lv_obj_add_event_cb(more_btn, more_btn_event_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_align(more_btn, LV_ALIGN_RIGHT_MID, 0, 0);

	lv_obj_t *more_icon = lv_image_create(more_btn);
	more_btn_icon = more_icon;
	lv_image_set_src(more_icon, &icon_ellipsis_vertical);
	lv_obj_add_style(more_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor(more_icon, lv_color_make(150, 150, 150), 0);
	lv_obj_set_style_image_recolor_opa(more_icon, LV_OPA_COVER, 0);
	lv_obj_center(more_icon);

	// No button chrome on prev/next: they sit on top of the blurred artwork,
	// and plain glyphs read better there than filled boxes.
	lv_obj_t *prev_btn = lv_btn_create(player_controls_buttons);
	prev_btn_obj = prev_btn;
	lv_obj_set_size(prev_btn, 76, 76);
	lv_obj_set_style_bg_opa(prev_btn, 0, 0);
	lv_obj_set_style_shadow_width(prev_btn, 0, 0);
	prev_icon = lv_image_create(prev_btn);
	lv_image_set_src(prev_icon, &icon_skip_back);
	lv_obj_add_style(prev_icon, &theme_style_icon, 0);
	lv_obj_center(prev_icon);

	lv_obj_add_event_cb(prev_btn, prev_btn_event_cb, LV_EVENT_CLICKED, NULL);

	// Play/pause: a white disc, with the glyph carrying the colour.
	play_btn = lv_btn_create(player_controls_buttons);
	lv_obj_set_size(play_btn, 84, 84);
	lv_obj_set_style_radius(play_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(play_btn, lv_color_white(), 0); // repainted by set_over_cover
	lv_obj_set_style_bg_opa(play_btn, LV_OPA_COVER, 0);
	lv_obj_set_style_shadow_width(play_btn, 0, 0);
	lv_obj_add_event_cb(play_btn, play_btn_event_cb, LV_EVENT_CLICKED, NULL);

	play_btn_icon = lv_image_create(play_btn);
	lv_obj_center(play_btn_icon);
	apply_playback_status(AUDIO_STATUS_STOPPED); // start on the play glyph

	lv_obj_t *next_btn = lv_btn_create(player_controls_buttons);
	next_btn_obj = next_btn;
	lv_obj_set_size(next_btn, 76, 76);
	lv_obj_set_style_bg_opa(next_btn, 0, 0);
	lv_obj_set_style_shadow_width(next_btn, 0, 0);
	next_icon = lv_image_create(next_btn);
	lv_image_set_src(next_icon, &icon_skip_forward);
	lv_obj_add_style(next_icon, &theme_style_icon, 0);
	lv_obj_center(next_icon);

	lv_obj_add_event_cb(next_btn, next_btn_event_cb, LV_EVENT_CLICKED, NULL);

	// Album art: a square as wide as the screen, flush against the top edge
	// (this page hides the status bar, see switch_screen). The picture is
	// centre-cropped to fill it rather than letterboxed. Sizes were worked out
	// at the top of this function, before the controls were laid out.
	cover_panel = lv_obj_create(player_screen);
	// Dragging the artwork to the right pushes the player back off screen.
	player_sheet_attach_drag(cover_panel, false);
	lv_obj_set_size(cover_panel, cover_size, cover_size);
	lv_obj_align(cover_panel, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_set_style_bg_color(cover_panel, theme()->cover_bg, 0);
	lv_obj_set_style_border_width(cover_panel, 0, 0);
	lv_obj_set_style_radius(cover_panel, 0, 0);
	lv_obj_set_style_pad_all(cover_panel, 0, 0);
	lv_obj_remove_flag(cover_panel, LV_OBJ_FLAG_SCROLLABLE);

	// The live badge, top right over the artwork: the one thing that has to be
	// legible at a glance is that this is not a file being played but a
	// broadcast going past. Red because that is what a live indicator is, and
	// deliberately not the accent colour -- it says something about the
	// stream, not about the theme.
	live_badge = lv_label_create(cover_panel);
	lv_label_set_text(live_badge, tr("player_live"));
	lv_obj_set_style_text_font(live_badge, &font_ui_18, 0);
	lv_obj_set_style_text_color(live_badge, lv_color_white(), 0);
	lv_obj_set_style_bg_color(live_badge, lv_color_make(224, 27, 36), 0); // Adwaita red
	lv_obj_set_style_bg_opa(live_badge, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(live_badge, 6, 0);
	lv_obj_set_style_pad_hor(live_badge, 10, 0);
	lv_obj_set_style_pad_ver(live_badge, 5, 0);
	lv_obj_align(live_badge, LV_ALIGN_TOP_RIGHT, -BADGE_INSET, BADGE_INSET);
	lv_obj_add_flag(live_badge, LV_OBJ_FLAG_HIDDEN);

	// The Qobuz mark, in the same corner and for the same reason: say where
	// what is playing comes from. It is the white-outlined image, legible even
	// over a black cover, and is not recoloured (theme_style_icon would turn
	// it into a solid square).
	qobuz_badge = lv_image_create(cover_panel);
	lv_image_set_src(qobuz_badge, &icon_qobuz_badge);
	lv_obj_align(qobuz_badge, LV_ALIGN_TOP_RIGHT, -BADGE_INSET, BADGE_INSET);
	lv_obj_add_flag(qobuz_badge, LV_OBJ_FLAG_HIDDEN);

	// Same corner, same size: the marks replace each other instead of sitting
	// side by side, and update_qobuz_badge() decides which.
	tidal_badge = lv_image_create(cover_panel);
	lv_image_set_src(tidal_badge, &icon_tidal_badge);
	lv_obj_align(tidal_badge, LV_ALIGN_TOP_RIGHT, -BADGE_INSET, BADGE_INSET);
	lv_obj_add_flag(tidal_badge, LV_OBJ_FLAG_HIDDEN);

	// And the third, same corner and same size as the other two.
	podcast_badge = lv_image_create(cover_panel);
	lv_image_set_src(podcast_badge, &icon_podcast_badge);
	lv_obj_align(podcast_badge, LV_ALIGN_TOP_RIGHT, -BADGE_INSET, BADGE_INSET);
	lv_obj_add_flag(podcast_badge, LV_OBJ_FLAG_HIDDEN);

	// Shown while there is no artwork for the current track.
	cover_placeholder_icon = lv_image_create(cover_panel);
	lv_image_set_src(cover_placeholder_icon, &icon_music_note);
	lv_obj_add_style(cover_placeholder_icon, &theme_style_text_dim, 0);
	lv_obj_set_style_image_recolor(cover_placeholder_icon, theme()->text_secondary, 0);
	lv_obj_set_style_image_recolor_opa(cover_placeholder_icon, LV_OPA_COVER, 0);
	lv_obj_center(cover_placeholder_icon);

	// Where the title and the star go in the alternative layout: two pills in
	// one bottom corner of the sleeve and a disc in the other. Built empty and
	// hidden; apply_layout() moves the real labels into them.
	//
	// Children of cover_panel so they sit on the sleeve and travel with it.
	// The column is what keeps the two pills stacked and both left-aligned
	// while each is only as wide as its own words.
	alt_text_col = lv_obj_create(cover_panel);
	lv_obj_remove_style_all(alt_text_col);
	lv_obj_set_size(alt_text_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_align(alt_text_col, LV_ALIGN_BOTTOM_LEFT, ALT_PAD, -ALT_PAD);
	lv_obj_set_flex_flow(alt_text_col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(alt_text_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(alt_text_col, 6, 0);
	lv_obj_remove_flag(alt_text_col, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(alt_text_col, LV_OBJ_FLAG_HIDDEN);
	// None of this takes presses. A plain object is clickable the moment it is
	// made, and these lie on the artwork -- which is the surface the player is
	// dragged shut by. Left clickable they swallowed every gesture that began
	// on the title, so putting the player away by pushing it off the right of
	// the screen worked everywhere except on the words naming the track.
	lv_obj_remove_flag(alt_text_col, LV_OBJ_FLAG_CLICKABLE);

	for (int i = 0; i < 2; i++) {
		lv_obj_t *pill = lv_obj_create(alt_text_col);
		lv_obj_remove_style_all(pill);
		lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
		lv_obj_set_style_pad_hor(pill, ALT_PILL_PAD_H, 0);
		lv_obj_set_style_pad_ver(pill, ALT_PILL_PAD_V, 0);
		lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
		// No shadow. There was one, to keep a pill from disappearing into a
		// patch of sleeve the same colour as itself -- but that was when the
		// fill was the sleeve's colour at full strength. It is now a dark
		// surface of fixed weight, which separates from artwork on its own, and
		// the shadow under it only made the pill look pasted on.
		lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
		if (i == 0) {
			alt_title_pill = pill;
		} else {
			alt_artist_pill = pill;
		}
	}

	alt_fav_circle = lv_obj_create(cover_panel);
	lv_obj_remove_style_all(alt_fav_circle);
	lv_obj_set_size(alt_fav_circle, ALT_FAV_SIZE, ALT_FAV_SIZE);
	lv_obj_set_style_radius(alt_fav_circle, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(alt_fav_circle, LV_OPA_COVER, 0);
	lv_obj_align(alt_fav_circle, LV_ALIGN_BOTTOM_RIGHT, -ALT_PAD, -ALT_PAD);
	lv_obj_remove_flag(alt_fav_circle, LV_OBJ_FLAG_SCROLLABLE);
	// Likewise: the star's own button is a child of this and takes its own
	// presses, so the disc around it has no reason to take any.
	lv_obj_remove_flag(alt_fav_circle, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(alt_fav_circle, LV_OBJ_FLAG_HIDDEN);

	cover_img = lv_image_create(cover_panel);
	lv_obj_center(cover_img);
	lv_obj_add_flag(cover_img, LV_OBJ_FLAG_HIDDEN);

	// ---------------------------------------------------------------------
	// Studio: the blurred sleeve behind the whole screen, and everything else
	// laid out over it.
	//
	// The background is a child of the page rather than of the artwork, so it
	// reaches under the controls as well; the panel that carries the rest is a
	// child of the artwork, so it travels with the sheet. Nothing on either
	// takes a press: the surface under them is what the player is pushed shut
	// by, and the buttons that do take presses are moved here from elsewhere.
	// ---------------------------------------------------------------------
	studio_bg = lv_image_create(player_screen);
	lv_obj_add_flag(studio_bg, LV_OBJ_FLAG_IGNORE_LAYOUT);
	lv_obj_set_size(studio_bg, cfg->screen_width, cfg->screen_height);
	lv_obj_set_pos(studio_bg, 0, 0);
	// COVER and not STRETCH: the blurred copy is asked for at the shape of the
	// screen, but between a change of arrangement and the picture that follows
	// it the one on hand is still the old shape, and stretching that is a
	// visibly squashed sleeve.
	lv_image_set_inner_align(studio_bg, LV_IMAGE_ALIGN_COVER);
	lv_obj_remove_flag(studio_bg, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(studio_bg, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_background(studio_bg);

	studio_box_w = studio_box_h = cover_size;
	studio_box = lv_obj_create(cover_panel);
	lv_obj_remove_style_all(studio_box);
	lv_obj_set_size(studio_box, studio_box_w, studio_box_h);
	lv_obj_set_pos(studio_box, 0, 0);
	lv_obj_remove_flag(studio_box, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(studio_box, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(studio_box, LV_OBJ_FLAG_HIDDEN);

	studio_head = lv_obj_create(studio_box);
	lv_obj_remove_style_all(studio_head);
	lv_obj_set_size(studio_head, cover_size - 2 * STUDIO_MARGIN, STUDIO_HEAD_H);
	lv_obj_remove_flag(studio_head, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(studio_head, LV_OBJ_FLAG_CLICKABLE);

	// Centred in the head and not filling it: the ellipsis sits at the right
	// edge and the chevron at the left, and the two names have to stop before
	// they reach either -- on both sides, or a long title would be centred
	// against one of them.
	studio_text_col = lv_obj_create(studio_head);
	lv_obj_remove_style_all(studio_text_col);
	lv_obj_set_size(studio_text_col, cover_size - 2 * STUDIO_MARGIN - 2 * 68, LV_SIZE_CONTENT);
	lv_obj_align(studio_text_col, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_flex_flow(studio_text_col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(studio_text_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(studio_text_col, 4, 0);
	lv_obj_set_style_text_align(studio_text_col, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_remove_flag(studio_text_col, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(studio_text_col, LV_OBJ_FLAG_CLICKABLE);

	// The sleeve at whatever size studio_place() settles on. The picture is
	// decoded at the width of the screen for the standard arrangement, so it is
	// scaled down here rather than decoded twice.
	studio_cover = lv_image_create(studio_box);
	lv_obj_add_flag(studio_cover, LV_OBJ_FLAG_IGNORE_LAYOUT);
	lv_image_set_inner_align(studio_cover, LV_IMAGE_ALIGN_STRETCH);
	lv_obj_remove_flag(studio_cover, LV_OBJ_FLAG_CLICKABLE);

	// What stands in the sleeve's place when the track has no artwork: the same
	// surface and the same mark the full-width cover panel uses, at the size
	// this arrangement gives the sleeve.
	studio_empty = lv_obj_create(studio_box);
	lv_obj_add_flag(studio_empty, LV_OBJ_FLAG_IGNORE_LAYOUT);
	lv_obj_add_style(studio_empty, &theme_style_panel, 0);
	lv_obj_set_style_border_width(studio_empty, 0, 0);
	lv_obj_set_style_radius(studio_empty, 8, 0);
	lv_obj_remove_flag(studio_empty, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(studio_empty, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(studio_empty, LV_OBJ_FLAG_HIDDEN);

	studio_empty_icon = lv_image_create(studio_empty);
	lv_obj_set_style_image_recolor_opa(studio_empty_icon, LV_OPA_COVER, 0);
	lv_obj_center(studio_empty_icon);
	lv_obj_remove_flag(studio_empty_icon, LV_OBJ_FLAG_CLICKABLE);

	studio_quality = lv_obj_create(studio_box);
	lv_obj_remove_style_all(studio_quality);
	lv_obj_set_flex_flow(studio_quality, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(studio_quality, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(studio_quality, 8, 0);
	lv_obj_remove_flag(studio_quality, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(studio_quality, LV_OBJ_FLAG_CLICKABLE);

	// No theme_style_icon here: the four quality marks carry their own colours,
	// which is how the track lists draw them, and a recolour to the text colour
	// flattens all four into the same grey glyph.
	studio_quality_icon = lv_image_create(studio_quality);

	// Last, so it is above the artwork: an invisible strip exactly where the
	// status bar would be, owning the pull-down that opens the control centre.
	lv_obj_t *panel_edge = lv_obj_create(player_screen);
	lv_obj_remove_style_all(panel_edge);
	lv_obj_set_size(panel_edge, cfg->screen_width, cfg->top_bar_height);
	lv_obj_set_pos(panel_edge, 0, 0);
	lv_obj_remove_flag(panel_edge, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(panel_edge, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(panel_edge, panel_edge_drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(panel_edge, panel_edge_drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(panel_edge, panel_edge_drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(panel_edge, panel_edge_drag_cb, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_move_foreground(panel_edge);

	// The backdrop over a loaded cover is always the dark treatment -- same
	// look in both themes; only the no-cover panel follows the theme.
	cover_set_backdrop_light(false);

	// Whichever arrangement was left selected, now that every widget it moves
	// exists.
	int saved = (int)config_get_int("screen", "player_layout_alt", 0);
	layout_choice = (saved == (int)PLAYER_LAYOUT_ALTERNATIVE)	? PLAYER_LAYOUT_ALTERNATIVE
					: (saved == (int)PLAYER_LAYOUT_STUDIO)		? PLAYER_LAYOUT_STUDIO
																: PLAYER_LAYOUT_STANDARD;
	layout_alt_now = false; // apply_layout() below puts the standard one up first
	layout_studio_now = false;
	apply_layout();

	theme_register_refresh(player_refresh_theme);
}
