#!/usr/bin/env bash
# js_sem_diff -- run every case under node and under this tree's QuickJS and
# diff the two stdouts byte for byte.
#
# Two engine binaries are built from the SAME QuickJS sources, differing ONLY
# in the three defines that separate a host gate from the shipped browser:
#
#   $BUILD/js_sem_hostflags   the flag regime 27 tests/*.mk fragments use
#   $BUILD/js_sem_browflags   -DLOGIT_OS -DCONFIG_STACK_CHECK -DNDEBUG
#
# Both are host binaries; NEITHER IS THE GUEST. The guest run is a separate
# step and no finding is reported without it. Building both here is the
# cheapest way to answer "is the thing I measured the thing that ships" -- if
# the two host binaries disagree, that outranks the finding.
#
# THE CONTROL (rule 5: a control that cannot be watched failing is worse than
# no control). `--control` copies the case set, changes ONE line of ONE file,
# feeds the ALTERED copy to node and the ORIGINAL to the engine, and requires
# the harness to report a diff. A control that both sides read identically
# would prove nothing: the point is that this harness can see a difference at
# all. It is a separate flag rather than a permanent case file so that the
# ordinary run's diff contains findings and nothing else.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build-jssem}"
NODE="${NODE:-node}"
CASES="${CASES:-$ROOT/tests/fixtures/jssem-ci}"

mkdir -p "$BUILD"

QJS="$ROOT/third_party/quickjs/quickjs.c $ROOT/third_party/quickjs/cutils.c \
     $ROOT/third_party/quickjs/libregexp.c $ROOT/third_party/quickjs/libunicode.c \
     $ROOT/third_party/quickjs/libbf.c"
INC="-I$ROOT/third_party/quickjs"

build() {
    out="$1"; shift
    echo "building $(basename "$out") ..." >&2
    cc -O2 -w $INC -DCONFIG_VERSION='"logit-2024"' "$@" \
       -o "$out" "$ROOT/tests/unit/js_sem.c" $QJS -lm || return 1
}

if [ ! -x "$BUILD/js_sem_hostflags" ] || [ "$ROOT/tests/unit/js_sem.c" -nt "$BUILD/js_sem_hostflags" ]; then
    build "$BUILD/js_sem_hostflags" || exit 1
    build "$BUILD/js_sem_browflags" -DLOGIT_OS -DCONFIG_STACK_CHECK -DNDEBUG || exit 1
fi

FILES=$(ls "$CASES"/*.js | sort)
[ -n "$FILES" ] || { echo "FAIL: no case files in $CASES"; exit 1; }

if [ "${1:-}" = "--control" ]; then
    CTL="$BUILD/control"
    rm -rf "$CTL"; mkdir -p "$CTL"
    cp $FILES "$CTL/"
    # One line, one file: the labelled-continue case prints "ij 00 / ij 10".
    # Changing the inner guard makes node print a third line the engine never
    # prints. If THIS does not show up as a diff the harness is not an
    # instrument and every clean run it produced is worthless.
    sed -i.bak "s/if (j === 1) continue outer;/if (j === 2) continue outer;/" "$CTL/c10-labels-finally.js"
    rm -f "$CTL"/*.bak
    CFILES=$(ls "$CTL"/*.js | sort)
    $NODE "$ROOT/tests/unit/js_sem_node.js" $CFILES > "$BUILD/ctl-node.out" 2>/dev/null
    "$BUILD/js_sem_browflags" $FILES > "$BUILD/ctl-engine.out" 2>/dev/null
    echo "=== CONTROL: node given an ALTERED c10, engine given the original ==="
    if diff "$BUILD/ctl-node.out" "$BUILD/ctl-engine.out" > "$BUILD/ctl.diff"; then
        echo "CONTROL FAILED: the harness reported NO difference. It is not an instrument."
        exit 1
    fi
    grep -c '^[<>]' "$BUILD/ctl.diff" | sed 's/^/control diff lines: /'
    sed -n '1,20p' "$BUILD/ctl.diff"
    echo "CONTROL OK: the harness reports a difference it was given."
    exit 0
fi

$NODE "$ROOT/tests/unit/js_sem_node.js" $FILES > "$BUILD/node.out" 2> "$BUILD/node.err"
"$BUILD/js_sem_hostflags" $FILES > "$BUILD/hostflags.out" 2> "$BUILD/hostflags.err"
"$BUILD/js_sem_browflags" $FILES > "$BUILD/browflags.out" 2> "$BUILD/browflags.err"

echo "=== hostflags vs browflags (must be empty, or it is the headline) ==="
diff "$BUILD/hostflags.out" "$BUILD/browflags.out" && echo "(identical)"
echo
echo "=== node vs browser-flags QuickJS ==="
diff "$BUILD/node.out" "$BUILD/browflags.out"
echo "(diff exit $?)"
exit 0
