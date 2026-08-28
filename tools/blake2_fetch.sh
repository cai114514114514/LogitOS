#!/usr/bin/env bash
# tools/blake2_fetch.sh -- fetch the BLAKE2 reference implementation's own
# known-answer-test vector file as DATA, at a pinned revision.
#
#   bash tools/blake2_fetch.sh [dest]     default dest: build/blake2-kat
#
# WHY THIS FILE AND NOT A HAND-WRITTEN VECTOR LIST. tests/unit/blake2s_test.c
# generating its own expected outputs would prove only that the implementation
# agrees with itself -- see CLAUDE.md on mini-libc's "correct means agrees with
# glibc" rule, applied here to the reference C implementation's own KAT. This
# is testvectors/blake2s-kat.txt from github.com/BLAKE2/BLAKE2 (the reference
# repository that defines the algorithm, not a third party's guess at it): 256
# vectors, keyed with the fixed 32-byte key 00..1f, input length running 0..255
# bytes as 00,01,02,...,(n-1), each with its 32-byte BLAKE2s digest. That
# exercises the keyed path (one padded key block prepended, counted into t) at
# every input length relative to the 64-byte block boundary -- 0, 1, partial,
# exactly one block, one-block-plus-one, and on past several blocks -- which is
# exactly where an off-by-one in the counter or the final-block flag would show
# up and nowhere a short "abc"-only smoke test would reach.
#
# THE PIN. Same argument as wpt_revision.txt: fetching HEAD and recording what
# arrived is not a pin -- it would make the file's own git history able to
# change what "known answer" means without the test source changing at all.
# tools/blake2_revision.txt is tracked; the corpus is not.
set -euo pipefail

DEST="${1:-build/blake2-kat}"
HERE="$(cd "$(dirname "$0")" && pwd)"

PIN="$(tr -d "[:space:]" < "$HERE/blake2_revision.txt")"
UPSTREAM="${BLAKE2_UPSTREAM:-https://raw.githubusercontent.com/BLAKE2/BLAKE2}"

mkdir -p "$DEST"
echo "blake2-fetch: $UPSTREAM @ ${PIN:0:12} -> $DEST/blake2s-kat.txt"

curl -fsSL "$UPSTREAM/$PIN/testvectors/blake2s-kat.txt" -o "$DEST/blake2s-kat.txt.tmp" || {
    echo "blake2-fetch: SKIP -- could not reach $UPSTREAM (no network, or host down)"
    echo "               tests/unit/blake2s_test.c treats a missing file as a"
    echo "               skip, not a failure -- run this script again once"
    echo "               network is available."
    rm -f "$DEST/blake2s-kat.txt.tmp"
    exit 0
}
mv "$DEST/blake2s-kat.txt.tmp" "$DEST/blake2s-kat.txt"

n=$(grep -c '^in:' "$DEST/blake2s-kat.txt" || true)
echo "blake2-fetch: $n vectors -> $DEST/blake2s-kat.txt"
