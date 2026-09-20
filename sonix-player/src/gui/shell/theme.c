#include "theme.h"

#include "src/system/device/bootlogo.h"
#include "src/system/core/config.h"

#include <stdio.h>
#include <stdlib.h>

lv_style_t theme_style_screen;
lv_style_t theme_style_panel;
lv_style_t theme_style_card;
lv_style_t theme_style_card_pressed;
lv_style_t theme_style_text;
lv_style_t theme_style_text_dim;
lv_style_t theme_style_icon;
lv_style_t theme_style_switch;		   // switch track, off state
lv_style_t theme_style_switch_checked; // switch indicator, on state
lv_style_t theme_style_accent_bg;	   // anything that must follow the accent
lv_style_t theme_style_slider_track;   // a slider's unfilled part

// GNOME Adwaita (libadwaita) colours, dark variant: window #242424, cards
// #303030 (white at 8%% over the window), accent blue #3584e4. Secondary text
// is white at 55%%, flattened against the card colour.
static theme_palette_t palette_dark = {
	.screen_bg = LV_COLOR_MAKE(36, 36, 36),
	.surface = LV_COLOR_MAKE(48, 48, 48),
	.surface_pressed = LV_COLOR_MAKE(64, 64, 64),
	.panel = LV_COLOR_MAKE(30, 30, 30),
	.cover_bg = LV_COLOR_MAKE(30, 30, 30),
	.text_primary = LV_COLOR_MAKE(255, 255, 255),
	.text_secondary = LV_COLOR_MAKE(160, 160, 160),
	.accent = LV_COLOR_MAKE(53, 132, 228),
	.dark = true,
};

// Adwaita light: window #fafafa, cards white, pressed = black at 8%%, text
// black at 80%%. The accent is the same #3584e4 in both variants.
static theme_palette_t palette_light = {
	// The window is pushed further from white than stock Adwaita's #fafafa:
	// on this panel white cards on a #fafafa window are indistinguishable, so
	// the window is a light grey and the pressed state a step deeper.
	.screen_bg = LV_COLOR_MAKE(236, 237, 240),
	.surface = LV_COLOR_MAKE(255, 255, 255),
	.surface_pressed = LV_COLOR_MAKE(221, 223, 228),
	.panel = LV_COLOR_MAKE(255, 255, 255),
	.cover_bg = LV_COLOR_MAKE(224, 226, 230),
	.text_primary = LV_COLOR_MAKE(50, 50, 50),
	.text_secondary = LV_COLOR_MAKE(115, 115, 115),
	.accent = LV_COLOR_MAKE(53, 132, 228),
	.dark = false,
};

// Whether the dark preset is the chosen one, kept apart from `active` because
// with the dynamic tint on `active` points at a derived copy rather than at
// either preset.
static bool dark_selected = true;

// The dynamic tint: the neutral colours -- backgrounds, cards, the status bar
// -- borrow the accent's hue, the way Material You colours a system from one
// seed. Off by default, remembered in the config.
static bool dynamic_tint;

// The tinted copy of whichever preset is chosen. Rebuilt on every theme,
// accent or tint change; `active` points here while the tint is on.
static theme_palette_t tinted;

static theme_palette_t *active = &palette_dark;

// The accent presets, Adwaita's named accents, ordered as requested: blue
// (the default), then warm to cool -- orange, yellow, green, purple, pink.
static const lv_color_t ACCENT_COLORS[THEME_ACCENT_COUNT] = {
	LV_COLOR_MAKE(53, 132, 228), // blue (#3584e4, the default)
	LV_COLOR_MAKE(255, 120, 0),  // orange (#ff7800)
	LV_COLOR_MAKE(229, 165, 10), // yellow (#e5a50a)
	LV_COLOR_MAKE(46, 194, 126), // green (#2ec27e)
	LV_COLOR_MAKE(145, 65, 172), // purple (#9141ac)
	LV_COLOR_MAKE(213, 97, 153), // pink (#d56199)
};
static int accent_index;

lv_color_t theme_accent_preset(int index) {
	if (index < 0 || index >= THEME_ACCENT_COUNT) {
		index = 0;
	}
	return ACCENT_COLORS[index];
}

int theme_get_accent(void) { return accent_index; }

