/* CSS Grid: the track sizing algorithm.
 *
 * css-grid-1 s12.3 the algorithm's five steps, s12.4 initialize, s12.5 resolve
 * intrinsic sizes (including "distribute extra space across spanned tracks"),
 * s12.6 maximize, s12.7 expand flexible tracks + find the size of an fr,
 * s12.8 stretch auto tracks.
 *
 * THIS FILE IS THE NEGATIVE CONTROL'S TARGET. Built with
 * -DGRID_SPAN_EVEN_SPLIT, layout_grid.c replaces the spec's space distribution
 * with an even split of a spanning item's leftover contribution across the
 * tracks it spans. That build still produces sensible-looking track sizes with
 * plausible totals -- only the per-track split is wrong -- and these
 * assertions must go red on it. Cases marked SPANNING below are the ones that
 * do the discriminating; the rest stay green either way, which is the point:
 * a control that fails everything proves nothing.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "layout_grid.h"

static int checks, fails;

/* Size `tpl` against `items` and compare the whole resolved track list. */
static void sz(const char *what, const char *tpl, int avail, int gap, unsigned char align,
               const struct gtrackitem *items, int nit, const int *want, int nw)
{
    struct gtracklist t;
    int got[16], i, bad = 0;

    checks++;
    if (grid_parse_tracklist(tpl, -1, 16, &t) < 0) {
        fails++; printf("FAIL %s: `%s` did not parse\n", what, tpl);
        return;
    }
    if (t.n != nw) {
        fails++; printf("FAIL %s: `%s` gave %d tracks, want %d\n", what, tpl, t.n, nw);
        grid_tracklist_free(&t); return;
    }
    memset(got, 0, sizeof got);
    if (grid_size_tracks(&t, t.n, items, nit, avail, gap, align, got) < 0) {
        fails++; printf("FAIL %s: sizing failed\n", what);
        grid_tracklist_free(&t); return;
    }
    for (i = 0; i < nw; i++) if (got[i] != want[i]) bad = 1;
    if (bad) {
        fails++;
        printf("FAIL %s: `%s` avail=%d gap=%d -> [", what, tpl, avail, gap);
        for (i = 0; i < nw; i++) printf("%s%d", i ? ", " : "", got[i]);
        printf("], want [");
        for (i = 0; i < nw; i++) printf("%s%d", i ? ", " : "", want[i]);
        printf("]\n");
    }
    grid_tracklist_free(&t);
}

static void chk_i(const char *what, long got, long want)
{
    checks++;
    if (got != want) { fails++; printf("FAIL %s: got %ld, want %ld\n", what, got, want); }
}

/* An item occupying tracks [start, start+span) with the three contributions the
 * algorithm asks for. */
static struct gtrackitem mk(int start, int span, int minimum, int minc, int maxc)
{
    struct gtrackitem it;
    it.start = start; it.span = span;
    it.m.minimum = minimum; it.m.min_content = minc; it.m.max_content = maxc;
    return it;
}

/* ------------------------------------------------------------ fixed sizes -- */

static void t_fixed(void)
{
    int w2[2], w1[1], w3[3];

    /* s12.4: a fixed min fn is the initial base size, a fixed max fn the
     * initial growth limit. Nothing else moves them. */
    w2[0] = 100; w2[1] = 200;
    sz("fixed tracks", "100px 200px", 500, 0, GA_START, NULL, 0, w2, 2);

    /* Percentages resolve against the available grid space. */
    w2[0] = 200; w2[1] = 200;
    sz("percentage tracks", "50% 50%", 400, 0, GA_START, NULL, 0, w2, 2);

    /* s7.2.1: a percentage against an INDEFINITE available space behaves as
     * auto, so with no items the track is zero. */
    w1[0] = 0;
    sz("percentage vs indefinite space", "50%", GRID_INDEFINITE, 0, GA_START, NULL, 0, w1, 1);

    /* minmax with two fixed values: maximize takes the base up to the limit. */
    w1[0] = 200;
    sz("minmax(100px,200px)", "minmax(100px, 200px)", 1000, 0, GA_START, NULL, 0, w1, 1);

    /* ... but never past it, even with room to spare. */
    w3[0] = 100; w3[1] = 200; w3[2] = 50;
    sz("fixed tracks do not stretch", "100px minmax(0px,200px) 50px", 900, 0, GA_START,
       NULL, 0, w3, 3);
}

/* ----------------------------------------------------- flexible tracks ----- */

