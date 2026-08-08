/* tests/unit/flex_test.c -- CSS Flexbox § 9, checked against the numbers the
 * spec names.
 *
 * WHY THIS TEST EXISTS IN THIS FORM. Flex layout is specified as a numbered
 * algorithm that produces EXACT values: given a 500px container and items with
 * `flex: 1 1 100px`, `flex: 2 1 100px` and `flex: 0 0 80px`, the used main
 * sizes are 173, 247 and 80 and nothing else. So the correctness of a flexbox
 * implementation can be settled with arithmetic and no renderer at all -- no
 * reference image, no screenshot, no font backend. Every expected number below
 * was worked out from the spec text (drafts.csswg.org/css-flexbox-1/ § 9.2,
 * 9.3, 9.4, 9.5, 9.6, 9.7 and § 4.5) BEFORE the implementation was written, and
 * the derivation is in the comment above each case so a reader can check the
 * expectation rather than trust it.
 *
 * This stays the right test after a reftest harness lands. A reftest asks
 * whether these numbers reach the screen. That is a different question from
 * whether they are right, and only one of the two is answered here.
 *
 * THE NEGATIVE CONTROL is -DFLEX_UNSCALED_SHRINK, which distributes negative
 * free space by the raw `flex-shrink` value instead of by flex-shrink times the
 * flex base size. It is not a deletion: it lays pages out, the totals still add
 * up, and only the individual item sizes are wrong. It is the mistake a careful
 * reading-from-memory produces. Cases marked [NEGCTL] are the ones that catch
 * it; `make test-flex-negctl` requires this binary to FAIL when it is built.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "layout_flex.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

/* ---------------- harness ---------------- */

static int checks, failures;
static const char *group = "";

static void ck(int ok, const char *what)
{
    checks++;
    if (!ok) { failures++; printf("  FAIL [%s] %s\n", group, what); }
}
static void ck_eq(int got, int want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL [%s] %s: got %d, want %d\n", group, what, got, want);
    }
}

/* ---------------- style construction ----------------
 *
 * A cstyle built by hand, not by the cascade. css_engine.c is not linked here
 * on purpose: this test is about the ALGORITHM, and running the real cascade
 * would put a second implementation (LibCSS's property mapping) inside the
 * thing under test, so a failure would no longer say which of the two moved. */
static void st_init(struct cstyle *s)
{
    memset(s, 0, sizeof *s);
    s->display      = DISP_BLOCK;
    s->flex_dir     = FDIR_ROW;
    s->flex_wrap    = FWRAP_NOWRAP;
    s->justify      = JC_START;
    s->align_items  = AL_STRETCH;
    s->align_content = AL_STRETCH;
    s->align_self   = AL_AUTO;
    s->flex_grow    = 0;
    s->flex_shrink  = 1024;          /* flex-shrink's initial value is 1 */
    s->box_sizing   = BOX_CONTENT;
    s->overflow_x   = OVF_VISIBLE;
    s->overflow_y   = OVF_VISIBLE;
    s->font_px      = 16;
    s->opacity      = 255;
}

/* `flex: <grow> <shrink> <basis>px` */
static void st_flex(struct cstyle *s, double grow, double shrink, int basis)
{
    s->flex_grow   = (int)(grow   * 1024 + 0.5);
    s->flex_shrink = (int)(shrink * 1024 + 0.5);
    s->flex_basis  = basis; s->has_fb = 1; s->fb_pct = 0; s->fb_off = 0;
}

/* ---------------- measurement stub ----------------
 *
 * The one thing the algorithm cannot compute for itself is how tall an item
 * gets at a given width. Here that is a table rather than real layout, which is
 * the point: the numbers are inputs, so a failure is the algorithm's. */
struct tref {
    int cross;          /* fixed content-box cross size */
    long area;          /* if > 0, cross = ceil(area / main) -- a text-shaped
                         * item, so a case can exercise "taller when narrower" */
    int baseline;       /* offset from the border-box cross-start edge, or -1 */
};

static int meas_cross(void *ref, int main_inner, void *ctx)
{
    (void)ctx;
    struct tref *t = ref;
    if (!t) return 0;
    if (t->area > 0) { if (main_inner <= 0) return 0;
                       return (int)((t->area + main_inner - 1) / main_inner); }
    return t->cross;
}
static int meas_base(void *ref, int mn, int cr, void *ctx)
{
    (void)mn; (void)cr; (void)ctx;
    struct tref *t = ref;
    return t ? t->baseline : -1;
}
static const struct flex_metrics MET = { meas_cross, meas_base, 0 };

/* ---------------- input construction ---------------- */

static struct flex_in mk_container(const struct cstyle *st, int am, int ac)
{
    struct flex_in c;
    memset(&c, 0, sizeof c);
    c.st = st; c.avail_main = am; c.avail_cross = ac;
    c.wm = FLEX_WM_HORIZ_TB; c.rtl = 0;
    c.align_content_space = -1;
    return c;
}
static void mk_item(struct flex_item_in *it, const struct cstyle *st, void *ref,
                    int minc, int maxc)
{
    memset(it, 0, sizeof *it);
    it->st = st; it->ref = ref;
    it->min_content_main = minc; it->max_content_main = maxc;
    it->basis = FLEX_FB_FROM_STYLE;
}

/* The output entry for input index `idx` (the output is in order-modified
 * order, so it is not simply out.items[idx]). */
static const struct flex_item_out *byidx(const struct flex_out *o, int idx)
{
    for (int i = 0; i < o->nitems; i++) if (o->items[i].idx == idx) return &o->items[i];
    return 0;
}

/* ================================================================ *
 * § 9.2  flex base size and hypothetical main size
 * ================================================================ */

