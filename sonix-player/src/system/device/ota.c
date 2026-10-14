#include "ota.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "src/system/core/json.h"
#include "src/system/core/sha256.h"
#include "src/system/core/utils.h"
#include "src/system/device/firmware.h"
#include "src/system/device/sysinfo.h"
#include "src/system/device/system.h"
#include "src/system/net/http.h"
#include "src/system/net/wifi.h"

#define DEFAULT_REPO "Jepl4r/sonix-player"

// The API's own media type and version, as GitHub asks every client to send.
#define API_HEADERS                                                                                                    \
	"Accept: application/vnd.github+json\r\n"                                                                          \
	"X-GitHub-Api-Version: 2022-11-28\r\n"

// Twenty releases with their descriptions stay well under this.
#define LIST_LIMIT (1536 * 1024)
#define LIST_TIMEOUT_SECS 20
#define DOWNLOAD_TIMEOUT_SECS 30

// Room left on the card beyond the file itself.
#define SPACE_MARGIN (8L * 1024 * 1024)

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static ota_state_t state = OTA_IDLE;
static ota_result_t result = OTA_OK;
static long progress_done, progress_total;
static bool cancel_requested;
static http_stream_t *active_stream; // the download's stream, for ota_cancel() to wake

static ota_release_t release = {.notes = ""};
static bool release_valid;
// What release.notes points at when the release has a description: as long
// as the description, so a long changelog is not cut at a fixed size.
static char *notes_text;

// Set on the interface thread when a check starts.
static char repo[128];
static char stem[64];

static unsigned char chunk[64 * 1024];

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

static bool begin(ota_state_t next) {
	pthread_mutex_lock(&lock);
	bool ok = state == OTA_IDLE || state == OTA_FINISHED;
	if (ok) {
		state = next;
		cancel_requested = false;
		progress_done = 0;
		progress_total = 0;
	}
	pthread_mutex_unlock(&lock);
	return ok;
}

static void finish(ota_result_t r) {
	pthread_mutex_lock(&lock);
	result = cancel_requested && r != OTA_OK ? OTA_CANCELLED : r;
	state = OTA_FINISHED;
	active_stream = NULL;
	pthread_mutex_unlock(&lock);
}

static bool cancelled(void) {
	pthread_mutex_lock(&lock);
	bool c = cancel_requested;
	pthread_mutex_unlock(&lock);
	return c;
}

void ota_cancel(void) {
	pthread_mutex_lock(&lock);
	if (state == OTA_CHECKING || state == OTA_DOWNLOADING) {
		cancel_requested = true;
		if (active_stream) {
			http_stream_wake(active_stream);
		}
	}
	pthread_mutex_unlock(&lock);
}

ota_state_t ota_state(void) {
	pthread_mutex_lock(&lock);
	ota_state_t s = state;
	pthread_mutex_unlock(&lock);
	return s;
}

ota_result_t ota_result(void) {
	pthread_mutex_lock(&lock);
	ota_result_t r = result;
	pthread_mutex_unlock(&lock);
	return r;
}

void ota_acknowledge(void) {
	pthread_mutex_lock(&lock);
	if (state == OTA_FINISHED) {
		state = OTA_IDLE;
	}
	pthread_mutex_unlock(&lock);
}

bool ota_busy(void) {
	ota_state_t s = ota_state();
	return s == OTA_CHECKING || s == OTA_DOWNLOADING;
}

void ota_progress(long *done, long *total) {
	pthread_mutex_lock(&lock);
	if (done) {
		*done = progress_done;
	}
	if (total) {
		*total = progress_total;
	}
	pthread_mutex_unlock(&lock);
}

const ota_release_t *ota_release(void) { return &release; }

const char *ota_installed_version(void) { return sysinfo_os_version(); }

// ---------------------------------------------------------------------------
// versions
// ---------------------------------------------------------------------------

#define VERSION_PARTS 4

