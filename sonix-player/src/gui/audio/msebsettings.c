#include "msebsettings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/shell/confirm.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/audio/eq.h"
#include "src/system/core/lang.h"

#define PRESET_NAME_MAX 100

static gui_config_t *config;

static lv_obj_t *settings_screen;
static lv_obj_t *range_buttons[3];
static const int RANGE_VALUES[3] = {20, 40, 100};

// The preset list, built fresh each time it is opened.
static lv_obj_t *list_screen;
static lv_obj_t *list_container;
static lv_obj_t *list_empty;

// The name dialog behind "Save preset".
static lv_obj_t *name_layer;
static lv_obj_t *name_field;
static keyboard_t *name_keyboard;

static void (*reload_cb)(void);

// The preset the ellipsis menu is about.
static char menu_name[PRESET_NAME_MAX + 1];

void msebsettings_set_reload_cb(void (*cb)(void)) { reload_cb = cb; }

lv_obj_t *msebsettings_screen(void) { return settings_screen; }

// ---------------------------------------------------------------------------
// the range pills
// ---------------------------------------------------------------------------

static void refresh_range_buttons(void) {
	int active = mseb_get_range();
	for (int i = 0; i < 3; i++) {
		if (!range_buttons[i]) {
			continue;
		}
		bool on = RANGE_VALUES[i] == active;
		lv_obj_set_style_bg_color(range_buttons[i], on ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(range_buttons[i], 0),
									on ? lv_color_white() : theme()->text_primary, 0);
	}
}

static void range_pick_cb(lv_event_t *e) {
	int range = (int)(intptr_t)lv_event_get_user_data(e);
	mseb_set_range(range);
	refresh_range_buttons();
	if (reload_cb) {
		reload_cb(); // the sliders' ends just moved
	}
}

static lv_obj_t *make_range_pill(lv_obj_t *parent, const char *text, int range) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 64);
	// Tight, because "-100 +100" is a wide label and all three have to fit
	// the card without the last one running off its right edge.
	lv_obj_set_style_pad_hor(btn, 10, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0); // the Adwaita pill, like the clock position
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, range_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)range);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, text);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_center(label);

	return btn;
}

// ---------------------------------------------------------------------------
// the preset list
// ---------------------------------------------------------------------------

static void rebuild_preset_list(void);

static void preset_row_delete_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

static void preset_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	const char *name = lv_event_get_user_data(e);
	if (!name) {
		return;
	}

	if (mseb_preset_load(name)) {
		refresh_range_buttons();
		if (reload_cb) {
			reload_cb();
		}
		toast_success("preset_loaded");
		back_btn_cb(NULL);
	} else {
		gui_notify_popup("cannot_load_the_preset");
	}
}

static void do_delete_preset(void *user) {
	(void)user;
	if (menu_name[0] && mseb_preset_delete(menu_name)) {
		rebuild_preset_list();
	}
	menu_name[0] = '\0';
}

static void menu_delete_action(void *user) {
	(void)user;
	if (!menu_name[0]) {
		return;
	}
	char message[PRESET_NAME_MAX + 64];
	snprintf(message, sizeof(message), tr("remove_from_card_confirm_note"), menu_name);
	confirm_show("delete_the_preset", message, "delete", do_delete_preset, NULL);
}

static void preset_menu_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e);
	const char *name = lv_event_get_user_data(e);
	if (!name) {
		return;
	}
	snprintf(menu_name, sizeof(menu_name), "%s", name);

	popover_item_t items[1] = {
		{"delete", menu_delete_action, NULL},
	};
	popover_show(lv_event_get_current_target(e), items, 1);
}

