#include "peqpage.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "src/gui/shell/confirm.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/audio/peqsettings.h"
#include "src/gui/audio/peqautoeq.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/audio/eq.h"
#include "src/system/core/lang.h"

// ---------------------------------------------------------------------------
// Parametric equaliser
//
// Two screens. The main one shows the response curve plus the list of
// PEQ_BANDS bands, so the whole filter can be read at a glance. The second
// edits a single band and exists once for all of them: the same screen
// refilled with different numbers, rather than one idle copy per band.
// ---------------------------------------------------------------------------

static lv_obj_t *main_screen;
static lv_obj_t *band_screen;

lv_obj_t *peqpage_screen(void) { return main_screen; }

// ---------------------------------------------------------------------------
// Number formatting
// ---------------------------------------------------------------------------

// "105 Hz", "12,0 kHz". The decimal comma follows the Italian base UI; other
// languages get the separator changed along with the rest of the translation.
static void freq_text(int hz, char *out, size_t size) {
	if (hz < 1000) {
		snprintf(out, size, "%d Hz", hz);
	} else {
		snprintf(out, size, "%d,%d kHz", hz / 1000, (hz % 1000) / 100);
	}
}

// "+4,5 dB", "-12,0 dB", "0,0 dB". The sign is explicit even when positive:
// on an EQ curve, boost versus cut is the first thing read.
static void gain_text(int tenths, char *out, size_t size) {
	int abs_tenths = tenths < 0 ? -tenths : tenths;
	const char *sign = tenths < 0 ? "-" : (tenths > 0 ? "+" : "");
	snprintf(out, size, "%s%d,%d dB", sign, abs_tenths / 10, abs_tenths % 10);
}

static void q_text(int cent, char *out, size_t size) { snprintf(out, size, "%d,%02d", cent / 100, cent % 100); }

// Filter shape names live in the UI, not in the engine: the engine builds
// filters, naming them is presentation. Each tr() call is spelled out
// literally because that is how the translation extractor finds the keys; a
// key built at runtime would be invisible to it.
static const char *type_label(int type) {
	switch (type) {
	case PEQ_TYPE_PEAK:
		return tr("peq_peak");
	case PEQ_TYPE_LOWSHELF:
		return tr("peq_low_shelf");
	case PEQ_TYPE_HIGHSHELF:
		return tr("peq_high_shelf");
	case PEQ_TYPE_LOWPASS:
		return tr("peq_low_pass");
	case PEQ_TYPE_HIGHPASS:
		return tr("peq_high_pass");
	default:
		return "?";
	}
}

// ---------------------------------------------------------------------------
// Sliders: position to value and back
//
// Frequency and Q move logarithmically. On a linear 20..20000 slider the whole
// range below 500 Hz -- where most corrections happen -- would fit in the
// first two percent of travel, unreachable with a finger.
// ---------------------------------------------------------------------------

#define FREQ_STEPS 121 // 120 steps from 20 Hz to 20 kHz: three decades, 40 per decade
#define GAIN_STEPS 61  // -15.0 to +15.0 dB in half-decibel steps
#define Q_STEPS 61	   // 0.10 to 10.00: two decades
#define PREAMP_STEPS 43 // -15.0 to +6.0 dB in half-decibel steps

static int freq_from_index(int index) {
	double hz = (double)PEQ_FREQ_MIN * pow(10.0, 3.0 * (double)index / (double)(FREQ_STEPS - 1));
	int rounded = (int)(hz + 0.5);
	return rounded < PEQ_FREQ_MIN ? PEQ_FREQ_MIN : (rounded > PEQ_FREQ_MAX ? PEQ_FREQ_MAX : rounded);
}

static int index_from_freq(int hz) {
	if (hz <= PEQ_FREQ_MIN) {
		return 0;
	}
	double index = (double)(FREQ_STEPS - 1) * log10((double)hz / (double)PEQ_FREQ_MIN) / 3.0;
	int rounded = (int)(index + 0.5);
	return rounded < 0 ? 0 : (rounded > FREQ_STEPS - 1 ? FREQ_STEPS - 1 : rounded);
}

static int gain_from_index(int index) { return PEQ_GAIN_MIN_TENTHS + index * 5; }
static int index_from_gain(int tenths) { return (tenths - PEQ_GAIN_MIN_TENTHS) / 5; }

static int q_from_index(int index) {
	double q = (double)PEQ_Q_MIN * pow(10.0, 2.0 * (double)index / (double)(Q_STEPS - 1));
	int rounded = (int)(q + 0.5);
	return rounded < PEQ_Q_MIN ? PEQ_Q_MIN : (rounded > PEQ_Q_MAX ? PEQ_Q_MAX : rounded);
}

static int index_from_q(int cent) {
	if (cent <= PEQ_Q_MIN) {
		return 0;
	}
	double index = (double)(Q_STEPS - 1) * log10((double)cent / (double)PEQ_Q_MIN) / 2.0;
	int rounded = (int)(index + 0.5);
	return rounded < 0 ? 0 : (rounded > Q_STEPS - 1 ? Q_STEPS - 1 : rounded);
}

static int preamp_from_index(int index) { return PEQ_PREAMP_MIN_TENTHS + index * 5; }
static int index_from_preamp(int tenths) { return (tenths - PEQ_PREAMP_MIN_TENTHS) / 5; }