static void t_base_sizes(void)
{
    group = "9.2 flex base size";
    struct cstyle cs; st_init(&cs); cs.display = DISP_FLEX;
    struct cstyle a, b, c;
    struct flex_item_in in[3];
    struct flex_out out;

    /* An explicit flex-basis LENGTH is the flex base size outright. Nothing
     * flexes here (grow 0, and the line overflows so shrink would apply -- but
     * shrink is 0 too), so the used main size IS the hypothetical main size. */
    st_init(&a); st_flex(&a, 0, 0, 100);
    st_init(&b); st_flex(&b, 0, 0, 250);
    mk_item(&in[0], &a, 0, 0, 999); mk_item(&in[1], &b, 0, 0, 999);
    struct flex_in ct = mk_container(&cs, 300, FLEX_INDEFINITE);
    ck(layout_flex_run(&ct, in, 2, &MET, &out) == 0, "run");
    ck_eq(byidx(&out, 0)->main_size, 100, "flex-basis:100px -> base 100");
    ck_eq(byidx(&out, 1)->main_size, 250, "flex-basis:250px -> base 250");
    layout_flex_free(&out);

    /* flex-basis:auto defers to the main size property (width, in a row). */
    st_init(&a); a.flex_grow = 0; a.flex_shrink = 0; a.has_w = 1; a.width = 120;
    mk_item(&in[0], &a, 0, 0, 999);
    ct = mk_container(&cs, 500, FLEX_INDEFINITE);
    layout_flex_run(&ct, in, 1, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 120, "flex-basis:auto + width:120 -> base 120");
    layout_flex_free(&out);

    /* flex-basis:auto with no width falls through to the max-content size. */
    st_init(&a); a.flex_grow = 0; a.flex_shrink = 0;
    mk_item(&in[0], &a, 0, 40, 175);
    layout_flex_run(&ct, in, 1, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 175, "flex-basis:auto, no width -> max-content");
    layout_flex_free(&out);

    /* flex-basis:content IGNORES width. This is the case struct cstyle cannot
     * express (it has no `content` value), which is why flex_item_in carries
     * the basis kind separately. */
    st_init(&a); a.flex_grow = 0; a.flex_shrink = 0; a.has_w = 1; a.width = 120;
    mk_item(&in[0], &a, 0, 40, 175); in[0].basis = FLEX_FB_CONTENT;
    layout_flex_run(&ct, in, 1, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 175, "flex-basis:content ignores width");
    layout_flex_free(&out);

    /* A percentage flex-basis resolves against the container's inner main size. */
    st_init(&a); a.flex_grow = 0; a.flex_shrink = 0;
    a.has_fb = 1; a.flex_basis = 50; a.fb_pct = 1;
    mk_item(&in[0], &a, 0, 0, 10);
    ct = mk_container(&cs, 400, FLEX_INDEFINITE);
    layout_flex_run(&ct, in, 1, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 200, "flex-basis:50% of 400 -> 200");
    layout_flex_free(&out);

    /* ...and is INDEFINITE against an indefinite main size, so it behaves as
     * auto and falls through to max-content. */
    ct = mk_container(&cs, FLEX_INDEFINITE, FLEX_INDEFINITE);
    mk_item(&in[0], &a, 0, 0, 60);
    layout_flex_run(&ct, in, 1, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 60, "flex-basis:% of indefinite -> max-content");
    layout_flex_free(&out);

    /* box-sizing:border-box. flex-basis:100px with 10px of padding a side is an
     * OUTER 100, hence an inner 80. */
    st_init(&a); st_flex(&a, 0, 0, 100); a.box_sizing = BOX_BORDER;
    a.pl = a.pr = 10;
    mk_item(&in[0], &a, 0, 0, 0);
    ct = mk_container(&cs, 500, FLEX_INDEFINITE);
    layout_flex_run(&ct, in, 1, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 80, "border-box basis 100 - 2x10 padding -> 80");
    ck_eq(byidx(&out, 0)->main_outer, 100, "...and the border box is still 100");
    layout_flex_free(&out);

    /* § 9.2's own example: "an item with a specified size of zero, positive
     * padding, and box-sizing: border-box will have an outer flex base size of
     * zero -- and hence a NEGATIVE inner flex base size". The hypothetical main
     * size then floors the content box at zero. */
    st_init(&a); st_flex(&a, 0, 0, 0); a.box_sizing = BOX_BORDER; a.pl = a.pr = 10;
    mk_item(&in[0], &a, 0, 0, 0);
    layout_flex_run(&ct, in, 1, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 0, "negative inner base floors at 0");
    ck_eq(byidx(&out, 0)->main_outer, 20, "...leaving the padding");
    layout_flex_free(&out);

    /* min/max clamp the flex base size into the hypothetical main size. */
    st_init(&a); st_flex(&a, 0, 0, 100); a.has_max_w = 1; a.max_w = 60;
    st_init(&b); st_flex(&b, 0, 0, 100); b.has_min_w = 1; b.min_w = 150;
    st_init(&c); st_flex(&c, 0, 0, 100); c.has_min_w = 1; c.min_w = 150;
    c.has_max_w = 1; c.max_w = 60;      /* min wins over max */
    mk_item(&in[0], &a, 0, 0, 0); mk_item(&in[1], &b, 0, 0, 0); mk_item(&in[2], &c, 0, 0, 0);
    ct = mk_container(&cs, 900, FLEX_INDEFINITE);
    layout_flex_run(&ct, in, 3, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 60,  "max-width clamps the hypothetical size");
    ck_eq(byidx(&out, 1)->main_size, 150, "min-width clamps the hypothetical size");
    ck_eq(byidx(&out, 2)->main_size, 150, "min beats max");
    layout_flex_free(&out);
}

/* ================================================================ *
 * § 9.7  resolving flexible lengths -- the heart of the thing
 * ================================================================ */

