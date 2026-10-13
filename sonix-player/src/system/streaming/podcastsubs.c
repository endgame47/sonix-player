#include "podcastsubs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/utils.h"
#include "src/system/db/sqlite3.h"

// Followed podcasts, in an SQLite database: <card>/.local/podcast.db.
//
// SQLite rather than a home-grown format, because the card already carries a
// database for the library and one for audiobooks: the same atomic writes and
// one record per podcast, with code already in the binary, and room for further
// per-subscription fields (last episode played, a counter, a date).
//
// The list is held in memory: the lists read it from the worker thread and the
// star from the GUI thread, and a hundred-entry array re-read on every change
// is simpler and sturdier than locked queries. The database is the on-disk copy
// of the array, not the other way round.
//
// A podcast-subs.ini sitting next to the database is imported once: its lines
// become records and the file is renamed rather than deleted, since a list of
// podcasts is the user's own work. The order (newest first) survives.

static char db_path[512];
static char legacy_path[512];
static podcast_feed_t subs[PODCASTSUBS_MAX];
static int subs_count;
// Which file the list in memory was last read from or written to.
static file_identity_t db_identity;

static bool exec(sqlite3 *db, const char *sql) {
	char *error = NULL;
	if (sqlite3_exec(db, sql, NULL, NULL, &error) != SQLITE_OK) {
		fprintf(stderr, "podcastsubs: %s\n", error ? error : "unknown error");
		sqlite3_free(error);
		return false;
	}
	return true;
}

// The feed address, which a list written before it was kept does not have.
// Added in place: the rows stay, with an empty address until the catalogue is
// asked (podcastsubs_missing_urls).
static void add_url_column(sqlite3 *db) {
	sqlite3_stmt *stmt = NULL;
	bool found = false;
	if (sqlite3_prepare_v2(db, "PRAGMA table_info(PODCAST_SUBS)", -1, &stmt, NULL) == SQLITE_OK) {
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *name = (const char *)sqlite3_column_text(stmt, 1);
			if (name && strcmp(name, "url") == 0) {
				found = true;
			}
		}
		sqlite3_finalize(stmt);
	}
	if (!found) {
		exec(db, "ALTER TABLE PODCAST_SUBS ADD COLUMN url TEXT DEFAULT ''");
	}
}

static sqlite3 *open_db(void) {
	if (!db_path[0]) {
		return NULL;
	}
	sqlite3 *db = NULL;
	if (sqlite3_open(db_path, &db) != SQLITE_OK) {
		fprintf(stderr, "podcastsubs: cannot open %s: %s\n", db_path, sqlite3_errmsg(db));
		sqlite3_close(db);
		return NULL;
	}
	// Same choices as the library and audiobook databases, for the same reason:
	// a slow card that can be pulled at any moment.
	//
	// `followed_at` grows by one on every star and defines the list order,
	// highest first. A counter rather than a real timestamp, because this
	// device's clock restarts at 1970 after every power-off without network, and
	// sorting by a clock that runs backwards would scramble the list.
	exec(db, "PRAGMA synchronous=NORMAL");
	exec(db, "PRAGMA journal_mode=TRUNCATE");
	exec(db, "PRAGMA cache_size=-64");
	exec(db, "CREATE TABLE IF NOT EXISTS PODCAST_SUBS("
			 "id INTEGER PRIMARY KEY, title TEXT, author TEXT, image TEXT,"
			 "followed_at INTEGER DEFAULT 0)");
	add_url_column(db);
	return db;
}

// The database is a copy of the array: it is rewritten whole inside a
// transaction, so the disk always holds either the previous list or the new one,
// never a mixture. A hundred records are nothing for SQLite, and rewriting
// everything cannot drift out of sync with memory the way piecemeal UPDATEs can.
static void save(void) {
	sqlite3 *db = open_db();
	if (!db) {
		return;
	}
	if (!exec(db, "BEGIN")) {
		sqlite3_close(db);
		return;
	}
	exec(db, "DELETE FROM PODCAST_SUBS");

	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "INSERT INTO PODCAST_SUBS(id,title,author,image,followed_at,url) VALUES(?,?,?,?,?,?)",
						   -1, &stmt, NULL) == SQLITE_OK) {
		for (int i = 0; i < subs_count; i++) {
			sqlite3_bind_int64(stmt, 1, (sqlite3_int64)subs[i].id);
			sqlite3_bind_text(stmt, 2, subs[i].title, -1, SQLITE_STATIC);
			sqlite3_bind_text(stmt, 3, subs[i].author, -1, SQLITE_STATIC);
			sqlite3_bind_text(stmt, 4, subs[i].image, -1, SQLITE_STATIC);
			// The array order is the order: the first entry is the most recently
			// followed and takes the highest number.
			sqlite3_bind_int(stmt, 5, subs_count - i);
			sqlite3_bind_text(stmt, 6, subs[i].url, -1, SQLITE_STATIC);
			if (sqlite3_step(stmt) != SQLITE_DONE) {
				fprintf(stderr, "podcastsubs: insert failed: %s\n", sqlite3_errmsg(db));
			}
			sqlite3_reset(stmt);
		}
		sqlite3_finalize(stmt);
	}
	exec(db, "COMMIT");
	sqlite3_close(db);
	file_identity_read(db_path, &db_identity);
}

