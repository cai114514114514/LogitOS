/* ol_display snapshot groups (c/lib/gfx/adapters/display_group.c).
 *
 * Every case draws the same random scene (rects, glyph coverage, gradients,
 * rounded rects, reaching past the group) twice: once inside a group on the
 * target under test, once straight onto an oracle copy clipped to the group's
 * rect. The group's result must be lerp(pre, drawn, w) per pixel within 2/255,
 * w computed in double, and every pixel outside the rect must be untouched.
 *
 *   opacity-lerp   (a) opacity groups, square corners, w = opacity/255
 *   rrect-corner   (b) opacity-255 rounded clips: r=24, 15, 17, 27 (125%/150%
 *                  scaling), mixed per-corner radii and r=180, the analytic
 *                  path past GFX_MASK_MAX
 *   nested-product (c) three nested groups, w = w1*w2*w3 (and a variant that
 *                  draws at every level, against the composed lerps)
 *   interleave     two displays on two gfx contexts, groups using the
 *                  per-display scratch slot, the hook switching contexts
 *                  mid-group
 *   plus limit, argument, bytes, lifo, clip-restored, outside-untouched and
 *   round-rect-match (a group around a fill paints ol_display_round_rect's
 *   corner).
 *
 * THE COVERAGE ORACLE. For r <= GFX_MASK_MAX the group's coverage is, by
 * design, the engine's own corner tile gfx_mask_corner(GFX_MASK_FILL, r, r):
 * a group must clip content to exactly the corner ol_display_round_rect paints
 * for a frame of the same radius. That tile is a 4-sub-scanline raster
 * (GFX_SUBS) and is up to ~20/255 away from the true pixel area, so for those
 * radii the oracle takes its coverage from the tile and checks everything
 * else in double (the deviation is printed as info). For r > GFX_MASK_MAX the
 * oracle is a 512-sub-row supersampled quarter circle with exact horizontal
 * coverage, i.e. the true area to well under 1/255.
 *
 * Built with -DGFX_NCTX=2 and without fb.c, so it defines gfx_ctx_hook.
 * -DOL_DISPLAY_GROUP_NO_RESTORE (the negative control) makes end() keep the
 * drawn pixels; opacity-lerp and rrect-corner must then FAIL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "openlogit_display.h"
#include "openlogit_display_group.h"
#include "gfx_mask.h"

int (*gfx_ctx_hook)(void);
static int g_ctx;
static int hook(void) { return g_ctx; }

static int fail_alloc;
static void *t_alloc(unsigned long n) { return fail_alloc ? 0 : malloc(n); }
static void t_free(void *p) { free(p); }

static int bad;
static void fail(const char *check, const char *what)
{
    printf("FAIL %s (%s)\n", check, what);
    bad = 1;
}

#define TW 520
#define TH 440

struct layout { unsigned r, g, b; const char *name; };
static const struct layout BGRX = { 16, 8, 0, "bgrx" };
static const struct layout ODD = { 0, 10, 20, "r0g10b20" };   /* scalar path */

static unsigned rng_s;
static unsigned rnd(void)
{
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 17; rng_s ^= rng_s << 5;
    return rng_s;
}

static void fill_noise(uint32_t *px, const struct layout *L, unsigned seed)
{
    rng_s = seed | 1;
    for (int i = 0; i < TW * TH; i++) {
        unsigned v = rnd();
        px[i] = (v & 255) << L->r | ((v >> 8) & 255) << L->g | ((v >> 16) & 255) << L->b;
    }
}