static void t_resolve(void)
{
    group = "9.7 resolve";
    struct cstyle cs; st_init(&cs); cs.display = DISP_FLEX;
    struct cstyle a, b, c, d;
    struct flex_item_in in[4];
    struct flex_out out;
    struct flex_in ct;

    /* THE CANONICAL CASE. 500px container; flex: 1 1 100px / 2 1 100px /
     * 0 0 80px.
     *   step 1: sum of outer hypothetical sizes = 280 < 500 -> GROW.
     *   step 3: item 3 has a flex factor of zero -> frozen at 80.
     *   step 4: initial free space = 500 - 280 = 220.
     *   loop:   sum of unfrozen grow factors = 3 (not < 1).
     *           item 1 = 100 + 220 * 1/3 = 173.33
     *           item 2 = 100 + 220 * 2/3 = 246.67
     *           no violations, total violation 0 -> freeze all, exit.
     *   round:  exact total 500; floors 173+246+80 = 499; the largest
     *           fractional part (.67) takes the pixel -> 173, 247, 80. */
    st_init(&a); st_flex(&a, 1, 1, 100);
    st_init(&b); st_flex(&b, 2, 1, 100);
    st_init(&c); st_flex(&c, 0, 0, 80);
    mk_item(&in[0], &a, 0, 0, 0); mk_item(&in[1], &b, 0, 0, 0); mk_item(&in[2], &c, 0, 0, 0);
    ct = mk_container(&cs, 500, FLEX_INDEFINITE);
    layout_flex_run(&ct, in, 3, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 173, "grow 1 of 3 over 220 -> 173");
    ck_eq(byidx(&out, 1)->main_size, 247, "grow 2 of 3 over 220 -> 247");
    ck_eq(byidx(&out, 2)->main_size, 80,  "flex factor 0 -> frozen at 80");
    ck_eq(byidx(&out, 0)->main_size + byidx(&out, 1)->main_size +
          byidx(&out, 2)->main_size, 500, "the line fills the container exactly");
    layout_flex_free(&out);

    /* [NEGCTL] THE SCALED FLEX SHRINK FACTOR. 200px container; two items,
     * flex: 0 1 200px and flex: 0 1 100px. Sum 300 > 200 -> shrink, free -100.
     *   scaled shrink factors: 1x200 = 200 and 1x100 = 100, sum 300.
     *   item 1 = 200 - 100 * 200/300 = 133.33
     *   item 2 = 100 - 100 * 100/300 =  66.67  -> rounds to 133 and 67.
     * An implementation using the RAW shrink factor takes 50 off each and gets
     * 150 / 50: the same total, both plausible, both wrong. */
    st_init(&a); st_flex(&a, 0, 1, 200);
    st_init(&b); st_flex(&b, 0, 1, 100);
    mk_item(&in[0], &a, 0, 0, 0); mk_item(&in[1], &b, 0, 0, 0);
    ct = mk_container(&cs, 200, FLEX_INDEFINITE);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 133, "shrink weighted by base: 200px item -> 133");
    ck_eq(byidx(&out, 1)->main_size, 67,  "shrink weighted by base: 100px item -> 67");
    layout_flex_free(&out);

    /* [NEGCTL] The same rule where the shrink factors also differ, so the two
     * readings cannot coincide. 300px container; flex: 0 1 300px and
     * flex: 0 3 100px. Sum 400, free -100.
     *   scaled: 1x300 = 300 and 3x100 = 300, sum 600 -> each loses 50.
     *   -> 250 and 50.  Raw factors (1 and 3, sum 4) would give 275 and 25. */
    st_init(&a); st_flex(&a, 0, 1, 300);
    st_init(&b); st_flex(&b, 0, 3, 100);
    mk_item(&in[0], &a, 0, 0, 0); mk_item(&in[1], &b, 0, 0, 0);
    ct = mk_container(&cs, 300, FLEX_INDEFINITE);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 250, "scaled shrink, unequal factors -> 250");
    ck_eq(byidx(&out, 1)->main_size, 50,  "scaled shrink, unequal factors -> 50");
    layout_flex_free(&out);

    /* THE LOOP, not one pass. 100px container; A flex: 0 1 100px, B the same
     * but with min-width:80px.
     *   pass 1: free -100; scaled factors 100/100 -> both to 50. B violates its
     *           minimum and is clamped to 80: a MIN violation, total +30 > 0,
     *           so B freezes at 80 and A does not.
     *   pass 2: remaining free = 100 - (80 + 100) = -80, all of it now A's.
     *           A = 100 - 80 = 20.
     *   -> 20 and 80. A single-pass implementation stops at 50 and 80 and
     *   overflows the container by 30px. */
    st_init(&a); st_flex(&a, 0, 1, 100);
    st_init(&b); st_flex(&b, 0, 1, 100); b.has_min_w = 1; b.min_w = 80;
    mk_item(&in[0], &a, 0, 0, 0); mk_item(&in[1], &b, 0, 0, 0);
    ct = mk_container(&cs, 100, FLEX_INDEFINITE);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 20, "freeze-and-redistribute: unclamped item -> 20");
    ck_eq(byidx(&out, 1)->main_size, 80, "freeze-and-redistribute: clamped item -> 80");
    ck_eq(byidx(&out, 0)->main_size + byidx(&out, 1)->main_size, 100,
          "...and the line still fits exactly");
    layout_flex_free(&out);

    /* The same, growing into a max violation. 300px container, three
     * flex: 1 1 0 items, the middle one capped at 50px.
     *   pass 1: 100 each; B clamps to 50 -- a MAX violation, total -50 < 0, so
     *           B freezes at 50.
     *   pass 2: remaining 250 over two items -> 125 each.
     *   -> 125 / 50 / 125. One pass would leave 100 / 50 / 100 = 250. */
    st_init(&a); st_flex(&a, 1, 1, 0);
    st_init(&b); st_flex(&b, 1, 1, 0); b.has_max_w = 1; b.max_w = 50;
    st_init(&c); st_flex(&c, 1, 1, 0);
    mk_item(&in[0], &a, 0, 0, 0); mk_item(&in[1], &b, 0, 0, 0); mk_item(&in[2], &c, 0, 0, 0);
    ct = mk_container(&cs, 300, FLEX_INDEFINITE);
    layout_flex_run(&ct, in, 3, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 125, "max violation redistributed -> 125");
    ck_eq(byidx(&out, 1)->main_size, 50,  "...the capped item stays at 50");
    ck_eq(byidx(&out, 2)->main_size, 125, "...and the third gets its share too");
    layout_flex_free(&out);

    /* "If the sum of the unfrozen flex items' flex factors is less than one,
     * multiply the initial free space by this sum." Two items, flex-grow 0.25
     * each, basis 0, in 500px. Sum of factors 0.5 < 1, so only 250px is
     * distributable, 125 to each -- the container is left half empty ON
     * PURPOSE, which is what fractional flex factors are for. */
    st_init(&a); st_flex(&a, 0.25, 1, 0);
    st_init(&b); st_flex(&b, 0.25, 1, 0);
    mk_item(&in[0], &a, 0, 0, 0); mk_item(&in[1], &b, 0, 0, 0);
    ct = mk_container(&cs, 500, FLEX_INDEFINITE);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 125, "sum of flex factors < 1 -> only half distributed");
    ck_eq(byidx(&out, 1)->main_size, 125, "...to both items equally");
    layout_flex_free(&out);

    /* flex-shrink:0 refuses to shrink; the line overflows and that is correct. */
    st_init(&a); st_flex(&a, 0, 0, 80);
    st_init(&b); st_flex(&b, 0, 0, 80);
    mk_item(&in[0], &a, 0, 0, 0); mk_item(&in[1], &b, 0, 0, 0);
    ct = mk_container(&cs, 100, FLEX_INDEFINITE);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 80, "flex-shrink:0 does not shrink");
    ck_eq(byidx(&out, 1)->main_size, 80, "...for either item");
    layout_flex_free(&out);

    /* Rounding: three flex:1 1 0 items in 100px are 33.33 each. Integer pixels
     * cannot hold that, so the line must still add up: 34 + 33 + 33. Plain
     * per-item rounding gives 33+33+33 and leaves the container a pixel short,
     * which is visible as a gap at the right edge of every such row. */
    st_init(&a); st_flex(&a, 1, 1, 0);
    mk_item(&in[0], &a, 0, 0, 0); mk_item(&in[1], &a, 0, 0, 0); mk_item(&in[2], &a, 0, 0, 0);
    ct = mk_container(&cs, 100, FLEX_INDEFINITE);
    layout_flex_run(&ct, in, 3, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size + byidx(&out, 1)->main_size +
          byidx(&out, 2)->main_size, 100, "3 x 1/3 of 100px still totals 100");
    ck_eq(byidx(&out, 0)->main_size, 34, "...the pixel goes to the first item");
    layout_flex_free(&out);

    /* An indefinite main size: nothing to distribute, so every item lands on
     * its hypothetical main size and the container reports their sum. */
    st_init(&a); st_flex(&a, 1, 1, 60);
    st_init(&b); st_flex(&b, 1, 1, 40);
    mk_item(&in[0], &a, 0, 0, 0); mk_item(&in[1], &b, 0, 0, 0);
    ct = mk_container(&cs, FLEX_INDEFINITE, FLEX_INDEFINITE);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 60, "indefinite main: no growth, item 1");
    ck_eq(byidx(&out, 1)->main_size, 40, "indefinite main: no growth, item 2");
    ck_eq(out.main_size, 100, "indefinite main: container is its max-content size");
    layout_flex_free(&out);

    /* Margins, borders and padding are OUTER sizes and come out of the free
     * space before anything flexes. 300px container, two flex:1 1 0 items, the
     * first with 10px margins each side and a 5px border each side.
     * Free space = 300 - 30 = 270 -> 135 each. */
    st_init(&a); st_flex(&a, 1, 1, 0); a.ml = a.mr = 10;
    a.border_w[1] = a.border_w[3] = 5;
    st_init(&b); st_flex(&b, 1, 1, 0);
    mk_item(&in[0], &a, 0, 0, 0); mk_item(&in[1], &b, 0, 0, 0);
    ct = mk_container(&cs, 300, FLEX_INDEFINITE);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 135, "margins+borders leave the flex space, item 1");
    ck_eq(byidx(&out, 1)->main_size, 135, "margins+borders leave the flex space, item 2");
    ck_eq(byidx(&out, 0)->x, 10, "...and the first item starts after its margin");
    ck_eq(byidx(&out, 1)->x, 10 + 10 + 135 + 10, "...the second after the first's box");
    layout_flex_free(&out);
    (void)d;
}

/* ================================================================ *
 * § 4.5  the automatic minimum size (min-width:auto on a flex item)
 * ================================================================ */

static void t_auto_min(void)
{
    group = "4.5 min-width:auto";
    struct cstyle cs; st_init(&cs); cs.display = DISP_FLEX;
    struct cstyle a, b;
    struct flex_item_in in[2];
    struct flex_out out;
    struct flex_in ct;

    /* Two flex: 0 1 100px items in a 100px container would shrink to 50 each,
     * but min-width:auto floors each at its MIN-CONTENT size (60 here), so the
     * row overflows instead. This is why a long word or a wide image pushes a
     * flex row wider rather than being squeezed out of existence. */
    st_init(&a); st_flex(&a, 0, 1, 100);
    mk_item(&in[0], &a, 0, 60, 100); mk_item(&in[1], &a, 0, 60, 100);
    ct = mk_container(&cs, 100, FLEX_INDEFINITE);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 60, "auto minimum floors at min-content");
    ck_eq(byidx(&out, 1)->main_size, 60, "...for both items, overflowing the row");
    layout_flex_free(&out);

    /* overflow != visible turns the automatic minimum off -- one of the two
     * standard escapes. Now they shrink to 50 each. */
    st_init(&b); st_flex(&b, 0, 1, 100); b.overflow_x = OVF_HIDDEN;
    mk_item(&in[0], &b, 0, 60, 100); mk_item(&in[1], &b, 0, 60, 100);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 50, "overflow:hidden switches the auto minimum off");
    ck_eq(byidx(&out, 1)->main_size, 50, "...for both items");
    layout_flex_free(&out);

    /* An explicit min-width:0 is the other escape. */
    st_init(&b); st_flex(&b, 0, 1, 100); b.has_min_w = 1; b.min_w = 0;
    mk_item(&in[0], &b, 0, 60, 100); mk_item(&in[1], &b, 0, 60, 100);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 50, "min-width:0 switches the auto minimum off");
    layout_flex_free(&out);

    /* "capped by the specified size suggestion": a definite width caps the
     * content-based minimum, so an item with width:30px never claims 60px of
     * minimum. Container 40, two items -> free -160, both driven to 20, both
     * clamped up to 30 (not 60). */
    st_init(&b); st_flex(&b, 0, 1, 100); b.has_w = 1; b.width = 30;
    mk_item(&in[0], &b, 0, 60, 100); mk_item(&in[1], &b, 0, 60, 100);
    ct = mk_container(&cs, 40, FLEX_INDEFINITE);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 30, "the specified size suggestion caps the minimum");
    ck_eq(byidx(&out, 1)->main_size, 30, "...for both items");
    layout_flex_free(&out);

    /* A replaced item takes the SMALLER of the content and transferred size
     * suggestions; a non-replaced item takes the larger. Item: 2:1 aspect
     * ratio, height:40 (so the transferred suggestion is 80), min-content 60.
     *   replaced     -> min( 80, 60 ) = 60
     *   non-replaced -> max( 80, 60 ) = 80
     * Squeezed hard enough, the two land in different places. */
    st_init(&b); st_flex(&b, 0, 1, 200); b.has_h = 1; b.height = 40;
    mk_item(&in[0], &b, 0, 60, 200); in[0].ratio_w = 2; in[0].ratio_h = 1; in[0].replaced = 1;
    mk_item(&in[1], &b, 0, 60, 200); in[1].ratio_w = 2; in[1].ratio_h = 1; in[1].replaced = 0;
    ct = mk_container(&cs, 20, FLEX_INDEFINITE);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 60, "replaced: the smaller suggestion wins");
    ck_eq(byidx(&out, 1)->main_size, 80, "non-replaced: the larger suggestion wins");
    layout_flex_free(&out);
}

