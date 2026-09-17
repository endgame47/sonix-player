#!/bin/bash
#
# FIXME: integrate into sonix_firmware_packer.sh and support multiple devices.
#
# NOTE: exclude 'assets/module_driver' as those are R3ProII-specific
#
# Builds a HiByOS firmware image that boots Sonix Player instead of the stock
# music player.
#
# Everything it needs sits next to it:
#
#   sonix_firmware_packer.sh   this script
#   r1_original.upt            the stock firmware to start from
#   sonix_player               the binary to install
#   assets/                    an overlay copied onto the root of the rootfs
#       |
#       v
#   r1.upt                the result
#
# No questions asked: run it and it does the lot.

set -euo pipefail
export COLUMNS=1

# --- terminal colours ---
RED='\033[1;31m'
GREEN='\033[1;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Homebrew, for macOS.
export PATH="/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

say()  { echo -e "$@"; }
step() { echo -e "${BLUE}==>${NC} $*"; }
warn() { echo -e "${YELLOW}!!${NC}  $*"; }
die()  { echo -e "${RED}Error:${NC} $*" >&2; exit 1; }

# ==========================================================================
# Where everything lives
# ==========================================================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"

SOURCE_UPT="$SCRIPT_DIR/r1_original.upt"
PLAYER_BIN="$SCRIPT_DIR/sonix_player"
OUTPUT_UPT="$SCRIPT_DIR/r1.upt"

WORK_DIR="$SCRIPT_DIR/temp"
OTA_DIR="$WORK_DIR/ota_v0"
SQUASH_DIR="$SCRIPT_DIR/squashfs-root"
MERGED_SQUASHFS="$SCRIPT_DIR/rootfs_original.squashfs"

# Paths inside the rootfs.
STOCK_PLAYER="usr/bin/hiby_player"
STOCK_LAUNCHER="usr/bin/hiby_player.sh"
NEW_LAUNCHER="usr/bin/sonix_player.sh"
INIT_SCRIPT="etc/init.d/S92_03_start_music_player"
SYSTEM_INFO="usr/resource/sonix/components/system-info.json"

# The stock interface's own resources. Nothing on the device reads them once
# the player is replaced, and they are several megabytes of an image that has
# to fit in flash.
#
# Note what is NOT in this list: usr/resource/sonix, which is where the assets
# folder lands. The stock fonts and strings live one level above it and go;
# ours stay.
STOCK_UI_DIRS=(usr/resource/litegui usr/resource/layout usr/resource/str usr/resource/fonts)

# The working folders always go, whether the run succeeded or not.
cleanup() { rm -rf "$WORK_DIR" "$SQUASH_DIR" "$MERGED_SQUASHFS" 2>/dev/null || true; }
trap cleanup EXIT

say "${BLUE}###############################################${NC}"
say "${BLUE}###   SONIX PLAYER -- FIRMWARE PACKER       ###${NC}"
say "${BLUE}###############################################${NC}"
say ""

# ==========================================================================
# Portability: md5, file size, build stamp
# ==========================================================================
if command -v md5sum >/dev/null 2>&1; then
	get_md5() { md5sum "$1" | awk '{print $1}'; }
else
	get_md5() { md5 -q "$1"; }
fi

if stat -c%s . >/dev/null 2>&1; then
	get_size()  { stat -c%s "$1"; }
	# When the file was last written, as DDMMYYYYHHMM. GNU date takes the file
	# itself here; the BSD one takes seconds, which is why this is not one line
	# for both.
	get_stamp() { date -r "$1" +%d%m%Y%H%M; }
else
	get_size()  { stat -f%z "$1"; }
	get_stamp() { stat -f%Sm -t %d%m%Y%H%M "$1"; }
fi

# sed -i is spelled differently on the two systems, so nothing here uses it:
# the file is rewritten through a temporary and moved into place.
sed_file() {
	local expression="$1" file="$2"
	sed -E "$expression" "$file" > "$file.packer.tmp"
	mv "$file.packer.tmp" "$file"
}

# How many times a string appears in a file -- every occurrence, not the lines
# that hold one. A single line can kill both hiby_player.sh and hiby_player,
# and counting lines would report that as one.
count_in() { tr -c '[:alnum:]_./' '\n' < "$2" | grep -c -- "$1" || true; }

