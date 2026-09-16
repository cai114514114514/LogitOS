#!/usr/bin/env bash
# S1 gate: compile the real token-dump tool as A3, then compare its output with
# the independent C frontend over the entire shipped library/example corpus.
# Previously this copied compat2/aslex.lacache and executed the A2 VM. That
# fallback is gone here; a native build failure must fail the tool's gate.
# test-as-lexer-lib additionally compares complete token bytes under ASan.
set -eu
INPUT="${1:?usage: run-selfhost-lex.sh <asc>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ASC="$(cd "$(dirname "$INPUT")" && pwd)/$(basename "$INPUT")"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
pass=0
fail=0
for mode in debug release; do
    options=()
    if [ "$mode" = debug ]; then options=(--debug); fi
    # Empty arrays need this expansion on macOS's Bash 3.2 with set -u.
    "$ASC" build "$ROOT/tests/unit/aslexdump.as" \
        --stdlib "$ROOT/fsroot/as/lib" --toolchain "$ROOT/c/apps/as/runtime" \
        ${options[@]+"${options[@]}"} -o "$TMP/aslexdump"
    for source in "$ROOT"/fsroot/as/lib/*.as "$ROOT"/fsroot/as/examples/*.as; do
        if ! "$ASC" -lex "$source" > "$TMP/c.out" 2> "$TMP/c.err"; then
            echo "FAIL (C lex error, $mode) $source"
            sed -n '1,5p' "$TMP/c.err"
            fail=$((fail+1))
            continue
        fi
        if ! "$TMP/aslexdump" "$source" > "$TMP/as.out" 2> "$TMP/as.err"; then
            echo "FAIL (native A3 error, $mode) $source"
            sed -n '1,5p' "$TMP/as.err"
            fail=$((fail+1))
        elif cmp -s "$TMP/c.out" "$TMP/as.out"; then
            pass=$((pass+1))
        else
            echo "FAIL (token mismatch, $mode) $source"
            diff "$TMP/c.out" "$TMP/as.out" | head -5
            fail=$((fail+1))
        fi
    done
done
echo "selfhost-lex: $pass native A3 streams identical (debug/release), $fail failed"
[ "$fail" -eq 0 ] && [ "$pass" -gt 0 ]
