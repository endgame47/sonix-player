#include "firmware.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/audio/audio.h"
#include "src/system/device/clock.h"
#include "src/system/core/config.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/remote/dlna.h"
#include "src/system/device/system.h"

// The card's own name for itself when the config carries no firmware_name.
// The stock player reads it from key 7 ("device"); on this hardware that is
// always R3PROII, so it is the fallback rather than a lookup.
#ifdef BOARD_R1
	#define FIRMWARE_DEFAULT_DEVICE "R1"
#else
	#define FIRMWARE_DEFAULT_DEVICE "R3PROII"
#endif

// The card is scanned once and every name compared case-insensitively,
// rather than guessing at spellings: FAT is case-insensitive but exFAT and
// ext4 are not, and a file written by Windows can land as UPDATE.UPT while
// the stock player only ever looks for update.upt. One readdir covers them
// all -- and there is no cheaper way to be right about it.
static bool find_in_dir(const char *root, const char *stem, char *out, size_t out_size) {
	char wanted[80];
	snprintf(wanted, sizeof(wanted), "%s.upt", stem);

	DIR *dir = opendir(root);
	if (!dir) {
		return false;
	}

	bool found = false;
	struct dirent *de;
	while (!found && (de = readdir(dir)) != NULL) {
		if (strcasecmp(de->d_name, wanted) != 0) {
			continue;
		}
		snprintf(out, out_size, "%s/%s", root, de->d_name);

		struct stat st;
		if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) {
			found = true;
		}
	}

	closedir(dir);
	if (!found) {
		out[0] = '\0';
	}
	return found;
}

bool firmware_update_file_find(char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return false;
	}
	out[0] = '\0';

	const char *root = storage_sd_root();
	if (!root || !*root) {
		return false;
	}

	// ONLY the device's own name -- deliberately NOT the stock binary's
	// "update.upt" fallback.
	//
	// The recovery kernel does not check what it is given: a .upt built for
	// another HiBy model, dropped on the card under the generic name, would be
	// written to the flash exactly the same way, and the device would not come
	// back. "update.upt" is a name anyone might use for anything; "r3proii.upt"
	// is a claim about which player the file is for. Losing the convenience of
	// the generic name is a fair price for that.
	const char *name = config_get("firmware", "name", "");
	if (!name || !*name) {
		name = FIRMWARE_DEFAULT_DEVICE;
	}
	return find_in_dir(root, name, out, out_size);
}

void firmware_update_start(void) {
#ifdef HOST_BUILD
	printf("firmware: (host build) would arm recovery and reboot\n");
#else
	printf("firmware: arming recovery\n");

	// Playback down first: the card is about to be handed to the recovery
	// kernel, and the decoder still has it open.
	audio_stop();

	// The streaming caches (tracks and covers) survive no power cycle, the
	// update's included.
	qobuzcache_clear_on_exit();
	tidalcache_clear_on_exit();
	podcastcache_clear_on_exit();
	dlna_clear_on_exit();

	// The clock goes into the RTC on the way out, like every other exit path
	// -- recovery reboots the device a second time when it is done, and the
	// time would otherwise be whatever the RTC last knew.
	clock_shutdown();

	sync();
	sleep(1);

	// Two commands: erase the first block of /dev/mtd5, write "ota:kernel2"
	// into it. That is the whole of the stock firmware's bootmode.sh, and it
	// is what U-Boot reads to decide which kernel to start.
	int rc = system("/usr/bin/bootmode.sh Recovery");
	if (rc != 0) {
		printf("firmware: bootmode.sh Recovery failed (rc=%d)\n", rc);
	}

	sync();
	sleep(1);

	rc = system("reboot");
	(void)rc;

	// Spins here rather than returning, as the stock binary does: with recovery
	// already armed there is nothing sensible left to do, and going back to the
	// settings page would leave the device one power-cycle away from an update
	// the user is no longer expecting.
	sleep(10);
	reboot(RB_AUTOBOOT);
	for (;;) {
		sleep(1);
	}
#endif
}
