#include "clock.h"

#include <fcntl.h>
#include <limits.h>
#include <linux/rtc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/device/tzrules.h"
#include "src/system/net/http.h"

// A system clock reading earlier than this cannot be right: it means nothing
// ever set it. Anything after it is taken at face value.
#define CLOCK_PLAUSIBLE_FROM 1735689600L // 2025-01-01 UTC

// ---------------------------------------------------------------------------
// Keeping time, the way the stock firmware does it
//
// What the stock firmware does, from reading its scripts and binary:
//
//   * At boot nothing in userspace sets the clock. /etc/init.d/rcS and every
//     S?? script under it were read: no `hwclock -s`, no `date`, no ntpd. The
//     kernel restores CLOCK_REALTIME from the RTC on its own.
//   * Across a suspend ("echo mem > /sys/power/state", at 0x47f660) it does not
//     touch the RTC either, before or after.
//   * On the way out, at 0x48bd04, it runs `hwclock -w --utc` and then
//     poweroff. That is the whole of its persistence, and busybox's `-w` writes
//     the current second verbatim, with no rounding or fudge.
//   * Whenever Wi-Fi associates it shells out to
//     `ntpdate 0.pool.ntp.org time.windows.com &` (0x4668b0), gated on
//     wpa_supplicant reporting COMPLETED.
//
// That last one is the only correction the firmware makes, so an RTC that runs
// fast or slow is quietly papered over every time the device sees a network.
//
// This file does the same: read the RTC, write the RTC, and ask the network
// when there is one. No rate correction is applied. A rate error cannot be
// told apart from a user rounding the time to the nearest minute -- clock_set()
// zeroes the seconds, so up to 59 seconds of pure rounding over a span as short
// as ten minutes reads as an enormous drift, and correcting for it is itself
// the drift.
// ---------------------------------------------------------------------------

// The device nodes busybox's hwclock would use. The stock player shells out to
// `hwclock -w --utc`; writing the ioctl directly does the same job without
// forking, which matters on a device this tight on memory.
static const char *const RTC_DEVICES[] = {"/dev/rtc0", "/dev/rtc", "/dev/misc/rtc"};

// The firmware's own network clock: same binary and servers the stock player
// uses. It is a real 168 KB ntpdate, not the busybox applet.
#define NTPDATE_BIN "/usr/bin/ntpdate"
#define NTP_SERVERS "0.pool.ntp.org time.windows.com"

// Once every half hour is plenty: the point is to correct an RTC that has
// wandered, not to discipline the clock continuously.
#define NTP_MIN_INTERVAL_SECONDS 1800

// ---------------------------------------------------------------------------
// the timezone
// ---------------------------------------------------------------------------
//
// The firmware sets TZ nowhere, so to the C library local time is UTC and
// mktime()/localtime() shift nothing. While only this player touches the clock
// that is consistent even though it is wrong: 21:00 set by hand is held and
// written to the RTC as 21:00 UTC and shown as 21:00.
//
// ntpdate then sets true UTC, and from that moment both conventions live in the
// same device: whoever wrote the RTC last decides what survives a power-off,
// and the clock comes back hours out with the minutes right.
//
// So the device is given a real timezone: the system holds UTC as it should and
// mktime/localtime convert everywhere, with no call site changed. By hand the
// offset comes from the user, in Settings > Date and time > Timezone -- guessing
// it from the gap between a hand-set time and what the network returns needs
// the hand-set time to be right to the minute in the first place.
//
// In automatic the network says where the device is instead: a geolocation
// service names the IANA zone ("Europe/Rome"), and TZ is set to that zone's
// POSIX rule (tzrules.h). The rule carries the daylight saving dates, so the C
// library changes between standard time and daylight saving at the right
// moment by itself, with or without a network that day.

#define TZ_OFFSET_UNKNOWN INT_MIN

// No real timezone is further out than fourteen hours.
#define TZ_MAX_OFFSET_SECONDS (14 * 3600)

static int tz_offset_minutes = TZ_OFFSET_UNKNOWN;

// The zone the network located, kept as [clock] zone and [clock] zone_rule so
// it holds across a power-off. Used while the clock is automatic; by hand the
// offset and the daylight saving switch below are what count.
#define ZONE_TEXT_MAX 64
static char zone_name[ZONE_TEXT_MAX];
static char zone_rule[ZONE_TEXT_MAX];

