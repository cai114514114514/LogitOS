/* Open Logit -- the scanline coverage rasterizer. THE one in this tree.
 *
 * CONSTRUCTION: `subs` sub-scanlines per pixel row (a per-call argument now --
 * see the GFX_SUBS comment in gfx.h for why one constant was wrong: shapes and
 * glyphs are not the same job), and along each of them EXACT fractional
 * horizontal coverage rather than a point sample. That asymmetry is the whole
 * trick -- horizontal coverage is analytic and free, vertical coverage costs a
 * pass each, so you buy accuracy where it is cheap and sample where it is not.
 * At the default of 4 sub-scanlines, measured against a 16x supersampled
 * reference the worst pixel error is 0.08 for fills and 0.14 for rings, at 1/4
 * the cost of 16x supersampling.
 *
 * WINDING. Both rules, from the same walk: crossings of a sub-scanline are
 * collected with their direction, sorted by x, and the interior is the run
 * where the accumulated winding is nonzero (GFX_NONZERO) or odd (GFX_EVENODD).
 * The edge test is HALF-OPEN in y (ytop <= ys < ybot), which is what stops a
 * vertex shared by two edges from being counted twice and punching a hole.
 *
 * NO ALLOCATION. The edge table, the active list, the crossing list and one
 * scratch row are bounded statics -- see gfx.h's LIMITS comment for the exact
 * byte count and why these particular caps. A path with more edges than fit is
 * refused rather than truncated (gfx_path.c explains why a truncated path is
 * the worst kind of failure), and as of this milestone so is a scanline that
 * would need more concurrently active edges, or more crossings, than the
 * active/crossing tables hold -- see "THE TWO-PASS SWEEP" below for why that
 * needed a different mechanism than "check once, up front", which is all
 * build_edges() has ever needed.
 *
 * THE TWO-PASS SWEEP. build_edges() can refuse before drawing anything because
 * it runs once, completely, before the first row exists. The active-edge and
 * crossing overflows can't use that trick as-is: whether either would happen
 * is only knowable scanline by scanline, DURING the same sweep that is also
 * handing finished rows to the caller's row_fn. Discovering an overflow on row
 * 800 of 1000 after rows 0-799 have already been written into the caller's
 * coverage buffer (gfx_fill_mask) or blended into a live destination surface
 * (gfx_fill) is exactly the "plausible wrong picture" this file refuses to
 * produce elsewhere -- and for gfx_fill there is no undo available: Porter-
 * Duff src-over has already been blended into pixels this file owns no backup
 * of. So sweep() below runs the full row-by-row edge walk TWICE: once with
 * `commit`=0, which does every bit of active-list/crossing-list bookkeeping
 * and returns 0 the instant either would overflow, but writes no coverage and
 * calls row_fn on nothing; and, only if that dry run clears every row all the
 * way to y1, a second time with `commit`=1 -- the original single-pass
 * algorithm, now guaranteed not to hit either cap because it walks the
 * identical edge order from the identical starting state. The doubled cost is
 * the edge/active/crossing bookkeeping, which is cheap (touches a handful of
 * short-lived edges); the expensive part -- span_add/span_hard actually
 * writing coverage across a row -- still runs exactly once, only in the
 * committed pass, same as before this file had two passes at all.
 *
 * raster() SKIPS the dry run entirely whenever this fill's total edge count
 * already fits inside GFX_MAX_ACTIVE, because the active list can never hold
 * more edges than the fill has in total -- overflow is then provably
 * impossible without simulating it. Every shape phase 1 actually draws today
 * (corner quadrants, rounded rects, triangles) is tens of edges against a
 * four-figure cap, so that check keeps the common case at the ORIGINAL
 * one-pass cost, measured: raising the two-pass path to cost nothing for that
 * case turned out to matter -- the first version of this file always ran both
 * passes and cost a rounded rect's whole-path fill 6.9us -> 12.2us on
 * `bench-gfx`, purely in edge/active/crossing bookkeeping over a shape that
 * was never going to overflow. Only a fill that approaches the new, much larger
 * GFX_MAX_EDGES -- a dense SVG path, a CJK glyph -- pays for the second
 * sweep, which is exactly the geometry that needed one.
 *
 * The core is shared by both consumers -- the coverage-mask writer and the
 * paint compositor -- because they differ only in what they do with a finished
 * row. That is the difference between one rasterizer and two. */