static void t_flex(void)
{
    int w2[2], w3[3];
    struct gtrackitem it[2];

    w2[0] = 150; w2[1] = 150;
    sz("1fr 1fr", "1fr 1fr", 300, 0, GA_START, NULL, 0, w2, 2);

    w3[0] = 100; w3[1] = 100; w3[2] = 200;
    sz("fixed + 1fr + 2fr", "100px 1fr 2fr", 400, 0, GA_START, NULL, 0, w3, 3);

    /* s12.7.1 step 1: the leftover space is the space to fill minus the base
     * sizes of the NON-flexible tracks -- gutters included. */
    w2[0] = 140; w2[1] = 140;
    sz("1fr 1fr with a 20px gap", "1fr 1fr", 300, 20, GA_START, NULL, 0, w2, 2);

    /* THE FLEX-FACTOR-SUM < 1 CASE (s7.2.4 + s12.7.1 step 2: "if this value is
     * less than 1, set it to 1 instead"). Two .25fr tracks request a quarter of
     * the leftover space each and get exactly that -- half the container is
     * left unfilled. Topping these up to fill would be the obvious bug. */
    w2[0] = 100; w2[1] = 100;
    sz("0.25fr 0.25fr leaves space unfilled", "0.25fr 0.25fr", 400, 0, GA_START,
       NULL, 0, w2, 2);

    /* Sums >= 1 are rebalanced to use exactly 100% of the leftover space. */
    w2[0] = 100; w2[1] = 200;
    sz("0.5fr 1fr", "0.5fr 1fr", 300, 0, GA_START, NULL, 0, w2, 2);

    /* Documented REMAINDER rule: an inexact division hands the leftover pixels
     * out one each to the earliest flexible tracks, so the parts sum to the
     * whole rather than losing a pixel per track. */
    w3[0] = 34; w3[1] = 33; w3[2] = 33;
    sz("1fr 1fr 1fr in 100px: remainder to the earliest track", "1fr 1fr 1fr",
       100, 0, GA_START, NULL, 0, w3, 3);

    /* s12.7.1 step 4: "If the product of the hypothetical fr size and a
     * flexible track's flex factor is less than the track's base size, restart
     * this algorithm treating all such tracks as inflexible."
     *
     * A 1fr track whose content needs 80px in a 100px container keeps its 80
     * and the OTHER 1fr track takes the remaining 20 -- they do not split 50/50.
     * This is the single most visible consequence of that step. */
    it[0] = mk(0, 1, 80, 80, 80);
    w2[0] = 80; w2[1] = 20;
    sz("a 1fr track floored by its content is frozen out of the fr split",
       "1fr 1fr", 100, 0, GA_START, it, 1, w2, 2);

    /* SPANNING (s12.5.4): an item crossing flexible tracks distributes into
     * them by the ratio of their flex factors, and with an indefinite available
     * space the flex fraction comes from the content. Two 1fr tracks under an
     * item whose max-content is 400 resolve to 200 each. */
    it[0] = mk(0, 2, 200, 200, 400);
    w2[0] = 200; w2[1] = 200;
    sz("item spanning two 1fr tracks, indefinite space", "1fr 1fr",
       GRID_INDEFINITE, 0, GA_START, it, 1, w2, 2);
}

/* -------------------------------------------------- intrinsic, span 1 ------ */

static void t_intrinsic1(void)
{
    int w1[1], w2[2];
    struct gtrackitem it[2];

    /* s12.5.2 "Size tracks to fit non-spanning items": a min-content min fn
     * takes the max of the items' min-content contributions; a max-content min
     * fn takes their max-content contributions. */
    it[0] = mk(0, 1, 20, 40, 100);
    it[1] = mk(1, 1, 20, 30, 90);
    w2[0] = 40; w2[1] = 90;
    sz("min-content and max-content tracks", "min-content max-content", 1000, 0, GA_START,
       it, 2, w2, 2);

    /* Two auto tracks. The order of s12.6 then s12.8 is visible here and is not
     * interchangeable: MAXIMIZE first grows each base to its growth limit (the
     * max-content contribution, 50 and 80), and only the space still left over
     * is then split EQUALLY by STRETCH -- 170/2 = 85 to each. Distributing the
     * whole 250 equally, or proportionally, gives different numbers. */
    it[0] = mk(0, 1, 20, 20, 50);
    it[1] = mk(1, 1, 30, 30, 80);
    w2[0] = 135; w2[1] = 165;
    sz("auto auto: maximize to growth limits, then stretch the remainder",
       "auto auto", 300, 0, GA_NORMAL, it, 2, w2, 2);

    /* justify-content other than normal/stretch suppresses s12.8 entirely, so
     * the tracks stay at their growth limits and the grid underfills. */
    w2[0] = 50; w2[1] = 80;
    sz("auto auto with justify-content: start -- no stretch step",
       "auto auto", 300, 0, GA_START, it, 2, w2, 2);

    /* s12.5.2 "For fit-content() maximums, furthermore clamp this growth limit
     * by the fit-content() argument." */
    it[0] = mk(0, 1, 20, 20, 200);
    w1[0] = 50;
    sz("fit-content(50px) clamps content that wants 200", "fit-content(50px)", 1000, 0,
       GA_START, it, 1, w1, 1);

    /* Below the argument, fit-content behaves as max-content. */
    it[0] = mk(0, 1, 20, 20, 30);
    w1[0] = 30;
    sz("fit-content(500px) below its argument is max-content", "fit-content(500px)", 1000, 0,
       GA_START, it, 1, w1, 1);

    /* fit-content beside a flexible track: the clamp holds and the fr takes
     * everything left. */
    it[0] = mk(0, 1, 20, 20, 200);
    w2[0] = 50; w2[1] = 250;
    sz("fit-content(50px) 1fr", "fit-content(50px) 1fr", 300, 0, GA_START, it, 1, w2, 2);

    /* An `auto` MIN track sizing function takes the item's MINIMUM
     * contribution, which is a different number from its min-content
     * contribution -- conflating the two is how `min-width: 0` stops working
     * inside a grid. Here the minimum is 10 while min-content is 40, and with
     * no room to grow (justify-content: start, container exactly the growth
     * limit) the track must show the growth limit, not the minimum. */
    it[0] = mk(0, 1, 10, 40, 60);
    w1[0] = 60;
    sz("auto track: growth limit is the max-content contribution", "auto", 1000, 0,
       GA_START, it, 1, w1, 1);

    /* minmax(auto, min-content): the growth limit caps the track at the
     * min-content contribution even though max-content is larger. */
    it[0] = mk(0, 1, 10, 40, 200);
    w1[0] = 40;
    sz("minmax(auto, min-content)", "minmax(auto, min-content)", 1000, 0, GA_START,
       it, 1, w1, 1);
}

