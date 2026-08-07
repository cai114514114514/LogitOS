#!/usr/bin/env bash
# tools/perf/bisect.sh -- the `git bisect run` driver for a performance
# regression.
#
#   cd /tmp/lo                       # a CLONE, never a worktree (CRLF)
#   git bisect start <bad> <good>
#   PERF_METRIC=read_net_ms PERF_THRESHOLD=1400 \
#     git bisect run /mnt/d/ststem/tools/perf/bisect.sh
#
# EXIT CODES ARE THE WHOLE POINT
#   0    good  -- metric measured, and below the threshold
#   1    bad   -- metric measured, and at or above the threshold
#   125  SKIP  -- this commit could not be judged: it did not build, or it
#                 built but the phase could not run. Today's history contains
#                 commits that do not compile (three of them deleted other
#                 lines' files and HEAD stopped building each time), and a
#                 harness that scored those as "bad" would name a build break
#                 as the performance culprit. 125 is `git bisect`'s skip.
#
# The harness itself is taken from PERF_TOOLS, which must live OUTSIDE the
# tree being bisected: `git bisect` checks out historical commits, and a
# harness that changed underneath the bisect would be measuring itself.

set -u

PERF_TOOLS="${PERF_TOOLS:-/mnt/d/ststem/tools/perf}"
BENCH="$PERF_TOOLS/perfbench.py"
METRIC="${PERF_METRIC:-read_net_ms}"
THRESHOLD="${PERF_THRESHOLD:?set PERF_THRESHOLD}"
REPEAT="${PERF_REPEAT:-3}"
JOBS="${PERF_JOBS:-$(nproc)}"
BUILD_LOG="${PERF_BUILD_LOG:-/tmp/perf-bisect-build.log}"

SHA="$(git rev-parse --short HEAD 2>/dev/null || echo '?')"
say() { echo "[bisect $SHA] $*" >&2; }

[ -f "$BENCH" ] || { say "no harness at $BENCH"; exit 125; }

# --- build ----------------------------------------------------------------
git clean -qxfd build 2>/dev/null
if ! make -j"$JOBS" >"$BUILD_LOG" 2>&1; then
    say "SKIP: kernel/iso did not build"; exit 125
fi
# The disk image is a SEPARATE target and every ring-3 program lives on it.
# Skipping it would silently benchmark yesterday's userland against today's
# kernel.
if ! make -j"$JOBS" build/disk.img >>"$BUILD_LOG" 2>&1; then
    say "SKIP: disk image did not build"; exit 125
fi
[ -f build/logit.iso ] && [ -f build/disk.img ] || { say "SKIP: no artifacts"; exit 125; }

# --- measure ---------------------------------------------------------------
OUT="$(mktemp)"
trap 'rm -f "$OUT"' EXIT
python3 "$BENCH" --iso build/logit.iso --disk build/disk.img \
        --repeat "$REPEAT" --json "$OUT" --label "$SHA" >/dev/null 2>&1

VAL="$(python3 - "$OUT" "$METRIC" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    sys.exit(0)
m = d.get("metrics", {}).get(sys.argv[2])
if m and m.get("n", 0):
    print("%.1f %d" % (m["median"], m["n"]))
PY
)"

if [ -z "$VAL" ]; then
    say "SKIP: $METRIC not measurable on this build"; exit 125
fi

MED="${VAL% *}"; N="${VAL#* }"
if awk "BEGIN{exit !($MED >= $THRESHOLD)}"; then
    say "BAD:  $METRIC median=$MED (n=$N) >= $THRESHOLD"
    exit 1
fi
say "GOOD: $METRIC median=$MED (n=$N) <  $THRESHOLD"
exit 0