/* Items [lo, hi) of the random scene for `seed`. */
static void draw_scene(struct ol_display *d, unsigned seed, int lo, int hi)
{
    static uint8_t cov[64 * 64];
    rng_s = seed | 1;
    for (int n = 0; n < hi; n++) {
        int kind = (int)(rnd() % 4);
        int x = (int)(rnd() % (TW + 80)) - 40, y = (int)(rnd() % (TH + 80)) - 40;
        int w = 10 + (int)(rnd() % 300), h = 10 + (int)(rnd() % 250);
        unsigned c1 = rnd(), c2 = rnd();
        uint32_t a = ol_display_rgb(d, c1 & 255, (c1 >> 8) & 255, (c1 >> 16) & 255);
        uint32_t b = ol_display_rgb(d, c2 & 255, (c2 >> 8) & 255, (c2 >> 16) & 255);
        int gw = 8 + (int)(rnd() % 56), gh = 8 + (int)(rnd() % 56), rr = (int)(rnd() % 30);
        if (kind == 2)
            for (int i = 0; i < gw * gh; i++) {
                unsigned v = rnd() & 511;
                cov[i] = (uint8_t)(v < 64 ? 0 : v > 448 ? 255 : v & 255);
            }
        if (n < lo) continue;
        if (kind == 0) ol_display_fill_rect(d, x, y, w, h, a);
        else if (kind == 1) ol_display_fill_vgrad(d, x, y, w, h, a, b);
        else if (kind == 2) ol_display_blit_glyph(d, x, y, cov, gw, gh, a);
        else ol_display_round_rect(d, x, y, w, h, rr, a);
    }
}

struct gspec { int x, y, w, h; int rad[4]; int opacity; };

/* 512 sub-rows, exact horizontal coverage: the quarter circle of radius r
 * centred on (r, r), pixel (cx, cy) measured from the outer corner. */
static double ss_cov(int r, int cx, int cy)
{
    double t = 0;
    for (int s = 0; s < 512; s++) {
        double y = cy + (s + 0.5) / 512, dy = r - y;
        double xb = r - sqrt((double)r * r - dy * dy), f = cx + 1 - xb;
        t += f < 0 ? 0 : f > 1 ? 1 : f;
    }
    return t / 512;
}

static double ref_cov(int r, int cx, int cy)
{
    if (r <= GFX_MASK_MAX) return gfx_mask_corner(GFX_MASK_FILL, r, r, 0)[cy * r + cx] / 255.0;
    return ss_cov(r, cx, cy);
}

/* Group weight at device (X, Y); -1 outside the rect. */
static double gweight(const struct gspec *g, int X, int Y)
{
    int li = X - g->x, lj = Y - g->y, w = g->w, h = g->h;
    if (li < 0 || lj < 0 || li >= w || lj >= h) return -1;
    int lim = (w < h ? w : h) / 2, r[4];
    for (int k = 0; k < 4; k++) r[k] = g->rad[k] > lim ? lim : g->rad[k];
    double c = 1;
    if (li < r[0] && lj < r[0]) c = ref_cov(r[0], li, lj);
    else if (li >= w - r[1] && lj < r[1]) c = ref_cov(r[1], w - 1 - li, lj);
    else if (li >= w - r[2] && lj >= h - r[2]) c = ref_cov(r[2], w - 1 - li, h - 1 - lj);
    else if (li < r[3] && lj >= h - r[3]) c = ref_cov(r[3], li, h - 1 - lj);
    return g->opacity / 255.0 * c;
}

struct clip { int on, x0, y0, x1, y1; };

static void isect(struct clip *c, int x0, int y0, int x1, int y1)
{
    if (!c->on) { c->on = 1; c->x0 = 0; c->y0 = 0; c->x1 = TW; c->y1 = TH; }
    if (x0 > c->x0) c->x0 = x0;
    if (y0 > c->y0) c->y0 = y0;
    if (x1 < c->x1) c->x1 = x1;
    if (y1 < c->y1) c->y1 = y1;
}

static int in_clip(const struct clip *c, int x, int y)
{
    return x >= c->x0 && x < c->x1 && y >= c->y0 && y < c->y1;
}

static void bind(struct ol_display *d, uint32_t *px, const struct layout *L, const struct clip *c)
{
    memset(d, 0, sizeof *d);
    d->allocate = t_alloc;
    d->release = t_free;
    struct ol_pixel_target t = { px, TW, TH, 0, 0, 0, 0, 0 };
    if (c && c->on) { t.clip_on = 1; t.clx0 = c->x0; t.cly0 = c->y0; t.clx1 = c->x1; t.cly1 = c->y1; }
    if (ol_display_bind(d, &t, L->r, L->g, L->b) != OL_OK) { fail("bind", L->name); exit(2); }
}

static int ch(uint32_t p, unsigned pos) { return (int)((p >> pos) & 255); }

/* Compare act against lerp(pre, val) where val(x,y) = model[] in double,
 * inside `in`; outside it act must equal pre exactly. Returns worst error. */