/* ------------------------------------------- SPANNING items (s12.5.1) ------ */

static void t_spanning(void)
{
    int w2[2];
    struct gtrackitem it[2];

    /* THE SPEC'S OWN WORKED EXAMPLE. From the "Why does the infinitely growable
     * flag exist?" note in s12.5.1, quoting Peter Salas:
     *
     *   Two auto tracks (minmax(min-content, max-content) each).
     *   Item 1 is in track 1 and has min-content = max-content = 10.
     *   Item 2 spans tracks 1 and 2, min-content = 30, max-content = 100.
     *
     * The note spells out the two candidate outcomes for the growth limits and
     * says which one is correct: NOT [45, 55] from growing both equally, but
     * [10, 90] from growing only the second -- "which we considered a better
     * result because the first track remains sized exactly to the first item."
     *
     * With the container at 100px the growth limits are also the used sizes, so
     * this is directly checkable. An even split of the spanning item's
     * contribution produces the plausible-and-wrong answer instead. */
    it[0] = mk(0, 1, 10, 10, 10);
    it[1] = mk(0, 2, 30, 30, 100);
    w2[0] = 10; w2[1] = 90;
    sz("SPANNING: the spec's infinitely-growable example resolves to [10, 90]",
       "minmax(min-content, max-content) minmax(min-content, max-content)",
       100, 0, GA_START, it, 2, w2, 2);

    /* SPANNING: "Subtract the affected size of every spanned track (not just
     * the affected tracks) from the item's size contribution." The 100px track
     * is not affected -- it has fixed sizing functions -- but its size still
     * comes off the item's contribution, so the auto track absorbs exactly
     * 300 - 100 = 200, not half of 300. */
    it[0] = mk(0, 2, 50, 50, 300);
    w2[0] = 100; w2[1] = 200;
    sz("SPANNING: a fixed track's size comes off the contribution first",
       "100px auto", 1000, 0, GA_START, it, 1, w2, 2);

    /* SPANNING: the same shape for a min-content minimum. */
    it[0] = mk(0, 2, 250, 250, 250);
    w2[0] = 100; w2[1] = 150;
    sz("SPANNING: min-content minimum across fixed + min-content",
       "100px min-content", 1000, 0, GA_START, it, 1, w2, 2);

    /* SPANNING: "freezing a track's item-incurred increase as its affected size
     * + item-incurred increase reaches its limit (and continuing to grow the
     * unfrozen tracks as needed)". The first track's growth limit is 50, so of
     * the 200px to distribute it takes 50 and the auto track takes 150 -- an
     * even split would give 100/100 and violate the 50px limit. */
    it[0] = mk(0, 2, 200, 200, 200);
    w2[0] = 50; w2[1] = 150;
    sz("SPANNING: distribution freezes a track at its growth limit",
       "minmax(min-content, 50px) auto", 1000, 0, GA_START, it, 1, w2, 2);

    /* SPANNING: gutters are fixed empty tracks spanned by the item, so the gap
     * comes off the contribution before anything is distributed. 210 across two
     * auto tracks with a 10px gap is 100 each, not 105. */
    it[0] = mk(0, 2, 100, 100, 210);
    w2[0] = 100; w2[1] = 100;
    sz("SPANNING: the spanned gutter comes off the contribution",
       "auto auto", GRID_INDEFINITE, 10, GA_START, it, 1, w2, 2);

    /* A spanning item that already fits asks for nothing. */
    it[0] = mk(0, 1, 10, 10, 10);
    it[1] = mk(0, 2, 20, 20, 20);
    w2[0] = 100; w2[1] = 100;
    sz("a spanning item that fits changes nothing", "100px 100px", 200, 0, GA_START,
       it, 2, w2, 2);
}

