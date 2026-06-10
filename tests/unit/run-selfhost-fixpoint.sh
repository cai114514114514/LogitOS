#!/usr/bin/env bash
# M21-P3 S4: the self-hosting fixpoint (Ken Thompson / GCC bootstrap proof).
#   stage1: the AS compiler (asc.as), run under the C VM, compiles asc.as     -> asc1.la
#   stage2: asc1.la (the self-compiled compiler) compiles asc.as             -> asc2.la
#   stage3: asc2.la compiles asc.as                                          -> asc3.la
# A correct, deterministic compiler is a FIXED POINT of itself: asc2 == asc3
# byte-identical (and here asc1 == asc2 too: the C-built and self-built
# compilers agree on the compiler's own source). `import asc` prefers asc.la
# over asc.as, so dropping stageN's output in as asc.la makes the next stage
# run on it.
set -u
ASC="${1:?usage: run-selfhost-fixpoint.sh <asc>}"
ROOT="$PWD"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cp fsroot/as/lib/aslex.as fsroot/as/lib/asc.as tests/unit/asc_driver.as "$TMP/"
cd "$TMP"

run_compile() { "$ROOT/$ASC" asc_driver.as asc.as "$1" 2>err.txt; }

rm -f asc.la
run_compile asc1.la || { echo "FAIL stage1: $(cat err.txt)"; exit 1; }
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
[ $ok -eq 1 ] && echo "selfhost-fixpoint: PASS (asc2 == asc3, $s2 bytes)"
[ $ok -eq 1 ]
