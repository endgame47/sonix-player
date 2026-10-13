#include "podcaststate.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/utils.h"
#include "src/system/db/sqlite3.h"
#include "src/system/library/metadata.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/podcastdl.h"

// The table lives in podcast.db with the followed podcasts (podcastsubs.c). The
// connection is kept open between pieces of work and opened again when the file
// under it has been replaced (copied over from a computer) or the card has
// changed: a connection kept across that points at nothing.
//
// `touched` grows by one at every write and orders "most recently left". A
// counter and not a clock, for the reason podcastsubs.c gives: the clock here
// starts at 1970 after every power-off without network.
//
// Paths are stored relative to the card, so a card read under another mount
// point keeps its rows.

#define WRITE_EVERY_MS 15000

// Below this an episode has not really been started: the first second is what
// the engine reports before a resume has landed.
#define START_SECONDS 5.0

static pthread_mutex_t db_lock = PTHREAD_MUTEX_INITIALIZER;
static char card_root[512];

// Bumped at every change worth redrawing a page for. Read from the GUI thread,
// written from there and from the writer: a torn read only means one redraw
// more or less.
static volatile unsigned revision;

unsigned podcaststate_revision(void) { return revision; }
static char db_path[600];

static uint32_t now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)(t.tv_sec * 1000u) + (uint32_t)(t.tv_nsec / 1000000);
}

// ---------------------------------------------------------------------------
// the database
// ---------------------------------------------------------------------------

static bool exec(sqlite3 *db, const char *sql) {
	char *error = NULL;
	if (sqlite3_exec(db, sql, NULL, NULL, &error) != SQLITE_OK) {
		fprintf(stderr, "podcaststate: %s\n", error ? error : "unknown error");
		sqlite3_free(error);
		return false;
	}
	return true;
}

static sqlite3 *conn;
static file_identity_t conn_identity;

// Called with db_lock held.
static void close_db(void) {
	if (conn) {
		sqlite3_close(conn);
		conn = NULL;
	}
}

static sqlite3 *connect_db(void);

// The open connection, opened when there is none or when the file was
// replaced. Called with db_lock held; nothing is closed by the caller.
static sqlite3 *open_db(void) {
	if (!db_path[0]) {
		return NULL;
	}
	if (conn && !file_identity_changed(db_path, &conn_identity)) {
		return conn;
	}
	close_db();
	conn = connect_db();
	if (conn) {
		file_identity_read(db_path, &conn_identity);
	}
	return conn;
}

// Called with db_lock held.
static sqlite3 *connect_db(void) {
	sqlite3 *db = NULL;
	if (sqlite3_open(db_path, &db) != SQLITE_OK) {
		fprintf(stderr, "podcaststate: cannot open %s: %s\n", db_path, sqlite3_errmsg(db));
		sqlite3_close(db);
		return NULL;
	}
	sqlite3_busy_timeout(db, 1000);
	exec(db, "PRAGMA synchronous=NORMAL");
	exec(db, "PRAGMA journal_mode=TRUNCATE");
	exec(db, "PRAGMA cache_size=-64");
	exec(db, "CREATE TABLE IF NOT EXISTS PODCAST_EPISODES("
			 "key TEXT PRIMARY KEY, path TEXT, title TEXT, show TEXT, image TEXT,"
			 "feed_id INTEGER DEFAULT 0, episode_id INTEGER DEFAULT 0,"
			 "position REAL DEFAULT 0, duration REAL DEFAULT 0, played INTEGER DEFAULT 0,"
			 "touched INTEGER DEFAULT 0)");
	exec(db, "CREATE INDEX IF NOT EXISTS PODCAST_EPISODES_PATH ON PODCAST_EPISODES(path)");
	exec(db, "CREATE INDEX IF NOT EXISTS PODCAST_EPISODES_FEED ON PODCAST_EPISODES(feed_id)");
	exec(db, "CREATE INDEX IF NOT EXISTS PODCAST_EPISODES_TOUCHED ON PODCAST_EPISODES(touched)");
	return db;
}

