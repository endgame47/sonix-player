#include "system.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/gui/nowplaying/cover.h"
#include "src/gui/settings/screensaver.h"
#include "src/gui/shell/gui.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/bluetooth/btplayer.h"
#include "src/system/audio/audio.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/playback/device_state.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/audio/eq.h"
#include "src/system/remote/dlna.h"
#include "src/system/gearboy/gbdb.h"
#include "src/system/input/hookclicks.h"
#include "src/system/input/keymap.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/podcastsubs.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/library/library.h"
#include "src/system/core/logging.h"
#include "src/system/library/playlists.h"
#include "src/system/device/usb.h"
#include "src/system/device/power.h"
#include "src/system/device/screenshot.h"
#include "src/system/device/sysserver.h"
#include "src/system/core/utils.h"
#include "src/system/core/utils.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static char battery_cache[16] = "!!";
static bool battery_charging_cache = false;
static const battery_config_t *g_battery_cfg = NULL;

typedef struct {
	storage_config_t *storage_cfg;
	const char *device_name;
	system_notification_cb_t notification_cb;
	void *notification_user_data;
} system_runtime_t;

static void send_notification(const system_runtime_t *runtime, const char *message) {
	if (runtime->notification_cb) {
		runtime->notification_cb(message, runtime->notification_user_data);
	}
}

static const char *storage_device_name(const char *device_path) {
	const char *name = strrchr(device_path, '/');
	return name ? name + 1 : device_path;
}

// ---------------------------------------------------------------------------
// battery
//
// Read the way the stock player does it: walk /sys/class/power_supply and sort
// the entries by their `type`. Guessing at names does not survive a firmware
// revision -- on this device the gauge is a CW2015 registered as "battery" and
// the charger is "mp2731-charger", but the charger's own `status` is the one
// that tells the truth about charging, and the gauge's `status` often just
// says Unknown.
//
//   type == "Battery"          -> capacity, and status if it is meaningful
//   type == "Mains" or "USB"   -> online, i.e. is a charger plugged in
// ---------------------------------------------------------------------------

#define POWER_SUPPLY_DIR "/sys/class/power_supply"

// Reads a one-line sysfs node into `out`, stripped of its newline.
static bool read_sysfs_line(const char *dir, const char *name, const char *node, char *out, size_t out_size) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s/%s", dir, name, node);

	char *content = read_file_content(path);
	if (!content) {
		return false;
	}

	content[strcspn(content, "\r\n")] = '\0';
	snprintf(out, out_size, "%s", content);
	free(content);
	return true;
}

// Which supply the level came from, for the one-off diagnostic below.
static char battery_source[288];

// Reads a battery's charge level. `capacity` is the usual node, but not every
// gauge exposes it as a file even when it reports it -- the stock player also
// parses `uevent`, which lists every property the driver publishes, so that is
// the fallback here. Returns -1 when the supply has no level at all.
static int read_capacity(const char *name) {
	char text[16];
	if (read_sysfs_line(POWER_SUPPLY_DIR, name, "capacity", text, sizeof(text)) && text[0] >= '0' && text[0] <= '9') {
		int value = atoi(text);
		return value < 0 ? 0 : (value > 100 ? 100 : value);
	}

	char path[512];
	snprintf(path, sizeof(path), "%s/%s/uevent", POWER_SUPPLY_DIR, name);

	char *content = read_file_content(path);
	if (!content) {
		return -1;
	}

	int value = -1;
	const char *found = strstr(content, "POWER_SUPPLY_CAPACITY=");
	if (found) {
		value = atoi(found + strlen("POWER_SUPPLY_CAPACITY="));
		if (value < 0)
			value = 0;
		if (value > 100)
			value = 100;
	}

	free(content);
	return value;
}

// Prints every power supply the kernel knows about, once. Which node is the
// gauge and what it publishes differs between firmware revisions, and without
// this a wrong reading is impossible to tell apart from a missing one.
static void log_power_supplies(void) {
	DIR *dir = opendir(POWER_SUPPLY_DIR);
	if (!dir) {
		fprintf(stderr, "battery: %s does not exist\n", POWER_SUPPLY_DIR);
		return;
	}

	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.') {
			continue;
		}

		char type[32] = "?", capacity[16] = "-", status[32] = "-", online[16] = "-";
		read_sysfs_line(POWER_SUPPLY_DIR, de->d_name, "type", type, sizeof(type));
		read_sysfs_line(POWER_SUPPLY_DIR, de->d_name, "capacity", capacity, sizeof(capacity));
		read_sysfs_line(POWER_SUPPLY_DIR, de->d_name, "status", status, sizeof(status));
		read_sysfs_line(POWER_SUPPLY_DIR, de->d_name, "online", online, sizeof(online));

		printf("battery: %-20s type=%-10s capacity=%-4s status=%-12s online=%s\n", de->d_name, type, capacity, status,
			   online);
	}

	closedir(dir);
}

// Scans the power supplies. `percent` is left at -1 when no battery reported a
// capacity; `charging` is true when the gauge says so or any charger is online.
static void scan_power_supplies(int *percent, bool *charging) {
	*percent = -1;
	*charging = false;

	DIR *dir = opendir(POWER_SUPPLY_DIR);
	if (!dir) {
		return;
	}

	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.') {
			continue;
		}

		char type[32];
		if (!read_sysfs_line(POWER_SUPPLY_DIR, de->d_name, "type", type, sizeof(type))) {
			continue;
		}

		if (strcmp(type, "Battery") == 0) {
			int value = read_capacity(de->d_name);

			// Some kernels expose a second, useless battery node that answers
			// every read with zero. Take the first one that reports a real
			// level, and only fall back to a zero reading if nothing better
			// turns up -- a player that always says 0% is worse than one that
			// admits it does not know.
			if (value > 0 && *percent <= 0) {
				*percent = value;
				snprintf(battery_source, sizeof(battery_source), "%s", de->d_name);
			} else if (value == 0 && *percent < 0) {
				*percent = 0;
				snprintf(battery_source, sizeof(battery_source), "%s", de->d_name);
			}

			char status[32];
			if (read_sysfs_line(POWER_SUPPLY_DIR, de->d_name, "status", status, sizeof(status))) {
				if (strcmp(status, "Charging") == 0 || strcmp(status, "Full") == 0) {
					*charging = true;
				}
			}
			continue;
		}

		// Mains, USB, USB_PD, ... anything that can supply power.
		if (strcmp(type, "Battery") != 0) {
			char online[16];
			if (read_sysfs_line(POWER_SUPPLY_DIR, de->d_name, "online", online, sizeof(online)) && online[0] == '1') {
				*charging = true;
			}
		}
	}

	closedir(dir);
}

void sync_battery_from_sysfs(void) {
	// The host build has no power supplies at all; the two override files let
	// the status bar be worked on without a device (see main.c).
	if (g_battery_cfg && g_battery_cfg->battery_capacity_file) {
		battery_charging_cache = false;

		if (g_battery_cfg->battery_status_file) {
			char *content = read_file_content(g_battery_cfg->battery_status_file);
			if (content) {
				content[strcspn(content, "\r\n")] = '\0';
				// "Full" is NOT charging: counting it as such leaves the
				// battery icon and the red LED in their charging state long
				// after the charge has finished.
				battery_charging_cache = (strcmp(content, "Charging") == 0);
				free(content);
			}
		}

		char *content = read_file_content(g_battery_cfg->battery_capacity_file);
		if (!content) {
			strcpy(battery_cache, "!!");
			return;
		}

		content[strcspn(content, "\r\n")] = '\0';
		snprintf(battery_cache, sizeof(battery_cache), "%s", content);
		free(content);
		return;
	}

	// One dump on the first read: enough to tell a missing gauge from one that
	// is there and reporting zero.
	static bool logged;
	if (!logged) {
		logged = true;
		log_power_supplies();
	}

	int percent = -1;
	bool charging = false;
	scan_power_supplies(&percent, &charging);

	static bool source_logged;
	if (!source_logged && percent >= 0) {
		source_logged = true;
		printf("battery: reading level from '%s'\n", battery_source);
	}

	battery_charging_cache = charging;
	if (percent < 0) {
		strcpy(battery_cache, "!!");
	} else {
		snprintf(battery_cache, sizeof(battery_cache), "%d", percent);
	}
}

char *read_battery_percent() { return battery_cache; }

// Set while something has deliberately stopped the charger -- DAC mode with
// charging switched off. The hardware may still report a healthy charger on
// the other end of the cable; what the user is told has to follow the choice
// they made, not the wire.
static bool charging_suppressed;

void system_suppress_charging(bool suppressed) {
	charging_suppressed = suppressed;
}

bool read_battery_charging(void) { return battery_charging_cache && !charging_suppressed; }