#include "gfx.h"

struct edge {
    int ytop, ybot;      /* device 24.8, half-open [ytop, ybot)  */
    int xtop;            /* x at ytop, device 24.8               */
    int dxdy;            /* 16.16 slope                          */
    int dir;             /* +1 downward, -1 upward               */
};

static struct edge g_edge[GFX_MAX_EDGES];
static short g_order[GFX_MAX_EDGES];       /* edge indices sorted by ytop */
static short g_act[GFX_MAX_ACTIVE];
static struct { int x; int dir; } g_cross[GFX_MAX_ACTIVE];
static unsigned char g_row[GFX_MAX_W];

#ifndef GFX_NO_AA
/* Add `amt` of coverage over the half-open span [x0,x1), both in 1/256 px.
 * Whole pixels take the full amount; the two partial ends take their fraction.
 * This is the analytic half of the construction. */
static void span_add(unsigned char *row, int n, long x0, long x1, int amt)
{
    long lim = (long)n * 256;
    if (x0 < 0) x0 = 0;
    if (x1 > lim) x1 = lim;
    if (x1 <= x0) return;
    int p0 = (int)(x0 >> 8), p1 = (int)((x1 - 1) >> 8);
    if (p0 == p1) {
        int v = row[p0] + amt * (int)(x1 - x0) / 256;
        row[p0] = (unsigned char)(v > 255 ? 255 : v);
        return;
    }
    int v = row[p0] + amt * (256 - (int)(x0 & 255)) / 256;
    row[p0] = (unsigned char)(v > 255 ? 255 : v);
    for (int p = p0 + 1; p < p1; p++) {
        v = row[p] + amt;
        row[p] = (unsigned char)(v > 255 ? 255 : v);
    }
    int tail = (int)(x1 & 255); if (!tail) tail = 256;
    v = row[p1] + amt * tail / 256;
    row[p1] = (unsigned char)(v > 255 ? 255 : v);
}
#endif

#ifdef GFX_NO_AA
/* NEGATIVE CONTROL (-DGFX_NO_AA). One centre sample per row, and a pixel is
 * either wholly in or wholly out. Every shape still draws and still looks
 * broadly right, which is exactly the point: the accuracy assertions against
 * the supersampled reference must FAIL, and that is what demonstrates they are
 * measuring the rasterizer rather than the geometry. */
static void span_hard(unsigned char *row, int n, long x0, long x1)
{
    int p0 = (int)((x0 + 128) >> 8), p1 = (int)((x1 + 128) >> 8);
    if (p0 < 0) p0 = 0;
    if (p1 > n) p1 = n;
    for (int p = p0; p < p1; p++) row[p] = 255;
}
#endif

/* ---- edge table ---- */

static int add_edge(int n, int x0, int y0, int x1, int y1)
{
    if (y0 == y1) return n;                    /* horizontal: contributes nothing */
    if (n >= GFX_MAX_EDGES) return -1;
    struct edge *e = &g_edge[n];
    if (y0 < y1) { e->ytop = y0; e->ybot = y1; e->xtop = x0; e->dir = 1;
                   e->dxdy = (int)(((long long)(x1 - x0) << 16) / (y1 - y0)); }
    else         { e->ytop = y1; e->ybot = y0; e->xtop = x1; e->dir = -1;
                   e->dxdy = (int)(((long long)(x0 - x1) << 16) / (y0 - y1)); }
    return n + 1;
}

/* Every subpath is closed implicitly -- a fill has no notion of an open
 * contour, and leaving the closing edge out is how a path that the caller
 * forgot to close() bleeds sideways across the whole scanline. */