// `path` under the card, or the path itself when it is not under it.
static const char *relative(const char *path) {
	size_t n = strlen(card_root);
	if (n > 0 && strncmp(path, card_root, n) == 0 && path[n] == '/') {
		return path + n + 1;
	}
	return path;
}

static void absolute(const char *stored, char *out, size_t size) {
	if (!stored || !stored[0]) {
		out[0] = '\0';
	} else if (stored[0] == '/' || !card_root[0]) {
		snprintf(out, size, "%s", stored);
	} else {
		snprintf(out, size, "%.*s/%.*s", 255, card_root, 255, stored);
	}
}

// "e:<id>" for an episode of the catalogue, "f:<path under the card>" for a
// file of the Podcast folder that is not one. False for anything else.
static bool key_for(const char *path, long long episode_id, char *out, size_t size) {
	if (episode_id > 0) {
		snprintf(out, size, "e:%lld", episode_id);
		return true;
	}
	if (!path || !path[0] || !podcastdl_owns(path)) {
		return false;
	}
	snprintf(out, size, "f:%s", relative(path));
	return true;
}

static const char *const COLUMNS = "path,title,show,image,feed_id,episode_id,position,duration,played,touched";

static void read_row(sqlite3_stmt *stmt, podcaststate_t *out) {
	memset(out, 0, sizeof(*out));
	const char *path = (const char *)sqlite3_column_text(stmt, 0);
	const char *title = (const char *)sqlite3_column_text(stmt, 1);
	const char *show = (const char *)sqlite3_column_text(stmt, 2);
	const char *image = (const char *)sqlite3_column_text(stmt, 3);
	absolute(path, out->path, sizeof(out->path));
	snprintf(out->title, sizeof(out->title), "%s", title ? title : "");
	snprintf(out->show, sizeof(out->show), "%s", show ? show : "");
	snprintf(out->image, sizeof(out->image), "%s", image ? image : "");
	out->feed_id = (long long)sqlite3_column_int64(stmt, 4);
	out->episode_id = (long long)sqlite3_column_int64(stmt, 5);
	out->position = sqlite3_column_double(stmt, 6);
	out->duration = sqlite3_column_double(stmt, 7);
	out->played = sqlite3_column_int(stmt, 8) != 0;
	out->touched = (long long)sqlite3_column_int64(stmt, 9);
}

// Called with db_lock held.
static bool select_key(sqlite3 *db, const char *key, podcaststate_t *out) {
	char sql[200];
	snprintf(sql, sizeof(sql), "SELECT %s FROM PODCAST_EPISODES WHERE key=?", COLUMNS);
	sqlite3_stmt *stmt = NULL;
	bool found = false;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			read_row(stmt, out);
			found = true;
		}
		sqlite3_finalize(stmt);
	}
	return found;
}

// Called with db_lock held.
static void upsert(sqlite3 *db, const char *key, const podcaststate_t *s) {
	sqlite3_stmt *stmt = NULL;
	const char *sql = "INSERT INTO PODCAST_EPISODES(key,path,title,show,image,feed_id,episode_id,position,duration,"
					  "played,touched) VALUES(?,?,?,?,?,?,?,?,?,?,"
					  "(SELECT IFNULL(MAX(touched),0)+1 FROM PODCAST_EPISODES))"
					  " ON CONFLICT(key) DO UPDATE SET path=excluded.path, title=excluded.title,"
					  " show=excluded.show, image=excluded.image, feed_id=excluded.feed_id,"
					  " episode_id=excluded.episode_id, position=excluded.position, duration=excluded.duration,"
					  " played=excluded.played, touched=excluded.touched";
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		fprintf(stderr, "podcaststate: %s\n", sqlite3_errmsg(db));
		return;
	}
	sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, s->path[0] ? relative(s->path) : "", -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, s->title, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 4, s->show, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 5, s->image, -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt, 6, (sqlite3_int64)s->feed_id);
	sqlite3_bind_int64(stmt, 7, (sqlite3_int64)s->episode_id);
	sqlite3_bind_double(stmt, 8, s->position);
	sqlite3_bind_double(stmt, 9, s->duration);
	sqlite3_bind_int(stmt, 10, s->played ? 1 : 0);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		fprintf(stderr, "podcaststate: write failed: %s\n", sqlite3_errmsg(db));
	}
	sqlite3_finalize(stmt);
}