// HiBy OS reaches the same directory under three different names -- /mnt/sd_0,
// /data/mnt/sd_0 and /usr/data/mnt/sd_0 -- because /mnt and /data are symlinks
// into the writable UBIFS mounted at /usr/data. Which of the three is the real
// path is a detail of how the firmware was built, and the stock binaries use
// all three interchangeably, so the player resolves it at runtime instead of
// betting on one.
static const char *const SD_ROOT_CANDIDATES[] = {
	"/mnt/sd_0",
	"/data/mnt/sd_0",
	"/usr/data/mnt/sd_0",
};

// The nodes sys_server and the stock player look for, most specific first: a
// card with a partition table shows up as mmcblk*p1, one formatted whole-disk
// as mmcblk* itself.
static const char *const SD_DEVICE_CANDIDATES[] = {
	"/dev/mmcblk0p1",
	"/dev/mmcblk1p1",
	"/dev/mmcblk0",
	"/dev/mmcblk1",
};

// Resolved once at startup and handed to the file browser, see storage_sd_root().
static char sd_root[PATH_MAX] = "";
static char sd_device_used[64] = "";

const char *storage_sd_device(void) { return sd_device_used[0] ? sd_device_used : NULL; }

const char *storage_sd_root(void) {
	return sd_root[0] ? sd_root : NULL;
}

// True if something is already mounted on `mount_point`.
//
// /proc/mounts lists the kernel's canonical path, so a plain string compare
// misses the mount whenever the caller went through a symlink -- which on this
// firmware is the common case. Both sides get resolved first.
static bool is_mounted(const char *mount_point) {
	char wanted[PATH_MAX];
	if (!realpath(mount_point, wanted)) {
		return false; // doesn't even exist, so nothing can be mounted on it
	}

	FILE *f = fopen("/proc/mounts", "r");
	if (!f) {
		return false;
	}

	char device[256], point[256];
	bool found = false;
	while (fscanf(f, "%255s %255s %*[^\n]", device, point) == 2) {
		if (strcmp(point, wanted) == 0) {
			found = true;
			break;
		}
	}

	fclose(f);
	return found;
}

// True when the filesystem mounted on `mount_point` is still ALIVE.
//
// Pulling a card unmounts nothing by itself: the entry stays in /proc/mounts
// pointing at a block device that no longer exists, and every read off it
// fails with EIO. is_mounted() cannot tell that corpse from a working mount, so
// a reinserted card would never be mounted over it. Opening the directory is
// the cheapest honest test there is.
static bool mount_is_alive(const char *mount_point) {
	DIR *dir = opendir(mount_point);
	if (!dir) {
		return false;
	}
	errno = 0;
	readdir(dir); // "." on a live mount; NULL with EIO on a dead one
	bool alive = (errno == 0);
	closedir(dir);
	return alive;
}

// Takes a mount down and means it. A plain umount(2) refuses with EBUSY while
// anything still holds a descriptor on the card, and a dead mount that survives
// a refusal blocks the next card. MNT_DETACH always succeeds: the mount point is
// freed at once and the filesystem is cleaned up when the last user lets go,
// which is precisely what is wanted here.
static bool force_unmount(const char *mount_point) {
	if (umount(mount_point) == 0) {
		return true;
	}
	if (umount2(mount_point, MNT_FORCE) == 0) {
		return true;
	}
	if (umount2(mount_point, MNT_DETACH) == 0) {
		fprintf(stderr, "storage: %s was busy; detached it lazily\n", mount_point);
		return true;
	}
	fprintf(stderr, "storage: could not unmount %s: %s\n", mount_point, strerror(errno));
	return false;
}

// Reads which block device is mounted on `mount_point` out of /proc/mounts.
// Needed when somebody else (a previous run, sys_server) did the mounting:
// the USB mass-storage export must know the device even then. Returns false
// when nothing is mounted there or the source isn't a /dev node.
static bool device_mounted_on(const char *mount_point, char *out, size_t out_size) {
	char wanted[PATH_MAX];
	if (!realpath(mount_point, wanted)) {
		return false;
	}

	FILE *f = fopen("/proc/mounts", "r");
	if (!f) {
		return false;
	}

	char device[256], point[256];
	bool found = false;
	while (fscanf(f, "%255s %255s %*[^\n]", device, point) == 2) {
		if (strcmp(point, wanted) == 0 && strncmp(device, "/dev/", 5) == 0) {
			size_t len = strlen(device);
			if (len >= out_size) {
				len = out_size - 1;
			}
			memcpy(out, device, len);
			out[len] = '\0';
			found = true;
			break;
		}
	}

	fclose(f);
	return found;
}

// Picks the block device to mount: the configured one if it is there, else the
// first of the nodes the stock firmware knows about that exists.
static const char *pick_sd_device(const storage_config_t *storage_cfg) {
	if (storage_cfg->device && access(storage_cfg->device, F_OK) == 0) {
		return storage_cfg->device;
	}

	for (size_t i = 0; i < sizeof(SD_DEVICE_CANDIDATES) / sizeof(SD_DEVICE_CANDIDATES[0]); i++) {
		if (access(SD_DEVICE_CANDIDATES[i], F_OK) == 0) {
			return SD_DEVICE_CANDIDATES[i];
		}
	}

	return storage_cfg->device; // may be NULL; caller reports it
}

// Picks the directory to mount on: one that is already a mount point wins,
// otherwise the first name that exists, otherwise the configured one (created
// if need be).
static const char *pick_sd_root(const storage_config_t *storage_cfg) {
	size_t count = sizeof(SD_ROOT_CANDIDATES) / sizeof(SD_ROOT_CANDIDATES[0]);

	if (storage_cfg->mount_point && is_mounted(storage_cfg->mount_point)) {
		return storage_cfg->mount_point;
	}

	for (size_t i = 0; i < count; i++) {
		if (is_mounted(SD_ROOT_CANDIDATES[i])) {
			return SD_ROOT_CANDIDATES[i];
		}
	}

	if (storage_cfg->mount_point && access(storage_cfg->mount_point, F_OK) == 0) {
		return storage_cfg->mount_point;
	}

	for (size_t i = 0; i < count; i++) {
		if (access(SD_ROOT_CANDIDATES[i], F_OK) == 0) {
			return SD_ROOT_CANDIDATES[i];
		}
	}

	return storage_cfg->mount_point ? storage_cfg->mount_point : SD_ROOT_CANDIDATES[0];
}

// How many entries the mount point holds. Zero on a directory that only looks
// like a card because nobody mounted anything on it yet.
static int count_entries(const char *path) {
	DIR *d = opendir(path);
	if (!d) {
		return -1;
	}

	int n = 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0) {
			n++;
		}
	}

	closedir(d);
	return n;
}

// Logs which mmcblk nodes the kernel knows about. Printed when the configured
// one isn't there, since which node is the card and which is internal storage
// differs between devices.
static void list_block_devices(void) {
	DIR *dev = opendir("/dev");
	if (!dev) {
		return;
	}

	fprintf(stderr, "storage: block devices present:");
	struct dirent *de;
	while ((de = readdir(dev)) != NULL) {
		if (strncmp(de->d_name, "mmcblk", 6) == 0 || strncmp(de->d_name, "sd", 2) == 0) {
			fprintf(stderr, " %s", de->d_name);
		}
	}
	fprintf(stderr, "\n");
	closedir(dev);
}

// Whether a mounted filesystem can actually be written to. A card can be
// mounted and useless: the write-protect notch on an adapter, or a filesystem
// the kernel will only read. The browser and the storage page do not care --
// they only read -- and everything else does, which is why "only the browser
// works" is the shape that gets reported.
static bool mount_is_writable(const char *mount_point) {
	struct statvfs st;
	if (statvfs(mount_point, &st) == 0 && (st.f_flag & ST_RDONLY)) {
		return false;
	}

	// statvfs is not the whole answer -- a FUSE filesystem can report a
	// writable superblock and still refuse -- so a file is actually created.
	char probe[600];
	snprintf(probe, sizeof(probe), "%s/.sonix-write-test", mount_point);
	int fd = open(probe, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0) {
		return false;
	}
	close(fd);
	unlink(probe);
	return true;
}

