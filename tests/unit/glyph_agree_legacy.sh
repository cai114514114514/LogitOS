#!/bin/sh
# THE ONE BUILD IN WHICH BOTH RASTERIZERS EXIST.
#
# c/kernel/gui/raster.c is deleted by the commit this script belongs to. Before
# that, this builds tests/unit/glyph_agree_test.c with BOTH producers linked --
# raster.c under renamed symbols (-Dtext_raster=raster_legacy, and the same for
# its two COLR entry points) beside the Open Logit bridge -- and prints each
# against the supersampled oracle plus the two against each other.
#
# It is kept, and it does not run from `make`, because it CANNOT run from the
# tree it ships in: it needs files that are gone. A make target pointing at a
# deleted file would be a broken target pretending to be a gate.
#
# IT TAKES TWO FILES OUT OF HISTORY, NOT ONE, and that is the whole reason this
# script does its own `git show` instead of documenting a one-liner for a human
# to paste: raster.c's fourth include is "vg.h", which the same commit deletes,
# so the obvious recipe (extract raster.c, compile it) fails on a missing header
# and looks like the script is broken. Ask git for both, into a scratch
# directory that is also the first -I.
#
#     sh tests/unit/glyph_agree_legacy.sh              # finds the revision itself
#     sh tests/unit/glyph_agree_legacy.sh <rev>        # an explicit commit-ish
#     sh tests/unit/glyph_agree_legacy.sh /path/raster.c   # a file, with vg.h beside it
#
# The permanent gate is `make test-glyph-agree`, which measures the surviving
# rasterizer against the same oracle and needs nothing that was deleted.
set -e
ARG=${1:-}
OUT=${OUT:-/tmp/glyph_agree_both}
CC=${CC:-gcc}
WORK=${WORK:-${TMPDIR:-/tmp}/glyph_agree_legacy.$$}
mkdir -p "$WORK"

if [ -n "$ARG" ] && [ -f "$ARG" ]; then
    # A file was handed over: vg.h has to be beside it, for the reason above.
    cp "$ARG" "$WORK/raster.c"
    if [ -f "$(dirname "$ARG")/vg.h" ]; then
        cp "$(dirname "$ARG")/vg.h" "$WORK/vg.h"
    else
        echo "no vg.h beside $ARG -- raster.c includes it and it was deleted too;"
        echo "run this script with no argument and it will fetch both from git."
        exit 2
    fi
else
    REV=$ARG
    if [ -z "$REV" ]; then
        # The newest commit that TOUCHED raster.c -- which, once the deletion is
        # committed, IS the commit that deleted it.
        REV=$(git rev-list -1 HEAD -- c/kernel/gui/raster.c 2>/dev/null || true)
        [ -n "$REV" ] || { echo "no commit in this history ever had c/kernel/gui/raster.c"; exit 2; }
    fi
    # Step to the parent only if the file is genuinely absent at REV. Testing
    # rather than assuming matters in both directions: before the deletion is
    # committed HEAD still HAS the file and stepping back would silently measure
    # an older rasterizer, and someone naming the deletion commit by hand should
    # get the numbers rather than a git error about a path that is not there.
    git cat-file -e "$REV:c/kernel/gui/raster.c" 2>/dev/null || REV="$REV^"
    echo "legacy rasterizer from $REV ($(git rev-parse --short "$REV"))"
    git show "$REV:c/kernel/gui/raster.c" > "$WORK/raster.c"
    git show "$REV:c/kernel/gui/vg.h"     > "$WORK/vg.h"
fi

$CC -O2 -w -c "$WORK/raster.c" -o "$OUT.legacy.o" \
    -I"$WORK" -Ic/lib/text -Ic/kernel/gui \
    -Dtext_raster=raster_legacy \
    -Dtext_raster_at=raster_legacy_at \
    -Dtext_raster_extent=raster_legacy_extent

$CC -O2 -Wall -Wextra -DGLYPH_AGREE_LEGACY -o "$OUT" \
    tests/unit/glyph_agree_test.c \
    c/lib/text/ttf.c c/lib/text/cff.c c/lib/text/otlayout.c c/lib/text/fontcolor.c \
    c/lib/text/glyphras.c c/lib/gfx/gfx_math.c c/lib/gfx/gfx_path.c \
    c/lib/gfx/gfx_raster.c c/lib/gfx/gfx_paint.c c/lib/gfx/gfx_mask.c \
    c/lib/gfx/gfx_stroke.c "$OUT.legacy.o" \
    -Ic/lib/text -Ic/kernel/gui -Ic/lib/gfx -lm

"$OUT" fsroot/fonts/ui.ttf fsroot/fonts/mono.ttf \
       tests/fixtures/fonts/SourceSans3-Regular.otf \
       tests/fixtures/fonts/kern-subset.ttf