// ---------------------------------------------------------------------------
// Response graph
//
// Sixty-one points from 20 Hz to 20 kHz, spaced by decade like any response
// plot. The grid then lands on useful values for free: four vertical lines
// over three decades fall exactly on 20, 200, 2k and 20k, and five horizontal
// lines over a symmetric scale put the middle one on zero.
//
// The plot rate is fixed at 48 kHz, the rate most material plays at. A curve
// that reshaped itself per track would be describing the file, not the filter.
// ---------------------------------------------------------------------------

#define GRAPH_POINTS 61
#define GRAPH_RATE 48000
#define GRAPH_RANGE_TENTHS 180 // +/- 18 dB, covering everything the bands can ask for

static lv_obj_t *graph;
static lv_chart_series_t *graph_series;
static int32_t graph_values[GRAPH_POINTS];

static int graph_freq(int index) {
	double hz = (double)PEQ_FREQ_MIN * pow(10.0, 3.0 * (double)index / (double)(GRAPH_POINTS - 1));
	return (int)(hz + 0.5);
}

static void graph_refresh(void) {
	if (!graph) {
		return;
	}
	for (int i = 0; i < GRAPH_POINTS; i++) {
		int db = peq_response_tenths(graph_freq(i), GRAPH_RATE);
		if (db > GRAPH_RANGE_TENTHS) {
			db = GRAPH_RANGE_TENTHS;
		}
		if (db < -GRAPH_RANGE_TENTHS) {
			db = -GRAPH_RANGE_TENTHS;
		}
		graph_values[i] = db;
	}
	lv_chart_refresh(graph);
}

static void graph_paint_theme(void) {
	if (!graph) {
		return;
	}
	lv_obj_set_style_bg_color(graph, theme()->surface_pressed, 0);
	lv_obj_set_style_line_color(graph, theme()->text_secondary, LV_PART_MAIN);
	if (graph_series) {
		lv_chart_set_series_color(graph, graph_series, theme()->accent);
	}
}