// NTFS, read-write, through the userspace driver.
//
// The kernel's own `ntfs` module is read-only on this kernel (CONFIG_NTFS_RW is
// off), and it mounts happily: the card appears, the browser lists it, and then
// every single thing that writes fails silently -- no log file, no thumbnail
// cache, no EQ presets, and no database, which also means no scan of anything,
// because library_scan_start() gives up the moment library.db could not be
// created. That is the whole "only the file browser works" report.
//
// mount(2) cannot reach a FUSE helper, so the helper is run as a program. Both
// spellings are tried: `ntfs-3g` is the usual binary, `mount.ntfs-3g` is what
// busybox mount would exec.
static int mount_ntfs_rw(const char *device, const char *mount_point) {
	static const char *const helpers[] = {"/usr/bin/ntfs-3g", "/bin/ntfs-3g", "/sbin/mount.ntfs-3g",
										  "/usr/sbin/mount.ntfs-3g"};

	for (size_t i = 0; i < sizeof(helpers) / sizeof(helpers[0]); i++) {
		if (access(helpers[i], X_OK) != 0) {
			continue;
		}

		pid_t pid = fork();
		if (pid < 0) {
			return -1;
		}
		if (pid == 0) {
			// big_writes and the default uid/gid: the player is the only user of
			// this card, and the helper otherwise leaves everything owned by
			// whoever mounted it.
			execl(helpers[i], helpers[i], device, mount_point, "-o", "big_writes,noatime", (char *)NULL);
			_exit(127);
		}

		int status = 0;
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
		}
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && is_mounted(mount_point)) {
			printf("storage: mounted %s on %s as ntfs-3g (%s)\n", device, mount_point, helpers[i]);
			return 0;
		}
		fprintf(stderr, "storage: %s would not mount %s (status %d)\n", helpers[i], device, status);
	}

	return -1;
}

// Last resort: mount the card with the mount(2) syscall directly. This can only
// use filesystems built into the kernel -- if exFAT is handled by a userspace
// helper on this firmware, only the sys_server path above will work.
static int mount_sd_directly(const char *device, const char *mount_point) {
	// Cards come formatted every which way and the kernel does not guess, so
	// the plausible types are tried in order of likelihood.
	//
	// ntfs-3g comes before the list, and the kernel `ntfs` module after it: the
	// module would win the race and mount the card read-only, which is worse
	// than not mounting it as NTFS at all. `ntfs3` is the newer kernel driver
	// that does write; it is not in a 4.4 kernel, but asking costs one failed
	// syscall and this file outlives the kernel it was written for.
	static const char *const fs_types[] = {"exfat", "vfat", "ext4", "ext3", "ext2", "ntfs3", "ntfs"};

	if (mount_ntfs_rw(device, mount_point) == 0) {
		return 0;
	}

	for (size_t i = 0; i < sizeof(fs_types) / sizeof(fs_types[0]); i++) {
		if (mount(device, mount_point, fs_types[i], MS_NOATIME, NULL) == 0) {
			printf("storage: mounted %s on %s as %s\n", device, mount_point, fs_types[i]);
			return 0;
		}

		// ENODEV means the kernel has no such filesystem built in; anything
		// else means this type isn't what's on the card.
		if (errno != ENODEV && errno != EINVAL) {
			fprintf(stderr, "storage: mounting %s as %s failed: %s\n", device, fs_types[i], strerror(errno));
		}
	}

	return -1;
}

// Whether the card can be written to, decided once when it is mounted. Read by
// storage_sd_writable(); everything that keeps state on the card can ask instead
// of failing silently.
static bool sd_writable = true;

// Records the writability of a mount that has just been established, and says
// so in the log. Returns 0 so it can stand in for the `return 0` it replaces.
static int mount_settled(const char *mount_point) {
	sd_writable = mount_is_writable(mount_point);
	if (!sd_writable) {
		fprintf(stderr,
				"storage: %s is READ-ONLY. Browsing works; the log, the databases, the thumbnail cache and the "
				"EQ presets all need to write and will fall back or fail. An NTFS card needs ntfs-3g on the "
				"device; a card in an adapter may have its write-protect notch closed.\n",
				mount_point);
	}
	return 0;
}

bool storage_sd_writable(void) { return sd_writable; }

static int mount_sd(storage_config_t *storage_cfg, const system_runtime_t *runtime) {
	(void)runtime;

	const char *mount_point = pick_sd_root(storage_cfg);
	snprintf(sd_root, sizeof(sd_root), "%s", mount_point);

	if (!storage_cfg->manage_mount || getenv("SONIX_NO_MOUNT")) {
		return mount_settled(mount_point); // somebody else owns the mount, see storage_config_t
	}

	// A mount left over from the card that was just pulled has to go before
	// anything else is attempted: it holds the mount point, and the check
	// below would otherwise read it as "already there, leave it alone".
	if (is_mounted(mount_point) && !mount_is_alive(mount_point)) {
		fprintf(stderr, "storage: %s is mounted but dead; clearing it first\n", mount_point);
		force_unmount(mount_point);
	}

	// If the stock UI is still up -- or the player was started alongside it --
	// the card is already there and must not be touched.
	if (is_mounted(mount_point)) {
		printf("storage: %s is already mounted, leaving it alone (%d entries)\n", mount_point, count_entries(mount_point));

		// The block device must still be recorded: the USB mass-storage export
		// needs it, and this early return is the common startup path (the card
		// is nearly always mounted already). Without it storage_sd_device()
		// stays NULL and USB mode never arms.
		if (!device_mounted_on(mount_point, sd_device_used, sizeof(sd_device_used))) {
			const char *device = pick_sd_device(storage_cfg);
			if (device) {
				snprintf(sd_device_used, sizeof(sd_device_used), "%s", device);
			}
		}
		if (sd_device_used[0]) {
			printf("storage: the card device is %s\n", sd_device_used);
		}
		return mount_settled(mount_point);
	}

	const char *device = pick_sd_device(storage_cfg);
	if (device) {
		snprintf(sd_device_used, sizeof(sd_device_used), "%s", device);
	}
	if (!device) {
		fprintf(stderr, "storage: no microSD block device found\n");
		list_block_devices();
		return -1;
	}

	mkdir(mount_point, 0755); // in case the mount point doesn't exist

	printf("storage: mounting %s on %s\n", device, mount_point);

	// mount(2) first: no other process involved, no protocol to get wrong, and
	// it either works or returns an errno. Anything that talks to another daemon
	// is a bigger risk than it looks on a device where this process dying reboots
	// the machine.
	if (mount_sd_directly(device, mount_point) == 0) {
		return mount_settled(mount_point);
	}

	// Only then HiBy OS's own mount daemon, which shells out to busybox
	// `mount` and so can reach the exFAT/NTFS userspace helpers that the
	// syscall cannot. SONIX_NO_SYSSERVER=1 skips it entirely.
	if (sysserver_mount(device, mount_point) == 0 && is_mounted(mount_point)) {
		printf("storage: mounted via sys_server (%d entries)\n", count_entries(mount_point));
		return mount_settled(mount_point);
	}

	fprintf(stderr, "storage: could not mount %s on %s\n", device, mount_point);
	list_block_devices();
	return -1;
}

static void unmount_sd(storage_config_t *storage_cfg) {
	if (!storage_cfg->manage_mount || !sd_root[0]) {
		return;
	}

	if (sysserver_umount(sd_root) == 0 && !is_mounted(sd_root)) {
		return;
	}
	// The daemon's answer is not the last word -- it shells out to busybox
	// umount, which fails just as quietly on EBUSY. Escalate until the mount
	// point is genuinely free, or the next card cannot take its place.
	force_unmount(sd_root);
}

static void check_and_mount_existing_sd(storage_config_t *storage_cfg, const system_runtime_t *runtime) {
	mount_sd(storage_cfg, runtime);

	if (!storage_cfg->manage_mount) {
		return; // host build: the browser reads an ordinary folder
	}

	int entries = count_entries(sd_root);
	printf("storage: browsing %s (%d entries, mounted=%s)\n", sd_root, entries, is_mounted(sd_root) ? "yes" : "no");

	// An unmounted mount point is a directory on the internal UBIFS that
	// happens to sit at the right path. It reads fine and lists whatever
	// stray files ended up in it, which is why "the browser shows one file"
	// looks like a browser bug when it is really a missing mount.
	if (!is_mounted(sd_root)) {
		fprintf(stderr, "storage: WARNING -- %s is NOT a mount point; anything listed there is internal storage, not the card\n",
				sd_root);
		list_block_devices();
	}
}


// ---------------------------------------------------------------------------
// The card's databases across a hot removal. Pulling the card leaves the
// sqlite handles pointing at a dead filesystem: every library page then says
// "nothing here" until a reboot, and the stale handles keep the old mount
// pinned so the REINSERTED card cannot take its place. Detach closes
// everything that lives on the card before the unmount; attach reopens it
// once the new mount is up.
// ---------------------------------------------------------------------------

// Both halves are idempotent. They have to be: the uevent filter matches the
// bare "mmcblk" prefix, so one card produces TWO events (the disk and its
// partition) and each half would otherwise run twice, the second call recording
// the state the first had already cleared.
static bool card_attached = true; // the card is mounted before this thread starts

bool storage_card_attached(void) { return card_attached; }