static int build_edges(const struct gfx_path *p)
{
    int n = 0;
    for (int s = 0; s < p->nsub; s++) {
        int a = p->sub[s];
        int b = (s + 1 < p->nsub) ? p->sub[s + 1] : p->npt;
        if (b - a < 2) continue;
        for (int i = a; i < b - 1; i++) {
            n = add_edge(n, p->pt[i * 2], p->pt[i * 2 + 1],
                            p->pt[i * 2 + 2], p->pt[i * 2 + 3]);
            if (n < 0) return -1;
        }
        n = add_edge(n, p->pt[(b - 1) * 2], p->pt[(b - 1) * 2 + 1],
                        p->pt[a * 2], p->pt[a * 2 + 1]);
        if (n < 0) return -1;
    }
    return n;
}

/* Shell sort of the edge order by ytop. Insertion sort is quadratic and this
 * table can hold GFX_MAX_EDGES entries; shell sort is a dozen lines, needs no
 * recursion and no scratch, and is comfortably fast at that size. */
static void sort_order(int n)
{
    static const int gaps[] = { 701, 301, 132, 57, 23, 10, 4, 1 };
    for (int i = 0; i < n; i++) g_order[i] = (short)i;
    for (unsigned gi = 0; gi < sizeof gaps / sizeof gaps[0]; gi++) {
        int g = gaps[gi];
        if (g >= n) continue;
        for (int i = g; i < n; i++) {
            short v = g_order[i];
            int key = g_edge[v].ytop, j = i;
            while (j >= g && g_edge[g_order[j - g]].ytop > key) {
                g_order[j] = g_order[j - g];
                j -= g;
            }
            g_order[j] = v;
        }
    }
}

typedef void (*row_fn)(void *user, int y, const unsigned char *row, int w);

/* One full row-by-row sweep of the sorted edge order [0,ne) over rows
 * [y0,y1), `subs` sub-scanlines per row.
 *
 * `commit`=0 is the dry run from the file comment: it performs the admit /
 * retire / crossing bookkeeping exactly as the real pass would, and returns 0
 * the INSTANT an edge that should activate can't (GFX_MAX_ACTIVE already
 * full) or a crossing that should be recorded can't (same cap on g_cross) --
 * this is the loud refusal for both truncations that used to be silent here.
 * It writes no coverage and calls `fn` on nothing, so a caller can throw the
 * result away with no visible effect.
 *
 * `commit`=1 is the original single-pass algorithm: it writes real coverage
 * into g_row and calls `fn` once per finished row. It carries the identical
 * overflow checks (rather than assuming the dry run already proved them
 * unreachable) so a bug that ever let the two passes diverge fails loud
 * instead of silently trusting a stale guarantee. */