/* ================================================================ *
 * § 9.3 step 5  collecting items into flex lines
 * ================================================================ */

static void t_lines(void)
{
    group = "9.3 line collection";
    struct cstyle cs; st_init(&cs); cs.display = DISP_FLEX; cs.flex_wrap = FWRAP_WRAP;
    struct cstyle a; st_init(&a); st_flex(&a, 0, 0, 100);
    struct flex_item_in in[4];
    struct flex_out out;
    struct flex_in ct;

    /* 300px container, four 100px items. Three fit EXACTLY (the test is "would
     * not fit", so an exact fit still fits); the fourth starts a second line. */
    for (int i = 0; i < 4; i++) mk_item(&in[i], &a, 0, 0, 0);
    ct = mk_container(&cs, 300, FLEX_INDEFINITE);
    layout_flex_run(&ct, in, 4, &MET, &out);
    ck_eq(out.nlines, 2, "4 x 100px in 300px -> 2 lines");
    ck_eq(byidx(&out, 2)->line, 0, "the third item still fits line 0 exactly");
    ck_eq(byidx(&out, 3)->line, 1, "the fourth starts line 1");
    layout_flex_free(&out);

    /* The gap counts towards the line's occupancy: with a 20px column-gap only
     * two items fit (100 + 20 + 100 = 220; a third would need 340). */
    cs.grid_gap_x = 20;
    layout_flex_run(&ct, in, 4, &MET, &out);
    ck_eq(out.nlines, 2, "gap:20 -> 2 lines of 2");
    ck_eq(byidx(&out, 1)->line, 0, "items 0,1 on line 0");
    ck_eq(byidx(&out, 2)->line, 1, "item 2 moves to line 1");
    ck_eq(byidx(&out, 1)->main_pos, 120, "...and the gap is real: item 1 at 120");
    layout_flex_free(&out);
    cs.grid_gap_x = 0;

    /* An item wider than the whole container still gets a line of its own
     * rather than no line: "If the very first uncollected item wouldn't fit,
     * collect just it into the line." */
    struct cstyle big; st_init(&big); st_flex(&big, 0, 0, 500);
    mk_item(&in[0], &big, 0, 0, 0); mk_item(&in[1], &a, 0, 0, 0);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(out.nlines, 2, "an oversized item takes a line by itself");
    ck_eq(byidx(&out, 0)->main_size, 500, "...at its full size");
    layout_flex_free(&out);

    /* nowrap does not wrap -- it shrinks. Four 100px items (default shrink 1)
     * in 300px: free -100, equal bases, so 25px off each -> 75. */
    cs.flex_wrap = FWRAP_NOWRAP;
    struct cstyle sh; st_init(&sh); st_flex(&sh, 0, 1, 100);
    for (int i = 0; i < 4; i++) mk_item(&in[i], &sh, 0, 0, 0);
    layout_flex_run(&ct, in, 4, &MET, &out);
    ck_eq(out.nlines, 1, "nowrap -> one line");
    ck_eq(byidx(&out, 0)->main_size, 75, "nowrap shrinks instead of wrapping");
    ck_eq(byidx(&out, 3)->main_size, 75, "...every item equally, bases being equal");
    layout_flex_free(&out);
}

/* ================================================================ *
 * § 9.4 / § 9.6  cross sizing and alignment
 * ================================================================ */