// An episode of the catalogue kept in the Podcast folder stays tied to that
// file when it is played from anywhere else (the cache, a stream): the card's
// pages find its state by the file. Called with db_lock held.
static void keep_download(sqlite3 *db, const char *key, podcaststate_t *s) {
	if (strncmp(key, "e:", 2) != 0 || (s->path[0] && podcastdl_owns(s->path))) {
		return;
	}
	podcaststate_t old;
	if (select_key(db, key, &old) && old.path[0] && podcastdl_owns(old.path) && access(old.path, F_OK) == 0) {
		snprintf(s->path, sizeof(s->path), "%s", old.path);
	}
}

static void write_now(const char *key, const podcaststate_t *s) {
	revision++;
	podcaststate_t record = *s;
	pthread_mutex_lock(&db_lock);
	sqlite3 *db = open_db();
	if (db) {
		keep_download(db, key, &record);
		upsert(db, key, &record);
	}
	pthread_mutex_unlock(&db_lock);
}

// ---------------------------------------------------------------------------
// the writer
//
// One record waiting at most: every note is about the episode playing, and a
// newer note of it replaces an older one. A note of another episode arriving
// while one is waiting is the track having changed, and that one is written
// first, through, on the caller's thread.
// ---------------------------------------------------------------------------

static pthread_mutex_t writer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t writer_wake = PTHREAD_COND_INITIALIZER;
static bool writer_started;
static bool writer_has;
static char writer_key[600];
static podcaststate_t writer_record;

static void *writer_main(void *arg) {
	(void)arg;
	thread_be_background("podcast state");
	for (;;) {
		pthread_mutex_lock(&writer_lock);
		while (!writer_has) {
			pthread_cond_wait(&writer_wake, &writer_lock);
		}
		char key[sizeof(writer_key)];
		podcaststate_t record = writer_record;
		snprintf(key, sizeof(key), "%s", writer_key);
		writer_has = false;
		pthread_mutex_unlock(&writer_lock);
		write_now(key, &record);
	}
	return NULL;
}

static void writer_post(const char *key, const podcaststate_t *s) {
	pthread_mutex_lock(&writer_lock);
	if (!writer_started) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, writer_main, NULL) == 0) {
			pthread_detach(thread);
			writer_started = true;
		}
	}
	if (!writer_started) {
		pthread_mutex_unlock(&writer_lock);
		write_now(key, s);
		return;
	}
	snprintf(writer_key, sizeof(writer_key), "%s", key);
	writer_record = *s;
	writer_has = true;
	pthread_cond_signal(&writer_wake);
	pthread_mutex_unlock(&writer_lock);
}

// Whatever is waiting, written on this thread.
static void writer_drain(void) {
	pthread_mutex_lock(&writer_lock);
	bool has = writer_has;
	char key[sizeof(writer_key)];
	podcaststate_t record = writer_record;
	snprintf(key, sizeof(key), "%s", writer_key);
	writer_has = false;
	pthread_mutex_unlock(&writer_lock);
	if (has) {
		write_now(key, &record);
	}
}

// ---------------------------------------------------------------------------
// the episode playing
// ---------------------------------------------------------------------------

// Written by the GUI thread's notes, read by whoever asks -- the page's worker
// starts episodes too -- so behind a lock of its own. Never held while taking
// db_lock.
static pthread_mutex_t current_lock = PTHREAD_MUTEX_INITIALIZER;
static podcaststate_t current;
static char current_key[600];
static bool current_valid;
static bool current_dirty;
static uint32_t current_written_ms;

