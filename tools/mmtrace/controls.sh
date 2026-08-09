#!/usr/bin/env bash
# The controls for tools/mmtrace/mmsim.c, asserted rather than eyeballed.
#
# A page-replacement simulator is exactly the kind of program that produces
# plausible numbers while being wrong: every policy returns some miss count,
# they are all in the same range, and nothing about the output says the offline
# optimum was computed correctly. These two inputs have answers that are known
# in advance from theory, and they fail loudly.
#
#   1. SEQUENTIAL SCAN of N+1 pages through N frames. LRU, FIFO and clock each
#      evict precisely the page that is needed next, so every reference after
#      the first loop is a miss -- a miss rate of 1.00. MIN misses about once
#      per loop. If this does not show a ~100x gap, MIN is wrong.
#
#   2. UNIFORM RANDOM with memory far smaller than the page set. There is no
#      locality to exploit, so every policy including MIN converges. If the
#      clock loses here, the harness is measuring something other than policy.
#
#   3. The clock must sit BETWEEN FIFO and LRU on a mixed hot/cold pattern.
#      That is what a second-chance clock IS, and a model of it that came out
#      identical to LRU (a common way to get this wrong -- resetting the hand
#      to the victim's frame on every eviction turns clock into something much
#      closer to LRU) would pass both controls above.
set -u
SIM="${1:?usage: controls.sh <path to mmsim>}"
fail=0
bad() { echo "FAIL: $1"; fail=1; }

val() { # val <output> <policy> <column: 4=capacity 5=vs-MIN 6=miss/ref>
    echo "$1" | awk -v p="$2" -v c="$3" '$1==p {print $c}'
}

echo "=== control 1: sequential scan of N+1 pages through N frames ==="
OUT=$($SIM --synth 'seq,loops=50' --frames 100)
echo "$OUT" | sed -n '3,12p'
for p in lru fifo clock; do
    MR=$(val "$OUT" "$p" 6)
    awk -v v="$MR" 'BEGIN{exit !(v > 0.999)}' \
        || bad "$p should miss on essentially every reference of a sequential scan, got miss/ref=$MR"
done
OPTMR=$(val "$OUT" opt 6)
awk -v v="$OPTMR" 'BEGIN{exit !(v < 0.05)}' \
    || bad "MIN should miss about once per loop on a sequential scan, got miss/ref=$OPTMR (MIN IS WRONG -- no other number from this program means anything)"
RATIO=$(val "$OUT" clock 5)
echo "  clock is ${RATIO} MIN on the scan"
awk -v v="${RATIO%x}" 'BEGIN{exit !(v > 50)}' \
    || bad "the clock-to-MIN gap on a sequential scan should be enormous, got $RATIO"

echo
echo "=== control 2: uniform random, memory far smaller than the page set ==="
OUT=$($SIM --synth 'rand,pages=2000,refs=400000' --frames 10)
echo "$OUT" | sed -n '3,12p'
for p in lru fifo clock rand; do
    R=$(val "$OUT" "$p" 5); R=${R%x}
    awk -v v="$R" 'BEGIN{exit !(v < 1.25)}' \
        || bad "$p should converge to MIN on unstructured access, got ${R}x"
done

echo
echo "=== control 3: the clock is a clock, not LRU and not FIFO ==="
OUT=$($SIM --synth 'hotcold,pages=1000,hot=50,refs=400000' --frames 100)
echo "$OUT" | sed -n '3,12p'
L=$(val "$OUT" lru 4); F=$(val "$OUT" fifo 4); C=$(val "$OUT" clock 4)
echo "  capacity misses: lru=$L clock=$C fifo=$F"
[ "$C" -gt "$L" ] || bad "the clock should not beat true LRU on a hot/cold pattern (clock=$C lru=$L)"
[ "$C" -lt "$F" ] || bad "the clock should beat FIFO on a hot/cold pattern -- the second chance is what it is for (clock=$C fifo=$F)"

echo
[ "$fail" -eq 0 ] && { echo "PASS: the simulator reproduces all three known answers"; exit 0; }
echo "the simulator is WRONG; do not use its numbers"
exit 1