static void t_cross(void)
{
    group = "9.4 cross";
    struct cstyle cs; st_init(&cs); cs.display = DISP_FLEX;
    struct cstyle a, b;
    struct flex_item_in in[4];
    struct flex_out out;
    struct flex_in ct;
    struct tref r40 = { 40, 0, -1 };

    /* align-items:stretch (the initial value) with a definite container cross
     * size: the item takes the whole line even though it measures 40. */
    st_init(&a); st_flex(&a, 0, 0, 100);
    mk_item(&in[0], &a, &r40, 0, 0);
    ct = mk_container(&cs, 500, 200);
    layout_flex_run(&ct, in, 1, &MET, &out);
    ck_eq(byidx(&out, 0)->cross_size, 200, "stretch fills the line's cross size");
    ck_eq(byidx(&out, 0)->cross_pos, 0, "...from the cross-start edge");
    layout_flex_free(&out);

    /* Stretch does NOT apply to an item with a definite cross size. */
    st_init(&b); st_flex(&b, 0, 0, 100); b.has_h = 1; b.height = 40;
    mk_item(&in[0], &b, &r40, 0, 0);
    layout_flex_run(&ct, in, 1, &MET, &out);
    ck_eq(byidx(&out, 0)->cross_size, 40, "a definite height is not stretched");
    layout_flex_free(&out);

    /* flex-start / flex-end / center inside a 200px line, item 40 tall. */
    cs.align_items = AL_END;
    layout_flex_run(&ct, in, 1, &MET, &out);
    ck_eq(byidx(&out, 0)->cross_pos, 160, "align-items:flex-end -> 200-40");
    ck_eq(byidx(&out, 0)->y, 160, "...and physically at y=160");
    layout_flex_free(&out);
    cs.align_items = AL_CENTER;
    layout_flex_run(&ct, in, 1, &MET, &out);
    ck_eq(byidx(&out, 0)->cross_pos, 80, "align-items:center -> (200-40)/2");
    layout_flex_free(&out);
    cs.align_items = AL_START;
    layout_flex_run(&ct, in, 1, &MET, &out);
    ck_eq(byidx(&out, 0)->cross_pos, 0, "align-items:flex-start -> 0");
    layout_flex_free(&out);

    /* align-self overrides align-items for one item. */
    cs.align_items = AL_START;
    struct cstyle c2; st_init(&c2); st_flex(&c2, 0, 0, 100);
    c2.has_h = 1; c2.height = 40; c2.align_self = AL_END;
    mk_item(&in[0], &b,  &r40, 0, 0);
    mk_item(&in[1], &c2, &r40, 0, 0);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->cross_pos, 0,   "align-items:flex-start for the plain item");
    ck_eq(byidx(&out, 1)->cross_pos, 160, "align-self:flex-end overrides it");
    layout_flex_free(&out);

    /* An item's cross size comes from the measurement at its USED main size,
     * not at its base size. A text-shaped item of area 4000 in a 100px-wide
     * slot is 40 tall; grown to 200 wide it is 20 tall. */
    group = "9.4 cross depends on used main";
    struct tref area = { 0, 4000, -1 };
    st_init(&a); st_flex(&a, 1, 1, 100);
    mk_item(&in[0], &a, &area, 0, 0);
    ct = mk_container(&cs, 200, FLEX_INDEFINITE);
    layout_flex_run(&ct, in, 1, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 200, "the item grows to 200");
    ck_eq(byidx(&out, 0)->cross_size, 20, "...and is measured AT 200, not at 100");
    ck_eq(out.cross_size, 20, "the container's cross size follows");
    layout_flex_free(&out);

    /* BASELINE ALIGNMENT. Two items, both align-self:baseline, in a container
     * with no definite cross size.
     *   A: no padding, 20 tall, baseline 16 from its border-box top.
     *   B: 10px padding-top, 20 tall (outer 30), baseline 26.
     *   ascents 16 and 26 -> max 26.  descents 20-16=4 and 30-26=4 -> max 4.
     *   line cross size = 26 + 4 = 30 -- TALLER than either item's outer cross
     *   size taken alone, which is the whole point of the two-part measurement.
     *   A sits at 26-16 = 10, B at 26-26 = 0, so their baselines coincide. */
    group = "9.4 baseline";
    struct tref ba = { 20, 0, 16 }, bb = { 20, 0, 26 };
    st_init(&a); st_flex(&a, 0, 0, 50); a.align_self = AL_BASELINE;
    st_init(&b); st_flex(&b, 0, 0, 50); b.align_self = AL_BASELINE; b.pt = 10;
    mk_item(&in[0], &a, &ba, 0, 0); mk_item(&in[1], &b, &bb, 0, 0);
    ct = mk_container(&cs, 500, FLEX_INDEFINITE);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(out.line_cross[0], 30, "baseline line cross = max ascent + max descent");
    ck_eq(byidx(&out, 0)->cross_pos, 10, "the shallower item drops to the baseline");
    ck_eq(byidx(&out, 1)->cross_pos, 0,  "the deeper item sets it");
    ck_eq(byidx(&out, 0)->cross_pos + 16, byidx(&out, 1)->cross_pos + 26,
          "the two baselines coincide");
    layout_flex_free(&out);

    /* An item that reports no baseline falls back to flex-start rather than
     * being dropped from the line's measurement. */
    struct tref nob = { 50, 0, -1 };
    st_init(&b); st_flex(&b, 0, 0, 50); b.align_self = AL_BASELINE;
    mk_item(&in[0], &a, &ba, 0, 0); mk_item(&in[1], &b, &nob, 0, 0);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 1)->cross_pos, 0, "no baseline -> flex-start");
    ck_eq(out.line_cross[0], 50, "...and it still contributes its height to the line");
    layout_flex_free(&out);

    /* align-content. Two lines of 50 in a 200px-tall container.
     *   stretch (initial): each line grows to 100, so the lines start at 0/100.
     *   center:            lines stay 50, free 100 -> both offset by 50.
     *   space-between:     line 0 at 0, line 1 at 50 + 100 = 150. */
    group = "9.4 align-content";
    struct cstyle cw; st_init(&cw); cw.display = DISP_FLEX; cw.flex_wrap = FWRAP_WRAP;
    struct cstyle it4; st_init(&it4); st_flex(&it4, 0, 0, 100);
    it4.has_h = 1; it4.height = 50;
    for (int i = 0; i < 4; i++) mk_item(&in[i], &it4, &r40, 0, 0);
    ct = mk_container(&cw, 200, 200);
    layout_flex_run(&ct, in, 4, &MET, &out);
    ck_eq(out.nlines, 2, "two lines");
    ck_eq(out.line_cross[0], 100, "align-content:stretch grows line 0");
    ck_eq(out.line_cross[1], 100, "...and line 1");
    ck_eq(out.line_pos[1], 100, "line 1 starts at 100");
    layout_flex_free(&out);

    ct.align_content_space = JC_CENTER;
    layout_flex_run(&ct, in, 4, &MET, &out);
    ck_eq(out.line_cross[0], 50, "align-content:center does not stretch the lines");
    ck_eq(out.line_pos[0], 50, "...it offsets them: line 0 at 50");
    ck_eq(out.line_pos[1], 100, "...line 1 at 100");
    layout_flex_free(&out);

    ct.align_content_space = JC_BETWEEN;
    layout_flex_run(&ct, in, 4, &MET, &out);
    ck_eq(out.line_pos[0], 0,   "align-content:space-between pins line 0");
    ck_eq(out.line_pos[1], 150, "...and pushes line 1 to the far edge");
    layout_flex_free(&out);

    /* wrap-reverse puts the FIRST line at the physically last cross position
     * and nothing else changes: the logical positions are identical. */
    group = "9.4 wrap-reverse";
    cw.flex_wrap = FWRAP_WRAP_REV;
    ct = mk_container(&cw, 200, 200);
    ct.align_content_space = JC_START;
    layout_flex_run(&ct, in, 4, &MET, &out);
    ck_eq(byidx(&out, 0)->line, 0, "item 0 is still on line 0");
    ck_eq(byidx(&out, 0)->cross_pos, 0, "...at logical cross position 0");
    ck_eq(byidx(&out, 0)->y, 150, "...which is PHYSICALLY the bottom line");
    ck_eq(byidx(&out, 2)->y, 100, "...and line 1 sits above it");
    layout_flex_free(&out);
}

/* ================================================================ *
 * § 9.5  main-axis alignment
 * ================================================================ */

static void t_justify(void)
{
    group = "9.5 justify-content";
    struct cstyle cs; st_init(&cs); cs.display = DISP_FLEX;
    struct cstyle a; st_init(&a); st_flex(&a, 0, 0, 100);
    struct flex_item_in in[2];
    struct flex_out out;
    struct flex_in ct;
    mk_item(&in[0], &a, 0, 0, 0); mk_item(&in[1], &a, 0, 0, 0);

    /* 500px container, two 100px items -> 300px of free space. */
    struct { int mode; int p0, p1; const char *name; } cases[] = {
        { JC_START,   0,   100, "flex-start"    },
        { JC_END,     300, 400, "flex-end"      },
        { JC_CENTER,  150, 250, "center"        },
        { JC_BETWEEN, 0,   400, "space-between" },
        /* space-around: half a share at each end, a full share between.
         * 300/4 = 75 -> 75 | 100 | 125 | 100 | 75 */
        { JC_AROUND,  75,  325, "space-around"  },
        /* space-evenly: three equal 100px gaps. */
        { JC_EVENLY,  100, 300, "space-evenly"  },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cs.justify = cases[i].mode;
        ct = mk_container(&cs, 500, FLEX_INDEFINITE);
        layout_flex_run(&ct, in, 2, &MET, &out);
        char buf[96];
        snprintf(buf, sizeof buf, "justify-content:%s item 0", cases[i].name);
        ck_eq(byidx(&out, 0)->main_pos, cases[i].p0, buf);
        snprintf(buf, sizeof buf, "justify-content:%s item 1", cases[i].name);
        ck_eq(byidx(&out, 1)->main_pos, cases[i].p1, buf);
        layout_flex_free(&out);
    }

    /* A SINGLE item. css-align's fallbacks: space-between behaves as start,
     * space-around and space-evenly as center. */
    cs.justify = JC_BETWEEN;
    ct = mk_container(&cs, 500, FLEX_INDEFINITE);
    layout_flex_run(&ct, in, 1, &MET, &out);
    ck_eq(byidx(&out, 0)->main_pos, 0, "space-between with one item -> flex-start");
    layout_flex_free(&out);
    cs.justify = JC_AROUND;
    layout_flex_run(&ct, in, 1, &MET, &out);
    ck_eq(byidx(&out, 0)->main_pos, 200, "space-around with one item -> centre");
    layout_flex_free(&out);
    cs.justify = JC_EVENLY;
    layout_flex_run(&ct, in, 1, &MET, &out);
    ck_eq(byidx(&out, 0)->main_pos, 200, "space-evenly with one item -> centre");
    layout_flex_free(&out);

    /* OVERFLOW (negative free space). Two 300px unshrinkable items in 500px:
     * free = -100. space-between falls back to start, so the overflow is all at
     * the end; center splits it, so the line hangs 50px off each side. */
    struct cstyle big; st_init(&big); st_flex(&big, 0, 0, 300);
    mk_item(&in[0], &big, 0, 0, 0); mk_item(&in[1], &big, 0, 0, 0);
    cs.justify = JC_BETWEEN;
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->main_pos, 0,   "overflow + space-between -> start, item 0");
    ck_eq(byidx(&out, 1)->main_pos, 300, "overflow + space-between -> start, item 1");
    layout_flex_free(&out);
    cs.justify = JC_CENTER;
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->main_pos, -50, "overflow + center hangs off both edges");
    layout_flex_free(&out);
    cs.justify = JC_AROUND;
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->main_pos, -50, "overflow + space-around -> centre");
    layout_flex_free(&out);

    /* An `auto` main-axis margin eats ALL the positive free space before
     * justify-content sees any of it -- this is `margin-left:auto` pushing one
     * item to the right, the standard navigation-bar idiom. */
    group = "9.5 auto margins";
    struct cstyle am; st_init(&am); st_flex(&am, 0, 0, 100); am.ml = -1;   /* auto */
    cs.justify = JC_START;
    mk_item(&in[0], &a,  0, 0, 0);
    mk_item(&in[1], &am, 0, 0, 0);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->main_pos, 0,   "the plain item stays put");
    ck_eq(byidx(&out, 1)->main_pos, 400, "margin-left:auto pushes the second to the end");
    layout_flex_free(&out);

    /* Two auto margins on one item centre it, and justify-content is again
     * left with nothing. */
    st_init(&am); st_flex(&am, 0, 0, 100); am.ml = -1; am.mr = -1;
    cs.justify = JC_END;
    mk_item(&in[0], &am, 0, 0, 0);
    layout_flex_run(&ct, in, 1, &MET, &out);
    ck_eq(byidx(&out, 0)->main_pos, 200, "two auto margins centre the item");
    layout_flex_free(&out);
}

