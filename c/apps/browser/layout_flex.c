/* CSS Flexible Box Layout § 9, as numbers.
 *
 * READ layout_flex.h FIRST -- it states the frames of reference every number
 * below is expressed in, and that is where the mistakes are.
 *
 * The structure of this file follows the spec's own numbering, and the section
 * comments give the § so a reader can check a step against the text rather than
 * against the code's own claim about itself:
 *
 *   § 9.2  flex base size + hypothetical main size ....... item_base_size()
 *   § 4.5  automatic minimum size ........................ auto_min_main()
 *   § 9.3  collect into lines ............................ in layout_flex_run
 *   § 9.7  resolve the flexible lengths .................. resolve_line()
 *   § 9.4  cross sizes of items and of lines ............. in layout_flex_run
 *   § 9.5  main-axis alignment ........................... pack_offset() + ditto
 *   § 9.6  cross-axis alignment + align-content .......... ditto
 *
 * TWO THINGS ARE WORTH KNOWING BEFORE EDITING.
 *
 * 1. THE SCALED FLEX SHRINK FACTOR. Growing distributes free space in
 *    proportion to `flex-grow`. SHRINKING does NOT distribute in proportion to
 *    `flex-shrink`: it distributes in proportion to `flex-shrink` MULTIPLIED BY
 *    THE ITEM'S INNER FLEX BASE SIZE (§ 9.7 step 4b). The reason is physical --
 *    two items with the same shrink factor but different sizes must lose the
 *    same PROPORTION of themselves, not the same number of pixels, or the small
 *    one collapses to nothing while the big one is barely touched. An
 *    implementation that uses the raw factor produces plausible-looking layouts
 *    with correct totals and wrong individual sizes, which is why it is the
 *    negative control: build with -DFLEX_UNSCALED_SHRINK and
 *    tests/unit/flex_test.c must go red (`make test-flex-negctl`).
 *
 * 2. THE LOOP'S ORDER IS LOAD-BEARING. Freezing happens at the END of an
 *    iteration, over the sign of the TOTAL violation, not per item as
 *    violations are found. Freezing an item the moment it clamps gives a
 *    different answer, because the space that item releases has to be
 *    redistributed among items that were themselves clamped in the same pass.
 *    The total-violation rule is what makes the loop terminate (it always
 *    freezes at least one item) AND what makes it converge on the right answer.
 */

#include "layout_flex.h"

void *kmalloc(unsigned long);
void  kfree(void *);
void *memset(void *, int, unsigned long);

/* ---------------- fixed point ----------------
 *
 * The engine is integer-px, but § 9.7 is a chain of proportional divisions and
 * rounding at every step of it drifts by several pixels across a row of eight
 * items. So the loop runs in 16.16 and rounds ONCE, in round_line(), which also
 * repairs the total.
 *
 * `fxw` is the wide type the proportional step multiplies in: free space is
 * ~2^32 in 16.16 and a scaled flex shrink factor is ~2^37 (a 1024-scaled shrink
 * factor times a pixel base times an item count), and their product overflows
 * 64 bits on a page that really does set flex-shrink:900 on a 4000px item. */
typedef long long fx;
#define FX_ONE 65536
#define TOFX(x) ((fx)(x) * FX_ONE)
#if defined(__SIZEOF_INT128__)
typedef __int128 fxw;
#else
typedef long long fxw;      /* falls back to 64-bit; only extreme inputs differ */
#endif

/* css_fixed's 1.0. css.h documents flex-grow/flex-shrink as css_fixed. */
#define F_ONE 1024

#define BIGSIZE 0x3FFFFFFF          /* stands in for an absent max constraint */

/* ---------------- axis mapping (§ 5, Appendix A) ---------------- */

struct axis {
    int main_horiz;     /* the main axis is physically horizontal */
    int main_rev;       /* main-start sits at the HIGH physical coordinate */
    int cross_rev;      /* cross-start sits at the HIGH physical coordinate */
    int main_is_inline; /* main axis == the inline axis (flex-direction: row*) */
    int ms_edge, me_edge;   /* physical edge index of main-start / main-end */
    int cs_edge, ce_edge;   /* 0 = top, 1 = right, 2 = bottom, 3 = left */
};

static void edges(int horiz, int rev, int *s, int *e)
{
    if (horiz) { *s = rev ? 1 : 3; *e = rev ? 3 : 1; }
    else       { *s = rev ? 2 : 0; *e = rev ? 0 : 2; }
}

static void axis_setup(struct axis *a, int dir, int wrap, int wm, int rtl)
{
    int row = (dir == FDIR_ROW || dir == FDIR_ROW_REV);
    int rev = (dir == FDIR_ROW_REV || dir == FDIR_COL_REV);

    /* The inline axis is horizontal in horizontal-tb and vertical otherwise,
     * and runs in the `direction` sense either way. The block axis is the
     * other one, running downwards in horizontal-tb, leftwards in vertical-rl
     * and rightwards in vertical-lr. */
    int inline_horiz = (wm == FLEX_WM_HORIZ_TB), inline_neg = rtl ? 1 : 0;
    int block_horiz  = !inline_horiz,            block_neg  = (wm == FLEX_WM_VERT_RL);

    a->main_is_inline = row;
    a->main_horiz = row ? inline_horiz : block_horiz;
    a->main_rev   = (row ? inline_neg : block_neg) ^ (rev ? 1 : 0);
    /* The cross axis is whichever one the main axis is not; flipping it is the
     * ONLY thing wrap-reverse does. */
    { int ch = row ? block_horiz : inline_horiz;
      int cn = row ? block_neg   : inline_neg;
      cn ^= (wrap == FWRAP_WRAP_REV) ? 1 : 0;
      a->cross_rev = cn;
      edges(ch, cn, &a->cs_edge, &a->ce_edge); }
    edges(a->main_horiz, a->main_rev, &a->ms_edge, &a->me_edge);
}