// A resume asked for on this file: notes from before the engine got there
// would otherwise write the start back over it.
static char expect_path[512];
static double expect_seconds;
static uint32_t expect_since_ms;

// The file's name as its title: no folder, no extension, and no date in front
// ("2026-10-05 Episode" is listed by its date already).
static void file_title(const char *path, char *out, size_t size) {
	const char *slash = strrchr(path, '/');
	const char *name = slash ? slash + 1 : path;
	int digits = 0;
	for (const char *p = name; *p && digits < 8; p++) {
		if (*p >= '0' && *p <= '9') {
			digits++;
		} else if (*p != '-' && *p != '.' && *p != '_') {
			break;
		}
	}
	if (digits == 8) {
		const char *p = name;
		while (*p == '-' || *p == '.' || *p == '_' || (*p >= '0' && *p <= '9')) {
			p++;
		}
		while (*p == ' ' || *p == '-' || *p == '_') {
			p++;
		}
		if (*p && *p != '.') {
			name = p;
		}
	}
	snprintf(out, size, "%.*s", (int)size - 1, name);
	char *dot = strrchr(out, '.');
	if (dot && dot != out) {
		*dot = '\0';
	}
}

// The parent folder's name: the podcast, for a file of the Podcast folder.
static void folder_name(const char *path, char *out, size_t size) {
	out[0] = '\0';
	const char *root = podcastdl_root();
	size_t n = strlen(root);
	if (!n || strncmp(path, root, n) != 0 || path[n] != '/') {
		return;
	}
	const char *rest = path + n + 1;
	const char *slash = strchr(rest, '/');
	if (!slash) {
		return; // straight in the Podcast folder: no podcast of its own
	}
	snprintf(out, size, "%.*s", (int)(slash - rest), rest);
}

// Fills in what the row says about the episode, from the sidecar when it has
// one and from the file's own tags otherwise.
static void describe(const char *path, podcaststate_t *s) {
	snprintf(s->path, sizeof(s->path), "%s", path);
	char text[32];
	s->episode_id = podcastcache_episode_id(path);
	if (podcastcache_tag(path, "feed_id", text, sizeof(text))) {
		s->feed_id = strtoll(text, NULL, 10);
	}
	if (!podcastcache_tag(path, "title", s->title, sizeof(s->title))) {
		song_metadata_t *m = malloc(sizeof(*m));
		if (m) {
			metadata_read(path, m);
			snprintf(s->title, sizeof(s->title), "%.*s", (int)sizeof(s->title) - 1, m->title);
			if (!s->show[0] && m->album[0]) {
				snprintf(s->show, sizeof(s->show), "%.*s", (int)sizeof(s->show) - 1, m->album);
			}
			free(m);
		}
		if (!s->title[0]) {
			file_title(path, s->title, sizeof(s->title));
		}
	}
	char show[PODCAST_TITLE_MAX];
	if (podcastcache_tag(path, "album", show, sizeof(show))) {
		snprintf(s->show, sizeof(s->show), "%s", show);
	} else if (!s->show[0]) {
		folder_name(path, s->show, sizeof(s->show));
	}
	podcastcache_tag(path, "feed_image", s->image, sizeof(s->image));
}

static void current_write(bool through) {
	pthread_mutex_lock(&current_lock);
	if (!current_valid || !current_dirty) {
		pthread_mutex_unlock(&current_lock);
		return;
	}
	current_dirty = false;
	current_written_ms = now_ms();
	podcaststate_t record = current;
	char key[sizeof(current_key)];
	snprintf(key, sizeof(key), "%s", current_key);
	pthread_mutex_unlock(&current_lock);
	if (through) {
		writer_drain();
		write_now(key, &record);
	} else {
		writer_post(key, &record);
	}
}

