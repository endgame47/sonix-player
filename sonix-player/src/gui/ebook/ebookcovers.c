#include "src/gui/ebook/ebookcovers.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/ebook/ebook.h"
#include "src/gui/shell/gui.h"
#include "src/system/core/utils.h"

// See ebookcovers.h for why this thread is detached, what the generation number
// is for, and why only the visible covers are read.

typedef struct {
	uint32_t gen;
	char dir[512];
	int count;
	int box_w, box_h;
	ebookcovers_ready_cb cb;
	char (*files)[256]; // the worker's own copy; it frees this
	uint8_t *done;		// one byte per book: asked for and finished with

	// What the page wants now. Written by the UI thread under `lock`, read by
	// the worker, and the condition it waits on when there is nothing to do --
	// a sleeping thread rather than one that wakes twenty times a second to
	// find the same empty list.
	pthread_mutex_t lock;
	pthread_cond_t wake;
	int want_from, want_to;
	bool stopping;
} covers_job_t;

typedef struct {
	uint32_t gen;
	int index;
	ebookcovers_ready_cb cb;
	char title[256];
	cover_image_t cover;
	bool has_cover;
} covers_result_t;

// Bumped by every start and every stop. Read by the worker without a lock: one
// aligned word, one writer (the UI thread), and the worst a torn read could do
// is one more book being opened before the worker gives up.
static volatile uint32_t covers_gen = 1;

// The job the current run belongs to. Only the UI thread touches this pointer;
// the worker has its own copy and frees it.
static covers_job_t *current_job;

// Runs on the UI thread and owns `user`.
static void covers_ready(void *user) {
	covers_result_t *result = user;
	if (!result) {
		return;
	}
	bool kept = false;
	if (result->gen == covers_gen && result->cb) {
		kept = result->cb(result->index, result->title, result->has_cover ? &result->cover : NULL);
	}
	if (result->has_cover && !kept) {
		cover_free(&result->cover);
	}
	free(result);
}

static void job_destroy(covers_job_t *job) {
	pthread_mutex_destroy(&job->lock);
	pthread_cond_destroy(&job->wake);
	free(job->files);
	free(job->done);
	free(job);
}

// The next book to read: the first one in the wanted range that has not been
// done. Waits when there is none, and returns -1 when the run is over.
static int next_index(covers_job_t *job) {
	pthread_mutex_lock(&job->lock);
	for (;;) {
		if (job->stopping || job->gen != covers_gen) {
			pthread_mutex_unlock(&job->lock);
			return -1;
		}
		int from = job->want_from < 0 ? 0 : job->want_from;
		int to = job->want_to >= job->count ? job->count - 1 : job->want_to;
		for (int i = from; i <= to; i++) {
			if (!job->done[i]) {
				job->done[i] = 1;
				pthread_mutex_unlock(&job->lock);
				return i;
			}
		}
		pthread_cond_wait(&job->wake, &job->lock);
	}
}

static void *worker_main(void *arg) {
	covers_job_t *job = arg;
	thread_be_background("ebookcovers");

	for (;;) {
		int index = next_index(job);
		if (index < 0) {
			break;
		}

		char path[800];
		snprintf(path, sizeof(path), "%s/%s", job->dir, job->files[index]);

		covers_result_t *result = calloc(1, sizeof(*result));
		if (!result) {
			continue;
		}
		result->gen = job->gen;
		result->index = index;
		result->cb = job->cb;

		// The cache first: a book seen before is one read, not a ZIP opened
		// and a picture decoded.
		char title[256];
		bool has_cover = false;
		if (cover_book_cached(path, job->box_w, job->box_h, &result->cover, &has_cover, title, sizeof(title))) {
			result->has_cover = has_cover;
		} else {
			uint32_t size = 0;
			void *bytes = ebook_peek_cover(path, &size, title, sizeof(title));
			if (bytes && size) {
				// Decoded straight to the size it is drawn at, so a
				// three-thousand pixel cover never exists at three thousand
				// pixels.
				result->has_cover =
					cover_load_image_memory(bytes, size, job->box_w, job->box_h, COVER_FIT_COVER, &result->cover);
				// A decode that failed is not kept: the budget it ran into is a
				// share of the memory free at that moment, and a minute later
				// the same cover may decode.
				if (result->has_cover) {
					cover_book_store(path, job->box_w, job->box_h, &result->cover, title);
				}
			} else if (title[0]) {
				// The book opened and has no cover. An empty title as well is a
				// book that may not have opened at all, and is asked again.
				cover_book_store(path, job->box_w, job->box_h, NULL, title);
			}
			free(bytes);
		}
		snprintf(result->title, sizeof(result->title), "%s", title[0] ? title : job->files[index]);

		if (job->gen != covers_gen || !gui_post(covers_ready, result)) {
			// The page went away while this book was being read, or the queue
			// was full: either way nobody is going to take this.
			if (result->has_cover) {
				cover_free(&result->cover);
			}
			free(result);
		}
	}

	job_destroy(job);
	return NULL;
}