/* ---------------- style accessors ---------------- */

/* A margin, by physical edge. cstyle spells `auto` as -1 -- which is also how
 * it would have to spell a -1px margin. That conflation is css_engine.c's and
 * predates this file; it is reported rather than papered over here. */
static int margin_edge(const struct cstyle *st, int e, unsigned char *isauto)
{
    int v = 0;
    if (st) v = (e == 0) ? st->mt : (e == 1) ? st->mr : (e == 2) ? st->mb : st->ml;
    if (v == -1) { *isauto = 1; return 0; }
    *isauto = 0;
    return v;
}

static int pad_border_edge(const struct cstyle *st, int e)
{
    if (!st) return 0;
    int p = (e == 0) ? st->pt : (e == 1) ? st->pr : (e == 2) ? st->pb : st->pl;
    return p + st->border_w[e];
}

struct sprop { int has, v, pct, off; };

static struct sprop pref_size(const struct cstyle *st, int horiz)
{
    struct sprop s; s.has = 0; s.v = 0; s.pct = 0; s.off = 0;
    if (!st) return s;
    if (horiz) { s.has = st->has_w; s.v = st->width;  s.pct = st->w_pct; s.off = st->w_off; }
    else       { s.has = st->has_h; s.v = st->height; s.pct = st->h_pct; s.off = st->h_off; }
    return s;
}
static struct sprop min_size(const struct cstyle *st, int horiz)
{
    struct sprop s; s.has = 0; s.v = 0; s.pct = 0; s.off = 0;
    if (!st) return s;
    if (horiz) { s.has = st->has_min_w; s.v = st->min_w; s.pct = st->min_w_pct; }
    else       { s.has = st->has_min_h; s.v = st->min_h; s.pct = st->min_h_pct; }
    return s;
}
static struct sprop max_size(const struct cstyle *st, int horiz)
{
    struct sprop s; s.has = 0; s.v = 0; s.pct = 0; s.off = 0;
    if (!st) return s;
    if (horiz) { s.has = st->has_max_w; s.v = st->max_w; s.pct = st->max_w_pct; }
    else       { s.has = st->has_max_h; s.v = st->max_h; s.pct = st->max_h_pct; }
    return s;
}

/* A length that may be a percentage of `avail`. 1 and *out if definite; 0 if
 * it is unspecified, or a percentage of an indefinite size -- which CSS treats
 * as auto. */
static int len_def(struct sprop s, int avail, int *out)
{
    if (!s.has) return 0;
    if (s.pct) {
        if (avail == FLEX_INDEFINITE) return 0;
        *out = (int)((long long)avail * s.v / 100) + s.off;
    } else *out = s.v;
    return 1;
}

/* An authored length is a border-box length under box-sizing:border-box; the
 * algorithm works in content-box sizes throughout. NOT floored at zero: § 9.2
 * says so explicitly for the flex base size ("an item with a specified size of
 * zero, positive padding, and box-sizing: border-box will have an outer flex
 * base size of zero -- and hence a negative inner flex base size"). Flooring
 * happens at the hypothetical main size, one step later. */
static int to_inner(const struct cstyle *st, int v, int bp)
{ return (st && st->box_sizing == BOX_BORDER) ? v - bp : v; }

static int clampi(int v, int lo, int hi)
{ if (v < lo) v = lo; if (v > hi) v = hi; return v; }

/* ---------------- the working item ---------------- */

struct fitem {
    const struct flex_item_in *in;
    const struct cstyle *st;
    int idx, order;
    int ms, me, cms, cme;               /* logical margins; autos start at 0 */
    unsigned char ms_auto, me_auto, cms_auto, cme_auto;
    int bp_main, bp_cross;              /* border + padding sums, per axis */
    int base;                           /* flex base size (inner) */
    int hypo;                           /* hypothetical main size (inner) */
    int minm, maxm;                     /* used min/max MAIN size (inner) */
    int minc, maxc;                     /* used min/max CROSS size (inner) */
    int grow, shrink;                   /* css_fixed */
    fx  target;
    unsigned char frozen;
    int viol;                           /* sign of this pass's clamp adjustment */
    int used_main, hypo_cross, used_cross;
    int baseline;
    int line, main_pos, cross_pos;
};

/* ---------------- § 4.5 automatic minimum size ----------------
 *
 * This is the rule that decides whether a real page overflows. `min-width` on
 * a flex item does not compute to 0 the way it does everywhere else in CSS: it
 * computes to the item's CONTENT-BASED MINIMUM SIZE, which is why a long word
 * or a wide image inside a flex row pushes the row wider instead of being
 * squeezed, and why `min-width:0` and `overflow:hidden` are the two standard
 * escapes from an item that refuses to shrink. */
