#include "main_menu.h"

#include <stdint.h>

#include "src/gui/library/audiobooks.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/morepage.h"
#include "src/gui/library/music.h"
#include "src/gui/settings/settings.h"
#include "src/gui/streaming/streaming.h"
#include "src/gui/wireless/wireless.h"
#include "src/gui/shell/switcher.h"

#include "lvgl/lvgl.h"

lv_obj_t *main_menu_screen;

// Audiobooks reads its library straight off the card, so it is the one tile
// that opens onto nothing when the card is out or at a computer. Music has the
// same rule a level deeper, where a list is asked for.
static void open_audiobooks(void) {
	if (!gui_card_available()) {
		gui_notify_no_card();
		return;
	}
	switch_screen(audiobooks_screen);
}

void main_menu_init(gui_config_t *cfg) {
	const grid_entry_t entries[] = {
		{"music", &icon_menu_music, &music_screen},
		{"streaming", &icon_menu_streaming, &streaming_screen},
		{"wireless", &icon_menu_wireless, &wireless_screen},
		{"audiobooks", &icon_menu_audiobooks, NULL, open_audiobooks},
		// The door to the More page, which holds the DAC and anything else
		// that does not fit. The main menu has exactly six tiles, and one of
		// them has to be able to keep growing.
		{"more", &icon_menu_more, &morepage_screen},
		{"settings", &icon_menu_settings, &settings_screen},
	};

	gridpage_build(main_menu_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 3, false);
}