// Callers asking to be called back after a theme or accent change. The list
// grows on demand and has no fixed limit: a dropped registration shows up only
// as a page that keeps the colours it was built with, which is invisible until
// somebody changes the accent. A failed allocation is logged for the same
// reason.
static theme_refresh_cb_t *refresh_cbs;
static int refresh_cb_count;
static int refresh_cb_capacity;

// How much of the accent's own saturation a neutral borrows. Dark surfaces
// take more before the hue stops reading as a shade and starts reading as a
// colour; the light ones are nearly white, where the same amount would turn
// the page pastel.
#define TINT_STRENGTH_DARK 0.30f
#define TINT_STRENGTH_LIGHT 0.15f

// Gives a neutral the accent's hue while keeping exactly how light it is: the
// brightest channel of the result is the brightest channel of the input. The
// colour is never pulled towards the accent's own brightness, which is what
// would turn a dark background light under a pale accent; the two channels
// below the brightest do come down, so a tinted surface is fractionally darker
// than the plain one, and that is what the two strengths above are set from.
static lv_color_t tint_with(lv_color_t c, lv_color_t accent, float strength) {
	uint8_t amax = accent.red > accent.green ? accent.red : accent.green;
	if (accent.blue > amax) {
		amax = accent.blue;
	}
	uint8_t amin = accent.red < accent.green ? accent.red : accent.green;
	if (accent.blue < amin) {
		amin = accent.blue;
	}
	uint8_t cmax = c.red > c.green ? c.red : c.green;
	if (c.blue > cmax) {
		cmax = c.blue;
	}
	if (amax == amin || cmax == 0) {
		return c; // a grey accent has no hue to lend, and black takes none
	}

	// The accent's shape: 1 on its brightest channel, 0 on its darkest. Applied
	// to the neutral's own brightness it reproduces that hue at the neutral's
	// level, at a fraction of the accent's saturation.
	float span = (float)(amax - amin);
	float sat = span / (float)amax * strength;
	float shape[3] = {
		((float)accent.red - (float)amin) / span,
		((float)accent.green - (float)amin) / span,
		((float)accent.blue - (float)amin) / span,
	};

	uint8_t out[3];
	for (int i = 0; i < 3; i++) {
		out[i] = (uint8_t)((float)cmax * (1.0f - sat * (1.0f - shape[i])) + 0.5f);
	}
	return lv_color_make(out[0], out[1], out[2]);
}

// Points `active` at the chosen preset, or at a tinted copy of it.
static void rebuild_active(void) {
	theme_palette_t *base = dark_selected ? &palette_dark : &palette_light;

	if (!dynamic_tint) {
		active = base;
		return;
	}

	float strength = base->dark ? TINT_STRENGTH_DARK : TINT_STRENGTH_LIGHT;
	tinted = *base;
	tinted.screen_bg = tint_with(base->screen_bg, base->accent, strength);
	tinted.surface = tint_with(base->surface, base->accent, strength);
	tinted.surface_pressed = tint_with(base->surface_pressed, base->accent, strength);
	tinted.panel = tint_with(base->panel, base->accent, strength);
	tinted.cover_bg = tint_with(base->cover_bg, base->accent, strength);
	// Text keeps the plain colours: the monochrome icons are recoloured with
	// text_primary, and they are meant to come through the tint untouched.
	active = &tinted;
}

// Pushes the active palette into the shared styles.
static void apply_palette(void) {
	lv_style_set_bg_color(&theme_style_screen, active->screen_bg);

	lv_style_set_bg_color(&theme_style_panel, active->panel);

	lv_style_set_bg_color(&theme_style_card, active->surface);
	lv_style_set_text_color(&theme_style_card, active->text_primary);

	lv_style_set_bg_color(&theme_style_card_pressed, active->surface_pressed);

	lv_style_set_text_color(&theme_style_text, active->text_primary);
	lv_style_set_text_color(&theme_style_text_dim, active->text_secondary);

	lv_style_set_image_recolor(&theme_style_icon, active->text_primary);

	lv_style_set_bg_color(&theme_style_switch, active->surface_pressed);
	lv_style_set_bg_color(&theme_style_switch_checked, active->accent);

	lv_style_set_bg_color(&theme_style_accent_bg, active->accent);
	lv_style_set_bg_color(&theme_style_slider_track, active->text_secondary);
}

