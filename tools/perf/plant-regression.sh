#!/usr/bin/env bash
# tools/perf/plant-regression.sh -- prove the harness can detect anything.
#
# A performance harness that has never been shown to catch a regression
# measures nothing, and this repository has a live example of that failure:
# tests/qmp/qmp_freeze.py passes on broken and working builds alike, because
# its coordinates rotted and nobody ever checked that it could still fail.
#
# So this script builds a SYNTHETIC history in a scratch clone -- several
# commits, exactly one of which inserts a deliberate delay on the process-
# creation path -- and then runs `git bisect run tools/perf/bisect.sh`. The
# bisect must name the planted commit. If it names anything else, or reports
# that every commit is good, the harness is not measuring what it claims to.
#
# Nothing is written to the real tree: the plant lives only in a throwaway
# clone under /tmp, and the real repository is never checked out or modified.
#
#   bash tools/perf/plant-regression.sh              # default: the execve path
#   PLANT_MS=400 bash tools/perf/plant-regression.sh # a bigger plant
#
# Exits 0 only if the bisect named the planted commit.

set -u

TREE="${PERF_TREE:-/mnt/d/ststem}"
SCRATCH="${PERF_SCRATCH:-/tmp/perf-plant}"
BASE="${PLANT_BASE:-}"                     # commit to branch from; default HEAD
NFILL="${PLANT_FILL:-3}"                   # innocent commits either side
# WHERE THE PLANT GOES, AND WHY IT IS NOT THE READ PATH
#
# The first two attempts planted a busy wait on the VFS read path and judged it
# with read_net_ms. Both bisects named the WRONG commit, and the reason is a
# result about the metric rather than a flaw in the plant: read_net_ms measures
# opening a 2.2 MB file, which pulls the whole thing through virtio-blk while
# the driver polls with the BKL held, so its cost is set by when QEMU's IO
# thread gets scheduled on a contended host. Measured on ONE unchanged build,
# in consecutive minutes: 889 ms and 1686 ms. A 1.9x swing with no code change
# swamps any plant small enough to fit inside the phase timeouts.
#
# So the plant goes on the process-creation path, judged by shell_net_ms --
# 24 fork+execve round trips, whose spread across the whole sweep was about
# 10%. Sized against that workload: 20 ms per execve is +480 ms on a ~570 ms
# baseline, an unmistakable ~1.8x against 10% noise.
PLANT_MS="${PLANT_MS:-20}"
METRIC="${PERF_METRIC:-shell_net_ms}"
REPEAT="${PERF_REPEAT:-3}"
TOOLS="$TREE/tools/perf"

say() { echo "=== $*"; }

rm -rf "$SCRATCH"
say "cloning $TREE -> $SCRATCH (a clone, never a worktree: a worktree rewrites"
say "    line endings on this host and every .sh and .as in the tree stops working)"
git -c core.autocrlf=false clone -q --no-hardlinks "$TREE" "$SCRATCH" || exit 2
cd "$SCRATCH" || exit 2
git config user.email perf@localhost
git config user.name "perf plant"
[ -n "$BASE" ] && git checkout -q -f "$BASE"
git checkout -q -B perf-plant

TARGET="c/kernel/exec/exec.c"
[ -f "$TARGET" ] || { echo "no $TARGET in this checkout"; exit 2; }

innocent() {
    # A commit that changes something real but costs nothing: a comment. The
    # bisect must walk past these, so they have to be commits and not no-ops.
    printf '\n/* perf-plant filler %s -- no behaviour change */\n' "$1" >> "$TARGET"
    git commit -q -am "filler $1"
}

for i in $(seq 1 "$NFILL"); do innocent "pre$i"; done

# ---- the plant -------------------------------------------------------------
# A busy wait at the top of proc_execve, keyed to the guest's own monotonic
# clock so it costs the same guest time whatever the host is doing. Every
# command the shell runs pays it once, so it lands squarely on shell_net_ms
# and leaves boot and the network alone -- which is what makes this a test of
# one metric rather than of "something changed somewhere".
python3 - "$TARGET" "$PLANT_MS" <<'PY'
import sys
path, ms = sys.argv[1], int(sys.argv[2])
src = open(path, encoding="utf-8", errors="surrogateescape").read()
needle = "    struct proc *p = proc_current();\n    if (!p) return -1;\n"
assert needle in src, "anchor moved; update plant-regression.sh"
plant = (needle +
         "    /* PERF-PLANT: a deliberate regression, %d ms per execve. */\n"
         "    { extern unsigned long long time_mono_ns(void);\n"
         "      unsigned long long _e = time_mono_ns() + %dULL;\n"
         "      while (time_mono_ns() < _e) __asm__ volatile(\"pause\"); }\n"
         % (ms, ms * 1000000))