static int sweep(int ne, int rule, int ox, int y0, int y1, int w, int subs,
                 row_fn fn, void *user, int commit)
{
    int nact = 0, next = 0;
    long ox256 = (long)ox * 256;
#ifndef GFX_NO_AA
    /* Loop-invariant over the whole sweep (subs is fixed for the call) but
     * NOT over the k-loop that computes `ys`, so only this one -- the
     * per-span coverage amount -- can be hoisted without touching the sample
     * positions span_add's accuracy depends on; see the raster() comment for
     * why `ys` itself keeps its division. Computed here rather than trusting
     * the compiler to hoist it out of sweep()'s own k-loop, since sweep()
     * alone (subs is just a parameter) cannot prove subs != 0 the way
     * raster()'s caller-side clamp does. */
    int amt = 256 / subs;
#endif

    /* Skip edges that end above the first row we care about. */
    while (next < ne && g_edge[g_order[next]].ybot <= (long)y0 * 256) next++;

    for (int y = y0; y < y1; y++) {
        if (commit) gfx_zero(g_row, w);
        for (int k = 0; k < subs; k++) {
            int ys = (y << 8) + (k * 256 + 128) / subs;
            /* admit newly started edges */
            while (next < ne && g_edge[g_order[next]].ytop <= ys) {
                if (g_edge[g_order[next]].ybot > ys) {
                    if (nact >= GFX_MAX_ACTIVE) return 0;
                    g_act[nact++] = g_order[next];
                }
                next++;
            }
            /* retire finished ones */
            for (int i = 0; i < nact; ) {
                if (g_edge[g_act[i]].ybot <= ys) g_act[i] = g_act[--nact];
                else i++;
            }
            /* crossings */
            int nc = 0;
            for (int i = 0; i < nact; i++) {
                const struct edge *e = &g_edge[g_act[i]];
                if (ys < e->ytop || ys >= e->ybot) continue;
                if (nc >= GFX_MAX_ACTIVE) return 0;
                long x = (long)e->xtop + (((long long)e->dxdy * (ys - e->ytop)) >> 16);
                g_cross[nc].x = (int)(x - ox256);
                g_cross[nc].dir = e->dir;
                nc++;
            }
            /* insertion sort by x: a scanline crosses few edges, and the list
             * is nearly sorted from the previous sub-row in the common case. */
            for (int i = 1; i < nc; i++) {
                int cx = g_cross[i].x, cd = g_cross[i].dir, j = i - 1;
                while (j >= 0 && g_cross[j].x > cx) { g_cross[j + 1] = g_cross[j]; j--; }
                g_cross[j + 1].x = cx; g_cross[j + 1].dir = cd;
            }
            /* winding walk -- coverage-writing only, so skipped entirely on
             * the dry run: it cannot affect nc/nact and so cannot change
             * whether this sweep overflows. */
            if (commit) {
                int wind = 0;
                for (int i = 0; i + 1 <= nc - 1; i++) {
                    wind += (rule == GFX_EVENODD) ? 1 : g_cross[i].dir;
                    int inside = (rule == GFX_EVENODD) ? (wind & 1) : (wind != 0);
                    if (!inside) continue;
#ifdef GFX_NO_AA
                    span_hard(g_row, w, g_cross[i].x, g_cross[i + 1].x);
#else
                    span_add(g_row, w, g_cross[i].x, g_cross[i + 1].x, amt);
#endif
                }
            }
        }
        if (commit) fn(user, y, g_row, w);
    }
    return 1;
}

/* The core. Rasterizes rows [y0,y1) of `p` at device x offset `ox`, `w` pixels
 * wide, `subs` sub-scanlines per row, calling `fn` with each finished coverage
 * row. `subs` is a caller choice (glyphs want more than a button) but is
 * clamped to GFX_MAX_SUBS and, under -DGFX_NO_AA, forced to the single centre
 * sample the negative control requires regardless of what was asked for. */
static int raster(const struct gfx_path *p, int rule, int ox, int y0, int y1,
                  int w, int subs, row_fn fn, void *user)
{
    if (p->overflow) return 0;                 /* refuse a truncated path (or
                                                 * one whose matrix changed
                                                 * mid-build -- see gfx_path.c) */
    if (w <= 0 || w > GFX_MAX_W || y1 <= y0) return 0;
#ifdef GFX_NO_AA
    subs = 1;               /* the negative control overrides any request */
#else
    if (subs <= 0) subs = 1;
    if (subs > GFX_MAX_SUBS) subs = GFX_MAX_SUBS;
#endif
    int ne = build_edges(p);
    if (ne <= 0) return ne == 0 ? 1 : 0;       /* empty is a success, overflow is not */
    sort_order(ne);

    /* The dry run's whole purpose is proving the active list and crossing
     * list can't overflow -- and nact (how many of `ne` edges are ever
     * simultaneously active) can never exceed `ne` itself, since it is
     * populated from a subset of the edge table. So whenever this fill's
     * TOTAL edge count already fits inside GFX_MAX_ACTIVE, neither list can
     * possibly overflow and the dry run would only reconfirm the arithmetic
     * that already proved it -- skip straight to the committed pass. This is
     * not a shortcut around the doctrine, it is the doctrine applied one
     * level up: build_edges() already refuses (returns -1) when `ne` would
     * exceed GFX_MAX_EDGES, before any row exists, for the same reason. Every
     * shape phase 1 actually draws today -- corner quadrants, rounded rects,
     * triangles -- is tens of edges, three orders of magnitude under
     * GFX_MAX_ACTIVE, so this keeps the common case at the original one-pass
     * cost; only a fill that legitimately approaches the new, much larger
     * GFX_MAX_EDGES (a dense SVG path, a CJK glyph) pays for the second
     * sweep, and that is exactly the geometry the two-pass check exists to
     * protect. */
    if (ne <= GFX_MAX_ACTIVE)
        return sweep(ne, rule, ox, y0, y1, w, subs, fn, user, 1);

    if (!sweep(ne, rule, ox, y0, y1, w, subs, fn, user, 0)) return 0;
    return sweep(ne, rule, ox, y0, y1, w, subs, fn, user, 1);
}

