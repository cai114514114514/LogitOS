#!/usr/bin/env bash
# tests/boot/run-perf-gate.sh -- the gate form of run-perf-test.sh.
#
# Fails if a named metric's median is at or above a threshold. This is what a
# `git bisect run` reduces to, and what CI would use to defend a number that
# somebody has decided is worth defending.
#
#   PERF_METRIC=read_net_ms PERF_THRESHOLD=1400 make test-perf-gate
#
# A build whose phase cannot run at all exits 125, not 1: "could not be
# measured" is not "too slow", and this exit code is also `git bisect`'s skip.

set -u
ISO="${1:?usage: run-perf-gate.sh <iso> <disk.img>}"
DISK="${2:?usage: run-perf-gate.sh <iso> <disk.img>}"
METRIC="${PERF_METRIC:?set PERF_METRIC}"
THRESHOLD="${PERF_THRESHOLD:?set PERF_THRESHOLD}"
REPEAT="${PERF_REPEAT:-3}"
HERE="$(cd "$(dirname "$0")/../.." && pwd)"

OUT="$(mktemp)"
trap 'rm -f "$OUT"' EXIT

python3 "$HERE/tools/perf/perfbench.py" --iso "$ISO" --disk "$DISK" \
        --repeat "$REPEAT" --json "$OUT" --label "gate" || true

read -r MED N <<EOF
$(python3 - "$OUT" "$METRIC" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    sys.exit(0)
m = d.get("metrics", {}).get(sys.argv[2])
if m and m.get("n", 0):
    print("%.1f %d" % (m["median"], m["n"]))
PY
)
EOF

if [ -z "${MED:-}" ]; then
    echo "SKIP: $METRIC was not measurable on this build"
    exit 125
fi

if awk "BEGIN{exit !($MED >= $THRESHOLD)}"; then
    echo "FAIL: $METRIC median=$MED (n=$N) >= threshold $THRESHOLD"
    exit 1
fi
echo "PASS: $METRIC median=$MED (n=$N) < threshold $THRESHOLD"
exit 0