static void load(void) {
	subs_count = 0;
	sqlite3 *db = open_db();
	if (!db) {
		return;
	}
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT id,title,author,image,url FROM PODCAST_SUBS ORDER BY followed_at DESC", -1,
						   &stmt, NULL) == SQLITE_OK) {
		while (subs_count < PODCASTSUBS_MAX && sqlite3_step(stmt) == SQLITE_ROW) {
			podcast_feed_t feed = {0};
			feed.id = (long long)sqlite3_column_int64(stmt, 0);
			if (feed.id <= 0) {
				continue;
			}
			const char *title = (const char *)sqlite3_column_text(stmt, 1);
			const char *author = (const char *)sqlite3_column_text(stmt, 2);
			const char *image = (const char *)sqlite3_column_text(stmt, 3);
			const char *url = (const char *)sqlite3_column_text(stmt, 4);
			snprintf(feed.title, sizeof(feed.title), "%s", title ? title : "");
			snprintf(feed.author, sizeof(feed.author), "%s", author ? author : "");
			snprintf(feed.image, sizeof(feed.image), "%s", image ? image : "");
			snprintf(feed.url, sizeof(feed.url), "%s", url ? url : "");
			subs[subs_count++] = feed;
		}
		sqlite3_finalize(stmt);
	}
	sqlite3_close(db);
	file_identity_read(db_path, &db_identity);
}

// The legacy podcast-subs.ini: id|title|author|cover, one line per podcast,
// newest first. When it exists, its lines move into the database and the file
// is renamed, once.
static void import_legacy(void) {
	if (!legacy_path[0]) {
		return;
	}
	FILE *f = fopen(legacy_path, "r");
	if (!f) {
		return;
	}

	printf("podcastsubs: importing %s into %s\n", legacy_path, db_path);
	int imported = 0;
	char line[1024];
	while (subs_count < PODCASTSUBS_MAX && fgets(line, sizeof(line), f)) {
		char *nl = strpbrk(line, "\r\n");
		if (nl) {
			*nl = '\0';
		}
		if (line[0] == '\0' || line[0] == '#') {
			continue;
		}

		podcast_feed_t feed = {0};
		char *cursor = line;
		char *bar = strchr(cursor, '|');
		if (bar) {
			*bar = '\0';
		}
		feed.id = strtoll(cursor, NULL, 10);
		if (feed.id <= 0) {
			continue;
		}
		if (bar) {
			char *rest[3] = {feed.title, feed.author, feed.image};
			size_t sizes[3] = {sizeof(feed.title), sizeof(feed.author), sizeof(feed.image)};
			cursor = bar + 1;
			for (int i = 0; i < 3 && cursor; i++) {
				bar = strchr(cursor, '|');
				if (bar) {
					*bar = '\0';
				}
				snprintf(rest[i], sizes[i], "%s", cursor);
				cursor = bar ? bar + 1 : NULL;
			}
		}

		// On a duplicate, the database copy wins: it is newer by construction,
		// since the file stops being written the moment the database exists.
		bool known = false;
		for (int i = 0; i < subs_count; i++) {
			if (subs[i].id == feed.id) {
				known = true;
				break;
			}
		}
		if (!known) {
			subs[subs_count++] = feed;
			imported++;
		}
	}
	fclose(f);

	if (imported > 0) {
		save();
	}

	// Renamed, not deleted: if anything went wrong the file is still there,
	// readable in a text editor.
	char moved[544];
	snprintf(moved, sizeof(moved), "%.500s.importato", legacy_path);
	if (rename(legacy_path, moved) != 0) {
		fprintf(stderr, "podcastsubs: cannot move %s aside: %s\n", legacy_path, strerror(errno));
	}
	printf("podcastsubs: imported %d podcasts from the old list\n", imported);
}