/* --------------------------------------------------------- coverage mask -- */

struct mask_sink { unsigned char *cov; int w, h, oy; };

static void mask_row(void *user, int y, const unsigned char *row, int w)
{
    struct mask_sink *m = (struct mask_sink *)user;
    int j = y - m->oy;
    if (j < 0 || j >= m->h) return;
    unsigned char *d = m->cov + (long)j * m->w;
    for (int i = 0; i < w; i++) d[i] = row[i];
}

int gfx_fill_mask_subs(const struct gfx_path *p, int rule,
                       unsigned char *cov, int w, int h, int ox, int oy, int subs)
{
    if (!cov || w <= 0 || h <= 0) return 0;
    gfx_zero(cov, w * h);
    struct mask_sink m;
    m.cov = cov; m.w = w; m.h = h; m.oy = oy;
    return raster(p, rule, ox, oy, oy + h, w, subs, mask_row, &m);
}

int gfx_fill_mask(const struct gfx_path *p, int rule,
                  unsigned char *cov, int w, int h, int ox, int oy)
{
    return gfx_fill_mask_subs(p, rule, cov, w, h, ox, oy, GFX_SUBS);
}

/* ------------------------------------------------------------- surface -- */

struct fill_sink {
    struct gfx_surface *dst;
    const struct gfx_paint *paint;
    int x0, x1;                 /* device x range being painted */
};

void gfx_paint_row(const struct gfx_paint *p, struct gfx_surface *dst, int y,
                   int x0, int x1, const unsigned char *cov, int cov_ox);

static void fill_row(void *user, int y, const unsigned char *row, int w)
{
    struct fill_sink *f = (struct fill_sink *)user;
    (void)w;
    if (y < 0 || y >= f->dst->h) return;
    gfx_paint_row(f->paint, f->dst, y, f->x0, f->x1, row, f->x0);
}

int gfx_fill_subs(struct gfx_surface *dst, const struct gfx_path *p, int rule,
                  const struct gfx_paint *paint, const struct gfx_rect *clip, int subs)
{
    if (!dst || !dst->px || !p || !paint) return 0;
    int bx0, by0, bx1, by1;
    if (!gfx_path_bounds(p, &bx0, &by0, &bx1, &by1)) return 1;    /* nothing to do */
    int cx0 = 0, cy0 = 0, cx1 = dst->w, cy1 = dst->h;
    if (clip) {
        if (clip->x > cx0) cx0 = clip->x;
        if (clip->y > cy0) cy0 = clip->y;
        if (clip->x + clip->w < cx1) cx1 = clip->x + clip->w;
        if (clip->y + clip->h < cy1) cy1 = clip->y + clip->h;
    }
    if (bx0 > cx0) cx0 = bx0;
    if (by0 > cy0) cy0 = by0;
    if (bx1 < cx1) cx1 = bx1;
    if (by1 < cy1) cy1 = by1;
    if (cx1 <= cx0 || cy1 <= cy0) return 1;
    /* USED TO silently clip to GFX_MAX_W here (cx1 = cx0 + GFX_MAX_W). GFX_MAX_W
     * already clears every real display width with margin (gfx.h's LIMITS
     * comment), so a fill that still needs more than that is not a normal
     * case being politely cropped -- it is exactly the kind of oversized
     * request the doctrine at the top of this file exists to catch, and
     * raster()'s own `w > GFX_MAX_W` check below refuses it. Nothing else is
     * needed here: no row has been drawn yet at this point, so the refusal is
     * free -- unlike the active/crossing overflows above, this one really is
     * knowable before a single pixel is touched. */

    struct fill_sink f;
    f.dst = dst; f.paint = paint; f.x0 = cx0; f.x1 = cx1;
    return raster(p, rule, cx0, cy0, cy1, cx1 - cx0, subs, fill_row, &f);
}