static bool add_preset_row(const char *name, void *user) {
	int *count = user;
	(*count)++;

	char *owned = strdup(name);
	if (!owned) {
		return true;
	}

	lv_obj_t *row = lv_btn_create(list_container);
	lv_obj_set_size(row, lv_pct(100), 84);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, 12, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 16, 0);
	lv_obj_set_style_pad_column(row, 14, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_event_cb(row, preset_clicked_cb, LV_EVENT_CLICKED, owned);
	lv_obj_add_event_cb(row, preset_row_delete_cb, LV_EVENT_DELETE, owned);

	lv_obj_t *icon = lv_image_create(row);
	lv_image_set_src(icon, &icon_equalizer);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);

	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text(label, name);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(label, 1);
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	// Two lines at most, the second ending in an ellipsis.
	lv_obj_set_style_max_height(label,
								2 * lv_font_get_line_height(&font_ui_24) +
									lv_obj_get_style_text_line_space(label, LV_PART_MAIN),
								0);

	lv_obj_t *menu_btn = lv_btn_create(row);
	lv_obj_set_size(menu_btn, 44, 44);
	lv_obj_set_style_bg_opa(menu_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(menu_btn, 0, 0);
	lv_obj_set_style_shadow_width(menu_btn, 0, 0);
	lv_obj_set_style_pad_all(menu_btn, 0, 0);
	lv_obj_add_event_cb(menu_btn, preset_menu_cb, LV_EVENT_CLICKED, owned);

	lv_obj_t *dots = lv_image_create(menu_btn);
	lv_image_set_src(dots, &icon_ellipsis_vertical);
	lv_obj_add_style(dots, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(dots, LV_OPA_COVER, 0);
	lv_obj_center(dots);

	return true;
}

static void rebuild_preset_list(void) {
	lv_obj_clean(list_container);

	int count = 0;
	mseb_preset_for_each(add_preset_row, &count);

	if (count == 0) {
		lv_obj_set_hidden(list_empty, false);
	} else {
		lv_obj_set_hidden(list_empty, true);
	}
}

static void open_preset_list_cb(lv_event_t *e) {
	(void)e;
	rebuild_preset_list();
	switch_screen(list_screen);
}

// ---------------------------------------------------------------------------
// the name dialog behind "Save preset"
// ---------------------------------------------------------------------------

static void name_layer_hide(void) {
	if (name_layer) {
		lv_obj_set_hidden(name_layer, true);
	}
}

static void name_cancel_cb(lv_event_t *e) {
	(void)e;
	name_layer_hide();
}

static void name_accept_cb(lv_event_t *e) {
	(void)e;

	const char *typed = lv_textarea_get_text(name_field);
	char name[PRESET_NAME_MAX + 1];
	snprintf(name, sizeof(name), "%s", typed ? typed : "");

	size_t len = strlen(name);
	while (len > 0 && name[len - 1] == ' ') {
		name[--len] = '\0';
	}
	if (len == 0) {
		gui_notify_popup("name_required");
		return;
	}

	if (!mseb_preset_save(name)) {
		gui_notify_popup("cannot_save_the_preset");
		return;
	}

	name_layer_hide();
	toast_success("preset_saved");
}

static void open_name_dialog_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(name_field, "");
	keyboard_reset(name_keyboard);
	// Nothing ever taps this field (the keys type straight into it), so focus is
	// forced on it; otherwise the caret sits there without blinking.
	lv_obj_add_state(name_field, LV_STATE_FOCUSED);
	lv_obj_send_event(name_field, LV_EVENT_FOCUSED, NULL);
	lv_obj_set_hidden(name_layer, false);
	lv_obj_move_foreground(name_layer);
}

