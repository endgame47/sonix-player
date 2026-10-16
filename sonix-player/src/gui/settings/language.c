#include "language.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/setupwifi.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/settings/timeset.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/input/kblayout.h"

#include <stdio.h>
#include <string.h>

lv_obj_t *language_screen;

// However many .ini files the folder holds, up to the 16 lang_list() reads.
#define MAX_LANGUAGES 16

// The same green the settings rows use to mark a page whose feature is on.
#define CHECK_GREEN lv_color_make(46, 194, 126)

// One entry, drawn twice: once in the settings page and once in the first-boot
// panel. Both lists exist from startup, so both have to be repainted when the
// choice changes.
typedef struct {
	char name[64];	// the file name, which is what lang_set() wants
	char label[64]; // the same, made readable: see lang_display_name()
	lv_obj_t *page_check;
	lv_obj_t *panel_check;
} entry_t;

static entry_t entries[MAX_LANGUAGES];
static int entry_count;

static lv_obj_t *panel;
static lv_obj_t *panel_title;
static lv_obj_t *page_title;
static lv_obj_t *menu_label; // the language row on the settings page

// The word for "language" in the language now in use, followed by the English
// one. Somebody who has just set the player to a language they cannot read has
// to be able to find the way back, and "Language" is the word they look for. In
// English the two would be the same word twice, so only one is shown.
static void language_label(char *out, size_t size) {
	const char *own = tr("language");
	if (strcmp(own, "Language") == 0) {
		snprintf(out, size, "%s", own);
	} else {
		snprintf(out, size, "%s/Language", own);
	}
}

// The three places that name is shown. Not left to the general re-labelling
// pass, which only recognises whole strings present in the language file: this
// label is two of them joined together and is not in there.
static void refresh_titles(void) {
	char text[128];
	language_label(text, sizeof(text));

	if (menu_label) {
		lv_label_set_text(menu_label, text);
	}
	if (page_title) {
		lv_label_set_text(page_title, text);
	}
	if (panel_title) {
		lv_label_set_text(panel_title, text);
	}
}

void language_bind_menu_row(lv_obj_t *row) {
	if (!row) {
		return;
	}
	// The row's name is the first label in it; the value and the chevron come
	// after, and this row has neither.
	uint32_t count = lv_obj_get_child_count(row);
	for (uint32_t i = 0; i < count; i++) {
		lv_obj_t *child = lv_obj_get_child(row, (int32_t)i);
		if (lv_obj_check_type(child, &lv_label_class)) {
			menu_label = child;
			break;
		}
	}
	refresh_titles();
}

static void paint_checks(void) {
	for (int i = 0; i < entry_count; i++) {
		bool on = strcmp(entries[i].name, lang_current()) == 0;
		lv_obj_t *const marks[] = {entries[i].page_check, entries[i].panel_check};
		for (int k = 0; k < 2; k++) {
			if (!marks[k]) {
				continue;
			}
			if (on) {
				lv_obj_set_hidden(marks[k], false);
			} else {
				lv_obj_set_hidden(marks[k], true);
			}
		}
	}
}

static void pick_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= entry_count) {
		return;
	}

	// lang_set() re-labels every page that is already built, so the change
	// shows immediately -- including on this page, whose own heading is a
	// label like any other.
	fonts_set_language(entries[index].name);
	if (!lang_set(entries[index].name)) {
		fonts_set_language(lang_current());
	} else {
		kblayout_language_changed();
		keyboard_refresh_layout();
	}
	paint_checks();
	refresh_titles();
}

