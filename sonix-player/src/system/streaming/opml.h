#ifndef OPML_H
#define OPML_H

#include <stdbool.h>

#include "src/system/streaming/podcast.h"

// OPML, the file every podcast app exports its subscriptions to: an XML list
// of <outline> elements, one per podcast, each naming its feed in xmlUrl.
//
// Only that much is read. Nested outlines (folders, as some apps group
// podcasts) are walked like flat ones, attributes in either quote style, the
// five named entities and numeric ones. Anything else in the file is ignored:
// what matters is the address.

#define OPML_MAX_FEEDS 200
#define OPML_MAX_FILES 16

typedef struct {
	char title[PODCAST_TITLE_MAX];
	char url[PODCAST_FEED_URL_MAX];
} opml_feed_t;

// The .opml files in the card's root and in its Podcast folder, by name.
// `out` gets full paths. Returns how many.
int opml_find_files(const char *card_root, char (*out)[512], int max);

// The feeds of one file, in the file's order, each address once. -1 when the
// file cannot be read.
int opml_read(const char *path, opml_feed_t *out, int max);

// Writes `feeds` as an OPML 2.0 file, through a temporary file renamed into
// place. False when it could not be written.
bool opml_write(const char *path, const opml_feed_t *feeds, int count);

#endif /* OPML_H */
