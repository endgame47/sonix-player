#include "metronomepage.h"

#include <math.h>
#include <stdio.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/volume_overlay.h"
#include "src/system/audio/audio.h"
#include "src/system/audio/metronome.h"
#include "src/system/bluetooth/btreceiver.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/remote/airplay.h"
#include "src/system/streaming/radio.h"

lv_obj_t *metronomepage_screen;

#define DEFAULT_BPM 100
#define DEFAULT_BEATS 4

// How often the arm is drawn again while it moves.
#define FRAME_MS 30

// The widest swing either side of upright, in degrees.
#define SWING_DEG 26.0f

// Taps further apart than this start a new tempo; the last few taps within it
// are averaged.
#define TAP_RESET_MS 2000
#define TAP_KEEP 5

#define ROUND_BTN 64
#define PLAY_BTN 88
#define PILL_H 56
#define DOT 16
#define DOT_FIRST 20

static lv_obj_t *drawing;
static lv_obj_t *dots[METRONOME_BEATS_MAX];
static lv_obj_t *bpm_label;
static lv_obj_t *tempo_label;
static lv_obj_t *slider;
static lv_obj_t *round_btns[2];
static lv_obj_t *round_glyphs[2];
static lv_obj_t *pill_btns[2];
static lv_obj_t *pill_labels[2];
static lv_obj_t *play_btn;
static lv_obj_t *play_glyph;
static lv_timer_t *frame_timer;

static float angle;	   // of the arm as drawn, in degrees; positive leans right
static int lit_beat = -1; // the dot shown as the beat being heard, -1 for none
static bool shown_running;
static bool config_dirty;

static uint32_t taps[TAP_KEEP];
static int tap_count;

// The time signatures offered, by beats in a bar.
static const char *const BEAT_NAMES[METRONOME_BEATS_MAX + 1] = {
	"", "1/4", "2/4", "3/4", "4/4", "5/4", "6/8", "7/8",
};

// The Italian tempo marks, which every score uses whatever its language: from
// this many beats a minute up to the next entry.
static const struct {
	int from;
	const char *name;
} TEMPOS[] = {
	{0, "Grave"},		{40, "Largo"},	   {60, "Larghetto"}, {66, "Adagio"}, {76, "Andante"},
	{108, "Moderato"},	{120, "Allegro"},  {156, "Vivace"},	  {176, "Presto"}, {200, "Prestissimo"},
};

static const char *tempo_name(int bpm) {
	const char *name = TEMPOS[0].name;
	for (size_t i = 0; i < sizeof(TEMPOS) / sizeof(TEMPOS[0]); i++) {
		if (bpm >= TEMPOS[i].from) {
			name = TEMPOS[i].name;
		}
	}
	return name;
}

// ---------------------------------------------------------------------------
// the drawing
//
// A metronome as GNOME draws its icons: flat shapes, round corners, the theme's
// own colours. A body narrowing to the top, a darker slot in it with a scale,
// and the arm with its weight. The weight sits higher the slower the tempo, as
// on a real one.
// ---------------------------------------------------------------------------

static lv_point_precise_t pt(float x, float y) {
	lv_point_precise_t p = {(lv_value_precise_t)lroundf(x), (lv_value_precise_t)lroundf(y)};
	return p;
}

static void line(lv_layer_t *layer, float x1, float y1, float x2, float y2, float width, lv_color_t color,
				 lv_opa_t opa) {
	lv_draw_line_dsc_t dsc;
	lv_draw_line_dsc_init(&dsc);
	dsc.p1 = pt(x1, y1);
	dsc.p2 = pt(x2, y2);
	dsc.width = (int32_t)lroundf(width);
	dsc.color = color;
	dsc.opa = opa;
	dsc.round_start = 1;
	dsc.round_end = 1;
	lv_draw_line(layer, &dsc);
}