static double compare(const char *check, const char *name, const struct layout *L,
                      const uint32_t *pre, const uint32_t *act, const double *model,
                      const struct clip *in)
{
    double worst = 0;
    int wx = 0, wy = 0;
    long outside_bad = 0;
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TW; x++) {
            int i = y * TW + x;
            if (!in_clip(in, x, y)) {
                if (act[i] != pre[i]) outside_bad++;
                continue;
            }
            unsigned pos[3] = { L->r, L->g, L->b };
            for (int c = 0; c < 3; c++) {
                double e = fabs(ch(act[i], pos[c]) - model[i * 3 + c]);
                if (e > worst) { worst = e; wx = x; wy = y; }
            }
        }
    char buf[160];
    if (outside_bad) {
        snprintf(buf, sizeof buf, "%s: %ld pixels outside the rect changed", name, outside_bad);
        fail("outside-untouched", buf);
    }
    if (worst > 2.0) {
        snprintf(buf, sizeof buf, "%s: worst %.2f/255 at %d,%d", name, worst, wx, wy);
        fail(check, buf);
    }
    printf("  %-15s %-28s worst %.3f/255\n", check, name, worst);
    return worst;
}

static int clip_is(const struct ol_display *d, const struct clip *c)
{
    const struct ol_pixel_target *t = &d->target;
    if (!c || !c->on) return t->clip_on == 0;
    return t->clip_on == 1 && t->clx0 == c->x0 && t->cly0 == c->y0 && t->clx1 == c->x1 && t->cly1 == c->y1;
}

/* Up to three nested groups; the scene items are split across levels as
 * draw[k] = {lo, hi} (drawn after begin k). */
static void run_nested(const char *check, const char *name, const struct layout *L,
                       const struct gspec *gs, int n, const int (*draw)[2],
                       const struct clip *pclip, unsigned seed, int scratch)
{
    uint32_t *pre = malloc(TW * TH * 4), *act = malloc(TW * TH * 4), *orc = malloc(TW * TH * 4);
    uint32_t *stage[3];
    double *model = malloc(sizeof(double) * TW * TH * 3);
    fill_noise(pre, L, seed);
    memcpy(act, pre, TW * TH * 4);
    memcpy(orc, pre, TW * TH * 4);

    /* Oracle: draw each level's items clipped to the running intersection,
     * remembering the destination at each begin. */
    struct clip run = pclip ? *pclip : (struct clip){ 0, 0, 0, 0, 0 }, lv[3];
    struct ol_display od;
    for (int k = 0; k < n; k++) {
        stage[k] = malloc(TW * TH * 4);
        memcpy(stage[k], orc, TW * TH * 4);
        isect(&run, gs[k].x, gs[k].y, gs[k].x + gs[k].w, gs[k].y + gs[k].h);
        lv[k] = run;
        bind(&od, orc, L, &run);
        draw_scene(&od, seed * 7 + 3, draw[k][0], draw[k][1]);
    }
    /* lerp(stage0, lerp(stage1, lerp(stage2, drawn, w2), w1), w0) */
    unsigned pos[3] = { L->r, L->g, L->b };
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TW; x++) {
            int i = y * TW + x;
            for (int c = 0; c < 3; c++) {
                double v = ch(orc[i], pos[c]);
                for (int k = n - 1; k >= 0; k--) {
                    double w = in_clip(&lv[k], x, y) ? gweight(&gs[k], x, y) : 0;
                    double s = ch(stage[k][i], pos[c]);
                    v = s + (v - s) * w;
                }
                model[i * 3 + c] = v;
            }
        }

    /* Under test. */
    struct ol_display d;
    struct ol_display_group g[3];
    uint32_t *snap[3] = { 0, 0, 0 };
    struct clip before[3];
    bind(&d, act, L, pclip);
    int ok = 1;
    for (int k = 0; k < n; k++) {
        unsigned long nb = ol_display_group_bytes(gs[k].w, gs[k].h, gs[k].rad, gs[k].opacity);
        if (!scratch) snap[k] = malloc(nb ? nb : 4);
        before[k] = (struct clip){ d.target.clip_on, d.target.clx0, d.target.cly0, d.target.clx1, d.target.cly1 };
        int rc = ol_display_group_begin(&d, &g[k], gs[k].x, gs[k].y, gs[k].w, gs[k].h, gs[k].rad,
                                        gs[k].opacity, snap[k], nb);
        if (rc != OL_OK) { fail(check, "begin failed"); ok = 0; break; }
        draw_scene(&d, seed * 7 + 3, draw[k][0], draw[k][1]);
    }
    for (int k = n - 1; ok && k >= 0; k--) {
        if (ol_display_group_end(&d, &g[k]) != OL_OK) fail(check, "end failed");
        if (!clip_is(&d, before[k].on ? &before[k] : 0)) fail("clip-restored", name);
    }
    struct clip all = { 1, 0, 0, TW, TH };
    compare(check, name, L, pre, act, model, &all);
    /* pixels outside the outermost effective rect: exact */
    struct clip outer = lv[0];
    long changed = 0;
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TW; x++)
            if (!in_clip(&outer, x, y) && act[y * TW + x] != pre[y * TW + x]) changed++;
    if (changed) fail("outside-untouched", name);

    for (int k = 0; k < n; k++) { free(stage[k]); free(snap[k]); }
    ol_display_destroy(&d);
    free(pre); free(act); free(orc); free(model);
}

