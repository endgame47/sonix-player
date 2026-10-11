#include "timeset.h"

#include <stdio.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/system/device/clock.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"

#define TIMESET_YEAR_FIRST 2025
#define TIMESET_YEAR_LAST 2044

// The chevron and the heading are laid out to the same numbers the rest of the
// interface uses (settingsrow.c, switcher.c): one 56 px button in the corner
// and the title beside it, not above it.
#define TIMESET_HEADER_BTN 56

// The panel sits on the top layer so it covers the status bar too: until the
// clock is set there is nothing useful up there anyway.
static lv_obj_t *panel;
static lv_obj_t *header_row; // the chevron and the title, on one row
static lv_obj_t *title_label;
static lv_obj_t *roller_day;
static lv_obj_t *roller_month;
static lv_obj_t *roller_year;
static lv_obj_t *roller_hour;
static lv_obj_t *roller_minute;
static lv_obj_t *roller_ampm; // only in the 12-hour format
static lv_obj_t *confirm_btn;
static lv_obj_t *h24_switch;
static lv_obj_t *tz_value;	// the label at the right of the "Time zone" row
static lv_obj_t *dst_pills[2]; // 0 = standard time, 1 = daylight saving

// The right one lit, the other not: same shape as the quality and theme pills,
// so it reads as a choice between two things rather than switching one on.
static void dst_paint(void) {
	bool on = clock_dst_enabled();
	for (int i = 0; i < 2; i++) {
		if (!dst_pills[i]) {
			continue;
		}
		bool active = (i == 1) == on;
		lv_obj_set_style_bg_color(dst_pills[i], active ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_t *label = lv_obj_get_child(dst_pills[i], 0);
		if (label) {
			lv_obj_set_style_text_color(label, active ? lv_color_white() : theme()->text_primary, 0);
		}
	}
}

static void seed_rollers_from_clock(void);

static void dst_pick_cb(lv_event_t *e) {
	bool want = (int)(intptr_t)lv_event_get_user_data(e) == 1;
	if (want == clock_dst_enabled()) {
		return;
	}
	clock_set_dst(want);
	dst_paint();
	// Changing daylight saving changes what time it is now, so the rollers are
	// reread from the clock, exactly as for the zone itself.
	seed_rollers_from_clock();
}
static lv_obj_t *tz_panel;	// the zone list, a panel laid over this one
static lv_obj_t *tz_list;

// Built once and kept: LVGL keeps a pointer to the option string unless asked
// to copy it, and these are big enough not to want a second copy.
static char days_options[4 * 31];
static char years_options[6 * (TIMESET_YEAR_LAST - TIMESET_YEAR_FIRST + 1)];
static char hours_options[4 * 24];
static char hours12_options[4 * 12];
static char minutes_options[4 * 60];

// AM and PM come out the same in every language shipped here: they are Latin
// abbreviations, not words to translate.
static const char *const AMPM_OPTIONS = "timeset_am_pm";

static const char *const MONTHS = "timeset_month_names";

static void fill_numbers(char *out, size_t out_size, int first, int last, const char *format) {
	size_t used = 0;
	for (int value = first; value <= last; value++) {
		used += (size_t)snprintf(out + used, out_size - used, format, value);
		if (value != last) {
			used += (size_t)snprintf(out + used, out_size - used, "\n");
		}
	}
}

// Adwaita-style roller: quiet dimmed digits on the card, the chosen row on a
// soft rounded highlight with the accent colour doing the talking -- no
// saturated selection bar. Kept apart because these colours have to be
// reapplied on a theme or accent change, not only at build time.
static void paint_roller(lv_obj_t *roller) {
	lv_obj_set_style_text_color(roller, theme()->text_secondary, 0);
	lv_obj_set_style_bg_color(roller, theme()->surface_pressed, LV_PART_SELECTED);
	lv_obj_set_style_text_color(roller, theme()->accent, LV_PART_SELECTED);
}

static lv_obj_t *make_roller(lv_obj_t *parent, const char *options, int width) {
	lv_obj_t *roller = lv_roller_create(parent);
	lv_roller_set_options(roller, tr(options), LV_ROLLER_MODE_NORMAL);
	lv_obj_set_width(roller, width);

	lv_obj_set_style_bg_opa(roller, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(roller, 0, 0);
	lv_obj_set_style_shadow_width(roller, 0, 0);
	lv_obj_set_style_text_font(roller, &font_ui_24, 0);
	lv_obj_set_style_text_line_space(roller, 14, 0);

	lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, LV_PART_SELECTED);
	lv_obj_set_style_radius(roller, 10, LV_PART_SELECTED);
	lv_obj_set_style_text_font(roller, &font_ui_24_bold, LV_PART_SELECTED);

	paint_roller(roller);

	// After the font and the line spacing, not before.
	//
	// lv_roller_set_visible_row_count() does not store the row count: it works
	// out a height there and then, as
	//
	//     (current font's line height + line spacing) * rows
	//
	// Called first it would use LVGL's default font height with zero line
	// spacing, giving a container too short for three 24 px rows and clipping
	// the rows above and below the selected one.
	lv_roller_set_visible_row_count(roller, 3);

	return roller;
}

// ---------------------------------------------------------------------------
// 12 or 24 hours
//
// There are two rollers (hours and minutes) and a third, AM/PM, appears only
// when needed. The alternative -- twenty-four entries reading "1 AM ... 12 PM"
// -- makes a very wide, unreadable column, and no system does it that way.
// ---------------------------------------------------------------------------

// The hour chosen on the rollers, always 0 to 23 however it is written.
static int selected_hour24(void) {
	int index = (int)lv_roller_get_selected(roller_hour);
	if (clock_use_24h()) {
		return index;
	}
	// In the 12-hour roller index 0 is "12", which stands for hour 0; the other
	// indices are already the hour.
	int hour = index % 12;
	if (roller_ampm && lv_roller_get_selected(roller_ampm) == 1) {
		hour += 12;
	}
	return hour;
}

// Puts an hour from 0 to 23 back on the rollers, in the format now in use.
static void show_hour24(int hour) {
	if (hour < 0 || hour > 23) {
		hour = 0;
	}
	if (clock_use_24h()) {
		lv_roller_set_selected(roller_hour, (uint32_t)hour, LV_ANIM_OFF);
		return;
	}
	lv_roller_set_selected(roller_hour, (uint32_t)(hour % 12), LV_ANIM_OFF);
	if (roller_ampm) {
		lv_roller_set_selected(roller_ampm, hour < 12 ? 0 : 1, LV_ANIM_OFF);
	}
}

static void apply_time_format(int hour24) {
	if (!roller_hour) {
		return;
	}

	bool h24 = clock_use_24h();
	lv_roller_set_options(roller_hour, h24 ? hours_options : hours12_options, LV_ROLLER_MODE_NORMAL);
	if (roller_ampm) {
		if (h24) {
			lv_obj_set_hidden(roller_ampm, true);
		} else {
			lv_obj_set_hidden(roller_ampm, false);
		}
	}

	// lv_roller_set_options() rebuilds the content and with it the height, so
	// the row count has to be set again or the hour roller is as tall as all its
	// entries. The font and line spacing are already set -- the order is what
	// matters, see make_roller().
	lv_roller_set_visible_row_count(roller_hour, 3);

	show_hour24(hour24);
}

static void h24_toggle_cb(lv_event_t *e) {
	(void)e;
	// Read first: a moment later the same rollers mean something else.
	int hour = selected_hour24();
	clock_set_use_24h(lv_obj_has_state(h24_switch, LV_STATE_CHECKED));
	apply_time_format(hour);
	topbar_refresh_clock(); // the bar above changes along with the rollers
}

// ---------------------------------------------------------------------------
// The time zone
//
// Stated by the user rather than inferred: the RTC keeps UTC, and nothing on
// the device can work out the offset to apply to it without being told. The
// entries are fixed offsets, as the original firmware treats them; daylight
// saving is a separate choice below the list.

// `places` is the tag of the cities beside the offset, not the cities
// themselves: a zone list that says "Londra, Lisbona" to somebody reading the
// player in German is the one page of the settings still in Italian, and there
// are thirty-eight of them. The offset beside it is built from the number and
// stays as it is -- UTC+01:00 is UTC+01:00 in every language.
//
// The tags are picked up from this table by tools/extract_strings.py (the
// TABLES list there), because a static initialiser cannot call tr().
typedef struct {
	int minutes;
	const char *places;
	char season;
} timezone_t;


static const timezone_t TIMEZONES[] = {
	{-12 * 60, "timeset_tz_baker_island"},
	{-11 * 60, "timeset_tz_pago_pago"},
	{-10 * 60, "timeset_tz_honolulu"},
	{-9 * 60 - 30, "timeset_tz_marquesas"},
	{-9 * 60, "timeset_tz_anchorage"},
	{-8 * 60, "timeset_tz_los_angeles"},
	{-7 * 60, "timeset_tz_denver"},
	{-6 * 60, "timeset_tz_chicago"},
	{-5 * 60, "timeset_tz_new_york"},
	{-4 * 60, "timeset_tz_halifax"},
	{-3 * 60 - 30, "timeset_tz_newfoundland"},
	{-3 * 60, "timeset_tz_sao_paulo"},
	{-2 * 60, "timeset_tz_noronha"},
	{-1 * 60, "timeset_tz_azores"},
	{0, "timeset_tz_london"},
	{1 * 60, "timeset_tz_rome"},
	{2 * 60, "timeset_tz_athens"},
	{3 * 60, "timeset_tz_moscow"},
	{3 * 60 + 30, "timeset_tz_tehran"},
	{4 * 60, "timeset_tz_dubai"},
	{4 * 60 + 30, "timeset_tz_kabul"},
	{5 * 60, "timeset_tz_karachi"},
	{5 * 60 + 30, "timeset_tz_new_delhi"},
	{5 * 60 + 45, "timeset_tz_kathmandu"},
	{6 * 60, "timeset_tz_dhaka"},
	{6 * 60 + 30, "timeset_tz_yangon"},
	{7 * 60, "timeset_tz_bangkok"},
	{8 * 60, "timeset_tz_beijing"},
	{8 * 60 + 45, "timeset_tz_eucla"},
	{9 * 60, "timeset_tz_tokyo"},
	{9 * 60 + 30, "timeset_tz_adelaide"},
	{10 * 60, "timeset_tz_sydney"},
	{10 * 60 + 30, "timeset_tz_lord_howe"},
	{11 * 60, "timeset_tz_noumea"},
	{12 * 60, "timeset_tz_auckland"},
	{12 * 60 + 45, "timeset_tz_chatham"},
	{13 * 60, "timeset_tz_samoa"},
	{14 * 60, "timeset_tz_kiritimati"},
};

#define TIMEZONE_COUNT ((int)(sizeof(TIMEZONES) / sizeof(TIMEZONES[0])))

// The green every list in this interface uses to mark the chosen entry.
#define TZ_CHECK_GREEN lv_color_make(46, 194, 126)

static lv_obj_t *tz_checks[TIMEZONE_COUNT];

// "UTC+01:00". Always with a sign and always two digits: lined up in a column,
// the entries can be scanned by eye instead of read one by one. Real offsets
// stop at 14 hours; the hours are kept to two digits so the text always fits
// in the 16-byte buffers the callers pass.
static void tz_format_offset(int minutes, char *out, size_t size) {
	unsigned abs_min = minutes < 0 ? 0u - (unsigned)minutes : (unsigned)minutes;
	unsigned hours = (abs_min / 60) % 100;
	snprintf(out, size, "UTC%c%02u:%02u", minutes < 0 ? '-' : '+', hours, abs_min % 60);
}

// Which list entry is the one in use, or -1 until someone has chosen: on the
// first boot the zone is unknown, and ticking London would state something the
// user never said.
static int tz_selected_index(void) {
	if (!clock_utc_offset_known()) {
		return -1;
	}
	int minutes = clock_utc_offset_minutes();
	for (int i = 0; i < TIMEZONE_COUNT; i++) {
		if (TIMEZONES[i].minutes == minutes) {
			return i;
		}
	}
	return -1;
}

static void seed_rollers_from_clock(void);

static void tz_paint(void) {
	int selected = tz_selected_index();

	for (int i = 0; i < TIMEZONE_COUNT; i++) {
		if (!tz_checks[i]) {
			continue;
		}
		if (i == selected) {
			lv_obj_set_hidden(tz_checks[i], false);
		} else {
			lv_obj_set_hidden(tz_checks[i], true);
		}
	}

	if (tz_value) {
		if (selected < 0) {
			lv_label_set_text(tz_value, tr("timeset_not_set"));
		} else {
			char text[16];
			tz_format_offset(TIMEZONES[selected].minutes, text, sizeof(text));
			lv_label_set_text(tz_value, text);
		}
	}
}

static void tz_close(void) {
	if (tz_panel) {
		lv_obj_set_hidden(tz_panel, true);
	}
	if (panel) {
		lv_obj_move_foreground(panel);
	}
	// The same condition the way in has, and for the same reason: on the first
	// boot the bar is not hidden, only covered by the panels built after it, so
	// an unguarded call to bring it forward puts it back on top for good.
	// Choosing a zone goes through here, since tz_pick_cb closes the list.
	if (!timeset_needed()) {
		topbar_bring_to_front();
	}
}

static void tz_back_cb(lv_event_t *e) {
	(void)e;
	tz_close();
}

static void tz_pick_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= TIMEZONE_COUNT) {
		return;
	}

	clock_set_utc_offset_minutes(TIMEZONES[index].minutes);
	tz_paint();

	// Changing the zone changes what time it is now, so the rollers are reread
	// from the clock; otherwise confirming would write the old time under the
	// new offset.
	seed_rollers_from_clock();
	topbar_refresh_clock();

	tz_close();
}