// "1.0.2", "v1.0.2", "1.0.2-r1": up to four numbers, a leading v ignored,
// anything after the numbers ignored. False when there is no number at all.
static bool parse_version(const char *text, long out[VERSION_PARTS]) {
	memset(out, 0, sizeof(long) * VERSION_PARTS);
	const char *p = text;
	if (*p == 'v' || *p == 'V') {
		p++;
	}
	if (!isdigit((unsigned char)*p)) {
		return false;
	}
	for (int i = 0; i < VERSION_PARTS && isdigit((unsigned char)*p); i++) {
		out[i] = strtol(p, (char **)&p, 10);
		if (*p != '.') {
			break;
		}
		p++;
	}
	return true;
}

static int version_cmp(const long a[VERSION_PARTS], const long b[VERSION_PARTS]) {
	for (int i = 0; i < VERSION_PARTS; i++) {
		if (a[i] != b[i]) {
			return a[i] < b[i] ? -1 : 1;
		}
	}
	return 0;
}

// ---------------------------------------------------------------------------
// the release list
// ---------------------------------------------------------------------------

// Drops a UTF-8 sequence cut in half at the end of `s`.
static void trim_partial_utf8(char *s) {
	size_t len = strlen(s);
	size_t i = len;
	while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) {
		i--;
	}
	if (i == 0) {
		return;
	}
	unsigned char lead = (unsigned char)s[i - 1];
	size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
	if (len - (i - 1) < need) {
		s[i - 1] = '\0';
	}
}

// The asset named "<stem>.upt" in release `rel`, -1 when there is none.
static int find_asset(const json_doc_t *doc, int rel) {
	char wanted[80];
	snprintf(wanted, sizeof(wanted), "%s.upt", stem);

	int assets = json_get(doc, rel, "assets");
	int count = json_len(doc, assets);
	for (int i = 0; i < count; i++) {
		int a = json_at(doc, assets, i);
		char name[96], asset_state[24];
		json_obj_str(doc, a, "name", name, sizeof(name));
		json_obj_str(doc, a, "state", asset_state, sizeof(asset_state));
		if (strcasecmp(name, wanted) == 0 && strcmp(asset_state, "uploaded") == 0) {
			return a;
		}
	}
	return -1;
}

static ota_result_t pick_release(const json_doc_t *doc) {
	int root = json_root(doc);
	if (root < 0 || doc->toks[root].type != JSON_ARRAY) {
		return OTA_CHECK_FAILED;
	}

	long installed[VERSION_PARTS];
	if (!parse_version(ota_installed_version(), installed)) {
		memset(installed, 0, sizeof(installed));
	}

	long best[VERSION_PARTS];
	memcpy(best, installed, sizeof(best));
	int best_rel = -1, best_asset = -1;

	int count = json_len(doc, root);
	for (int i = 0; i < count; i++) {
		int rel = json_at(doc, root, i);
		if (json_obj_bool(doc, rel, "draft", true) || json_obj_bool(doc, rel, "prerelease", true)) {
			continue;
		}
		char tag[32];
		long version[VERSION_PARTS];
		if (!json_obj_str(doc, rel, "tag_name", tag, sizeof(tag)) || !parse_version(tag, version)) {
			continue;
		}
		if (version_cmp(version, best) <= 0) {
			continue;
		}
		int asset = find_asset(doc, rel);
		if (asset < 0) {
			printf("ota: %s has no %s.upt, skipped\n", tag, stem);
			continue;
		}
		memcpy(best, version, sizeof(best));
		best_rel = rel;
		best_asset = asset;
	}

	if (best_rel < 0) {
		return OTA_UP_TO_DATE;
	}

	memset(&release, 0, sizeof(release));
	release.notes = "";
	free(notes_text);
	notes_text = NULL;
	json_obj_str(doc, best_rel, "tag_name", release.tag, sizeof(release.tag));
	if (!json_obj_str(doc, best_rel, "name", release.name, sizeof(release.name)) || !release.name[0]) {
		snprintf(release.name, sizeof(release.name), "%s", release.tag);
	}
	// Unescaped, the text is never longer than it is in the JSON.
	int body = json_get(doc, best_rel, "body");
	if (body >= 0 && doc->toks[body].type == JSON_STRING) {
		size_t size = (size_t)(doc->toks[body].end - doc->toks[body].start) + 1;
		if (size > OTA_NOTES_MAX) {
			size = OTA_NOTES_MAX;
		}
		notes_text = malloc(size);
		if (notes_text && json_str(doc, body, notes_text, size)) {
			trim_partial_utf8(notes_text);
			release.notes = notes_text;
		}
	}
	release.size = json_obj_long(doc, best_asset, "size", 0);
	json_obj_str(doc, best_asset, "browser_download_url", release.url, sizeof(release.url));

	// "sha256:<64 hex>" on assets uploaded since mid-2025.
	char digest[80];
	if (json_obj_str(doc, best_asset, "digest", digest, sizeof(digest)) && strncmp(digest, "sha256:", 7) == 0 &&
		strlen(digest + 7) == 64) {
		snprintf(release.sha256, sizeof(release.sha256), "%s", digest + 7);
	}

	if (release.size <= 0 || strncmp(release.url, "https://", 8) != 0) {
		return OTA_CHECK_FAILED;
	}
	release_valid = true;
	printf("ota: %s (%s) available, %ld bytes, sha256 %s\n", release.tag, release.name, release.size,
		   release.sha256[0] ? release.sha256 : "not published");
	return OTA_OK;
}