void theme_init(void) {
	lv_style_init(&theme_style_screen);
	lv_style_set_bg_opa(&theme_style_screen, LV_OPA_COVER);

	lv_style_init(&theme_style_panel);
	lv_style_set_bg_opa(&theme_style_panel, LV_OPA_COVER);

	lv_style_init(&theme_style_card);
	lv_style_set_bg_opa(&theme_style_card, LV_OPA_COVER);

	lv_style_init(&theme_style_card_pressed);
	lv_style_set_bg_opa(&theme_style_card_pressed, LV_OPA_COVER);
	// The default theme grows a pressed button by a few pixels; on a list of
	// cards that reads as the row jumping out of the list, so pin it flat.
	lv_style_set_transform_width(&theme_style_card_pressed, 0);
	lv_style_set_transform_height(&theme_style_card_pressed, 0);

	lv_style_init(&theme_style_text);
	lv_style_init(&theme_style_text_dim);

	lv_style_init(&theme_style_icon);
	lv_style_set_image_recolor_opa(&theme_style_icon, LV_OPA_COVER);

	lv_style_init(&theme_style_switch);
	lv_style_set_bg_opa(&theme_style_switch, LV_OPA_COVER);
	lv_style_init(&theme_style_switch_checked);
	lv_style_set_bg_opa(&theme_style_switch_checked, LV_OPA_COVER);

	lv_style_init(&theme_style_accent_bg);
	lv_style_set_bg_opa(&theme_style_accent_bg, LV_OPA_COVER);
	lv_style_init(&theme_style_slider_track);
	lv_style_set_bg_opa(&theme_style_slider_track, LV_OPA_40);
	lv_style_set_radius(&theme_style_slider_track, LV_RADIUS_CIRCLE);

	// Whatever was chosen last time. Dark is the default for a first boot.
	dark_selected = config_get_bool("ui", "dark_theme", true);

	// The accent as it was left; both palettes carry the same one.
	accent_index = (int)config_get_int("ui", "accent", 0);
	if (accent_index < 0 || accent_index >= THEME_ACCENT_COUNT) {
		accent_index = 0;
	}
	palette_dark.accent = ACCENT_COLORS[accent_index];
	palette_light.accent = ACCENT_COLORS[accent_index];

	dynamic_tint = config_get_bool("ui", "dynamic_tint", false);

	rebuild_active();
	apply_palette();
}

void theme_apply_slider_knob(lv_obj_t *slider) {
	if (!slider) {
		return;
	}
	bool dark = theme()->dark;
	lv_obj_set_style_border_color(slider, dark ? lv_color_black() : lv_color_make(120, 120, 120), LV_PART_KNOB);
	lv_obj_set_style_border_opa(slider, dark ? LV_OPA_10 : LV_OPA_COVER, LV_PART_KNOB);
	lv_obj_set_style_border_width(slider, dark ? 1 : 2, LV_PART_KNOB);
}

void theme_register_refresh(theme_refresh_cb_t cb) {
	if (!cb) {
		return;
	}

	if (refresh_cb_count == refresh_cb_capacity) {
		int wanted = refresh_cb_capacity ? refresh_cb_capacity * 2 : 16;
		theme_refresh_cb_t *grown = realloc(refresh_cbs, (size_t)wanted * sizeof(*grown));
		if (!grown) {
			// A device that cannot find a few dozen bytes has bigger problems,
			// but a registration must never be lost unnoticed.
			fprintf(stderr, "theme: out of memory for refresh callback number %d\n",
					refresh_cb_count + 1);
			return;
		}
		refresh_cbs = grown;
		refresh_cb_capacity = wanted;
	}

	refresh_cbs[refresh_cb_count++] = cb;
}

const theme_palette_t *theme(void) { return active; }

bool theme_is_dark(void) { return active->dark; }

void theme_set_dark(bool dark) {
	if (active->dark == dark) {
		return;
	}
	theme_toggle();
}

