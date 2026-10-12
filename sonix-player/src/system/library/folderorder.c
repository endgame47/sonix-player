#include "folderorder.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "src/system/library/cue.h"
#include "src/system/library/library.h"
#include "src/system/library/metadata.h"

// How many files of one folder may be opened for their tags. A handful of
// tracks copied on since the last scan is worth reading; a whole folder the
// index has never seen is a scan's job, and opening hundreds of files here
// would hold the listing up for seconds on a slow card. Past this nothing is
// opened and those files keep the order of their names.
#define FOLDERORDER_TAG_READS 64

// ---------------------------------------------------------------------------
// what was read from the files themselves
//
// Only the folder last asked about is kept: the browser lists a folder and the
// queue is started from the same folder a moment later. A file that turned out
// to have no track number is kept too, so it is not opened again.
// ---------------------------------------------------------------------------

typedef struct {
	char *name;
	char *album;
	uint16_t disc;
	uint16_t track;
} read_place_t;

static pthread_mutex_t read_lock = PTHREAD_MUTEX_INITIALIZER;
static char *read_folder;
static read_place_t read_places[FOLDERORDER_TAG_READS];
static size_t read_count;

static void read_clear_locked(void) {
	for (size_t i = 0; i < read_count; i++) {
		free(read_places[i].name);
		free(read_places[i].album);
	}
	read_count = 0;
	free(read_folder);
	read_folder = NULL;
}

static bool read_is_folder_locked(const char *folder) { return read_folder && strcmp(read_folder, folder) == 0; }