static void triangle(lv_layer_t *layer, lv_point_precise_t a, lv_point_precise_t b, lv_point_precise_t c,
					 lv_color_t color) {
	lv_draw_triangle_dsc_t dsc;
	lv_draw_triangle_dsc_init(&dsc);
	dsc.p[0] = a;
	dsc.p[1] = b;
	dsc.p[2] = c;
	dsc.color = color;
	dsc.opa = LV_OPA_COVER;
	lv_draw_triangle(layer, &dsc);
}

static void circle(lv_layer_t *layer, float x, float y, float r, lv_color_t color) {
	lv_draw_rect_dsc_t dsc;
	lv_draw_rect_dsc_init(&dsc);
	dsc.bg_color = color;
	dsc.radius = LV_RADIUS_CIRCLE;
	lv_area_t a = {(int32_t)lroundf(x - r), (int32_t)lroundf(y - r), (int32_t)lroundf(x + r), (int32_t)lroundf(y + r)};
	lv_draw_rect(layer, &dsc, &a);
}

// A trapezoid with rounded corners, symmetric about `cx`: filled inside its
// corners' centres, and its edges stroked round with the corners' diameter.
static void trapezoid(lv_layer_t *layer, float cx, float top, float bottom, float top_half, float bottom_half,
					  float radius, lv_color_t color) {
	// Moved in along the slanted sides by a little more than the radius, so
	// the round stroke lands on the outline.
	float slant = (bottom_half - top_half) / (bottom - top);
	float inset = radius * sqrtf(1.0f + slant * slant);
	float t = top + radius, b = bottom - radius;
	float th = top_half + slant * radius - inset;
	float bh = bottom_half - slant * radius - inset;
	// A rectangle down the middle and a triangle either side of it: the joins
	// run straight down on whole pixels, where a diagonal one would show as a
	// faint seam.
	int32_t il = (int32_t)lroundf(cx - th), ir = (int32_t)lroundf(cx + th);
	lv_draw_rect_dsc_t rect;
	lv_draw_rect_dsc_init(&rect);
	rect.bg_color = color;
	lv_area_t middle = {il, (int32_t)lroundf(t), ir, (int32_t)lroundf(b)};
	lv_draw_rect(layer, &rect, &middle);
	lv_point_precise_t tl = {il, (lv_value_precise_t)lroundf(t)}, tr = {ir, (lv_value_precise_t)lroundf(t)};
	lv_point_precise_t ml = {il, (lv_value_precise_t)lroundf(b)}, mr = {ir, (lv_value_precise_t)lroundf(b)};
	triangle(layer, tl, ml, pt(cx - bh, b), color);
	triangle(layer, tr, mr, pt(cx + bh, b), color);
	float w = radius * 2.0f;
	line(layer, cx - th, t, cx + th, t, w, color, LV_OPA_COVER);
	line(layer, cx + th, t, cx + bh, b, w, color, LV_OPA_COVER);
	line(layer, cx + bh, b, cx - bh, b, w, color, LV_OPA_COVER);
	line(layer, cx - bh, b, cx - th, t, w, color, LV_OPA_COVER);
}

