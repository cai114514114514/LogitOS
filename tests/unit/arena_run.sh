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

# Zero-fill bytes of an object file, printed only when the object was understood.
# ELF names the section .bss and GNU `size -A` lists it. Mach-O, the macOS dev
# host's format, calls it __DATA,__bss (plus __common for tentative
# definitions), and Apple's size lists sections only with -m. Measured on
# 2026-10-26 with Apple size: malloc.o's __bss is 288 B real and 268435752 B
# under -DARENA_NO_MMAP, so the Mach-O reading separates the builds as well as
# the ELF one does.
bss_bytes() {
    if out=$(size -A "$1" 2>/dev/null); then
        printf '%s\n' "$out" | awk '$1==".bss"{s+=$2} END{print s+0}'
    elif out=$(size -m "$1" 2>/dev/null); then
        printf '%s\n' "$out" | awk '/^[[:space:]]*Section \(/{seen=1}
            /Section \(__DATA, __(bss|common)\):/{sub(/.*\): */, ""); s+=$1}
            END{if (seen) print s+0}'
    fi
}

# $1 = tag, $2 = extra -D flags, $3 = "pass" or "fail"
run_case() {
    tag="$1"; extra="$2"; want="$3"
    obj="$BUILD/arena_malloc_$tag.o"
    bin="$BUILD/arena_mem_$tag"

    $CC -O2 -g $WARN -DARENA_SIZE=${ASIZE}u -DARENA_COMMIT=${ACOMMIT}u $extra \
        -c c/apps/libc/src/malloc.c -o "$obj"

    # .bss of the object, from the object. This used to be `size -A` with
    # `[ -n "$bss" ] || bss=0` behind it. Apple's size has no -A, so on macOS
    # the measurement failed, the fallback said 0 -- exactly the passing value
    # -- and the .bss assertion could not fail on the host this tree is
    # developed on. A size nobody could read is an error now, never a 0.
    bss=$(bss_bytes "$obj")
    if [ -z "$bss" ]; then
        echo "arena_run: cannot read the .bss of $obj with GNU 'size -A' or Apple 'size -m'; refusing to assume 0" >&2
        exit 1
    fi

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
