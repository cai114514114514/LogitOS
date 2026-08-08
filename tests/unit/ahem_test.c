/* Can we render Ahem? -- the question that had to be answered before the layout
 * work went any further, and the reason it is its own target.
 *
 * A large class of WPT layout reftests sets `font: 25px/1 Ahem`. That is not
 * decoration: Ahem's glyphs are exactly-specified filled rectangles, one em
 * wide, running from 0.8em above the baseline to 0.2em below, with three
 * documented exceptions. A test drawn in Ahem and a reference drawn as a plain
 * coloured <div> are then the SAME PICTURE in every conforming engine, which is
 * what removes font rasterization from the comparison entirely. If we cannot
 * render Ahem, that whole class is unjudgeable no matter how good layout gets.
 *
 * THE ANSWER IS YES, and this file is the assertion rather than the claim.
 * Everything below goes through the REAL text path -- c/kernel/gui/text.c over
 * c/lib/text's ttf.c and c/kernel/gui/raster.c, the same code the machine draws
 * with -- so a regression that made Ahem fuzzy would fail here rather than
 * showing up later as an unexplained drop in the reftest rate.
 *
 * Note third_party/wpt/fonts/ahem.ttf does NOT exist: the vendored WPT subset in
 * this tree omits fonts/. The font is fetched to .cache/ (make reftest-ahem-fetch).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "refhost.h"

int  text_measure(const char *s, int len, int px, int mono);
int  text_draw_run(int x, int y, const char *s, int len, int px, int mono, uint32_t color);
int  text_line_height(int px);

static int fails;
#define CHECK(c, ...) do { if (c) { printf("ok: "); } else { printf("FAIL: "); fails++; } \
                           printf(__VA_ARGS__); printf("\n"); } while (0)

/* Ink bounding box of the current canvas, plus the crispness census. `partial`
 * is the count of pixels that are neither background nor fully inked -- for
 * Ahem it must be ZERO, because an antialiased edge is exactly what makes exact
 * pixel comparison impossible. */
static void inkbox(const uint32_t *px, int w, int h, int *x0, int *y0, int *x1, int *y1,
                   long *solid, long *partial)
{
    *x0 = *y0 = 1 << 28; *x1 = *y1 = -1; *solid = *partial = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t v = px[(long)y * w + x] & 0xFFFFFFu;
            if (v == 0xFFFFFFu) continue;                 /* white background */
            if (x < *x0) *x0 = x; if (y < *y0) *y0 = y;
            if (x > *x1) *x1 = x; if (y > *y1) *y1 = y;
            if (v == 0x000000u) (*solid)++; else (*partial)++;
        }
}

static void draw(const char *utf8, int len, int px)
{
    refhost_begin(400, 400, 0xFFFFFF);
    text_draw_run(0, 0, utf8, len, px, 0, 0x000000);
    refhost_end();
}

int main(int argc, char **argv)
{
    const char *ahem = argc > 1 ? argv[1] : ".cache/ahem/Ahem.ttf";
    refhost_font_map(ahem, ahem);
    if (refhost_fonts() != 0) {
        printf("FAIL: %s did not load through ttf_parse -- Ahem is UNUSABLE\n", ahem);
        printf("  Every Ahem-based layout reftest is unjudgeable until this passes.\n");
        return 1;
    }
    printf("Ahem loaded: %s\n\n", ahem);

    const int px = 25;

    /* --- metrics. These are what layout word-wraps against, so they matter as
     * much as the pixels: text_measure is the SAME function layout.c calls. --- */
    CHECK(text_measure("X", 1, px, 0) == px,
          "text_measure(\"X\", 25px) == 25  (one em per glyph)  [got %d]",
          text_measure("X", 1, px, 0));
    CHECK(text_measure("XX", 2, px, 0) == 2 * px,
          "text_measure(\"XX\", 25px) == 50 (advances are exact em multiples)  [got %d]",
          text_measure("XX", 2, px, 0));
    CHECK(text_line_height(px) == px,
          "text_line_height(25) == 25  (ascent .8em + descent .2em)  [got %d]",
          text_line_height(px));

    /* --- the glyph itself. A full em box, and crisp. --- */
    uint32_t *cv;
    int x0, y0, x1, y1; long solid, partial;
    draw("X", 1, px);
    cv = refhost_end();
    inkbox(cv, 400, 400, &x0, &y0, &x1, &y1, &solid, &partial);
    CHECK(x1 >= 0, "'X' draws ink at all");
    CHECK(x0 == 0 && y0 == 0 && x1 == px - 1 && y1 == px - 1,
          "'X' at y=top=0 is exactly the em box x[0..%d] y[0..%d]  [got x[%d..%d] y[%d..%d]]",
          px - 1, px - 1, x0, x1, y0, y1);
    CHECK(solid == (long)px * px,
          "the box is %ld fully-inked pixels  [got %ld]", (long)px * px, solid);
    /* THE ONE THAT MAKES EXACT COMPARISON VIABLE. */
    CHECK(partial == 0,
          "ZERO antialiased edge pixels -- exact pixel comparison is viable  [got %ld]",
          partial);

    /* --- the three documented exceptions, from Ahem's own README. Getting
     * these right is how you know the cmap and the glyf outlines are real and
     * not a lucky uniform box. --- */
    draw(" ", 1, px);
    cv = refhost_end();
    inkbox(cv, 400, 400, &x0, &y0, &x1, &y1, &solid, &partial);
    CHECK(x1 < 0, "space draws nothing");

    draw("p", 1, px);
    cv = refhost_end();
    inkbox(cv, 400, 400, &x0, &y0, &x1, &y1, &solid, &partial);
    CHECK(y0 == (px * 4) / 5 && y1 == px - 1,
          "'p' is the DESCENDER only, y[%d..%d]  [got y[%d..%d]]",
          (px * 4) / 5, px - 1, y0, y1);

    draw("\xC3\x89", 2, px);          /* U+00C9 LATIN CAPITAL E WITH ACUTE */
    cv = refhost_end();
    inkbox(cv, 400, 400, &x0, &y0, &x1, &y1, &solid, &partial);
    CHECK(y0 == 0 && y1 == (px * 4) / 5 - 1,
          "U+00C9 is the ASCENDER only, y[0..%d]  [got y[%d..%d]]",
          (px * 4) / 5 - 1, y0, y1);

    /* --- and that it holds at other sizes, since layout uses many --- */
    for (int p = 10; p <= 60; p += 10) {
        draw("X", 1, p);
        cv = refhost_end();
        inkbox(cv, 400, 400, &x0, &y0, &x1, &y1, &solid, &partial);
        CHECK(x1 - x0 + 1 == p && y1 - y0 + 1 == p && partial == 0,
              "%dpx: exact %dx%d box, no AA  [got %dx%d, %ld partial]",
              p, p, p, x1 - x0 + 1, y1 - y0 + 1, partial);
    }

    printf("\n%s\n", fails ? "AHEM: FAIL" :
        "AHEM: USABLE -- the Ahem-based layout reftests are judgeable");
    return fails ? 1 : 0;
}