static void tz_open_cb(lv_event_t *e) {
	(void)e;
	if (!tz_panel) {
		return;
	}
	tz_paint();
	lv_obj_set_hidden(tz_panel, false);
	lv_obj_move_foreground(tz_panel);

	// The status bar goes back in front of the panel just brought forward.
	//
	// This panel lives on the top layer, which is where the bar lives too, so
	// the lv_obj_move_foreground() above puts it in front of that whole layer,
	// bar included. The room is already reserved -- build_timezone_panel()'s
	// pad_top leaves the bar's height -- the bar is merely covered.
	//
	// Only when opened from the settings. On the first boot the bar stays
	// behind, for the same reason it does on Date and time: there is no clock to
	// show yet, and that is exactly what is being asked for (see
	// timeset_show()).
	if (!timeset_needed()) {
		topbar_bring_to_front();
	}

	// The list opens on the chosen entry, not at the top: with thirty-eight
	// zones, scrolling from Baker Island to find one's own is a test of
	// patience.
	int selected = tz_selected_index();
	if (selected >= 0 && tz_list) {
		lv_obj_t *row = lv_obj_get_child(tz_list, selected);
		if (row) {
			lv_obj_scroll_to_view(row, LV_ANIM_OFF);
		}
	} else if (tz_list) {
		lv_obj_scroll_to_y(tz_list, 0, LV_ANIM_OFF);
	}
}

