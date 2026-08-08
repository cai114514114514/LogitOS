#!/usr/bin/env bash
# tools/wpt_fetch.sh -- vendor the Web Platform Tests subsets this browser is
# measured against, as DATA. Nothing executable from upstream is used: the
# runner is tests/unit/wpt_test.c and the Makefile fragment is tests/wpt.mk.
#
#   bash tools/wpt_fetch.sh [dest]     default dest: third_party/wpt
#
# WHY A SCRIPT AND NOT A COMMITTED TREE ONLY. Two reasons, and they are the
# same reason from opposite ends:
#
#   - the corpus may be DELETED (the request that started this was "bring it
#     in, fix everything, then delete it"). The runner treats a missing corpus
#     as "nothing to measure", exits 0, and says how to get it back. This is
#     how it gets back.
#   - the corpus may be pointed ELSEWHERE. `make test-wpt WPT_ROOT=~/wpt` runs
#     against a full upstream checkout, which is strictly more than what is
#     vendored here.
#
# FULL WPT IS ~50,000 TESTS AND IS NOT THE GOAL. What is taken is the layer
# whose defects were showing up as broken real sites -- the DOM, the HTML DOM
# and its reflection, form controls, encoding, URL, console. Everything else is
# left upstream on purpose; adding a subset is one line below.

set -euo pipefail

DEST="${1:-third_party/wpt}"
UPSTREAM="${WPT_UPSTREAM:-https://github.com/web-platform-tests/wpt}"

# The subsets. resources/ and common/ are not test directories -- they are what
# every test loads -- so they are not optional.
SUBSETS=(
    resources           # testharness.js and friends: the harness itself
    common              # get-host-info.sub.js, reftest-wait.js, ... helpers
    dom                 # nodes, events, ranges, traversal, the interfaces
    html/dom            # reflection, document.*, the element interface map
    html/semantics      # elements; FORMS above all -- typing into a page
    encoding            # TextEncoder / TextDecoder
    url                 # the URL parser (ours does not do IPv6 literals)
    console             # console.*
    css                 # the cascade, computed values and the CSSOM
)

# ON css/. It is by far the largest thing here -- 54,891 files, 239 MB -- and
# the first instinct was to leave it out on size. That was a category error:
# this corpus lives on the DEVELOPMENT HOST and is read by a host-side runner.
# It never enters build/disk.img, is never linked into anything, and never
# ships. The guest's 64 MB disk is not the constraint it was being compared to.
#
# Of those files, ~7,300 are testharness tests and run with the machinery that
# already exists -- they cover parsing, the cascade, computed values and the
# CSSOM, which is the layer that produced a real defect this week (css_vars.c
# truncated a custom property at 192 bytes, so apple.com's cascade saw 4% of
# its own declarations). ~17,000 are REFTESTS: judged by rendering the document
# and comparing pixels against a reference. Those cannot run here and the
# runner reports them as NOT RUN in their own column -- counted as failures
# they would drown the rate, counted as passes they would be a lie.

# Pure size with no bearing on this engine. Each line is a claim, so each line
# gets a reason:
PRUNE=(
    # 19 MB of legacy CJK codec tables. We implement UTF-8 and nothing else,
    # and a legacy-encoding table cannot become a passing test by fixing a bug
    # in the DOM -- it needs a decoder we have deliberately not written.
    encoding/legacy-mb-japanese
    encoding/legacy-mb-korean
    encoding/legacy-mb-tchinese
    encoding/legacy-mb-schinese
    # Media elements, images and plugins: 5.5 MB, mostly binary fixtures, and
    # the tests need a real media pipeline and a compositor rather than a DOM.
    html/semantics/embedded-content
    # testharness.js's own test suite -- it tests the harness, not us.
    resources/test
    # Vendor/tooling payloads that ride along in resources/.
    resources/chromium
    resources/webidl2
    resources/test262
    # Upstream's own runner/tooling. We take DATA; the runner is ours.
    wpt
    wpt.py
    lint.ignore
    CODEOWNERS
)

# css/ is deliberately NOT in SUBSETS this round. Its top level alone is 96
# directories and the subtree is the largest thing in the WPT repository --
# taking it would multiply the vendored size by roughly an order of magnitude
# before this mechanism has a rate worth defending. It is a second evaluation,
# not an omission: the CSS gaps that real pages actually reach (transform,
# ::before/::after) are ranked separately and would be the reason to add it.

echo "wpt-fetch: $UPSTREAM -> $DEST"
mkdir -p "$(dirname "$DEST")"
rm -rf "$DEST.tmp"

# Blobless + sparse + shallow: the full history is 3 GB and none of it is
# wanted. This pulls the tree of one commit and only the paths above.
git clone --filter=blob:none --no-checkout --depth=1 "$UPSTREAM" "$DEST.tmp"
(
    cd "$DEST.tmp"
    git sparse-checkout init --cone
    git sparse-checkout set "${SUBSETS[@]}"
    git checkout --quiet
    REV="$(git rev-parse HEAD)"
    for p in "${PRUNE[@]}"; do rm -rf "$p"; done
    # The git metadata is a third of the download and is not data.
    rm -rf .git
    printf '%s\n' "$REV" > WPT_REVISION
)

rm -rf "$DEST"
mv "$DEST.tmp" "$DEST"

# The corpus must be byte-identical to upstream or it measures the checkout
# rather than the browser. autocrlf=true in this repo would rewrite every one
# of these files, and a .dat/.html corpus rewritten to CRLF measures line
# endings. .gitattributes marks the whole tree -text; assert it is there rather
# than assuming, because the failure is silent.
if ! grep -q 'third_party/wpt' .gitattributes 2>/dev/null; then
    echo "wpt-fetch: WARNING -- .gitattributes has no 'third_party/wpt/** -text'"
    echo "  rule. Committing this corpus under autocrlf=true would rewrite every"
    echo "  file's line endings and the suite would measure that instead."
fi

echo "wpt-fetch: $(find "$DEST" -type f | wc -l) files, $(du -sh "$DEST" | cut -f1)"
echo "wpt-fetch: revision $(cat "$DEST/WPT_REVISION")"
