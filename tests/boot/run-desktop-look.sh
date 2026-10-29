#!/usr/bin/env bash
# THE LOOK GATE, invoked the way every other boot test in this tree is.
#
# All the real work -- booting, screendumping the guest's OWN scanout over
# QMP, and measuring pixels -- is in tests/qmp/qmp_desktop_look.py, for the
# same reason run-input-test.sh gives for the identical split: it needs QMP
# (screendump) and the serial console (the guest's `[wm] win 0 frame ...`
# report) at once, which is more than a shell pipeline drives cleanly. This
# wrapper exists only so `make test-desktop-look` looks like every other
# target in tests/*.mk.
#
# THE WALLPAPER MATTERS HERE MORE THAN IN MOST HARNESSES. fsroot/wallpaper.png
# is gitignored; a disk image built from a clean checkout falls back to a flat
# built-in gradient, and every pixel measurement in this file -- the shadow's
# proportional darkening, the dock rim mirror diff, even defect 1's row0-vs-
# row4 comparison -- assumes a photograph with real local detail behind the
# glass. Build against the working tree (or copy the file in first) or the
# numbers this prints will not be the ones recorded in qmp_desktop_look.py.

set -u
ISO="${1:?usage: run-desktop-look.sh <iso> <disk.img>}"
DISK="${2:?usage: run-desktop-look.sh <iso> <disk.img>}"
exec python3 "$(dirname "$0")/../qmp/qmp_desktop_look.py" "$ISO" "$DISK"