static void draw_cb(lv_event_t *e) {
	lv_obj_t *obj = lv_event_get_current_target(e);
	lv_layer_t *layer = lv_event_get_layer(e);
	if (!layer) {
		return;
	}
	lv_area_t area;
	lv_obj_get_coords(obj, &area);
	float w = (float)lv_area_get_width(&area);
	float h = (float)lv_area_get_height(&area);

	// The whole figure fits the box: as tall as it can be at its proportions.
	float H = h < w * 1.25f ? h : w * 1.25f;
	float W = H * 0.80f;
	float cx = (float)area.x1 + w / 2.0f;
	float top = (float)area.y1 + (h - H) / 2.0f;

	const theme_palette_t *t = theme();
	lv_color_t body = lv_color_mix(t->accent, t->surface, 96);
	lv_color_t face = t->screen_bg;
	lv_color_t arm = t->text_primary;

	float body_bottom = top + H;
	trapezoid(layer, cx, top, body_bottom, W * 0.17f, W * 0.42f, H * 0.045f, body);

	// The slot the arm swings in front of, with its scale.
	float face_top = top + H * 0.11f;
	float face_bottom = body_bottom - H * 0.10f;
	trapezoid(layer, cx, face_top, face_bottom, W * 0.065f, W * 0.20f, H * 0.02f, face);
	for (int i = 1; i <= 6; i++) {
		float y = face_top + (face_bottom - face_top) * (float)i / 8.0f;
		float half = (W * 0.065f + (W * 0.20f - W * 0.065f) * (y - face_top) / (face_bottom - face_top)) * 0.45f;
		line(layer, cx - half, y, cx + half, y, H * 0.008f > 2.0f ? H * 0.008f : 2.0f, t->text_secondary, LV_OPA_40);
	}

	// The arm, from its pivot near the bottom of the slot.
	float px = cx, py = face_bottom - H * 0.045f;
	float length = py - (top + H * 0.02f);
	float rad = angle * (float)M_PI / 180.0f;
	float dx = sinf(rad), dy = -cosf(rad);
	float arm_w = H * 0.028f > 4.0f ? H * 0.028f : 4.0f;
	line(layer, px, py, px + dx * length, py + dy * length, arm_w, arm, LV_OPA_COVER);

	// The weight: higher on the arm the slower the tempo.
	float slow = 1.0f - (float)(metronome_bpm() - METRONOME_BPM_MIN) / (float)(METRONOME_BPM_MAX - METRONOME_BPM_MIN);
	float at = length * (0.40f + 0.46f * slow);
	float half = H * 0.035f;
	float wx = px + dx * at, wy = py + dy * at;
	line(layer, wx - dx * half, wy - dy * half, wx + dx * half, wy + dy * half, H * 0.085f, t->accent, LV_OPA_COVER);

	circle(layer, px, py, H * 0.032f, arm);
	circle(layer, px, py, H * 0.012f, face);
}

// ---------------------------------------------------------------------------
// the controls
// ---------------------------------------------------------------------------

static void paint_dots(void) {
	int beats = metronome_beats();
	for (int i = 0; i < METRONOME_BEATS_MAX; i++) {
		lv_obj_set_hidden(dots[i], i >= beats);
		bool lit = i == lit_beat;
		lv_obj_set_style_bg_color(dots[i], lit ? theme()->accent : theme()->text_secondary, 0);
		lv_obj_set_style_bg_opa(dots[i], lit ? LV_OPA_COVER : LV_OPA_40, 0);
	}
}

static void show_tempo(void) {
	int bpm = metronome_bpm();
	lv_label_set_text_fmt(bpm_label, "%d", bpm);
	lv_label_set_text_fmt(tempo_label, "BPM \xC2\xB7 %s", tempo_name(bpm)); // U+00B7
	if (lv_slider_get_value(slider) != bpm) {
		lv_slider_set_value(slider, bpm, LV_ANIM_OFF);
	}
	lv_obj_invalidate(drawing);
}

static void show_running(bool running) {
	shown_running = running;
	lv_image_set_src(play_glyph, running ? &icon_pause_large : &icon_play_large);
	// While it clicks the screen stays on: the beat is something to watch.
	power_hold_screen_for_view(running);
}

static void set_bpm(int bpm) {
	if (bpm == metronome_bpm()) {
		return;
	}
	metronome_set_bpm(bpm);
	config_dirty = true;
	show_tempo();
}

static void stop(void) {
	metronome_stop();
	lit_beat = -1;
	paint_dots();
	show_running(false);
}

// Anything sounding: a track, a station, a phone playing to the player over
// Bluetooth or AirPlay. The metronome never plays over it.
static bool something_playing(void) {
	if (audio_get_status() == AUDIO_STATUS_PLAYING || radio_is_playing() || radio_is_connecting() ||
		btreceiver_is_streaming()) {
		return true;
	}
	airplay_state_t airplay;
	if (airplay_running()) {
		airplay_get_state(&airplay);
		return airplay.playing;
	}
	return false;
}