static void card_databases_detach(void) {
	if (!card_attached) {
		return;
	}
	card_attached = false;

	// Playback first. The playback thread keeps the track's descriptor open for
	// the whole track -- and with "remember track" a track is loaded and paused
	// from boot -- so without this the card is busy essentially always, the
	// unmount fails, and the dead mount blocks the next card. The USB export
	// does the same (usb.c: storage_export).
	//
	// Where the track was, first of all: the stop below is what the next press
	// of play would otherwise read as "the track ended, start it again".
	device_state_note_storage_gone();
	audio_stop();

	logging_suspend_for_usb(); // closes the on-card log file
	library_close();
	audiobookdb_close();
	printf("storage: card databases closed for removal\n");
}

static void card_databases_attach(const char *root) {
	if (card_attached) {
		return;
	}
	card_attached = true;

	// PATH_MAX, because the card root comes from a mount point and a mount point
	// can be as long as any path.
	char path[PATH_MAX];
	// The root is bounded by what fits alongside the file name, so a root too
	// long to hold both is rejected rather than silently cut in half and opened
	// somewhere else.
	const int room = (int)sizeof(path) - 32;
	snprintf(path, sizeof(path), "%.*s/.local/library.db", room, root);
	library_open(path);
	snprintf(path, sizeof(path), "%.*s/.local/audiobooks.db", room, root);
	audiobookdb_open(path);

	// Everything else that keeps a path on the card. These only cache a
	// directory and open files on demand, but the directory is derived once, so
	// a card change has to re-derive it or the playlists, the presets and the
	// artwork cache end up pointing at nothing.
	playlists_init(root);
	screensaver_set_card_root(root);
	cover_set_cache_dir(root);
	eq_presets_set_dir(root);
	peq_presets_set_dir(root);
	mseb_presets_set_dir(root);
	qobuzcache_set_root(root);
	tidalcache_set_root(root);
	podcastcache_set_root(root);
	podcastsubs_set_root(root);
	dlna_set_root(root);
	gbdb_set_root(root);

	// The log, from the setting rather than from a remembered flag: a flag does
	// not survive two remove events, and a failed reopen consumes it for good.
	// logging_attach_sd() re-derives the path and honours the setting.
	logging_attach_sd(root);

	printf("storage: card databases reopened on %s\n", root);
}

// ---------------------------------------------------------------------------
// The card supervisor
//
// Too many things can lose the card: the hotplug path, the USB export (which
// unmounts it on purpose and remounts on unplug), and ADB, which rearranges
// the whole USB gadget and can pull the composite export apart underneath.
// There is always one more order of events, so rather than hardening each path,
// a supervisor asks one question every few seconds: is the card device there,
// and is the mount on it alive? If the answer is no and nobody is deliberately
// holding the card away, it puts it back.
// ---------------------------------------------------------------------------

static pthread_mutex_t card_lock = PTHREAD_MUTEX_INITIALIZER;
static storage_config_t *g_storage_cfg;
static system_runtime_t *g_runtime;

void storage_recheck_card(void) {
	if (!g_storage_cfg || !g_storage_cfg->manage_mount || getenv("SONIX_NO_MOUNT")) {
		return;
	}
	// A USB export has the card handed to the host on purpose. Taking it back
	// from under a PC that is writing to it is how filesystems die.
	if (usb_storage_active()) {
		return;
	}

	pthread_mutex_lock(&card_lock);

	if (sd_root[0] && is_mounted(sd_root) && mount_is_alive(sd_root)) {
		pthread_mutex_unlock(&card_lock); // nothing to do, the common case
		return;
	}

	// Is there even a card to mount? No point thrashing when the slot is empty.
	if (!pick_sd_device(g_storage_cfg)) {
		card_databases_detach();
		pthread_mutex_unlock(&card_lock);
		return;
	}

	fprintf(stderr, "storage: the card is not readable; putting it back\n");
	card_databases_detach();

	if (sd_root[0] && is_mounted(sd_root)) {
		force_unmount(sd_root);
	}

	if (mount_sd(g_storage_cfg, g_runtime) == 0 && mount_is_alive(sd_root)) {
		card_databases_attach(sd_root);
		fprintf(stderr, "storage: the card is back on %s\n", sd_root);
	}

	pthread_mutex_unlock(&card_lock);
}

static void *card_supervisor_thread(void *arg) {
	(void)arg;
	thread_be_background("card supervisor");

	for (;;) {
		sleep(4);
		storage_recheck_card();
	}
	return NULL;
}

void *sd_hotplug_thread(void *arg) {
	system_runtime_t *runtime = arg;
	storage_config_t *storage_cfg = runtime->storage_cfg;

	int sock = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);

	struct sockaddr_nl addr = {
		.nl_family = AF_NETLINK,
		.nl_pid = getpid(),
		.nl_groups = 1,
	};

	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "Failed to bind netlink socket\n");
		close(sock);
		return NULL;
	}

	char buf[4096 + 1]; // +1 is for null terminator

	while (1) {
		int len = recv(sock, buf, sizeof(buf), 0);
		if (len <= 0) {
			continue;
		}

		buf[len] = '\0';

		if (strstr(buf, runtime->device_name)) {
			if (strstr(buf, "add@")) {
				printf("SD Card Inserted\n");
				pthread_mutex_lock(&card_lock);

				// The uevent arrives before mdev has finished making the
				// partition node, and a card produces two of these (the disk,
				// then the partition). Whichever one this is, keep trying for
				// a few seconds: this is the only notice there is, and a single
				// failed attempt would leave the card invisible until the
				// player is restarted.
				bool mounted = false;
				for (int attempt = 0; attempt < 12 && !mounted; attempt++) {
					if (attempt) {
						usleep(300 * 1000);
					}
					mounted = mount_sd(storage_cfg, runtime) == 0 && mount_is_alive(sd_root);
				}

				if (mounted) {
					send_notification(runtime, "sd_card_inserted");
					card_databases_attach(sd_root);
				} else {
					fprintf(stderr, "storage: the card would not mount after the insert\n");
				}
				pthread_mutex_unlock(&card_lock);
			}

			if (strstr(buf, "remove@")) {
				printf("SD Card Removed\n");
				pthread_mutex_lock(&card_lock);
				bool was_attached = card_attached;
				card_databases_detach();
				unmount_sd(storage_cfg);
				pthread_mutex_unlock(&card_lock);
				if (was_attached) {
					send_notification(runtime, "sd_card_removed");
				}
			}
		}
	}
}

// Prints what the kernel has registered as input devices, so an unknown button
// can be traced to the node it arrives on.
static void log_input_devices(void) {
	char *content = read_file_content("/proc/bus/input/devices");
	if (!content) {
		return;
	}
	printf("input: registered devices\n%s\n", content);
	free(content);
}

// Every key event is printed with its raw code, so a button nobody has mapped
// yet can be identified by pressing it and reading the log.
//
// Except for touch traffic with the screen off. The touch controller keeps
// scanning in doze mode when the double-tap wake is on, so a player in a pocket
// reports a stream of BTN_TOUCH events all night -- and each of these lines is
// a write to the log file on the microSD, which means waking the card
// controller for something nobody will ever read.
//
// That flood is touch, and touch is what the BTN_* codes are: 0x100 and up. A
// real key is a deliberate press, rare and worth a line whatever the screen is
// doing -- a headphone button in particular is pressed WITH the screen off, so
// silencing keys as well would hide the presses most worth seeing.
//
// Repeats are never logged, on any node. A repeat says nothing its press has
// not already said, and it is the one thing here that can arrive in a storm:
// the kernel repeats a key it never saw released, thirty times a second, for as
// long as it stays down -- and a lost release over a radio link is not a rare
// event. The log is line-buffered onto the card, so a line each would be thirty
// synchronous writes a second with stdout's lock held across every one of them.
// That is not a noisy log, it is a frozen interface: every other thread blocks
// on its next printf.
static void log_key_event(const char *node, const struct input_event *ev) {
	if (ev->value == 2) {
		return;
	}
	if (!power_screen_is_on() && ev->code >= 0x100) {
		return;
	}
	fprintf(stderr, "input: %s key code=%d (0x%x) value=%d (%s)\n", node, ev->code, ev->code, ev->value,
		   ev->value == 1 ? "press" : (ev->value == 0 ? "release" : "repeat"));
}

// ---------------------------------------------------------------------------
// physical buttons
//
// The device sends a plain press and a plain release -- no kernel autorepeat --
// so holding volume down has to be turned into a repeat here. The same loop
// gives the power key its long press: poll() with a deadline instead of a
// blocking read, and the timeout is the "still held" tick.
//
// Codes as read off this device:
//   event0  116 power, 165 previous
//   event2  115 volume up, 114 volume down, 163 next, 164 play/pause
// ---------------------------------------------------------------------------

// How long a volume key waits before it starts repeating, and how fast it goes
// once it does. One step is small enough that a quick tap is a fine adjustment
// and a hold sweeps the whole range in a few seconds.
#define VOLUME_REPEAT_DELAY_MS 400
#define VOLUME_REPEAT_PERIOD_MS 70
#define VOLUME_STEP_PERCENT 1