int gfx_fill(struct gfx_surface *dst, const struct gfx_path *p, int rule,
             const struct gfx_paint *paint, const struct gfx_rect *clip)
{
    return gfx_fill_subs(dst, p, rule, paint, clip, GFX_SUBS);
}

/* ------------------------------------------------------------ path clip --
 * gfx.h's own comment on GFX_CLIP_MASK_MAX explains why the clip mask always
 * arrives pre-rasterized: raster()'s edge/active/crossing tables above are
 * file statics (not reentrant), so a clip path can never be swept from
 * INSIDE the subject's own row callback -- the caller rasterizes the clip
 * first with an ordinary gfx_fill_mask() call, into a buffer it owns, and
 * hands the FINISHED coverage in here. What is left for this file to do is
 * the multiply: for every row raster() streams for the SUBJECT, look up the
 * clip's coverage at the same device (x,y) and multiply the two.
 *
 * "At the same device (x,y)" is the entire difficulty, per the milestone
 * brief, so it is worth being explicit about the two coordinate systems in
 * play. The subject sweep is parameterised by (ox, y0..y1, w) -- row_fn's
 * `row[i]` is device pixel (ox+i, y). The clip mask is parameterised by its
 * own (clip->ox, clip->oy, clip->w, clip->h) -- clip->cov[j*clip->w+i] is
 * device pixel (clip->ox+i, clip->oy+j). Neither origin has any reason to
 * equal the other (a window's content clip and a child element's own fill
 * are almost never aligned), so every lookup below re-derives the clip index
 * from the device coordinate rather than from the subject's row index --
 * that re-derivation, done once per axis per row/pixel, is what keeps a
 * one-axis offset from silently reading the wrong clip column. */

/* One row of subject coverage (w px starting at device x=ox, at device row
 * y), multiplied by clip's coverage at those SAME device pixels. A device
 * pixel outside [clip->ox, clip->ox+clip->w) x [clip->oy, clip->oy+clip->h)
 * reads as clip coverage 0 -- a clip CLIPS, so unmapped territory means
 * "nothing shows here", not "this pixel is unclipped". Treating out-of-
 * extent as unclipped would let a caller hand a smaller clip buffer than it
 * actually needs and silently get a BIGGER clip region than it rasterized.
 *
 * Rounds rather than truncates -- (s*c + 127) / 255, not (s*c) / 255 -- for
 * the reason gfx_over's file comment already put on record for this exact
 * shape of division: truncation is not a half-bit of error in general, it is
 * a hole at the specific case (both operands near 255) where the answer is
 * least expected to be small. s=255,c=255 truncated is still 255 (255*255/
 * 255=255 exactly), but s=254,c=254 truncates to 253 instead of rounding to
 * 254 -- a visible one-step dimming along every fully-opaque-ish clipped
 * edge that rounding removes.
 *
 * Writes into g_row_clip and returns it rather than writing through `row`:
 * row_fn hands row_fn's caller a `const unsigned char *`, and rightly so --
 * it is g_row above, about to be overwritten in place for the NEXT row the
 * instant this call returns, so this function needs storage of its own for
 * the product, not license to mutate the rasterizer's own scratch through a
 * cast. Same size as g_row (GFX_MAX_W) and so the same kernel-.bss delta,
 * GFX_MAX_W bytes -- see gfx.h's LIMITS comment for the running total this
 * adds to. */
static unsigned char g_row_clip[GFX_MAX_W];