static void run_one(const char *check, const char *name, const struct layout *L,
                    struct gspec g, const struct clip *pclip, unsigned seed)
{
    static const int all[1][2] = { { 0, 24 } };
    run_nested(check, name, L, &g, 1, all, pclip, seed, 0);
}

/* A group at opacity 255 around a full fill paints the same corner as
 * ol_display_round_rect (blend_cov truncates where the group rounds: <= 1). */
static void round_rect_match(int r)
{
    uint32_t *a = malloc(TW * TH * 4), *b = malloc(TW * TH * 4);
    fill_noise(a, &BGRX, 99 + r);
    memcpy(b, a, TW * TH * 4);
    struct ol_display da, db;
    bind(&da, a, &BGRX, 0);
    bind(&db, b, &BGRX, 0);
    uint32_t col = ol_display_rgb(&da, 250, 30, 120);
    int rad[4] = { r, r, r, r };
    uint32_t snap[4 * 72 * 72];
    struct ol_display_group g;
    if (ol_display_group_begin(&da, &g, 33, 21, 301, 207, rad, 255, snap, sizeof snap) != OL_OK)
        fail("round-rect-match", "begin");
    ol_display_fill_rect(&da, 0, 0, TW, TH, col);
    ol_display_group_end(&da, &g);
    ol_display_round_rect(&db, 33, 21, 301, 207, r, col);
    int worst = 0;
    for (int i = 0; i < TW * TH; i++)
        for (unsigned p = 0; p <= 16; p += 8) {
            int e = abs(ch(a[i], p) - ch(b[i], p));
            if (e > worst) worst = e;
        }
    char buf[64];
    snprintf(buf, sizeof buf, "r=%d worst %d/255", r, worst);
    if (worst > 1) fail("round-rect-match", buf);
    printf("  %-15s %-28s worst %d/255\n", "round-rect-match", buf, worst);
    free(a); free(b);
}

/* Two displays, two contexts, scratch snapshots; the hook switches contexts
 * between calls while both groups are open. */