// Hold the power key this long and the shutdown menu comes up instead of the
// screen simply toggling.
#define POWER_LONG_PRESS_MS 1200

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000) + (uint32_t)(ts.tv_nsec / 1000000);
}

// The keys always move the volume, over Bluetooth as over the jack: the step
// reaches the stream itself, so synchronised, bluealsa passes it on to the
// headphones, and separate, bluealsa attenuates on this side.
static void apply_volume_step(int code) {
	device_state_change_volume_percent(code == KEY_VOLUMEUP ? VOLUME_STEP_PERCENT : -VOLUME_STEP_PERCENT);
	gui_notify_volume(device_state_get_volume_percent());
}

// ---------------------------------------------------------------------------
// Screenshot: volume up + power
//
// The state lives out here under a lock because the two keys can arrive on
// different input nodes, and each node has its own thread: the one seeing the
// volume key would never see the other's power key.
//
// When the combination fires both keys must be consumed: the volume must not go
// up and power must not blank the screen on release. It rearms only once both
// are back up -- otherwise holding them down would fire a burst of screenshots.
// ---------------------------------------------------------------------------

static pthread_mutex_t combo_lock = PTHREAD_MUTEX_INITIALIZER;
static bool combo_vol_down;
static bool combo_power_down;
static bool combo_fired;

static bool combo_active(void) {
	pthread_mutex_lock(&combo_lock);
	bool active = combo_fired;
	pthread_mutex_unlock(&combo_lock);
	return active;
}

// True when this key is part of a fired combination and so must not do what it
// would do on its own.
static bool combo_press(int code) {
	if (code != KEY_VOLUMEUP && code != KEY_POWER) {
		return false;
	}

	// Read outside the lock: it reads the configuration, and the option does not
	// change a thousand times a second.
	bool enabled = screenshot_enabled();

	bool fire = false;
	pthread_mutex_lock(&combo_lock);
	if (code == KEY_VOLUMEUP) {
		combo_vol_down = true;
	} else {
		combo_power_down = true;
	}
	if (combo_vol_down && combo_power_down && !combo_fired && enabled) {
		combo_fired = true;
		fire = true;
	}
	bool active = combo_fired;
	pthread_mutex_unlock(&combo_lock);

	if (fire) {
		printf("input: volume up + power -> screenshot\n");
		screenshot_request();
	}
	return active;
}

// True when the release must be ignored.
static bool combo_release(int code) {
	if (code != KEY_VOLUMEUP && code != KEY_POWER) {
		return false;
	}

	pthread_mutex_lock(&combo_lock);
	if (code == KEY_VOLUMEUP) {
		combo_vol_down = false;
	} else {
		combo_power_down = false;
	}
	bool consumed = combo_fired;
	// Rearm only with both back up: releasing one and pressing it again must not
	// fire the combination by itself.
	if (!combo_vol_down && !combo_power_down) {
		combo_fired = false;
	}
	pthread_mutex_unlock(&combo_lock);
	return consumed;
}

// From kernel code to the button as it sits in the hand, or KEYMAP_BTN_COUNT
// when the code is not one of the five remappable ones.
//
// Remapping applies ONLY to the device's own buttons. The inline cable remote
// sends the same codes but is a different thing -- one click, two clicks, three
// clicks -- and remapping it along with the side buttons would change two
// things while meaning to change one (see keymap.h).
//
// The two media keys arrive SWAPPED: the kernel reports NEXTSONG for the UPPER
// one and PREVIOUSSONG for the lower. The stock firmware compensates the same
// way, and there are years of muscle memory built on it.
//
// Rotating the screen does not flip this on top. The button remap page can say
// exactly that instead, and a hidden second swap on top of a user's own mapping
// is a setting fighting a setting.
static keymap_button_t keymap_button_for_code(int code) {
	switch (code) {
	case KEY_NEXTSONG:
#if defined(BOARD_R1)
		return KEYMAP_BTN_NEXT;
#else
		return KEYMAP_BTN_PREV;
#endif
	case KEY_PREVIOUSSONG:
		return KEYMAP_BTN_NEXT;
	case KEY_PLAYPAUSE:
		return KEYMAP_BTN_PLAY;
	case KEY_VOLUMEUP:
		return KEYMAP_BTN_VOL_UP;
	case KEY_VOLUMEDOWN:
		return KEYMAP_BTN_VOL_DOWN;
	default:
		return KEYMAP_BTN_COUNT;
	}
}

// Whether the button, as currently mapped, does something worth repeating while
// held. Volume yes, everything else no: a button remapped to "next track" and
// held down would skip fourteen tracks a second.
static bool mapped_action_repeats(keymap_button_t button) {
	keymap_action_t action = keymap_get(button);
	return action == KEYMAP_ACTION_VOLUME_UP || action == KEYMAP_ACTION_VOLUME_DOWN;
}

// ---------------------------------------------------------------------------
// Holding next or previous: seeking inside the track
//
// A short press changes track and a long one runs through the one playing, the
// way every player with two skip buttons has always worked. Which means the
// track change moves to the RELEASE: deciding it on the way down would change
// track and then seek inside the new one, both from a single press.
//
// The step is deliberately coarse and constant. A ramp reads well on a screen
// where the position is visible; with the player in a pocket, "so many seconds
// per second" is the thing a thumb can aim with.
// ---------------------------------------------------------------------------
#define SEEK_HOLD_MS 500	 // held this long: it is a seek, not a skip
#define SEEK_REPEAT_MS 250	 // and another step every quarter second
#define SEEK_STEP_SECONDS 5.0

static bool mapped_action_seeks(keymap_button_t button) {
	keymap_action_t action = keymap_get(button);
	return action == KEYMAP_ACTION_NEXT || action == KEYMAP_ACTION_PREV;
}

// One step, in whichever direction the button means. Nothing is asked of the
// decoder here: the step moves the scrub position, the interface follows it,
// and the seek itself happens when the button is let go (device_state.h).
// Refuses on anything with no position to move along -- a live radio stream has
// neither a length nor a point to come back to.
static void seek_step(keymap_button_t button) {
	keymap_action_t action = keymap_get(button);
	if (action != KEYMAP_ACTION_NEXT && action != KEYMAP_ACTION_PREV) {
		return;
	}

	device_state_t state;
	device_state_get(&state);
	if (state.live || state.progress_total_secs <= 0) {
		return;
	}

	device_state_scrub_by((action == KEYMAP_ACTION_NEXT) ? SEEK_STEP_SECONDS : -SEEK_STEP_SECONDS);
}

// WHAT REPEATS IS THE ACTION, NOT THE KEY.
//
// This device sends no kernel autorepeat: one press and one release, nothing
// else. The repeat is produced by the loop below, using a deadline instead of a
// blocking read, and the deadline is armed from what the held key DOES rather
// than from which key it is. Arming it for the two physical volume keys alone
// would leave "volume up" mapped onto a media key unable to repeat.
//
// The inline cable remote stays outside the mapping, as it does on press: its
// keys are what they are, and its volume always repeats.
static bool key_repeats_held(int code, bool headset) {
	if (code < 0) {
		return false;
	}
	if (headset) {
		return code == KEY_VOLUMEUP || code == KEY_VOLUMEDOWN;
	}
	keymap_button_t button = keymap_button_for_code(code);
	return mapped_action_repeats(button) || mapped_action_seeks(button);
}

// A held key that seeks rather than steps the volume: it waits longer before
// the first step and then goes at its own pace.
static bool key_seeks_held(int code, bool headset) {
	if (code < 0 || headset) {
		return false;
	}
	return mapped_action_seeks(keymap_button_for_code(code));
}

static void run_mapped_action(keymap_button_t button);

static void repeat_held(int code, bool headset) {
	if (headset) {
		apply_volume_step(code);
		return;
	}
	keymap_button_t button = keymap_button_for_code(code);
	if (mapped_action_seeks(button)) {
		seek_step(button);
		return;
	}
	run_mapped_action(button);
}

static void run_mapped_action(keymap_button_t button) {
	switch (keymap_get(button)) {
	case KEYMAP_ACTION_PLAY_PAUSE:
		gui_notify_key(GUI_KEY_PLAY_PAUSE);
		break;
	case KEYMAP_ACTION_PREV:
		gui_notify_key(GUI_KEY_PREV);
		break;
	case KEYMAP_ACTION_NEXT:
		gui_notify_key(GUI_KEY_NEXT);
		break;
	case KEYMAP_ACTION_VOLUME_UP:
		apply_volume_step(KEY_VOLUMEUP);
		break;
	case KEYMAP_ACTION_VOLUME_DOWN:
		apply_volume_step(KEY_VOLUMEDOWN);
		break;
	case KEYMAP_ACTION_NONE:
	default:
		break;
	}
}