// Automatic or manual, [clock] automatic. On unless switched off: before there
// was a choice the network was always asked, and a device updated from then
// keeps doing so.
static bool auto_on = true;

static bool zone_active(void) { return auto_on && zone_rule[0]; }

// Daylight saving, kept apart from the offset. See clock.h: the list says
// where, this says when, and the system gets the sum.
static bool tz_dst;

#define DST_MINUTES 60

// Installs the offset into the C library. The POSIX string has the sign
// reversed from the usual notation: "UTC-2" means two hours east, what everyone
// calls UTC+2. Getting it wrong moves the clock by four hours instead of zero,
// so the flip happens once, here.
static void tz_apply(int minutes) {
	int posix = -minutes;
	char sign = posix < 0 ? '-' : '+';
	int abs_min = posix < 0 ? -posix : posix;

	char tz[32];
	snprintf(tz, sizeof(tz), "UTC%c%d:%02d", sign, abs_min / 60, abs_min % 60);
	setenv("TZ", tz, 1);
	tzset();

	printf("clock: time zone UTC%+d:%02d (TZ=%s)\n", minutes / 60, (minutes < 0 ? -minutes : minutes) % 60, tz);
}

static bool manual_offset_known(void) { return tz_offset_minutes != TZ_OFFSET_UNKNOWN; }

// What actually goes to the system by hand.
static int tz_effective(void) { return tz_offset_minutes + (tz_dst ? DST_MINUTES : 0); }

// The offset from UTC the C library applies at `when`, in minutes.
static int offset_at(time_t when) {
	struct tm local;
	localtime_r(&when, &local);
	return (int)(local.tm_gmtoff / 60);
}

// The located zone's standard time: the lower of its offsets in January and in
// July, so it comes out right on both sides of the equator, and for a zone whose
// rule calls winter the exception (Europe/Dublin).
static int zone_standard_minutes(void) {
	time_t now = time(NULL);
	struct tm utc;
	gmtime_r(&now, &utc);
	struct tm probe;
	memset(&probe, 0, sizeof(probe));
	probe.tm_year = utc.tm_year;
	probe.tm_mday = 1;
	probe.tm_hour = 12;
	int january = offset_at(timegm(&probe));
	probe.tm_mon = 6;
	int july = offset_at(timegm(&probe));
	return january < july ? january : july;
}

// TZ as the mode says: the located zone's rule in automatic, the offset and the
// switch by hand. With neither known it is left alone.
static void tz_refresh(void) {
	if (zone_active()) {
		setenv("TZ", zone_rule, 1);
		tzset();
		printf("clock: time zone %s (TZ=%s)\n", zone_name, zone_rule);
	} else if (manual_offset_known()) {
		tz_apply(tz_effective());
	}
}

bool clock_utc_offset_known(void) { return zone_active() || manual_offset_known(); }

// The base offset, as picked from the list, which the list compares against to
// know which row to tick. Daylight saving stays out of it, or enabling it would
// clear the tick.
int clock_utc_offset_minutes(void) {
	if (zone_active()) {
		return zone_standard_minutes();
	}
	return manual_offset_known() ? tz_offset_minutes : 0;
}

bool clock_dst_enabled(void) {
	if (zone_active()) {
		return offset_at(time(NULL)) > zone_standard_minutes();
	}
	return tz_dst;
}

void clock_set_dst(bool on) {
	if (tz_dst == on) {
		return;
	}
	tz_dst = on;
	config_set_int("clock", "dst", on ? 1 : 0);
	config_save();

	if (!zone_active() && manual_offset_known()) {
		tz_apply(tz_effective());
	}
}

void clock_set_utc_offset_minutes(int minutes) {
	if (minutes < -TZ_MAX_OFFSET_SECONDS / 60 || minutes > TZ_MAX_OFFSET_SECONDS / 60) {
		return;
	}
	tz_offset_minutes = minutes;
	if (!zone_active()) {
		tz_apply(tz_effective());
	}
	config_set_int("clock", "utc_offset_minutes", minutes);
	config_save();
}

bool clock_zone_automatic(void) { return zone_active(); }

