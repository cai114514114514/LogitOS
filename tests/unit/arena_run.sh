#!/bin/sh
# arena_run.sh: the positive run plus the two negative controls.
#
# The property under test is a SECTION SIZE, so it has to be measured from a
# compiled object rather than asserted inside the program: malloc.o is built
# first, its .bss is read with `size`, and that number is handed to the test on
# the command line. The test cannot compute it about itself.
#
# Both negative controls are compiled builds with one half of the change taken
# out, and both are REQUIRED to fail. If either passes, the positive run proved
# nothing.
set -e
BUILD="${1:-build}"
CC="${CC:-clang}"
mkdir -p "$BUILD"

# 256 MiB reserved, 32 MiB of it allowed to be occupied. The gap is the point:
# it is what a reservation being address space rather than memory buys, and it
# keeps the bound test to a few seconds.
ASIZE=268435456
ACOMMIT=33554432

WARN="-Wall -Wextra -Wno-unused-parameter"

# $1 = tag, $2 = extra -D flags, $3 = "pass" or "fail"
run_case() {
    tag="$1"; extra="$2"; want="$3"
    obj="$BUILD/arena_malloc_$tag.o"
    bin="$BUILD/arena_mem_$tag"

    $CC -O2 -g $WARN -DARENA_SIZE=${ASIZE}u -DARENA_COMMIT=${ACOMMIT}u $extra \
        -c c/apps/libc/src/malloc.c -o "$obj"

    # .bss of the object, from the object. `size -A` names the section.
    bss=$(size -A "$obj" | awk '$1==".bss"{print $2}')
    [ -n "$bss" ] || bss=0

    $CC -O2 -g $WARN -DARENA_BSS_BYTES=${bss}ull -DARENA_BUILD_SIZE=${ASIZE}ull \
        -o "$bin" tests/unit/arena_mem_test.c "$obj"

    echo "=============================================================="
    echo "arena_run: $tag (expect $want)   malloc.o .bss = $bss B"
    echo "=============================================================="
    if "$bin"; then got=pass; else got=fail; fi

    if [ "$got" != "$want" ]; then
        echo "arena_run: $tag was expected to $want but did $got" >&2
        exit 1
    fi
    echo "arena_run: $tag -> $got (as required)"
    echo
}

run_case real       ""                 pass
run_case no_mmap    "-DARENA_NO_MMAP"  fail
run_case no_bound   "-DARENA_NO_BOUND" fail

echo "arena_run: ALL PASS (1 positive, 2 negative controls confirmed failing)"
