#!/usr/bin/env bash
# NEGATIVE CONTROL for run-usb-test.sh.
#
# A test that still passes when you take away the thing it is testing is
# measuring something else. This runs the same machine as run-usb-test.sh --
# same kernel, same xHCI controller, same absent PS/2 controller -- with the
# usb-kbd and usb-mouse UNPLUGGED, and requires that:
#
#   1. the controller still comes up and enumerates ZERO devices, and
#   2. no input reaches the application.
#
# qmp_usb.py exits 2 in that case, which is what this script demands. If it
# exits 0, the positive test's assertions are satisfiable without any USB device
# present and prove nothing; if it exits 1, the control failed for some other
# reason and is itself broken.

set -u
ISO="${1:?usage: run-usb-negctl.sh <iso> <disk.img>}"
DISK="${2:?usage: run-usb-negctl.sh <iso> <disk.img>}"

python3 "$(dirname "$0")/../qmp/qmp_usb.py" "$ISO" "$DISK" --no-devices
rc=$?

case "$rc" in
  2) echo "PASS: with the USB devices unplugged the app received no input, so the"
     echo "      positive test in run-usb-test.sh is measuring USB and not something else"
     exit 0 ;;
  0) echo "FAIL: the harness PASSED with no USB device attached -- run-usb-test.sh"
     echo "      is not testing what it claims to test"
     exit 1 ;;
  *) echo "FAIL: the negative control did not reach its verdict (exit $rc);"
     echo "      the control itself is broken, so it proves nothing either way"
     exit 1 ;;
esac