static ota_result_t do_check(void) {
	wifi_status_t wifi;
	wifi_get_status(&wifi);
	if (wifi.state != WIFI_STATE_CONNECTED) {
		return OTA_NO_NETWORK;
	}
	if (!stem[0]) {
		return OTA_NO_MODEL;
	}

	char url[256];
	snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases?per_page=20", repo);
	printf("ota: checking %s, installed '%s', file %s.upt\n", url, ota_installed_version(), stem);

	char *body = NULL;
	size_t body_len = 0;
	int status = 0;
	if (!http_get_ex(url, API_HEADERS, false, &body, &body_len, LIST_LIMIT, LIST_TIMEOUT_SECS, &status)) {
		const char *why = http_last_error();
		printf("ota: release list not fetched (status %d%s%s)\n", status, why ? ", " : "", why ? why : "");
		return OTA_CHECK_FAILED;
	}
	if (cancelled()) {
		free(body);
		return OTA_CANCELLED;
	}

	json_doc_t doc;
	ota_result_t r = OTA_CHECK_FAILED;
	if (json_parse(body, &doc)) {
		r = pick_release(&doc);
		json_free(&doc);
	} else {
		printf("ota: release list is not valid JSON (%zu bytes)\n", body_len);
	}
	free(body);
	return r;
}

static void *check_thread(void *unused) {
	(void)unused;
	thread_be_background("ota check");
	finish(do_check());
	return NULL;
}

bool ota_check_start(void) {
	if (!begin(OTA_CHECKING)) {
		return false;
	}
	release_valid = false;

	const char *r = sysinfo_ota_repo();
	snprintf(repo, sizeof(repo), "%s", r[0] ? r : DEFAULT_REPO);
	const char *s = firmware_update_stem();
	snprintf(stem, sizeof(stem), "%s", s ? s : "");

	pthread_t thread;
	if (pthread_create(&thread, NULL, check_thread, NULL) != 0) {
		finish(OTA_CHECK_FAILED);
		return true;
	}
	pthread_detach(thread);
	return true;
}

// ---------------------------------------------------------------------------
// the download
// ---------------------------------------------------------------------------

// Removes every "<stem>.upt" on the card, whatever its case: on exFAT two
// spellings can sit side by side, and the recovery kernel would take either.
static void remove_old_files(const char *root) {
	char wanted[80];
	snprintf(wanted, sizeof(wanted), "%s.upt", stem);

	DIR *dir = opendir(root);
	if (!dir) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (strcasecmp(de->d_name, wanted) == 0) {
			char path[600];
			snprintf(path, sizeof(path), "%s/%s", root, de->d_name);
			unlink(path);
		}
	}
	closedir(dir);
}