// The card the pickers sit in: one boxed group, libadwaita-style, instead of
// five free-floating rollers.
static lv_obj_t *make_card(lv_obj_t *parent) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 14, 0);
	lv_obj_set_style_pad_gap(card, 10, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	return card;
}

static void confirm_cb(lv_event_t *e) {
	(void)e;

	int day = (int)lv_roller_get_selected(roller_day) + 1;
	int month = (int)lv_roller_get_selected(roller_month) + 1;
	int year = (int)lv_roller_get_selected(roller_year) + TIMESET_YEAR_FIRST;
	int hour = selected_hour24();
	int minute = (int)lv_roller_get_selected(roller_minute);

	clock_set(year, month, day, hour, minute);
	topbar_refresh_clock();

	power_hold_screen_on(false);
	power_notify_activity();

	lv_obj_set_hidden(panel, true);
	if (tz_panel) {
		lv_obj_set_hidden(tz_panel, true);
	}
}

// The back chevron only exists when the page was opened from the settings:
// on a first boot there is nothing sensible to go back to, and the clock has
// to be set before anything else can be trusted.
static lv_obj_t *cancel_btn;

static void cancel_cb(lv_event_t *e) {
	(void)e;
	// Nothing is applied: the rollers were only ever a proposal, and the
	// clock still says what it said. Half a turn of a wheel by mistake must
	// not become the device's new time.
	power_hold_screen_on(false);
	power_notify_activity();
	lv_obj_set_hidden(panel, true);
	if (tz_panel) {
		lv_obj_set_hidden(tz_panel, true);
	}
}

