#!/usr/bin/env bash
# tests/boot/run-perf-test.sh -- boot the machine and print what it costs.
#
# This is a MEASUREMENT, not a gate. It exits 0 whatever the numbers say. A
# performance target that goes red on a loaded machine only teaches people to
# stop reading the build; the gate is run-perf-gate.sh, which is given an
# explicit threshold by whoever is defending a specific number.
#
#   make test-perf                 # 3 boots, median + spread
#   PERF_REPEAT=7 make test-perf   # more samples on a busy host

set -u
ISO="${1:?usage: run-perf-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-perf-test.sh <iso> <disk.img>}"
REPEAT="${PERF_REPEAT:-3}"
HERE="$(cd "$(dirname "$0")/../.." && pwd)"

exec python3 "$HERE/tools/perf/perfbench.py" \
     --iso "$ISO" --disk "$DISK" --repeat "$REPEAT" \
     ${PERF_JSON:+--json "$PERF_JSON"} \
     ${PERF_LABEL:+--label "$PERF_LABEL"}
