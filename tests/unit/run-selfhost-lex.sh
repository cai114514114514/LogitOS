#!/usr/bin/env bash
# M21-P3 S1 gate: the AetherScript lexer (lib/aslex.as) must produce a token
# stream identical to the C lexer over the whole in-tree .as corpus.
set -u
ASC="${1:?usage: run-selfhost-lex.sh <asc>}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cp fsroot/as/lib/aslex.as "$TMP/aslex.as"
cp tests/unit/aslexdump.as "$TMP/aslexdump.as"
pass=0; fail=0
for f in fsroot/as/lib/*.as fsroot/as/examples/*.as; do
    "$ASC" -lex "$f" > "$TMP/c.out" 2>/dev/null || { echo "SKIP (C lex error) $f"; continue; }
    ( cd "$TMP" && "$OLDPWD/$ASC" aslexdump.as "$OLDPWD/$f" > as.out 2>err.txt ) \
        || { echo "FAIL (as error) $f: $(cat "$TMP/err.txt")"; fail=$((fail+1)); continue; }
    if cmp -s "$TMP/c.out" "$TMP/as.out"; then pass=$((pass+1));
    else echo "FAIL (mismatch) $f"; diff "$TMP/c.out" "$TMP/as.out" | head -5; fail=$((fail+1)); fi
done
echo "selfhost-lex: $pass identical, $fail failed"
[ "$fail" -eq 0 ] && [ "$pass" -gt 0 ]