bool timeset_needed(void) { return !clock_is_set(); }

// Puts the five rollers back on what the clock now says. Needed when the page
// opens -- correcting the time by ten minutes must not mean dialling in the
// date as well -- and after a zone change, which moves exactly what the rollers
// were showing.
static void seed_rollers_from_clock(void) {
	if (!roller_day) {
		return;
	}

	time_t now = time(NULL);
	struct tm local;
	localtime_r(&now, &local);

	int year = local.tm_year + 1900;
	if (year < TIMESET_YEAR_FIRST || year > TIMESET_YEAR_LAST) {
		year = TIMESET_YEAR_FIRST;
		local.tm_mon = 0;
		local.tm_mday = 1;
		local.tm_hour = 12;
		local.tm_min = 0;
	}

	lv_roller_set_selected(roller_day, (uint32_t)(local.tm_mday - 1), LV_ANIM_OFF);
	lv_roller_set_selected(roller_month, (uint32_t)local.tm_mon, LV_ANIM_OFF);
	lv_roller_set_selected(roller_year, (uint32_t)(year - TIMESET_YEAR_FIRST), LV_ANIM_OFF);
	lv_roller_set_selected(roller_minute, (uint32_t)local.tm_min, LV_ANIM_OFF);

	apply_time_format(local.tm_hour);
}