static bool current_load(const char *path) {
	podcaststate_t s;
	memset(&s, 0, sizeof(s));
	describe(path, &s);
	char key[sizeof(current_key)];
	if (!key_for(path, s.episode_id, key, sizeof(key))) {
		return false;
	}
	// What is known already: whether it was heard, how long it is.
	podcaststate_t known;
	pthread_mutex_lock(&db_lock);
	sqlite3 *db = open_db();
	bool found = db && select_key(db, key, &known);
	pthread_mutex_unlock(&db_lock);
	if (found) {
		s.position = known.position;
		s.duration = known.duration;
		s.played = known.played;
		if (!s.image[0]) {
			snprintf(s.image, sizeof(s.image), "%s", known.image);
		}
	}
	pthread_mutex_lock(&current_lock);
	current = s;
	snprintf(current_key, sizeof(current_key), "%s", key);
	current_valid = true;
	current_dirty = false;
	current_written_ms = now_ms();
	pthread_mutex_unlock(&current_lock);
	return true;
}

void podcaststate_note(const char *path, double seconds, double total, bool force) {
	if (!path || !path[0] || !db_path[0]) {
		return;
	}
	pthread_mutex_lock(&current_lock);
	bool same = current_valid && strcmp(current.path, path) == 0;
	pthread_mutex_unlock(&current_lock);
	if (!same) {
		// The track changed: the last word on the one before is written now.
		current_write(false);
		pthread_mutex_lock(&current_lock);
		current_valid = false;
		pthread_mutex_unlock(&current_lock);
		if (!current_load(path)) {
			return;
		}
	}

	pthread_mutex_lock(&current_lock);
	// Before the resume has landed the engine reports the start of the file.
	if (expect_path[0] && strcmp(expect_path, path) == 0) {
		if (seconds < expect_seconds - 10.0 && now_ms() - expect_since_ms < 8000) {
			pthread_mutex_unlock(&current_lock);
			return;
		}
		expect_path[0] = '\0';
	}

	double was_position = current.position;
	double was_duration = current.duration;
	if (total > 0) {
		current.duration = total;
	}
	int margin = podcaststate_margin();
	// An episode shorter than twice the margin would count as heard from its
	// first half; for those only its last seconds do.
	double end_zone = total > 2.0 * margin ? (double)margin : 3.0;
	bool was_played = current.played;

	if (total > 0 && seconds > 0 && seconds >= total - end_zone) {
		current.played = true;
		current.position = 0;
	} else if (seconds > START_SECONDS) {
		// Heard to the end once, and now well into it again: listening again.
		if (current.played && seconds > 30.0) {
			current.played = false;
		}
		if (!current.played) {
			current.position = seconds;
		}
	} else {
		pthread_mutex_unlock(&current_lock);
		return; // the first seconds say nothing worth keeping
	}
	// Paused, the same second comes back on every refresh: nothing to write.
	if (current.played != was_played || current.position < was_position - 0.5 ||
		current.position > was_position + 0.5 || current.duration != was_duration) {
		current_dirty = true;
	}
	bool due = current_dirty &&
			   (force || current.played != was_played || now_ms() - current_written_ms >= WRITE_EVERY_MS);
	pthread_mutex_unlock(&current_lock);

	if (due) {
		current_write(false);
	}
}

void podcaststate_flush(void) {
	current_write(true);
	writer_drain();
}

// ---------------------------------------------------------------------------
// asking
// ---------------------------------------------------------------------------

static bool get_by_key(const char *key, podcaststate_t *out) {
	// The episode playing answers from memory: its latest note may still be
	// waiting for the writer.
	pthread_mutex_lock(&current_lock);
	if (current_valid && strcmp(current_key, key) == 0) {
		*out = current;
		out->touched = PODCASTSTATE_TOUCHED_NOW;
		pthread_mutex_unlock(&current_lock);
		return true;
	}
	pthread_mutex_unlock(&current_lock);
	pthread_mutex_lock(&db_lock);
	sqlite3 *db = open_db();
	bool found = db && select_key(db, key, out);
	pthread_mutex_unlock(&db_lock);
	return found;
}