void podcastsubs_set_root(const char *sd_root) {
	if (!sd_root || !*sd_root) {
		db_path[0] = '\0';
		legacy_path[0] = '\0';
		subs_count = 0;
		return;
	}
	// The same hidden folder as everything else: the podcast cache, the library,
	// the radio stations.
	char dir[512];
	snprintf(dir, sizeof(dir), "%.480s/.local", sd_root);
	mkdir(dir, 0777);
	snprintf(db_path, sizeof(db_path), "%.480s/podcast.db", dir);
	snprintf(legacy_path, sizeof(legacy_path), "%.480s/podcast-subs.ini", dir);
	load();
	import_legacy();
}

void podcastsubs_reload_if_replaced(void) {
	if (!db_path[0] || !file_identity_changed(db_path, &db_identity)) {
		return;
	}
	printf("podcastsubs: %s was deleted or replaced; reading it again\n", db_path);
	// The folder may have gone with it.
	char dir[sizeof(db_path)];
	snprintf(dir, sizeof(dir), "%s", db_path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		mkdir(dir, 0777);
	}
	load();
}

int podcastsubs_count(void) { return subs_count; }

int podcastsubs_list(podcast_feed_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	int n = subs_count < max ? subs_count : max;
	memcpy(out, subs, sizeof(*out) * (size_t)n);
	return n;
}

bool podcastsubs_is_followed(long long feed_id) {
	for (int i = 0; i < subs_count; i++) {
		if (subs[i].id == feed_id) {
			return true;
		}
	}
	return false;
}

void podcastsubs_follow(const podcast_feed_t *feed) {
	if (!feed || feed->id <= 0) {
		return;
	}

	// Already followed: refresh the stored entry (the title may have changed)
	// and move it to the head, which is the order the list is read in. The
	// player's star follows from the sidecar, which has no feed address, so an
	// address already known is kept rather than wiped.
	for (int i = 0; i < subs_count; i++) {
		if (subs[i].id == feed->id) {
			char url[sizeof(subs[0].url)];
			snprintf(url, sizeof(url), "%s", subs[i].url);
			memmove(&subs[1], &subs[0], sizeof(subs[0]) * (size_t)i);
			subs[0] = *feed;
			if (!subs[0].url[0]) {
				snprintf(subs[0].url, sizeof(subs[0].url), "%s", url);
			}
			save();
			return;
		}
	}

	if (subs_count >= PODCASTSUBS_MAX) {
		// Full: the oldest entry goes. Better than refusing, which would mean a
		// star that can be pressed and does nothing.
		subs_count = PODCASTSUBS_MAX - 1;
	}
	memmove(&subs[1], &subs[0], sizeof(subs[0]) * (size_t)subs_count);
	subs[0] = *feed;
	subs_count++;
	save();
}

void podcastsubs_unfollow(long long feed_id) {
	for (int i = 0; i < subs_count; i++) {
		if (subs[i].id != feed_id) {
			continue;
		}
		memmove(&subs[i], &subs[i + 1], sizeof(subs[0]) * (size_t)(subs_count - i - 1));
		subs_count--;
		save();
		return;
	}
}

int podcastsubs_follow_batch(const podcast_feed_t *feeds, int count) {
	if (!feeds || count <= 0) {
		return 0;
	}
	int added = 0;
	// In the order given, each ahead of the ones before it, as a row of single
	// follows would leave them.
	for (int k = 0; k < count && subs_count < PODCASTSUBS_MAX; k++) {
		const podcast_feed_t *feed = &feeds[k];
		if (feed->id <= 0 || podcastsubs_is_followed(feed->id)) {
			continue;
		}
		memmove(&subs[1], &subs[0], sizeof(subs[0]) * (size_t)subs_count);
		subs[0] = *feed;
		subs_count++;
		added++;
	}
	if (added > 0) {
		save();
	}
	return added;
}

int podcastsubs_missing_urls(long long *ids_out, int max) {
	int n = 0;
	for (int i = 0; i < subs_count && n < max; i++) {
		if (!subs[i].url[0]) {
			ids_out[n++] = subs[i].id;
		}
	}
	return n;
}

void podcastsubs_set_url(long long feed_id, const char *url) {
	if (!url || !url[0]) {
		return;
	}
	for (int i = 0; i < subs_count; i++) {
		if (subs[i].id == feed_id) {
			if (strcmp(subs[i].url, url) != 0) {
				snprintf(subs[i].url, sizeof(subs[i].url), "%s", url);
				save();
			}
			return;
		}
	}
}

bool podcastsubs_is_followed_url(const char *url) {
	if (!url || !url[0]) {
		return false;
	}
	for (int i = 0; i < subs_count; i++) {
		if (subs[i].url[0] && strcmp(subs[i].url, url) == 0) {
			return true;
		}
	}
	return false;
}
