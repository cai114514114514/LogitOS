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

# Targets that need an argument are recorded, not run -- see NEEDS_ARGS in
# sweep-classify.py. They are in the result file so the sweep still accounts for
# every enumerated target, and out of the FAIL list so it stays worth reading.
while IFS=$(printf '\t') read -r t why; do
    [ -n "$t" ] || continue
    printf 'NEEDSARGS\t0\t%s\n' "$t" >> "$OUT"
    printf '%-9s       %s  (%s)\n' "NEEDSARGS" "$t" "$why"
done < "$LOGDIR/args.txt"

# Aggregates are recorded, not run -- see aggregate() in sweep-classify.py. Every
# member is in this sweep already, with its own log and its own verdict, so
# running the aggregate costs the sum of its parts a second time and adds no
# coverage. Three of the four timeouts in the first full sweep were these:
# test-fs alone expands to fifteen targets and was killed at 900 s in the middle
# of test-fscrash's third crash round.
while IFS=$(printf '\t') read -r t members; do
    [ -n "$t" ] || continue
    printf 'AGGREGATE\t0\t%s\n' "$t" >> "$OUT"
    printf '%-9s       %s  (= %s)\n' "AGGREGATE" "$t" "$members"
done < "$LOGDIR/agg.txt"

run_one() {
    t="$1"
    # A NAME THAT IS NOT A TARGET, CAUGHT WHERE IT IS CREATED.
    #
    # One row in a 451-target sweep read `st-h264-pts` -- a target that does not
    # exist, with a real one (`test-h264-pts`) missing its result. It cost 28
    # seconds proving make cannot build a name nobody wrote, and the honest
    # record is that the mechanism was never identified: the enumeration at the
    # top of this file anchors on `^test-`, so it cannot emit that string, and
    # the append is a single short printf to an O_APPEND fd.
    #
    # Not knowing HOW is not a reason to leave it undetectable. A phantom row is
    # silent twice over -- it reports a failure that is not one, and it hides a
    # target that never ran -- and the second half is the expensive one, because
    # a sweep's whole value is the claim that it covered everything. So the name
    # is checked against the enumerated list at the moment it is used, and a
    # miss is loud. If it never fires, it cost one grep per target.
    if ! grep -qxF -- "$t" "$LOGDIR/all.txt"; then
        printf 'sweep: BUG: refusing to run "%s" -- not an enumerated target\n' "$t" >&2
        printf 'PHANTOM\t0\t%s\n' "$t" >> "$OUT"
        return
    fi
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
if [ "${SWEEP_HOST_ONLY:-0}" = "1" ]; then
    echo "sweep: skipping $(grep -c . "$LOGDIR/dev.txt") device targets (SWEEP_HOST_ONLY=1)"
    : > "$LOGDIR/dev.txt"
fi
echo "sweep: device targets (sequential -- two QEMUs contend)"
while read -r t; do
    [ -n "$t" ] || continue
    run_one "$t"
done < "$LOGDIR/dev.txt"

# --- CONFIRM every failure serially -----------------------------------------
# The confirmation pass lives in ONE place, and this is not it.
#
# It used to be duplicated here, and the copy carried a bug the original does
# not: it dropped the already-recorded rows with
#
#     grep -v -f "$LOGDIR/recheck.txt" "$OUT"
#
# where recheck.txt holds TARGET NAMES and -f reads them as UNANCHORED REGEXES
# matched against whole result lines. So one failing `test-fs` deletes the PASS
# rows of test-fsck, test-fs-crash, test-fsmount, test-fs-format, test-fs-host,
# test-fs-journal and test-fsreplay -- and since those names are not in
# recheck.txt, nothing re-runs them. Seven targets leave the sweep silently, and
# the summary still says everything was covered.
#
# sweep-confirm.sh gets this right by filtering POSITIVELY on the status field
# (`grep -E "^(PASS|NOTARGET)\t"`), which cannot confuse a name with a pattern.
# Two implementations of one step is how they drift; the second one is deleted
# rather than repaired.
echo "--- confirming failures serially ---"
bash "$(dirname "$0")/sweep-confirm.sh" "$OUT" "$TMO"

echo "--- summary ---"
cut -f1 "$OUT" | sort | uniq -c | sort -rn
echo "--- not passing ---"
grep -v -E "^PASS	" "$OUT" | sort | sed "s/	/  /g"
