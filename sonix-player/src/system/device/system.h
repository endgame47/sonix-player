#ifndef SYSTEM_H
#define SYSTEM_H

#include <stdbool.h>

typedef struct {
	// Preferred block device, e.g. "/dev/mmcblk0p1". May be NULL: the player
	// then probes the nodes the stock firmware knows about (mmcblk0p1,
	// mmcblk1p1, and the whole-disk nodes for cards without a partition table).
	const char *device;

	// Preferred mount point. May be NULL, in which case the player picks
	// whichever of HiBy OS's three names for the same directory actually exists
	// on this firmware (/mnt/sd_0, /data/mnt/sd_0, /usr/data/mnt/sd_0 -- two of
	// them are symlinks, but which two differs between builds).
	const char *mount_point;

	// Whether the player mounts the card itself.
	//
	// This has to be true on the device. Nothing in HiBy OS's boot sequence
	// mounts the microSD: mdev has no mmcblk rule, and sys_server only mounts
	// when something asks it to over its socket. The thing that asks is the
	// stock hiby_player, which doubles as the hotplug manager -- so replacing
	// it means taking over that job too, which is exactly what this does.
	bool manage_mount;
} storage_config_t;

typedef struct {
	const char *battery_capacity_file;
	// sysfs "status" node: "Charging", "Discharging", "Full", "Not charging".
	// May be NULL, in which case charging always reads as false.
	const char *battery_status_file;
} battery_config_t;

typedef struct {
	storage_config_t *storage_cfg;
	battery_config_t *battery_cfg;
} system_config_t;

typedef void (*system_notification_cb_t)(const char *message, void *user_data);

// Re-reads the battery level and charging state from sysfs into the cache the
// two readers below serve.
void sync_battery_from_sysfs(void);
char *read_battery_percent();

// True while the charger is plugged in (sysfs status "Charging" or "Full").
bool read_battery_charging(void);

// Hides the charging indication (battery icon, status LED) while something has
// deliberately turned the charger off -- DAC mode with charging disabled.
void system_suppress_charging(bool suppressed);

void system_start_services(system_config_t *cfg, system_notification_cb_t notification_cb, void *user_data);

// The directory the card ended up mounted on, valid once
// system_start_services() has returned. This is what the file browser should
// read: hardcoding a path guesses wrong on firmwares that arrange the
// /mnt -> /data/mnt -> /usr/data/mnt symlinks differently.
const char *storage_sd_root(void);

// Whether the card can be written to. False for a card mounted read-only -- an
// NTFS one where ntfs-3g is not on the device, or an adapter with its
// write-protect notch closed. Everything that keeps state on the card asks
// this rather than discovering it as a silent failure.
bool storage_sd_writable(void);

// The SD block device the storage layer settled on (e.g. /dev/mmcblk0p1),
// or NULL when no card was found. Used by the USB mass-storage export.
const char *storage_sd_device(void);

// Whether the card is in the slot and everything that lives on it is open.
// False from the moment a removal is seen until a card is mounted again, which
// is what anything asking "is there a card" wants -- the block device name
// outlives the card that was pulled out.
bool storage_card_attached(void);

// Looks at the card and puts it back if it has gone missing: a mount that has
// died under it -- the card was pulled, or a USB export unmounted it and the
// remount did not take -- is cleared, the card is mounted again, and
// everything that lives on it is reopened.
//
// Safe to call at any time and from any thread. It does nothing when the card
// is fine, and nothing while a USB export deliberately has the card away.
//
// A supervisor thread calls this every few seconds, and that is what makes the
// card come back on its own rather than staying dark until a reboot: the paths
// that can lose it are many (hotplug, USB export, ADB rearranging the gadget
// under the export) and hardening each of them one at a time has not worked.
void storage_recheck_card(void);

#ifdef HOST_BUILD
// Feeds one key press or release to the button thread, as if it had come off
// an evdev node. `code` is a KEY_* from linux/input.h.
void input_host_button(int code, bool down);
#endif

#endif /* SYSTEM_H */