static void play_cb(lv_event_t *e) {
	(void)e;
	if (metronome_running()) {
		stop();
		return;
	}
	if (something_playing()) {
		gui_notify_popup("metronome_stop_playback_first");
		return;
	}
	if (metronome_start()) {
		show_running(true);
	}
}

static void step_cb(lv_event_t *e) {
	set_bpm(metronome_bpm() + (int)(intptr_t)lv_event_get_user_data(e));
}

static void slider_cb(lv_event_t *e) {
	(void)e;
	set_bpm((int)lv_slider_get_value(slider));
}

static void tap_cb(lv_event_t *e) {
	(void)e;
	uint32_t now = lv_tick_get();
	if (tap_count > 0 && lv_tick_diff(now, taps[tap_count - 1]) > TAP_RESET_MS) {
		tap_count = 0;
	}
	if (tap_count == TAP_KEEP) {
		for (int i = 1; i < TAP_KEEP; i++) {
			taps[i - 1] = taps[i];
		}
		tap_count--;
	}
	taps[tap_count++] = now;
	if (tap_count >= 2) {
		uint32_t span = lv_tick_diff(taps[tap_count - 1], taps[0]);
		if (span > 0) {
			set_bpm((int)lroundf(60000.0f * (float)(tap_count - 1) / (float)span));
		}
	}
}

static void beats_chosen(void *user) {
	int beats = (int)(intptr_t)user;
	if (beats != metronome_beats()) {
		metronome_set_beats(beats);
		config_dirty = true;
	}
	lv_label_set_text(pill_labels[1], BEAT_NAMES[metronome_beats()]);
	lit_beat = -1;
	paint_dots();
}

static void beats_cb(lv_event_t *e) {
	popover_item_t items[METRONOME_BEATS_MAX];
	for (int i = 0; i < METRONOME_BEATS_MAX; i++) {
		items[i] = (popover_item_t){BEAT_NAMES[i + 1], beats_chosen, (void *)(intptr_t)(i + 1), i + 1 == metronome_beats()};
	}
	popover_show(lv_event_get_current_target(e), items, METRONOME_BEATS_MAX);
}

// Moves the arm and the lit dot to where the sound is. Stopped, the arm
// settles back to upright instead of stopping where it was.
static void frame_cb(lv_timer_t *timer) {
	(void)timer;
	if (lv_screen_active() != metronomepage_screen) {
		return;
	}
	bool running = metronome_running();
	if (running != shown_running && !running) {
		// The output would not open, or something started playing and took
		// it: the metronome stops without a word.
		stop();
	}
	if (running) {
		// Again on every frame: the player lets go of the same hold when its
		// sheet closes over this page.
		power_hold_screen_for_view(true);
	}

	float target = angle;
	int beat = -1;
	uint32_t count = 0;
	float phase = 0.0f;
	if (running && metronome_position(&beat, &count, &phase)) {
		// Through the middle on every beat, out to one side and back between
		// them, the other side each time: the way a real one moves.
		target = SWING_DEG * sinf((float)M_PI * phase) * ((count & 1) ? -1.0f : 1.0f);
	} else if (!running) {
		target = fabsf(angle) < 0.3f ? 0.0f : angle * 0.7f;
	}
	if (target != angle) {
		angle = target;
		lv_obj_invalidate(drawing);
	}
	if (beat != lit_beat && (running || lit_beat >= 0)) {
		lit_beat = beat;
		paint_dots();
	}
}

static lv_obj_t *plain_row(lv_obj_t *parent) {
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, 0, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	return row;
}

static lv_obj_t *round_button(lv_obj_t *parent, int size, const lv_image_dsc_t *glyph, lv_obj_t **glyph_out) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, size, size);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_pad_all(btn, 0, 0);
	lv_obj_t *icon = lv_image_create(btn);
	lv_image_set_src(icon, glyph);
	lv_obj_center(icon);
	*glyph_out = icon;
	return btn;
}

