#!/usr/bin/env bash
# Boot LogitOS and drive REAL input at it: check that an app receives the new
# event ABI (mouse up / move / wheel / button id / modifier flags) correctly, and
# that a flood of pointer motion does not overflow the window manager's 256-entry
# event ring.
#
# The work is in tests/qmp/qmp_input.py -- it needs QMP (to inject input) and a
# bidirectional serial console (to read the guest's own counters back) at the
# same time, which is more than a shell pipeline can drive. This wrapper exists
# so the harness is invoked like every other boot test.
#
# The deterministic half of the ring proof is tests/unit/evq_test.c, which floods
# the same code with 100k samples on the host. This is the other half: real PS/2
# packets, real window routing, a live app draining.

set -u
ISO="${1:?usage: run-input-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-input-test.sh <iso> <disk.img>}"
exec python3 "$(dirname "$0")/../qmp/qmp_input.py" "$ISO" "$DISK"
