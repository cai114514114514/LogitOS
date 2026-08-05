#!/usr/bin/env bash
# Bytecode stability gate for the runtime-rewrite milestone.
#
# Phase 1 replaces the runtime (value representation, allocator, GC, interning,
# global/field lookup, execution context) WITHOUT touching the compiler or the
# language. So every .la the compiler produces must stay byte-identical the whole
# way through. This hashes the compiled stdlib + the self-hosted compiler and
# compares against a checked-in baseline.
#
# It fires earlier and far more precisely than the fixpoint test: it names the
# module whose bytecode moved, instead of reporting that a 37 KB binary differs.
#
# When a slice is INTENDED to change codegen (the iterator-protocol slice is the
# only planned one), re-baseline deliberately:
#     bash tests/unit/run-bcstable.sh build/asc --update
# and say so in the commit message.
set -u
ASC="${1:?usage: run-bcstable.sh <asc> [--update]}"
MODE="${2:-check}"
BASE="tests/unit/bcstable.sha256"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# macOS has shasum, Linux has sha256sum.
if command -v sha256sum >/dev/null 2>&1; then SHA() { sha256sum "$@"; }
elif command -v shasum   >/dev/null 2>&1; then SHA() { shasum -a 256 "$@"; }
else echo "run-bcstable.sh: no sha256sum/shasum available"; exit 1; fi

for src in fsroot/as/lib/*.as; do
    name="$(basename "$src" .as)"
    if ! "$ASC" -c "$src" -o "$TMP/$name.la" >"$TMP/err" 2>&1; then
        echo "FAIL: $ASC -c $src failed: $(cat "$TMP/err")"
        exit 1
    fi
done

( cd "$TMP" && SHA *.la | sort -k2 ) > "$TMP/actual.sha256"

if [ "$MODE" = "--update" ]; then
    cp "$TMP/actual.sha256" "$BASE"
    echo "bcstable: baseline updated ($(wc -l < "$BASE" | tr -d ' ') modules)"
    exit 0
fi

if [ ! -f "$BASE" ]; then
    echo "bcstable: no baseline at $BASE -- create it with: bash tests/unit/run-bcstable.sh $ASC --update"
    exit 1
fi

if diff -u "$BASE" "$TMP/actual.sha256" > "$TMP/diff" 2>&1; then
    echo "bcstable: ok ($(wc -l < "$BASE" | tr -d ' ') modules byte-identical to baseline)"
    exit 0
fi

echo "bcstable: FAIL -- compiled bytecode changed"
echo
# Name the modules that moved, rather than dumping the raw hash diff.
awk 'NR==FNR { want[$2]=$1; next } { got[$2]=$1 }
     END { for (m in want) if (m in got && want[m] != got[m]) print "  changed: " m
           for (m in want) if (!(m in got))                  print "  missing: " m
           for (m in got)  if (!(m in want))                 print "  new:     " m }' \
    "$BASE" "$TMP/actual.sha256" | sort
echo
echo "Phase 1 must not change codegen. If this slice legitimately does, re-baseline with:"
echo "    bash tests/unit/run-bcstable.sh $ASC --update"
echo "To see WHAT moved:  $ASC -c <module>.as -o /tmp/new.la && $ASC -dis /tmp/new.la > /tmp/new.dis"
exit 1
