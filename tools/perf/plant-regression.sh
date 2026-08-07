#!/usr/bin/env bash
# tools/perf/plant-regression.sh -- prove the harness can detect anything.
#
# A performance harness that has never been shown to catch a regression
# measures nothing, and this repository has a live example of that failure:
# tests/qmp/qmp_freeze.py passes on broken and working builds alike, because
# its coordinates rotted and nobody ever checked that it could still fail.
#
# So this script builds a SYNTHETIC history in a scratch clone -- several
# commits, exactly one of which inserts a deliberate delay on the file-read
# path -- and then runs `git bisect run tools/perf/bisect.sh` over it. The
# bisect must name the planted commit. If it names anything else, or reports
# that every commit is good, the harness is not measuring what it claims to.
#
# Nothing is written to the real tree: the plant lives only in a throwaway
# clone under /tmp, and the real repository is never checked out or modified.
#
#   bash tools/perf/plant-regression.sh              # default: read path
#   PLANT_US=400 bash tools/perf/plant-regression.sh # a bigger plant
#
# Exits 0 only if the bisect named the planted commit.

set -u

TREE="${PERF_TREE:-/mnt/d/ststem}"
SCRATCH="${PERF_SCRATCH:-/tmp/perf-plant}"
BASE="${PLANT_BASE:-}"                     # commit to branch from; default HEAD
NFILL="${PLANT_FILL:-3}"                   # innocent commits either side
PLANT_US="${PLANT_US:-250}"                # microseconds burned per VFS read
METRIC="${PERF_METRIC:-read_net_ms}"
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

TARGET="c/kernel/exec/file.c"
[ -f "$TARGET" ] || { echo "no $TARGET in this checkout"; exit 2; }

innocent() {
    # A commit that changes something real but costs nothing: a comment. The
    # bisect must walk past these, so they have to be commits and not no-ops.
    printf '\n/* perf-plant filler %s -- no behaviour change */\n' "$1" >> "$TARGET"
    git commit -q -am "filler $1"
}

for i in $(seq 1 "$NFILL"); do innocent "pre$i"; done

# ---- the plant -------------------------------------------------------------
# A busy wait on the F_VFS read path, keyed to the guest's own timer so it
# costs the same wall time whatever the host is doing. Every `wc /fonts/ui.ttf`
# reads the 2.2 MB file in many chunks, so this lands squarely on read_net_ms
# and leaves boot alone -- which is what makes it a test of the metric and not
# just of "something changed".
python3 - "$TARGET" "$PLANT_US" <<'PY'
import re, sys
path, us = sys.argv[1], int(sys.argv[2])
src = open(path, encoding="utf-8", errors="surrogateescape").read()
needle = "        long n = len < avail ? len : avail;\n"
assert needle in src, "anchor moved; update plant-regression.sh"
plant = (needle +
         "        /* PERF-PLANT: a deliberate regression, %d us per read. */\n"
         "        { extern unsigned long long time_mono_ns(void);\n"
         "          unsigned long long _e = time_mono_ns() + %dULL;\n"
         "          while (time_mono_ns() < _e) __asm__ volatile(\"pause\"); }\n"
         % (us, us * 1000))
open(path, "w", encoding="utf-8", errors="surrogateescape").write(
    src.replace(needle, plant, 1))
PY
git commit -q -am "PLANT: ${PLANT_US}us busy wait on every VFS read"
PLANTED="$(git rev-parse HEAD)"
say "planted ${PLANT_US}us at $(git rev-parse --short HEAD)"

for i in $(seq 1 "$NFILL"); do innocent "post$i"; done
BAD="$(git rev-parse HEAD)"
GOOD="$(git rev-parse "HEAD~$((2 * NFILL + 1))")"

# ---- calibrate the threshold from the endpoints ----------------------------
# The threshold is not guessed. Both ends are measured now, on this host, in
# this minute, and the threshold is put halfway between them in log space. A
# hardcoded number would rot exactly the way qmp_freeze.py's coordinates did.
measure() {
    git checkout -q -f "$1"
    git clean -qxfd build 2>/dev/null
    make -j"$(nproc)" >/dev/null 2>&1 || { echo ""; return; }
    make -j"$(nproc)" build/disk.img >/dev/null 2>&1 || { echo ""; return; }
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
GV="$(measure "$GOOD")"
say "measuring the bad end  ($(git rev-parse --short "$BAD"))"
BV="$(measure "$BAD")"
say "good $METRIC=$GV   bad $METRIC=$BV"

if [ -z "$GV" ] || [ -z "$BV" ]; then
    echo "FAIL: could not measure both ends"; exit 1
fi
if ! awk "BEGIN{exit !($BV > $GV * 1.25)}"; then
    echo "FAIL: the plant did not move $METRIC ($GV -> $BV)."
    echo "      Either PLANT_US is too small to see, or -- the finding that"
    echo "      matters -- this metric does not actually observe that code path."
    exit 1
fi
THRESH="$(awk "BEGIN{printf \"%.1f\", sqrt($GV*$BV)}")"
say "threshold = geometric mean = $THRESH"

# ---- the bisect ------------------------------------------------------------
git bisect start "$BAD" "$GOOD" >/dev/null 2>&1
PERF_TOOLS="$TOOLS" PERF_METRIC="$METRIC" PERF_THRESHOLD="$THRESH" \
    PERF_REPEAT="$REPEAT" git bisect run "$TOOLS/bisect.sh" 2>&1 | tee /tmp/perf-plant-bisect.log
FOUND="$(git rev-parse refs/bisect/bad 2>/dev/null)"
git bisect reset >/dev/null 2>&1

echo
if [ "$FOUND" = "$PLANTED" ]; then
    echo "PASS: bisect named the planted commit ${PLANTED:0:9}"
    echo "      ($METRIC: good=$GV bad=$BV threshold=$THRESH)"
    exit 0
fi
echo "FAIL: bisect named ${FOUND:0:9}, planted was ${PLANTED:0:9}"
exit 1