static const unsigned char *clip_row(const unsigned char *row, int w, int ox, int y,
                                     const struct gfx_clip_mask *clip)
{
    int cy = y - clip->oy;
    if (cy < 0 || cy >= clip->h) { gfx_zero(g_row_clip, w); return g_row_clip; }
    const unsigned char *crow = clip->cov + (long)cy * clip->w;
    for (int i = 0; i < w; i++) {
        int cx = ox + i - clip->ox;
        int c = (cx >= 0 && cx < clip->w) ? crow[cx] : 0;
#ifdef GFX_NO_CLIP
        /* NEGATIVE CONTROL (-DGFX_NO_CLIP): the multiply point is bypassed --
         * every clipped fill draws exactly as its unclipped twin would.
         * Every containment assertion in gfx_clip_test.c must then fail,
         * which is what proves those assertions are actually exercising the
         * multiply above rather than passing for some unrelated reason. */
        (void)c;
        g_row_clip[i] = row[i];
#else
        int s = row[i];
        g_row_clip[i] = (unsigned char)((s * c + 127) / 255);
#endif
    }
    return g_row_clip;
}

struct mask_sink_clipped {
    unsigned char *cov; int w, h, ox, oy;
    const struct gfx_clip_mask *clip;
};

static void mask_row_clipped(void *user, int y, const unsigned char *row, int w)
{
    struct mask_sink_clipped *m = (struct mask_sink_clipped *)user;
    int j = y - m->oy;
    if (j < 0 || j >= m->h) return;
    const unsigned char *cr = clip_row(row, w, m->ox, y, m->clip);
    unsigned char *d = m->cov + (long)j * m->w;
    for (int i = 0; i < w; i++) d[i] = cr[i];
}

/* Shared by both entry points: the three refusals gfx.h documents for a
 * gfx_clip_mask, checked before either does anything else. A clip->w<=0 or
 * ->h<=0 extent is deliberately NOT refused here -- both callers treat an
 * empty clip as the empty-intersection RESULT, not a malformed request; see
 * each entry point's own comment for where that is decided. */
static int clip_bad(const struct gfx_clip_mask *clip)
{
    if (!clip) return 1;                 /* these entry points exist FOR a
                                          * clip; a caller with none wants
                                          * gfx_fill_mask_subs/gfx_fill_subs */
    if (clip->w < 0 || clip->h < 0) return 1;
    if (clip->w > GFX_CLIP_MASK_MAX || clip->h > GFX_CLIP_MASK_MAX) return 1;
    /* A NULL cov is only a refusal when the mask claims to cover pixels --
     * w<=0 or h<=0 already means "no pixels", and a cov pointer nobody will
     * ever dereference is not worth refusing over. */
    if (clip->w > 0 && clip->h > 0 && !clip->cov) return 1;
    return 0;
}

int gfx_fill_mask_clipped(const struct gfx_path *p, int rule,
                          unsigned char *cov, int w, int h, int ox, int oy,
                          int subs, const struct gfx_clip_mask *clip)
{
    if (!cov || w <= 0 || h <= 0) return 0;
    if (clip_bad(clip)) return 0;
    gfx_zero(cov, w * h);

    /* Empty intersection between the subject's own [ox,ox+w)x[oy,oy+h) and
     * the clip's [clip->ox,clip->ox+clip->w)x[clip->oy,clip->oy+clip->h) --
     * including a clip whose w or h is itself <=0 -- is a RESULT, not a
     * malformed request: "clip to nothing" is a legitimate way to ask for
     * "draw nothing", and the all-zero buffer already written above is
     * exactly that answer. Refusing it would make an empty clip behave
     * differently from a clip that merely doesn't reach this tile, which is
     * the same shape as every other silent-truncation-turned-refusal this
     * milestone follows, just inverted -- here the RIGHT answer is success. */
    if (clip->w <= 0 || clip->h <= 0 ||
        ox >= clip->ox + clip->w || ox + w <= clip->ox ||
        oy >= clip->oy + clip->h || oy + h <= clip->oy)
        return 1;

    struct mask_sink_clipped m;
    m.cov = cov; m.w = w; m.h = h; m.ox = ox; m.oy = oy; m.clip = clip;
    return raster(p, rule, ox, oy, oy + h, w, subs, mask_row_clipped, &m);
}