static int auto_min_main(const struct fitem *f, const struct axis *a,
                         int avail_main, int avail_cross, int maxm)
{
    const struct cstyle *st = f->st;
#ifdef FLEX_NEGCTL_NOAUTOMIN
    /* NEGATIVE CONTROL: min-width:auto behaves the way min-width does
     * everywhere else in CSS -- it computes to zero. Pages lay out; they just
     * overflow, or rather they DON'T, which is the visible symptom: content
     * that should have pushed a row wider gets squeezed instead. */
    (void)avail_main; (void)avail_cross; (void)maxm; (void)a;
    return 0;
#endif
    /* "for main-axis scroll containers the automatic minimum size is zero" */
    if (st) {
        int ov = a->main_horiz ? st->overflow_x : st->overflow_y;
        if (ov != OVF_VISIBLE) return 0;
    }

    int v = f->in->min_content_main;            /* content size suggestion */
    if (v < 0) v = 0;

    /* transferred size suggestion: a definite preferred CROSS size put through
     * the aspect ratio, clamped first by the definite min/max cross sizes. */
    int have_trans = 0, trans = 0;
    if (f->in->ratio_w > 0 && f->in->ratio_h > 0) {
        int c;
        if (len_def(pref_size(st, !a->main_horiz), avail_cross, &c)) {
            int lo = 0, hi = BIGSIZE, t;
            c = to_inner(st, c, f->bp_cross);
            if (len_def(min_size(st, !a->main_horiz), avail_cross, &t))
                lo = to_inner(st, t, f->bp_cross);
            if (len_def(max_size(st, !a->main_horiz), avail_cross, &t))
                hi = to_inner(st, t, f->bp_cross);
            if (hi < lo) hi = lo;
            c = clampi(c, lo, hi);
            trans = a->main_horiz ? (int)((long long)c * f->in->ratio_w / f->in->ratio_h)
                                  : (int)((long long)c * f->in->ratio_h / f->in->ratio_w);
            have_trans = 1;
        }
    }
    if (have_trans)
        v = f->in->replaced ? (trans < v ? trans : v)     /* replaced: the smaller */
                            : (trans > v ? trans : v);    /* otherwise: the larger */

    /* "capped by the specified size suggestion (if one exists)" */
    int spec;
    if (len_def(pref_size(st, a->main_horiz), avail_main, &spec)) {
        spec = to_inner(st, spec, f->bp_main);
        if (spec < 0) spec = 0;
        if (v > spec) v = spec;
    }
    /* "the size is clamped by the maximum main size if it's definite" */
    if (v > maxm) v = maxm;
    return v < 0 ? 0 : v;
}

/* ---------------- § 9.2 flex base + hypothetical main size ---------------- */

static void item_base_size(struct fitem *f, const struct axis *a,
                           int avail_main, int avail_cross)
{
    const struct cstyle *st = f->st;
    int horiz = a->main_horiz;

    int kind = f->in->basis;                    /* FLEX_FB_* */
    int have_basis_len = 0, basis_len = 0;
    if (kind == FLEX_FB_FROM_STYLE && st && st->has_fb) {
        struct sprop s; s.has = 1; s.v = st->flex_basis; s.pct = st->fb_pct; s.off = st->fb_off;
        have_basis_len = len_def(s, avail_main, &basis_len);
    }

    /* The item's definite preferred cross size, if it has one -- both
     * aspect-ratio cases below need it. */
    int have_cross = 0, transferred = 0;
    if (f->in->ratio_w > 0 && f->in->ratio_h > 0) {
        int c;
        if (len_def(pref_size(st, !horiz), avail_cross, &c)) {
            int ci = to_inner(st, c, f->bp_cross);
            transferred = horiz ? (int)((long long)ci * f->in->ratio_w / f->in->ratio_h)
                                : (int)((long long)ci * f->in->ratio_h / f->in->ratio_w);
            have_cross = 1;
        }
    }

    if (have_basis_len) {
        /* "If the item has a definite used flex basis, that's the flex base size." */
        f->base = to_inner(st, basis_len, f->bp_main);
    } else if (kind == FLEX_FB_CONTENT) {
        /* `content` ignores the main size property outright. */
        f->base = have_cross ? transferred : f->in->max_content_main;
    } else {
        /* `auto` defers to the main size property, then to the ratio, then to
         * the max-content size. */
        int p;
        if (len_def(pref_size(st, horiz), avail_main, &p)) f->base = to_inner(st, p, f->bp_main);
        else if (have_cross)                               f->base = transferred;
        else                                               f->base = f->in->max_content_main;
    }

    /* used min/max main sizes. Max first: the automatic minimum is clamped by
     * it, so it has to exist before the minimum is derived. */
    int t;
    f->maxm = len_def(max_size(st, horiz), avail_main, &t) ? to_inner(st, t, f->bp_main) : BIGSIZE;
    if (f->maxm < 0) f->maxm = 0;
    if (len_def(min_size(st, horiz), avail_main, &t)) {
        f->minm = to_inner(st, t, f->bp_main);
        if (f->minm < 0) f->minm = 0;
    } else {
        f->minm = auto_min_main(f, a, avail_main, avail_cross, f->maxm);
    }
    if (f->minm > f->maxm) f->maxm = f->minm;   /* min wins over max, as in CSS 2.1 */

    /* "The hypothetical main size is the item's flex base size clamped
     * according to its used min and max main sizes (and flooring the content
     * box size at zero)." */
    f->hypo = clampi(f->base, f->minm, f->maxm);
    if (f->hypo < 0) f->hypo = 0;
}

/* ---------------- § 9.7 resolving flexible lengths ---------------- */

/* Outer main size of an item, using `sz` as its inner main size. */
static int outer_main(const struct fitem *f, int sz)
{ return f->ms + f->bp_main + sz + f->me; }