bool podcaststate_get(const char *path, podcaststate_t *out) {
	if (!path || !out || !db_path[0]) {
		return false;
	}
	char key[600];
	if (!key_for(path, podcastcache_episode_id(path), key, sizeof(key))) {
		return false;
	}
	return get_by_key(key, out);
}

bool podcaststate_get_episode(long long episode_id, podcaststate_t *out) {
	if (episode_id <= 0 || !out || !db_path[0]) {
		return false;
	}
	char key[64];
	snprintf(key, sizeof(key), "e:%lld", episode_id);
	return get_by_key(key, out);
}

double podcaststate_resume_point(const char *path) {
	podcaststate_t s;
	if (!podcaststate_get(path, &s) || s.played || s.position <= START_SECONDS) {
		return 0;
	}
	// A few seconds back, so the sentence that was cut off is heard whole.
	double at = s.position - 3.0;
	if (s.duration > 0 && at >= s.duration - 1.0) {
		return 0;
	}
	pthread_mutex_lock(&current_lock);
	snprintf(expect_path, sizeof(expect_path), "%s", path);
	expect_seconds = at;
	expect_since_ms = now_ms();
	pthread_mutex_unlock(&current_lock);
	return at;
}

void podcaststate_set_played(const char *path, bool played) {
	if (!path || !path[0] || !db_path[0]) {
		return;
	}
	podcaststate_t s;
	memset(&s, 0, sizeof(s));
	char key[600];
	long long id = podcastcache_episode_id(path);
	if (!key_for(path, id, key, sizeof(key))) {
		return;
	}
	if (!get_by_key(key, &s)) {
		memset(&s, 0, sizeof(s));
		describe(path, &s);
	}
	snprintf(s.path, sizeof(s.path), "%s", path);
	s.played = played;
	if (played) {
		s.position = 0;
	}
	pthread_mutex_lock(&current_lock);
	if (current_valid && strcmp(current_key, key) == 0) {
		current.played = s.played;
		current.position = s.position;
		current_dirty = false;
	}
	pthread_mutex_unlock(&current_lock);
	writer_drain();
	write_now(key, &s);
}

