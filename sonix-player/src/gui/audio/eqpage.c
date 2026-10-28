#include "eqpage.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/src/misc/cache/instance/lv_image_cache.h"
#include "src/gui/audio/eqsettings.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/audio/eq.h"
#include "src/system/core/lang.h"

// The board: the plot with the dB scale on its left, each band's value above
// it and its frequency below.
#define BOARD_HEIGHT 330
#define CARD_PAD 12
#define AXIS_WIDTH 40
#define TOP_ROW 30
#define BOTTOM_ROW 30
#define RIGHT_PAD 6
#define KNOB 22
#define KNOB_CHOSEN 30
// How far a finger moves before a press on the board is a drag.
#define DRAG_SLOP 4
// The curve is worked out at this many frequencies and drawn between them.
#define CURVE_POINTS 160
// The curve as a 96 kHz stream has it. Nearer the rates of CD and 48 kHz the
// top band is squeezed against half the rate and the curve drops to 0 dB past
// 20 kHz, which nobody hears and which would only look like a fault.
#define CURVE_RATE 96000
// The lowest band's centre as the octave scale has it (eq_band_freq rounds it).
#define FIRST_CENTRE 31.25

static struct {
	gui_config_t *cfg;
	lv_obj_t *screen;
	lv_obj_t *enable_sw;
	lv_obj_t *reset_btn;
	lv_obj_t *card;
	lv_obj_t *board;
	lv_obj_t *plot;
	lv_obj_t *knobs[EQ_BANDS];
	lv_obj_t *values[EQ_BANDS];
	lv_obj_t *freqs[EQ_BANDS];
	lv_obj_t *band_card;
	lv_obj_t *band_title;
	lv_obj_t *band_value;
	lv_obj_t *band_btns[3];
	lv_obj_t *width_pills[EQ_WIDTHS];
	lv_obj_t *headroom;

	lv_draw_buf_t *buf;
	int plot_w, plot_h;
	int selected;
	bool pressing;
	bool dragging;
	lv_point_t press_at;
	int press_value;
} eq;

// ---------------------------------------------------------------------------
// Numbers
// ---------------------------------------------------------------------------

// "+3,5", "-12", "0": a band's value above it. The decimal comma follows the
// Italian base interface, as on the parametric's page.
static void short_db(int tenths, char *out, size_t size) {
	int a = tenths < 0 ? -tenths : tenths;
	const char *sign = tenths < 0 ? "-" : (tenths > 0 ? "+" : "");
	if (a % 10 == 0) {
		snprintf(out, size, "%s%d", sign, a / 10);
	} else {
		snprintf(out, size, "%s%d,%d", sign, a / 10, a % 10);
	}
}

static void long_db(int tenths, char *out, size_t size) {
	int a = tenths < 0 ? -tenths : tenths;
	const char *sign = tenths < 0 ? "-" : (tenths > 0 ? "+" : "");
	snprintf(out, size, "%s%d,%d dB", sign, a / 10, a % 10);
}

static void freq_short(int hz, char *out, size_t size) {
	hz &= 0xffff;
	if (hz >= 1000) {
		snprintf(out, size, "%dk", hz / 1000);
	} else {
		snprintf(out, size, "%d", hz);
	}
}

static void freq_long(int hz, char *out, size_t size) {
	hz &= 0xffff;
	if (hz >= 1000) {
		snprintf(out, size, "%d kHz", hz / 1000);
	} else {
		snprintf(out, size, "%d Hz", hz);
	}
}

// ---------------------------------------------------------------------------
// Geometry, pixels from the board's top left
// ---------------------------------------------------------------------------

static float column_width(void) { return (float)eq.plot_w / EQ_BANDS; }

static int column_x(int band) { return AXIS_WIDTH + (int)lroundf(column_width() * ((float)band + 0.5f)); }

// The distance from the 0 dB line to the top of the range.
static float half_range(void) { return (float)eq.plot_h / 2.0f - KNOB_CHOSEN / 2.0f - 2.0f; }

static int value_y(int tenths) {
	return TOP_ROW + (int)lroundf((float)eq.plot_h / 2.0f - (float)tenths * half_range() / (EQ_GAIN_MAX_DB * 10));
}

// ---------------------------------------------------------------------------
// The plot: the grid, the curve and the colour under it, drawn by hand into a
// picture of the plot's size.
// ---------------------------------------------------------------------------