const char *clock_zone_name(void) { return zone_active() ? zone_name : ""; }

static bool clock_set_flag;

// The last instant the wall clock was known to be right, paired with the
// boot-time reading taken at the same moment. CLOCK_BOOTTIME keeps counting
// while the SoC is suspended, so the gap between two of its readings is the
// real elapsed time -- exactly what CLOCK_REALTIME loses across a suspend on
// this hardware.
static time_t ref_wall;
static time_t ref_boot;

// Below this a correction is not worth making: a second either way is the
// reading, not drift.
#define RESYNC_THRESHOLD_SECONDS 2

bool clock_is_set(void) { return clock_set_flag; }

static time_t boottime_now(void) {
	struct timespec ts;
#ifdef CLOCK_BOOTTIME
	if (clock_gettime(CLOCK_BOOTTIME, &ts) == 0) {
		return ts.tv_sec;
	}
#endif
	// No CLOCK_BOOTTIME (a very old kernel): monotonic is better than nothing,
	// even though it cannot see the suspended time.
	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
		return ts.tv_sec;
	}
	return 0;
}

// Records "the wall clock was right at this instant" for clock_resync() to
// reason from later.
static void mark_reference(time_t wall) {
	ref_wall = wall;
	ref_boot = boottime_now();
}

// ---------------------------------------------------------------------------
// the RTC
// ---------------------------------------------------------------------------

// Pushes a UTC time into the RTC. Failure is not fatal: the clock still runs,
// it just will not survive a power cycle.
static bool write_rtc(time_t when) {
	struct tm utc;
	gmtime_r(&when, &utc);

	struct rtc_time rtc = {
		.tm_sec = utc.tm_sec,
		.tm_min = utc.tm_min,
		.tm_hour = utc.tm_hour,
		.tm_mday = utc.tm_mday,
		.tm_mon = utc.tm_mon,
		.tm_year = utc.tm_year,
	};

	for (size_t i = 0; i < sizeof(RTC_DEVICES) / sizeof(RTC_DEVICES[0]); i++) {
		int fd = open(RTC_DEVICES[i], O_WRONLY);
		if (fd < 0) {
			continue;
		}

		bool ok = (ioctl(fd, RTC_SET_TIME, &rtc) == 0);
		close(fd);

		if (ok) {
			printf("clock: written through to %s\n", RTC_DEVICES[i]);
			config_set_int("clock", "last_seen", (long)when);
			config_save();
			return true;
		}
	}

	fprintf(stderr, "clock: no writable RTC; the time will be lost when the device powers off\n");
	return false;
}

// Reads the RTC back. Returns 0 when there is no readable one, or when what it
// holds is obviously not a real time.
static time_t read_rtc(void) {
	for (size_t i = 0; i < sizeof(RTC_DEVICES) / sizeof(RTC_DEVICES[0]); i++) {
		int fd = open(RTC_DEVICES[i], O_RDONLY);
		if (fd < 0) {
			continue;
		}

		struct rtc_time rtc;
		bool ok = (ioctl(fd, RTC_RD_TIME, &rtc) == 0);
		close(fd);
		if (!ok) {
			continue;
		}

		struct tm utc;
		memset(&utc, 0, sizeof(utc));
		utc.tm_sec = rtc.tm_sec;
		utc.tm_min = rtc.tm_min;
		utc.tm_hour = rtc.tm_hour;
		utc.tm_mday = rtc.tm_mday;
		utc.tm_mon = rtc.tm_mon;
		utc.tm_year = rtc.tm_year;

		// The RTC holds UTC -- write_rtc puts it there in UTC, and the stock
		// firmware's `hwclock -w --utc` does the same -- so it has to be read
		// back as UTC. mktime would apply the local offset twice.
		time_t when = timegm(&utc);
		if (when >= CLOCK_PLAUSIBLE_FROM) {
			return when;
		}
	}
	return 0;
}

// ---------------------------------------------------------------------------
// asking the network
// ---------------------------------------------------------------------------

static pthread_mutex_t ntp_lock = PTHREAD_MUTEX_INITIALIZER;
static bool ntp_running;
static time_t ntp_last_attempt;
static unsigned ntp_finished; // attempts that have ended, for clock_network_sync_result()
static bool ntp_last_ok;