/* --------------------------------------------------------------- edges ----- */

static void t_edges(void)
{
    int w1[1], w2[2];
    struct gtrackitem it[1];

    /* No tracks at all is not a crash. */
    {
        struct gtracklist t;
        int got[4];
        checks++;
        if (grid_parse_tracklist("none", -1, 16, &t) < 0 ||
            grid_size_tracks(&t, 0, NULL, 0, 300, 0, GA_START, got) < 0) {
            fails++; printf("FAIL empty track list must not fail\n");
        }
        grid_tracklist_free(&t);
    }

    /* An overflowing grid keeps its track sizes; free space is floored, not
     * negative-distributed. */
    w2[0] = 200; w2[1] = 200;
    sz("tracks larger than the container are not shrunk", "200px 200px", 100, 0, GA_START,
       NULL, 0, w2, 2);

    /* A track with no items and an intrinsic size resolves to zero, not to
     * infinity (s12.5 step 5: an infinite growth limit becomes the base size). */
    w1[0] = 0;
    sz("an empty auto track is zero wide", "auto", GRID_INDEFINITE, 0, GA_START,
       NULL, 0, w1, 1);

    /* Under a max-content constraint the free space is infinite, so every track
     * grows to its growth limit (s12.6). */
    it[0] = mk(0, 1, 10, 40, 60);
    w1[0] = 60;
    sz("max-content constraint grows every track to its growth limit", "auto",
       GRID_INDEFINITE, 0, GA_START, it, 1, w1, 1);

    /* s12.8 is skipped when the available space is indefinite -- there is no
     * definite free space to divide. */
    w2[0] = 60; w2[1] = 0;
    sz("no stretch step when the available space is indefinite", "auto auto",
       GRID_INDEFINITE, 0, GA_NORMAL, it, 1, w2, 2);
}

/* The sum of the resolved tracks plus the gutters must equal the container
 * whenever the tracks can fill it. This is what the REMAINDER rule buys, and it
 * is checked separately because an implementation that loses a pixel per track
 * still passes every individual size assertion above with numbers that look
 * right. */
static void t_exact_fill(void)
{
    static const struct { const char *tpl; int n, avail, gap; } cases[] = {
        { "1fr 1fr 1fr",        3, 100, 0 },
        { "1fr 1fr 1fr",        3, 101, 0 },
        { "1fr 1fr 1fr",        3, 700, 7 },
        { "1fr 2fr 3fr",        3, 1000, 0 },
        { "100px 1fr 1fr",      3, 333, 5 },
        { "repeat(7, 1fr)",     7, 1000, 3 },
    };
    unsigned c;
    for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        struct gtracklist t;
        int got[16], i, sum = 0;
        checks++;
        if (grid_parse_tracklist(cases[c].tpl, -1, 16, &t) < 0 ||
            grid_size_tracks(&t, t.n, NULL, 0, cases[c].avail, cases[c].gap,
                             GA_START, got) < 0) {
            fails++; printf("FAIL exact fill: `%s` failed\n", cases[c].tpl);
            grid_tracklist_free(&t); continue;
        }
        for (i = 0; i < t.n; i++) sum += got[i];
        sum += (t.n - 1) * cases[c].gap;
        if (sum != cases[c].avail) {
            fails++;
            printf("FAIL exact fill: `%s` in %d (gap %d) sums to %d\n",
                   cases[c].tpl, cases[c].avail, cases[c].gap, sum);
        }
        grid_tracklist_free(&t);
    }

    /* ... except where the spec says it must NOT fill: flex factors summing to
     * less than 1 deliberately leave space unused. */
    {
        struct gtracklist t;
        int got[4];
        grid_parse_tracklist("0.25fr 0.5fr", -1, 16, &t);
        grid_size_tracks(&t, t.n, NULL, 0, 400, 0, GA_START, got);
        chk_i("sub-1fr sum: track 0", got[0], 100);
        chk_i("sub-1fr sum: track 1", got[1], 200);
        grid_tracklist_free(&t);
    }
}

int main(void)
{
    t_fixed();
    t_flex();
    t_intrinsic1();
    t_spanning();
    t_edges();
    t_exact_fill();
    printf("grid_size_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