static inline void blend(uint8_t *px, lv_color_t c, float a) {
	if (a <= 0.0f) {
		return;
	}
	if (a > 1.0f) {
		a = 1.0f;
	}
	float da = px[3] / 255.0f;
	float oa = a + da * (1.0f - a);
	if (oa <= 0.0f) {
		return;
	}
	float k = a / oa;
	px[0] = (uint8_t)(c.blue * k + px[0] * (1.0f - k));
	px[1] = (uint8_t)(c.green * k + px[1] * (1.0f - k));
	px[2] = (uint8_t)(c.red * k + px[2] * (1.0f - k));
	px[3] = (uint8_t)(oa * 255.0f + 0.5f);
}

static void hline(int y, int w, int h, lv_color_t c, float a, int thick) {
	uint8_t *base = eq.buf->data;
	uint32_t stride = eq.buf->header.stride;
	for (int t = 0; t < thick; t++) {
		int yy = y + t - thick / 2;
		if (yy < 0 || yy >= h) {
			continue;
		}
		for (int x = 0; x < w; x++) {
			blend(base + yy * stride + x * 4, c, a);
		}
	}
}

static void vline(int x, int h, lv_color_t c, float a) {
	uint8_t *base = eq.buf->data;
	uint32_t stride = eq.buf->header.stride;
	for (int y = 0; y < h; y++) {
		blend(base + y * stride + x * 4, c, a);
	}
}

static void render_plot(void) {
	if (!eq.buf) {
		return;
	}
	int w = (int)eq.buf->header.w;
	int h = (int)eq.buf->header.h;
	uint8_t *base = eq.buf->data;
	uint32_t stride = eq.buf->header.stride;
	memset(base, 0, (size_t)stride * (size_t)h);

	bool on = eq_get_enabled();
	lv_color_t ink = theme()->text_secondary;
	lv_color_t accent = on ? theme()->accent : theme()->text_secondary;
	float mid = (float)h / 2.0f;
	float per_tenth = half_range() / (EQ_GAIN_MAX_DB * 10);

	// The scale every 6 dB, the 0 dB line stronger; a faint line down each band.
	for (int db = -EQ_GAIN_MAX_DB; db <= EQ_GAIN_MAX_DB; db += 6) {
		int y = (int)lroundf(mid - (float)db * 10.0f * per_tenth);
		hline(y, w, h, ink, db == 0 ? 0.45f : 0.16f, db == 0 ? 2 : 1);
	}
	for (int i = 0; i < EQ_BANDS; i++) {
		int x = column_x(i) - AXIS_WIDTH;
		if (x >= 0 && x < w) {
			vline(x, h, ink, 0.08f);
		}
	}

	// The curve, a value per pixel column, between CURVE_POINTS worked out by
	// the engine. One band is one octave, so frequency doubles every column.
	float col = column_width();
	float first = col / 2.0f;
	double lo = log2(FIRST_CENTRE) - first / col;
	double hi = log2(FIRST_CENTRE) + (w - first) / col;
	int freqs[CURVE_POINTS + 1];
	int tenths_at[CURVE_POINTS + 1];
	for (int i = 0; i <= CURVE_POINTS; i++) {
		freqs[i] = (int)lround(pow(2.0, lo + (hi - lo) * i / CURVE_POINTS));
	}
	eq_response_curve(freqs, CURVE_POINTS + 1, CURVE_RATE, tenths_at);
	float curve[CURVE_POINTS + 1];
	for (int i = 0; i <= CURVE_POINTS; i++) {
		curve[i] = (float)tenths_at[i];
	}
	float *ys = malloc(sizeof(float) * (size_t)w);
	if (!ys) {
		return;
	}
	for (int x = 0; x < w; x++) {
		float pos = (float)x * CURVE_POINTS / (float)(w - 1);
		int i = (int)pos;
		if (i >= CURVE_POINTS) {
			i = CURVE_POINTS - 1;
		}
		float t = pos - (float)i;
		float tenths = curve[i] + (curve[i + 1] - curve[i]) * t;
		ys[x] = mid - tenths * per_tenth;
	}

	// The colour between the curve and 0 dB, strongest at the curve.
	for (int x = 0; x < w; x++) {
		float y = ys[x];
		float span = fabsf(y - mid);
		if (span < 0.5f) {
			continue;
		}
		int a = (int)floorf(fminf(y, mid));
		int b = (int)ceilf(fmaxf(y, mid));
		for (int yy = a < 0 ? 0 : a; yy <= b && yy < h; yy++) {
			float frac = fabsf((float)yy + 0.5f - mid) / span;
			if (frac > 1.0f) {
				frac = 1.0f;
			}
			blend(base + yy * stride + x * 4, accent, 0.06f + 0.30f * frac);
		}
	}

	// The line itself, its edges smoothed by the distance to it.
	float half = 1.6f;
	for (int x = 0; x < w; x++) {
		float slope = x + 1 < w ? ys[x + 1] - ys[x] : ys[x] - ys[x - 1];
		float norm = sqrtf(1.0f + slope * slope);
		float reach = half * norm + 2.0f;
		int a = (int)floorf(ys[x] - reach);
		int b = (int)ceilf(ys[x] + reach);
		for (int yy = a < 0 ? 0 : a; yy <= b && yy < h; yy++) {
			float d = fabsf((float)yy + 0.5f - ys[x]) / norm;
			blend(base + yy * stride + x * 4, accent, half + 0.5f - d);
		}
	}
	free(ys);

	lv_image_cache_drop(eq.buf);
	lv_obj_invalidate(eq.plot);
}

