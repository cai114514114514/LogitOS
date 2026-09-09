#!/usr/bin/env bash
# tools/coldcode/build.sh -- build the coverage-instrumented probes for the
# cold-code instrument, WITHOUT hand-copying a source list.
#
# CLAUDE.md rule 4 ("hand-copied source lists") is the reason this asks make
# for the link line instead of restating it: CANVAS_SRC, PROBE_SRC, H2MUX_SRC
# and MSE_INC are all copies of a TU list the tree kept growing, and every one
# of them has drifted. A cold-code report built from a stale copy of PROBE_SRC
# would call every function in the missing TU cold -- the same failure in a new
# costume, which is exactly what this instrument exists to find.
#
# So: `make -n -B` prints the real recipe, we take it verbatim, and the ONLY
# edits are (a) the coverage flags, already injected through CC, and (b) one
# extra TU, tests/unit/cold_control.c, which is the control.
#
#   tools/coldcode/build.sh <outdir>
#
# Produces <outdir>/webapi_probe and <outdir>/wpt_test, both instrumented.
# <outdir> should be OUTSIDE build/ -- CLAUDE.md's sixth rule for this line:
# "a full-corpus run died at implementation #1 because a background make
# replaced the binary mid-run."
set -euo pipefail

OUT=${1:?usage: build.sh <outdir>}
BUILDDIR=${COLD_BUILD:-build-cold}
CCCOV="clang -fprofile-instr-generate -fcoverage-mapping"
CTRL=tests/unit/cold_control.c

mkdir -p "$OUT"

# The control's premise, checked rather than assumed: nothing in the tree may
# define COLDCTL_ENABLE_IMPOSSIBLE. If somebody ever does, the cold half of the
# control silently becomes reachable and stops being a control.
if grep -rl --include='*.c' --include='*.h' --include='*.mk' --include='Makefile' \
        --include='*.sh' --include='*.py' 'COLDCTL_ENABLE_IMPOSSIBLE' . 2>/dev/null \
        | grep -v '^\./tests/unit/cold_control\.c$' | grep -v '^\./tools/coldcode/' | grep -q .; then
    echo "coldcode/build.sh: FAIL -- something outside cold_control.c mentions"
    echo "  COLDCTL_ENABLE_IMPOSSIBLE. The cold half of the control may now be"
    echo "  reachable, so it has stopped being a control."
    exit 1
fi

build_one() {
    local target=$1 name=$2
    echo "coldcode: asking make for the $name recipe"
    # -B so make prints the recipe even when the target is up to date. The
    # link line is the one naming the output; take the LAST such (the recipe
    # may echo diagnostics that mention the path).
    #
    # JOIN THE CONTINUATIONS FIRST -- CLAUDE.md rule 2, and this line paid for
    # it inside ten minutes. `make -n` prints a wrapped recipe with its
    # backslash-newlines INTACT, so a plain grep for the output path returns
    # only the FIRST physical fragment. webapi_probe's recipe is one line and
    # linked fine; wpt_test's is two, and the fragment that matched carried the
    # sources without `libcss_host.a` or the Rust archive -- 40 undefined
    # symbols in a link line nobody wrote. Exactly the shape the six make
    # parsers in tools/ all carry this same substitution for.
    local line
    line=$(make -n -B BUILD="$BUILDDIR" CC="$CCCOV" "$BUILDDIR/$target" 2>/dev/null \
           | python3 -c 'import sys,re; sys.stdout.write(re.sub(r"\\\r?\n[ \t]*", " ", sys.stdin.read()))' \
           | grep -- "-o $BUILDDIR/$target " | tail -1)
    if [ -z "$line" ]; then
        echo "coldcode/build.sh: FAIL -- make printed no link line for $target."
        exit 1
    fi
    # Same command, one extra TU (the control), a different output path.
    local newline=${line/-o $BUILDDIR\/$target/-o $OUT\/$target $CTRL}
    echo "coldcode: linking $OUT/$target"
    eval "$newline"
    # Prove the binary carries a coverage map at all. A binary built without
    # -fcoverage-mapping produces an EMPTY report, which reads as "everything
    # is cold" -- the loudest possible wrong answer.
    if ! otool -l "$OUT/$target" | grep -q __llvm_covmap; then
        echo "coldcode/build.sh: FAIL -- $OUT/$target has no __llvm_covmap section."
        exit 1
    fi
}

build_one webapi_probe "webapi_probe"
build_one wpt_test     "wpt_test"

echo "coldcode: built"
ls -l "$OUT/webapi_probe" "$OUT/wpt_test"