static void handle_key_press(const char *node, int code, bool headset) {
	// The side buttons, translated from kernel code to the button as held. The
	// kernel reports NEXTSONG for the UPPER one and PREVIOUSSONG for the lower;
	// the stock firmware swaps them the same way and there are years of muscle
	// memory built on it.
	if (!headset) {
		keymap_button_t button = keymap_button_for_code(code);
		if (button != KEYMAP_BTN_COUNT) {
			run_mapped_action(button);
			return;
		}
	}

	switch (code) {
	case KEY_VOLUMEUP:
	case KEY_VOLUMEDOWN:
		apply_volume_step(code);
		break;

	case KEY_PLAYPAUSE:
		gui_notify_key(GUI_KEY_PLAY_PAUSE);
		break;

	// Only the cable remote reaches here: the side buttons were translated and
	// dispatched above. On a remote next is next -- the upper/lower swap
	// compensates for the buttons on the case, and there are none here.
	case KEY_NEXTSONG:
		gui_notify_key(GUI_KEY_NEXT);
		break;

	case KEY_PREVIOUSSONG:
		gui_notify_key(GUI_KEY_PREV);
		break;

	default:
		// Double-tap wake, decided HERE rather than trusted to the driver:
		// in doze this controller reports plain touch events (BTN_TOUCH) for
		// every tap and no distinct gesture key ever arrives. A real KEY_*
		// code (< 0x100) wakes at once; touch events wake only when TWO of
		// them land within the double-tap window.
		if (!power_screen_is_on()) {
			if (code < 0x100) {
				printf("input: %s code %d while screen off -> waking\n", node, code);
				power_notify_power_button(); // same path as a real power press
				break;
			}
			// ...but only if the option is on. The controller reports taps in
			// doze whether or not gesture mode took, so without this check a
			// double tap would wake the screen with the setting switched off.
			if (!power_double_tap_wake_enabled()) {
				break;
			}

			static uint32_t last_offscreen_touch_ms; // only the touch node gets here
			uint32_t tnow = now_ms();
			if (tnow - last_offscreen_touch_ms <= 500) {
				printf("input: %s double tap while screen off -> waking\n", node);
				last_offscreen_touch_ms = 0;
				power_notify_power_button();
			} else {
				last_offscreen_touch_ms = tnow;
			}
			break;
		}
		// The touch node fires BTN_TOUCH (0x14a) at every tap with the screen
		// on; that is ordinary touch traffic, not worth a log line each.
		if (code >= 0x100 && code <= 0x2ff) {
			break;
		}
		printf("input: %s code %d is not mapped to anything yet\n", node, code);
		break;
	}
}

// Carries out what the clicks on the cable remote's centre button added up to
// (see hookclicks.h for why they are counted here at all).
static void run_hook_gesture(hook_clicks_t *clicks) {
	switch (hook_clicks_take(clicks)) {
	case HOOK_GESTURE_PLAY_PAUSE:
		gui_notify_key(GUI_KEY_PLAY_PAUSE);
		break;
	case HOOK_GESTURE_NEXT:
		gui_notify_key(GUI_KEY_NEXT);
		break;
	case HOOK_GESTURE_PREV:
		gui_notify_key(GUI_KEY_PREV);
		break;
	case HOOK_GESTURE_NONE:
	default:
		break;
	}
}

// One input node and what its thread needs to know about it. Allocated once and
// never freed: the threads never end.
typedef struct {
	char node[32];
	bool headset;	  // the keys on the headphone cable, not those on the case
	int preopened_fd; // already open, or -1 to open `node`
} input_node_t;

static void *input_thread_func(void *arg) {
	const input_node_t *info = arg;
	const char *node = info->node;

	int fd = info->preopened_fd;
	if (fd < 0) {
		fd = open(node, O_RDONLY);
	}
	if (fd < 0) {
		fprintf(stderr, "input: cannot open %s: %s\n", node, strerror(errno));
		return NULL;
	}

	int held = -1;			   // key currently down, or -1
	uint32_t held_since = 0;   // when it went down
	uint32_t next_repeat = 0;  // when the next volume step is due
	bool long_press_done = false;
	// Next and previous do their work on the way up, so that holding them can
	// mean something else. `deferred` says the press left a job for the release;
	// `seeked` says the hold took it over and there is no longer one.
	bool deferred = false;
	bool seeked = false;
	// Clicks on the cable remote's centre button waiting to be told apart.
	hook_clicks_t hook = {0};

	for (;;) {
		// The deadline is whatever the held key is waiting for; with nothing
		// held there is nothing to wait for and poll() blocks.
		int timeout = -1;
		uint32_t now = now_ms();

		if (key_repeats_held(held, info->headset)) {
			timeout = (int)(next_repeat > now ? next_repeat - now : 0);
		} else if (held == KEY_POWER && !long_press_done) {
			uint32_t due = held_since + POWER_LONG_PRESS_MS;
			timeout = (int)(due > now ? due - now : 0);
		}

		// Whichever comes first: the click window can be open with no key down
		// at all, so it is a deadline of its own and not an else.
		int hook_wait = hook_clicks_timeout(&hook, now);
		if (hook_wait >= 0 && (timeout < 0 || hook_wait < timeout)) {
			timeout = hook_wait;
		}

		struct pollfd pfd = {.fd = fd, .events = POLLIN};
		int ready = poll(&pfd, 1, timeout);

		if (ready < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}

		if (ready == 0) {
			// Deadline reached with the key still down.
			now = now_ms();

			// No more clicks are coming: the gesture is whatever was counted.
			if (hook_clicks_expired(&hook, now)) {
				run_hook_gesture(&hook);
			}

			// While the screenshot combination is active these two keys are no
			// longer themselves: the volume does not rise and the power menu
			// does not open. The check is here and not only on press because the
			// combination can complete LATER, on the other node's thread, while
			// this one is already repeating.
			bool suppressed = combo_active();

			if (key_repeats_held(held, info->headset)) {
				bool seeking = key_seeks_held(held, info->headset);
				if (!suppressed) {
					repeat_held(held, info->headset);
				}
				if (seeking) {
					// From here the release has nothing left to do: the press
					// became a seek and must not also change track.
					seeked = true;
				}
				next_repeat = now + (seeking ? SEEK_REPEAT_MS : VOLUME_REPEAT_PERIOD_MS);
			} else if (held == KEY_POWER) {
				long_press_done = true;
				if (!suppressed) {
					printf("input: power held, opening the power menu\n");
					power_notify_activity();
					gui_notify_power_menu();
				}
			}
			continue;
		}

		struct input_event ev;
		if (read(fd, &ev, sizeof(ev)) != sizeof(ev)) {
			break;
		}

		if (ev.type != EV_KEY) {
			continue;
		}

		log_key_event(node, &ev);

		if (ev.value == 1) {
			power_notify_activity(); // any physical button counts as use

			// Volume up + power: if the combination has fired -- now, or because
			// the other key was already down -- this key belongs to it.
			bool combo = combo_press(ev.code);

			// A scrub still open from a button whose release never arrived --
			// another key went down first and took `held` with it -- is closed
			// here rather than left to freeze the position for good.
			if (seeked) {
				device_state_scrub_commit();
			}

			held = ev.code;
			held_since = now_ms();
			bool seeks = key_seeks_held(held, info->headset);
			next_repeat = held_since + (seeks ? SEEK_HOLD_MS : VOLUME_REPEAT_DELAY_MS);
			long_press_done = false;
			deferred = false;
			seeked = false;

			if (combo) {
				// No volume repeat and no countdown to the power menu for as
				// long as the combination lasts.
				held = -1;
				continue;
			}

			// The power key is decided on release (short) or on the deadline
			// (long), so nothing happens here for it. Next and previous are the
			// same shape of decision for the same reason: a press that turns
			// into a hold is a seek, and only a press that does not is a skip.
			if (seeks) {
				deferred = true;
			} else if (info->headset && ev.code == KEY_PLAYPAUSE) {
				// Counted, not acted on: see hookclicks.h.
				hook_clicks_press(&hook, held_since);
			} else if (ev.code != KEY_POWER) {
				// Anything else ends the gesture: a volume key pressed halfway
				// through means the centre button is finished with.
				if (ev.code == KEY_NEXTSONG || ev.code == KEY_PREVIOUSSONG) {
					hook_clicks_cancel(&hook); // the driver counted this one
				} else {
					run_hook_gesture(&hook);
				}
				handle_key_press(node, ev.code, info->headset);
			}
			continue;
		}

		if (ev.value == 0) {
			bool combo = combo_release(ev.code);

			if (ev.code == KEY_POWER && !long_press_done && !combo) {
				// Short press: the screen toggle.
				power_notify_power_button();
			}

			if (ev.code == held) {
				// A skip button let go before it became a seek: now it skips.
				if (deferred && !seeked && !combo) {
					run_mapped_action(keymap_button_for_code(ev.code));
				}
				// And one that did become a seek: this is where the music moves
				// to where the thumb ran to, in a single seek.
				if (seeked) {
					device_state_scrub_commit();
				}
				held = -1;
				long_press_done = false;
				deferred = false;
				seeked = false;
			}
		}
	}

	close(fd);
	return NULL;
}