static void build_name_dialog(gui_config_t *cfg) {
	name_layer = lv_obj_create(settings_screen);
	lv_obj_set_size(name_layer, cfg->screen_width, cfg->screen_height);
	lv_obj_align(name_layer, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(name_layer, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(name_layer, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(name_layer, 0, 0);
	lv_obj_set_style_radius(name_layer, 0, 0);
	lv_obj_set_style_pad_all(name_layer, 0, 0);
	lv_obj_set_scrollable(name_layer, false);
	lv_obj_set_hidden(name_layer, true);

	lv_obj_t *heading = lv_label_create(name_layer);
	lv_label_set_text(heading, tr("preset_name"));
	lv_obj_add_style(heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(heading, &font_ui_24, 0);
	lv_obj_align(heading, LV_ALIGN_TOP_LEFT, cfg->padding + 56 + 14, cfg->padding + cfg->top_bar_height + 10);

	lv_obj_t *cancel = lv_btn_create(name_layer);
	lv_obj_set_size(cancel, 56, 56);
	lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cancel, 0, 0);
	lv_obj_set_style_shadow_width(cancel, 0, 0);
	lv_obj_set_style_pad_all(cancel, 0, 0);
	lv_obj_add_event_cb(cancel, name_cancel_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *cancel_icon = lv_image_create(cancel);
	lv_image_set_src(cancel_icon, &icon_close);
	lv_obj_add_style(cancel_icon, &theme_style_icon, 0);
	lv_obj_center(cancel_icon);

	name_field = lv_textarea_create(name_layer);
	lv_textarea_set_one_line(name_field, true);
	lv_textarea_set_max_length(name_field, PRESET_NAME_MAX);
	lv_textarea_set_placeholder_text(name_field, tr("name"));
	lv_obj_set_size(name_field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(name_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(name_field, LV_ALIGN_TOP_LEFT, cfg->padding, cfg->padding + cfg->top_bar_height + 60);
	lv_obj_add_style(name_field, &theme_style_card, 0);
	lv_obj_set_style_radius(name_field, 12, 0);
	lv_obj_set_style_border_width(name_field, 0, 0);
	lv_obj_set_style_shadow_width(name_field, 0, 0);
	lv_obj_set_style_pad_all(name_field, 14, 0);
	lv_obj_set_style_text_font(name_field, &font_ui_24, 0);
	keyboard_style_caret(name_field);

	name_keyboard = keyboard_create(name_layer, cfg->screen_width, 316, name_field, NULL, "ok", name_accept_cb, NULL);
}

// ---------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------

static void build_list_page(gui_config_t *cfg) {
	list_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(list_screen, cfg, "load_preset");
	list_container = container;

	list_empty = lv_label_create(list_screen);
	lv_label_set_text(list_empty, tr("no_saved_presets"));
	lv_obj_set_style_text_align(list_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(list_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(list_empty, &font_ui_24, 0);
	lv_obj_align(list_empty, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(cfg) + 100);
	lv_obj_set_hidden(list_empty, true);

	switcher_attach_back_gesture(list_screen);
}

void msebsettings_init(gui_config_t *cfg) {
	config = cfg;

	settings_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(settings_screen, cfg, "mseb_settings");

	settingsrow_action(container, "save_preset", open_name_dialog_cb, NULL);
	settingsrow_add(container, "load_preset", NULL, open_preset_list_cb, NULL);

	// The slider travel, as pills: the same shape the clock position uses.
	lv_obj_t *card = lv_obj_create(container);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 16, 0);
	lv_obj_set_style_pad_row(card, 12, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *title = lv_label_create(card);
	lv_label_set_text(title, tr("mseb_change_range"));
	lv_obj_add_style(title, &theme_style_text, 0);
	lv_obj_set_style_text_font(title, &font_ui_24, 0);

	lv_obj_t *pills = lv_obj_create(card);
	lv_obj_set_width(pills, lv_pct(100));
	lv_obj_set_height(pills, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(pills, 0, 0);
	lv_obj_set_style_border_width(pills, 0, 0);
	lv_obj_set_style_pad_all(pills, 0, 0);
	lv_obj_set_style_pad_column(pills, 8, 0);
	lv_obj_set_scrollable(pills, false);
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(pills, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	range_buttons[0] = make_range_pill(pills, "-20 +20", 20);
	range_buttons[1] = make_range_pill(pills, "-40 +40", 40);
	range_buttons[2] = make_range_pill(pills, "-100 +100", 100);
	refresh_range_buttons();

	build_list_page(cfg);
	build_name_dialog(cfg);

	switcher_attach_back_gesture(settings_screen);
	theme_register_refresh(refresh_range_buttons);
}
