#include "webcgi.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// The root the stock CGI programs are allowed to look at, compiled into them
// and repeated here: this answers the same URL, so it applies the same rule.
// Nothing outside /data/mnt is any of the web page's business.
#define CGI_ROOT "/data/mnt/"

#define MAX_PATH 1024

// ---------------------------------------------------------------------------
// the query string
// ---------------------------------------------------------------------------

static int hexval(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

// %20 -> space, + -> space, in place.
static void url_decode(char *s) {
	char *out = s;

	for (char *in = s; *in; in++) {
		if (*in == '+') {
			*out++ = ' ';
		} else if (*in == '%' && hexval(in[1]) >= 0 && hexval(in[2]) >= 0) {
			*out++ = (char)((hexval(in[1]) << 4) | hexval(in[2]));
			in += 2;
		} else {
			*out++ = *in;
		}
	}
	*out = '\0';
}

// Writes the decoded value of `key` from a query string into `out`. False if
// the key is not there.
static bool query_value(const char *query, const char *key, char *out, size_t size) {
	size_t key_len = strlen(key);

	for (const char *at = query; at && *at;) {
		const char *amp = strchr(at, '&');
		size_t pair_len = amp ? (size_t)(amp - at) : strlen(at);

		if (pair_len > key_len && at[key_len] == '=' && strncmp(at, key, key_len) == 0) {
			size_t value_len = pair_len - key_len - 1;
			if (value_len >= size) {
				value_len = size - 1;
			}
			memcpy(out, at + key_len + 1, value_len);
			out[value_len] = '\0';
			url_decode(out);
			return true;
		}

		at = amp ? amp + 1 : NULL;
	}

	return false;
}

// ---------------------------------------------------------------------------
// output
// ---------------------------------------------------------------------------

// JSON string escaping. FAT and exFAT forbid quotes and backslashes in names,
// so a card should never produce one, but this reads a real directory and any
// name it finds must still come out as valid JSON.
static void print_json_string(const char *s) {
	putchar('"');
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		switch (*p) {
		case '"':
			fputs("\\\"", stdout);
			break;
		case '\\':
			fputs("\\\\", stdout);
			break;
		case '\n':
			fputs("\\n", stdout);
			break;
		case '\r':
			fputs("\\r", stdout);
			break;
		case '\t':
			fputs("\\t", stdout);
			break;
		default:
			if (*p < 0x20) {
				printf("\\u%04x", *p);
			} else {
				putchar((char)*p); // UTF-8 passes through untouched
			}
			break;
		}
	}
	putchar('"');
}

static void fail(const char *why) {
	printf("Status: 404 Not Found\r\n");
	printf("Content-Type: application/json; charset=utf-8\r\n\r\n");
	printf("{\"error\":");
	print_json_string(why);
	printf("}\n");
}

// ---------------------------------------------------------------------------

bool webcgi_should_run(int argc, char **argv) {
	// Every web server sets this before running a CGI, and nothing else does.
	if (getenv("GATEWAY_INTERFACE") == NULL) {
		return false;
	}

	// And argv[0] has to be the published CGI name, not the player started from
	// a shell that happens to have that variable exported.
	const char *name = (argc > 0 && argv[0]) ? argv[0] : "";
	const char *slash = strrchr(name, '/');
	if (slash) {
		name = slash + 1;
	}

	return strcmp(name, "sonix-list") == 0;
}

int webcgi_main(void) {
	const char *query = getenv("QUERY_STRING");
	if (!query) {
		query = "";
	}

	char path[MAX_PATH];
	if (!query_value(query, "path", path, sizeof(path)) || !path[0]) {
		fail("no path");
		return 1;
	}

	char hidden_flag[8] = "";
	bool hidden = query_value(query, "hidden", hidden_flag, sizeof(hidden_flag)) && hidden_flag[0] == '1';

	// Same rule as the programs this stands in for: below /data/mnt, and no
	// climbing out of it. This runs as root (thttpd's user= says so), so without
	// the check the transfer page could ask for a listing of /etc.
	if (strncmp(path, CGI_ROOT, strlen(CGI_ROOT)) != 0 || strstr(path, "/../") != NULL) {
		fail("path not allowed");
		return 1;
	}

	// One trailing slash, however many arrived.
	size_t len = strlen(path);
	while (len > 1 && path[len - 1] == '/') {
		path[--len] = '\0';
	}

	DIR *dir = opendir(path);
	if (!dir) {
		fail("cannot open");
		return 1;
	}

	printf("Content-Type: application/json; charset=utf-8\r\n");
	printf("Cache-Control: no-store\r\n\r\n");
	putchar('[');

	struct dirent *entry;
	bool first = true;

	while ((entry = readdir(dir)) != NULL) {
		const char *name = entry->d_name;

		// "." and ".." are never entries anyone wants, hidden or not.
		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
			continue;
		}
		if (!hidden && name[0] == '.') {
			continue;
		}

		// One byte is kept for the slash a folder gets below; a path that does
		// not fit is left out rather than listed under a cut name.
		char full[MAX_PATH + 256];
		int full_len = snprintf(full, sizeof(full), "%s/%s", path, name);
		if (full_len < 0 || (size_t)full_len + 1 >= sizeof(full)) {
			continue;
		}

		struct stat info;
		if (lstat(full, &info) != 0) {
			continue; // vanished between readdir and here; say nothing about it
		}

		bool is_dir = S_ISDIR(info.st_mode);

		if (!first) {
			putchar(',');
		}
		first = false;

		printf("{\"path\":");
		if (is_dir) {
			full[full_len] = '/';
			full[full_len + 1] = '\0';
		}
		print_json_string(full);
		printf(",\"name\":");
		print_json_string(name);
		printf(",\"ctime\":\"%ld\"", (long)info.st_mtime);

		if (!is_dir) {
			printf(",\"size\":%lld", (long long)info.st_size);
		}
		putchar('}');
	}

	putchar(']');
	putchar('\n');

	closedir(dir);
	return 0;
}