# Replaces every hiby_player with sonix_player, and proves it: the count before,
# every resulting line printed, and a hard stop if anything survived.
#
# One substitution covers both spellings -- a reference to `hiby_player` becomes
# `sonix_player`, and one to `hiby_player.sh` becomes `sonix_player.sh`, which
# is the name the launcher has just been given.
rename_player_in() {
	local file="$1" label="$2"
	local before
	before="$(count_in "hiby_player" "$file")"

	sed_file "s/hiby_player/sonix_player/g" "$file"

	if grep -q "hiby_player" "$file"; then
		die "$label still mentions hiby_player after the rewrite."
	fi
	say "    $label: $before occurrence(s) of hiby_player replaced"
	grep -n "sonix_player" "$file" | sed 's/^/        /'
}

# ==========================================================================
# What has to be there before anything is touched
# ==========================================================================
step "Checking what is needed"

# A string and not an array: bash 3.2, which is the one macOS ships, treats
# ${#array[@]} on an empty array as an unbound variable under `set -u`.
MISSING=""
for tool in 7z unsquashfs mksquashfs split tar; do
	command -v "$tool" >/dev/null 2>&1 || MISSING="$MISSING $tool"
done
# The ISO writer answers to more than one name depending on the system.
ISO_TOOL=""
for tool in mkisofs genisoimage xorrisofs; do
	if command -v "$tool" >/dev/null 2>&1; then
		ISO_TOOL="$tool"
		break
	fi
done
[ -n "$ISO_TOOL" ] || MISSING="$MISSING mkisofs"

if [ -n "$MISSING" ]; then
	die "missing:$MISSING
  Debian/Ubuntu:  sudo apt install p7zip-full squashfs-tools genisoimage
  macOS:          brew install p7zip squashfs cdrtools"
fi

[ -f "$SOURCE_UPT" ] || die "$(basename "$SOURCE_UPT") is not next to this script."
[ -f "$PLAYER_BIN" ] || die "sonix_player is not next to this script."

# The overlay folder, whatever case it was created in.
ASSETS_DIR=""
ASSETS_SEEN=0
while IFS= read -r candidate; do
	ASSETS_SEEN=$((ASSETS_SEEN + 1))
	[ -n "$ASSETS_DIR" ] || ASSETS_DIR="$candidate"
done < <(find "$SCRIPT_DIR" -maxdepth 1 -type d -iname "assets" 2>/dev/null | sort)

if [ "$ASSETS_SEEN" -gt 1 ]; then
	die "there are $ASSETS_SEEN folders called assets next to this script, differing
  only in case. Keep one: which of them should win is not obvious."
fi

if [ -n "$ASSETS_DIR" ]; then
	say "    assets:  $(basename "$ASSETS_DIR")/"
else
	warn "no assets folder next to the script: the image will carry the binary and nothing else."
fi

BUILD_STAMP="$(get_stamp "$PLAYER_BIN")"
TODAY="$(date +%d%m%Y)"
say "    binary:  sonix_player, $(get_size "$PLAYER_BIN") bytes, built $BUILD_STAMP"
if [ "${BUILD_STAMP:0:8}" != "$TODAY" ]; then
	warn "sonix_player was not built today; build_version will still say when it was."
fi
say ""

# ==========================================================================
# 1. Unpack the stock firmware
# ==========================================================================
step "Unpacking $(basename "$SOURCE_UPT")"

rm -rf "$WORK_DIR" "$SQUASH_DIR" "$MERGED_SQUASHFS"
mkdir -p "$WORK_DIR"

7z x "$SOURCE_UPT" -o"$WORK_DIR" -y > /dev/null

[ -d "$OTA_DIR" ] || die "there is no ota_v0 folder inside the .upt: not a firmware of this kind."

say "    Joining the rootfs chunks..."
( cd "$OTA_DIR" && cat rootfs.squashfs.* ) > "$MERGED_SQUASHFS"

say "    Extracting the filesystem..."
unsquashfs -f -d "$SQUASH_DIR" "$MERGED_SQUASHFS" > /dev/null
rm -f "$MERGED_SQUASHFS"