// A zone located by the worker, waiting for clock_poll() to install it on the
// interface thread: setenv() and tzset() are not for two threads at once.
static char pending_zone[ZONE_TEXT_MAX];
static char pending_rule[ZONE_TEXT_MAX];

bool clock_auto_enabled(void) { return auto_on; }

void clock_set_auto(bool on) {
	if (on == auto_on) {
		return;
	}
	// Leaving automatic, the zone becomes the hand-set one as it stands now --
	// its standard offset and whether daylight saving is on -- so the clock does
	// not jump when the mode changes.
	if (!on && zone_active()) {
		tz_offset_minutes = zone_standard_minutes();
		tz_dst = offset_at(time(NULL)) > tz_offset_minutes;
		config_set_int("clock", "utc_offset_minutes", tz_offset_minutes);
		config_set_int("clock", "dst", tz_dst ? 1 : 0);
	}
	auto_on = on;
	config_set_bool("clock", "automatic", on);
	config_save();
	tz_refresh();
}

static void (*change_hook)(void);

void clock_set_change_hook(void (*hook)(void)) { change_hook = hook; }

static void notify_change(void) {
	if (change_hook) {
		change_hook();
	}
}

// The IANA name of the zone the device is in, from its public address. Two
// services, the second only when the first does not answer: neither needs a
// key, and both name the zone the way the tz database does. Plain HTTP first,
// which answers in a fraction of the time a TLS handshake takes on this CPU.
static bool zone_locate(char *out, size_t size) {
	static const char *const SOURCES[] = {
		"http://ip-api.com/json/?fields=status,timezone",
		"https://get.geojs.io/v1/ip/geo.json",
	};
	for (size_t i = 0; i < sizeof(SOURCES) / sizeof(SOURCES[0]); i++) {
		char *body = NULL;
		int status = 0;
		if (!http_get_ex(SOURCES[i], NULL, false, &body, NULL, 16 * 1024, 5, &status)) {
			continue;
		}
		bool found = false;
		json_doc_t doc;
		if (json_parse(body, &doc)) {
			int root = json_root(&doc);
			found = root >= 0 && json_obj_str(&doc, root, "timezone", out, size) && out[0];
			json_free(&doc);
		}
		free(body);
		if (found) {
			return true;
		}
	}
	return false;
}

// Where, as well as when: the zone, and with it daylight saving. Run beside
// ntpdate rather than after it, and handed to the interface the moment it is
// known.
static void *zone_worker(void *unused) {
	(void)unused;
	char zone[ZONE_TEXT_MAX] = "";
	if (!zone_locate(zone, sizeof(zone))) {
		printf("clock: no answer about the time zone\n");
		return NULL;
	}
	const char *rule = tzrules_lookup(zone);
	printf("clock: the network places the device in %s%s\n", zone, rule ? "" : ", a zone with no known rule");
	if (rule) {
		pthread_mutex_lock(&ntp_lock);
		snprintf(pending_zone, sizeof(pending_zone), "%s", zone);
		snprintf(pending_rule, sizeof(pending_rule), "%s", rule);
		pthread_mutex_unlock(&ntp_lock);
		notify_change();
	}
	return NULL;
}

static void *ntp_worker(void *unused) {
	(void)unused;

	pthread_t zone_thread;
	bool zone_started = auto_on && pthread_create(&zone_thread, NULL, zone_worker, NULL) == 0;

	time_t before = time(NULL);

	// The firmware's own binary and servers. The stock player backgrounds the
	// command with `&` and forgets it, which is fine for a shell but leaves
	// this thread sitting in system() for as long as ntpdate takes. Two
	// samples a server instead of four, two seconds each at most: a server
	// that does not answer -- time.windows.com often does not -- otherwise
	// holds the clock back for four times the timeout.
	int rc = system(NTPDATE_BIN " -p 2 -t 2 " NTP_SERVERS " >/dev/null 2>&1");
	time_t after = time(NULL);
	bool ok = rc == 0 && after >= CLOCK_PLAUSIBLE_FROM;

	if (ok) {
		printf("clock: ntpdate agreed with the network (%+ld s)\n", (long)(after - before));

		// The RTC is what carries this across the next power-off, and it is
		// the thing most likely to have been wrong in the first place.
		write_rtc(after);
		mark_reference(after);

		clock_set_flag = true;
		config_set_bool("clock", "configured", true);
		config_save();
		notify_change(); // the status bar shows the time now, not at its next tick
	} else {
		printf("clock: ntpdate did not answer (%d)\n", rc);
	}

	if (zone_started) {
		pthread_join(zone_thread, NULL);
	}

	pthread_mutex_lock(&ntp_lock);
	ntp_running = false;
	ntp_last_ok = ok;
	ntp_finished++;
	pthread_mutex_unlock(&ntp_lock);
	notify_change();
	return NULL;
}