static void start_input_thread_fd(const char *node, bool headset, int fd) {
	input_node_t *info = calloc(1, sizeof(*info));
	if (!info) {
		return;
	}
	snprintf(info->node, sizeof(info->node), "%s", node);
	info->headset = headset;
	info->preopened_fd = fd;

	pthread_t thread;
	if (pthread_create(&thread, NULL, input_thread_func, info) == 0) {
		pthread_detach(thread);
	} else {
		free(info);
	}
}

#ifndef HOST_BUILD
static void start_input_thread(const char *node, bool headset) { start_input_thread_fd(node, headset, -1); }
#endif

#ifdef HOST_BUILD
// The simulator has no evdev node, so the keyboard writes input_event records
// into a pipe and the ordinary button thread reads that instead. Everything the
// thread does with a real key -- the long press, the volume repeat, the seek on
// hold, the remapping, the screenshot combination -- then works unchanged.
static int host_key_pipe[2] = {-1, -1};

static void start_host_input_thread(void) {
	if (pipe(host_key_pipe) != 0) {
		perror("input: pipe");
		return;
	}
	start_input_thread_fd("keyboard", false, host_key_pipe[0]);
}

void input_host_button(int code, bool down) {
	if (host_key_pipe[1] < 0) {
		return;
	}
	struct input_event ev = {.type = EV_KEY, .code = (unsigned short)code, .value = down ? 1 : 0};
	if (write(host_key_pipe[1], &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
		perror("input: write");
	}
}
#endif

// The keys on the headphone cable come from a separate device -- the
// sa_earpods_adc module, which registers under the name "earpods_adc" -- and
// its /dev/input/eventN is not fixed: it depends on the order the modules
// loaded in.
//
// So the name is matched, not the number, which is what the stock binary does
// too (enumerate /dev/input, ask EVIOCGNAME, compare the "earpods_" prefix).
// Prefix matching for the same reason it uses one: the device name comes from
// the platform name, and HiBy left the door open to a second module of the same
// family.
#ifndef HOST_BUILD
static void start_headset_input_thread(void) {
	DIR *d = opendir("/dev/input");
	if (!d) {
		return;
	}

	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (strncmp(e->d_name, "event", 5) != 0) {
			continue;
		}

		char node[32];
		if ((size_t)snprintf(node, sizeof(node), "/dev/input/%s", e->d_name) >= sizeof(node)) {
			continue;
		}

		int fd = open(node, O_RDONLY);
		if (fd < 0) {
			continue;
		}

		char name[128] = {0};
		bool is_headset = ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 && strncmp(name, "earpods_", 8) == 0;
		close(fd);

		if (is_headset) {
			printf("input: %s is the headset remote (%s)\n", node, name);
			start_input_thread(node, true);
		}
	}

	closedir(d);
}
#endif

// ---------------------------------------------------------------------------
// Bluetooth media keys (AVRCP)
//
// A button on the headphones sends a COMMAND, not audio: bluez receives it over
// AVRCP and re-presents it to the system as a brand-new /dev/input node, created
// on connection and torn down on the next one.
//
// The node is identified by what it reports, not by when it appeared. Timing
// cannot decide it in either direction: bluez keeps the adapter powered when
// this player exits, so on a restart with the headphones still connected the
// remote's node is already there at startup, while a cable remote probed after
// startup or a touch controller re-enumerated appears late without being one.
//
// EVIOCGID gives the bus the node hangs off, and a remote reached over AVRCP is
// BUS_BLUETOOTH; EVIOCGBIT confirms it actually reports transport keys. Between
// them nothing else on this device can be mistaken for a remote, and a remote is
// recognised whenever it appears.
//
// Each node gets its own thread in a blocking read: when the headphones
// disconnect the kernel removes the node, the read fails and the thread exits by
// itself. On reconnection the node reappears (often under a different number)
// and the scanner picks it up again.
// ---------------------------------------------------------------------------

#define BT_INPUT_SCAN_MS 2000
#define BT_INPUT_MAX 4 // more Bluetooth remotes connected AT ONCE do not exist

static pthread_mutex_t bt_input_lock = PTHREAD_MUTEX_INITIALIZER;
static char bt_input_claimed[BT_INPUT_MAX][32];

// The transport keys a media remote has to offer at least one of. A headset
// that reports none of them is not what this scanner is looking for, whatever
// bus it is on.
static const int BT_REMOTE_KEYS[] = {
	KEY_PLAYPAUSE, KEY_PLAYCD, KEY_PLAY, KEY_PAUSECD, KEY_PAUSE, KEY_STOPCD, KEY_STOP, KEY_NEXTSONG, KEY_PREVIOUSSONG,
};

// Whether this node is a media remote arriving over Bluetooth. Opened read-only
// and closed again: the reader thread opens its own descriptor.
//
// Two tests, not one. The bus first: BUS_BLUETOOTH is what bluez stamps on the
// node it builds, and BUS_VIRTUAL is what a uinput device carries when whoever
// created it left the field alone -- neither is a bus this device has any
// hardware on, so between them nothing of ours can be mistaken for a remote.
// Then the keys: a node offering none of the transport keys is not what this
// is looking for, whatever bus it claims.
static bool bt_input_is_remote(const char *node) {
	int fd = open(node, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		return false;
	}

	bool remote = false;
	struct input_id id;
	if (ioctl(fd, EVIOCGID, &id) == 0 && (id.bustype == BUS_BLUETOOTH || id.bustype == BUS_VIRTUAL)) {
		unsigned long bits[(KEY_MAX + 1 + 8 * sizeof(long) - 1) / (8 * sizeof(long))];
		memset(bits, 0, sizeof(bits));
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) >= 0) {
			for (size_t i = 0; i < sizeof(BT_REMOTE_KEYS) / sizeof(BT_REMOTE_KEYS[0]); i++) {
				int code = BT_REMOTE_KEYS[i];
				if (bits[code / (8 * sizeof(long))] & (1UL << (code % (8 * sizeof(long))))) {
					remote = true;
					break;
				}
			}
		}
	}

	close(fd);
	return remote;
}

// How many event nodes there were last time round.
//
// A pair of headphones connecting adds one and disconnecting takes it away, so
// a change in the count is the moment worth looking at -- and when the buttons
// do nothing, the log then says whether bluez built a node at all, what it
// called it and what bus it put it on. Cheaper than a line per node per scan,
// and it is the one dump that answers the question.
static int bt_input_node_count(void) {
	DIR *d = opendir("/dev/input");
	if (!d) {
		return -1;
	}
	int count = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (strncmp(e->d_name, "event", 5) == 0) {
			count++;
		}
	}
	closedir(d);
	return count;
}

// True when the node was claimed HERE, meaning no other thread is reading it.
static bool bt_input_claim(const char *node) {
	bool taken = false;
	pthread_mutex_lock(&bt_input_lock);
	int free_slot = -1;
	bool already = false;
	for (int i = 0; i < BT_INPUT_MAX; i++) {
		if (bt_input_claimed[i][0] == '\0') {
			if (free_slot < 0) {
				free_slot = i;
			}
		} else if (strcmp(bt_input_claimed[i], node) == 0) {
			already = true;
		}
	}
	if (!already && free_slot >= 0) {
		snprintf(bt_input_claimed[free_slot], sizeof(bt_input_claimed[free_slot]), "%s", node);
		taken = true;
	}
	pthread_mutex_unlock(&bt_input_lock);
	return taken;
}

static void bt_input_release(const char *node) {
	pthread_mutex_lock(&bt_input_lock);
	for (int i = 0; i < BT_INPUT_MAX; i++) {
		if (strcmp(bt_input_claimed[i], node) == 0) {
			bt_input_claimed[i][0] = '\0';
		}
	}
	pthread_mutex_unlock(&bt_input_lock);
}

