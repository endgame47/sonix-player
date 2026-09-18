#!/usr/bin/env python3
"""
Removes the one-contact cap from the R1's touchscreen driver.

The stock cst8xx_touch.ko reports a single finger even though the panel 
under it reports two. Two separate things cause that, and both have to
be dealt with:

  1. Setting the number of allowed touches to 1 in init_module.
     This script patches it in the compiled module.
  2. cst_max_touch_number=1 on the insmod line. 
     This script rewrites the line in cst8xx_touch.sh.

FIXME: Explain things in PATCHES.md.

    python3 tools/cst8xx_multitouch_patch.py <module_driver directory>
    python3 tools/cst8xx_multitouch_patch.py --check  <directory>
    python3 tools/cst8xx_multitouch_patch.py --revert <directory>

The directory is the one holding cst8xx_touch.ko and cst8xx_touch.sh inside an
unpacked rootfs. Both files are backed up next to themselves with a .orig
suffix before anything is written, and --revert puts them back.
"""

import argparse
import os
import re
import shutil
import struct
import sys

MODULE = "cst8xx_touch.ko"
SCRIPT = "cst8xx_touch.sh"
BACKUP_SUFFIX = ".orig"

# The three instructions that set touches to 1, as they are assembled in the
# stock module:
#
#       00  05  02  24    li         v0,0x500           puVar4[0x13] = 0x500;
#       4c  00  22  ae    sw         v0,0x4c (s1)
#       01  00  02  24    li         v0,0x1             puVar4[0x16] = 1;
#
# The first two are matched only to place the third, which is the one replaced.
# Little-endian words, as they sit in the file.

ANCHOR = struct.pack("<III", 0x24020500, 0xAE22004C, 0x24020001)
BRANCH_OFFSET = 8 # where the instruction sits inside ANCHOR
PATCHED_BRANCH = struct.pack("<I", 0x24020002)

# What a patched module looks like: the same two instructions, then the value 2
ANCHOR_PATCHED = ANCHOR[:BRANCH_OFFSET] + PATCHED_BRANCH

# Contacts the panel reports.
MAX_TOUCH = 2


def fail(message):
    sys.exit("cst8xx_multitouch_patch: " + message)


def find_once(data, pattern, what):
    """The single offset of `pattern`, or None. More than one is an error."""
    hits = []
    start = 0
    while True:
        i = data.find(pattern, start)
        if i < 0:
            break
        hits.append(i)
        start = i + 1
    if len(hits) > 1:
        fail("%s found %d times, expected once -- this is not the module this "
             "patch was written for" % (what, len(hits)))
    return hits[0] if hits else None


def module_state(path):
    """'stock', 'patched', or a reason it is neither."""
    with open(path, "rb") as f:
        data = f.read()

    if data[:4] != b"\x7fELF":
        return data, "not an ELF file"

    if find_once(data, ANCHOR, "the rate limit") is not None:
        return data, "stock"
    if find_once(data, ANCHOR_PATCHED, "the patched sequence") is not None:
        return data, "patched"
    return data, "neither the stock sequence nor a patched one is present"


def patch_module(path, revert):
    data, state = module_state(path)

    want = "patched" if revert else "stock"
    done = "stock" if revert else "patched"
    if state == done:
        print("  %s: already %s" % (MODULE, done))
        return False
    if state != want:
        fail("%s: %s" % (MODULE, state))

    src = ANCHOR_PATCHED if revert else ANCHOR
    dst = ANCHOR if revert else ANCHOR_PATCHED
    at = find_once(data, src, "the sequence")

    backup(path)
    patched = data[:at] + dst + data[at + len(dst):]
    with open(path, "wb") as f:
        f.write(patched)

    print("  %s: %s -> %s at file offset 0x%x" % (MODULE, want, done, at + BRANCH_OFFSET))
    return True


def patch_script(path, revert):
    with open(path, encoding="utf-8") as f:
        text = f.read()

    m = re.search(r"cst_max_touch_number=(\d+)", text)
    if not m:
        fail("%s: no cst_max_touch_number on the insmod line" % SCRIPT)

    current = int(m.group(1))
    wanted = 1 if revert else MAX_TOUCH
    if current == wanted:
        print("  %s: cst_max_touch_number is already %d" % (SCRIPT, wanted))
        return False

    backup(path)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text[:m.start(1)] + str(wanted) + text[m.end(1):])

    print("  %s: cst_max_touch_number %d -> %d" % (SCRIPT, current, wanted))
    return True


def backup(path):
    """Keeps the first version seen. A second run must not overwrite it."""
    dest = path + BACKUP_SUFFIX
    if not os.path.exists(dest):
        shutil.copy2(path, dest)
        print("  backup: %s" % os.path.basename(dest))


def check(module_path, script_path):
    _, state = module_state(module_path)
    print("  %s: %s" % (MODULE, state))

    with open(script_path, encoding="utf-8") as f:
        m = re.search(r"cst_max_touch_number=(\d+)", f.read())
    if not m:
        print("  %s: no cst_max_touch_number on the insmod line" % SCRIPT)
        return 1

    number = int(m.group(1))
    print("  %s: cst_max_touch_number=%d" % (SCRIPT, number))

    if state == "patched" and number >= 2:
        print("multitouch: both parts are in place")
        return 0
    print("multitouch: NOT in place -- run this script without --check")
    return 1


def main():
    ap = argparse.ArgumentParser(description="Uncap the CST8xx touch driver.")
    ap.add_argument("directory", help="the module_driver directory of an unpacked rootfs")
    group = ap.add_mutually_exclusive_group()
    group.add_argument("--check", action="store_true", help="report the state and change nothing")
    group.add_argument("--revert", action="store_true", help="put the stock behaviour back")
    args = ap.parse_args()

    module_path = os.path.join(args.directory, MODULE)
    script_path = os.path.join(args.directory, SCRIPT)
    for path in (module_path, script_path):
        if not os.path.exists(path):
            fail("%s is not there" % path)

    if args.check:
        sys.exit(check(module_path, script_path))

    changed = patch_module(module_path, args.revert)
    changed |= patch_script(script_path, args.revert)

    if not changed:
        print("nothing to do")
        return

    if args.revert:
        print("reverted. Repack the firmware for it to take effect.")
    else:
        print("patched. Repack the firmware, flash it, and check with "
              "tools/mtcircles.c: MAX TOGETHER must reach 2.")


if __name__ == "__main__":
    main()