static void interleave(void)
{
    const struct gspec ga = { 40, 30, 300, 260, { 24, 24, 24, 24 }, 128 };
    const struct gspec gb = { 70, 20, 400, 390, { 17, 15, 27, 90 }, 200 };
    uint32_t *pa = malloc(TW * TH * 4), *pb = malloc(TW * TH * 4);
    uint32_t *aa = malloc(TW * TH * 4), *ab = malloc(TW * TH * 4);
    uint32_t *oa = malloc(TW * TH * 4), *ob = malloc(TW * TH * 4);
    double *ma = malloc(sizeof(double) * TW * TH * 3), *mb = malloc(sizeof(double) * TW * TH * 3);
    fill_noise(pa, &BGRX, 501); fill_noise(pb, &BGRX, 502);
    memcpy(aa, pa, TW * TH * 4); memcpy(oa, pa, TW * TH * 4);
    memcpy(ab, pb, TW * TH * 4); memcpy(ob, pb, TW * TH * 4);
    struct clip ca = { 0 }, cb = { 0 };
    isect(&ca, ga.x, ga.y, ga.x + ga.w, ga.y + ga.h);
    isect(&cb, gb.x, gb.y, gb.x + gb.w, gb.y + gb.h);
    struct ol_display od;
    g_ctx = 0; bind(&od, oa, &BGRX, &ca); draw_scene(&od, 601, 0, 24);
    g_ctx = 1; bind(&od, ob, &BGRX, &cb); draw_scene(&od, 602, 0, 24);
    for (int i = 0; i < TW * TH; i++)
        for (int c = 0; c < 3; c++) {
            unsigned p = c * 8;
            int x = i % TW, y = i / TW;
            double wa = in_clip(&ca, x, y) ? gweight(&ga, x, y) : 0;
            double wb = in_clip(&cb, x, y) ? gweight(&gb, x, y) : 0;
            ma[i * 3 + 2 - c] = ch(pa[i], p) + (ch(oa[i], p) - ch(pa[i], p)) * wa;
            mb[i * 3 + 2 - c] = ch(pb[i], p) + (ch(ob[i], p) - ch(pb[i], p)) * wb;
        }

    struct ol_display a, b;
    struct ol_display_group g1, g2;
    bind(&a, aa, &BGRX, 0);
    bind(&b, ab, &BGRX, 0);
    g_ctx = 0;
    int r1 = ol_display_group_begin(&a, &g1, ga.x, ga.y, ga.w, ga.h, ga.rad, ga.opacity, 0, 0);
    draw_scene(&a, 601, 0, 10);
    g_ctx = 1;
    int r2 = ol_display_group_begin(&b, &g2, gb.x, gb.y, gb.w, gb.h, gb.rad, gb.opacity, 0, 0);
    draw_scene(&b, 602, 0, 12);
    g_ctx = 0;
    draw_scene(&a, 601, 10, 24);
    g_ctx = 1;
    draw_scene(&b, 602, 12, 24);
    int r3 = ol_display_group_end(&b, &g2);
    g_ctx = 0;
    int r4 = ol_display_group_end(&a, &g1);
    if (r1 || r2 || r3 || r4) fail("interleave", "status");
    if (!(g1.flags & OL_DISPLAY_GROUP_SCRATCH) || !(g2.flags & OL_DISPLAY_GROUP_SCRATCH) ||
        g1.snap == g2.snap)
        fail("interleave", "scratch slots not per display");
    struct clip all = { 1, 0, 0, TW, TH };
    compare("interleave", "display A ctx 0", &BGRX, pa, aa, ma, &all);
    compare("interleave", "display B ctx 1", &BGRX, pb, ab, mb, &all);
    ol_display_destroy(&a); ol_display_destroy(&b);
    free(pa); free(pb); free(aa); free(ab); free(oa); free(ob); free(ma); free(mb);
    g_ctx = 0;
}