// Starts an attempt unless one is running, or, without `force`, one was made
// in the last half hour. True when one is running now.
static bool network_sync_start(bool force) {
	if (access(NTPDATE_BIN, X_OK) != 0) {
		return false; // not this firmware
	}

	time_t now = time(NULL);

	pthread_mutex_lock(&ntp_lock);
	bool running = ntp_running;
	bool skip = running || (!force && ntp_last_attempt != 0 && now - ntp_last_attempt < NTP_MIN_INTERVAL_SECONDS);
	if (!skip) {
		ntp_running = true;
		ntp_last_attempt = now;
	}
	pthread_mutex_unlock(&ntp_lock);

	if (skip) {
		return running;
	}

	pthread_t t;
	if (pthread_create(&t, NULL, ntp_worker, NULL) != 0) {
		pthread_mutex_lock(&ntp_lock);
		ntp_running = false;
		pthread_mutex_unlock(&ntp_lock);
		return false;
	}
	pthread_detach(t);
	return true;
}

void clock_network_sync(void) {
	if (auto_on) {
		network_sync_start(false);
	}
}

bool clock_network_sync_now(void) { return network_sync_start(true); }

bool clock_poll(void) {
	char zone[ZONE_TEXT_MAX], rule[ZONE_TEXT_MAX];
	pthread_mutex_lock(&ntp_lock);
	snprintf(zone, sizeof(zone), "%s", pending_zone);
	snprintf(rule, sizeof(rule), "%s", pending_rule);
	pending_zone[0] = pending_rule[0] = '\0';
	pthread_mutex_unlock(&ntp_lock);

	if (!rule[0] || (strcmp(zone, zone_name) == 0 && strcmp(rule, zone_rule) == 0)) {
		return false;
	}
	snprintf(zone_name, sizeof(zone_name), "%s", zone);
	snprintf(zone_rule, sizeof(zone_rule), "%s", rule);
	config_set("clock", "zone", zone_name);
	config_set("clock", "zone_rule", zone_rule);
	config_save();
	if (!auto_on) {
		return false; // kept for when the clock is automatic again
	}
	tz_refresh();
	return true;
}

unsigned clock_network_sync_result(bool *running, bool *ok) {
	pthread_mutex_lock(&ntp_lock);
	unsigned finished = ntp_finished;
	if (running) {
		*running = ntp_running;
	}
	if (ok) {
		*ok = ntp_last_ok;
	}
	pthread_mutex_unlock(&ntp_lock);
	return finished;
}

bool clock_resync(void) {
	if (!clock_set_flag) {
		return false; // nothing trustworthy to resync against yet
	}

	time_t now = time(NULL);
	time_t truth = 0;

	// Real elapsed time since the clock was last known to be right, suspended
	// time included. This is the answer, and it is deliberately NOT the RTC:
	// an RTC that runs fast would otherwise drag the wall clock forward every
	// few minutes, which is the failure this file exists to have stopped.
	if (ref_wall != 0) {
		time_t boot = boottime_now();
		if (boot > ref_boot) {
			truth = ref_wall + (boot - ref_boot);
		}
	}

	// Only when there is no reference to work from is the RTC worth asking.
	if (truth == 0) {
		truth = read_rtc();
	}

	if (truth == 0) {
		return false;
	}

	long behind = (long)(truth - now);
	if (behind > -RESYNC_THRESHOLD_SECONDS && behind < RESYNC_THRESHOLD_SECONDS) {
		mark_reference(now); // already right; just refresh the reference
		return false;
	}

	struct timeval tv = {.tv_sec = truth, .tv_usec = 0};
	if (settimeofday(&tv, NULL) != 0) {
		perror("clock: settimeofday (resync)");
		return false;
	}

	printf("clock: resynced by %+ld s (time the system clock did not count)\n", behind);
	mark_reference(truth);
	return true;
}