static void build_graph(lv_obj_t *parent) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, 236);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 14, 0);
	lv_obj_set_scrollable(card, false);
	// Not clickable, so the back gesture can start on top of the graph.
	lv_obj_set_clickable(card, false);

	graph = lv_chart_create(card);
	lv_obj_set_size(graph, lv_pct(100), 168);
	lv_obj_align(graph, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_set_clickable(graph, false);
	lv_obj_set_style_border_width(graph, 0, 0);
	lv_obj_set_style_radius(graph, 8, 0);
	lv_obj_set_style_pad_all(graph, 6, 0);
	lv_chart_set_type(graph, LV_CHART_TYPE_LINE);
	lv_chart_set_point_count(graph, GRAPH_POINTS);
	lv_chart_set_range(graph, LV_CHART_AXIS_PRIMARY_Y, -GRAPH_RANGE_TENTHS, GRAPH_RANGE_TENTHS);
	lv_chart_set_div_line_count(graph, 5, 4);
	// No point markers: with sixty-one points the bare line reads better.
	lv_obj_set_style_size(graph, 0, 0, LV_PART_INDICATOR);
	lv_obj_set_style_line_width(graph, 3, LV_PART_ITEMS);

	graph_series = lv_chart_add_series(graph, theme()->accent, LV_CHART_AXIS_PRIMARY_Y);
	lv_chart_set_ext_y_array(graph, graph_series, graph_values);

	// Axis labels under the vertical grid lines. Laid out by a flex row rather
	// than at computed pixel offsets, since the card width depends on the page
	// padding.
	lv_obj_t *marks = lv_obj_create(card);
	lv_obj_set_size(marks, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_align(marks, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_bg_opa(marks, 0, 0);
	lv_obj_set_style_border_width(marks, 0, 0);
	lv_obj_set_style_pad_all(marks, 0, 0);
	lv_obj_set_scrollable(marks, false);
	lv_obj_set_clickable(marks, false);
	lv_obj_set_flex_flow(marks, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(marks, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	static const char *const MARKS[] = {"20", "200", "2k", "20k"};
	for (int i = 0; i < 4; i++) {
		lv_obj_t *mark = lv_label_create(marks);
		lv_label_set_text(mark, MARKS[i]);
		lv_obj_add_style(mark, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(mark, &font_ui_18, 0);
	}

	graph_paint_theme();
	graph_refresh();
}

// ---------------------------------------------------------------------------
// Single band screen
// ---------------------------------------------------------------------------

static int editing = -1; // band currently open in the edit screen, -1 if none

static lv_obj_t *band_title;
static lv_obj_t *band_on_switch;
static lv_obj_t *type_pills[PEQ_TYPE_COUNT];
static lv_obj_t *freq_slider, *freq_value;
static lv_obj_t *gain_slider, *gain_value;
static lv_obj_t *gain_card;
static lv_obj_t *q_slider, *q_value;

static void refresh_main_rows(void);
static void refresh_preamp(void);

// Gain is meaningless on a low-pass or high-pass, so its card is hidden
// instead of sitting there accepting changes that do nothing.
static bool type_has_gain(int type) {
	return type == PEQ_TYPE_PEAK || type == PEQ_TYPE_LOWSHELF || type == PEQ_TYPE_HIGHSHELF;
}

static void paint_pill(lv_obj_t *btn, bool on) {
	if (!btn) {
		return;
	}
	lv_obj_set_style_bg_color(btn, on ? theme()->accent : theme()->surface_pressed, 0);
	lv_obj_set_style_text_color(lv_obj_get_child(btn, 0), on ? lv_color_white() : theme()->text_primary, 0);
}

static void band_page_refresh(void) {
	if (editing < 0) {
		return;
	}

	peq_band_t b;
	peq_get_band(editing, &b);

	lv_label_set_text_fmt(band_title, tr("peq_band"), editing + 1);

	if (b.on) {
		lv_obj_add_state(band_on_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(band_on_switch, LV_STATE_CHECKED);
	}

	for (int t = 0; t < PEQ_TYPE_COUNT; t++) {
		paint_pill(type_pills[t], t == b.type);
	}

	char text[32];

	lv_slider_set_value(freq_slider, index_from_freq(b.freq), LV_ANIM_OFF);
	freq_text(b.freq, text, sizeof(text));
	lv_label_set_text(freq_value, text);

	lv_slider_set_value(gain_slider, index_from_gain(b.gain_tenths), LV_ANIM_OFF);
	gain_text(b.gain_tenths, text, sizeof(text));
	lv_label_set_text(gain_value, text);
	if (type_has_gain(b.type)) {
		lv_obj_set_hidden(gain_card, false);
	} else {
		lv_obj_set_hidden(gain_card, true);
	}

	lv_slider_set_value(q_slider, index_from_q(b.q_cent), LV_ANIM_OFF);
	q_text(b.q_cent, text, sizeof(text));
	lv_label_set_text(q_value, text);
}

// Every edit goes through here: read the band, change one field, write it
// back whole. Writing the full struct rather than a single field keeps the
// engine seeing one already-validated state at a time.
static void band_commit(void (*change)(peq_band_t *b, int value), int value) {
	if (editing < 0) {
		return;
	}
	peq_band_t b;
	peq_get_band(editing, &b);
	change(&b, value);
	peq_set_band(editing, &b);

	band_page_refresh();
	graph_refresh();
	refresh_main_rows();
	refresh_preamp(); // automatic headroom depends on the band just changed
}

static void set_on(peq_band_t *b, int value) { b->on = value != 0; }
static void set_type(peq_band_t *b, int value) { b->type = value; }
static void set_freq(peq_band_t *b, int value) { b->freq = freq_from_index(value); }
static void set_gain(peq_band_t *b, int value) { b->gain_tenths = gain_from_index(value); }
static void set_q(peq_band_t *b, int value) { b->q_cent = q_from_index(value); }

static void band_on_cb(lv_event_t *e) {
	(void)e;
	band_commit(set_on, lv_obj_has_state(band_on_switch, LV_STATE_CHECKED) ? 1 : 0);
}

static void type_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	band_commit(set_type, (int)(intptr_t)lv_event_get_user_data(e));
}

static void freq_cb(lv_event_t *e) {
	(void)e;
	band_commit(set_freq, (int)lv_slider_get_value(freq_slider));
}

static void gain_cb(lv_event_t *e) {
	(void)e;
	band_commit(set_gain, (int)lv_slider_get_value(gain_slider));
}

static void q_cb(lv_event_t *e) {
	(void)e;
	band_commit(set_q, (int)lv_slider_get_value(q_slider));
}

// ---------------------------------------------------------------------------
// Typing a value
//
// Tapping the gain or the Q over its slider opens a keypad, the way the stock
// player does it, and what is typed goes into the band as it is, clamped to
// the band's limits: the sliders move in steps of 0,5 dB and of a few
// hundredths of Q.
// ---------------------------------------------------------------------------

typedef enum { ENTRY_GAIN, ENTRY_Q } entry_kind_t;

#define ENTRY_MAX_CHARS 7

static lv_obj_t *entry_layer;
static lv_obj_t *entry_heading;
static lv_obj_t *entry_display;
static lv_obj_t *entry_sign_btn;
static lv_obj_t *entry_ok_btn;
static entry_kind_t entry_kind;
static char entry_text[16]; // digits, one comma, a leading minus
static bool entry_fresh;	// the shown value is the old one: the first key replaces it

static void entry_paint(void) {
	char shown[32];
	const char *unit = entry_kind == ENTRY_GAIN ? " dB" : "";
	snprintf(shown, sizeof(shown), "%s%s", entry_text[0] ? entry_text : "0", unit);
	lv_label_set_text(entry_display, shown);
	lv_obj_set_style_text_color(entry_display, entry_fresh ? theme()->text_secondary : theme()->text_primary, 0);
}

// Tenths or hundredths out of "12,5" or "-3": the number times 10^decimals,
// rounded on the last place kept.
static int entry_parse_scaled(const char *text, int decimals) {
	bool negative = text[0] == '-';
	const char *p = negative ? text + 1 : text;
	long whole = 0, frac = 0;
	int frac_digits = 0;
	bool after_comma = false;
	int round_digit = 0;
	for (; *p; p++) {
		if (*p == ',') {
			after_comma = true;
		} else if (!after_comma) {
			whole = whole * 10 + (*p - '0');
		} else if (frac_digits < decimals) {
			frac = frac * 10 + (*p - '0');
			frac_digits++;
		} else if (frac_digits == decimals && round_digit == 0) {
			round_digit = *p - '0';
			frac_digits++;
		}
	}
	for (int i = frac_digits < decimals ? frac_digits : decimals; i < decimals; i++) {
		frac *= 10;
	}
	long scale = 1;
	for (int i = 0; i < decimals; i++) {
		scale *= 10;
	}
	long value = whole * scale + frac + (round_digit >= 5 ? 1 : 0);
	if (value > 1000000) {
		value = 1000000;
	}
	return (int)(negative ? -value : value);
}

// The keypad's own X is its way out: the back arrow goes while it is up.
static void entry_hide(void) {
	lv_obj_set_hidden(entry_layer, true);
	back_btn_hide_on_screen(band_screen, false);
}

static void entry_cancel_cb(lv_event_t *e) {
	(void)e;
	entry_hide();
}

static void entry_set_gain(peq_band_t *b, int value) { b->gain_tenths = value; }
static void entry_set_q(peq_band_t *b, int value) { b->q_cent = value; }

static void entry_accept_cb(lv_event_t *e) {
	(void)e;
	if (!entry_text[0] || strcmp(entry_text, "-") == 0) {
		entry_hide();
		return;
	}
	int value;
	switch (entry_kind) {
	case ENTRY_GAIN:
		value = entry_parse_scaled(entry_text, 1);
		value = value < PEQ_GAIN_MIN_TENTHS ? PEQ_GAIN_MIN_TENTHS : (value > PEQ_GAIN_MAX_TENTHS ? PEQ_GAIN_MAX_TENTHS : value);
		band_commit(entry_set_gain, value);
		break;
	case ENTRY_Q:
		value = entry_parse_scaled(entry_text, 2);
		value = value < PEQ_Q_MIN ? PEQ_Q_MIN : (value > PEQ_Q_MAX ? PEQ_Q_MAX : value);
		band_commit(entry_set_q, value);
		break;
	}
	entry_hide();
}

// One key: a digit, ',' or '-' (which flips the sign rather than typing one).
static void entry_key_cb(lv_event_t *e) {
	char key = (char)(intptr_t)lv_event_get_user_data(e);
	if (entry_fresh) {
		// The old value is there to be read, not edited: the first digit starts
		// a new number, keeping only its sign for the gain. The sign key flips
		// the old value and leaves it fresh, so a +4,5 that should have been
		// -4,5 is one tap, and ± then 3 is -3.
		if (key == '-') {
			size_t len = strlen(entry_text);
			if (entry_text[0] == '-') {
				memmove(entry_text, entry_text + 1, len);
			} else if (len < sizeof(entry_text) - 1) {
				memmove(entry_text + 1, entry_text, len + 1);
				entry_text[0] = '-';
			}
			entry_paint();
			return;
		}
		bool negative = entry_text[0] == '-';
		entry_text[0] = '\0';
		if (negative && entry_kind == ENTRY_GAIN) {
			snprintf(entry_text, sizeof(entry_text), "-");
		}
		entry_fresh = false;
	}

	size_t len = strlen(entry_text);
	if (key == '-') {
		if (entry_text[0] == '-') {
			memmove(entry_text, entry_text + 1, len);
		} else if (len < sizeof(entry_text) - 1) {
			memmove(entry_text + 1, entry_text, len + 1);
			entry_text[0] = '-';
		}
	} else if (key == ',') {
		if (!strchr(entry_text, ',') && len < ENTRY_MAX_CHARS) {
			if (len == 0 || (len == 1 && entry_text[0] == '-')) {
				strcat(entry_text, "0");
			}
			strcat(entry_text, ",");
		}
	} else if (len < ENTRY_MAX_CHARS) {
		// No run of leading zeros: "0" followed by a digit becomes the digit.
		const char *digits = entry_text[0] == '-' ? entry_text + 1 : entry_text;
		if (strcmp(digits, "0") == 0) {
			entry_text[len - 1] = key;
		} else {
			entry_text[len] = key;
			entry_text[len + 1] = '\0';
		}
	}
	entry_paint();
}

static void entry_delete_cb(lv_event_t *e) {
	(void)e;
	if (entry_fresh) {
		entry_text[0] = '\0';
		entry_fresh = false;
	} else {
		size_t len = strlen(entry_text);
		if (len) {
			entry_text[len - 1] = '\0';
		}
	}
	entry_paint();
}

static void entry_open(entry_kind_t kind) {
	if (editing < 0 || switcher_back_drag_active()) {
		return;
	}
	peq_band_t b;
	peq_get_band(editing, &b);

	entry_kind = kind;
	switch (kind) {
	case ENTRY_GAIN: {
		lv_label_set_text(entry_heading, tr("gain"));
		int t = b.gain_tenths < 0 ? -b.gain_tenths : b.gain_tenths;
		snprintf(entry_text, sizeof(entry_text), "%s%d,%d", b.gain_tenths < 0 ? "-" : "", t / 10, t % 10);
		break;
	}
	case ENTRY_Q:
		lv_label_set_text(entry_heading, "Q");
		snprintf(entry_text, sizeof(entry_text), "%d,%02d", b.q_cent / 100, b.q_cent % 100);
		break;
	}
	entry_fresh = true;

	// Only the gain has a sign.
	lv_obj_set_hidden(entry_sign_btn, kind != ENTRY_GAIN);
	// OK takes the whole last row, or what the sign key leaves of it.
	lv_obj_set_width(entry_ok_btn, kind == ENTRY_GAIN ? lv_pct(66) : lv_pct(100));

	entry_paint();
	lv_obj_set_hidden(entry_layer, false);
	lv_obj_move_foreground(entry_layer);
	back_btn_hide_on_screen(band_screen, true);
}

// Leaving the band page by the back arrow or the gesture closes the keypad
// with it, rather than having it greet the next band opened.
static void entry_screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	entry_hide();
}

static void entry_open_cb(lv_event_t *e) { entry_open((entry_kind_t)(intptr_t)lv_event_get_user_data(e)); }

static lv_obj_t *entry_key(lv_obj_t *parent, const char *text, const lv_image_dsc_t *icon, lv_event_cb_t cb, intptr_t user, int width_pct) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, lv_pct(width_pct), lv_pct(18));
	lv_obj_add_style(btn, &theme_style_card, 0);
	lv_obj_add_style(btn, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(btn, 14, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)user);
	if (icon) {
		lv_obj_t *image = lv_image_create(btn);
		lv_image_set_src(image, icon);
		lv_obj_add_style(image, &theme_style_icon, 0);
		lv_obj_center(image);
	} else {
		lv_obj_t *label = lv_label_create(btn);
		lv_label_set_text(label, text);
		lv_obj_add_style(label, &theme_style_text, 0);
		lv_obj_set_style_text_font(label, &font_ui_32, 0);
		lv_obj_center(label);
	}
	return btn;
}

static void build_entry_layer(gui_config_t *cfg) {
	entry_layer = lv_obj_create(band_screen);
	lv_obj_set_size(entry_layer, cfg->screen_width, cfg->screen_height);
	lv_obj_align(entry_layer, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(entry_layer, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(entry_layer, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(entry_layer, 0, 0);
	lv_obj_set_style_radius(entry_layer, 0, 0);
	lv_obj_set_style_pad_all(entry_layer, 0, 0);
	lv_obj_set_scrollable(entry_layer, false);
	lv_obj_set_hidden(entry_layer, true);
	lv_obj_add_event_cb(band_screen, entry_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	int top = cfg->padding + cfg->top_bar_height;

	entry_heading = lv_label_create(entry_layer);
	lv_obj_add_style(entry_heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(entry_heading, &font_ui_24, 0);
	lv_obj_align(entry_heading, LV_ALIGN_TOP_LEFT, cfg->padding + 6, top + 10);

	lv_obj_t *cancel = lv_btn_create(entry_layer);
	lv_obj_set_size(cancel, 56, 56);
	lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -cfg->padding, top);
	lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cancel, 0, 0);
	lv_obj_set_style_shadow_width(cancel, 0, 0);
	lv_obj_set_style_pad_all(cancel, 0, 0);
	lv_obj_add_event_cb(cancel, entry_cancel_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *cancel_icon = lv_image_create(cancel);
	lv_image_set_src(cancel_icon, &icon_close);
	lv_obj_add_style(cancel_icon, &theme_style_icon, 0);
	lv_obj_center(cancel_icon);

	lv_obj_t *field = lv_obj_create(entry_layer);
	lv_obj_set_size(field, cfg->screen_width - 2 * cfg->padding, 84);
	lv_obj_align(field, LV_ALIGN_TOP_LEFT, cfg->padding, top + 66);
	lv_obj_add_style(field, &theme_style_card, 0);
	lv_obj_set_style_radius(field, 12, 0);
	lv_obj_set_style_border_width(field, 0, 0);
	lv_obj_set_style_shadow_width(field, 0, 0);
	lv_obj_set_scrollable(field, false);
	entry_display = lv_label_create(field);
	lv_obj_set_style_text_font(entry_display, &font_ui_36_bold, 0);
	lv_obj_align(entry_display, LV_ALIGN_RIGHT_MID, -10, 0);

	int pad_top = top + 66 + 84 + 16;
	lv_obj_t *pad = lv_obj_create(entry_layer);
	lv_obj_set_size(pad, cfg->screen_width - 2 * cfg->padding, cfg->screen_height - pad_top - cfg->padding);
	lv_obj_align(pad, LV_ALIGN_TOP_LEFT, cfg->padding, pad_top);
	lv_obj_set_style_bg_opa(pad, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(pad, 0, 0);
	lv_obj_set_style_pad_all(pad, 0, 0);
	lv_obj_set_style_pad_row(pad, 10, 0);
	lv_obj_set_style_pad_column(pad, 10, 0);
	lv_obj_set_scrollable(pad, false);
	lv_obj_set_flex_flow(pad, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(pad, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	static const char *const DIGITS[9] = {"1", "2", "3", "4", "5", "6", "7", "8", "9"};
	for (int i = 0; i < 9; i++) {
		entry_key(pad, DIGITS[i], NULL, entry_key_cb, '1' + i, 31);
	}
	entry_key(pad, ",", NULL, entry_key_cb, ',', 31);
	entry_key(pad, "0", NULL, entry_key_cb, '0', 31);
	lv_obj_t *del = entry_key(pad, NULL, &icon_delete, entry_delete_cb, 0, 31);
	lv_obj_add_event_cb(del, entry_delete_cb, LV_EVENT_LONG_PRESSED_REPEAT, NULL);

	entry_sign_btn = entry_key(pad, "+/-", NULL, entry_key_cb, '-', 31);
	entry_ok_btn = entry_key(pad, tr("ok"), NULL, entry_accept_cb, 0, 100);
	lv_obj_add_style(entry_ok_btn, &theme_style_accent_bg, 0);
	lv_obj_set_style_text_color(lv_obj_get_child(entry_ok_btn, 0), lv_color_white(), 0);
}

// The number over a slider becomes the way into the keypad: set on a rounded
// box of the pressed-card colour, with a margin round it a finger can find.
static void entry_attach(lv_obj_t *value_label, entry_kind_t kind) {
	lv_obj_add_style(value_label, &theme_style_card_pressed, 0);
	lv_obj_set_style_radius(value_label, 10, 0);
	lv_obj_set_style_pad_hor(value_label, 12, 0);
	lv_obj_set_style_pad_ver(value_label, 4, 0);
	lv_obj_set_clickable(value_label, true);
	lv_obj_set_ext_click_area(value_label, 20);
	lv_obj_add_event_cb(value_label, entry_open_cb, LV_EVENT_CLICKED, (void *)(intptr_t)kind);
}

static lv_obj_t *make_pill(lv_obj_t *parent, const char *text, int value) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 52);
	lv_obj_set_style_pad_hor(btn, 20, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, type_cb, LV_EVENT_CLICKED, (void *)(intptr_t)value);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, text); // already translated by the caller
	lv_obj_set_style_text_font(label, &font_ui_20, 0);
	lv_obj_center(label);
	return btn;
}

// The round top-right buttons shared with the Music, graphic EQ and MSEB
// screens. Slot 0 is the corner itself, slot 1 sits to its left.
static lv_obj_t *corner_button(lv_obj_t *screen, gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph,
							   lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(screen);
	lv_obj_set_size(button, 56, 56);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * (56 + 6), cfg->padding + cfg->top_bar_height);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);

	return button;
}

// Reset of a single band. The main screen already resets them all, but one
// botched band is redone far more often than the whole curve.
static void do_reset_band(void *user) {
	(void)user;
	if (editing < 0) {
		return;
	}
	peq_reset_band(editing);
	band_page_refresh();
	graph_refresh();
	refresh_main_rows();
	refresh_preamp();
}

static void reset_band_cb(lv_event_t *e) {
	(void)e;
	if (editing < 0) {
		return;
	}
	char title[64];
	snprintf(title, sizeof(title), tr("peq_band_reset_confirm"), editing + 1);
	confirm_show(title, "peq_band_reset_confirm_note", "reset", do_reset_band,
				 NULL);
}

static void build_band_page(gui_config_t *cfg) {
	band_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(band_screen, cfg, "peq_band");
	band_title = settingsrow_page_title(band_screen);
	settingsrow_title_corner_slots(band_title, cfg, 1);
	corner_button(band_screen, cfg, 0, &icon_reset, reset_band_cb);

	settingsrow_toggle(container, "peq_on_2", &band_on_switch, band_on_cb);

	// Shapes as pills rather than a list: only five, mutually exclusive, and
	// seeing the alternatives is half of making the choice.
	lv_obj_t *shape_card = lv_obj_create(container);
	lv_obj_set_width(shape_card, lv_pct(100));
	lv_obj_set_height(shape_card, LV_SIZE_CONTENT);
	lv_obj_add_style(shape_card, &theme_style_card, 0);
	lv_obj_set_style_radius(shape_card, 12, 0);
	lv_obj_set_style_border_width(shape_card, 0, 0);
	lv_obj_set_style_shadow_width(shape_card, 0, 0);
	lv_obj_set_style_pad_all(shape_card, 18, 0);
	lv_obj_set_style_pad_row(shape_card, 14, 0);
	lv_obj_set_scrollable(shape_card, false);
	lv_obj_set_flex_flow(shape_card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(shape_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *shape_name = lv_label_create(shape_card);
	lv_label_set_text(shape_name, tr("peq_shape"));
	lv_obj_add_style(shape_name, &theme_style_text, 0);
	lv_obj_set_style_text_font(shape_name, &font_ui_24, 0);

	lv_obj_t *pills = lv_obj_create(shape_card);
	lv_obj_set_size(pills, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(pills, 0, 0);
	lv_obj_set_style_border_width(pills, 0, 0);
	lv_obj_set_style_pad_all(pills, 0, 0);
	lv_obj_set_style_pad_gap(pills, 10, 0);
	lv_obj_set_scrollable(pills, false);
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(pills, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	for (int t = 0; t < PEQ_TYPE_COUNT; t++) {
		type_pills[t] = make_pill(pills, type_label(t), t);
	}

	settingsrow_slider(container, "peq_frequency", FREQ_STEPS, &freq_value, &freq_slider, freq_cb);
	gain_card = settingsrow_slider(container, "gain", GAIN_STEPS, &gain_value, &gain_slider, gain_cb);
	settingsrow_slider(container, "Q", Q_STEPS, &q_value, &q_slider, q_cb);

	build_entry_layer(cfg);
	entry_attach(gain_value, ENTRY_GAIN);
	entry_attach(q_value, ENTRY_Q);

	switcher_attach_back_gesture(band_screen);
}

// ---------------------------------------------------------------------------
// Main screen
// ---------------------------------------------------------------------------

static lv_obj_t *enable_switch;
static lv_obj_t *reset_btn;
static lv_obj_t *autoeq_btn;
static lv_timer_t *autoeq_timer;
static lv_obj_t *preamp_slider, *preamp_value;
static lv_obj_t *headroom_label;
static lv_obj_t *band_rows[PEQ_BANDS];
static lv_obj_t *band_values[PEQ_BANDS];

// One-line band summary for the list: where it acts and by how much. Pass
// filters have no gain, so the shape name replaces it -- printing "0,0 dB"
// there would look like a reading rather than an absence.
static void band_summary(const peq_band_t *b, char *out, size_t size) {
	if (!b->on) {
		snprintf(out, size, "%s", tr("peq_off"));
		return;
	}

	char freq[24];
	freq_text(b->freq, freq, sizeof(freq));

	if (!type_has_gain(b->type)) {
		snprintf(out, size, "%s  %s", type_label(b->type), freq);
		return;
	}

	char gain[24];
	gain_text(b->gain_tenths, gain, sizeof(gain));
	snprintf(out, size, "%s  %s", freq, gain);
}

static void refresh_main_rows(void) {
	for (int i = 0; i < PEQ_BANDS; i++) {
		if (!band_rows[i] || !band_values[i]) {
			continue;
		}
		peq_band_t b;
		peq_get_band(i, &b);

		// The name is rewritten on every refresh, not only at build time: the
		// translation file holds "Band %d", so the formatted "Band 3" on screen
		// is not a key the language switch can retranslate on its own.
		lv_obj_t *name = lv_obj_get_child(band_rows[i], 0);
		if (name) {
			lv_label_set_text_fmt(name, tr("peq_band"), i + 1);
		}

		char text[64];
		band_summary(&b, text, sizeof(text));
		lv_label_set_text(band_values[i], text);
	}
}

static void refresh_preamp(void) {
	if (!preamp_slider) {
		return;
	}
	int tenths = peq_get_preamp();
	lv_slider_set_value(preamp_slider, index_from_preamp(tenths), LV_ANIM_OFF);

	char text[24];
	gain_text(tenths, text, sizeof(text));
	lv_label_set_text(preamp_value, text);

	// What the engine does to the level on its own, which is not always a cut:
	// MSEB hands level back when its curve takes more away than it adds, so
	// this can read either way. Shown only when non-zero, since a "0,0 dB" line
	// is noise.
	if (headroom_label) {
		int headroom = eq_auto_headroom_tenths();
		if (headroom == 0) {
			lv_obj_set_hidden(headroom_label, true);
		} else {
			char cut[24];
			gain_text(headroom, cut, sizeof(cut));
			lv_label_set_text_fmt(headroom_label, tr("peq_headroom_auto"), cut);
			lv_obj_set_hidden(headroom_label, false);
		}
	}
}

// Everything on the page except the switch itself follows the switch: the
// reset, the preamp and the band rows go dead and half-lit while the
// parametric is off, the way the graphic equaliser and MSEB do it.
static void apply_enabled(bool on) {
	if (reset_btn) {
		if (on) {
			lv_obj_remove_state(reset_btn, LV_STATE_DISABLED);
			lv_obj_set_clickable(reset_btn, true);
			lv_obj_set_style_opa(reset_btn, LV_OPA_COVER, 0);
		} else {
			lv_obj_add_state(reset_btn, LV_STATE_DISABLED);
			lv_obj_set_clickable(reset_btn, false);
			lv_obj_set_style_opa(reset_btn, LV_OPA_40, 0);
		}
	}

	if (preamp_slider) {
		if (on) {
			lv_obj_remove_state(preamp_slider, LV_STATE_DISABLED);
		} else {
			lv_obj_add_state(preamp_slider, LV_STATE_DISABLED);
		}
	}

	for (int i = 0; i < PEQ_BANDS; i++) {
		if (!band_rows[i]) {
			continue;
		}
		if (on) {
			lv_obj_set_clickable(band_rows[i], true);
		} else {
			lv_obj_set_clickable(band_rows[i], false);
		}
		lv_obj_set_style_opa(band_rows[i], LV_OPA_COVER, 0);

		uint32_t children = lv_obj_get_child_count(band_rows[i]);
		for (uint32_t c = 0; c < children; c++) {
			lv_obj_set_style_opa(lv_obj_get_child(band_rows[i], c), on ? LV_OPA_COVER : LV_OPA_50, 0);
		}
	}
}

static void enable_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(enable_switch, LV_STATE_CHECKED);
	peq_set_enabled(on);
	apply_enabled(on);
	refresh_preamp();
	graph_refresh();
}

static void preamp_cb(lv_event_t *e) {
	(void)e;
	peq_set_preamp(preamp_from_index((int)lv_slider_get_value(preamp_slider)));
	refresh_preamp();
	graph_refresh();
}

static void band_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	editing = (int)(intptr_t)lv_event_get_user_data(e);
	band_page_refresh();
	switch_screen(band_screen);
}

static void do_reset(void *user) {
	(void)user;
	peq_reset();

	refresh_preamp();
	refresh_main_rows();
	graph_refresh();
}

static void reset_cb(lv_event_t *e) {
	(void)e;
	// The band count is substituted into the translated sentence from
	// PEQ_BANDS, so the two cannot drift apart.
	char message[160];
	snprintf(message, sizeof(message), tr("peq_reset_all_confirm_note"), PEQ_BANDS);
	confirm_show("peq_reset_the_parametric_equaliser", message, "reset", do_reset, NULL);
}

static void open_settings_cb(lv_event_t *e) {
	(void)e;
	switch_screen(peqsettings_screen());
}

static void open_autoeq_cb(lv_event_t *e) {
	(void)e;
	if (peqautoeq_network_needed()) {
		return;
	}
	peqautoeq_open();
}

// The AutoEq button at half strength without a network; a tap on it then only
// says why.
static void autoeq_btn_refresh(void) {
	if (autoeq_btn) {
		lv_obj_set_style_opa(autoeq_btn, peqautoeq_network_ready() ? LV_OPA_COVER : LV_OPA_40, 0);
	}
}

// Wi-Fi can come and go while the page is open.
static void autoeq_timer_cb(lv_timer_t *timer) {
	if (lv_screen_active() != main_screen) {
		lv_timer_pause(timer);
		return;
	}
	autoeq_btn_refresh();
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;

	// The switch as well, and not only at build time: the parametric can also
	// be turned on from the control centre, so the page must show the engine's
	// state on every load.
	if (enable_switch) {
		if (peq_get_enabled()) {
			lv_obj_add_state(enable_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(enable_switch, LV_STATE_CHECKED);
		}
	}

	apply_enabled(peq_get_enabled());
	refresh_main_rows();
	refresh_preamp();
	graph_refresh();

	autoeq_btn_refresh();
	if (autoeq_timer) {
		lv_timer_resume(autoeq_timer);
	}
}

// Called after a preset is loaded: the band rows, the preamp, the graph and,
// if a band screen is open, that screen too.
static void reload_after_preset(void) {
	refresh_main_rows();
	refresh_preamp();
	graph_refresh();
	band_page_refresh();
}

static void refresh_theme(void) {
	graph_paint_theme();
	if (editing >= 0) {
		peq_band_t b;
		peq_get_band(editing, &b);
		for (int t = 0; t < PEQ_TYPE_COUNT; t++) {
			paint_pill(type_pills[t], t == b.type);
		}
	} else {
		for (int t = 0; t < PEQ_TYPE_COUNT; t++) {
			paint_pill(type_pills[t], false);
		}
	}
}

void peqpage_init(gui_config_t *cfg) {
	main_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(main_screen, cfg, "peq");

	// Reset, AutoEq search and the preset gear share the title's corner slots.
	settingsrow_title_corner_slots(settingsrow_page_title(main_screen), cfg, 3);
	reset_btn = corner_button(main_screen, cfg, 0, &icon_reset, reset_cb);
	corner_button(main_screen, cfg, 1, &icon_music_settings, open_settings_cb);
	autoeq_btn = corner_button(main_screen, cfg, 2, &icon_audio_waveform, open_autoeq_cb);
	autoeq_timer = lv_timer_create(autoeq_timer_cb, 1000, NULL);
	lv_timer_pause(autoeq_timer);

	peqsettings_set_reload_cb(reload_after_preset);
	peqautoeq_set_reload_cb(reload_after_preset);

	settingsrow_toggle(container, "on", &enable_switch, enable_cb);
	if (peq_get_enabled()) {
		lv_obj_add_state(enable_switch, LV_STATE_CHECKED);
	}

	build_graph(container);

	settingsrow_slider(container, "peq_preamp", PREAMP_STEPS, &preamp_value, &preamp_slider, preamp_cb);

	// A dead slider looks dead, the same half opacity the graphic equaliser and
	// MSEB use for theirs.
	lv_obj_set_style_opa(preamp_slider, LV_OPA_40, LV_STATE_DISABLED);

	// The automatic anti-clipping cut, placed under the preamp because it is
	// the same quantity: one chosen by the listener, one by the engine, and
	// they add up.
	headroom_label = lv_label_create(container);
	lv_label_set_long_mode(headroom_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(headroom_label, lv_pct(100));
	lv_obj_add_style(headroom_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(headroom_label, &font_ui_18, 0);
	lv_label_set_text(headroom_label, "");
	lv_obj_set_hidden(headroom_label, true);

	for (int i = 0; i < PEQ_BANDS; i++) {
		band_rows[i] = settingsrow_add(container, "peq_band", &band_values[i], band_clicked_cb, (void *)(intptr_t)i);
	}

	build_band_page(cfg);

	apply_enabled(peq_get_enabled());

	refresh_preamp();
	refresh_main_rows();
	graph_refresh();

	lv_obj_add_event_cb(main_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	// The same reading when the control centre switches the equaliser with the
	// page open under it.
	lv_obj_add_event_cb(main_screen, screen_loaded_cb, LV_EVENT_REFRESH, NULL);
	switcher_attach_back_gesture(main_screen);
	theme_register_refresh(refresh_theme);
}
