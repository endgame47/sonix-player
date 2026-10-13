#ifndef TABBAR_H
#define TABBAR_H

#include <stdbool.h>

#include "lvgl.h"

// A row of tabs in one rounded trough, the way GNOME switches between the
// views of one page: the tabs fill the width between them and the one showing
// is filled with the accent. For a handful of short names -- a page's sections,
// a list's filters -- where pills would wrap or scroll.

#define TABBAR_MAX 4

typedef struct {
	lv_obj_t *bar;
	lv_obj_t *tabs[TABBAR_MAX];
	lv_obj_t *labels[TABBAR_MAX];
	int count;
	int active; // -1 for none
} tabbar_t;

// Builds the empty trough in `parent`, as wide as its parent's content.
lv_obj_t *tabbar_create(tabbar_t *t, lv_obj_t *parent);

// Adds a tab named by the translation key `text`. `cb` gets the click, with
// `user` as its user data. Returns the tab.
lv_obj_t *tabbar_add(tabbar_t *t, const char *text, lv_event_cb_t cb, void *user);

// The tab showing; -1 for none. Also repaints, after a theme change.
void tabbar_set_active(tabbar_t *t, int index);

// What a tab says, already translated: a name with a count, say.
void tabbar_set_text(tabbar_t *t, int index, const char *text);

// Greys a tab out and stops it answering.
void tabbar_set_enabled(tabbar_t *t, int index, bool enabled);

#endif /* TABBAR_H */