void clock_init(void) {
	// Before any time is read: from here on mktime() and localtime() know the
	// timezone, and the rest of this file, and of the player, works in UTC
	// without knowing it.
	long saved_offset = config_get_int("clock", "utc_offset_minutes", TZ_OFFSET_UNKNOWN);
	if (saved_offset != TZ_OFFSET_UNKNOWN && saved_offset >= -TZ_MAX_OFFSET_SECONDS / 60 &&
		saved_offset <= TZ_MAX_OFFSET_SECONDS / 60) {
		tz_offset_minutes = (int)saved_offset;
		tz_dst = config_get_bool("clock", "dst", false);
	}
	auto_on = config_get_bool("clock", "automatic", true);

	// The located zone's rule as this build knows it, which may be newer than
	// the one written down; the written one when the name is not in the table.
	snprintf(zone_name, sizeof(zone_name), "%s", config_get("clock", "zone", ""));
	const char *rule = tzrules_lookup(zone_name);
	snprintf(zone_rule, sizeof(zone_rule), "%s", rule ? rule : (zone_name[0] ? config_get("clock", "zone_rule", "") : ""));

	if (clock_utc_offset_known()) {
		tz_refresh();
	} else {
		printf("clock: time zone not chosen yet (Settings > Date and time > Time zone)\n");
	}

	clock_set_flag = config_get_bool("clock", "configured", false);

	// Older builds of this player learned a "drift" figure and applied it to
	// every RTC reading. It was wrong, it made the clock gain minutes across a
	// power-off, and a device carrying one has to be healed rather than left
	// with it forever.
	if (config_get_int("clock", "rtc_drift_ppm", 0) != 0) {
		printf("clock: dropping the stored drift correction; it was making the clock run fast\n");
		config_set_int("clock", "rtc_drift_ppm", 0);
		config_set_int("clock", "rtc_written", 0);
		config_save();
	}

	// Three sources, all of which can be wrong in their own way:
	//   - the system clock, which the kernel may or may not have restored;
	//   - the RTC;
	//   - the last time the player wrote down before it stopped.
	// The latest of them wins: time never runs backwards on this device, so the
	// newest plausible reading is closest to the truth. All three are logged so
	// a complaint about the clock can be diagnosed.
	time_t system_now = time(NULL);
	time_t rtc_now = read_rtc();
	time_t remembered = (time_t)config_get_int("clock", "last_seen", 0);

	printf("clock: at boot system=%ld rtc=%ld remembered=%ld\n", (long)system_now, (long)rtc_now,
		   (long)remembered);

	time_t best = system_now;
	const char *source = "system clock";
	if (rtc_now > best) {
		best = rtc_now;
		source = "RTC";
	}
	if (remembered > best) {
		best = remembered;
		source = "the last time seen";
	}

	if (best < CLOCK_PLAUSIBLE_FROM) {
		clock_set_flag = false; // nothing to go on: ask the user
		return;
	}

	if (best > system_now + RESYNC_THRESHOLD_SECONDS) {
		struct timeval tv = {.tv_sec = best, .tv_usec = 0};
		if (settimeofday(&tv, NULL) == 0) {
			printf("clock: started from %s (%+ld s on the system clock)\n", source,
				   (long)(best - system_now));
		} else {
			fprintf(stderr, "clock: could not set the system clock from %s\n", source);
			best = system_now;
		}
	}

	mark_reference(best);
}

bool clock_set(int year, int month, int day, int hour, int minute) {
	struct tm local;
	memset(&local, 0, sizeof(local));

	local.tm_year = year - 1900;
	local.tm_mon = month - 1;
	local.tm_mday = day;
	local.tm_hour = hour;
	local.tm_min = minute;
	local.tm_sec = 0;
	local.tm_isdst = -1; // let the C library work it out

	time_t when = mktime(&local);
	if (when == (time_t)-1) {
		fprintf(stderr, "clock: %04d-%02d-%02d %02d:%02d is not a valid time\n", year, month, day, hour, minute);
		return false;
	}

	struct timeval tv = {.tv_sec = when, .tv_usec = 0};
	if (settimeofday(&tv, NULL) != 0) {
		perror("clock: settimeofday");
		return false;
	}

	write_rtc(when);
	mark_reference(when); // this is the new "known good" instant

	clock_set_flag = true;
	config_set_bool("clock", "configured", true);
	config_set_int("clock", "last_seen", (long)when);
	config_save();

	printf("clock: set to %04d-%02d-%02d %02d:%02d\n", year, month, day, hour, minute);
	return true;
}