void ebookcovers_stop(uint32_t token) {
	// Only the run that asked. A stale token is a page that has already been
	// replaced, and its stop has nothing left to stop.
	if (token != covers_gen) {
		return;
	}
	covers_gen++;
	if (current_job) {
		// Woken as well as told, or a worker asleep on the condition would sit
		// there until something else happened to signal it -- which, on a page
		// nobody is looking at any more, is never.
		pthread_mutex_lock(&current_job->lock);
		current_job->stopping = true;
		pthread_cond_signal(&current_job->wake);
		pthread_mutex_unlock(&current_job->lock);
		current_job = NULL;
	}
}

void ebookcovers_want(uint32_t token, int from, int to) {
	if (token != covers_gen || !current_job || to < from) {
		return;
	}
	pthread_mutex_lock(&current_job->lock);
	current_job->want_from = from;
	current_job->want_to = to;
	pthread_cond_signal(&current_job->wake);
	pthread_mutex_unlock(&current_job->lock);
}

void ebookcovers_want_visible(uint32_t token, lv_obj_t *view, lv_obj_t *const *items, int count, int32_t margin) {
	if (!view || !items || count <= 0) {
		return;
	}
	// The layout has to have happened, or every object is still at its starting
	// coordinates and the answer is "the first one, always".
	lv_obj_update_layout(view);

	lv_area_t visible;
	lv_obj_get_coords(view, &visible);

	int first = -1, last = -1;
	for (int i = 0; i < count; i++) {
		if (!items[i]) {
			continue;
		}
		lv_area_t area;
		lv_obj_get_coords(items[i], &area);
		if (area.y2 < visible.y1 - margin || area.y1 > visible.y2 + margin) {
			continue;
		}
		if (first < 0) {
			first = i;
		}
		last = i;
	}
	if (first >= 0) {
		ebookcovers_want(token, first, last);
	}
}

uint32_t ebookcovers_start(const char *dir, const char (*files)[256], int count, int box_w, int box_h,
						   ebookcovers_ready_cb cb) {
	ebookcovers_stop(covers_gen); // whatever was running belongs to a page that has moved on
	if (!dir || !files || count <= 0 || !cb) {
		return covers_gen;
	}

	covers_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return covers_gen;
	}
	job->files = calloc((size_t)count, sizeof(job->files[0]));
	job->done = calloc((size_t)count, 1);
	if (!job->files || !job->done) {
		free(job->files);
		free(job->done);
		free(job);
		return covers_gen;
	}
	pthread_mutex_init(&job->lock, NULL);
	pthread_cond_init(&job->wake, NULL);
	job->gen = covers_gen;
	job->count = count;
	job->box_w = box_w;
	job->box_h = box_h;
	job->cb = cb;
	// Nothing wanted yet: the page says what is on screen once it has laid
	// itself out, and until then the worker waits rather than starting at the
	// top of a list it may never need the top of.
	job->want_from = 0;
	job->want_to = -1;
	snprintf(job->dir, sizeof(job->dir), "%s", dir);
	memcpy(job->files, files, (size_t)count * sizeof(job->files[0]));

	pthread_t thread;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	// Detached: nobody joins it, so its own resources go when it returns
	// instead of waiting for a join that is never coming.
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	int rc = pthread_create(&thread, &attr, worker_main, job);
	pthread_attr_destroy(&attr);
	if (rc != 0) {
		job_destroy(job);
		return covers_gen;
	}
	current_job = job;
	return covers_gen;
}
