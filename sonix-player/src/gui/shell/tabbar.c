#include "tabbar.h"

#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

#define TAB_HEIGHT 46
#define TROUGH_PAD 4
#define TAB_PAD 10

// Each tab as wide as its name plus an equal share of the width left over, so a
// long name in a language that has one is not cut to fit a third while the
// short ones sit in empty space. When the names do not fit at all, each gives
// up width in proportion and is shortened with an ellipsis.
static void layout(tabbar_t *t) {
	if (t->count == 0) {
		return;
	}
	int natural[TABBAR_MAX];
	int sum = 0;
	for (int i = 0; i < t->count; i++) {
		lv_label_set_long_mode(t->labels[i], LV_LABEL_LONG_CLIP);
		lv_obj_set_width(t->labels[i], LV_SIZE_CONTENT);
	}
	lv_obj_update_layout(t->bar);
	for (int i = 0; i < t->count; i++) {
		natural[i] = lv_obj_get_width(t->labels[i]) + 2 * TAB_PAD;
		sum += natural[i];
	}
	int inner = lv_obj_get_content_width(t->bar) - TROUGH_PAD * (t->count - 1);
	int extra = (inner - sum) / t->count;
	for (int i = 0; i < t->count; i++) {
		int width = extra >= 0 ? natural[i] + extra : natural[i] * inner / sum;
		lv_obj_set_width(t->tabs[i], width);
		if (extra < 0) {
			lv_label_set_long_mode(t->labels[i], LV_LABEL_LONG_DOT);
			lv_obj_set_width(t->labels[i], width - 2 * TAB_PAD);
		}
	}
}

lv_obj_t *tabbar_create(tabbar_t *t, lv_obj_t *parent) {
	t->count = 0;
	t->active = -1;
	t->bar = lv_obj_create(parent);
	lv_obj_remove_style_all(t->bar);
	lv_obj_set_size(t->bar, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_add_style(t->bar, &theme_style_card, 0);
	lv_obj_set_style_radius(t->bar, 14, 0);
	lv_obj_set_style_border_width(t->bar, 0, 0);
	lv_obj_set_style_shadow_width(t->bar, 0, 0);
	lv_obj_set_style_pad_all(t->bar, TROUGH_PAD, 0);
	lv_obj_set_style_pad_column(t->bar, TROUGH_PAD, 0);
	lv_obj_set_scrollable(t->bar, false);
	// Presses between the tabs belong to the page under it, which carries the
	// swipe back and the player sheet.
	lv_obj_set_event_bubble(t->bar, true);
	lv_obj_set_flex_flow(t->bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(t->bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	return t->bar;
}

lv_obj_t *tabbar_add(tabbar_t *t, const char *text, lv_event_cb_t cb, void *user) {
	if (t->count >= TABBAR_MAX) {
		return NULL;
	}
	lv_obj_t *tab = lv_btn_create(t->bar);
	lv_obj_set_size(tab, LV_SIZE_CONTENT, TAB_HEIGHT);
	lv_obj_set_style_radius(tab, 10, 0);
	lv_obj_set_style_shadow_width(tab, 0, 0);
	lv_obj_set_style_border_width(tab, 0, 0);
	lv_obj_set_style_pad_hor(tab, TAB_PAD, 0);
	lv_obj_set_style_pad_ver(tab, 0, 0);
	lv_obj_set_event_bubble(tab, true);
	if (cb) {
		lv_obj_add_event_cb(tab, cb, LV_EVENT_CLICKED, user);
	}

	lv_obj_t *label = lv_label_create(tab);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_20, 0);
	lv_obj_center(label);

	t->tabs[t->count] = tab;
	t->labels[t->count] = label;
	t->count++;
	tabbar_set_active(t, t->active);
	layout(t);
	return tab;
}

void tabbar_set_active(tabbar_t *t, int index) {
	t->active = index;
	for (int i = 0; i < t->count; i++) {
		bool on = i == index;
		lv_obj_set_style_bg_opa(t->tabs[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		lv_obj_set_style_bg_color(t->tabs[i], theme()->accent, 0);
		// Pressed, an idle tab darkens as a row would.
		lv_obj_set_style_bg_opa(t->tabs[i], LV_OPA_COVER, LV_STATE_PRESSED);
		lv_obj_set_style_bg_color(t->tabs[i], on ? theme()->accent : theme()->surface_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_text_color(t->labels[i], on ? lv_color_white() : theme()->text_primary, 0);
		bool off = lv_obj_has_state(t->tabs[i], LV_STATE_DISABLED);
		lv_obj_set_style_opa(t->tabs[i], off ? LV_OPA_40 : LV_OPA_COVER, 0);
	}
}

void tabbar_set_text(tabbar_t *t, int index, const char *text) {
	if (index < 0 || index >= t->count) {
		return;
	}
	if (strcmp(lv_label_get_text(t->labels[index]), text) == 0) {
		return;
	}
	lv_label_set_text(t->labels[index], text);
	layout(t);
}

void tabbar_set_enabled(tabbar_t *t, int index, bool enabled) {
	if (index < 0 || index >= t->count) {
		return;
	}
	if (enabled) {
		lv_obj_remove_state(t->tabs[index], LV_STATE_DISABLED);
	} else {
		lv_obj_add_state(t->tabs[index], LV_STATE_DISABLED);
	}
	tabbar_set_active(t, t->active);
}
