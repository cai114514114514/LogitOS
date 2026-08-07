#!/usr/bin/env bash
# Is the desktop still responsive while the network is busy? That is the claim
# the whole non-blocking socket change rests on, so it is measured rather than
# asserted: real clicks are injected over QMP during a deliberately slow
# four-connection transfer, and the time each takes to reach a ring-3 app is
# recorded. The same clicks are then injected during the OLD blocking
# SYS_HTTP_GET, because a measurement that cannot see the frozen case says
# nothing about the unfrozen one.
#
# The work is in tests/qmp/qmp_sock_ui.py -- it needs QMP (to inject input), a
# bidirectional serial console (to drive /bin/sh) and its own HTTP server (to
# dribble the body) at the same time, which is more than a shell pipeline can
# drive. This wrapper exists so it is invoked like every other boot test.

set -u
ISO="${1:?usage: run-sock-ui-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-sock-ui-test.sh <iso> <disk.img>}"
exec python3 "$(dirname "$0")/../qmp/qmp_sock_ui.py" "$ISO" "$DISK"
