#include "welcome.h"

#include <stdlib.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/settings/language.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"

// ---------------------------------------------------------------------------
// The words
//
// Literal rather than from tr(): these are the shipped languages greeting at
// once, not one string to translate into the chosen language -- which at this
// point has not been chosen yet.
//
// The language names beside them are for whoever reads this file; nothing is
// shown under the word on screen.
// ---------------------------------------------------------------------------

// English first: it is the language the player starts in on first boot (see
// LANG_FIRST_BOOT) and the first word shown. After that the cycle picks at
// random, never the same word twice running.
static const char *const WELCOME_WORDS[] = {
	"Welcome",	  // English
	"Benvenuto",  // Italian
	"Willkommen", // German
	"Bienvenue",  // French
	"Bienvenido", // Spanish
	// The default face draws Cyrillic and the CJK block, as the whole interface
	// does in Russian and in Chinese, so neither of these needs a font of its
	// own.
	"Добро пожаловать",
	"欢迎",
};

#define WELCOME_COUNT ((int)(sizeof(WELCOME_WORDS) / sizeof(WELCOME_WORDS[0])))

// Timing of one cycle step: the word rests for WELCOME_HOLD_MS, slides out over
// WELCOME_SLIDE_MS, and the next one slides in over the same time.
#define WELCOME_HOLD_MS 1500
#define WELCOME_SLIDE_MS 380

// Travel of the slide in and out. Deliberately small: it accompanies the fade
// rather than being a carousel.
#define WELCOME_SLIDE_PX 46

static lv_obj_t *panel;
static lv_obj_t *word_label;
static lv_obj_t *start_btn;
static lv_obj_t *start_label;
static lv_timer_t *hold_timer;
static int current_word = -1;

// ---------------------------------------------------------------------------
// The word cycle
// ---------------------------------------------------------------------------

// Random, but never the same word twice in a row: with this few words a repeat
// looks like a stuck animation rather than a random pick.
static int next_word(void) {
	if (WELCOME_COUNT <= 1) {
		return 0;
	}
	int pick;
	do {
		pick = rand() % WELCOME_COUNT;
	} while (pick == current_word);
	return pick;
}

static void slide_out_done(lv_anim_t *a);
static void start_hold(void);

static void set_x(void *obj, int32_t value) { lv_obj_set_style_translate_x((lv_obj_t *)obj, value, 0); }

static void set_opa(void *obj, int32_t value) { lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0); }

// Slides in from the right while fading up.
static void slide_in(void) {
	lv_anim_t move;
	lv_anim_init(&move);
	lv_anim_set_var(&move, word_label);
	lv_anim_set_exec_cb(&move, set_x);
	lv_anim_set_values(&move, WELCOME_SLIDE_PX, 0);
	lv_anim_set_duration(&move, WELCOME_SLIDE_MS);
	lv_anim_set_path_cb(&move, lv_anim_path_ease_out);
	lv_anim_start(&move);

	lv_anim_t fade;
	lv_anim_init(&fade);
	lv_anim_set_var(&fade, word_label);
	lv_anim_set_exec_cb(&fade, set_opa);
	lv_anim_set_values(&fade, LV_OPA_TRANSP, LV_OPA_COVER);
	lv_anim_set_duration(&fade, WELCOME_SLIDE_MS);
	lv_anim_start(&fade);

	start_hold();
}

// Slides out to the left while fading down, then swaps the word and slides the
// next one in from the other side.
static void slide_out(void) {
	lv_anim_t move;
	lv_anim_init(&move);
	lv_anim_set_var(&move, word_label);
	lv_anim_set_exec_cb(&move, set_x);
	lv_anim_set_values(&move, 0, -WELCOME_SLIDE_PX);
	lv_anim_set_duration(&move, WELCOME_SLIDE_MS);
	lv_anim_set_path_cb(&move, lv_anim_path_ease_in);
	lv_anim_start(&move);

	lv_anim_t fade;
	lv_anim_init(&fade);
	lv_anim_set_var(&fade, word_label);
	lv_anim_set_exec_cb(&fade, set_opa);
	lv_anim_set_values(&fade, LV_OPA_COVER, LV_OPA_TRANSP);
	lv_anim_set_duration(&fade, WELCOME_SLIDE_MS);
	// The continuation hangs off the fade, not the slide: both last the same,
	// but opacity is what decides when the old word is no longer visible, which
	// is the moment it can be swapped unseen.
	lv_anim_set_completed_cb(&fade, slide_out_done);
	lv_anim_start(&fade);
}

