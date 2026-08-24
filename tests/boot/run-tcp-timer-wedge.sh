#!/usr/bin/env bash
# TCP's timers with the window manager taken away.
#
# The whole harness is tests/boot/tcp_timer_wedge.py -- read its docstring
# first; it explains why a lossless fetch cannot see this property at all and
# what the wire cut is for. This wrapper exists only so the target looks like
# every other boot gate in the tree (`bash tests/boot/run-*.sh $(ISO) $(DISK)`)
# and so a missing python3 is a sentence rather than a stack trace.
set -u

ISO="${1:?usage: run-tcp-timer-wedge.sh <iso> <disk.img>}"
DISK="${2:?usage: run-tcp-timer-wedge.sh <iso> <disk.img>}"
HERE="$(cd "$(dirname "$0")" && pwd)"

command -v python3 >/dev/null 2>&1 || {
    echo "FAIL: python3 is required to drive QMP for this gate"; exit 1; }

exec python3 "$HERE/tcp_timer_wedge.py" "$ISO" "$DISK"