static lv_obj_t *pill_button(lv_obj_t *parent, const char *text, lv_obj_t **label_out) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, PILL_H);
	lv_obj_set_style_min_width(btn, 104, 0);
	lv_obj_set_style_pad_hor(btn, 24, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0); // Adwaita pill button
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, text);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_center(label);
	*label_out = label;
	return btn;
}

// The colours set by hand, again after a theme or accent change.
static void refresh_theme(void) {
	const theme_palette_t *t = theme();
	for (int i = 0; i < 2; i++) {
		lv_obj_set_style_bg_color(round_btns[i], t->surface_pressed, 0);
		lv_obj_set_style_image_recolor(round_glyphs[i], t->text_primary, 0);
		lv_obj_set_style_bg_color(pill_btns[i], t->surface_pressed, 0);
		lv_obj_set_style_text_color(pill_labels[i], t->text_primary, 0);
	}
	lv_obj_set_style_bg_color(play_btn, t->accent, 0);
	theme_apply_slider_knob(slider);
	paint_dots();
	lv_obj_invalidate(drawing);
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	volume_overlay_allow_outside_player(true);
	lit_beat = -1;
	angle = 0.0f;
	tap_count = 0;
	lv_label_set_text(pill_labels[1], BEAT_NAMES[metronome_beats()]);
	paint_dots();
	show_tempo();
	show_running(metronome_running());
	lv_timer_resume(frame_timer);
}

// Leaving stops it: the output goes back to the music.
static void unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(frame_timer);
	stop();
	volume_overlay_allow_outside_player(false);
	if (config_dirty) {
		config_dirty = false;
		config_set_int("metronome", "bpm", metronome_bpm());
		config_set_int("metronome", "beats", metronome_beats());
		config_save();
	}
}

bool metronomepage_key(gui_key_t key) {
	if (!metronome_running()) {
		return false;
	}
	if (key == GUI_KEY_PLAY_PAUSE) {
		stop();
	}
	return true;
}