// Which palette the pages on screen were last walked for.
//
// lv_obj_report_style_change(NULL) walks every object of every screen and
// refreshes each one against its whole subtree. The pages here are built at
// startup and stay built, so that is the entire interface -- thousands of
// objects -- for a change that moves nothing but colours, and it takes far
// longer than a frame: the finger is still on the switch when it starts.
//
// A colour is read out of the style at draw time, so the page in front of the
// user is the only one that has to be walked at the moment of the change. The
// pages behind it are walked the first time they are shown again, which is
// where a redraw was going to happen anyway.
static unsigned palette_generation = 1;

static struct screen_generation {
	lv_obj_t *screen;
	unsigned generation;
} *screen_gens;
static int screen_gen_count;
static int screen_gen_capacity;

// True when `screen` still carries an older palette than the one in force, and
// records it as walked for this one. A screen this has never seen is taken as
// current: it was built from the styles as they stand.
//
// An allocation that fails costs a walk per page change rather than a page
// with the wrong colours, which is why the failure is silent here.
static bool screen_is_stale(lv_obj_t *screen) {
	for (int i = 0; i < screen_gen_count; i++) {
		if (screen_gens[i].screen != screen) {
			continue;
		}
		if (screen_gens[i].generation == palette_generation) {
			return false;
		}
		screen_gens[i].generation = palette_generation;
		return true;
	}

	if (screen_gen_count == screen_gen_capacity) {
		int wanted = screen_gen_capacity ? screen_gen_capacity * 2 : 32;
		struct screen_generation *grown = realloc(screen_gens, (size_t)wanted * sizeof(*grown));
		if (!grown) {
			return true;
		}
		screen_gens = grown;
		screen_gen_capacity = wanted;
	}

	screen_gens[screen_gen_count].screen = screen;
	screen_gens[screen_gen_count].generation = palette_generation;
	screen_gen_count++;
	return false;
}

// One tree, refreshed against the styles it carries. LV_STYLE_PROP_ANY takes
// the object's own properties and recurses into its children.
static void refresh_tree(lv_obj_t *obj) {
	if (obj) {
		lv_obj_refresh_style(obj, LV_PART_ANY, LV_STYLE_PROP_ANY);
	}
}

void theme_notify_screen_shown(lv_obj_t *screen) {
	if (screen && screen_is_stale(screen)) {
		refresh_tree(screen);
	}
}

// Repaints after a palette change: the page on screen and the three layers the
// status bar, the player sheet and the popups live on, then the callbacks,
// which catch what cannot be expressed as a shared style -- an icon recoloured
// at runtime, the blurred artwork behind the controls. Those run for every
// page, on screen or not, and cost about as much as one frame in total.
static void refresh_all(void) {
	palette_generation++;

	lv_obj_t *screen = lv_screen_active();
	screen_is_stale(screen);
	refresh_tree(screen);
	refresh_tree(lv_layer_top());
	refresh_tree(lv_layer_sys());
	refresh_tree(lv_layer_bottom());

	for (int i = 0; i < refresh_cb_count; i++) {
		refresh_cbs[i]();
	}
}

void theme_toggle(void) {
	dark_selected = !dark_selected;
	rebuild_active();
	apply_palette();

	config_set_bool("ui", "dark_theme", active->dark);
	config_save();

	// The picture shown while the player boots follows the theme too. It is
	// written into raw flash rather than into the config, because the script
	// that draws it runs long before any filesystem holding a config is
	// mounted -- see bootlogo.h.
	bootlogo_follow_theme(active->dark);

	refresh_all();
}

void theme_set_accent(int index) {
	if (index < 0 || index >= THEME_ACCENT_COUNT) {
		index = 0;
	}
	accent_index = index;
	palette_dark.accent = ACCENT_COLORS[index];
	palette_light.accent = ACCENT_COLORS[index];
	rebuild_active(); // the tint is drawn from the accent, so it moves with it
	apply_palette();

	config_set_int("ui", "accent", index);
	config_save();

	refresh_all();
}

bool theme_dynamic_tint(void) { return dynamic_tint; }

void theme_set_dynamic_tint(bool on) {
	if (dynamic_tint == on) {
		return;
	}
	dynamic_tint = on;
	rebuild_active();
	apply_palette();

	config_set_bool("ui", "dynamic_tint", on);
	config_save();

	refresh_all();
}
