#!/usr/bin/env bash
# Host unit tests for the M27 blocking core, plus the negative control.
#
# Two builds of the same test against the same wait.c:
#   1. the normal build must pass;
#   2. the -DWAIT_NEGCTRL build, which inverts one line of the harness's park so
#      the caller's lock is released BEFORE the thread becomes parked, must FAIL.
#      A suite that passes both ways is measuring nothing, so this script treats
#      "the negative control passed" as an error.
#
# Run: bash tests/unit/run-wait-tests.sh   (or `make test-wait`)
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
CC="${CC:-cc}"
mkdir -p "$BUILD"

INC="-I$ROOT/tests/unit/waitstub -I$ROOT/c/kernel/core"
SRC="$ROOT/tests/unit/wait_test.c $ROOT/c/kernel/core/wait.c"
FLAGS="-O1 -g -Wall -Wextra -pthread -fsanitize=thread"

# ThreadSanitizer is the point of the host build: this code is only interesting
# under concurrency, and a data race in the queue is exactly the bug class.
# Fall back to a plain build if the toolchain has no TSan.
PROBE="$BUILD/.tsan_probe"
printf 'int main(void){return 0;}
' > "$PROBE.c"
if ! $CC $FLAGS -o "$PROBE" "$PROBE.c" 2>/dev/null; then
    echo "note: no ThreadSanitizer available, building without it"
    FLAGS="-O1 -g -Wall -Wextra -pthread"
fi
rm -f "$PROBE" "$PROBE.c"

echo "== wait_test (positive) =="
$CC $FLAGS $INC -o "$BUILD/wait_test" $SRC || exit 1
"$BUILD/wait_test" || { echo "FAIL: wait_test"; exit 1; }

echo
echo "== wait_test (negative control: park releases the lock before parking) =="
$CC $FLAGS -DWAIT_NEGCTRL $INC -o "$BUILD/wait_test_negctrl" $SRC || exit 1
if "$BUILD/wait_test_negctrl"; then
    :
else
    echo "FAIL: the negative control build did not report its own failure"
    exit 1
fi

echo
echo "PASS: wait_test green, and the negative control fails as required"