# The old chunks go now: what gets written back must not sit next to what it
# replaces, or the image ends up carrying both.
rm -f "$OTA_DIR/ota_md5_rootfs.squashfs."* "$OTA_DIR/rootfs."*

[ -f "$SQUASH_DIR/$STOCK_LAUNCHER" ] || die "the rootfs has no $STOCK_LAUNCHER: unexpected firmware."
[ -f "$SQUASH_DIR/$INIT_SCRIPT" ]    || die "the rootfs has no $INIT_SCRIPT: unexpected firmware."
say ""

# ==========================================================================
# 2. The binary
# ==========================================================================
step "Installing sonix_player"

chmod 777 "$PLAYER_BIN"

if [ -e "$SQUASH_DIR/$STOCK_PLAYER" ]; then
	rm -f "$SQUASH_DIR/$STOCK_PLAYER"
	say "    $STOCK_PLAYER removed"
else
	warn "$STOCK_PLAYER was not there to begin with"
fi

cp -f "$PLAYER_BIN" "$SQUASH_DIR/usr/bin/sonix_player"
chmod 777 "$SQUASH_DIR/usr/bin/sonix_player"
say "    usr/bin/sonix_player installed (777)"
say ""

# ==========================================================================
# 3. The launcher script
# ==========================================================================
step "Renaming the launcher"

mv "$SQUASH_DIR/$STOCK_LAUNCHER" "$SQUASH_DIR/$NEW_LAUNCHER"
say "    $STOCK_LAUNCHER -> $NEW_LAUNCHER"
rename_player_in "$SQUASH_DIR/$NEW_LAUNCHER" "$NEW_LAUNCHER"
chmod 755 "$SQUASH_DIR/$NEW_LAUNCHER"
say ""

# ==========================================================================
# 4. The init script
# ==========================================================================
step "Updating $INIT_SCRIPT"

rename_player_in "$SQUASH_DIR/$INIT_SCRIPT" "$INIT_SCRIPT"
chmod 755 "$SQUASH_DIR/$INIT_SCRIPT"
say ""

# ==========================================================================
# 5. The overlay
# ==========================================================================
if [ -n "$ASSETS_DIR" ]; then
	step "Copying $(basename "$ASSETS_DIR")/ onto the root of the rootfs"

	# tar rather than cp: it merges into directories that already exist,
	# overwrites the files that clash, carries the hidden ones, and behaves the
	# same way on macOS and on Linux -- none of which is true of `cp -a` on
	# both.
	( cd "$ASSETS_DIR" && tar --exclude='module_driver' -cf - . ) | ( cd "$SQUASH_DIR" && tar xf - )

	COPIED="$(cd "$ASSETS_DIR" && find . -type f | wc -l | tr -d ' ')"
	say "    $COPIED files copied"
	say ""
fi

# ==========================================================================
# 6. The stock interface's resources
# ==========================================================================
step "Removing the stock interface's folders"

for dir in "${STOCK_UI_DIRS[@]}"; do
	if [ -e "$SQUASH_DIR/$dir" ]; then
		rm -rf "$SQUASH_DIR/$dir"
		[ -e "$SQUASH_DIR/$dir" ] && die "could not remove $dir."
		say "    $dir removed"
	else
		say "    $dir was not there"
	fi
done
say ""

# ==========================================================================
# 7. The build stamp
# ==========================================================================
step "build_version in $SYSTEM_INFO"

JSON="$SQUASH_DIR/$SYSTEM_INFO"
[ -f "$JSON" ] || die "$SYSTEM_INFO is not in the rootfs.
  It should come from the assets folder: assets/$SYSTEM_INFO"

# Whatever case the key was written in. The player reads it case-insensitively
# (find_nocase in src/system/sysinfo.c), so "Build_version" and "build_version"
# are equally correct in the file and this has to accept the same. The spelling
# is read out first and then used literally in the substitution, because sed's
# case-insensitive flag is a GNU extension and this has to run on macOS too.
KEY="$(grep -oE '"[Bb][Uu][Ii][Ll][Dd]_[Vv][Ee][Rr][Ss][Ii][Oo][Nn]"' "$JSON" | head -1 || true)"
[ -n "$KEY" ] || die "$SYSTEM_INFO has no build_version key, in any case."

sed_file "s/($KEY[[:space:]]*:[[:space:]]*\")[^\"]*(\")/\1$BUILD_STAMP\2/" "$JSON"