void podcaststate_forget(const char *path) {
	if (!path || !path[0] || !db_path[0]) {
		return;
	}
	// A file copied by hand has no state apart from its file, and goes with
	// it. An episode of the catalogue keeps where it was left and whether it was
	// heard: it can still be streamed, and its row moves to its copy in the
	// cache, or to no file at all.
	char key[600];
	bool own_row = key_for(path, 0, key, sizeof(key));
	pthread_mutex_lock(&current_lock);
	if (current_valid && strcmp(current.path, path) == 0) {
		current_valid = false;
	}
	pthread_mutex_unlock(&current_lock);
	writer_drain();
	pthread_mutex_lock(&db_lock);
	sqlite3 *db = open_db();
	revision++;
	if (db) {
		sqlite3_stmt *stmt = NULL;
		if (own_row &&
			sqlite3_prepare_v2(db, "DELETE FROM PODCAST_EPISODES WHERE key=?", -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
			sqlite3_step(stmt);
			sqlite3_finalize(stmt);
		}
		// One episode at most, in practice: a file is one episode's.
		long long episodes[4];
		int count = 0;
		stmt = NULL;
		if (sqlite3_prepare_v2(db, "SELECT episode_id FROM PODCAST_EPISODES WHERE path=? AND episode_id>0", -1,
							   &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, relative(path), -1, SQLITE_STATIC);
			while (count < 4 && sqlite3_step(stmt) == SQLITE_ROW) {
				episodes[count++] = (long long)sqlite3_column_int64(stmt, 0);
			}
			sqlite3_finalize(stmt);
		}
		for (int i = 0; i < count; i++) {
			char cached[512] = "";
			if (!podcastcache_find(episodes[i], cached, sizeof(cached))) {
				cached[0] = '\0';
			}
			char episode_key[64];
			snprintf(episode_key, sizeof(episode_key), "e:%lld", episodes[i]);
			stmt = NULL;
			if (sqlite3_prepare_v2(db, "UPDATE PODCAST_EPISODES SET path=? WHERE key=?", -1, &stmt, NULL) ==
				SQLITE_OK) {
				sqlite3_bind_text(stmt, 1, cached[0] ? relative(cached) : "", -1, SQLITE_STATIC);
				sqlite3_bind_text(stmt, 2, episode_key, -1, SQLITE_STATIC);
				sqlite3_step(stmt);
				sqlite3_finalize(stmt);
			}
		}
	}
	pthread_mutex_unlock(&db_lock);
}

void podcaststate_downloaded(long long episode_id, const char *path) {
	if (episode_id <= 0 || !path || !path[0] || !db_path[0] || !podcastdl_owns(path)) {
		return;
	}
	char key[64];
	snprintf(key, sizeof(key), "e:%lld", episode_id);
	pthread_mutex_lock(&db_lock);
	sqlite3 *db = open_db();
	podcaststate_t old;
	// Never played: nothing to move. Already on a file of the folder that is
	// still there: that one stays.
	if (db && select_key(db, key, &old) && strcmp(old.path, path) != 0 &&
		!(old.path[0] && podcastdl_owns(old.path) && access(old.path, F_OK) == 0)) {
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(db, "UPDATE PODCAST_EPISODES SET path=? WHERE key=?", -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, relative(path), -1, SQLITE_STATIC);
			sqlite3_bind_text(stmt, 2, key, -1, SQLITE_STATIC);
			if (sqlite3_step(stmt) == SQLITE_DONE) {
				revision++;
			}
			sqlite3_finalize(stmt);
		}
	}
	pthread_mutex_unlock(&db_lock);
}

// Rows from `where` (with one text or one integer to bind), handed out with the
// episode playing laid over its own row.
static int select_many(const char *where, const char *text, const char *text_end, long long number,
					   podcaststate_cb_t cb, void *user) {
	if (!cb || !db_path[0]) {
		return 0;
	}
	char sql[300];
	snprintf(sql, sizeof(sql), "SELECT %s,key FROM PODCAST_EPISODES WHERE %s", COLUMNS, where);
	int n = 0;
	pthread_mutex_lock(&db_lock);
	sqlite3 *db = open_db();
	if (db) {
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
			if (text) {
				sqlite3_bind_text(stmt, 1, text, -1, SQLITE_STATIC);
				if (text_end) {
					sqlite3_bind_text(stmt, 2, text_end, -1, SQLITE_STATIC);
				}
			} else {
				sqlite3_bind_int64(stmt, 1, (sqlite3_int64)number);
			}
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				podcaststate_t s;
				read_row(stmt, &s);
				const char *key = (const char *)sqlite3_column_text(stmt, 10);
				pthread_mutex_lock(&current_lock);
				if (current_valid && key && strcmp(key, current_key) == 0) {
					// The file stays the row's: the episode may be playing from
					// the cache while the row points at its download.
					char path[sizeof(s.path)];
					snprintf(path, sizeof(path), "%s", s.path);
					s = current;
					snprintf(s.path, sizeof(s.path), "%s", path);
					s.touched = PODCASTSTATE_TOUCHED_NOW;
				}
				pthread_mutex_unlock(&current_lock);
				cb(&s, user);
				n++;
			}
			sqlite3_finalize(stmt);
		}
	}
	pthread_mutex_unlock(&db_lock);
	return n;
}

int podcaststate_in_folder(const char *folder, podcaststate_cb_t cb, void *user) {
	if (!folder || !folder[0]) {
		return 0;
	}
	// Everything from "<folder>/" up to, but not including, "<folder>0": '0'
	// is the character after '/', so the range is exactly what is inside.
	char from[600];
	char to[600];
	snprintf(from, sizeof(from), "%s/", relative(folder));
	snprintf(to, sizeof(to), "%s0", relative(folder));
	return select_many("path>=? AND path<?", from, to, 0, cb, user);
}