static void resolve_line(struct fitem *it, const int *ix, int n, int cmain, int gap)
{
    int i;
    if (n <= 0) return;

    /* 1. determine the used flex factor */
    long long sum_outer = (long long)gap * (n - 1);
    for (i = 0; i < n; i++) sum_outer += outer_main(&it[ix[i]], it[ix[i]].hypo);
    int grow_mode = (sum_outer < cmain);

    /* 2. target main size starts at the flex base size, nothing frozen.
     * 3. size inflexible items -- freeze them AT THE HYPOTHETICAL size. */
    for (i = 0; i < n; i++) {
        struct fitem *f = &it[ix[i]];
        f->target = TOFX(f->base);
        f->frozen = 0;
        int factor = grow_mode ? f->grow : f->shrink;
        if (factor == 0 ||
            (grow_mode  && f->base > f->hypo) ||
            (!grow_mode && f->base < f->hypo)) {
            f->frozen = 1;
            f->target = TOFX(f->hypo);
        }
    }

    /* 4. initial free space (frozen items count their target, others their base) */
    fx initial_free;
    {
        fx used = TOFX((long long)gap * (n - 1));
        for (i = 0; i < n; i++) {
            struct fitem *f = &it[ix[i]];
            used += f->frozen ? (f->target + TOFX(f->ms + f->bp_main + f->me))
                              : TOFX(outer_main(f, f->base));
        }
        initial_free = TOFX(cmain) - used;
    }

    /* 5. the loop */
    for (;;) {
        int unfrozen = 0;
        for (i = 0; i < n; i++) if (!it[ix[i]].frozen) unfrozen++;
        if (!unfrozen) break;                            /* a: check for flexible items */

        /* b: remaining free space, computed exactly as the initial one was */
        fx remaining;
        {
            fx used = TOFX((long long)gap * (n - 1));
            for (i = 0; i < n; i++) {
                struct fitem *f = &it[ix[i]];
                used += f->frozen ? (f->target + TOFX(f->ms + f->bp_main + f->me))
                                  : TOFX(outer_main(f, f->base));
            }
            remaining = TOFX(cmain) - used;
        }
        /* "If the sum of the unfrozen flex items' flex factors is less than
         * one, multiply the initial free space by this sum. If the magnitude
         * of this value is less than the magnitude of the remaining free
         * space, use this as the remaining free space." -- this is what keeps
         * three `flex-grow:0.25` items from filling the container. */
        long long sumf = 0;
        for (i = 0; i < n; i++) {
            struct fitem *f = &it[ix[i]];
            if (!f->frozen) sumf += grow_mode ? f->grow : f->shrink;
        }
        if (sumf < F_ONE) {
            fx scaled = (fx)((fxw)initial_free * sumf / F_ONE);
            fx ar = remaining < 0 ? -remaining : remaining;
            fx as = scaled    < 0 ? -scaled    : scaled;
            if (as < ar) remaining = scaled;
        }

        /* c: distribute it proportional to the flex factors */
        if (remaining != 0) {
            if (grow_mode) {
                long long sumg = 0;
                for (i = 0; i < n; i++) if (!it[ix[i]].frozen) sumg += it[ix[i]].grow;
                if (sumg > 0)
                    for (i = 0; i < n; i++) {
                        struct fitem *f = &it[ix[i]];
                        if (f->frozen) continue;
                        f->target = TOFX(f->base) + (fx)((fxw)remaining * f->grow / sumg);
                    }
            } else {
                /* THE SCALED FLEX SHRINK FACTOR -- see the header comment.
                 * The weight is flex-shrink TIMES the inner flex base size. */
                long long sums = 0;
                for (i = 0; i < n; i++) {
                    struct fitem *f = &it[ix[i]];
                    if (f->frozen) continue;
#ifdef FLEX_UNSCALED_SHRINK
                    sums += f->shrink;                       /* the negative control */
#else
                    sums += (long long)f->shrink * (f->base > 0 ? f->base : 0);
#endif
                }
                fx mag = remaining < 0 ? -remaining : remaining;
                if (sums > 0)
                    for (i = 0; i < n; i++) {
                        struct fitem *f = &it[ix[i]];
                        if (f->frozen) continue;
#ifdef FLEX_UNSCALED_SHRINK
                        long long w = f->shrink;
#else
                        long long w = (long long)f->shrink * (f->base > 0 ? f->base : 0);
#endif
                        f->target = TOFX(f->base) - (fx)((fxw)mag * w / sums);
                    }
            }
        }

        /* d: fix min/max violations */
        fx total = 0;
        for (i = 0; i < n; i++) {
            struct fitem *f = &it[ix[i]];
            if (f->frozen) { f->viol = 0; continue; }
            fx before = f->target, after = before;
            if (after > TOFX(f->maxm)) after = TOFX(f->maxm);
            if (after < TOFX(f->minm)) after = TOFX(f->minm);
            if (after < 0) after = 0;
            f->target = after;
            f->viol = (after > before) ? 1 : (after < before) ? -1 : 0;
            total += after - before;
        }

        /* e: freeze over-flexed items. Zero -> everything; positive -> the min
         * violations; negative -> the max violations. Always at least one, so
         * the loop terminates. */
        int froze = 0;
        for (i = 0; i < n; i++) {
            struct fitem *f = &it[ix[i]];
            if (f->frozen) continue;
            if (total == 0)      { f->frozen = 1; froze = 1; }
            else if (total > 0)  { if (f->viol > 0) { f->frozen = 1; froze = 1; } }
            else                 { if (f->viol < 0) { f->frozen = 1; froze = 1; } }
        }
        if (!froze) for (i = 0; i < n; i++) it[ix[i]].frozen = 1;   /* cannot happen; not a hang either */
#ifdef FLEX_NEGCTL_ONEPASS
        /* NEGATIVE CONTROL: resolve in ONE pass and clamp, instead of
         * freezing the violators and redistributing what they gave back. This
         * is not a straw man -- it is precisely the approximation
         * c/apps/browser/layout.c's own flex_resolve() makes today, and its
         * own comment says so. It looks right until two items on a line clamp
         * at once, and then the line silently over- or under-flows. */
        for (i = 0; i < n; i++) it[ix[i]].frozen = 1;
#endif
    }
}

