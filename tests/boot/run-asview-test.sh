#!/usr/bin/env bash
# UNIT IV's gate: an image viewer WRITTEN IN AETHERSCRIPT, running on the
# device, proved by the pixels it put on the screen.
#
# Two invocations of one QMP driver, and the pair is the assertion:
#
#   POSITIVE   tests/qmp/qmp_asview.py --only fit
#              /usr/as/examples/asview.as opens /media/dot.png, scales it to
#              the window, and the screendump holds EXACTLY the number of
#              pixels of RGB(255,0,229) that the fit rectangle covers -- a
#              number this harness's Python computes itself, from the same
#              integer arithmetic image.fit() uses, rather than reading it out
#              of the program's own output. Then 'a' is pressed and the count
#              becomes exactly 60x40, which is the interactivity claim: the
#              image is STILL, so the frame cannot change unless the app
#              changed it. Then the same viewer is pointed at /media/img, a
#              directory with six fixtures in five formats, and 'n' has to step
#              to a different one and decode it -- so the gate covers a decoder
#              that is not PNG and a directory listing, neither of which
#              /media/dot.png alone can exercise.
#
#   CONTROL    tests/qmp/qmp_asview.py --only refuse,scope
#              The same viewer, handed /media/sample.h264 -- a real 80 KB file
#              that is on the disk and is not an image -- and then handed a
#              real image by a process whose capability was narrowed away from
#              it. Both must produce a WINDOW WITH A SENTENCE ON IT naming the
#              path and the reason, and zero pixels of the picture colour.
#
# WHY THE CONTROL IS RUN AS A NORMAL PASSING CASE AND NOT AN INVERTED ONE.
# The usual shape in this tree -- run a sabotaged build and require the gate to
# FAIL -- does not fit here, because the property under test is not "the
# viewer draws" but "the viewer says why when it cannot". A refusal that failed
# the gate would be exactly backwards. What makes it a control rather than
# another happy path is the pair of measurements it makes on the SAME frame:
# zero pixels of the picture colour (so nothing was drawn) AND more than twenty
# distinct colours inside the picture box (so it is not an empty window). A
# blank window passes neither the second nor, therefore, the gate -- which is
# the bug this whole unit exists to make impossible.
#
# WATCHED FAILING. Before the message existed at all, `--only refuse` reported
#   FAIL the refusal gives the REASON, with the bytes it looked at
#   FAIL the window is NOT BLANK -- the message box holds 1 distinct colours
# which is the reading a blank window produces, and is why the threshold is a
# count of colours and not a brightness.
#
# Usage: run-asview-test.sh <iso> <disk.img> [fit|refuse|all]

set -u

ISO="${1:?usage: run-asview-test.sh <iso> <disk.img> [fit|refuse|all]}"
DISK="${2:?usage: run-asview-test.sh <iso> <disk.img> [fit|refuse|all]}"
WHICH="${3:-all}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${ASVIEW_OUT:-$ROOT/build/asview-shots}"
DRIVER="$ROOT/tests/qmp/qmp_asview.py"

run() {   # run <label> <case-list> <outdir-suffix>
    echo "=== asview: $1 ==="
    python3 "$DRIVER" --iso "$ISO" --disk "$DISK" --only "$2" --out "$OUT$3"
}

rc=0
case "$WHICH" in
  fit)    run "the picture, the key, and the directory walk" fit,next "" || rc=1 ;;
  refuse) run "the refusals (negative control)" refuse,scope "-neg" || rc=1 ;;
  all)
    run "the picture, the key, and the directory walk" fit,next "" || rc=1
    run "the refusals (negative control)" refuse,scope "-neg" || rc=1
    ;;
  *) echo "unknown case '$WHICH' (fit|refuse|all)"; exit 2 ;;
esac

if [ "$rc" -ne 0 ]; then
    echo "FAIL: asview gate"
    exit 1
fi
echo "PASS: an AetherScript application drew a picture, answered a key, and"
echo "      named what it could not decode -- screendumps under $OUT"