static void slide_out_done(lv_anim_t *a) {
	(void)a;
	if (!word_label) {
		return;
	}
	current_word = next_word();
	lv_label_set_text(word_label, WELCOME_WORDS[current_word]);
	slide_in();
}

static void hold_elapsed(lv_timer_t *t) {
	(void)t;
	hold_timer = NULL; // single shot: LVGL has already deleted it
	slide_out();
}

static void start_hold(void) {
	if (hold_timer) {
		lv_timer_delete(hold_timer);
	}
	hold_timer = lv_timer_create(hold_elapsed, WELCOME_HOLD_MS, NULL);
	lv_timer_set_repeat_count(hold_timer, 1);
}

static void stop_cycle(void) {
	if (hold_timer) {
		lv_timer_delete(hold_timer);
		hold_timer = NULL;
	}
	if (word_label) {
		lv_anim_delete(word_label, NULL);
	}
}

// ---------------------------------------------------------------------------

static void start_cb(lv_event_t *e) {
	(void)e;

	stop_cycle();
	lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

	// The next screen holds the display on itself, so nothing is released here:
	// the panel must not blank between the two pages.
	language_show_first_boot();
}

static void refresh_theme(void) {
	if (start_btn) {
		lv_obj_set_style_bg_color(start_btn, theme()->accent, 0);
	}
}

void welcome_init(gui_config_t *cfg) {
	// The cycle is random and needs a seed: without one, rand() returns the
	// same sequence on every boot and the words always appear in one fixed
	// order.
	srand((unsigned int)time(NULL));

	panel = lv_obj_create(lv_layer_top());
	lv_obj_set_size(panel, cfg->screen_width, cfg->screen_height);
	lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(panel, &theme_style_screen, 0);
	lv_obj_set_style_border_width(panel, 0, 0);
	lv_obj_set_style_radius(panel, 0, 0);
	lv_obj_set_style_pad_all(panel, cfg->padding, 0);
	lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

	// The word and the button below it are the only two elements, so they are
	// centred in the panel rather than stacked from the top as on the language
	// page, which holds more.
	lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(panel, 64, 0);

	word_label = lv_label_create(panel);
	lv_label_set_text(word_label, WELCOME_WORDS[0]);
	lv_obj_add_style(word_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(word_label, &font_ui_32, 0);
	// "Добро пожаловать" is the wide one at 32 px: one centred line, sized to
	// the panel less its padding so the longest word does not reach the edges.
	lv_obj_set_width(word_label, cfg->screen_width - 4 * cfg->padding);
	lv_obj_set_style_text_align(word_label, LV_TEXT_ALIGN_CENTER, 0);

	start_btn = lv_btn_create(panel);
	lv_obj_set_size(start_btn, 228, 64);
	lv_obj_set_style_radius(start_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(start_btn, theme()->accent, 0);
	lv_obj_set_style_shadow_width(start_btn, 0, 0);
	lv_obj_set_style_border_width(start_btn, 0, 0);
	lv_obj_add_event_cb(start_btn, start_cb, LV_EVENT_CLICKED, NULL);

	start_label = lv_label_create(start_btn);
	lv_label_set_text(start_label, tr("welcome_start"));
	lv_obj_set_style_text_font(start_label, &font_ui_24, 0);
	lv_obj_set_style_text_color(start_label, lv_color_white(), 0);
	lv_obj_center(start_label);

	theme_register_refresh(refresh_theme);
}

void welcome_show_first_boot(void) {
	if (!panel) {
		return;
	}

	// Always starts on "Welcome", the language the player boots in, and it
	// stands still while the page appears; the cycle starts afterwards.
	current_word = 0;
	lv_label_set_text(word_label, WELCOME_WORDS[current_word]);
	lv_obj_set_style_translate_x(word_label, 0, 0);
	lv_obj_set_style_opa(word_label, LV_OPA_COVER, 0);

	lv_obj_remove_flag(panel, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(panel);

	start_hold();

	// As on the other first-boot pages: reading the greetings takes as long as
	// it takes, and blanking while a top-layer panel is up also kills the
	// touchscreen.
	power_hold_screen_on(true);
}