/* Round a line's fixed-point target sizes to whole pixels.
 *
 * Plain per-item rounding loses up to a pixel per item, and a row of eight grow
 * items then stops a visible distance short of its container's edge. So: floor
 * everything, then hand the shortfall out by largest fractional part. An item
 * that CLAMPED has an integer target (min/max are whole pixels), so its
 * fraction is zero and it is never chosen -- the repair cannot undo a
 * constraint the loop just enforced. */
static void round_line(struct fitem *it, const int *ix, int n)
{
    int i;
    fx exact_sum = 0;
    for (i = 0; i < n; i++) exact_sum += it[ix[i]].target;
    long long want = (exact_sum + FX_ONE / 2) / FX_ONE;

    long long have = 0;
    for (i = 0; i < n; i++) {
        struct fitem *f = &it[ix[i]];
        fx t = f->target < 0 ? 0 : f->target;
        f->used_main = (int)(t / FX_ONE);
        have += f->used_main;
    }
    long long deficit = want - have;
    while (deficit > 0) {
        int best = -1;
        fx bestfrac = 0;
        for (i = 0; i < n; i++) {
            struct fitem *f = &it[ix[i]];
            if (f->used_main + 1 > f->maxm) continue;
            fx frac = f->target - TOFX(f->used_main);
            if (frac > bestfrac) { bestfrac = frac; best = i; }
        }
        if (best < 0) break;
        it[ix[best]].used_main++;
        deficit--;
    }
}

/* ---------------- packing (justify-content / align-content) ----------------
 *
 * The extra space that sits BEFORE subject `k` of `count`, given `free` px of
 * leftover space. Cumulative rather than a per-gap increment, so `count`
 * truncations cannot drift the last subject away from the container's edge.
 *
 * The fallbacks are css-align-3's, and they are exactly what a single item and
 * an overflowing line do: space-between falls back to start, space-around and
 * space-evenly to center. */
static int pack_offset(int mode, int free, int count, int k)
{
    if (count <= 0) return 0;
    switch (mode) {
    case JC_END:     return free;
    case JC_CENTER:  return free / 2;
    case JC_BETWEEN:
        if (count == 1 || free <= 0) return 0;                    /* -> start */
        return (int)((long long)free * k / (count - 1));
    case JC_AROUND:
        if (free <= 0) return free / 2;                           /* -> center */
        return (int)((long long)free * (2 * k + 1) / (2 * count));
    case JC_EVENLY:
        if (free <= 0) return free / 2;                           /* -> center */
        return (int)((long long)free * (k + 1) / (count + 1));
    default:         return 0;                                    /* JC_START */
    }
}

/* align-content's vocabulary is align's (AL_*) PLUS the three distributed
 * values, which `struct cstyle` cannot spell -- see flex_in.align_content_space. */
static int content_mode(const struct flex_in *c)
{
    if (c->align_content_space >= 0) return c->align_content_space;
    int a = c->st ? c->st->align_content : AL_STRETCH;
    return (a == AL_END) ? JC_END : (a == AL_CENTER) ? JC_CENTER : JC_START;
}

static int align_of(const struct cstyle *cst, const struct cstyle *ist)
{
    int a = ist ? ist->align_self : AL_AUTO;
    if (a == AL_AUTO) a = cst ? cst->align_items : AL_STRETCH;
    return a;
}

/* ---------------- the entry point ---------------- */