grep -q "\"$BUILD_STAMP\"" "$JSON" || die "$KEY was not rewritten."
say "    $KEY = $BUILD_STAMP"
say ""

# ==========================================================================
# 8. Repack
# ==========================================================================
say "${YELLOW}###############################${NC}"
say "${YELLOW}###   NEW FILESYSTEM        ###${NC}"
say "${YELLOW}###############################${NC}"
say ""

step "Clearing macOS clutter"
find "$SQUASH_DIR" -name '.DS_Store' -type f -delete 2>/dev/null || true
find "$SQUASH_DIR" -name '._*' -type f -delete 2>/dev/null || true

# Nothing this script writes belongs in the image.
find "$SQUASH_DIR" -name '*.packer.tmp' -type f -delete 2>/dev/null || true

ROOTFS_NEW="$OTA_DIR/rootfs.squashfs"

step "Building the filesystem"
mksquashfs "$SQUASH_DIR" "$ROOTFS_NEW" -comp lzo -all-root > /dev/null

ORIGINAL_SUM="$(get_md5 "$ROOTFS_NEW")"
SIZE="$(get_size "$ROOTFS_NEW")"
say "    rootfs.squashfs: $SIZE bytes, md5 $ORIGINAL_SUM"

step "Updating ota_update.in"
# The kernel is not touched, so its two lines are carried over exactly as they
# were: recomputing them from a file this script never writes would be a way of
# getting them wrong.
X_SIZE="$(grep -A 3 'img_name=xImage' "$OTA_DIR/ota_update.in" | grep 'img_size' | cut -d= -f2 | tr -d '\r ')"
X_MD5="$(grep -A 3 'img_name=xImage' "$OTA_DIR/ota_update.in" | grep 'img_md5' | cut -d= -f2 | tr -d '\r ')"

[ -n "$X_SIZE" ] && [ -n "$X_MD5" ] || die "cannot read the kernel entry from ota_update.in."

cat > "$OTA_DIR/ota_update.in" <<EOF
ota_version=0

img_type=kernel
img_name=xImage
img_size=$X_SIZE
img_md5=$X_MD5

img_type=rootfs
img_name=rootfs.squashfs
img_size=$SIZE
img_md5=$ORIGINAL_SUM
EOF

step "Splitting into chunks and building the md5 chain"
split -b 524288 -a 4 "$ROOTFS_NEW" "$OTA_DIR/temp_chunk_"
rm -f "$ROOTFS_NEW"

MD5_FILE="$OTA_DIR/ota_md5_rootfs.squashfs.$ORIGINAL_SUM"
: > "$MD5_FILE"

count=0
CURRENT_SUM="$ORIGINAL_SUM"
for f in "$OTA_DIR/temp_chunk_"*; do
	[ -e "$f" ] || continue
	suffix="$(printf "%04d" $count)"
	NEW_FILENAME="$OTA_DIR/rootfs.squashfs.$suffix.$CURRENT_SUM"
	mv "$f" "$NEW_FILENAME"
	CURRENT_SUM="$(get_md5 "$NEW_FILENAME")"
	echo "$CURRENT_SUM" >> "$MD5_FILE"
	count=$((count + 1))
done
say "    $count chunks"
say ""

# ==========================================================================
# 9. The image
# ==========================================================================
say "${YELLOW}###############################${NC}"
say "${YELLOW}###   FIRMWARE IMAGE        ###${NC}"
say "${YELLOW}###############################${NC}"
say ""

step "Writing $(basename "$OUTPUT_UPT")"
rm -f "$OUTPUT_UPT"
"$ISO_TOOL" -o "$OUTPUT_UPT" -J -r "$WORK_DIR" > /dev/null 2>&1

[ -f "$OUTPUT_UPT" ] || die "the image was not written."

cleanup

say ""
say "${GREEN}#############################${NC}"
say "${GREEN}###   DONE                ###${NC}"
say "${GREEN}#############################${NC}"
say ""
say "  $OUTPUT_UPT"
say "  $(get_size "$OUTPUT_UPT") bytes, build_version $BUILD_STAMP"
say ""
say "${GREEN}  Ready to be copied to the SD card.${NC}"
say ""