static void api_checks(void)
{
    static uint32_t px[TW * TH];
    struct ol_display d;
    struct ol_display_group g, g2;
    struct clip c0 = { 1, 5, 6, 400, 300 };
    bind(&d, px, &BGRX, &c0);
    const int r24[4] = { 24, 24, 24, 24 }, rbig[4] = { 500, 3, 0, 9 }, rneg[4] = { 1, -1, 0, 0 };

    /* bytes */
    int okb = ol_display_group_bytes(100, 80, r24, 255) == 4ul * 4 * 24 * 24 &&
              ol_display_group_bytes(100, 80, r24, 254) == 4ul * 100 * 80 &&
              ol_display_group_bytes(100, 80, 0, 255) == 0 &&
              ol_display_group_bytes(100, 80, rbig, 255) == 4ul * (40 * 40 + 9 + 81) &&
              ol_display_group_bytes(100, 80, rneg, 255) == 0 &&
              ol_display_group_bytes(100, 80, r24, 256) == 0 &&
              ol_display_group_bytes(-1, 80, r24, 128) == 0;
    if (!okb) fail("bytes", "ol_display_group_bytes values");

    /* limit: a short buffer and a failing scratch allocation change nothing */
    uint32_t snap[4 * 24 * 24];
    int rc = ol_display_group_begin(&d, &g, 10, 10, 100, 80, r24, 255, snap, sizeof snap - 1);
    if (rc != OL_LIMIT || (g.flags & OL_DISPLAY_GROUP_ACTIVE) || !clip_is(&d, &c0))
        fail("limit", "short caller buffer");
    fail_alloc = 1;
    rc = ol_display_group_begin(&d, &g, 10, 10, 100, 80, r24, 128, 0, 0);
    fail_alloc = 0;
    if (rc != OL_LIMIT || (g.flags & OL_DISPLAY_GROUP_ACTIVE) || !clip_is(&d, &c0))
        fail("limit", "scratch allocation failure");
    rc = ol_display_group_begin(&d, &g, 10, 10, 100, 80, r24, 255, snap, sizeof snap);
    if (rc != OL_OK) fail("limit", "exact-size buffer refused");
    else ol_display_group_end(&d, &g);

    /* argument */
    if (ol_display_group_begin(0, &g, 0, 0, 10, 10, 0, 128, snap, sizeof snap) != OL_ARGUMENT ||
        ol_display_group_begin(&d, 0, 0, 0, 10, 10, 0, 128, snap, sizeof snap) != OL_ARGUMENT ||
        ol_display_group_begin(&d, &g, 0, 0, 10, 10, 0, 256, snap, sizeof snap) != OL_ARGUMENT ||
        ol_display_group_begin(&d, &g, 0, 0, 10, 10, 0, -1, snap, sizeof snap) != OL_ARGUMENT ||
        ol_display_group_begin(&d, &g, 0, 0, 10, 10, rneg, 255, snap, sizeof snap) != OL_ARGUMENT ||
        ol_display_group_begin(&d, &g, 0, 0, -3, 10, 0, 255, snap, sizeof snap) != OL_ARGUMENT ||
        ol_display_group_end(&d, &g) != OL_ARGUMENT || !clip_is(&d, &c0))
        fail("argument", "invalid arguments accepted");

    /* clip: begin intersects, end restores; an empty group draws nothing */
    rc = ol_display_group_begin(&d, &g, 300, 250, 200, 100, 0, 255, 0, 0);
    struct clip want = { 1, 300, 250, 400, 300 };
    if (rc != OL_OK || !clip_is(&d, &want)) fail("clip-restored", "begin intersection");
    if (ol_display_group_end(&d, &g) != OL_OK || !clip_is(&d, &c0)) fail("clip-restored", "end");
    rc = ol_display_group_begin(&d, &g, 450, 10, 20, 20, 0, 128, snap, sizeof snap);
    uint32_t before = px[15 * TW + 455];
    ol_display_fill_rect(&d, 0, 0, TW, TH, 0x123456);
    if (rc != OL_OK || ol_display_group_end(&d, &g) != OL_OK || !clip_is(&d, &c0))
        fail("clip-restored", "empty group");
    if (px[15 * TW + 455] != before) fail("outside-untouched", "empty group wrote");

    /* lifo: ending the outer group while the inner one is open is reported */
    uint32_t s1[4 * 24 * 24], s2[4 * 24 * 24];
    ol_display_group_begin(&d, &g, 10, 10, 200, 200, r24, 255, s1, sizeof s1);
    ol_display_group_begin(&d, &g2, 20, 20, 100, 100, r24, 255, s2, sizeof s2);
    if (ol_display_group_end(&d, &g) != OL_ARGUMENT) fail("lifo", "out-of-order end not reported");
    if (!clip_is(&d, &c0)) fail("lifo", "outer end did not restore the clip");
    g2.flags = 0;

    /* a nested scratch group that grows the slot frees the outer snapshot:
     * end() must notice and refuse to read it */
    ol_display_destroy(&d);
    bind(&d, px, &BGRX, 0);
    ol_display_group_begin(&d, &g, 10, 10, 20, 20, 0, 128, 0, 0);
    ol_display_group_begin(&d, &g2, 10, 10, 200, 200, 0, 128, 0, 0);
    ol_display_group_end(&d, &g2);
    if (ol_display_group_end(&d, &g) != OL_ARGUMENT || !clip_is(&d, 0))
        fail("scratch", "regrown scratch snapshot not detected");
    ol_display_destroy(&d);
    printf("  api checks done\n");
}

static void info_mask_deviation(void)
{
    const int rs[] = { 15, 17, 24, 27, 72 };
    for (unsigned k = 0; k < sizeof rs / sizeof rs[0]; k++) {
        int r = rs[k];
        const unsigned char *m = gfx_mask_corner(GFX_MASK_FILL, r, r, 0);
        double mx = 0;
        for (int j = 0; j < r; j++)
            for (int i = 0; i < r; i++) {
                double e = fabs(m[j * r + i] - 255 * ss_cov(r, i, j));
                if (e > mx) mx = e;
            }
        printf("  info: gfx_mask_corner r=%d vs supersampled area: max %.2f/255\n", r, mx);
    }
}