int layout_flex_run(const struct flex_in *c, const struct flex_item_in *in, int n,
                    const struct flex_metrics *m, struct flex_out *out)
{
    int i, k, L;
    memset(out, 0, sizeof *out);
    if (n < 0) n = 0;

    const struct cstyle *cst = c->st;
    struct axis ax;
    axis_setup(&ax, cst ? cst->flex_dir : FDIR_ROW, cst ? cst->flex_wrap : FWRAP_NOWRAP,
               c->wm, c->rtl);

    /* grid_gap_x is column-gap (along the INLINE axis), grid_gap_y is row-gap.
     * Which one is the main-axis gap follows from the axis mapping, not from
     * flex-direction alone -- they part company in a vertical writing mode. */
    int gap_main = 0, gap_cross = 0;
    if (cst) {
        int ig = cst->grid_gap_x > 0 ? cst->grid_gap_x : 0;
        int bg = cst->grid_gap_y > 0 ? cst->grid_gap_y : 0;
        gap_main  = ax.main_is_inline ? ig : bg;
        gap_cross = ax.main_is_inline ? bg : ig;
    }

    /* One allocation: work items, output items, and five int arrays of n. */
    unsigned long need = (unsigned long)(n + 1) *
                         (sizeof(struct fitem) + sizeof(struct flex_item_out) + 5 * sizeof(int));
    char *blk = kmalloc(need);
    if (!blk) return -1;
    memset(blk, 0, need);
    struct fitem *it = (struct fitem *)blk;
    struct flex_item_out *oi = (struct flex_item_out *)(it + (n + 1));
    int *ix     = (int *)(oi + (n + 1));     /* flex items, order-modified */
    int *lstart = ix + (n + 1);
    int *lcross = lstart + (n + 1);
    int *lpos   = lcross + (n + 1);
    int *lasc   = lpos + (n + 1);            /* per-line max baseline ascent */

    /* ---- § 9.1: build one work item per input, indexed BY INPUT INDEX ---- */
    for (i = 0; i < n; i++) {
        struct fitem *f = &it[i];
        f->in = &in[i]; f->st = in[i].st; f->idx = i;
        f->order = in[i].st ? in[i].st->order : 0;
        f->ms  = margin_edge(f->st, ax.ms_edge, &f->ms_auto);
        f->me  = margin_edge(f->st, ax.me_edge, &f->me_auto);
        f->cms = margin_edge(f->st, ax.cs_edge, &f->cms_auto);
        f->cme = margin_edge(f->st, ax.ce_edge, &f->cme_auto);
        f->bp_main  = pad_border_edge(f->st, ax.ms_edge) + pad_border_edge(f->st, ax.me_edge);
        f->bp_cross = pad_border_edge(f->st, ax.cs_edge) + pad_border_edge(f->st, ax.ce_edge);
        f->grow   = f->st ? f->st->flex_grow   : 0;
        f->shrink = f->st ? f->st->flex_shrink : F_ONE;
        if (f->grow < 0) f->grow = 0;
        if (f->shrink < 0) f->shrink = 0;
        f->line = -1;
        f->baseline = -1;
    }
    /* The flex items, in order-modified document order. Insertion sort over
     * indices: stable, and `order` is all-zero on nearly every real page, so
     * this is one comparison pass in practice. */
    int cnt = 0;
    for (i = 0; i < n; i++) if (!in[i].abspos) ix[cnt++] = i;
    for (i = 1; i < cnt; i++) {
        int key = ix[i], ko = it[key].order, j = i - 1;
        while (j >= 0 && it[ix[j]].order > ko) { ix[j + 1] = ix[j]; j--; }
        ix[j + 1] = key;
    }

    /* ---- § 9.2 ---- */
    for (i = 0; i < n; i++)
        item_base_size(&it[i], &ax, c->avail_main, c->avail_cross);

    /* ---- § 9.3 step 5: collect flex items into flex lines ----
     * The size used here is the item's OUTER HYPOTHETICAL main size, and the
     * gap counts. A line always takes at least one item, however badly it
     * fits. */
    int single = (!cst || cst->flex_wrap == FWRAP_NOWRAP) || c->avail_main == FLEX_INDEFINITE;
    int nline = 0;
    if (cnt == 0)      nline = 0;
    else if (single) { lstart[0] = 0; nline = 1; }
    else {
        i = 0;
        while (i < cnt) {
            long long used = 0;
            int j = i;
            while (j < cnt) {
                struct fitem *f = &it[ix[j]];
                long long outer = outer_main(f, f->hypo) + (j > i ? gap_main : 0);
                if (j > i && used + outer > c->avail_main) break;
                used += outer; j++;
            }
            if (j == i) j = i + 1;
            lstart[nline++] = i;
            i = j;
        }
    }
#define LEND(L) ((L) + 1 < nline ? lstart[(L) + 1] : cnt)

    /* ---- the container's used main size ---- */
    int cmain;
    if (c->avail_main != FLEX_INDEFINITE) cmain = c->avail_main;
    else {
        /* An auto-sized main axis: the container's automatic main size is its
         * max-content size (§ 9.2 step 3 / § 9.9.1), which for a single line is
         * the sum of the items' outer hypothetical main sizes. Nothing is left
         * over, so nothing grows or shrinks -- the honest answer for a
         * container whose size is defined BY its content. */
        long long mx = 0;
        for (L = 0; L < nline; L++) {
            long long s = (long long)gap_main * (LEND(L) - lstart[L] - 1);
            for (k = lstart[L]; k < LEND(L); k++) s += outer_main(&it[ix[k]], it[ix[k]].hypo);
            if (s > mx) mx = s;
        }
        cmain = (int)mx;
    }

    /* ---- § 9.3 step 6 / § 9.7 ---- */
    for (L = 0; L < nline; L++) {
        int lo = lstart[L], hi = LEND(L);
        resolve_line(it, ix + lo, hi - lo, cmain, gap_main);
        round_line(it, ix + lo, hi - lo);
        for (k = lo; k < hi; k++) it[ix[k]].line = L;
    }

    /* ---- § 9.4 step 7: the hypothetical cross size of each item ---- */
    int horiz_cross = !ax.main_horiz;
    for (k = 0; k < cnt; k++) {
        struct fitem *f = &it[ix[k]];
        int t;
        f->maxc = len_def(max_size(f->st, horiz_cross), c->avail_cross, &t)
                    ? to_inner(f->st, t, f->bp_cross) : BIGSIZE;
        if (f->maxc < 0) f->maxc = 0;
        f->minc = len_def(min_size(f->st, horiz_cross), c->avail_cross, &t)
                    ? to_inner(f->st, t, f->bp_cross) : 0;
        if (f->minc < 0) f->minc = 0;
        if (f->minc > f->maxc) f->maxc = f->minc;

        int hc;
        if (len_def(pref_size(f->st, horiz_cross), c->avail_cross, &t))
            hc = to_inner(f->st, t, f->bp_cross);
        else if (f->in->ratio_w > 0 && f->in->ratio_h > 0)
            hc = ax.main_horiz ? (int)((long long)f->used_main * f->in->ratio_h / f->in->ratio_w)
                               : (int)((long long)f->used_main * f->in->ratio_w / f->in->ratio_h);
        else
            hc = (m && m->cross) ? m->cross(f->in->ref, f->used_main, m->ctx) : 0;
        if (hc < 0) hc = 0;
        f->hypo_cross = clampi(hc, f->minc, f->maxc);
        f->used_cross = f->hypo_cross;          /* stretch may replace this */
    }

    /* ---- § 9.4 step 8: the cross size of each flex line ----
     * The baseline group and the plain group are measured SEPARATELY and the
     * larger wins: a line of baseline-aligned items is as tall as the deepest
     * ascent plus the deepest descent, which is generally MORE than the tallest
     * single item. */
    for (L = 0; L < nline; L++) {
        int lo = lstart[L], hi = LEND(L);
        int asc = 0, desc = 0, plain = 0, anybase = 0;
        for (k = lo; k < hi; k++) {
            struct fitem *f = &it[ix[k]];
            int outer = f->cms + f->bp_cross + f->hypo_cross + f->cme;
            int a = align_of(cst, f->st);
            int part = (a == AL_BASELINE) && ax.main_is_inline && !f->cms_auto && !f->cme_auto;
            if (part && m && m->baseline) {
                int b = m->baseline(f->in->ref, f->used_main, f->hypo_cross, m->ctx);
                if (b >= 0) {
                    f->baseline = b;
                    int aa = f->cms + b;
                    if (aa > asc) asc = aa;
                    if (outer - aa > desc) desc = outer - aa;
                    anybase = 1;
                    continue;
                }
            }
            if (outer > plain) plain = outer;
        }
        lasc[L] = asc;
        { int v = anybase ? asc + desc : 0;
          lcross[L] = v > plain ? v : plain;
          if (lcross[L] < 0) lcross[L] = 0; }
    }
    if (nline == 1) {
        if (c->avail_cross != FLEX_INDEFINITE) lcross[0] = c->avail_cross;
        /* "If the flex container is single-line, then clamp the line's
         * cross-size to be within the container's computed min and max cross
         * sizes." */
        int t, lo = 0, hi = BIGSIZE;
        if (len_def(min_size(cst, horiz_cross), c->avail_cross, &t)) lo = t;
        if (len_def(max_size(cst, horiz_cross), c->avail_cross, &t)) hi = t;
        if (hi < lo) hi = lo;
        lcross[0] = clampi(lcross[0], lo, hi);
    }

    /* ---- § 9.4 step 9: align-content:stretch grows the LINES ---- */
    if (nline > 1 && c->avail_cross != FLEX_INDEFINITE && c->align_content_space < 0 &&
        (cst ? cst->align_content : AL_STRETCH) == AL_STRETCH) {
        long long sum = (long long)gap_cross * (nline - 1);
        for (L = 0; L < nline; L++) sum += lcross[L];
        if (sum < c->avail_cross) {
            long long extra = c->avail_cross - sum;
            for (L = 0; L < nline; L++)
                lcross[L] += (int)(extra * (L + 1) / nline - extra * L / nline);
        }
    }

    /* ---- § 9.4 step 11: the used cross size (the `stretch` case) ---- */
    for (k = 0; k < cnt; k++) {
        struct fitem *f = &it[ix[k]];
        int a = align_of(cst, f->st);
        int auto_cross = !pref_size(f->st, horiz_cross).has;
        if (a == AL_STRETCH && auto_cross && !f->cms_auto && !f->cme_auto) {
            int v = lcross[f->line] - f->cms - f->cme - f->bp_cross;
            if (v < 0) v = 0;
            f->used_cross = clampi(v, f->minc, f->maxc);
        }
    }

    /* ---- § 9.5 step 12: main-axis alignment ---- */
    for (L = 0; L < nline; L++) {
        int lo = lstart[L], hi = LEND(L), ncell = hi - lo;
        long long used = (long long)gap_main * (ncell - 1);
        int nauto = 0;
        for (k = lo; k < hi; k++) {
            struct fitem *f = &it[ix[k]];
            used += outer_main(f, f->used_main);
            nauto += (f->ms_auto ? 1 : 0) + (f->me_auto ? 1 : 0);
        }
        int freem = cmain - (int)used;
        if (freem > 0 && nauto > 0) {
            /* auto main margins take ALL the positive free space before
             * justify-content ever sees any of it. Cumulative division so the
             * shares sum to exactly `freem`. */
            int given = 0, seen = 0;
            for (k = lo; k < hi; k++) {
                struct fitem *f = &it[ix[k]];
                if (f->ms_auto) { seen++; int t = (int)((long long)freem * seen / nauto);
                                  f->ms = t - given; given = t; }
                if (f->me_auto) { seen++; int t = (int)((long long)freem * seen / nauto);
                                  f->me = t - given; given = t; }
            }
            freem = 0;
        }
        int mode = cst ? cst->justify : JC_START;
        int run = 0;
        for (k = 0; k < ncell; k++) {
            struct fitem *f = &it[ix[lo + k]];
            f->main_pos = run + f->ms + pack_offset(mode, freem, ncell, k);
            run += outer_main(f, f->used_main) + gap_main;
        }
    }

    /* ---- § 9.6 steps 13-14: cross-axis alignment within the line ---- */
    for (L = 0; L < nline; L++) {
        int lo = lstart[L], hi = LEND(L);
        for (k = lo; k < hi; k++) {
            struct fitem *f = &it[ix[k]];
            int box = f->bp_cross + f->used_cross;
            int outer = f->cms + box + f->cme;
            if (f->cms_auto || f->cme_auto) {
                int space = lcross[L] - outer;
                int na = (f->cms_auto ? 1 : 0) + (f->cme_auto ? 1 : 0);
                if (space > 0) {
                    if (na == 2) { f->cms = space / 2; f->cme = space - space / 2; }
                    else if (f->cms_auto) f->cms = space;
                    else f->cme = space;
                } else {
                    if (f->cms_auto) f->cms = 0;
                    if (f->cme_auto) { f->cme = lcross[L] - f->cms - box;
                                       if (f->cme < 0) f->cme = 0; }
                }
                f->cross_pos = f->cms;
                continue;
            }
            int a = align_of(cst, f->st);
            int space = lcross[L] - outer;
            switch (a) {
            case AL_END:    f->cross_pos = lcross[L] - f->cme - box; break;
            case AL_CENTER: f->cross_pos = f->cms + space / 2; break;
            case AL_BASELINE:
                if (ax.main_is_inline && f->baseline >= 0) {
                    f->cross_pos = lasc[L] - f->baseline; break;
                }
                f->cross_pos = f->cms; break;                  /* -> flex-start */
            default:        f->cross_pos = f->cms; break;      /* stretch, start */
            }
        }
    }

    /* ---- § 9.6 steps 15-16: the container's cross size, then align-content -- */
    long long total = (long long)gap_cross * (nline > 0 ? nline - 1 : 0);
    for (L = 0; L < nline; L++) total += lcross[L];
    if (total < 0) total = 0;
    int ccross = (c->avail_cross != FLEX_INDEFINITE) ? c->avail_cross : (int)total;
    {
        int freec = ccross - (int)total;
        /* align-content has no effect on a single-line flex container (§ 8.4). */
        int mode = (nline > 1) ? content_mode(c) : JC_START;
        int run = 0;
        for (L = 0; L < nline; L++) {
            lpos[L] = run + pack_offset(mode, freec, nline, L);
            run += lcross[L] + gap_cross;
        }
    }

    /* ---- emit the flex items ---- */
    int no = 0;
    for (k = 0; k < cnt; k++) {
        struct fitem *f = &it[ix[k]];
        struct flex_item_out *o = &oi[no++];
        o->ref = f->in->ref; o->idx = f->idx; o->line = f->line;
        o->main_size = f->used_main; o->cross_size = f->used_cross;
        o->main_outer  = f->bp_main  + f->used_main;
        o->cross_outer = f->bp_cross + f->used_cross;
        o->main_pos  = f->main_pos;
        o->cross_pos = lpos[f->line] + f->cross_pos;
        o->baseline  = f->baseline;
        int pm = ax.main_rev  ? cmain  - o->main_pos  - o->main_outer  : o->main_pos;
        int pc = ax.cross_rev ? ccross - o->cross_pos - o->cross_outer : o->cross_pos;
        o->x = ax.main_horiz ? pm : pc;
        o->y = ax.main_horiz ? pc : pm;
        o->w = ax.main_horiz ? o->main_outer  : o->cross_outer;
        o->h = ax.main_horiz ? o->cross_outer : o->main_outer;
    }

    /* ---- § 4.1: absolutely positioned children ----
     * Not flex items at all. What flexbox owes them is a STATIC POSITION: the
     * one they would have as the SOLE flex item of the container, which is what
     * justify-content and align-items say about a single subject. The size here
     * is a shrink-to-fit stand-in -- the real one comes from the box's own
     * offsets, which this module does not resolve -- so treat main_pos/cross_pos
     * as the answer and the sizes as advisory. */
    for (i = 0; i < n; i++) {
        if (!in[i].abspos) continue;
        struct fitem *f = &it[i];
        struct flex_item_out *o = &oi[no++];
        int mainsz = f->hypo, t;
        if (c->avail_main != FLEX_INDEFINITE) {
            int room = c->avail_main - f->ms - f->me - f->bp_main;
            if (room < 0) room = 0;
            if (mainsz > room) mainsz = room;
        }
        int crosssz = 0;
        if (len_def(pref_size(f->st, horiz_cross), c->avail_cross, &t))
            crosssz = to_inner(f->st, t, f->bp_cross);
        else if (m && m->cross) crosssz = m->cross(f->in->ref, mainsz, m->ctx);
        if (crosssz < 0) crosssz = 0;

        o->ref = f->in->ref; o->idx = i; o->line = -1;
        o->main_size = mainsz; o->cross_size = crosssz;
        o->main_outer  = f->bp_main  + mainsz;
        o->cross_outer = f->bp_cross + crosssz;
        int mfree = cmain  - (f->ms  + o->main_outer  + f->me);
        int cfree = ccross - (f->cms + o->cross_outer + f->cme);
        o->main_pos = f->ms + pack_offset(cst ? cst->justify : JC_START, mfree, 1, 0);
        { int a = align_of(cst, f->st);
          o->cross_pos = f->cms + (a == AL_END ? cfree : a == AL_CENTER ? cfree / 2 : 0); }
        o->baseline = -1;
        int pm = ax.main_rev  ? cmain  - o->main_pos  - o->main_outer  : o->main_pos;
        int pc = ax.cross_rev ? ccross - o->cross_pos - o->cross_outer : o->cross_pos;
        o->x = ax.main_horiz ? pm : pc;
        o->y = ax.main_horiz ? pc : pm;
        o->w = ax.main_horiz ? o->main_outer  : o->cross_outer;
        o->h = ax.main_horiz ? o->cross_outer : o->main_outer;
    }

    out->items = oi; out->nitems = no;
    out->line_cross = lcross; out->line_pos = lpos; out->nlines = nline;
    out->main_size = cmain; out->cross_size = ccross;
    out->blk = blk;
    return 0;
#undef LEND
}

void layout_flex_free(struct flex_out *out)
{
    if (!out || !out->blk) return;
    kfree(out->blk);
    memset(out, 0, sizeof *out);
}
