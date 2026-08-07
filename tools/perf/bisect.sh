#!/usr/bin/env bash
# tools/perf/bisect.sh -- the `git bisect run` driver for a performance
# regression.
#
#   cd /tmp/lo                       # a CLONE, never a worktree (CRLF)
#   git bisect start <bad> <good>
#   PERF_METRIC=read_net_ms PERF_THRESHOLD=1400 \
#     git bisect run /mnt/d/ststem/tools/perf/bisect.sh
#
# TWO WAYS TO DECIDE, AND THE SECOND IS THE ONE TO USE
#
#   absolute:  PERF_THRESHOLD=1400
#              Simple, and fragile. A bisect walks for an hour; this host's
#              load moves a metric by 2.3x inside that hour. Measured: a
#              planted 1.5x regression was bisected to the WRONG commit,
#              because a commit after the plant happened to be measured in a
#              quiet minute and came in at 2529 against a 2559 threshold.
#
#   relative:  PERF_REF_ISO=<iso> PERF_REF_DISK=<disk> PERF_RATIO=1.30
#              The reference build is benchmarked at EVERY step, immediately
#              before the candidate, and the decision is on the ratio. The
#              host's mood in that minute divides out. It costs one extra boot
#              per step -- about ten seconds -- and it is the difference
#              between a bisect that answers and a bisect that guesses.
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
REF_ISO="${PERF_REF_ISO:-}"
REF_DISK="${PERF_REF_DISK:-}"
RATIO="${PERF_RATIO:-}"
THRESHOLD="${PERF_THRESHOLD:-}"
if [ -z "$RATIO" ] && [ -z "$THRESHOLD" ]; then
    echo "set PERF_THRESHOLD, or PERF_RATIO with PERF_REF_ISO/PERF_REF_DISK" >&2
    exit 125
fi
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
OUT="$(mktemp)"; REFOUT="$(mktemp)"
trap 'rm -f "$OUT" "$REFOUT"' EXIT

read_metric() {
    python3 - "$1" "$METRIC" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    sys.exit(0)
m = d.get("metrics", {}).get(sys.argv[2])
if m and m.get("n", 0):
    print("%.1f %d" % (m["median"], m["n"]))
PY
}

# The reference goes FIRST and in the same minute, so that whatever the host is
# doing to the candidate it is also doing to the denominator.
REFMED=""
if [ -n "$RATIO" ] && [ -n "$REF_ISO" ] && [ -n "$REF_DISK" ]; then
    python3 "$BENCH" --iso "$REF_ISO" --disk "$REF_DISK" \
            --repeat "$REPEAT" --json "$REFOUT" --label ref >/dev/null 2>&1
    RV="$(read_metric "$REFOUT")"
    [ -z "$RV" ] && { say "SKIP: reference build did not yield $METRIC"; exit 125; }
    REFMED="${RV% *}"
fi

python3 "$BENCH" --iso build/logit.iso --disk build/disk.img \
        --repeat "$REPEAT" --json "$OUT" --label "$SHA" >/dev/null 2>&1
VAL="$(read_metric "$OUT")"
if [ -z "$VAL" ]; then
    say "SKIP: $METRIC not measurable on this build"; exit 125
fi
MED="${VAL% *}"; N="${VAL#* }"

if [ -n "$REFMED" ]; then
    R="$(awk "BEGIN{printf \"%.3f\", $MED/$REFMED}")"
    if awk "BEGIN{exit !($R >= $RATIO)}"; then
        say "BAD:  $METRIC=$MED ref=$REFMED ratio=$R (n=$N) >= $RATIO"
        exit 1
    fi
    say "GOOD: $METRIC=$MED ref=$REFMED ratio=$R (n=$N) <  $RATIO"
    exit 0
fi

if awk "BEGIN{exit !($MED >= $THRESHOLD)}"; then
    say "BAD:  $METRIC median=$MED (n=$N) >= $THRESHOLD"
    exit 1
fi
say "GOOD: $METRIC median=$MED (n=$N) <  $THRESHOLD"
exit 0
