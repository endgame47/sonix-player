#include "peqsettings.h"

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

// The page behind the PEQ gear: save the parametric chain under a name and
// load it back. Same shape as the MSEB and graphic equalizer settings pages;
// only the contents of a preset differ -- here an Equalizer APO file in the
// card's PEQ folder, which is also where AutoEq's ParametricEQ.txt files go.

#define PRESET_NAME_MAX 100

static gui_config_t *config;

static lv_obj_t *settings_screen;

// The preset list, built fresh each time it is opened.
static lv_obj_t *list_screen;
static lv_obj_t *list_container;
static lv_obj_t *list_empty;

// The name dialog behind "save preset".
static lv_obj_t *name_layer;
static lv_obj_t *name_field;
static keyboard_t *name_keyboard;

static void (*reload_cb)(void);

// The preset the ellipsis menu is about.
static char menu_name[PRESET_NAME_MAX + 1];

void peqsettings_set_reload_cb(void (*cb)(void)) { reload_cb = cb; }

lv_obj_t *peqsettings_screen(void) { return settings_screen; }

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

	int ignored = 0;
	if (peq_preset_load(name, &ignored)) {
		if (reload_cb) {
			reload_cb();
		}
		if (ignored > 0) {
			// Said plainly rather than left for the ear to find: the curve that
			// plays is not the whole of the one in the file.
			char message[256];
			snprintf(message, sizeof(message), tr("peq_preset_loaded_partly"), ignored);
			gui_notify_popup(message);
		} else {
			toast_success("preset_loaded");
		}
		back_btn_cb(NULL);
	} else {
		gui_notify_popup("cannot_load_the_preset");
	}
}

static void do_delete_preset(void *user) {
	(void)user;
	if (menu_name[0] && peq_preset_delete(menu_name)) {
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
	peq_preset_for_each(add_preset_row, &count);

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
// the name dialog behind "save preset"
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

	if (!peq_preset_save(name)) {
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
	// Nothing ever taps this field -- the keys type straight into it -- so the
	// caret is told it has the focus, or it would sit there without blinking.
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
	lv_label_set_text(list_empty, tr("peq_presets_empty_note"));
	lv_label_set_long_mode(list_empty, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(list_empty, cfg->screen_width - 4 * cfg->padding);
	lv_obj_set_style_text_align(list_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(list_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(list_empty, &font_ui_24, 0);
	lv_obj_align(list_empty, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(cfg) + 100);
	lv_obj_set_hidden(list_empty, true);

	switcher_attach_back_gesture(list_screen);
}

void peqsettings_init(gui_config_t *cfg) {
	config = cfg;

	settings_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(settings_screen, cfg, "peq_settings");

	// No factory presets here: the graphic equalizer's canned curves are ten
	// gains, which mean nothing for a parametric chain where a band also has a
	// frequency, a Q and a shape. A parametric preset is a curve someone built,
	// or the one AutoEq built for a pair of headphones.
	settingsrow_action(container, "save_preset", open_name_dialog_cb, NULL);
	settingsrow_add(container, "load_preset", NULL, open_preset_list_cb, NULL);

	build_list_page(cfg);
	build_name_dialog(cfg);

	switcher_attach_back_gesture(settings_screen);
}