void timeset_show(void) {
	if (!panel) {
		return;
	}

	seed_rollers_from_clock();
	tz_paint();

	// The switch and the hour rollers are set afresh every time: the setting can
	// be changed from here, but the page also reopens long afterwards.
	if (h24_switch) {
		if (clock_use_24h()) {
			lv_obj_add_state(h24_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(h24_switch, LV_STATE_CHECKED);
		}
		// The switch just restored may have changed the format under the rollers
		// seed_rollers_from_clock() has only now filled.
		apply_time_format(selected_hour24());
	}

	// Set from the settings: offer the way out. On the first-boot showing the
	// clock is not set yet and there is no way back.
	if (cancel_btn) {
		if (timeset_needed()) {
			lv_obj_set_hidden(cancel_btn, true);
		} else {
			lv_obj_set_hidden(cancel_btn, false);
		}
	}

	// The heading follows the chevron. Opened from the settings it sits beside
	// it, where every other page keeps its heading; on the first-boot showing
	// there is no chevron, and a title held to the left of an empty corner looks
	// out of place, so it goes in the middle.
	//
	// The title lives in the header row, which is a flex container: it is not
	// moved by hand, the row is told how to lay out what it has left. With the
	// chevron hidden LVGL skips it, and centring the row centres the title.
	if (header_row) {
		lv_obj_set_flex_align(header_row, timeset_needed() ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START,
							  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	}

	lv_obj_set_hidden(panel, false);
	lv_obj_move_foreground(panel);

	// Opened from the settings, this is a page like any other and the status
	// bar belongs on top of it -- the panel lives on the top layer, so moving
	// it forward had put it over the bar. On the first-boot showing the bar
	// stays away: there is no clock to show yet, which is the whole reason the
	// panel is up, and a bar reading "--:--" over it would be a poor welcome.
	if (!timeset_needed()) {
		topbar_set_hidden(false);
		topbar_bring_to_front();
	}

	// Setting a date takes as long as it takes; the screen must not blank
	// halfway through, because blanking this panel also kills the touchscreen.
	power_hold_screen_on(true);
}

// Re-paints the rollers' hand-set colours after a theme or accent switch.
static void timeset_refresh_theme(void) {
	lv_obj_t *const rollers[] = {roller_day, roller_month, roller_year, roller_hour, roller_minute, roller_ampm};
	for (size_t i = 0; i < sizeof(rollers) / sizeof(rollers[0]); i++) {
		if (!rollers[i]) {
			continue;
		}
		lv_obj_set_style_text_color(rollers[i], theme()->text_secondary, 0);
		lv_obj_set_style_bg_color(rollers[i], theme()->surface_pressed, LV_PART_SELECTED);
		lv_obj_set_style_text_color(rollers[i], theme()->accent, LV_PART_SELECTED);
	}

	// And the pill: its fill is set by hand when the page is built, so it holds
	// whichever accent was live then.
	if (confirm_btn) {
		lv_obj_set_style_bg_color(confirm_btn, theme()->accent, 0);
	}

	// Standard time and daylight saving, for the same reason. Those two carry
	// no shared style at all -- both the fill and the ink are decided in
	// dst_paint() -- so each is a snapshot of the palette at the moment the page
	// was built or a pill was last tapped.
	dst_paint();
}

// The zone list is a panel over the Date and time one, not a screen. Date and
// time lives on the top layer (it has to cover the status bar on the first
// boot, when there is no clock to show yet) and no screen can be opened from
// there, so a second panel is laid over it, as the language page does.
static void build_timezone_panel(gui_config_t *cfg) {
	tz_panel = lv_obj_create(lv_layer_top());
	lv_obj_set_size(tz_panel, cfg->screen_width, cfg->screen_height);
	lv_obj_align(tz_panel, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(tz_panel, &theme_style_screen, 0);
	lv_obj_set_style_border_width(tz_panel, 0, 0);
	lv_obj_set_style_radius(tz_panel, 0, 0);
	lv_obj_set_style_pad_all(tz_panel, cfg->padding, 0);
	lv_obj_set_style_pad_hor(tz_panel, 0, 0); // as above: the scrollbar at the screen edge
	lv_obj_set_style_pad_top(tz_panel, cfg->padding + cfg->top_bar_height, 0);
	lv_obj_set_scrollable(tz_panel, false);
	lv_obj_set_hidden(tz_panel, true);
	lv_obj_set_flex_flow(tz_panel, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(tz_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(tz_panel, 0, 0);

	lv_obj_t *head = lv_obj_create(tz_panel);
	lv_obj_set_size(head, lv_pct(100), TIMESET_HEADER_BTN);
	lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(head, 0, 0);
	lv_obj_set_style_pad_all(head, 0, 0);
	lv_obj_set_style_pad_hor(head, cfg->padding, 0);
	lv_obj_set_style_pad_column(head, 14, 0);
	lv_obj_set_scrollable(head, false);
	lv_obj_set_clickable(head, false);
	lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *back = lv_btn_create(head);
	lv_obj_set_size(back, TIMESET_HEADER_BTN, TIMESET_HEADER_BTN);
	lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(back, 0, 0);
	lv_obj_set_style_shadow_width(back, 0, 0);
	lv_obj_set_style_pad_all(back, 0, 0);
	lv_obj_add_event_cb(back, tz_back_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *back_icon = lv_image_create(back);
	lv_image_set_src(back_icon, &icon_chevron_left);
	lv_obj_add_style(back_icon, &theme_style_icon, 0);
	lv_obj_center(back_icon);

	lv_obj_t *head_title = lv_label_create(head);
	lv_label_set_text(head_title, tr("timeset_time_zone"));
	lv_obj_add_style(head_title, &theme_style_text, 0);
	lv_obj_set_style_text_font(head_title, &font_ui_32, 0);

	tz_list = lv_obj_create(tz_panel);
	lv_obj_set_width(tz_list, lv_pct(100));
	lv_obj_set_flex_grow(tz_list, 1);
	lv_obj_set_style_bg_opa(tz_list, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(tz_list, 0, 0);
	lv_obj_set_style_pad_hor(tz_list, cfg->padding, 0);
	lv_obj_set_style_pad_ver(tz_list, 10, 0);
	lv_obj_set_style_pad_top(tz_list, cfg->padding, 0); // as above: the breathing room of the other pages
	lv_obj_set_style_pad_gap(tz_list, 8, 0);
	lv_obj_set_flex_flow(tz_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(tz_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scroll_dir(tz_list, LV_DIR_VER);

	for (int i = 0; i < TIMEZONE_COUNT; i++) {
		lv_obj_t *row = lv_btn_create(tz_list);
		lv_obj_set_size(row, lv_pct(100), 74);
		lv_obj_add_style(row, &theme_style_card, 0);
		lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_radius(row, 12, 0);
		lv_obj_set_style_border_width(row, 0, 0);
		lv_obj_set_style_shadow_width(row, 0, 0);
		lv_obj_set_style_pad_hor(row, 20, 0);
		lv_obj_set_style_pad_ver(row, 0, 0);
		lv_obj_set_style_pad_column(row, 12, 0);
		lv_obj_set_scrollable(row, false);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_add_event_cb(row, tz_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		// The offset on the left, in a column, and the places beside it: the
		// offset is what is looked for, the places are what make it
		// recognisable.
		char offset[16];
		tz_format_offset(TIMEZONES[i].minutes, offset, sizeof(offset));

		lv_obj_t *offset_label = lv_label_create(row);
		lv_label_set_text(offset_label, offset);
		lv_obj_set_width(offset_label, 118);
		lv_obj_add_style(offset_label, &theme_style_text, 0);
		lv_obj_set_style_text_font(offset_label, &font_ui_22, 0);

		lv_obj_t *places = lv_label_create(row);
		lv_label_set_text(places, tr(TIMEZONES[i].places));
		lv_label_set_long_mode(places, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(places, 1);
		lv_obj_set_height(places, lv_font_get_line_height(&font_ui_20));
		lv_obj_add_style(places, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(places, &font_ui_20, 0);

		tz_checks[i] = lv_image_create(row);
		lv_image_set_src(tz_checks[i], &icon_check);
		lv_obj_set_style_image_recolor(tz_checks[i], TZ_CHECK_GREEN, 0);
		lv_obj_set_style_image_recolor_opa(tz_checks[i], LV_OPA_COVER, 0);
		lv_obj_set_hidden(tz_checks[i], true);
	}
}

void timeset_init(gui_config_t *cfg) {
	fill_numbers(days_options, sizeof(days_options), 1, 31, "%d");
	fill_numbers(years_options, sizeof(years_options), TIMESET_YEAR_FIRST, TIMESET_YEAR_LAST, "%d");
	fill_numbers(hours_options, sizeof(hours_options), 0, 23, "%02d");
	fill_numbers(minutes_options, sizeof(minutes_options), 0, 59, "%02d");

	// The twelve hours are not a plain sequence: they start at 12 and then run 1
	// to 11, the order of every clock face and every 12-hour picker. No leading
	// zero, the way they are written.
	{
		size_t used = (size_t)snprintf(hours12_options, sizeof(hours12_options), "12");
		for (int h = 1; h <= 11; h++) {
			used += (size_t)snprintf(hours12_options + used, sizeof(hours12_options) - used, "\n%d", h);
		}
	}

	panel = lv_obj_create(lv_layer_top());
	lv_obj_set_size(panel, cfg->screen_width, cfg->screen_height);
	lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(panel, &theme_style_screen, 0);
	lv_obj_set_style_border_width(panel, 0, 0);
	lv_obj_set_style_radius(panel, 0, 0);
	// No horizontal padding on the panel: the scrolling content has to reach the
	// screen edge, so the scrollbar falls on the right edge as on every
	// settingsrow page. The 15 px inset is applied by the title row and by the
	// content, each for itself.
	lv_obj_set_style_pad_all(panel, cfg->padding, 0);
	lv_obj_set_style_pad_hor(panel, 0, 0);
	lv_obj_set_scrollable(panel, false);
	lv_obj_set_hidden(panel, true);

	lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(panel, 0, 0);

	// The status bar's height goes above the header row, so the chevron falls
	// exactly where it does on every other page (padding + bar height), and the
	// title follows it.
	lv_obj_set_style_pad_top(panel, cfg->padding + cfg->top_bar_height, 0);

	// The chevron and the title on a real row inside the column, not pinned
	// above it: a pinned header stays put while a centred column grows, and a
	// column tall enough rises over it. The language page keeps its header in
	// the flow for the same reason.
	header_row = lv_obj_create(panel);
	lv_obj_set_size(header_row, lv_pct(100), TIMESET_HEADER_BTN);
	lv_obj_set_style_bg_opa(header_row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(header_row, 0, 0);
	lv_obj_set_style_pad_all(header_row, 0, 0);
	lv_obj_set_style_pad_hor(header_row, cfg->padding, 0);
	lv_obj_set_style_pad_column(header_row, 14, 0);
	lv_obj_set_scrollable(header_row, false);
	lv_obj_set_clickable(header_row, false);
	lv_obj_set_flex_flow(header_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(header_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// The chevron comes first in the row, so the title places itself beside it
	// -- and when it disappears (first boot) it frees the space instead of
	// leaving a hole.
	cancel_btn = lv_btn_create(header_row);
	lv_obj_set_size(cancel_btn, TIMESET_HEADER_BTN, TIMESET_HEADER_BTN);
	lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cancel_btn, 0, 0);
	lv_obj_set_style_shadow_width(cancel_btn, 0, 0);
	lv_obj_set_style_pad_all(cancel_btn, 0, 0);
	lv_obj_set_hidden(cancel_btn, true);
	lv_obj_add_event_cb(cancel_btn, cancel_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *cancel_icon = lv_image_create(cancel_btn);
	lv_image_set_src(cancel_icon, &icon_chevron_left);
	lv_obj_add_style(cancel_icon, &theme_style_icon, 0);
	lv_obj_center(cancel_icon);

	title_label = lv_label_create(header_row);
	lv_label_set_text(title_label, tr("date_and_time"));
	lv_obj_add_style(title_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(title_label, &font_ui_32, 0);

	// Everything else goes in a container that takes what is left and centres
	// it: the cards sit in the middle of the space below the header, not in the
	// middle of the page. Growing further, they close on the centre of that
	// space instead of climbing back over the title.
	lv_obj_t *content = lv_obj_create(panel);
	lv_obj_set_width(content, lv_pct(100));
	lv_obj_set_flex_grow(content, 1);
	lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(content, 0, 0);
	lv_obj_set_style_pad_all(content, 0, 0);
	lv_obj_set_style_pad_hor(content, cfg->padding, 0);
	// The same gap between title and content as every other page: on settingsrow
	// pages the content starts one padding below the header row (see
	// settingsrow_content_top).
	lv_obj_set_style_pad_top(content, cfg->padding, 0);
	lv_obj_set_style_pad_gap(content, 18, 0);
	lv_obj_set_clickable(content, false);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

	// Scrollable, and vertically only: the cards together (the two rollers, the
	// zone, daylight saving, 24 hour, Confirm) are taller than the room below
	// the header, and without this the excess is simply cut off.
	//
	// The alignment is START and not CENTER: centring content taller than its
	// container starts it above the top edge, so the first card stays cut off
	// even when it can scroll.
	lv_obj_set_scrollable(content, true);
	lv_obj_set_scroll_dir(content, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_set_style_pad_bottom(content, 12, 0);
	lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// Date in one card, time in the next: five rollers side by side would be
	// too narrow to hit on a 480 px panel, and the two boxed groups read as
	// "the date" and "the time" at a glance.
	lv_obj_t *date_card = make_card(content);
	roller_day = make_roller(date_card, days_options, 86);
	roller_month = make_roller(date_card, MONTHS, 184);
	roller_year = make_roller(date_card, years_options, 110);

	lv_obj_t *time_card = make_card(content);
	roller_hour = make_roller(time_card, hours_options, 118);

	// The colon between the two, like every clock dialog.
	lv_obj_t *colon = lv_label_create(time_card);
	lv_label_set_text(colon, ":");
	lv_obj_add_style(colon, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(colon, &font_ui_32, 0);

	roller_minute = make_roller(time_card, minutes_options, 118);

	// The third roller always exists and is merely hidden: building and
	// destroying it on every format change would mean rebuilding its styling too
	// and re-entering it in the list the theme repaints.
	roller_ampm = make_roller(time_card, AMPM_OPTIONS, 92);
	lv_obj_set_hidden(roller_ampm, true);

	// Time zone, above "24 hour", because it decides what time the rollers above
	// it say. A single row with the value on the right, like any settings row;
	// tapping it opens the list.
	lv_obj_t *tz_card = lv_obj_create(content);
	lv_obj_set_size(tz_card, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_add_style(tz_card, &theme_style_card, 0);
	lv_obj_add_style(tz_card, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(tz_card, 12, 0);
	lv_obj_set_style_border_width(tz_card, 0, 0);
	lv_obj_set_style_shadow_width(tz_card, 0, 0);
	lv_obj_set_style_pad_hor(tz_card, 20, 0);
	lv_obj_set_style_pad_ver(tz_card, 16, 0);
	lv_obj_set_scrollable(tz_card, false);
	lv_obj_set_flex_flow(tz_card, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(tz_card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_event_cb(tz_card, tz_open_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *tz_name = lv_label_create(tz_card);
	lv_label_set_text(tz_name, tr("timeset_time_zone"));
	lv_obj_add_style(tz_name, &theme_style_text, 0);
	lv_obj_set_style_text_font(tz_name, &font_ui_24, 0);

	tz_value = lv_label_create(tz_card);
	lv_obj_add_style(tz_value, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(tz_value, &font_ui_20, 0);
	lv_label_set_text(tz_value, "");

	// Daylight saving, two pills below the zone.
	//
	// Below the list and not inside it: the list says where one is -- "Rome" is
	// +1 as on any map -- and this says when. Keeping them apart means that in
	// March and October a pill is tapped instead of hunting for one's own city
	// on a different row, and that the list stays one list instead of two nearly
	// identical ones.
	lv_obj_t *dst_card = lv_obj_create(content);
	lv_obj_set_size(dst_card, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_add_style(dst_card, &theme_style_card, 0);
	lv_obj_set_style_radius(dst_card, 12, 0);
	lv_obj_set_style_border_width(dst_card, 0, 0);
	lv_obj_set_style_shadow_width(dst_card, 0, 0);
	lv_obj_set_style_pad_hor(dst_card, 20, 0);
	lv_obj_set_style_pad_ver(dst_card, 14, 0);
	lv_obj_set_style_pad_gap(dst_card, 10, 0);
	lv_obj_set_scrollable(dst_card, false);
	lv_obj_set_flex_flow(dst_card, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(dst_card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	for (int i = 0; i < 2; i++) {
		lv_obj_t *pill = lv_btn_create(dst_card);
		lv_obj_set_size(pill, LV_SIZE_CONTENT, 52);
		lv_obj_set_flex_grow(pill, 1);
		lv_obj_set_style_pad_hor(pill, 14, 0);
		lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_shadow_width(pill, 0, 0);
		lv_obj_set_style_border_width(pill, 0, 0);
		lv_obj_add_event_cb(pill, dst_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		lv_obj_t *label = lv_label_create(pill);
		lv_label_set_text(label, tr(i == 0 ? "timeset_standard_time" : "timeset_daylight_saving"));
		lv_obj_set_style_text_font(label, &font_ui_20, 0);
		lv_obj_center(label);
		dst_pills[i] = pill;
	}
	dst_paint();

	// 24 hour: below the rollers and above Confirm, where it is looked for -- it
	// is decided while looking at the time being entered, not on another page.
	lv_obj_t *h24_card = lv_obj_create(content);
	lv_obj_set_size(h24_card, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_add_style(h24_card, &theme_style_card, 0);
	lv_obj_set_style_radius(h24_card, 12, 0);
	lv_obj_set_style_border_width(h24_card, 0, 0);
	lv_obj_set_style_shadow_width(h24_card, 0, 0);
	lv_obj_set_style_pad_hor(h24_card, 20, 0);
	lv_obj_set_style_pad_ver(h24_card, 16, 0);
	lv_obj_set_scrollable(h24_card, false);
	lv_obj_set_flex_flow(h24_card, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(h24_card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *h24_label = lv_label_create(h24_card);
	lv_label_set_text(h24_label, tr("timeset_24_hour_clock"));
	lv_obj_add_style(h24_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(h24_label, &font_ui_24, 0);

	h24_switch = settingsrow_switch(h24_card, h24_toggle_cb);
	if (clock_use_24h()) {
		lv_obj_add_state(h24_switch, LV_STATE_CHECKED);
	}

	lv_obj_t *confirm = lv_btn_create(content);
	confirm_btn = confirm;
	lv_obj_set_size(confirm, 230, 64);
	lv_obj_add_style(confirm, &theme_style_card, 0);
	lv_obj_add_style(confirm, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(confirm, theme()->accent, 0);
	lv_obj_set_style_radius(confirm, LV_RADIUS_CIRCLE, 0); // Adwaita pill button
	lv_obj_set_style_shadow_width(confirm, 0, 0);
	lv_obj_add_event_cb(confirm, confirm_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *confirm_label = lv_label_create(confirm);
	lv_label_set_text(confirm_label, tr("confirm"));
	lv_obj_set_style_text_font(confirm_label, &font_ui_24, 0);
	lv_obj_set_style_text_color(confirm_label, lv_color_white(), 0);
	lv_obj_center(confirm_label);

	build_timezone_panel(cfg);
	tz_paint();

	theme_register_refresh(timeset_refresh_theme);
}