int main(void)
{
    gfx_ctx_hook = hook;
    struct clip cut = { 1, 60, 50, 300, 200 };          /* cuts through corners */

    /* (a) opacity */
    run_one("opacity-lerp", "a=128 bgrx", &BGRX, (struct gspec){ 40, 30, 380, 300, { 0 }, 128 }, 0, 11);
    run_one("opacity-lerp", "a=128 bgrx clipped", &BGRX, (struct gspec){ 40, 30, 380, 300, { 0 }, 128 }, &cut, 12);
    run_one("opacity-lerp", "a=128 r0g10b20", &ODD, (struct gspec){ 40, 30, 380, 300, { 0 }, 128 }, 0, 13);
    run_one("opacity-lerp", "a=77 off-target", &BGRX, (struct gspec){ -50, -40, 300, 250, { 0 }, 77 }, 0, 14);
    run_one("opacity-lerp", "a=200 r=24", &BGRX, (struct gspec){ 40, 30, 380, 300, { 24, 24, 24, 24 }, 200 }, 0, 15);
    run_one("opacity-lerp", "a=0", &BGRX, (struct gspec){ 40, 30, 380, 300, { 0 }, 0 }, 0, 16);

    /* (b) rounded clips at opacity 255 */
    run_one("rrect-corner", "r=24", &BGRX, (struct gspec){ 40, 30, 380, 300, { 24, 24, 24, 24 }, 255 }, 0, 21);
    run_one("rrect-corner", "r=24 clipped", &BGRX, (struct gspec){ 40, 30, 380, 300, { 24, 24, 24, 24 }, 255 }, &cut, 22);
    run_one("rrect-corner", "r=15 (125%)", &BGRX, (struct gspec){ 41, 31, 301, 211, { 15, 15, 15, 15 }, 255 }, 0, 23);
    run_one("rrect-corner", "r=17", &BGRX, (struct gspec){ 41, 31, 301, 211, { 17, 17, 17, 17 }, 255 }, 0, 24);
    run_one("rrect-corner", "r=27 (150%)", &BGRX, (struct gspec){ 41, 31, 301, 211, { 27, 27, 27, 27 }, 255 }, 0, 25);
    run_one("rrect-corner", "r=180 analytic", &BGRX, (struct gspec){ 60, 30, 400, 380, { 180, 180, 180, 180 }, 255 }, 0, 26);
    run_one("rrect-corner", "r=24/17/0/90 mixed", &BGRX, (struct gspec){ 30, 20, 400, 380, { 24, 17, 0, 90 }, 255 }, 0, 27);
    run_one("rrect-corner", "r=27 r0g10b20", &ODD, (struct gspec){ 41, 31, 301, 211, { 27, 27, 27, 27 }, 255 }, 0, 28);
    run_one("rrect-corner", "r=180 a=128", &BGRX, (struct gspec){ 60, 30, 400, 380, { 180, 180, 180, 180 }, 128 }, 0, 29);

    /* (c) nesting */
    {
        const struct gspec n3[3] = {
            { 20, 20, 460, 380, { 24, 24, 24, 24 }, 200 },
            { 60, 40, 420, 300, { 17, 17, 17, 17 }, 255 },
            { 30, 70, 300, 340, { 90, 15, 27, 0 }, 160 },
        };
        const int inner[3][2] = { { 0, 0 }, { 0, 0 }, { 0, 24 } };
        const int every[3][2] = { { 0, 8 }, { 8, 16 }, { 16, 24 } };
        run_nested("nested-product", "3 levels, draw innermost", &BGRX, n3, 3, inner, 0, 31, 0);
        run_nested("nested-product", "3 levels, draw every level", &BGRX, n3, 3, every, 0, 32, 0);
        run_nested("nested-product", "3 levels clipped", &BGRX, n3, 3, inner, &cut, 33, 0);
    }

    round_rect_match(15);
    round_rect_match(24);
    interleave();
    api_checks();
    info_mask_deviation();

    if (!bad) puts("display_group_test: ok");
    return bad;
}
