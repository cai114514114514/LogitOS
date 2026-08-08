#!/usr/bin/env bash
# Boot LogitOS, open the aui Gallery, and assert against the pixels it draws.
#
# The work is in tests/qmp/qmp_gallery.py -- it needs QMP (to move a real
# pointer and press real keys) and a serial console (to read the guest's own
# frame-cost report) at the same time, which is more than a shell pipeline can
# drive. This wrapper exists so the harness is invoked like every other boot
# test, and so the negative control (make test-aui-negctl) can point the same
# script at a disk carrying a deliberately hard-edged build.
set -u
ISO="${1:?usage: run-aui-test.sh <iso> <disk.img> [extra args]}"
DISK="${2:?usage: run-aui-test.sh <iso> <disk.img> [extra args]}"
shift 2
exec python3 "$(dirname "$0")/../qmp/qmp_gallery.py" --iso "$ISO" --disk "$DISK" "$@"