void metronomepage_init(gui_config_t *cfg) {
	metronome_set_bpm((int)config_get_int("metronome", "bpm", DEFAULT_BPM));
	metronome_set_beats((int)config_get_int("metronome", "beats", DEFAULT_BEATS));

	lv_obj_add_style(metronomepage_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(metronomepage_screen, false);
	settingsrow_title(metronomepage_screen, cfg, "metronome");

	int top = settingsrow_content_top(cfg);
	lv_obj_t *page = lv_obj_create(metronomepage_screen);
	lv_obj_set_pos(page, cfg->padding, top);
	lv_obj_set_size(page, (int32_t)cfg->screen_width - 2 * cfg->padding, (int32_t)cfg->screen_height - top - cfg->padding);
	lv_obj_set_style_bg_opa(page, 0, 0);
	lv_obj_set_style_border_width(page, 0, 0);
	lv_obj_set_style_pad_all(page, 0, 0);
	lv_obj_set_style_pad_row(page, 0, 0);
	lv_obj_set_scrollable(page, false);
	lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	switcher_attach_back_gesture(page);

	// The drawing takes what the controls leave.
	drawing = lv_obj_create(page);
	lv_obj_remove_style_all(drawing);
	lv_obj_set_width(drawing, lv_pct(100));
	lv_obj_set_flex_grow(drawing, 1);
	lv_obj_set_clickable(drawing, false);
	lv_obj_add_event_cb(drawing, draw_cb, LV_EVENT_DRAW_MAIN, NULL);

	// A dot for each beat of the bar, the first one larger.
	lv_obj_t *dot_row = plain_row(page);
	lv_obj_set_style_pad_column(dot_row, 14, 0);
	lv_obj_set_style_margin_top(dot_row, 14, 0);
	lv_obj_set_height(dot_row, DOT_FIRST);
	for (int i = 0; i < METRONOME_BEATS_MAX; i++) {
		dots[i] = lv_obj_create(dot_row);
		lv_obj_remove_style_all(dots[i]);
		int size = i == 0 ? DOT_FIRST : DOT;
		lv_obj_set_size(dots[i], size, size);
		lv_obj_set_style_radius(dots[i], LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(dots[i], LV_OPA_40, 0);
	}

	// The tempo, with a step either side of it.
	lv_obj_t *tempo_row = plain_row(page);
	lv_obj_set_style_pad_column(tempo_row, 18, 0);
	lv_obj_set_style_margin_top(tempo_row, 10, 0);
	round_btns[0] = round_button(tempo_row, ROUND_BTN, &icon_minus, &round_glyphs[0]);
	bpm_label = lv_label_create(tempo_row);
	lv_obj_set_width(bpm_label, 170);
	lv_obj_add_style(bpm_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(bpm_label, &font_ui_64_bold, 0);
	lv_obj_set_style_text_align(bpm_label, LV_TEXT_ALIGN_CENTER, 0);
	round_btns[1] = round_button(tempo_row, ROUND_BTN, &icon_plus, &round_glyphs[1]);
	for (int i = 0; i < 2; i++) {
		void *step = (void *)(intptr_t)(i == 0 ? -1 : 1);
		lv_obj_add_event_cb(round_btns[i], step_cb, LV_EVENT_SHORT_CLICKED, step);
		lv_obj_add_event_cb(round_btns[i], step_cb, LV_EVENT_LONG_PRESSED, step);
		lv_obj_add_event_cb(round_btns[i], step_cb, LV_EVENT_LONG_PRESSED_REPEAT, step);
	}

	tempo_label = lv_label_create(page);
	lv_obj_add_style(tempo_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(tempo_label, &font_ui_22, 0);

	slider = lv_slider_create(page);
	lv_obj_set_width(slider, lv_pct(92));
	lv_obj_set_height(slider, 8);
	lv_obj_set_style_margin_top(slider, 22, 0);
	lv_slider_set_range(slider, METRONOME_BPM_MIN, METRONOME_BPM_MAX);
	lv_obj_add_style(slider, &theme_style_slider_track, LV_PART_MAIN);
	lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_add_style(slider, &theme_style_accent_bg, LV_PART_INDICATOR);
	lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
	lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
	lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(slider, 8, LV_PART_KNOB);
	lv_obj_set_style_shadow_opa(slider, LV_OPA_30, LV_PART_KNOB);
	lv_obj_set_style_shadow_color(slider, lv_color_black(), LV_PART_KNOB);
	lv_obj_set_style_shadow_offset_y(slider, 1, LV_PART_KNOB);
	lv_obj_set_ext_click_area(slider, 20);
	lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

	// Tap, start and stop, the bar.
	lv_obj_t *bottom = plain_row(page);
	lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_margin_top(bottom, 24, 0);
	pill_btns[0] = pill_button(bottom, tr("metronome_tap"), &pill_labels[0]);
	// On the press, not the release: a tap is timed by when the finger lands.
	lv_obj_add_event_cb(pill_btns[0], tap_cb, LV_EVENT_PRESSED, NULL);
	play_btn = round_button(bottom, PLAY_BTN, &icon_play_large, &play_glyph);
	lv_obj_set_style_image_recolor(play_glyph, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(play_glyph, LV_OPA_COVER, 0);
	lv_obj_add_event_cb(play_btn, play_cb, LV_EVENT_CLICKED, NULL);
	pill_btns[1] = pill_button(bottom, BEAT_NAMES[metronome_beats()], &pill_labels[1]);
	lv_obj_add_event_cb(pill_btns[1], beats_cb, LV_EVENT_CLICKED, NULL);

	for (int i = 0; i < 2; i++) {
		lv_obj_set_style_image_recolor_opa(round_glyphs[i], LV_OPA_COVER, 0);
	}
	refresh_theme();
	theme_register_refresh(refresh_theme);

	lv_obj_add_event_cb(metronomepage_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(metronomepage_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	frame_timer = lv_timer_create(frame_cb, FRAME_MS, NULL);
	lv_timer_pause(frame_timer);
}