static const read_place_t *read_find_locked(const char *name) {
	for (size_t i = 0; i < read_count; i++) {
		if (strcmp(read_places[i].name, name) == 0) {
			return &read_places[i];
		}
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// one call's working state
// ---------------------------------------------------------------------------

typedef struct {
	// Each distinct album name once, compared without case as the index
	// compares it.
	char **albums;
	size_t album_count;
	size_t album_cap;

	// Per file: which album, the disc and the track. album_of is -1 for a
	// file whose place is not known.
	int *album_of;
	uint16_t *disc;
	uint16_t *track;
	bool *known;
} work_t;

static int work_album(work_t *w, const char *album) {
	for (size_t i = 0; i < w->album_count; i++) {
		if (strcasecmp(w->albums[i], album) == 0) {
			return (int)i;
		}
	}
	if (w->album_count == w->album_cap) {
		size_t cap = w->album_cap ? w->album_cap * 2 : 8;
		char **grown = realloc(w->albums, cap * sizeof(*grown));
		if (!grown) {
			return -1;
		}
		w->albums = grown;
		w->album_cap = cap;
	}
	char *copy = strdup(album);
	if (!copy) {
		return -1;
	}
	w->albums[w->album_count] = copy;
	return (int)w->album_count++;
}

static uint16_t clamp16(int value) {
	if (value < 0) {
		return 0;
	}
	return value > UINT16_MAX ? UINT16_MAX : (uint16_t)value;
}

static void work_set(work_t *w, size_t i, const char *album, int disc, int track) {
	w->known[i] = true;
	if (track <= 0) {
		return; // in the index, with no track number: still unknown
	}
	int id = work_album(w, album ? album : "");
	if (id < 0) {
		return;
	}
	w->album_of[i] = id;
	w->disc[i] = clamp16(disc > 0 ? disc : 1);
	w->track[i] = clamp16(track);
}

static void place_cb(size_t index, const char *album, int disc, int track, void *user) {
	work_set(user, index, album, disc, track);
}

// ---------------------------------------------------------------------------
// the album ranks
// ---------------------------------------------------------------------------

static __thread char **rank_albums;

static int rank_cmp(const void *a, const void *b) {
	int ia = *(const int *)a;
	int ib = *(const int *)b;
	int by_name = strcasecmp(rank_albums[ia], rank_albums[ib]);
	return by_name ? by_name : ia - ib;
}

// ---------------------------------------------------------------------------

void folderorder_places(const char *folder, const char *const *names, size_t count, bool read_tags,
						folder_place_t *out) {
	if (!out || count == 0) {
		return;
	}
	memset(out, 0, count * sizeof(*out));
	if (!folder || !names) {
		return;
	}

	work_t w = {0};
	w.album_of = malloc(count * sizeof(*w.album_of));
	w.disc = calloc(count, sizeof(*w.disc));
	w.track = calloc(count, sizeof(*w.track));
	w.known = calloc(count, sizeof(*w.known));

	// The index is looked up by full path, built for every file at once in one
	// buffer.
	size_t folder_len = strlen(folder);
	size_t pool_size = 0;
	for (size_t i = 0; i < count; i++) {
		pool_size += folder_len + 1 + (names[i] ? strlen(names[i]) : 0) + 1;
	}
	char *pool = malloc(pool_size ? pool_size : 1);
	const char **paths = malloc(count * sizeof(*paths));

	if (!w.album_of || !w.disc || !w.track || !w.known || !pool || !paths) {
		goto done;
	}

	char *at = pool;
	for (size_t i = 0; i < count; i++) {
		w.album_of[i] = -1;
		paths[i] = at;
		at += sprintf(at, "%s/%s", folder, names[i] ? names[i] : "") + 1;
	}

	library_track_places(paths, count, place_cb, &w);

	// What the index does not hold: first what an earlier call read for this
	// folder, then -- if the caller allows it and there are few enough -- the
	// files themselves. A cue sheet's track is never opened: the sheet's own
	// numbers order those, and the index holds them once the card is scanned.
	size_t to_read = 0;
	pthread_mutex_lock(&read_lock);
	bool same_folder = read_is_folder_locked(folder);
	for (size_t i = 0; i < count; i++) {
		if (w.known[i] || !names[i] || cue_split_path(names[i], NULL, 0) > 0) {
			continue;
		}
		const read_place_t *r = same_folder ? read_find_locked(names[i]) : NULL;
		if (r) {
			work_set(&w, i, r->album, r->disc, r->track);
		} else {
			to_read++;
		}
	}
	pthread_mutex_unlock(&read_lock);

	if (read_tags && to_read > 0 && to_read <= FOLDERORDER_TAG_READS) {
		song_metadata_t *tags = malloc(sizeof(*tags));
		if (tags) {
			for (size_t i = 0; i < count; i++) {
				if (w.known[i] || !names[i] || cue_split_path(names[i], NULL, 0) > 0) {
					continue;
				}
				// Opened without the lock held: the queue asking about the
				// same folder meanwhile is not made to wait for the card.
				metadata_read(paths[i], tags);
				work_set(&w, i, tags->album, tags->disc_number, tags->track_number);

				pthread_mutex_lock(&read_lock);
				if (!read_is_folder_locked(folder)) {
					read_clear_locked();
					read_folder = strdup(folder);
				}
				if (read_folder && read_count < FOLDERORDER_TAG_READS && !read_find_locked(names[i])) {
					read_place_t *r = &read_places[read_count];
					r->name = strdup(names[i]);
					r->album = strdup(tags->album);
					r->disc = clamp16(tags->disc_number > 0 ? tags->disc_number : 1);
					r->track = clamp16(tags->track_number);
					if (r->name && r->album) {
						read_count++;
					} else {
						free(r->name);
						free(r->album);
					}
				}
				pthread_mutex_unlock(&read_lock);
			}
			free(tags);
		}
	}

	// Albums by name. Usually there is one, and the whole sort is over one
	// element.
	if (w.album_count > 0) {
		int *by_name = malloc(w.album_count * sizeof(*by_name));
		uint16_t *rank = malloc(w.album_count * sizeof(*rank));
		if (by_name && rank) {
			for (size_t i = 0; i < w.album_count; i++) {
				by_name[i] = (int)i;
			}
			rank_albums = w.albums;
			qsort(by_name, w.album_count, sizeof(*by_name), rank_cmp);
			for (size_t i = 0; i < w.album_count; i++) {
				rank[by_name[i]] = clamp16((int)i);
			}
			for (size_t i = 0; i < count; i++) {
				if (w.album_of[i] >= 0) {
					out[i].album = rank[w.album_of[i]];
					out[i].disc = w.disc[i];
					out[i].track = w.track[i];
				}
			}
		}
		free(by_name);
		free(rank);
	}

done:
	for (size_t i = 0; i < w.album_count; i++) {
		free(w.albums[i]);
	}
	free(w.albums);
	free(w.album_of);
	free(w.disc);
	free(w.track);
	free(w.known);
	free(pool);
	free(paths);
}

int folderorder_cmp(const folder_place_t *a, const folder_place_t *b) {
	bool known_a = a->track > 0;
	bool known_b = b->track > 0;
	if (known_a != known_b) {
		return known_a ? -1 : 1;
	}
	if (!known_a) {
		return 0;
	}
	if (a->album != b->album) {
		return a->album < b->album ? -1 : 1;
	}
	if (a->disc != b->disc) {
		return a->disc < b->disc ? -1 : 1;
	}
	if (a->track != b->track) {
		return a->track < b->track ? -1 : 1;
	}
	return 0;
}
