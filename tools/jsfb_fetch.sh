#!/usr/bin/env bash
# tools/jsfb_fetch.sh -- js-framework-benchmark as DATA, at a pin.
#
#   bash tools/jsfb_fetch.sh [dest]     default dest: build/jsfb
#
# WHAT THIS IS FOR, AND WHY IT IS NOT A BENCHMARK HERE.
# krausest/js-framework-benchmark is 253 independent implementations of ONE
# specified application: create 1,000 rows, create 10,000, append, update every
# tenth, select, swap, remove, clear. Upstream uses it to compare frameworks by
# milliseconds. That is not what it is for in this tree, and the difference
# matters:
#
#   - Timings here are meaningless in absolute terms. The guest runs under
#     QEMU/TCG on an arm64 host with no acceleration, and tools/perf/'s own
#     rule applies -- "the host is contended, other agents run QEMU
#     concurrently, so host wall clock is worthless here". Only BEFORE/AFTER of
#     the same build, measured inside the guest, means anything.
#   - What it IS good for is a CONFORMANCE MATRIX. Twenty-nine independent
#     authors wrote twenty-nine implementations of the same app against the
#     same spec. If ours runs six of them and not the other twenty-three, the
#     causes of those twenty-three are a work order that no single site can
#     produce -- and, decisively, one that CANNOT BE FITTED TO. You cannot
#     special-case thirty independent codebases into working; you can only
#     implement the platform they all stand on.
#
# tests/frameworks.mk already measures seven applications built from the
# frameworks' own toolchains, and its header states what it adds over captured
# real sites: "Nothing in them is broken, which is what makes an exception here
# attributable." This corpus adds the next thing that one lacks: those seven
# prove a framework can BOOT. These prove one can drive a large, mutating DOM,
# which is what an application does the moment its data arrives -- the failure
# class where a page renders a shell and never fills it.
#
# THE PIN. Same argument as tools/wpt_revision.txt, quoted because it is the
# reason: "Fetching HEAD and RECORDING what arrived is not a pin -- the ratchet
# was measured against one revision, and a corpus from another makes it lie
# quietly." tools/jsfb_revision.txt is tracked; the corpus is not.
#
# NOT third_party/. Like build/wpt and build/html5lib-tests, this is test DATA
# fetched at a revision, not vendored source. It never enters build/disk.img,
# is never linked into anything, and never ships.
set -euo pipefail

DEST="${1:-build/jsfb}"
HERE="$(cd "$(dirname "$0")" && pwd)"

PIN="$(tr -d "[:space:]" < "$HERE/jsfb_revision.txt")"
UPSTREAM="${JSFB_UPSTREAM:-https://github.com/krausest/js-framework-benchmark}"
BRANCH="${JSFB_BRANCH:-master}"

# Pure size and payloads with no bearing on this browser. Each line is a claim,
# so each line gets a reason -- the shape tools/wpt_fetch.sh's PRUNE uses, and
# for the same reason: a corpus that carries what cannot possibly pass reports
# a rate about the checkout rather than about the engine. WPT's own PRUNE
# argument transfers exactly: "a legacy-encoding table cannot become a passing
# test by fixing a bug in the DOM -- it needs a decoder we have deliberately
# not written." Neither can a .wasm blob.
PRUNE=(
    # Upstream's own Playwright/CDP runner and its published results. We take
    # DATA; the runner is ours. 11 MB.
    webdriver-ts
    webdriver-ts-results
    # A LibreOffice spreadsheet of somebody's Chrome results.
    Chrome_Results.ods
    # Implementations upstream itself marks broken.
    broken-frameworks
)

mkdir -p "$(dirname "$DEST")"
rm -rf "$DEST.tmp"; mkdir -p "$DEST.tmp"