/* ================================================================ *
 * § 5  order, direction, and the physical mapping
 * ================================================================ */

static void t_order_dir(void)
{
    group = "5.4 order";
    struct cstyle cs; st_init(&cs); cs.display = DISP_FLEX;
    struct cstyle a, b, c;
    struct flex_item_in in[3];
    struct flex_out out;
    struct flex_in ct;

    /* `order` reorders the items; the output comes back in order-modified
     * order and the positions follow it. */
    st_init(&a); st_flex(&a, 0, 0, 100); a.order = 2;
    st_init(&b); st_flex(&b, 0, 0, 100); b.order = 1;
    st_init(&c); st_flex(&c, 0, 0, 100); c.order = 0;
    mk_item(&in[0], &a, 0, 0, 0); mk_item(&in[1], &b, 0, 0, 0); mk_item(&in[2], &c, 0, 0, 0);
    ct = mk_container(&cs, 500, FLEX_INDEFINITE);
    layout_flex_run(&ct, in, 3, &MET, &out);
    ck_eq(out.items[0].idx, 2, "order:0 comes first");
    ck_eq(out.items[2].idx, 0, "order:2 comes last");
    ck_eq(byidx(&out, 2)->main_pos, 0,   "...and is placed first");
    ck_eq(byidx(&out, 0)->main_pos, 200, "...while the document-first item is placed last");
    layout_flex_free(&out);

    /* Equal `order` values keep document order (the sort must be stable). */
    st_init(&a); st_flex(&a, 0, 0, 100); a.order = 1;
    mk_item(&in[0], &a, 0, 0, 0); mk_item(&in[1], &a, 0, 0, 0); mk_item(&in[2], &a, 0, 0, 0);
    layout_flex_run(&ct, in, 3, &MET, &out);
    ck_eq(out.items[0].idx, 0, "equal order is stable, first");
    ck_eq(out.items[2].idx, 2, "equal order is stable, last");
    layout_flex_free(&out);

    group = "5.1 flex-direction";
    st_init(&a); st_flex(&a, 0, 0, 100);
    struct tref r30 = { 30, 0, -1 };
    mk_item(&in[0], &a, &r30, 0, 0); mk_item(&in[1], &a, &r30, 0, 0);

    /* row: main is +x. */
    cs.flex_dir = FDIR_ROW; cs.align_items = AL_START;
    ct = mk_container(&cs, 500, FLEX_INDEFINITE);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->x, 0,   "row: item 0 at x=0");
    ck_eq(byidx(&out, 1)->x, 100, "row: item 1 at x=100");
    ck_eq(byidx(&out, 0)->w, 100, "row: main size is the width");
    ck_eq(byidx(&out, 0)->h, 30,  "row: cross size is the height");
    layout_flex_free(&out);

    /* row-reverse: the same logical positions, mirrored physically. */
    cs.flex_dir = FDIR_ROW_REV;
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->main_pos, 0, "row-reverse: logical position unchanged");
    ck_eq(byidx(&out, 0)->x, 400, "row-reverse: item 0 at the right edge");
    ck_eq(byidx(&out, 1)->x, 300, "row-reverse: item 1 to its left");
    layout_flex_free(&out);

    /* column: main is +y, so the sizes swap axes. */
    cs.flex_dir = FDIR_COL;
    ct = mk_container(&cs, 500, 200);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->y, 0,   "column: item 0 at y=0");
    ck_eq(byidx(&out, 1)->y, 100, "column: item 1 at y=100");
    ck_eq(byidx(&out, 0)->h, 100, "column: main size is the height");
    ck_eq(byidx(&out, 0)->x, 0,   "column: cross axis is x");
    layout_flex_free(&out);

    /* column-reverse mirrors the main axis, which is now vertical. */
    cs.flex_dir = FDIR_COL_REV;
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->y, 400, "column-reverse: item 0 at the bottom");
    ck_eq(byidx(&out, 1)->y, 300, "column-reverse: item 1 above it");
    layout_flex_free(&out);

    /* Row gap in a column container is the MAIN-axis gap (row-gap, grid_gap_y),
     * not column-gap. Getting these the wrong way round is invisible until a
     * container sets only one of them. */
    group = "gap";
    cs.flex_dir = FDIR_COL; cs.grid_gap_y = 20; cs.grid_gap_x = 0;
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 1)->y, 120, "column: row-gap separates the items");
    layout_flex_free(&out);
    cs.grid_gap_y = 0; cs.grid_gap_x = 20;
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 1)->y, 100, "column: column-gap does NOT");
    layout_flex_free(&out);

    /* Writing mode: in vertical-rl the inline axis is vertical, so a ROW flex
     * container runs down the page and its cross axis runs leftwards. */
    group = "writing mode";
    st_init(&cs); cs.display = DISP_FLEX; cs.align_items = AL_START;
    ct = mk_container(&cs, 500, 200);
    ct.wm = FLEX_WM_VERT_RL;
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->y, 0,   "vertical-rl row: main axis is vertical");
    ck_eq(byidx(&out, 1)->y, 100, "vertical-rl row: item 1 below item 0");
    ck_eq(byidx(&out, 0)->x, 200 - 30, "vertical-rl row: cross axis starts at the right");
    layout_flex_free(&out);

    /* direction:rtl reverses the main axis of a row without touching the
     * logical result. */
    ct = mk_container(&cs, 500, 200);
    ct.rtl = 1;
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->main_pos, 0, "rtl: logical position unchanged");
    ck_eq(byidx(&out, 0)->x, 400, "rtl row: item 0 at the right edge");
    layout_flex_free(&out);
}

/* ================================================================ *
 * § 4.1  absolutely positioned children
 * ================================================================ */