// ---------------------------------------------------------------------------
// The knobs, the values and the chosen band
// ---------------------------------------------------------------------------

static void paint_knob(int band) {
	lv_obj_t *k = eq.knobs[band];
	bool on = eq_get_enabled();
	bool chosen = band == eq.selected;
	int size = chosen ? KNOB_CHOSEN : KNOB;
	lv_obj_set_size(k, size, size);
	lv_obj_set_pos(k, column_x(band) - size / 2, value_y(eq_get_band_tenths(band)) - size / 2);
	lv_color_t accent = on ? theme()->accent : theme()->text_secondary;
	lv_obj_set_style_bg_color(k, chosen ? accent : lv_color_white(), 0);
	lv_obj_set_style_border_color(k, chosen ? lv_color_white() : accent, 0);
	lv_obj_set_style_border_width(k, chosen ? 3 : 4, 0);

	char text[16];
	short_db(eq_get_band_tenths(band), text, sizeof(text));
	lv_label_set_text(eq.values[band], text);
	lv_obj_set_style_text_color(eq.values[band], chosen && on ? theme()->accent : theme()->text_secondary, 0);
}

static void paint_band_card(void) {
	char text[24];
	freq_long(eq_band_freq[eq.selected], text, sizeof(text));
	lv_label_set_text(eq.band_title, text);
	long_db(eq_get_band_tenths(eq.selected), text, sizeof(text));
	lv_label_set_text(eq.band_value, text);
	lv_obj_set_style_text_color(eq.band_value, eq_get_enabled() ? theme()->accent : theme()->text_secondary, 0);
}

static void paint_headroom(void) {
	int tenths = eq_get_enabled() ? eq_auto_headroom_tenths() : 0;
	if (tenths >= 0) {
		lv_obj_set_hidden(eq.headroom, true);
		return;
	}
	char db[16], text[160];
	snprintf(db, sizeof(db), "%d,%d dB", -tenths / 10, -tenths % 10);
	snprintf(text, sizeof(text), tr("eq_headroom_note"), db);
	lv_label_set_text(eq.headroom, text);
	lv_obj_set_hidden(eq.headroom, false);
}

static void set_controls_enabled(bool on) {
	lv_obj_set_clickable(eq.board, on);
	for (int i = 0; i < 3; i++) {
		lv_obj_set_state(eq.band_btns[i], LV_STATE_DISABLED, !on);
	}
	lv_obj_set_style_opa(eq.band_card, on ? LV_OPA_COVER : LV_OPA_50, 0);
	if (eq.reset_btn) {
		lv_obj_set_clickable(eq.reset_btn, on);
		lv_obj_set_style_opa(eq.reset_btn, on ? LV_OPA_COVER : LV_OPA_40, 0);
	}
}

static void paint_all(void) {
	for (int i = 0; i < EQ_BANDS; i++) {
		paint_knob(i);
	}
	paint_band_card();
	render_plot();
}