open(path, "w", encoding="utf-8", errors="surrogateescape").write(
    src.replace(needle, plant, 1))
PY
git commit -q -am "PLANT: ${PLANT_MS}ms busy wait on every execve"
PLANTED="$(git rev-parse HEAD)"
say "planted ${PLANT_MS}ms at $(git rev-parse --short HEAD)"

for i in $(seq 1 "$NFILL"); do innocent "post$i"; done
BAD="$(git rev-parse HEAD)"
GOOD="$(git rev-parse "HEAD~$((2 * NFILL + 1))")"

# ---- calibrate from the endpoints ------------------------------------------
# The decision rule is not guessed. Both ends are measured now, on this host,
# and the ratio to use is put halfway between 1.0 and the observed separation.
# A hardcoded number would rot exactly the way qmp_freeze.py's coordinates did.
#
# The good end's artifacts are KEPT, because the bisect runs in ratio mode: the
# reference is re-benchmarked at every step so the decision is a ratio, not an
# absolute. That matters and is not a refinement -- with an absolute threshold
# an earlier plant bisected to the WRONG commit, because a post-plant
# commit was measured in a quiet minute and landed 30 ms under the threshold.
REFDIR="$SCRATCH/.refbuild"
measure() {
    git checkout -q -f "$1"
    git clean -qxfd build 2>/dev/null
    make -j"$(nproc)" >/dev/null 2>&1 || { echo ""; return; }
    make -j"$(nproc)" build/disk.img >/dev/null 2>&1 || { echo ""; return; }
    if [ "${2:-}" = "keep" ]; then
        mkdir -p "$REFDIR"
        cp build/logit.iso "$REFDIR/logit.iso"
        cp build/disk.img "$REFDIR/disk.img"
    fi
    local j; j="$(mktemp)"
    python3 "$TOOLS/perfbench.py" --iso build/logit.iso --disk build/disk.img \
            --repeat "$REPEAT" --json "$j" >/dev/null 2>&1
    python3 - "$j" "$METRIC" <<'PY'
import json, sys
try: d = json.load(open(sys.argv[1]))
except Exception: sys.exit(0)
m = d.get("metrics", {}).get(sys.argv[2])
if m and m.get("n", 0): print("%.1f" % m["median"])
PY
    rm -f "$j"
}

say "measuring the good end ($(git rev-parse --short "$GOOD"))"
GV="$(measure "$GOOD" keep)"
say "measuring the bad end  ($(git rev-parse --short "$BAD"))"
BV="$(measure "$BAD")"
say "good $METRIC=$GV   bad $METRIC=$BV"

if [ -z "$GV" ] || [ -z "$BV" ]; then
    echo "FAIL: could not measure both ends"; exit 1
fi
if ! awk "BEGIN{exit !($BV > $GV * 1.25)}"; then
    echo "FAIL: the plant did not move $METRIC ($GV -> $BV)."
    echo "      Either PLANT_MS is too small to see, or -- the finding that"
    echo "      matters -- this metric does not actually observe that code path."
    exit 1
fi
SEP="$(awk "BEGIN{printf \"%.3f\", $BV/$GV}")"
RATIO="$(awk "BEGIN{printf \"%.3f\", sqrt($SEP)}")"
say "separation = ${SEP}x; deciding at ratio >= $RATIO against the good build"

# ---- the bisect ------------------------------------------------------------
git bisect start "$BAD" "$GOOD" >/dev/null 2>&1
PERF_TOOLS="$TOOLS" PERF_METRIC="$METRIC" PERF_RATIO="$RATIO" \
    PERF_REF_ISO="$REFDIR/logit.iso" PERF_REF_DISK="$REFDIR/disk.img" \
    PERF_REPEAT="$REPEAT" git bisect run "$TOOLS/bisect.sh" 2>&1 | tee /tmp/perf-plant-bisect.log
FOUND="$(git rev-parse refs/bisect/bad 2>/dev/null)"
git bisect reset >/dev/null 2>&1

echo
if [ "$FOUND" = "$PLANTED" ]; then
    echo "PASS: bisect named the planted commit ${PLANTED:0:9}"
    echo "      ($METRIC: good=$GV bad=$BV separation=${SEP}x decided at $RATIO)"
    exit 0
fi
echo "FAIL: bisect named ${FOUND:0:9}, planted was ${PLANTED:0:9}"
exit 1