static void t_abspos(void)
{
    group = "4.1 abspos";
    struct cstyle cs; st_init(&cs); cs.display = DISP_FLEX;
    struct cstyle a, p;
    struct flex_item_in in[3];
    struct flex_out out;
    struct flex_in ct;

    st_init(&a); st_flex(&a, 1, 1, 0);
    st_init(&p); st_flex(&p, 0, 0, 100); p.position = POS_ABSOLUTE; p.pos_abs = 1;
    p.has_h = 1; p.height = 40;
    mk_item(&in[0], &a, 0, 0, 0);
    mk_item(&in[1], &p, 0, 0, 0); in[1].abspos = 1;
    mk_item(&in[2], &a, 0, 0, 0);
    ct = mk_container(&cs, 500, 200);
    layout_flex_run(&ct, in, 3, &MET, &out);

    /* The absolutely positioned child takes part in NOTHING: it is on no line,
     * and the two real items share all 500px as though it were not there. */
    ck_eq(byidx(&out, 1)->line, -1, "an abspos child is on no flex line");
    ck_eq(out.nlines, 1, "...and does not make a line of its own");
    ck_eq(byidx(&out, 0)->main_size, 250, "the real items share the whole container");
    ck_eq(byidx(&out, 2)->main_size, 250, "...both of them");
    /* Its static position is the one it would have as the SOLE flex item, so
     * justify-content:flex-start puts it at the container's content origin. */
    ck_eq(byidx(&out, 1)->main_pos, 0, "static position from justify-content:flex-start");
    layout_flex_free(&out);

    cs.justify = JC_CENTER; cs.align_items = AL_CENTER;
    layout_flex_run(&ct, in, 3, &MET, &out);
    ck_eq(byidx(&out, 1)->main_pos, 200, "justify-content:center -> (500-100)/2");
    ck_eq(byidx(&out, 1)->cross_pos, 80, "align-items:center -> (200-40)/2");
    layout_flex_free(&out);
}

/* ================================================================ *
 * An INDEPENDENT ORACLE: web-platform-tests' own expected values.
 *
 * Everything above this point is arithmetic done from the spec by the same
 * person who then wrote the implementation, which is exactly the way both come
 * to share a misreading. So these cases are transcribed from
 * third_party/wpt/css/css-flexbox/flex-factor-less-than-one.html, whose
 * `data-expected-width` attributes are the numbers Chrome and Firefox actually
 * produce. The expectations here were NOT computed by this file's author.
 *
 * They also happen to be the hardest corner of § 9.7 -- the "sum of the flex
 * factors is less than one" rule, which is what makes a `flex-grow: 0.5` item
 * take half the free space instead of all of it -- combined, in the last case,
 * with the automatic minimum size forcing a second pass of the loop.
 *
 * The container in that file is `width: 100px` with `border: 1px solid`, so its
 * INNER main size is 100 and the offsets it records are 1px larger than the
 * ones here.
 * ================================================================ */

static void t_wpt_oracle(void)
{
    group = "wpt flex-factor-less-than-one";
    struct cstyle cs; st_init(&cs); cs.display = DISP_FLEX;
    struct cstyle a, b;
    struct flex_item_in in[2];
    struct flex_out out;
    struct flex_in ct = mk_container(&cs, 100, FLEX_INDEFINITE);

    /* <div class="child-flex-grow-0-5" data-expected-width="50"> */
    st_init(&a); a.flex_grow = (int)(0.5 * 1024); a.flex_shrink = 1024;
    mk_item(&in[0], &a, 0, 0, 0);
    layout_flex_run(&ct, in, 1, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 50, "flex-grow:0.5 alone takes half the free space");
    layout_flex_free(&out);

    /* grow 0.5 + grow 0.25 -> 50 and 25 (sum of factors 0.75 < 1). */
    st_init(&b); b.flex_grow = (int)(0.25 * 1024); b.flex_shrink = 1024;
    mk_item(&in[0], &a, 0, 0, 0); mk_item(&in[1], &b, 0, 0, 0);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 50, "grow 0.5 of a 0.75 sum -> 50");
    ck_eq(byidx(&out, 1)->main_size, 25, "grow 0.25 of a 0.75 sum -> 25");
    layout_flex_free(&out);

    /* The same two with flex-basis:30px -> 50 and 40. */
    a.has_fb = 1; a.flex_basis = 30;
    b.has_fb = 1; b.flex_basis = 30;
    mk_item(&in[0], &a, 0, 0, 0); mk_item(&in[1], &b, 0, 0, 0);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 50, "basis 30 + grow 0.5 of 0.75 -> 50");
    ck_eq(byidx(&out, 1)->main_size, 40, "basis 30 + grow 0.25 of 0.75 -> 40");
    layout_flex_free(&out);

    /* <div class="child-flex-shrink-0-5" width:200> alone -> 150. */
    st_init(&a); a.flex_grow = 0; a.flex_shrink = (int)(0.5 * 1024);
    a.has_w = 1; a.width = 200;
    mk_item(&in[0], &a, 0, 0, 0);
    layout_flex_run(&ct, in, 1, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 150, "flex-shrink:0.5 alone gives back half");

    layout_flex_free(&out);

    /* shrink 0.5 and shrink 0.25, both width:200 -> 50 and 125. */
    st_init(&b); b.flex_grow = 0; b.flex_shrink = (int)(0.25 * 1024);
    b.has_w = 1; b.width = 200;
    mk_item(&in[0], &a, 0, 0, 0); mk_item(&in[1], &b, 0, 0, 0);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 50,  "shrink 0.5 of a 0.75 sum -> 50");
    ck_eq(byidx(&out, 1)->main_size, 125, "shrink 0.25 of a 0.75 sum -> 125");
    layout_flex_free(&out);

    /* The same two with flex-basis:100px -> 50 and 75. */
    a.has_fb = 1; a.flex_basis = 100;
    b.has_fb = 1; b.flex_basis = 100;
    mk_item(&in[0], &a, 0, 0, 0); mk_item(&in[1], &b, 0, 0, 0);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 50, "basis 100 + shrink 0.5 of 0.75 -> 50");
    ck_eq(byidx(&out, 1)->main_size, 75, "basis 100 + shrink 0.25 of 0.75 -> 75");
    layout_flex_free(&out);

    /* "Interaction of min-width:auto with fractional flex basis":
     *   grow 0.25 basis 0, empty          -> 10
     *   grow 0.75 basis 0, 90px of content -> 90
     * This one needs everything at once. Pass 1 distributes 25/75 (the factors
     * sum to exactly 1, so the <1 rule does NOT engage); the second item
     * violates its automatic minimum of 90 and freezes there; pass 2 has 10px
     * of free space and a single unfrozen item whose factor is 0.25, so NOW the
     * <1 rule engages -- and it must not apply, because 0.25 x the INITIAL free
     * space (25) is larger in magnitude than the 10 actually remaining. An
     * implementation that takes the scaled value unconditionally gives 25 here
     * and overflows the container. */
    st_init(&a); a.flex_grow = (int)(0.25 * 1024); a.flex_shrink = 1024;
    a.has_fb = 1; a.flex_basis = 0;
    st_init(&b); b.flex_grow = (int)(0.75 * 1024); b.flex_shrink = 1024;
    b.has_fb = 1; b.flex_basis = 0;
    mk_item(&in[0], &a, 0, 0, 0);
    mk_item(&in[1], &b, 0, 90, 90);
    layout_flex_run(&ct, in, 2, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 10, "fractional grow + auto minimum -> 10");
    ck_eq(byidx(&out, 1)->main_size, 90, "...and the content-bound item keeps 90");
    layout_flex_free(&out);

    /* The justify-content offsets from the same file, minus the container's
     * 1px border: a 50px item in 100px of free-ish space. */
    st_init(&a); a.flex_grow = (int)(0.5 * 1024); a.flex_shrink = 1024;
    mk_item(&in[0], &a, 0, 0, 0);
    cs.justify = JC_CENTER;
    layout_flex_run(&ct, in, 1, &MET, &out);
    ck_eq(byidx(&out, 0)->main_pos, 25, "wpt: justify-content:center offset 26 (-1 border)");
    layout_flex_free(&out);
    cs.justify = JC_END;
    layout_flex_run(&ct, in, 1, &MET, &out);
    ck_eq(byidx(&out, 0)->main_pos, 50, "wpt: justify-content:flex-end offset 51 (-1 border)");
    layout_flex_free(&out);
    cs.justify = JC_AROUND;
    layout_flex_run(&ct, in, 1, &MET, &out);
    ck_eq(byidx(&out, 0)->main_pos, 25, "wpt: space-around with one item centres it");
    layout_flex_free(&out);
}