// A band's value changed: what it moves on the page.
static void band_changed(int band) {
	paint_knob(band);
	if (band == eq.selected) {
		paint_band_card();
	}
	render_plot();
}

// After a change is finished: the file, and what is worked out from all ten.
static void settled(void) {
	eq_save();
	paint_headroom();
}

static void select_band(int band) {
	if (band < 0 || band >= EQ_BANDS || band == eq.selected) {
		return;
	}
	int old = eq.selected;
	eq.selected = band;
	paint_knob(old);
	paint_knob(band);
	paint_band_card();
}

void eqpage_refresh(void) {
	if (!eq.screen) {
		return;
	}
	bool on = eq_get_enabled();
	lv_obj_set_state(eq.enable_sw, LV_STATE_CHECKED, on);
	set_controls_enabled(on);
	for (int i = 0; i < EQ_WIDTHS; i++) {
		settingsrow_pill_active(eq.width_pills[i], eq_get_width() == i);
	}
	paint_all();
	paint_headroom();
}

// ---------------------------------------------------------------------------
// Touch
// ---------------------------------------------------------------------------

static void board_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev = lv_indev_active();
	if (!indev || !eq_get_enabled()) {
		return;
	}
	lv_point_t p;
	lv_indev_get_point(indev, &p);
	lv_area_t a;
	lv_obj_get_coords(eq.board, &a);

	if (code == LV_EVENT_PRESSED) {
		int band = (int)floorf((float)(p.x - a.x1 - AXIS_WIDTH) / column_width());
		band = band < 0 ? 0 : (band >= EQ_BANDS ? EQ_BANDS - 1 : band);
		select_band(band);
		eq.pressing = true;
		eq.dragging = false;
		eq.press_at = p;
		eq.press_value = eq_get_band_tenths(band);
	} else if (code == LV_EVENT_PRESSING && eq.pressing) {
		int dy = p.y - eq.press_at.y;
		if (!eq.dragging && abs(dy) < DRAG_SLOP) {
			return;
		}
		eq.dragging = true;
		int tenths = eq.press_value - (int)lroundf((float)dy * (EQ_GAIN_MAX_DB * 10) / half_range());
		int before = eq_get_band_tenths(eq.selected);
		eq_set_band_tenths(eq.selected, tenths);
		if (eq_get_band_tenths(eq.selected) != before) {
			band_changed(eq.selected);
		}
	} else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		if (eq.pressing && eq.dragging) {
			settled();
		}
		eq.pressing = false;
		eq.dragging = false;
	}
}

static void step_cb(lv_event_t *e) {
	if (!eq_get_enabled()) {
		return;
	}
	int step = (int)(intptr_t)lv_event_get_user_data(e);
	int before = eq_get_band_tenths(eq.selected);
	eq_set_band_tenths(eq.selected, step == 0 ? 0 : before + step);
	if (eq_get_band_tenths(eq.selected) != before) {
		band_changed(eq.selected);
		settled();
	}
}

static void enable_cb(lv_event_t *e) {
	(void)e;
	eq_set_enabled(lv_obj_has_state(eq.enable_sw, LV_STATE_CHECKED));
	eqpage_refresh();
}

static void width_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	eq_set_width((int)(intptr_t)lv_event_get_user_data(e));
	eqpage_refresh();
}

static void reset_action(void *user) {
	(void)user;
	eq_reset();
	eqpage_refresh();
}

static void reset_cb(lv_event_t *e) {
	(void)e;
	confirm_show("musicsettings_reset_the_equaliser", "musicsettings_eq_reset_confirm_note", "reset", reset_action, NULL);
}

static void presets_cb(lv_event_t *e) {
	(void)e;
	eqsettings_open();
}

// The plot's picture exists only while the page is on screen.
static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	if (!eq.buf) {
		eq.buf = lv_draw_buf_create((uint32_t)eq.plot_w, (uint32_t)eq.plot_h, LV_COLOR_FORMAT_ARGB8888,
									LV_STRIDE_AUTO);
		if (eq.buf) {
			lv_image_set_src(eq.plot, eq.buf);
		}
	}
	eqpage_refresh();
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	if (eq.buf) {
		lv_image_set_src(eq.plot, NULL);
		lv_image_cache_drop(eq.buf);
		lv_draw_buf_destroy(eq.buf);
		eq.buf = NULL;
	}
}

