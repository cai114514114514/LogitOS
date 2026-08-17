#!/usr/bin/env bash
# Run the targets a sweep has not recorded yet, then confirm its failures.
#
# A full sweep is an afternoon, and an afternoon is long enough for something to
# interrupt it -- a killed shell, a machine that needed rebooting, or (the
# reason this exists) someone editing kernel sources mid-run and breaking the
# build, which turns every remaining DEVICE target into a 900-second timeout
# that says nothing about the target.
#
# Resuming beats restarting: the results already recorded are still results, and
# re-running 440 passing targets to reach the 82 that never ran is an hour spent
# learning nothing.
#
#   sweep-resume.sh <result-file> [per-target-timeout]
set -u

OUT="${1:?usage: sweep-resume.sh <result-file> [timeout]}"
TMO="${2:-900}"
LOGDIR="$(dirname "$OUT")/sweeplogs"
mkdir -p "$LOGDIR"

cut -f3 "$OUT" | sort -u > "$LOGDIR/done.txt"
cat "$LOGDIR/host.txt" "$LOGDIR/dev.txt" 2>/dev/null | sort -u > "$LOGDIR/want.txt"
comm -23 "$LOGDIR/want.txt" "$LOGDIR/done.txt" > "$LOGDIR/todo.txt"

n=$(grep -c . "$LOGDIR/todo.txt" || true)
echo "resume: $n target(s) never ran"
[ "$n" = "0" ] || while read -r t; do
    [ -n "$t" ] || continue
    s=$(date +%s)
    timeout "$TMO" make "$t" > "$LOGDIR/$t.log" 2>&1
    rc=$?
    e=$(date +%s)
    case $rc in 0) st=PASS ;; 124) st=TIMEOUT ;; *) st=FAIL ;; esac
    printf '%s\t%s\t%s\n' "$st" "$((e-s))" "$t" >> "$OUT"
    printf '%-8s %4ds  %s\n' "$st" "$((e-s))" "$t"
done < "$LOGDIR/todo.txt"

echo "resume: now confirming every failure, alone"
bash "$(dirname "$0")/sweep-confirm.sh" "$OUT" "$TMO"
