#!/usr/bin/env bash
# M21-P3 S4: the self-hosting fixpoint (Ken Thompson / GCC bootstrap proof).
#   stage1: the AS compiler (asc.as), run under the C VM, compiles asc.as     -> asc1.la
#   stage2: asc1.la (the self-compiled compiler) compiles asc.as             -> asc2.la
#   stage3: asc2.la compiles asc.as                                          -> asc3.la
# A correct, deterministic compiler is a FIXED POINT of itself: asc2 == asc3
# byte-identical (and here asc1 == asc2 too: the C-built and self-built
# compilers agree on the compiler's own source).
#
# ISOLATION (do not remove -- this is what makes the proof mean anything):
# `import asc` prefers asc.la but SILENTLY FALLS BACK to asc.as when the .la is
# absent OR fails the magic/version check (vm.c as_import). With both files in
# the import path, bumping AS_BC_VERSION without regenerating asc.as would make
# stages 2 and 3 quietly re-compile asc.as with the same C compiler -- identical
# output, test green, nothing proved. So the compiler SOURCE lives in src/ and is
# only ever named as an explicit compile_file() argument; after stage1 there is
# no importable asc.as, and a stale/bad asc.la is a hard "cannot import module".
# The negative control at the end asserts that this guard actually fires.
set -u
ASC="${1:?usage: run-selfhost-fixpoint.sh <asc>}"
ROOT="$PWD"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/src"
cp fsroot/as/lib/asc.as "$TMP/src/"                       # compile target: explicit path only
cp fsroot/as/lib/aslex.as tests/unit/asc_driver.as "$TMP/"
cd "$TMP"

# $1 = output .la; compiles src/asc.as with whatever `import asc` resolves to.
# `as` reports errors through as_emit (fd 1), not stderr, so capture both.
run_compile() { "$ROOT/$ASC" asc_driver.as src/asc.as "$1" >err.txt 2>&1; }

# stage1: no asc.la yet, so `import asc` must load source -- provide it here only.
rm -f asc.la
cp src/asc.as asc.as
run_compile asc1.la || { echo "FAIL stage1: $(cat err.txt)"; exit 1; }
rm -f asc.as                                               # from here on: .la or bust

cp asc1.la asc.la
run_compile asc2.la || { echo "FAIL stage2: $(cat err.txt)"; exit 1; }
cp asc2.la asc.la
run_compile asc3.la || { echo "FAIL stage3: $(cat err.txt)"; exit 1; }

s1=$(wc -c < asc1.la); s2=$(wc -c < asc2.la); s3=$(wc -c < asc3.la)
echo "stage sizes: asc1=$s1 asc2=$s2 asc3=$s3 bytes"
ok=1
cmp -s asc2.la asc3.la || { echo "FAIL: asc2 != asc3 (NOT a fixpoint)"; ok=0; }
if cmp -s asc1.la asc2.la; then echo "bonus: asc1 == asc2 (C-built and self-built compilers agree)"
else echo "note: asc1 != asc2 (C-built vs self-built differ -- fixpoint asc2==asc3 is the proof)"; fi

# Negative control: corrupt the .la version byte (offset 4 = u32 AS_BC_VERSION LE)
# and assert the stage now FAILS. If this passes, the isolation above has been
# broken and every stage result is meaningless.
cp asc2.la asc.la
printf '\x63' | dd of=asc.la bs=1 seek=4 conv=notrunc status=none
if run_compile probe.la; then
    echo "FAIL: a version-mismatched asc.la still compiled -- stages silently fell back to source"
    ok=0
else
    echo "guard ok: version-mismatched asc.la is a hard failure ($(head -c 120 err.txt))"
fi

[ $ok -eq 1 ] && echo "selfhost-fixpoint: PASS (asc2 == asc3, $s2 bytes)"
[ $ok -eq 1 ]
