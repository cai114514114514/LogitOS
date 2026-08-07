#!/usr/bin/env bash
# Boot LogitOS with a USB keyboard and mouse behind an xHCI controller AND NO
# PS/2 CONTROLLER AT ALL, and require that real input reaches a ring-3 app.
#
# The work is in tests/qmp/qmp_usb.py -- it needs QMP (to inject input) and a
# bidirectional serial console (to launch the app and read its output) at the
# same time, which is more than a shell pipeline can drive. This wrapper exists
# so the harness is invoked like every other boot test.
#
# See the header of qmp_usb.py for why `-machine pc,i8042=off` is the thing that
# makes this evidence rather than a coincidence.

set -u
ISO="${1:?usage: run-usb-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-usb-test.sh <iso> <disk.img>}"
exec python3 "$(dirname "$0")/../qmp/qmp_usb.py" "$ISO" "$DISK"
