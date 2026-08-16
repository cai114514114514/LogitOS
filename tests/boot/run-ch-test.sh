#!/usr/bin/env bash
# GATE B for the chat window: the reply arrives in pieces and each piece is
# drawn, proven on the machine.
#
# The work is in tests/qmp/qmp_ch.py, which needs three things at once that a
# shell pipeline cannot drive: QMP (to inject the keystrokes and to take
# screendumps), a bidirectional serial console (to write /etc/ai.conf and to
# read the app's markers back), and its own HTTP server dribbling a canned SSE
# response. This wrapper exists so it is invoked like every other boot test.
#
# NO KEY IS INVOLVED. The mock endpoint needs none, /etc/ai.conf is written with
# an empty `key`, and the harness asserts that no Authorization header was sent.
# The one key-shaped string in the run is a canary that must NOT appear on the
# console.

set -u
ISO="${1:?usage: run-ch-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-ch-test.sh <iso> <disk.img>}"
exec python3 "$(dirname "$0")/../qmp/qmp_ch.py" --iso "$ISO" --disk "$DISK" \
     --only "${CH_ONLY:-all}"
