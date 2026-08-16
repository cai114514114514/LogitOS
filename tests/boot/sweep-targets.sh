#!/usr/bin/env bash
# Run every test target in the tree and say what each one did.
#
# WHY IT IS NOT JUST `make -k every-target`: the targets are not equal. Most are
# host programs that finish in seconds; a few dozen boot QEMU and take minutes,
# and TWO QEMUs on this host contend badly enough to turn a pass into a timeout
# -- a measured effect, and the reason a naive parallel sweep reports failures
# that are not there. So this splits them:
#
#   host targets    run in parallel, JOBS at a time
#   device targets  (their recipe mentions qemu) run one at a time, after
#
# Classification is by reading the recipe with `make -n`, not by a name list
# that would rot the first time somebody adds a boot test called test-foo.
#
#   sweep-targets.sh <result-file> [jobs] [per-target-timeout]
#
# Result lines are "STATUS<TAB>seconds<TAB>target". NOTARGET is kept distinct
# from FAIL: a target that does not exist is a build-system finding, and
# summing it with a broken test hides both.
set -u

OUT="${1:?usage: sweep-targets.sh <result-file> [jobs] [timeout]}"
JOBS="${2:-6}"
TMO="${3:-1200}"
LOGDIR="$(dirname "$OUT")/sweeplogs"
mkdir -p "$LOGDIR"
: > "$OUT"

echo "sweep: enumerating targets"
make -pRrq 2>/dev/null | grep -oE '^test-[a-z0-9-]+' | sort -u > "$LOGDIR/all.txt"
total=$(grep -c . "$LOGDIR/all.txt")
echo "sweep: $total targets"

# --- classify, in ONE make run (see sweep-classify.py for why not 524) ------
python3 tests/boot/sweep-classify.py "$LOGDIR" || exit 1
: > "$LOGDIR/none.txt"

run_one() {
    t="$1"
    s=$(date +%s)
    timeout "$TMO" make "$t" > "$LOGDIR/$t.log" 2>&1
    rc=$?
    e=$(date +%s)
    case $rc in 0) st=PASS ;; 124) st=TIMEOUT ;; *) st=FAIL ;; esac
    printf '%s\t%s\t%s\n' "$st" "$((e-s))" "$t" >> "$OUT"
    printf '%-8s %4ds  %s\n' "$st" "$((e-s))" "$t"
}

# --- host targets, JOBS at a time ------------------------------------------
# Serialised through a fifo rather than `xargs -P`: every worker shares one
# build/ directory, and two makes racing to build the same object is a
# corruption, not a speedup. One make per target is fine; the shared artifacts
# are built once by the first and reused, which is what -j on the OUTER loop
# would break.
echo "sweep: host targets ($JOBS at a time)"
n=0
while read -r t; do
    [ -n "$t" ] || continue
    run_one "$t" &
    n=$((n+1))
    if [ $((n % JOBS)) -eq 0 ]; then wait; fi
done < "$LOGDIR/host.txt"
wait

# --- device targets, one at a time -----------------------------------------
echo "sweep: device targets (sequential -- two QEMUs contend)"
while read -r t; do
    [ -n "$t" ] || continue
    run_one "$t"
done < "$LOGDIR/dev.txt"

# --- CONFIRM every failure serially -----------------------------------------
# Six makes share one build/ directory, so two of them racing to produce the
# same object makes a target fail for a reason that has nothing to do with the
# target. Measured: test-audio-codec-fuzz-deep failed in the parallel phase and
# passes alone. So no failure from the parallel phase is believed until it has
# been reproduced with nothing else running -- otherwise this sweep manufactures
# bugs, which is worse than missing them.
echo "--- confirming failures serially ---"
grep -v -E "^(PASS|NOTARGET)	" "$OUT" | cut -f3 > "$LOGDIR/recheck.txt"
if [ -s "$LOGDIR/recheck.txt" ]; then
    grep -v -f "$LOGDIR/recheck.txt" "$OUT" > "$OUT.keep" 2>/dev/null || cp "$OUT" "$OUT.keep"
    mv "$OUT.keep" "$OUT"
    while read -r t; do
        [ -n "$t" ] && run_one "$t"
    done < "$LOGDIR/recheck.txt"
fi

echo "--- summary ---"
cut -f1 "$OUT" | sort | uniq -c | sort -rn
echo "--- not passing ---"
grep -v -E "^PASS	" "$OUT" | sort | sed "s/	/  /g"