// Executes one AVRCP command. PLAY and PAUSE are ORDERS, not toggles: the
// headphones state which condition they want (an AirPod taken out of the ear
// sends PAUSE, put back sends PLAY), and if that state already holds nothing is
// done -- with a blind toggle a second PAUSE would restart the music.
static void handle_bt_remote_key(const char *node, int code) {
	power_notify_activity();

	// The same command can reach the player twice -- here, and as a method call
	// on the media player registered with bluez. Acting on both toggles twice.
	if (btplayer_command_is_duplicate()) {
		printf("input: %s code %d already handled by the registered player\n", node, code);
		return;
	}

	// What was decided, said out loud -- including the times nothing was done.
	// An order already satisfied is a legitimate outcome (see below), but from
	// the outside it looks exactly like a button that did not arrive, and the
	// two need different fixes.
	audio_status_t status = audio_get_status();
	const char *state = status == AUDIO_STATUS_PLAYING ? "playing"
													   : (status == AUDIO_STATUS_PAUSED ? "paused" : "stopped");

	switch (code) {
	// Play is a TAP, not an order.
	//
	// It cannot be read as an order -- "start, if you are not already" --
	// because the headphones never learn what this player is doing: with no
	// media player registered with bluez there is nothing to answer their
	// status queries, so they believe playback is stopped for ever and send
	// PLAY at every single tap, including the taps meant to pause. Obeying
	// that literally makes the button work once and then never again.
	//
	// Pause and stop below keep the order reading, because those the
	// headphones send when they know something this player does not -- an
	// earbud taken out.
	case KEY_PLAYCD:
	case KEY_PLAY:
	case KEY_PLAYPAUSE:
		printf("input: %s play/pause (was %s)\n", node, state);
		gui_notify_key(GUI_KEY_PLAY_PAUSE);
		break;

	case KEY_PAUSECD:
	case KEY_PAUSE:
	case KEY_STOPCD:
	case KEY_STOP:
		if (status == AUDIO_STATUS_PLAYING) {
			printf("input: %s pause\n", node);
			gui_notify_key(GUI_KEY_PLAY_PAUSE);
		} else {
			printf("input: %s pause, but it is already %s: nothing to do\n", node, state);
		}
		break;

	// No upper/lower swap here: that compensates for the buttons on the case,
	// and these are headphones -- next is next.
	case KEY_NEXTSONG:
	case KEY_FASTFORWARD:
		printf("input: %s next\n", node);
		gui_notify_key(GUI_KEY_NEXT);
		break;

	case KEY_PREVIOUSSONG:
	case KEY_REWIND:
		printf("input: %s previous\n", node);
		gui_notify_key(GUI_KEY_PREV);
		break;

	// A volume request COMING FROM the headphones is not the locked case (that
	// one blocks the device keys when the volume belongs to the headphones):
	// here the headphones themselves are asking, so it is carried out.
	case KEY_VOLUMEUP:
	case KEY_VOLUMEDOWN:
		device_state_change_volume_percent(code == KEY_VOLUMEUP ? VOLUME_STEP_PERCENT : -VOLUME_STEP_PERCENT);
		gui_notify_volume(device_state_get_volume_percent());
		break;

	default:
		printf("input: %s AVRCP code %d not mapped\n", node, code);
		break;
	}
}

static void *bt_input_thread_func(void *arg) {
	char *node = arg; // strdup from the scanner: freed here
	int fd = open(node, O_RDONLY);
	if (fd < 0) {
		// Appeared and vanished between the scan and the open: release it, and
		// the scanner picks it up again if it comes back.
		bt_input_release(node);
		free(node);
		return NULL;
	}

	char name[128] = {0};
	if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) {
		snprintf(name, sizeof(name), "?");
	}

	// Which transport keys the node says it can send. bluez sets these bits
	// from its own key map when it builds the device, so the list is what it
	// intends to deliver -- and a press that produces nothing is then a
	// question about the headphones, not about this end.
	char keys[128];
	int used = 0;
	keys[0] = '\0';
	{
		unsigned long bits[(KEY_MAX + 1 + 8 * sizeof(long) - 1) / (8 * sizeof(long))];
		memset(bits, 0, sizeof(bits));
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) >= 0) {
			for (size_t i = 0; i < sizeof(BT_REMOTE_KEYS) / sizeof(BT_REMOTE_KEYS[0]); i++) {
				int code = BT_REMOTE_KEYS[i];
				if (bits[code / (8 * sizeof(long))] & (1UL << (code % (8 * sizeof(long))))) {
					used += snprintf(keys + used, sizeof(keys) - (size_t)used, "%s%d", used ? "," : "", code);
					if (used >= (int)sizeof(keys)) {
						break;
					}
				}
			}
		}
	}
	printf("input: %s is a Bluetooth remote (%s), declared keys: %s\n", node, name,
		   keys[0] ? keys : "none");

	for (;;) {
		struct input_event ev;
		if (read(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
			break; // the node dies with the disconnection: ENODEV, so exit
		}
		if (ev.type != EV_KEY || ev.value != 1) {
			continue; // AVRCP commands act on press, not on release
		}
		log_key_event(node, &ev);
		handle_bt_remote_key(node, ev.code);
	}

	close(fd);
	printf("input: %s is gone (Bluetooth remote disconnected)\n", node);
	bt_input_release(node);
	free(node);
	return NULL;
}

static void *bt_input_scan_thread(void *arg) {
	(void)arg;
	int last_node_count = bt_input_node_count();
	for (;;) {
		// With Bluetooth off the node cannot exist: sleep longer and do not even
		// open the directory.
		if (!bluetooth_get_enabled()) {
			usleep(5000 * 1000);
			continue;
		}
		usleep(BT_INPUT_SCAN_MS * 1000);

		int count = bt_input_node_count();
		if (count >= 0 && count != last_node_count) {
			// The one line, not the whole of /proc/bus/input/devices. The dump
			// was what found the AVRCP node in the first place; now that the
			// node announces itself below, a headset going in and out of its
			// case would be fifty lines onto the card every couple of seconds.
			printf("input: /dev/input changed (%d nodes -> %d)\n", last_node_count, count);
			last_node_count = count;
		}

		DIR *d = opendir("/dev/input");
		if (!d) {
			continue;
		}
		struct dirent *e;
		while ((e = readdir(d)) != NULL) {
			if (strncmp(e->d_name, "event", 5) != 0) {
				continue;
			}
			char node[32];
			if ((size_t)snprintf(node, sizeof(node), "/dev/input/%s", e->d_name) >= sizeof(node)) {
				continue;
			}
			if (!bt_input_is_remote(node) || !bt_input_claim(node)) {
				continue;
			}
			char *copy = strdup(node);
			pthread_t thread;
			if (copy && pthread_create(&thread, NULL, bt_input_thread_func, copy) == 0) {
				pthread_detach(thread);
			} else {
				bt_input_release(node);
				free(copy);
			}
		}
		closedir(d);
	}
	return NULL;
}

static void start_bt_input_scanner(void) {
	pthread_t thread;
	if (pthread_create(&thread, NULL, bt_input_scan_thread, NULL) == 0) {
		pthread_detach(thread);
	}
}

/*
 * Starts system services including:
 *     - SD card detection + mounting
 *     - Audio service initializing
 */
void system_start_services(system_config_t *cfg, system_notification_cb_t notification_cb, void *user_data) {
	static bool initialized = false;
	if (initialized)
		return;

	static system_runtime_t runtime;

	// --- Battery ---
	g_battery_cfg = cfg->battery_cfg;

	// --- Storage ---
	runtime.storage_cfg = cfg->storage_cfg;
	// Hotplug events are matched on the device name. With no configured device
	// the player watches every mmcblk node, which is what the stock player does
	// (it keys off DEVNAME=/DEVTYPE= and the "mmcblk" prefix).
	runtime.device_name = cfg->storage_cfg->device ? storage_device_name(cfg->storage_cfg->device) : "mmcblk";
	runtime.notification_cb = notification_cb;
	runtime.notification_user_data = user_data;

	g_storage_cfg = cfg->storage_cfg;
	g_runtime = &runtime;

	check_and_mount_existing_sd(cfg->storage_cfg, &runtime); // mount sd on startup, if it's present

	pthread_t supervisor;
	if (pthread_create(&supervisor, NULL, card_supervisor_thread, NULL) == 0) {
		pthread_detach(supervisor);
		printf("storage: card supervisor watching %s\n", sd_root);
	}

	pthread_t sd_thread;
	if (pthread_create(&sd_thread, NULL, sd_hotplug_thread, &runtime) != 0) {
		fprintf(stderr, "Failed to start SD hotplug thread :(\n");
	} else {
		printf("Started SD hotplug thread :)\n");
		pthread_detach(sd_thread);
	}

	log_input_devices();

#ifdef HOST_BUILD
	start_host_input_thread();
#else
	// One thread per button node. event1 is the touchscreen and is LVGL's for
	// touches -- but it is watched here too, because with double-tap wake on,
	// the touch controller reports the wake tap as a power-key press on its
	// own node while the panel is blanked. Touch coordinates are ABS events
	// this thread simply ignores; only key events are acted on.
	static const char *const button_nodes[] = {"/dev/input/event0", "/dev/input/event2", "/dev/input/event1"};
	for (size_t i = 0; i < sizeof(button_nodes) / sizeof(button_nodes[0]); i++) {
		start_input_thread(button_nodes[i], false);
	}

	start_headset_input_thread();
#endif
	start_bt_input_scanner();

	// --- Audio ---
	audio_init();

	initialized = true;
}