int podcaststate_for_feed(long long feed_id, podcaststate_cb_t cb, void *user) {
	if (feed_id <= 0) {
		return 0;
	}
	return select_many("feed_id=?", NULL, NULL, feed_id, cb, user);
}

int podcaststate_in_progress(podcaststate_t *out, int max) {
	if (!out || max <= 0 || !db_path[0]) {
		return 0;
	}
	int n = 0;
	char current_copy_key[sizeof(current_key)] = "";
	pthread_mutex_lock(&current_lock);
	if (current_valid && !current.played && current.position > START_SECONDS) {
		out[n] = current;
		out[n].touched = PODCASTSTATE_TOUCHED_NOW;
		snprintf(current_copy_key, sizeof(current_copy_key), "%s", current_key);
		n++;
	}
	pthread_mutex_unlock(&current_lock);

	char sql[240];
	snprintf(sql, sizeof(sql),
			 "SELECT %s,key FROM PODCAST_EPISODES WHERE played=0 AND position>%d ORDER BY touched DESC LIMIT %d",
			 COLUMNS, (int)START_SECONDS, max + 1);
	pthread_mutex_lock(&db_lock);
	sqlite3 *db = open_db();
	if (db) {
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
			while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
				const char *key = (const char *)sqlite3_column_text(stmt, 10);
				if (key && current_copy_key[0] && strcmp(key, current_copy_key) == 0) {
					continue; // already first, from memory
				}
				read_row(stmt, &out[n++]);
			}
			sqlite3_finalize(stmt);
		}
	}
	pthread_mutex_unlock(&db_lock);
	return n;
}

void podcaststate_set_played_episode(const podcast_episode_t *episode, const char *image, bool played) {
	if (!episode || episode->id <= 0 || !db_path[0]) {
		return;
	}
	char key[64];
	snprintf(key, sizeof(key), "e:%lld", episode->id);
	podcaststate_t s;
	if (!get_by_key(key, &s)) {
		memset(&s, 0, sizeof(s));
		snprintf(s.title, sizeof(s.title), "%s", episode->title);
		snprintf(s.show, sizeof(s.show), "%s", episode->feed_title);
		snprintf(s.image, sizeof(s.image), "%s", image && image[0] ? image : episode->image);
		s.feed_id = episode->feed_id;
		s.episode_id = episode->id;
		s.duration = episode->duration_secs;
	}
	s.played = played;
	if (played) {
		s.position = 0;
	}
	pthread_mutex_lock(&current_lock);
	if (current_valid && strcmp(current_key, key) == 0) {
		current.played = s.played;
		current.position = s.position;
		current_dirty = false;
	}
	pthread_mutex_unlock(&current_lock);
	writer_drain();
	write_now(key, &s);
}

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------

void podcaststate_set_root(const char *sd_root) {
	writer_drain();
	pthread_mutex_lock(&current_lock);
	current_valid = false;
	pthread_mutex_unlock(&current_lock);
	pthread_mutex_lock(&db_lock);
	close_db();
	if (!sd_root || !sd_root[0]) {
		card_root[0] = '\0';
		db_path[0] = '\0';
	} else {
		snprintf(card_root, sizeof(card_root), "%s", sd_root);
		char dir[560];
		snprintf(dir, sizeof(dir), "%.500s/.local", sd_root);
		mkdir(dir, 0777);
		snprintf(db_path, sizeof(db_path), "%s/podcast.db", dir);
	}
	pthread_mutex_unlock(&db_lock);
}

int podcaststate_margin(void) {
	long value = config_get_int("podcast", "played_margin", 30);
	return value == 60 || value == 120 ? (int)value : 30;
}

void podcaststate_set_margin(int seconds) {
	config_set_int("podcast", "played_margin", seconds == 60 || seconds == 120 ? seconds : 30);
	config_save();
}