echo "jsfb-fetch: $UPSTREAM @ ${PIN:0:12} -> $DEST"
git -C "$DEST.tmp" init --quiet
git -C "$DEST.tmp" remote add origin "$UPSTREAM"
# Blobless + shallow: the full history is not wanted. Fetch EXACTLY the pinned
# commit, not the branch tip -- GitHub serves a reachable SHA directly.
git -C "$DEST.tmp" fetch --quiet --filter=blob:none --depth 1 origin "$PIN" \
    || git -C "$DEST.tmp" fetch --quiet --filter=blob:none --depth 1 origin "$BRANCH"
(
    cd "$DEST.tmp"
    git checkout --quiet FETCH_HEAD
    REV="$(git rev-parse HEAD)"
    if [ "$REV" != "$PIN" ]; then
        echo "jsfb-fetch: got $REV, pinned $PIN -- refusing" >&2; exit 1
    fi
    for p in "${PRUNE[@]}"; do rm -rf "$p"; done

    # WASM AND NON-JAVASCRIPT IMPLEMENTATIONS ARE REMOVED, AND THIS IS THE ONE
    # PRUNE DECISION THAT IS NOT ABOUT SIZE.
    #
    # This browser has no WebAssembly. An implementation whose application
    # logic is a .wasm blob cannot become runnable by fixing anything in the
    # DOM, the event loop or the CSS engine -- it needs an interpreter this
    # tree has deliberately not written. Leaving them in would put ~24
    # permanently-red rows in a matrix whose whole value is that a red row is
    # a work order.
    #
    # It is DERIVED, not listed. A hand-written list of "the wasm ones" is
    # exactly the constant-spelled-twice this tree has paid for three times;
    # upstream adds implementations continuously and the list would rot. The
    # test is the artefact itself: a directory shipping a .wasm cannot run.
    #
    # NOTE WHAT IS *NOT* REMOVED: implementations whose JavaScript was
    # generated by a compiler from Scala, Haskell, PureScript or OCaml
    # (binding.scala, reflex-dom, halogen, incr_dom, miso-js, laminar). That
    # output IS JavaScript and it exercises the DOM API in shapes no
    # hand-written bundle produces -- which is a feature of a conformance
    # matrix, not a defect in it.
    nwasm=0
    for d in frameworks/*/*/; do
        [ -d "$d" ] || continue
        if find "$d" -name '*.wasm' -print -quit 2>/dev/null | grep -q .; then
            rm -rf "$d"; nwasm=$((nwasm+1))
        fi
    done
    echo "jsfb-fetch: pruned $nwasm wasm implementations (no WebAssembly on this machine)"

    rm -rf .git
    printf '%s\n' "$REV" > JSFB_REVISION
)

rm -rf "$DEST"
mv "$DEST.tmp" "$DEST"

# THE SPLIT IS DISCOVERED, NOT DECLARED. Which implementations need an npm
# build is a property of each package.json ("build-prod"), and writing it into
# a list here would be a second door onto the same jar -- it would disagree
# with the corpus the first time upstream changes a script.
#
# This used to test `build-prod` inline, which made it the SECOND spelling of
# the rule the moment anything else consumed the corpus -- exactly the shape
# this tree has paid for three times (/dev/log, LOGIT_ARG_MAX, the IME chord).
# tests/unit/jsfb_corpus.py is the one reader now and this asks it. It also
# knows the thing this loop did not: where the DOCUMENT is. 39 implementations
# declare `js-framework-benchmark.customURL`, and two of them -- binding.scala
# and reflex-dom, the Scala and Haskell ones -- have no index.html beside their
# package.json at all.
echo "jsfb-fetch: $(find "$DEST" -type f | wc -l | tr -d ' ') files, $(du -sh "$DEST" | cut -f1)"
echo "jsfb-fetch: revision $(cat "$DEST/JSFB_REVISION")"
echo "jsfb-fetch: $(python3 "$HERE/../tests/unit/jsfb_corpus.py" --root "$DEST" --count)"