// The stock player writes the RTC on its way out (0x48bd04, `hwclock -w
// --utc`, immediately before poweroff); so does this, from the power menu and
// from the idle auto-off. Without it every minute the clock gained since it
// was last set is thrown away at power-off, and the next boot starts from a
// stale RTC.
void clock_shutdown(void) {
	if (!clock_set_flag) {
		return;
	}
	time_t now = time(NULL);
	if (now < CLOCK_PLAUSIBLE_FROM) {
		return;
	}
	printf("clock: writing %ld through to the RTC before shutdown\n", (long)now);
	if (!write_rtc(now)) {
		// No RTC to write: the config is the only thing that will survive, so
		// make sure it carries the very last time seen rather than whatever
		// the two-minute timer happened to leave there.
		config_set_int("clock", "last_seen", (long)now);
		config_save();
	}
}

// Below this step a rewrite buys nothing. Five minutes is the most that can be
// lost if the battery is pulled, and it is worth a tenth of the flash writes.
#define REMEMBER_MIN_STEP_SECONDS 300

static time_t last_written;

void clock_remember(void) {
	if (!clock_set_flag) {
		return;
	}

	time_t now = time(NULL);
	if (now < CLOCK_PLAUSIBLE_FROM) {
		return;
	}

	// config_save() rewrites and renames the whole device_config.ini on UBIFS.
	// Doing that every two minutes was 240 rewrites across a night of sleep,
	// with the matching flash wear, and only the last one ever mattered. So
	// write only when the value has really moved, plus always on screen-off
	// (power.c calls it there) and on exit.
	if (last_written != 0 && now - last_written < REMEMBER_MIN_STEP_SECONDS) {
		return;
	}
	last_written = now;

	config_set_int("clock", "last_seen", (long)now);
	config_save();
}

// ---------------------------------------------------------------------------
// 12 or 24 hour
//
// Read once and cached here: the status bar asks once a second, and going back
// to the config every time for a value that only changes when the user changes
// it is pointless. -1 means not read yet.
// ---------------------------------------------------------------------------

static int use_24h = -1;

bool clock_use_24h(void) {
	if (use_24h < 0) {
		use_24h = config_get_bool("ui", "clock_24h", true) ? 1 : 0;
	}
	return use_24h != 0;
}

void clock_set_use_24h(bool on) {
	use_24h = on ? 1 : 0;
	config_set_bool("ui", "clock_24h", on);
	config_save();
}

void clock_format_hm_parts(char *time_out, size_t time_size, char *suffix_out, size_t suffix_size, int hour, int minute) {
	if (suffix_out && suffix_size > 0) {
		suffix_out[0] = '\0';
	}
	if (!time_out || time_size == 0) {
		return;
	}

	if (clock_use_24h()) {
		snprintf(time_out, time_size, "%02d:%02d", hour, minute);
		return;
	}

	// Midnight and noon are the two that are always got wrong: 0 and 12 both
	// display as "12", one AM and one PM.
	int shown = hour % 12;
	if (shown == 0) {
		shown = 12;
	}
	// No leading zero: "2:05 PM", not "02:05 PM", as no 12-hour clock pads the
	// hour.
	snprintf(time_out, time_size, "%d:%02d", shown, minute);
	if (suffix_out && suffix_size > 0) {
		snprintf(suffix_out, suffix_size, "%s", hour < 12 ? "AM" : "PM");
	}
}

void clock_format_hm(char *out, size_t out_size, int hour, int minute) {
	if (!out || out_size == 0) {
		return;
	}
	char time_text[16], suffix[8];
	clock_format_hm_parts(time_text, sizeof(time_text), suffix, sizeof(suffix), hour, minute);
	snprintf(out, out_size, suffix[0] ? "%s %s" : "%s", time_text, suffix);
}