// A row that marks itself instead of leading somewhere. settingsrow_action
// gives the row without the chevron (a chevron would promise a page that does
// not exist), and the tick goes where the chevron would have been, so a
// language that is chosen and one that is not sit on the same grid.
static lv_obj_t *make_row(lv_obj_t *parent, const char *name, int index) {
	lv_obj_t *row = settingsrow_action(parent, name, pick_cb, (void *)(intptr_t)index);

	lv_obj_t *mark = lv_image_create(row);
	lv_image_set_src(mark, &icon_check);
	lv_obj_set_style_image_recolor(mark, CHECK_GREEN, 0);
	lv_obj_set_style_image_recolor_opa(mark, LV_OPA_COVER, 0);
	lv_obj_align(mark, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_hidden(mark, true);
	return mark;
}

// ---------------------------------------------------------------------------
// the first-boot panel
// ---------------------------------------------------------------------------

static void confirm_cb(lv_event_t *e) {
	(void)e;

	// Whatever is on the list now is already applied -- picking a row applies
	// it there and then -- so this only has to write down that the question
	// has been answered, and get out of the way.
	config_set("ui", "language", lang_current());
	config_save();

	lv_obj_set_hidden(panel, true);
	power_hold_screen_on(false);
	power_notify_activity();

	// And on to the clock, which is the other thing a player that has never
	// run cannot work out for itself: by way of the Wi-Fi, so that the
	// network can tell it.
	if (timeset_needed()) {
		setupwifi_show();
	}
}

static void build_panel(gui_config_t *cfg) {
	panel = lv_obj_create(lv_layer_top());
	lv_obj_set_size(panel, cfg->screen_width, cfg->screen_height);
	lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(panel, &theme_style_screen, 0);
	lv_obj_set_style_border_width(panel, 0, 0);
	lv_obj_set_style_radius(panel, 0, 0);
	lv_obj_set_style_pad_all(panel, cfg->padding, 0);
	lv_obj_set_scrollable(panel, false);
	lv_obj_set_hidden(panel, true);
	lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
	// Down from the top rather than centred: with five languages the column is
	// tall enough that centring would slide it up over the heading. The date
	// panel can centre because two cards and a button leave room to; this one
	// does not.
	lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_gap(panel, 18, 0);

	// Where the status bar would be, if this panel did not cover it.
	lv_obj_set_style_pad_top(panel, cfg->top_bar_height + 10, 0);

	// The heading is the first item in the column rather than pinned out of it,
	// the way the date panel does it: a pinned label is positioned from inside
	// the panel's padding, so it would slide down with the very padding meant
	// to clear the status bar and end up behind the list.
	panel_title = lv_label_create(panel);
	lv_label_set_text(panel_title, "Lingua/Language"); // replaced by refresh_titles()
	lv_obj_add_style(panel_title, &theme_style_text, 0);
	lv_obj_set_style_text_font(panel_title, &font_ui_32, 0);

	// The list takes whatever is left between the heading and the button, and
	// scrolls inside it. Sized to its content instead, five languages already
	// push the button off the bottom of a 720 px panel -- and the point of
	// reading the folder rather than a table in here is that there is no fixed
	// number of them.
	lv_obj_t *list = lv_obj_create(panel);
	lv_obj_set_width(list, lv_pct(100));
	lv_obj_set_flex_grow(list, 1);
	lv_obj_set_style_bg_opa(list, 0, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_style_pad_all(list, 0, 0);
	lv_obj_set_style_pad_row(list, 8, 0);
	lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	for (int i = 0; i < entry_count; i++) {
		entries[i].panel_check = make_row(list, entries[i].label, i);
	}

	lv_obj_t *confirm = lv_btn_create(panel);
	lv_obj_set_size(confirm, 228, 64);
	lv_obj_set_style_radius(confirm, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(confirm, theme()->accent, 0);
	lv_obj_set_style_shadow_width(confirm, 0, 0);
	lv_obj_set_style_border_width(confirm, 0, 0);
	lv_obj_add_event_cb(confirm, confirm_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *confirm_label = lv_label_create(confirm);
	lv_label_set_text(confirm_label, tr("confirm"));
	lv_obj_set_style_text_font(confirm_label, &font_ui_24, 0);
	lv_obj_set_style_text_color(confirm_label, lv_color_white(), 0);
	lv_obj_center(confirm_label);
}

bool language_needed(void) {
	// Not "is the language Italian": Italian is also what someone chooses. The
	// question is whether anybody has answered at all, so the absence of the
	// line in the settings is what counts.
	return config_get("ui", "language", NULL) == NULL;
}

void language_show_first_boot(void) {
	if (!panel) {
		return;
	}
	paint_checks();
	lv_obj_set_hidden(panel, false);
	lv_obj_move_foreground(panel);

	// Reading five languages takes as long as it takes, and blanking a panel
	// on the top layer also kills the touchscreen.
	power_hold_screen_on(true);
}

void language_init(gui_config_t *cfg) {
	// The list comes from the folder, not from a table in here: dropping
	// another .ini next to the shipped ones is all it takes to add a language.
	const char *names[MAX_LANGUAGES];
	int found = lang_list(names, MAX_LANGUAGES);
	for (int i = 0; i < found && i < MAX_LANGUAGES; i++) {
		snprintf(entries[i].name, sizeof(entries[0].name), "%s", names[i]);
		// The files the stock firmware ships are named like
		// `Fran#U00e7ais.ini`: the file name stays as it is, what the page
		// shows does not.
		lang_display_name(entries[i].name, entries[i].label, sizeof(entries[0].label));
		entry_count = i + 1;
	}

	lv_obj_t *container = settingsrow_page(language_screen, cfg, "language");
	page_title = settingsrow_page_title(language_screen);
	for (int i = 0; i < entry_count; i++) {
		entries[i].page_check = make_row(container, entries[i].label, i);
	}

	build_panel(cfg);

	paint_checks();
	refresh_titles();
	theme_register_refresh(paint_checks);
}
