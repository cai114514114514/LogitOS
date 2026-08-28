#!/usr/bin/env bash
# tools/h5l_fetch.sh -- fetch the HTML parser conformance corpus as DATA.
#
#   bash tools/h5l_fetch.sh [dest]     default dest: build/html5lib-tests
#
# WHY THIS EXISTS AT ALL, and why it is two fetches rather than one.
# `third_party/html5lib-tests/` was ONE directory carrying TWO upstreams and two
# licence files, which is why it had no single revision to pin and never carried
# one. Measured 2026-08-28, byte for byte, all 75 files:
#
#   tokenizer/          14 files  ==  html5lib/html5lib-tests            (MIT)
#   tree-construction/  61 files  ==  wpt html/syntax/parsing/resources  (BSD-3)
#
# So the 61 were never html5lib-tests' at all -- they are a copy of a corpus this
# tree ALREADY fetches at a pinned revision (tools/wpt_fetch.sh). Vendoring them
# a second time is the "one jar, TWO doors" shape applied to test DATA: two
# copies of the same corpus, and nothing checking that they agree.
#
# THE PIN. Same argument as wpt_revision.txt, quoted because it is the reason:
# "Fetching HEAD and RECORDING what arrived is not a pin -- the ratchet was
# measured against one revision, and a corpus from another makes it lie quietly."
# tools/h5l_revision.txt is tracked; the corpus is not.
set -euo pipefail

DEST="${1:-build/html5lib-tests}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

PIN="$(tr -d "[:space:]" < "$HERE/h5l_revision.txt")"
UPSTREAM="${H5L_UPSTREAM:-https://github.com/html5lib/html5lib-tests}"

# The WPT half comes from wherever WPT already is -- it is not fetched twice.
WPT="${WPT_ROOT:-$ROOT/build/wpt}"
WPT_SUB="html/syntax/parsing/resources"

mkdir -p "$DEST"

# --- half 1: the tokenizer tests, from html5lib-tests at the pin -------------
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
echo "h5l-fetch: $UPSTREAM @ ${PIN:0:12} -> $DEST/tokenizer"
git -C "$tmp" init -q
git -C "$tmp" remote add o "$UPSTREAM"
git -C "$tmp" fetch -q --depth 1 o "$PIN"
rm -rf "$DEST/tokenizer"
mkdir -p "$DEST/tokenizer"
git -C "$tmp" archive FETCH_HEAD tokenizer | tar -x -C "$DEST" 2>/dev/null || {
    git -C "$tmp" checkout -q FETCH_HEAD -- tokenizer
    cp -R "$tmp/tokenizer/." "$DEST/tokenizer/"
}
git -C "$tmp" archive FETCH_HEAD LICENSE > /dev/null 2>&1 &&
    git -C "$tmp" show FETCH_HEAD:LICENSE > "$DEST/LICENSE.html5lib-tests" || true

# --- half 2: tree-construction, from the WPT checkout that already exists ----
# NOT a second download. If WPT is absent, say so and name the one command that
# fixes it -- the same shape wpt.mk uses, because a corpus that is not there is
# a capability the machine does not have right now, not a failure of the parser.
if [ ! -d "$WPT/$WPT_SUB" ]; then
    echo "h5l-fetch: SKIP tree-construction -- no WPT corpus at $WPT"
    echo "           the 61 tree-construction files ARE wpt $WPT_SUB;"
    echo "           run \`make wpt-fetch\` first, or pass WPT_ROOT=<a checkout>."
    echo "           tokenizer/ is complete; test-html5lib-tok can run, the"
    echo "           tree-construction gates cannot."
else
    rm -rf "$DEST/tree-construction"
    mkdir -p "$DEST/tree-construction"
    n=0
    for f in "$WPT/$WPT_SUB"/*.dat; do
        [ -e "$f" ] || continue
        cp "$f" "$DEST/tree-construction/"; n=$((n+1))
    done
    cp "$WPT/../LICENSE.md" "$DEST/LICENSE.wpt" 2>/dev/null ||
        cp "$WPT/LICENSE.md" "$DEST/LICENSE.wpt" 2>/dev/null || true
    echo "h5l-fetch: $n tree-construction files from $WPT/$WPT_SUB"
fi

echo "h5l-fetch: $(find "$DEST" -type f | wc -l | tr -d ' ') files -> $DEST"
