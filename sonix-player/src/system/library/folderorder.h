#ifndef FOLDERORDER_H
#define FOLDERORDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The order of the tracks in one folder: the order of the record, by disc and
// track number, and not the order the file names happen to spell.
//
// The browser lists a folder in this order and the queue a folder starts is
// built in the same one, so the track after the one tapped is the one under
// it on the screen. Both ask here, which is what keeps the two from drifting.
//
// What a file's place is comes from the library index, which already holds
// the album, disc and track number of every scanned file. A file the index
// does not hold yet -- copied on since the last scan -- is opened for its tags
// when the caller allows it and the folder has only a few such files; what was
// read is kept for the folder last asked about, so the queue started from it
// finds the same answer without opening anything.

typedef struct {
	uint16_t album; // the album's place among the folder's albums, by name
	uint16_t disc;	// 1 for a file with no disc tag
	uint16_t track; // 0 when the file's place on its record is not known
} folder_place_t;

// Fills out[i] for names[i], a file (or a cue sheet's track, as its virtual
// name) inside `folder`. `read_tags` allows opening files the index does not
// hold; without it they are looked up in what an earlier call read and are
// otherwise left unknown. Every out[i] is written.
void folderorder_places(const char *folder, const char *const *names, size_t count, bool read_tags,
						folder_place_t *out);

// Two places in record order: by album, disc, then track, and every known
// place before every unknown one. 0 when the places do not decide -- two
// unknown files, or the same album, disc and track -- and the caller orders
// those by name.
int folderorder_cmp(const folder_place_t *a, const folder_place_t *b);

#endif // FOLDERORDER_H