static ota_result_t do_download(void) {
	const char *root = storage_sd_root();
	if (!root || !*root) {
		return OTA_NO_CARD;
	}

	struct statvfs vfs;
	if (statvfs(root, &vfs) == 0) {
		unsigned long long unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
		unsigned long long avail = (unsigned long long)vfs.f_bavail * unit;
		if (avail < (unsigned long long)release.size + SPACE_MARGIN) {
			return OTA_NO_SPACE;
		}
	}

	char final_path[600], part_path[620];
	snprintf(final_path, sizeof(final_path), "%s/%s.upt", root, stem);
	snprintf(part_path, sizeof(part_path), "%s.part", final_path);

	FILE *f = fopen(part_path, "wb");
	if (!f) {
		printf("ota: cannot create %s\n", part_path);
		return OTA_DOWNLOAD_FAILED;
	}

	http_stream_t stream;
	memset(&stream, 0, sizeof(stream));
	stream.fd = -1;
	printf("ota: downloading %s\n", release.url);
	if (!http_stream_open(&stream, release.url, DOWNLOAD_TIMEOUT_SECS)) {
		const char *why = http_last_error();
		printf("ota: download not started%s%s\n", why ? ": " : "", why ? why : "");
		fclose(f);
		unlink(part_path);
		return cancelled() ? OTA_CANCELLED : OTA_DOWNLOAD_FAILED;
	}

	pthread_mutex_lock(&lock);
	active_stream = &stream;
	bool stop = cancel_requested;
	pthread_mutex_unlock(&lock);

	ota_result_t r = OTA_OK;
	if (stream.content_length > 0 && stream.content_length != release.size) {
		printf("ota: the server sends %ld bytes, the release says %ld\n", stream.content_length, release.size);
		r = OTA_CORRUPT;
	}

	sha256_ctx sha;
	sha256_init(&sha);
	long done = 0;
	while (r == OTA_OK && !stop && done < release.size) {
		long want = release.size - done;
		int n = http_stream_read(&stream, chunk, want < (long)sizeof(chunk) ? (int)want : (int)sizeof(chunk));
		if (n <= 0) {
			printf("ota: the connection ended at %ld of %ld bytes\n", done, release.size);
			r = OTA_DOWNLOAD_FAILED;
			break;
		}
		if (fwrite(chunk, 1, (size_t)n, f) != (size_t)n) {
			printf("ota: write to the card failed at %ld bytes\n", done);
			r = OTA_DOWNLOAD_FAILED;
			break;
		}
		sha256_update(&sha, chunk, (size_t)n);
		done += n;

		pthread_mutex_lock(&lock);
		progress_done = done;
		stop = cancel_requested;
		pthread_mutex_unlock(&lock);
	}

	pthread_mutex_lock(&lock);
	active_stream = NULL;
	pthread_mutex_unlock(&lock);
	http_stream_close(&stream);

	if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
		r = r == OTA_OK ? OTA_DOWNLOAD_FAILED : r;
	}
	if (fclose(f) != 0 && r == OTA_OK) {
		r = OTA_DOWNLOAD_FAILED;
	}

	if (stop) {
		r = OTA_CANCELLED;
	}
	if (r == OTA_OK && release.sha256[0]) {
		char hex[SHA256_HEX_LEN];
		sha256_final_hex(&sha, hex);
		if (strcasecmp(hex, release.sha256) != 0) {
			printf("ota: sha256 %s, the release says %s\n", hex, release.sha256);
			r = OTA_CORRUPT;
		}
	}
	if (r != OTA_OK) {
		unlink(part_path);
		return r;
	}

	remove_old_files(root);
	if (rename(part_path, final_path) != 0) {
		printf("ota: cannot rename %s\n", part_path);
		unlink(part_path);
		return OTA_DOWNLOAD_FAILED;
	}
	sync();
	printf("ota: %s ready (%ld bytes%s)\n", final_path, done, release.sha256[0] ? ", sha256 matches" : "");
	return OTA_OK;
}

static void *download_thread(void *unused) {
	(void)unused;
	thread_be_background("ota download");
	finish(do_download());
	return NULL;
}

bool ota_download_start(void) {
	if (!release_valid || !begin(OTA_DOWNLOADING)) {
		return false;
	}

	pthread_mutex_lock(&lock);
	progress_total = release.size;
	pthread_mutex_unlock(&lock);

	pthread_t thread;
	if (pthread_create(&thread, NULL, download_thread, NULL) != 0) {
		finish(OTA_DOWNLOAD_FAILED);
		return true;
	}
	pthread_detach(thread);
	return true;
}