/* ================================================================ *
 * degenerate inputs -- these must not crash and must not invent boxes
 * ================================================================ */

static void t_degenerate(void)
{
    group = "degenerate";
    struct cstyle cs; st_init(&cs); cs.display = DISP_FLEX;
    struct flex_out out;
    struct flex_in ct = mk_container(&cs, 500, 200);

    ck(layout_flex_run(&ct, 0, 0, &MET, &out) == 0, "zero items runs");
    ck_eq(out.nitems, 0, "zero items produce nothing");
    ck_eq(out.nlines, 0, "...and no lines");
    layout_flex_free(&out);

    /* A NULL item style is an anonymous flex item: every property at its
     * initial value, so flex: 0 1 auto and a base of its max-content size. */
    struct flex_item_in in[1];
    mk_item(&in[0], 0, 0, 20, 120);
    ck(layout_flex_run(&ct, in, 1, &MET, &out) == 0, "a NULL style runs");
    ck_eq(byidx(&out, 0)->main_size, 120, "anonymous item -> max-content base");
    layout_flex_free(&out);

    /* A zero-width container: items shrink to their automatic minimum, not
     * below it, and nothing goes negative. */
    ct = mk_container(&cs, 0, 0);
    struct cstyle a; st_init(&a); st_flex(&a, 0, 1, 100);
    mk_item(&in[0], &a, 0, 30, 100);
    layout_flex_run(&ct, in, 1, &MET, &out);
    ck_eq(byidx(&out, 0)->main_size, 30, "a zero-width container floors at the auto minimum");
    layout_flex_free(&out);
}

/* ================================================================ *
 * A randomised invariant sweep.
 *
 * § 9.7's loop terminates because every pass freezes at least one item. That is
 * an ARGUMENT, and an argument is not a tested fact -- a wrong freeze condition
 * gives an implementation that is right on every case above and spins forever
 * on some combination of clamps nobody thought to write down. So: 4000 random
 * configurations, run to completion, with the structural invariants checked on
 * each. If the loop can hang, this hangs, and a hung test is a failed test.
 *
 * The inputs are deterministic (a fixed-seed LCG), so a failure here is
 * reproducible rather than a story about a bad afternoon.
 * ================================================================ */

static unsigned long rng_s = 0x9E3779B97F4A7C15ULL;
static unsigned rnd(unsigned n)
{ rng_s = rng_s * 6364136223846793005ULL + 1442695040888963407ULL;
  return n ? (unsigned)((rng_s >> 33) % n) : 0; }

static void t_sweep(void)
{
    group = "invariant sweep";
    enum { N = 12 };
    struct cstyle cs, st[N];
    struct flex_item_in in[N];
    struct tref tr[N];
    struct flex_out out;
    int bad_run = 0, bad_count = 0, bad_line = 0, bad_neg = 0, bad_clamp = 0, bad_ord = 0;

    for (int iter = 0; iter < 4000; iter++) {
        int n = 1 + (int)rnd(N);
        st_init(&cs); cs.display = DISP_FLEX;
        cs.flex_dir  = (unsigned char)rnd(4);
        cs.flex_wrap = (unsigned char)rnd(3);
        cs.justify   = (unsigned char)rnd(6);
        cs.align_items = (unsigned char)rnd(5);
        cs.grid_gap_x = (int)rnd(30); cs.grid_gap_y = (int)rnd(30);

        for (int i = 0; i < n; i++) {
            st_init(&st[i]);
            st[i].flex_grow   = (int)rnd(4096);
            st[i].flex_shrink = (int)rnd(4096);
            if (rnd(2)) { st[i].has_fb = 1; st[i].flex_basis = (int)rnd(400); }
            if (rnd(3) == 0) { st[i].has_w = 1; st[i].width = (int)rnd(300); }
            if (rnd(3) == 0) { st[i].has_min_w = 1; st[i].min_w = (int)rnd(200); }
            if (rnd(3) == 0) { st[i].has_max_w = 1; st[i].max_w = (int)rnd(200); }
            if (rnd(3) == 0) { st[i].has_h = 1; st[i].height = (int)rnd(200); }
            if (rnd(4) == 0) st[i].align_self = (unsigned char)rnd(6);
            st[i].pl = (int)rnd(20); st[i].pr = (int)rnd(20);
            st[i].pt = (int)rnd(20); st[i].pb = (int)rnd(20);
            st[i].border_w[0] = st[i].border_w[2] = (int)rnd(6);
            st[i].border_w[1] = st[i].border_w[3] = (int)rnd(6);
            st[i].order = (int)rnd(5) - 2;
            tr[i].cross = (int)rnd(120); tr[i].area = 0;
            tr[i].baseline = rnd(2) ? (int)rnd(60) : -1;
            mk_item(&in[i], &st[i], &tr[i], (int)rnd(120), (int)rnd(400));
            if (rnd(16) == 0) in[i].abspos = 1;
        }
        struct flex_in ct = mk_container(&cs, rnd(4) ? (int)rnd(800) : FLEX_INDEFINITE,
                                              rnd(4) ? (int)rnd(600) : FLEX_INDEFINITE);
        if (rnd(6) == 0) ct.wm = (unsigned char)(1 + rnd(2));
        ct.rtl = (unsigned char)rnd(2);

        if (layout_flex_run(&ct, in, n, &MET, &out) != 0) { bad_run++; continue; }

        /* every input comes back exactly once */
        int seen[N]; memset(seen, 0, sizeof seen);
        if (out.nitems != n) bad_count++;
        for (int i = 0; i < out.nitems; i++) {
            const struct flex_item_out *o = &out.items[i];
            if (o->idx < 0 || o->idx >= n || seen[o->idx]) { bad_count++; break; }
            seen[o->idx] = 1;

            /* a flex item is on a real line; an abspos child is on none */
            if (in[o->idx].abspos) { if (o->line != -1) bad_line++; }
            else if (o->line < 0 || o->line >= out.nlines) bad_line++;

            /* no negative geometry ever escapes */
            if (o->main_size < 0 || o->cross_size < 0 || o->w < 0 || o->h < 0) bad_neg++;

            /* an explicit min-width/max-width is never violated (content-box
             * styles, so the used main size is directly comparable) */
            if (!in[o->idx].abspos && st[o->idx].display != DISP_NONE) {
                int horiz_main = (cs.flex_dir == FDIR_ROW || cs.flex_dir == FDIR_ROW_REV)
                                 == (ct.wm == FLEX_WM_HORIZ_TB);
                if (horiz_main) {
                    if (st[o->idx].has_min_w && o->main_size < st[o->idx].min_w) bad_clamp++;
                    if (st[o->idx].has_max_w && !st[o->idx].has_min_w &&
                        o->main_size > st[o->idx].max_w) bad_clamp++;
                }
            }
        }
        /* within a line, order-modified order is placement order */
        for (int i = 1; i < out.nitems; i++) {
            const struct flex_item_out *p = &out.items[i - 1], *q = &out.items[i];
            if (p->line >= 0 && p->line == q->line && q->main_pos < p->main_pos) bad_ord++;
        }
        layout_flex_free(&out);
    }
    ck_eq(bad_run,   0, "4000 random configurations all resolve");
    ck_eq(bad_count, 0, "every item comes back exactly once");
    ck_eq(bad_line,  0, "flex items land on a real line, abspos children on none");
    ck_eq(bad_neg,   0, "no negative size or extent escapes");
    ck_eq(bad_clamp, 0, "an explicit min-width/max-width is never violated");
    ck_eq(bad_ord,   0, "order-modified order is placement order within a line");
}

int main(void)
{
    printf("flex_test: CSS Flexbox layout algorithm\n");
    t_base_sizes();
    t_resolve();
    t_auto_min();
    t_lines();
    t_cross();
    t_justify();
    t_order_dir();
    t_abspos();
    t_wpt_oracle();
    t_degenerate();
    t_sweep();
    printf("%s: %d checks, %d failures\n",
           failures ? "FAILED" : "ok", checks, failures);
    return failures ? 1 : 0;
}
