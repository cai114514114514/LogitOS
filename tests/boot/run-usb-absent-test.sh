#!/usr/bin/env bash
# THE REGRESSION THAT WOULD TAKE EVERYONE DOWN AT ONCE.
#
# Development runs under QEMU with PS/2 input, and every QMP driver in
# tests/qmp -- qmp_ui.py, qmp_browser.py, qmp_apps.py, qmp_term.py and the rest
# -- injects PS/2 events. If adding USB displaced PS/2, raced it, or delivered
# each event twice, it would break sixteen lines of work's test infrastructure
# simultaneously and the USB tests would still pass.
#
# So this boots a machine with NO xHCI CONTROLLER AT ALL, on the same kernel,
# and requires that:
#
#   1. it boots (the USB driver must not have wedged anything on the way past --
#      it binds by PCI class, so with no such device its probe never runs);
#   2. the PS/2 keyboard still delivers keystrokes to a ring-3 application;
#   3. the PS/2 mouse still delivers motion and clicks to the same application;
#   4. NOTHING is delivered twice.
#
# Point 4 is checked by counting: the app is sent exactly three distinct
# characters and must report each exactly once. A second producer feeding the
# same queue would show up as doubles, which is the specific failure mode a
# USB stack that does not co-operate with an 8042 produces on real hardware.
#
# The mouse half of that is checked by clicking once and requiring exactly one
# press event: motion is coalesced by c/kernel/gui/evq.c on purpose and so
# cannot be counted, but a click is never merged.

set -u
ISO="${1:?usage: run-usb-absent-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-usb-absent-test.sh <iso> <disk.img>}"
exec python3 "$(dirname "$0")/../qmp/qmp_ps2_only.py" "$ISO" "$DISK"