static void theme_changed(void) {
	for (int i = 0; i < 3; i++) {
		lv_obj_set_style_bg_color(eq.band_btns[i], theme()->surface_pressed, 0);
	}
	eqpage_refresh();
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

static lv_obj_t *corner_button(int slot, const lv_image_dsc_t *glyph, lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(eq.screen);
	lv_obj_set_size(button, 56, 56);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -eq.cfg->padding - slot * (56 + 6),
				 eq.cfg->padding + eq.cfg->top_bar_height);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);
	return button;
}

static lv_obj_t *small_label(lv_obj_t *parent, const lv_font_t *font) {
	lv_obj_t *l = lv_label_create(parent);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_add_style(l, &theme_style_text_dim, 0);
	lv_obj_set_clickable(l, false);
	return l;
}

static lv_obj_t *card_create(lv_obj_t *parent) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_scrollable(card, false);
	return card;
}

static void build_board(lv_obj_t *container) {
	eq.card = card_create(container);
	lv_obj_set_height(eq.card, BOARD_HEIGHT + 2 * CARD_PAD);
	lv_obj_set_style_pad_all(eq.card, CARD_PAD, 0);

	eq.board = lv_obj_create(eq.card);
	lv_obj_remove_style_all(eq.board);
	lv_obj_set_size(eq.board, lv_pct(100), BOARD_HEIGHT);
	lv_obj_set_scrollable(eq.board, false);
	lv_obj_set_scroll_chain_hor(eq.board, false);
	lv_obj_set_scroll_chain_ver(eq.board, false);
	lv_obj_set_clickable(eq.board, true);
	lv_obj_add_event_cb(eq.board, board_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(eq.board, board_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(eq.board, board_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(eq.board, board_cb, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_update_layout(eq.card);

	int board_w = lv_obj_get_width(eq.board);
	eq.plot_w = board_w - AXIS_WIDTH - RIGHT_PAD;
	eq.plot_h = BOARD_HEIGHT - TOP_ROW - BOTTOM_ROW;

	eq.plot = lv_image_create(eq.board);
	lv_obj_set_size(eq.plot, eq.plot_w, eq.plot_h);
	lv_obj_set_pos(eq.plot, AXIS_WIDTH, TOP_ROW);
	lv_obj_set_clickable(eq.plot, false);

	// The scale, right-aligned against the plot.
	for (int db = EQ_GAIN_MAX_DB; db >= -EQ_GAIN_MAX_DB; db -= 6) {
		lv_obj_t *l = small_label(eq.board, &font_ui_16);
		char text[8];
		snprintf(text, sizeof(text), db > 0 ? "+%d" : "%d", db);
		lv_label_set_text(l, text);
		lv_obj_set_width(l, AXIS_WIDTH - 8);
		lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
		lv_obj_set_pos(l, 0, value_y(db * 10) - lv_font_get_line_height(&font_ui_16) / 2);
	}

	int col = (int)column_width();
	for (int i = 0; i < EQ_BANDS; i++) {
		eq.values[i] = small_label(eq.board, &font_ui_16);
		lv_obj_set_width(eq.values[i], col + 6);
		lv_obj_set_style_text_align(eq.values[i], LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_pos(eq.values[i], column_x(i) - (col + 6) / 2, 2);

		eq.freqs[i] = small_label(eq.board, &font_ui_16);
		char text[8];
		freq_short(eq_band_freq[i], text, sizeof(text));
		lv_label_set_text(eq.freqs[i], text);
		lv_obj_set_width(eq.freqs[i], col + 6);
		lv_obj_set_style_text_align(eq.freqs[i], LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_pos(eq.freqs[i], column_x(i) - (col + 6) / 2, BOARD_HEIGHT - BOTTOM_ROW + 6);

		lv_obj_t *k = lv_obj_create(eq.board);
		lv_obj_remove_style_all(k);
		lv_obj_set_style_radius(k, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
		lv_obj_set_style_border_opa(k, LV_OPA_COVER, 0);
		lv_obj_set_style_shadow_width(k, 8, 0);
		lv_obj_set_style_shadow_opa(k, LV_OPA_30, 0);
		lv_obj_set_style_shadow_color(k, lv_color_black(), 0);
		lv_obj_set_clickable(k, false);
		lv_obj_set_scrollable(k, false);
		eq.knobs[i] = k;
	}
}

static lv_obj_t *round_button(lv_obj_t *parent, const char *text, const lv_image_dsc_t *glyph, int step) {
	lv_obj_t *b = lv_btn_create(parent);
	lv_obj_set_size(b, 56, 56);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(b, theme()->surface_pressed, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(b, 0, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_set_style_pad_all(b, 0, 0);
	lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
	lv_obj_add_event_cb(b, step_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)step);
	if (step != 0) {
		// Held, it keeps stepping.
		lv_obj_add_event_cb(b, step_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)step);
	}
	if (glyph) {
		lv_obj_t *icon = lv_image_create(b);
		lv_image_set_src(icon, glyph);
		lv_obj_add_style(icon, &theme_style_icon, 0);
		lv_obj_center(icon);
	} else {
		lv_obj_t *l = lv_label_create(b);
		lv_label_set_text(l, text);
		lv_obj_add_style(l, &theme_style_text, 0);
		lv_obj_set_style_text_font(l, &font_ui_32, 0);
		lv_obj_center(l);
	}
	return b;
}

static void build_band_card(lv_obj_t *container) {
	eq.band_card = card_create(container);
	lv_obj_set_height(eq.band_card, 96);
	lv_obj_set_style_pad_hor(eq.band_card, 18, 0);
	lv_obj_set_style_pad_ver(eq.band_card, 0, 0);
	lv_obj_set_style_pad_column(eq.band_card, 10, 0);
	lv_obj_set_flex_flow(eq.band_card, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(eq.band_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *text = lv_obj_create(eq.band_card);
	lv_obj_remove_style_all(text);
	lv_obj_set_flex_grow(text, 1);
	lv_obj_set_height(text, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(text, 2, 0);
	lv_obj_set_clickable(text, false);

	eq.band_title = lv_label_create(text);
	lv_obj_add_style(eq.band_title, &theme_style_text, 0);
	lv_obj_set_style_text_font(eq.band_title, &font_ui_24, 0);
	eq.band_value = lv_label_create(text);
	lv_obj_set_style_text_font(eq.band_value, &font_ui_22, 0);

	eq.band_btns[0] = round_button(eq.band_card, "-", NULL, -EQ_STEP_TENTHS);
	eq.band_btns[1] = round_button(eq.band_card, "+", NULL, EQ_STEP_TENTHS);
	eq.band_btns[2] = round_button(eq.band_card, NULL, &icon_reset, 0);
}

static lv_obj_t *note(lv_obj_t *parent, const char *text) {
	lv_obj_t *l = lv_label_create(parent);
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(l, lv_pct(100));
	lv_obj_add_style(l, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(l, &font_ui_20, 0);
	if (text) {
		lv_label_set_text(l, tr(text));
	}
	return l;
}

void eqpage_init(gui_config_t *cfg) {
	eq.cfg = cfg;
	eq.selected = 5;
	eqsettings_set_reload_cb(eqpage_refresh);

	eq.screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(eq.screen, cfg, "equaliser");
	settingsrow_title_corner_slots(settingsrow_page_title(eq.screen), cfg, 2);

	settingsrow_toggle(container, "on", &eq.enable_sw, enable_cb);

	build_board(container);
	build_band_card(container);

	lv_obj_t *pills = NULL;
	settingsrow_pills(container, "eq_width", &pills);
	static const char *const WIDTH_KEYS[EQ_WIDTHS] = {"eq_width_wide", "eq_width_normal", "eq_width_narrow"};
	for (int i = 0; i < EQ_WIDTHS; i++) {
		eq.width_pills[i] = settingsrow_pill(pills, WIDTH_KEYS[i], i, width_cb);
	}
	note(container, "eq_width_note");

	eq.headroom = note(container, NULL);
	note(container, "eq_page_note");

	corner_button(0, &icon_music_settings, presets_cb);
	eq.reset_btn = corner_button(1, &icon_reset, reset_cb);

	eqpage_refresh();
	lv_obj_add_event_cb(eq.screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(eq.screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(eq.screen);
	theme_register_refresh(theme_changed);
}

lv_obj_t *eqpage_screen(void) { return eq.screen; }