struct fill_sink_clipped {
    struct gfx_surface *dst;
    const struct gfx_paint *paint;
    int x0, x1;
    const struct gfx_clip_mask *clip;
};

static void fill_row_clipped(void *user, int y, const unsigned char *row, int w)
{
    struct fill_sink_clipped *f = (struct fill_sink_clipped *)user;
    (void)w;
    if (y < 0 || y >= f->dst->h) return;
    const unsigned char *cr = clip_row(row, f->x1 - f->x0, f->x0, y, f->clip);
    gfx_paint_row(f->paint, f->dst, y, f->x0, f->x1, cr, f->x0);
}

int gfx_fill_clipped(struct gfx_surface *dst, const struct gfx_path *p, int rule,
                     const struct gfx_paint *paint, const struct gfx_rect *rectclip,
                     int subs, const struct gfx_clip_mask *clip)
{
    if (!dst || !dst->px || !p || !paint) return 0;
    if (clip_bad(clip)) return 0;
    int bx0, by0, bx1, by1;
    if (!gfx_path_bounds(p, &bx0, &by0, &bx1, &by1)) return 1;    /* nothing to do */
    int cx0 = 0, cy0 = 0, cx1 = dst->w, cy1 = dst->h;
    if (rectclip) {
        if (rectclip->x > cx0) cx0 = rectclip->x;
        if (rectclip->y > cy0) cy0 = rectclip->y;
        if (rectclip->x + rectclip->w < cx1) cx1 = rectclip->x + rectclip->w;
        if (rectclip->y + rectclip->h < cy1) cy1 = rectclip->y + rectclip->h;
    }
    if (bx0 > cx0) cx0 = bx0;
    if (by0 > cy0) cy0 = by0;
    if (bx1 < cx1) cx1 = bx1;
    if (by1 < cy1) cy1 = by1;
    /* Unlike gfx_fill_mask_clipped above, this entry point has no fixed
     * caller-owned output buffer to fill exactly -- it composites straight
     * into `dst`, and the sweep window is already an intersection of the
     * surface bounds, the path's own bounds and rectclip. Folding the clip
     * MASK's extent into that same intersection is the same optimisation
     * rectclip already gets: a device pixel outside the mask contributes
     * nothing to `dst` no matter what, so there is no reason to sweep it,
     * blend it, and multiply it by zero when it can be excluded from the
     * window up front. gfx_fill_mask_clipped cannot take this shortcut
     * because its contract is to fill the exact w x h buffer the caller
     * asked for, clip and all; this one has no such promise to keep.
     *
     * Gated out under -DGFX_NO_CLIP along with the row_fn multiply itself
     * (clip_row, above): this window narrowing is USING the clip mask (its
     * extent), same as the multiply uses its content, so the negative
     * control has to bypass both or a hard clip-window edge would keep
     * reading as clipped -- correctly, but for the wrong reason -- purely
     * from this optimisation, with the multiply it's supposed to be testing
     * never even running. GFX_NO_CLIP's job is "every containment assertion
     * FAILS", not "most of them". */
#ifndef GFX_NO_CLIP
    if (clip->w <= 0 || clip->h <= 0) return 1;   /* empty clip: nothing to do */
    if (clip->ox > cx0) cx0 = clip->ox;
    if (clip->oy > cy0) cy0 = clip->oy;
    if (clip->ox + clip->w < cx1) cx1 = clip->ox + clip->w;
    if (clip->oy + clip->h < cy1) cy1 = clip->oy + clip->h;
#else
    if (clip->w <= 0 || clip->h <= 0) return 1;   /* still a real refusal-adjacent
                                                   * case, not clip BEHAVIOUR --
                                                   * an empty mask has no content
                                                   * to ignore either way */
#endif
    if (cx1 <= cx0 || cy1 <= cy0) return 1;

    struct fill_sink_clipped f;
    f.dst = dst; f.paint = paint; f.x0 = cx0; f.x1 = cx1; f.clip = clip;
    return raster(p, rule, cx0, cy0, cy1, cx1 - cx0, subs, fill_row_clipped, &f);
}
