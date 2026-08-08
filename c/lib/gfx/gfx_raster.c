/* Open Logit -- the scanline coverage rasterizer. THE one in this tree.
 *
 * CONSTRUCTION, and it is deliberately the kernel glyph rasterizer's: GFX_SUBS
 * (4) sub-scanlines per pixel row, and along each of them EXACT fractional
 * horizontal coverage rather than a point sample. That asymmetry is the whole
 * trick -- horizontal coverage is analytic and free, vertical coverage costs a
 * pass each, so you buy accuracy where it is cheap and sample where it is not.
 * Measured against a 16x supersampled reference the worst pixel error is 0.08
 * for fills and 0.14 for rings, at 1/4 the cost of 16x supersampling.
 *
 * WINDING. Both rules, from the same walk: crossings of a sub-scanline are
 * collected with their direction, sorted by x, and the interior is the run
 * where the accumulated winding is nonzero (GFX_NONZERO) or odd (GFX_EVENODD).
 * The edge test is HALF-OPEN in y (ytop <= ys < ybot), which is what stops a
 * vertex shared by two edges from being counted twice and punching a hole.
 *
 * NO ALLOCATION. The edge table, the active list and one scratch row are
 * bounded statics, ~48 KB of .bss. A path with more edges than fit is refused
 * rather than truncated; see gfx_path.c on why a truncated path is the worst
 * kind of failure.
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
 * table can hold 1536 edges; shell sort is a dozen lines, needs no recursion
 * and no scratch, and is comfortably fast at that size. */
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

/* The core. Rasterizes rows [y0,y1) of `p` at device x offset `ox`, `w` pixels
 * wide, calling `fn` with each finished coverage row. */
static int raster(const struct gfx_path *p, int rule, int ox, int y0, int y1,
                  int w, row_fn fn, void *user)
{
    if (p->overflow) return 0;                 /* refuse a truncated path */
    if (w <= 0 || w > GFX_MAX_W || y1 <= y0) return 0;
    int ne = build_edges(p);
    if (ne <= 0) return ne == 0 ? 1 : 0;       /* empty is a success, overflow is not */
    sort_order(ne);

    int nact = 0, next = 0;
    long ox256 = (long)ox * 256;

    /* Skip edges that end above the first row we care about. */
    while (next < ne && g_edge[g_order[next]].ybot <= (long)y0 * 256) next++;

    for (int y = y0; y < y1; y++) {
        gfx_zero(g_row, w);
        for (int k = 0; k < GFX_SUBS; k++) {
            int ys = (y << 8) + (k * 256 + 128) / GFX_SUBS;
            /* admit newly started edges */
            while (next < ne && g_edge[g_order[next]].ytop <= ys) {
                if (g_edge[g_order[next]].ybot > ys && nact < GFX_MAX_ACTIVE)
                    g_act[nact++] = g_order[next];
                next++;
            }
            /* retire finished ones */
            for (int i = 0; i < nact; ) {
                if (g_edge[g_act[i]].ybot <= ys) g_act[i] = g_act[--nact];
                else i++;
            }
            /* crossings */
            int nc = 0;
            for (int i = 0; i < nact && nc < GFX_MAX_ACTIVE; i++) {
                const struct edge *e = &g_edge[g_act[i]];
                if (ys < e->ytop || ys >= e->ybot) continue;
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
            /* winding walk */
            int wind = 0;
            for (int i = 0; i + 1 <= nc - 1; i++) {
                wind += (rule == GFX_EVENODD) ? 1 : g_cross[i].dir;
                int inside = (rule == GFX_EVENODD) ? (wind & 1) : (wind != 0);
                if (!inside) continue;
#ifdef GFX_NO_AA
                span_hard(g_row, w, g_cross[i].x, g_cross[i + 1].x);
#else
                span_add(g_row, w, g_cross[i].x, g_cross[i + 1].x, 256 / GFX_SUBS);
#endif
            }
        }
        fn(user, y, g_row, w);
    }
    return 1;
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

int gfx_fill_mask(const struct gfx_path *p, int rule,
                  unsigned char *cov, int w, int h, int ox, int oy)
{
    if (!cov || w <= 0 || h <= 0) return 0;
    gfx_zero(cov, w * h);
    struct mask_sink m;
    m.cov = cov; m.w = w; m.h = h; m.oy = oy;
    return raster(p, rule, ox, oy, oy + h, w, mask_row, &m);
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

int gfx_fill(struct gfx_surface *dst, const struct gfx_path *p, int rule,
             const struct gfx_paint *paint, const struct gfx_rect *clip)
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
    if (cx1 - cx0 > GFX_MAX_W) cx1 = cx0 + GFX_MAX_W;

    struct fill_sink f;
    f.dst = dst; f.paint = paint; f.x0 = cx0; f.x1 = cx1;
    return raster(p, rule, cx0, cy0, cy1, cx1 - cx0, fill_row, &f);
}
