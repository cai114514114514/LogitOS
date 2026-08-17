#!/usr/bin/env bash
# Re-run every non-PASS target from a sweep, ALONE, and record the second answer.
#
# WHY THIS IS NOT OPTIONAL. The sweep runs host targets several at a time and
# they share one build/ directory, so two makes racing to produce the same
# object make a target fail for a reason that has nothing to do with that
# target. Measured: test-audio-codec-fuzz-deep failed in the parallel phase and
# passes alone; so did test-h265 and test-demux, both of which had been green in
# an earlier sequential sweep of the same tree.
#
# A sweep that reports those is MANUFACTURING bugs, which is worse than missing
# them -- a list with false entries is a list nobody reads, and the real failure
# in it goes unnoticed.
#
#   sweep-confirm.sh <result-file> [per-target-timeout]
#
# Rewrites <result-file>: the confirmed verdict replaces the parallel one.
# NOTARGET rows are left alone -- they are a build-system finding, not a run.
set -u

OUT="${1:?usage: sweep-confirm.sh <result-file> [timeout]}"
TMO="${2:-1200}"
LOGDIR="$(dirname "$OUT")/sweeplogs"
mkdir -p "$LOGDIR"

grep -v -E "^(PASS|NOTARGET|NEEDSARGS|AGGREGATE)	" "$OUT" | cut -f3 | sort -u > "$LOGDIR/recheck.txt"
n=$(grep -c . "$LOGDIR/recheck.txt" || true)
echo "confirm: $n target(s) to re-run alone"
[ "$n" = "0" ] && exit 0

grep -E "^(PASS|NOTARGET|NEEDSARGS|AGGREGATE)	" "$OUT" > "$OUT.keep" || true
mv "$OUT.keep" "$OUT"

while read -r t; do
    [ -n "$t" ] || continue
    s=$(date +%s)
    timeout "$TMO" make "$t" > "$LOGDIR/$t.confirm.log" 2>&1
    rc=$?
    e=$(date +%s)
    case $rc in 0) st=PASS ;; 124) st=TIMEOUT ;; *) st=FAIL ;; esac
    printf '%s\t%s\t%s\n' "$st" "$((e-s))" "$t" >> "$OUT"
    printf '%-8s %4ds  %s\n' "$st" "$((e-s))" "$t"
done < "$LOGDIR/recheck.txt"

echo "--- confirmed summary ---"
cut -f1 "$OUT" | sort | uniq -c | sort -rn
echo "--- still not passing ---"
grep -v -E "^PASS	" "$OUT" | sort | sed "s/\t/  /g"
