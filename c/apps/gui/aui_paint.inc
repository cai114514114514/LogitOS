/* aui_paint.inc -- shape, gradient and shadow drawing, and its statics.
 * Unity-included by aui.c (one translation unit): every static it names is
 * file-scope in aui.c, and the variant builds (-DAUI_NO_AA, -DAUI_COST, ...)
 * reach it automatically. Moved verbatim; do not compile on its own. */

/* Included TWICE from aui.c, at the two positions the code originally sat:
 * with AUI_PAINT_MASK defined (the mask-tile plumbing, before the frame-state
 * globals) and without it (the drawing primitives, after them). */
#ifdef AUI_PAINT_MASK
/* ---- getting a mask onto the screen ---- */
static unsigned char rgba_buf[MASK_MAX * MASK_MAX * 4];
#define GRAD_MAX 1024
static unsigned char grad_buf[GRAD_MAX * 4];

/* Blit a coverage tile into the point rect (x,y,w,h), optionally mirrored, out
 * of a CALLER-CHOSEN scratch buffer. Parameterized on the buffer (rather than
 * always writing rgba_buf) because the uncached path below needs a second,
 * bigger buffer, and every widget that goes through the small one should not
 * pay for the big one's size -- see the BIG_MASK block for the buffer itself.
 * The source is generated at the rect's exact device size, so the kernel's
 * rescale is a no-op and the anti-aliasing survives at any backing scale. One
 * rasterized quadrant serves all four corners, which is what the mirroring is
 * for. */
static void blit_mask_buf(unsigned char *rbuf, long rcap, int x, int y, int w, int h,
                          const unsigned char *cov, int cw, int ch, unsigned color,
                          int alpha, int fx, int fy)
{
    if (!cov || cw <= 0 || ch <= 0 || w <= 0 || h <= 0) return;
    if ((long)cw * ch * 4 > rcap) return;
    gfx_mask_to_rgba(rbuf, cov, cw, ch, color, alpha, fx, fy);
    gui_blit(x, y, w, h, rbuf, cw, ch);
}
/* NOTE: there is deliberately no plain `blit_mask(...)` wrapper pinned to
 * rgba_buf any more. Every call site below now goes through corner_mask()
 * first, which hands back the RIGHT buffer (rgba_buf for a cached tile,
 * big_rgba for an uncached one) -- so every blit already has to know which
 * one it got, and a fixed-to-rgba_buf wrapper would just be a second, unused
 * name for the same call. */

/* ---- the uncached escape hatch ----
 *
 * gfx_mask_corner() REFUSES -- returns NULL, never a truncated or wrong mask
 * -- once a tile's device geometry passes GFX_MASK_MAX (72 px); see the
 * contract comment on it in gfx_mask.c, which also names the three honest
 * responses a caller has. This toolkit used to have exactly ONE of the three
 * (round_impl's square fallback, tier 3 below) and, worse, two call sites
 * that checked nothing at all -- see aui_stroke and aui_vgrad_round's own
 * comments for what that produced. This block is tier 2, the one that keeps
 * the actual requested shape instead of a worse one: gfx_corner_fill/
 * gfx_corner_ring/gfx_corner_shadow (aliased above as raster_*_corner) are
 * exactly what gfx_mask_corner calls internally to FILL the cache, and they
 * are public so a caller can call them directly into its OWN buffer when the
 * cache's fixed-size pool is the wrong shape for this one request -- "the
 * cache is a cache; nothing forces geometry through it."
 *
 * BIG_MASK is a real ceiling, not a promotion to "unlimited": this buffer is
 * a static too, paid by every app that links aui.c whether it ever needs this
 * path or not, so it stays a deliberate, bounded step up from GFX_MASK_MAX
 * rather than sized to the largest thing anyone could ask for. 256 device px
 * was chosen against the ONE thing that actually bounds it -- not display
 * size, not a guess: gfx_mask.c's corner generators share a single 512-point,
 * 8-subpath scratch path (`cpt`/`csub` in that file) for EVERY call
 * regardless of the tile size they are asked to fill, and that scratch is not
 * this file's to resize (it is also kernel .bss, via fb.c -- see gfx.h's
 * LIMITS section). A stroke corner (gfx_corner_ring, the worst case: two
 * nested arcs in one path) at 256 device px measured 260 of those 512 points
 * -- and, verified by walking the same construction across sizes rather than
 * guessing the slope from one sample, that count does not move off 260 again
 * until past 700 px; it does not reach the 512 ceiling (gfx_corner_ring's
 * overflow -> a silently blank tile, the failure mode below) until 913 px.
 * 256 leaves better than 3.5x headroom, not merely "over 2x" -- against a
 * failure mode this file could not even detect if it happened:
 * gfx_corner_fill/gfx_corner_ring both discard gfx_fill_mask's success/
 * refuse return value internally, so an actual overflow there would hand
 * back a silently BLANK tile, not a NULL this file could check for. Measured,
 * not round, for that reason -- c/lib/gfx is a different unit's file, so the
 * only lever available here is staying well inside what it already provably
 * handles rather than asking it to grow. */
#define BIG_MASK 256
static unsigned char big_cov[BIG_MASK * BIG_MASK];
static unsigned char big_rgba[BIG_MASK * BIG_MASK * 4];

/* gfx_mask.c's refusal counter (mrefuse, incremented at the exact point
 * gfx_mask_corner returns NULL) is not declared in gfx.h -- see the comment
 * on gfx_mask_refused() in gfx_mask.c for why -- so a caller in this
 * translation unit reaches it with its own one-line extern, the sound thing
 * to do across a TU boundary for a symbol that does exist and is exported.
 * ck_report() below reads it so a bench run can show "N tile requests could
 * not be served at all this session", which used to be a number nothing
 * could produce. */
extern int gfx_mask_refused(void);

/* THE SHARED DECISION every fixed call site below now makes, instead of each
 * accepting gfx_mask_corner's NULL as final in its own slightly different
 * (and, twice, badly wrong) way: try the cache, then the uncached BIG_MASK
 * tile, and only report "nothing" if NEITHER fits. `kind`/`param` are the
 * same vocabulary mask_get() already takes (MK_FILL/MK_STROKE/MK_SHADOW).
 * Hands back which scratch RGBA buffer the mask is good against via the
 * rbuf/rcap out-params, because a cached tile's data lives in `mdata` (bounded by
 * rgba_buf, MASK_MAX) and an uncached one lives in `big_cov` (bounded by
 * big_rgba, BIG_MASK) -- blit_mask_buf() needs to know which ceiling applies
 * or it silently refuses to blit a big tile through the small buffer's size
 * check.
 *
 * ONE MORE REASON this is a function and not four inlined copies:
 * tests/unit/aui_mask_test.c can call it directly, with no gui_* syscall
 * anywhere near it (see that file's own note on why no syscall in that TU is
 * ever allowed to fire). That is what makes aui_stroke's fix -- "past the
 * ceiling, try the uncached ring before giving up the corners" -- provable
 * from a host test rather than only from a screenshot: the test calls this
 * exact function with an oversized stroke radius and asserts what comes back
 * is a real, continuous ring, not NULL. */
static const unsigned char *corner_mask(int kind, int cw, int ch, int param,
                                        unsigned char **rbuf, long *rcap)
{
    const unsigned char *m = mask_get(kind, cw, ch, param);
    if (m) { *rbuf = rgba_buf; *rcap = (long)sizeof rgba_buf; return m; }
    if (cw <= 0 || ch <= 0 || cw > BIG_MASK || ch > BIG_MASK) return 0;
    if (kind == MK_FILL)        raster_fill_corner(big_cov, cw, ch);
    else if (kind == MK_STROKE) raster_stroke_corner(big_cov, cw, ch, param);
    else                        raster_shadow_corner(big_cov, cw, ch, param);
    *rbuf = big_rgba; *rcap = (long)sizeof big_rgba;
    return big_cov;
}

#elif !defined(AUI_PAINT_API2)
/* ----------------------------------------------- 4. drawing primitives */

#define X_(v) ((v) + ox_)
#define Y_(v) ((v) + oy_)

/* ======================================================================
 * GUI ABI v2 lowering (SYS_GUI_PATH).
 *
 * When SYS_GUI2_INFO(0) reports LOGIT_GUI2_F_PATH, the shape primitives below
 * go out as ONE ol_window_path2 call -- a rounded rect with alpha, a stroke,
 * an arc -- where the tile path (kept, for older kernels) needs a band per
 * edge plus four corner blits. Everything else about a shape is decided
 * exactly as before, in DEVICE pixels: the edges and corner radii are the
 * aui_dev()/devlen() numbers the tile path uses, converted back to the 24.8
 * points the wire carries by dev_to_pt24(), so the kernel's pct scaling
 * returns them to (within 1/256 px of) the same device coordinate and a
 * shape lands on the same pixels at 125% / 150% as it does at 100%.
 *
 * path_ok() asks the kernel once (one cached int: the only state this adds;
 * a capability bit cannot change under a running app). -DAUI_FORCE_TILE_PATH
 * is the negative control: it pretends the kernel never reports PATH. */
#if defined(AUI_NO_AA) || defined(AUI_FORCE_TILE_PATH)
#define path_ok() 0
#else
static int path_caps = -1;            /* -1 not asked yet, 0 no, 1 yes */
static int path_ok(void)
{
    if (path_caps < 0) {
        long i = ol_gui2_info(0);
        path_caps = (i > 0 && (i & LOGIT_GUI2_F_PATH)) ? 1 : 0;
    }
    return path_caps;
}
#endif

static long long ap_fdiv(long long n, long long d)
{
    long long q = n / d;
    return (n % d != 0 && n < 0) ? q - 1 : q;
}
static long long ap_abs(long long v) { return v < 0 ? -v : v; }

/* The 24.8 point value p whose kernel conversion floor(p * pct / 100) is
 * nearest the device 24.8 value d256 (exact whenever one exists: always at
 * 100%, and at every whole-pixel edge at 125/150/200%). */
static long long dev_to_pt24(long long d256)
{
    long long pct = aui_scale();
    long long p = -ap_fdiv(-d256 * 100, pct);            /* smallest p with p*pct/100 >= d256 */
    long long e0 = ap_fdiv(p * pct, 100);
    if (e0 != d256 && ap_abs(ap_fdiv((p - 1) * pct, 100) - d256) < ap_abs(e0 - d256)) p--;
    return p;
}

/* The smallest 24.8 point value whose kernel conversion is >= the device value
 * d256: what LOGIT_RECT_SNAP wants (it floors each converted edge to a whole
 * device pixel, so an edge at a whole device pixel d lands on exactly d). */
static long long dev_ceil_pt24(long long d256) { return -ap_fdiv(-d256 * 100, aui_scale()); }

/* A path under construction. coords are DEVICE 24.8 when .dev is set (and are
 * converted on the way in), otherwise already the 24.8 points of the wire.
 * Sized for the largest shape built here (rrect/squircle: 10 verbs, 17 points). */
#define AP_V  24
#define AP_XY 64
struct ap {
    unsigned char v[AP_V];
    int xy[AP_XY];
    int nv, nxy, dev;
};
static void ap_begin(struct ap *p, int dev) { p->nv = 0; p->nxy = 0; p->dev = dev; }
static void ap_pt(struct ap *p, long long x, long long y)
{
    if (p->nxy + 2 > AP_XY) return;
    if (p->dev) { x = dev_to_pt24(x); y = dev_to_pt24(y); }
    p->xy[p->nxy++] = (int)x; p->xy[p->nxy++] = (int)y;
}
static void ap_m(struct ap *p, long long x, long long y)
{ if (p->nv < AP_V) { p->v[p->nv++] = LOGIT_VERB_M; ap_pt(p, x, y); } }
static void ap_l(struct ap *p, long long x, long long y)
{ if (p->nv < AP_V) { p->v[p->nv++] = LOGIT_VERB_L; ap_pt(p, x, y); } }
static void ap_c(struct ap *p, long long x1, long long y1, long long x2, long long y2, long long x, long long y)
{
    if (p->nv < AP_V) {
        p->v[p->nv++] = LOGIT_VERB_C;
        ap_pt(p, x1, y1); ap_pt(p, x2, y2); ap_pt(p, x, y);
    }
}
static void ap_z(struct ap *p) { if (p->nv < AP_V) p->v[p->nv++] = LOGIT_VERB_Z; }

/* Rounded rect with per-corner elliptical radii (TL, TR, BR, BL), the corner
 * arc being the very cubic gfx_corner_fill rasterizes (kappa 36195/65536), so
 * a path-drawn corner has the tile path's coverage. */
static void ap_rrect(struct ap *p, long long x0, long long y0, long long x1, long long y1,
                     const long long rx[4], const long long ry[4])
{
#define AP_K(r) (((r) * 36195) >> 16)
    /* each corner: the line to the arc's start, then the arc (sx,sy)->(ex,ey)
     * with its two control points */
#define AP_CORNER(i, sx, sy, c1x, c1y, c2x, c2y, ex, ey)                              \
    do {                                                                              \
        ap_l(p, (sx), (sy));                                                          \
        if (rx[i] > 0 && ry[i] > 0)                                                   \
            ap_c(p, (c1x), (c1y), (c2x), (c2y), (ex), (ey));                          \
        else ap_l(p, (ex), (ey));                                                     \
    } while (0)
    ap_m(p, x0 + rx[0], y0);
    AP_CORNER(1, x1 - rx[1], y0, x1 - rx[1] + AP_K(rx[1]), y0, x1, y0 + ry[1] - AP_K(ry[1]), x1, y0 + ry[1]);
    AP_CORNER(2, x1, y1 - ry[2], x1, y1 - ry[2] + AP_K(ry[2]), x1 - rx[2] + AP_K(rx[2]), y1, x1 - rx[2], y1);
    AP_CORNER(3, x0 + rx[3], y1, x0 + rx[3] - AP_K(rx[3]), y1, x0, y1 - ry[3] + AP_K(ry[3]), x0, y1 - ry[3]);
    AP_CORNER(0, x0, y0 + ry[0], x0, y0 + ry[0] - AP_K(ry[0]), x0 + rx[0] - AP_K(rx[0]), y0, x0 + rx[0], y0);
    ap_z(p);
#undef AP_CORNER
#undef AP_K
}

/* The damage note for a path primitive: its rect grown by the halo (points)
 * through ad_add_halo, hashed with everything that changes its pixels. */
static void ad_note_path(int x, int y, int w, int h, int halo, const unsigned char *verbs, int nv,
                         const int *xy, int nxy, unsigned extra)
{
    struct ad_set s;
    unsigned sig = 2166136261u;
    s.n = 0;
    AD_MIX(sig, extra); AD_MIX(sig, nv);
    sig = ad_hash_bytes(verbs, nv, sig);
    sig = ad_hash_bytes(xy, (long)nxy * 4, sig);
    ad_add_halo(&s, x, y, w, h, halo, halo, halo, halo);
    if (s.n) ad_record(s.r[0].x0, s.r[0].y0, s.r[0].x1 - s.r[0].x0, s.r[0].y1 - s.r[0].y0, sig);
}

/* Fill (stroke_q8 == 0) or stroke the path in `p` with one solid ARGB colour.
 * (bx,by,bw,bh) is the shape's rect in window points (translation applied),
 * halo the extra reach of a stroke, in points. */
static long path_emit(const struct ap *p, unsigned argb, int snap, int stroke_q8,
                      int round_stroke, int bx, int by, int bw, int bh, int halo)
{
    struct logit_path2 pa;
    struct logit_paint2 pt;
    struct logit_stroke2 st;
    long rc;
    gfx_zero(&pa, sizeof pa); gfx_zero(&pt, sizeof pt); gfx_zero(&st, sizeof st);
    pa.verbs = p->v; pa.xy = p->xy; pa.nverb = (unsigned)p->nv; pa.nxy = (unsigned)p->nxy;
    pa.rule = LOGIT_PATH2_NONZERO;
    pt.kind = LOGIT_PAINT2_SOLID; pt.argb = argb; pt.flags = snap ? LOGIT_RECT_SNAP : 0;
    if (stroke_q8 > 0) {
        st.width = stroke_q8;
        st.cap = round_stroke ? LOGIT_STROKE2_CAP_ROUND : LOGIT_STROKE2_CAP_BUTT;
        st.join = round_stroke ? LOGIT_STROKE2_JOIN_ROUND : LOGIT_STROKE2_JOIN_MITER;
        st.miter = 4 * 65536;
    }
    ad_note_path(bx, by, bw, bh, halo, p->v, p->nv, p->xy, p->nxy, argb ^ (unsigned)stroke_q8);
#ifdef AUI_COST
    t0_();
#endif
    rc = ol_window_path2(&pa, &pt, stroke_q8 > 0 ? &st : 0);
#ifdef AUI_COST
    t1_(&ck_shape);
#endif
    return rc;
}

static unsigned ap_argb(unsigned rgb24, int alpha)
{ return ((unsigned)iclamp(alpha, 0, 255) << 24) | (rgb24 & 0xFFFFFFu); }

/* A rounded rect (stroke_q8 == 0) or its outline, in window points (the
 * current translation is applied here). r[] are the four corner radii in
 * points, already clamped; the edges and radii are converted to device pixels
 * exactly as the tile path does (aui_dev edges, devlen radii). A stroke is
 * centred on a rect inset by half its device width, so its OUTER edge is the
 * rect's, like aui_stroke's ring. */
static long path_rrect(int x, int y, int w, int h, const int r[4], int stroke_q8, unsigned argb)
{
    struct ap p;
    long long rx[4], ry[4];
    int X = X_(x), Y = Y_(y);
    long long x0 = (long long)aui_dev(X) * 256, x1 = (long long)aui_dev(X + w) * 256;
    long long y0 = (long long)aui_dev(Y) * 256, y1 = (long long)aui_dev(Y + h) * 256;
    long long hw = 0;
    int ax[4] = { X, X + w - r[1], X + w - r[2], X };
    int ay[4] = { Y, Y, Y + h - r[2], Y + h - r[3] };
    if (stroke_q8 > 0) {
        hw = ap_fdiv((long long)stroke_q8 * aui_scale(), 100) / 2;
        x0 += hw; y0 += hw; x1 -= hw; y1 -= hw;
    }
    for (int i = 0; i < 4; i++) {
        rx[i] = (long long)devlen(ax[i], r[i]) * 256 - hw;
        ry[i] = (long long)devlen(ay[i], r[i]) * 256 - hw;
        if (rx[i] < 0 || ry[i] < 0) rx[i] = ry[i] = 0;
    }
    ap_begin(&p, 1);
    ap_rrect(&p, x0, y0, x1, y1, rx, ry);
    return path_emit(&p, argb, 0, stroke_q8, 0, X, Y, w, h, stroke_q8 > 0 ? 1 : 0);
}

/* An axis-aligned rect on whole device pixels (LOGIT_RECT_SNAP floors each
 * edge, and dev_to_pt24 lands each edge just above its device value), the
 * alpha twin of gui_rect. */
static long path_rect_snapped(int x, int y, int w, int h, unsigned argb)
{
    struct ap p;
    int X = X_(x), Y = Y_(y);
    long long x0 = dev_ceil_pt24((long long)aui_dev(X) * 256), x1 = dev_ceil_pt24((long long)aui_dev(X + w) * 256);
    long long y0 = dev_ceil_pt24((long long)aui_dev(Y) * 256), y1 = dev_ceil_pt24((long long)aui_dev(Y + h) * 256);
    ap_begin(&p, 0);
    ap_m(&p, x0, y0); ap_l(&p, x1, y0); ap_l(&p, x1, y1); ap_l(&p, x0, y1); ap_z(&p);
    return path_emit(&p, argb, 1, 0, 0, X, Y, w, h, 0);
}

/* Passthrough for callers that build their own path. The verbs and points are
 * in the CURRENT origin's window points (24.8), like every aui draw: the
 * translation is folded into the path matrix. Reports damage from the points'
 * bounding box (transformed by the matrix) grown by the stroke's reach. The
 * kernel's status comes back unchanged (OL_UNSUPPORTED on a kernel without
 * LOGIT_GUI2_F_PATH). */
long aui_path(const struct logit_path2 *path, const struct logit_paint2 *paint,
              const struct logit_stroke2 *stroke)
{
    struct logit_path2 pa = *path;
    struct logit_paint2 pt = *paint;
    struct logit_stroke2 st;
    long long mnx = 0x7fffffff, mny = 0x7fffffff, mxx = -0x7fffffff, mxy = -0x7fffffff;
    int a = 65536, b = 0, c = 0, d = 65536, e = 0, f = 0, halo = 1;
    long rc;
    gfx_zero(&st, sizeof st);
    if (stroke) st = *stroke;
    if (pa.flags & LOGIT_PATH_MATRIX) {
        a = pa.m[0]; b = pa.m[1]; c = pa.m[2]; d = pa.m[3]; e = pa.m[4]; f = pa.m[5];
    }
    e += ox_ * 256; f += oy_ * 256;
    pa.flags |= LOGIT_PATH_MATRIX;
    pa.m[0] = a; pa.m[1] = b; pa.m[2] = c; pa.m[3] = d; pa.m[4] = e; pa.m[5] = f;
    if (pa.xy) {
        for (unsigned i = 0; i + 1 < pa.nxy; i += 2) {
            long long x = (((long long)a * pa.xy[i] + (long long)c * pa.xy[i + 1]) >> 16) + e;
            long long y = (((long long)b * pa.xy[i] + (long long)d * pa.xy[i + 1]) >> 16) + f;
            if (x < mnx) mnx = x;
            if (x > mxx) mxx = x;
            if (y < mny) mny = y;
            if (y > mxy) mxy = y;
        }
    }
    if (stroke) {
        long long hw = ((long long)st.width + 255) / 256 / 2 + 1;
        halo = (int)(st.join == LOGIT_STROKE2_JOIN_MITER ? hw * (st.miter / 65536 > 1 ? st.miter / 65536 : 1)
                                                         : hw * 2);
    }
    if (mnx <= mxx && mny <= mxy)
        ad_note_path((int)(mnx >> 8), (int)(mny >> 8), (int)((mxx - mnx) >> 8) + 1, (int)((mxy - mny) >> 8) + 1,
                     halo, pa.verbs, (int)pa.nverb, pa.xy, (int)pa.nxy, pt.argb ^ (unsigned)(pa.m[0] ^ pa.m[3] ^ pa.m[4] ^ pa.m[5]));
    rc = ol_window_path2(&pa, &pt, stroke ? &st : 0);
    return rc;
}

static int abs_i(int v) { return v < 0 ? -v : v; }

void aui_fill(int x, int y, int w, int h, unsigned color)
{
    if (w <= 0 || h <= 0) return;
    gui_rect(X_(x), Y_(y), w, h, color);
}

void aui_fill_a(int x, int y, int w, int h, unsigned color, int alpha)
{
    if (w <= 0 || h <= 0) return;
    if (alpha >= 255) { aui_fill(x, y, w, h, color); return; }
    if (alpha <= 0) return;
    if (path_ok()) { path_rect_snapped(x, y, w, h, ap_argb(color, alpha)); return; }
    /* One RGBA pixel stretched over the rect: the kernel's blit does the blend,
     * so an alpha fill costs exactly what an opaque fill costs. There is no
     * alpha-rectangle syscall, and this is why none is needed. */
    rgba_buf[0] = (unsigned char)((color >> 16) & 255);
    rgba_buf[1] = (unsigned char)((color >> 8) & 255);
    rgba_buf[2] = (unsigned char)(color & 255);
    rgba_buf[3] = (unsigned char)alpha;
    gui_blit(X_(x), Y_(y), w, h, rgba_buf, 1, 1);
}

void aui_hairline(int x, int y, int w) { aui_fill(x, y, w, 1, AUI_BORDER); }
void aui_vhairline(int x, int y, int h) { aui_fill(x, y, 1, h, AUI_BORDER); }

static int clamp_radius(int w, int h, int r)
{
    int m = imin(w, h) / 2;
    if (r > m) r = m;
    return r < 0 ? 0 : r;
}

static void round_impl(int x, int y, int w, int h, int r, unsigned c, int alpha)
{
    if (w <= 0 || h <= 0) return;
    r = clamp_radius(w, h, r);
    if (r == 0) { aui_fill_a(x, y, w, h, c, alpha); return; }
    if (path_ok()) {
        /* Opaque and uniform: the kernel's corner-mask fast path, one legacy
         * call. Anything with alpha (or a radius past the call's 8 bits) is
         * one SYS_GUI_PATH. */
        if (alpha >= 255 && r <= 255) { gui_rrect(X_(x), Y_(y), w, h, r, c); return; }
        if (alpha > 0) {
            int rr[4] = { r, r, r, r };
            path_rrect(x, y, w, h, rr, 0, ap_argb(c, alpha));
        }
        return;
    }
#ifdef AUI_NO_AA
    /* NEGATIVE CONTROL (make test-aui-negctl). Route every rounded shape back
     * through the kernel's SYS_GUI_RRECT, whose corner test is the boolean
     * dx*dx + dy*dy <= r*r -- i.e. the hard-edged staircase this file exists to
     * replace. The gallery still builds, still runs and still looks broadly
     * right, and tests/qmp/qmp_gallery.py's anti-aliasing assertions fail. That
     * is the demonstration that they are measuring the rasterizer and not the
     * theme. */
    (void)alpha;
    gui_rrect(X_(x), Y_(y), w, h, r, c);
    return;
#else
    int cw = devlen(X_(x), r), ch = devlen(Y_(y), r);
    unsigned char *rbuf; long rcap;
    const unsigned char *m = corner_mask(MK_FILL, cw, ch, 0, &rbuf, &rcap);
    if (!m) {
        /* Past BOTH the cache's ceiling and the uncached one. THE BUG THIS
         * WRITE-UP IS ABOUT: falling straight to a square used to be the
         * ONLY response, which is what let a 180-point clock face ask for a
         * 90-point corner and get a SQUARE clock, silently, correctly by
         * this function's own old contract, and wrong -- clock.c:138-148 is
         * the write-up, from the caller's side, of having to route around
         * this by hand. corner_mask() tries the uncached tile first now
         * (see the BIG_MASK block above), so this square is reached only
         * past BIG_MASK -- and it is the acceptable half of gfx_mask.c's
         * contract even then: COMPLETE (no missing pixels), and, because
         * gfx_mask_corner already counted the refusal that got us here, no
         * longer silent either. */
        aui_fill_a(x, y, w, h, c, alpha);
        return;
    }
    /* interior: three opaque bands, the kernel's fast path */
    aui_fill_a(x + r, y,         w - 2 * r, r,         c, alpha);
    aui_fill_a(x,     y + r,     w,         h - 2 * r, c, alpha);
    aui_fill_a(x + r, y + h - r, w - 2 * r, r,         c, alpha);
    /* four corners, one rasterized quadrant mirrored into place */
    blit_mask_buf(rbuf, rcap, X_(x),         Y_(y),         r, r, m, cw, ch, c, alpha, 0, 0);
    blit_mask_buf(rbuf, rcap, X_(x + w - r), Y_(y),         r, r, m, cw, ch, c, alpha, 1, 0);
    blit_mask_buf(rbuf, rcap, X_(x),         Y_(y + h - r), r, r, m, cw, ch, c, alpha, 0, 1);
    blit_mask_buf(rbuf, rcap, X_(x + w - r), Y_(y + h - r), r, r, m, cw, ch, c, alpha, 1, 1);
#endif
}

void aui_round(int x, int y, int w, int h, int r, unsigned c) { round_impl(x, y, w, h, r, c, 255); }
void aui_round_a(int x, int y, int w, int h, int r, unsigned c, int a) { round_impl(x, y, w, h, r, c, a); }

void aui_stroke(int x, int y, int w, int h, int r, int t, unsigned c)
{
    if (w <= 0 || h <= 0 || t <= 0) return;
    r = clamp_radius(w, h, r);
    if (r == 0) {
        aui_fill(x, y, w, t, c); aui_fill(x, y + h - t, w, t, c);
        aui_fill(x, y + t, t, h - 2 * t, c); aui_fill(x + w - t, y + t, t, h - 2 * t, c);
        return;
    }
    if (path_ok()) {
        /* One stroked path; the width is the tile path's: whole device pixels, at least one. */
        int rr[4] = { r, r, r, r };
        path_rrect(x, y, w, h, rr, (int)dev_ceil_pt24((long long)imax(1, aui_dev(t)) * 256), ap_argb(c, 255));
        return;
    }
#ifdef AUI_NO_AA
    gui_rrect(X_(x), Y_(y), w, h, r, c);                       /* see round_impl */
    gui_rrect(X_(x + t), Y_(y + t), w - 2 * t, h - 2 * t, r - t, AUI_BG);
    return;
#else
    int cw = devlen(X_(x), r), ch = devlen(Y_(y), r), td = imax(1, aui_dev(t));
    unsigned char *rbuf; long rcap;
    const unsigned char *m = corner_mask(MK_STROKE, cw, ch, td, &rbuf, &rcap);
    if (!m) {
        /* THE WORST OFFENDER IN THIS FILE, before this fix (see gfx_mask.c's
         * contract comment on gfx_mask_corner for the taxonomy). There used
         * to be NO check here at all: a radius past GFX_MASK_MAX went
         * straight into blit_mask with a NULL cov, which silently no-ops on
         * a NULL source -- so the four straight edges below still drew and
         * the four corners just never arrived. That is not "a plausible
         * wrong picture", it is a DIFFERENT, perfectly complete-looking
         * shape (a plain square outline where a rounded one was asked for),
         * which is exactly the "drops geometry" case that same contract
         * comment says is never acceptable -- unlike round_impl's square,
         * there is no honest degraded version of THIS shape with the
         * corners missing. corner_mask() tries the uncached ring first now;
         * only past BOTH ceilings does this still fall back to an actual
         * square ring (the r==0 shape above), which is complete and, like
         * every gfx_mask_corner refusal, already counted.
         * tests/unit/aui_mask_test.c's "aui_stroke past the ceiling" case
         * calls corner_mask() with exactly this kind at an oversized radius
         * and asserts it comes back a continuous ring rather than NULL --
         * which is what proves this branch is reached only when it truly
         * has to be. */
        aui_fill(x, y, w, t, c); aui_fill(x, y + h - t, w, t, c);
        aui_fill(x, y + t, t, h - 2 * t, c); aui_fill(x + w - t, y + t, t, h - 2 * t, c);
        return;
    }
    aui_fill(x + r, y,         w - 2 * r, t, c);
    aui_fill(x + r, y + h - t, w - 2 * r, t, c);
    aui_fill(x,         y + r, t, h - 2 * r, c);
    aui_fill(x + w - t, y + r, t, h - 2 * r, c);
    blit_mask_buf(rbuf, rcap, X_(x),         Y_(y),         r, r, m, cw, ch, c, 255, 0, 0);
    blit_mask_buf(rbuf, rcap, X_(x + w - r), Y_(y),         r, r, m, cw, ch, c, 255, 1, 0);
    blit_mask_buf(rbuf, rcap, X_(x),         Y_(y + h - r), r, r, m, cw, ch, c, 255, 0, 1);
    blit_mask_buf(rbuf, rcap, X_(x + w - r), Y_(y + h - r), r, r, m, cw, ch, c, 255, 1, 1);
#endif
}

void aui_circle(int cx, int cy, int r, unsigned c) { aui_round(cx - r, cy - r, 2 * r, 2 * r, r, c); }
void aui_ring(int cx, int cy, int r, int t, unsigned c) { aui_stroke(cx - r, cy - r, 2 * r, 2 * r, r, t, c); }

/* ---- shapes that need the path engine (GUI ABI v2) ----
 * Colours here are ARGB: alpha is the high byte (0 = nothing drawn). On a
 * kernel without LOGIT_GUI2_F_PATH each has a best-effort tile-path fallback,
 * noted per function; none can be exact. */

/* A rounded rect with four independent corner radii (TL, TR, BR, BL), points.
 * Fallback: one uniform radius, the smallest of the four. */
void aui_round4(struct aui_rect rc, const int rad[4], unsigned argb)
{
    int r[4], m;
    if (rc.w <= 0 || rc.h <= 0 || (argb >> 24) == 0) return;
    for (int i = 0; i < 4; i++) r[i] = clamp_radius(rc.w, rc.h, rad[i]);
    if (path_ok()) { path_rrect(rc.x, rc.y, rc.w, rc.h, r, 0, argb); return; }
    m = imin(imin(r[0], r[1]), imin(r[2], r[3]));
    round_impl(rc.x, rc.y, rc.w, rc.h, m, argb & 0xFFFFFFu, (int)(argb >> 24));
}

/* The outline of a rounded rect, `width_q8` wide (24.8 points: 128 = half a
 * point), its OUTER edge on the rect's, with alpha. Fallback: aui_stroke, a
 * whole-point opaque ring (the alpha is dropped). */
void aui_stroke_a(struct aui_rect rc, int radius, int width_q8, unsigned argb)
{
    if (rc.w <= 0 || rc.h <= 0 || width_q8 <= 0 || (argb >> 24) == 0) return;
    radius = clamp_radius(rc.w, rc.h, radius);
    if (path_ok()) {
        int r[4] = { radius, radius, radius, radius };
        path_rrect(rc.x, rc.y, rc.w, rc.h, r, width_q8, argb);
        return;
    }
    aui_stroke(rc.x, rc.y, rc.w, rc.h, radius, imax(1, (width_q8 + 255) / 256), argb & 0xFFFFFFu);
}

/* A line of exactly ONE DEVICE pixel at any backing scale, from (x0,y0) to
 * (x1,y1) in points. A horizontal or vertical one is snapped to the device
 * grid (LOGIT_RECT_SNAP) and half-open like aui_hairline: it covers
 * [x0,x1) on the device row (column) the point y0 (x0) lands on, fully. A
 * diagonal is an anti-aliased one-pixel stroke. Fallback: a one-POINT
 * aui_fill_a (several device pixels at 150%/200%), axis-aligned lines only. */
void aui_hairline_dev(int x0, int y0, int x1, int y1, unsigned argb)
{
    int horiz = y0 == y1, vert = x0 == x1;
    if ((argb >> 24) == 0) return;
    if (!path_ok()) {
        if (horiz) aui_fill_a(imin(x0, x1), y0, abs_i(x1 - x0), 1, argb & 0xFFFFFFu, (int)(argb >> 24));
        else if (vert) aui_fill_a(x0, imin(y0, y1), 1, abs_i(y1 - y0), argb & 0xFFFFFFu, (int)(argb >> 24));
        return;
    }
    struct ap p;
    ap_begin(&p, 0);
    if (horiz || vert) {
        if (horiz && x1 < x0) { int t = x0; x0 = x1; x1 = t; }
        if (vert && y1 < y0) { int t = y0; y0 = y1; y1 = t; }
        long long dx0 = aui_dev(X_(x0)), dy0 = aui_dev(Y_(y0));
        long long dx1 = horiz ? aui_dev(X_(x1)) : dx0 + 1, dy1 = horiz ? dy0 + 1 : aui_dev(Y_(y1));
        if (dx1 <= dx0 || dy1 <= dy0) return;
        ap_m(&p, dev_ceil_pt24(dx0 * 256), dev_ceil_pt24(dy0 * 256));
        ap_l(&p, dev_ceil_pt24(dx1 * 256), dev_ceil_pt24(dy0 * 256));
        ap_l(&p, dev_ceil_pt24(dx1 * 256), dev_ceil_pt24(dy1 * 256));
        ap_l(&p, dev_ceil_pt24(dx0 * 256), dev_ceil_pt24(dy1 * 256));
        ap_z(&p);
        path_emit(&p, argb, 1, 0, 0, X_(imin(x0, x1)), Y_(imin(y0, y1)), imax(1, abs_i(x1 - x0)), imax(1, abs_i(y1 - y0)), 1);
        return;
    }
    ap_m(&p, (long long)X_(x0) * 256, (long long)Y_(y0) * 256);
    ap_l(&p, (long long)X_(x1) * 256, (long long)Y_(y1) * 256);
    path_emit(&p, argb, 0, (int)dev_ceil_pt24(256), 0, X_(imin(x0, x1)), Y_(imin(y0, y1)),
              imax(1, abs_i(x1 - x0)), imax(1, abs_i(y1 - y0)), 2);
}

/* A continuous-curvature ("squircle") rounded rect: the corner starts a
 * quarter of the radius sooner on each edge and its curve is flatter at the
 * ends, so the join into the straight edge has no visible kink. Fallback: the
 * ordinary circular corner. */
void aui_squircle(struct aui_rect rc, int radius, unsigned argb)
{
    if (rc.w <= 0 || rc.h <= 0 || (argb >> 24) == 0) return;
    radius = clamp_radius(rc.w, rc.h, radius);
    if (radius == 0 || !path_ok()) {
        round_impl(rc.x, rc.y, rc.w, rc.h, radius, argb & 0xFFFFFFu, (int)(argb >> 24));
        return;
    }
    struct ap p;
    int X = X_(rc.x), Y = Y_(rc.y);
    long long x0 = (long long)aui_dev(X) * 256, x1 = (long long)aui_dev(X + rc.w) * 256;
    long long y0 = (long long)aui_dev(Y) * 256, y1 = (long long)aui_dev(Y + rc.h) * 256;
    long long e = (long long)devlen(X, radius) * 256 * 5 / 4;          /* corner extent */
    long long cap = imin((int)((x1 - x0) / 512), (int)((y1 - y0) / 512)) * 256LL;
    if (e > cap) e = cap;
    long long a = e * 87 / 256;                                        /* handle: 34% of e from the edge's end */
    ap_begin(&p, 1);
    ap_m(&p, x0 + e, y0);
    ap_l(&p, x1 - e, y0);
    ap_c(&p, x1 - a, y0, x1, y0 + a, x1, y0 + e);
    ap_l(&p, x1, y1 - e);
    ap_c(&p, x1, y1 - a, x1 - a, y1, x1 - e, y1);
    ap_l(&p, x0 + e, y1);
    ap_c(&p, x0 + a, y1, x0, y1 - a, x0, y1 - e);
    ap_l(&p, x0, y0 + e);
    ap_c(&p, x0, y0 + a, x0 + a, y0, x0 + e, y0);
    ap_z(&p);
    path_emit(&p, argb, 0, 0, 0, X, Y, rc.w, rc.h, 0);
}

/* An arc of a circle, stroked `width` points wide with round ends. Angles are
 * whole degrees, 0 at 3 o'clock, positive clockwise (y is down), so a sweep of
 * 90 from start 0 runs from 3 o'clock to 6 o'clock. |sweep| is capped at 360.
 * (cx, cy) and radius are points; the stroke is centred on the circle.
 * Fallback: a chain of round dots of the stroke's width, one per 6 degrees. */
void aui_arc(int cx, int cy, int r, int start, int sweep, int width, unsigned argb)
{
    if (r <= 0 || width <= 0 || sweep == 0 || (argb >> 24) == 0) return;
    sweep = iclamp(sweep, -360, 360);
    if (!path_ok()) {
        int n = abs_i(sweep) / 6 + 1;
        for (int i = 0; i <= n; i++) {
            int ang = (start + sweep * i / n) * 256;
            int px = cx + (int)(((long long)r * gfx_cos(ang)) >> 16);
            int py = cy + (int)(((long long)r * gfx_sin(ang)) >> 16);
            round_impl(px - width / 2, py - width / 2, width, width, width / 2, argb & 0xFFFFFFu, (int)(argb >> 24));
        }
        return;
    }
    struct ap p;
    int n = (abs_i(sweep) + 89) / 90, th = sweep * 256 / n;
    long long R = (long long)r * 256, CX = (long long)X_(cx) * 256, CY = (long long)Y_(cy) * 256;
    long long k = (long long)gfx_cos(th / 4);
    k = k ? (long long)gfx_sin(th / 4) * 65536 / k * 4 / 3 : 0;         /* 4/3 tan(th/4), 16.16 */
    ap_begin(&p, 0);
    for (int i = 0; i < n; i++) {
        int a0 = start * 256 + i * th, a1 = a0 + th;
        long long s0 = gfx_sin(a0), c0 = gfx_cos(a0), s1 = gfx_sin(a1), c1 = gfx_cos(a1);
        long long x0 = CX + (R * c0 >> 16), y0 = CY + (R * s0 >> 16);
        long long x3 = CX + (R * c1 >> 16), y3 = CY + (R * s1 >> 16);
        if (i == 0) ap_m(&p, x0, y0);
        ap_c(&p, x0 - (R * s0 >> 16) * k / 65536, y0 + (R * c0 >> 16) * k / 65536,
                 x3 + (R * s1 >> 16) * k / 65536, y3 - (R * c1 >> 16) * k / 65536, x3, y3);
    }
    path_emit(&p, argb, 0, width * 256, 1, X_(cx) - r, Y_(cy) - r, 2 * r, 2 * r, (width + 1) / 2 + 1);
}

void aui_vgrad(int x, int y, int w, int h, unsigned top, unsigned bot)
{
    if (w <= 0 || h <= 0) return;
    int n = devlen(Y_(y), h);
    if (n <= 0) return;
    if (n > GRAD_MAX) n = GRAD_MAX;       /* subsample; the kernel rescales it back */
    gfx_gradient_strip(grad_buf, n, top, bot, 255);
    gui_blit(X_(x), Y_(y), w, h, grad_buf, 1, n);   /* sw = 1: replicated across x */
}

void aui_hgrad(int x, int y, int w, int h, unsigned l, unsigned r)
{
    if (w <= 0 || h <= 0) return;
    int n = devlen(X_(x), w);
    if (n <= 0) return;
    if (n > GRAD_MAX) n = GRAD_MAX;
    gfx_gradient_strip(grad_buf, n, l, r, 255);
    gui_blit(X_(x), Y_(y), w, h, grad_buf, n, 1);
}

/* One gradient corner: the mask's shape, each row tinted by its own position
 * in the vertical ramp so the curve does not shear away from the band it
 * abuts. Factored out of aui_vgrad_round so the cached tile (rgba_buf) and
 * the uncached one (big_rgba, see the BIG_MASK block above) share the one
 * copy of this loop instead of drifting apart. */
static void vgrad_corner(unsigned char *rbuf, long rcap, int devx, int devy, int r,
                         const unsigned char *m, int cw, int ch, int fx, int fy,
                         unsigned top, unsigned bot, int hd)
{
    if ((long)cw * ch * 4 > rcap) return;
    for (int j = 0; j < ch; j++) {
        int sj = fy ? ch - 1 - j : j;
        int grow = fy ? hd - ch + j : j;
        unsigned c = aui_mix(top, bot, iclamp(grow * 255 / hd, 0, 255));
        unsigned char *d = rbuf + (long)j * cw * 4;
        const unsigned char *s = m + (long)sj * cw;
        ol_bitmap_tint_row(d,s,cw,c,255,fx);
    }
    gui_blit(devx, devy, r, r, rbuf, cw, ch);
}

void aui_vgrad_round(int x, int y, int w, int h, int r, unsigned top, unsigned bot)
{
    if (w <= 0 || h <= 0) return;
    r = clamp_radius(w, h, r);
    if (r == 0) { aui_vgrad(x, y, w, h, top, bot); return; }

    /* Decide the corner source BEFORE drawing anything. The old order drew
     * the three bands FIRST and only then asked for the corners, so a
     * refusal's `if (!m) return;` left the four r x r corner squares as pure,
     * untouched background -- not a square corner, an actual HOLE in the
     * shape (gfx_mask.c's contract comment calls this out by name: the same
     * "drops geometry" failure as aui_stroke's vanishing arcs, just a gap
     * instead of a missing outline). Deciding first is what makes a clean
     * whole-rect fallback possible instead of a hole. */
    int cw = devlen(X_(x), r), ch = devlen(Y_(y), r);
    unsigned char *rbuf; long rcap;
    const unsigned char *m = corner_mask(MK_FILL, cw, ch, 0, &rbuf, &rcap);
    if (!m) {
        /* Past both ceilings: a flat-cornered gradient over the WHOLE rect is
         * complete (no hole, no seam at the band edge) -- the acceptable
         * half of the contract, and, like every gfx_mask_corner refusal,
         * already counted there. */
        aui_vgrad(x, y, w, h, top, bot);
        return;
    }

    aui_vgrad(x + r, y,         w - 2 * r, r,         top, aui_mix(top, bot, r * 255 / h));
    aui_vgrad(x,     y + r,     w,         h - 2 * r, aui_mix(top, bot, r * 255 / h),
                                                      aui_mix(top, bot, (h - r) * 255 / h));
    aui_vgrad(x + r, y + h - r, w - 2 * r, r,         aui_mix(top, bot, (h - r) * 255 / h), bot);
    /* The corners take the gradient's colour at their own row, so the curve does
     * not shear away from the band it abuts. */
    int hd = imax(1, devlen(Y_(y), h));
    for (int corner = 0; corner < 4; corner++) {
        int fx = corner & 1, fy = corner >> 1;
        int px = fx ? x + w - r : x, py = fy ? y + h - r : y;
        vgrad_corner(rbuf, rcap, X_(px), Y_(py), r, m, cw, ch, fx, fy, top, bot, hd);
    }
}

/* An edge strip: one device pixel across the constant axis, `n` along the
 * falloff, stretched by the kernel. This is what makes a shadow's cost depend on
 * the blur radius and not on the size of the thing casting it. */
static void shadow_edge(int x, int y, int w, int h, int n, int vertical,
                        int reverse, unsigned color, int alpha)
{
    if (w <= 0 || h <= 0 || n <= 0 || n > GRAD_MAX) return;
    long blur = (long)n * 256;
    for (int k = 0; k < n; k++) {
        int kk = reverse ? k : n - 1 - k;            /* distance from the box edge */
        long d = (long)kk * 256 + 128;
        int a = gfx_shadow_falloff(d, blur) * alpha / 255;
        grad_buf[k * 4 + 0] = (unsigned char)((color >> 16) & 255);
        grad_buf[k * 4 + 1] = (unsigned char)((color >> 8) & 255);
        grad_buf[k * 4 + 2] = (unsigned char)(color & 255);
        grad_buf[k * 4 + 3] = (unsigned char)a;
    }
    if (vertical) gui_blit(x, y, w, h, grad_buf, 1, n);
    else          gui_blit(x, y, w, h, grad_buf, n, 1);
}

/* How many shadow requests this session had to shrink `blur` to fit even the
 * uncached tile (BIG_MASK) -- see aui_shadow_ex's tier 3 below. A count, not
 * a bool, and read the same way gfx_mask_refused() is: nothing else makes
 * "shadows are coming out tighter than CSS asked for" visible, and fb.c's
 * fb_shadow has the identical silent clamp today (see its own comment) --
 * this is the one half of that bug this unit can actually fix, since fb.c is
 * kernel code and cannot afford this file's BIG_MASK buffers (see fb.c). */
static unsigned shadow_degraded;

/* The mask-tile shadow: four cached corner tiles plus four 1-device-pixel edge
 * strips. It is the whole implementation on a kernel without
 * LOGIT_GUI2_F_SHADOW2 and the fallback when the v2 call is refused. `blur` is
 * the falloff EXTENT in points (not a sigma), `col` is 0xRRGGBB. */
static void shadow_tiles(int x, int y, int w, int h, int r, int dy, int blur, int alpha, unsigned col)
{
    if (w <= 0 || h <= 0 || blur <= 0 || alpha <= 0) return;
#ifdef AUI_NO_AA
    (void)r; (void)dy; (void)col;         /* no shadows without alpha compositing */
    return;
#else
    r = clamp_radius(w, h, r);
    int sx = X_(x), sy = Y_(y) + dy;
    int T = blur + r;                                  /* corner tile edge, points */
    int cw = devlen(sx - blur, T), ch = devlen(sy - blur, T);
    int rd = imin(aui_dev(r), cw - 1);
    if (rd < 0) rd = 0;
    unsigned char *rbuf; long rcap;
    const unsigned char *m = corner_mask(MK_SHADOW, cw, ch, rd, &rbuf, &rcap);
    if (!m) {
        /* Old code: `if (m) { four blits }` with NOTHING gating the edge
         * strips below, which draw unconditionally -- so a refused corner
         * left the edges with no corner to meet, exactly the seam gfx.h's
         * clip-mask note warns a mismatched pair produces. corner_mask()
         * covers tier 1 (cached) and tier 2 (uncached, AT THE REQUESTED
         * blur -- no clamp) above; reaching here means BOTH refused, and
         * only then does tier 3 shrink blur -- BEFORE recomputing T/cw/ch,
         * so the edge strips further down (which key off the same `blur`
         * variable) automatically fall off over the same, now-shorter,
         * distance and nothing seams.
         *
         * `blur`/`r` are POINTS here (aui_shadow_ex's whole interface is),
         * but BIG_MASK is a DEVICE bound -- the same point/device split
         * clock.c:126 works around with `MASK_MAX * 100 / aui_scale()`.
         * Same idiom: convert the device ceiling back to a point span
         * first, THEN shrink blur in the units it is actually expressed
         * in. */
        shadow_degraded++;
        int capT = BIG_MASK * 100 / aui_scale();
        blur = capT - r;
        if (blur <= 0) return;      /* radius alone doesn't fit even the device cap: no sound shadow to draw */
        T = blur + r;
        cw = devlen(sx - blur, T); ch = devlen(sy - blur, T);
        /* devlen rounds; clamp hard rather than trust the arithmetic -- this
         * feeds corner_mask()'s own BIG_MASK check, and a byte over would
         * just be refused again (safe, but pointless) rather than overflow
         * anything, since corner_mask never writes past what it checked. */
        if (cw > BIG_MASK) cw = BIG_MASK;
        if (ch > BIG_MASK) ch = BIG_MASK;
        rd = imin(aui_dev(r), cw - 1);
        if (rd < 0) rd = 0;
        m = corner_mask(MK_SHADOW, cw, ch, rd, &rbuf, &rcap);
        if (!m) return;    /* shouldn't happen -- cw,ch are now <= BIG_MASK by construction -- but never draw on an unmet assumption */
    }
    blit_mask_buf(rbuf, rcap, sx - blur,     sy - blur,     T, T, m, cw, ch, col, alpha, 0, 0);
    blit_mask_buf(rbuf, rcap, sx + w - r,    sy - blur,     T, T, m, cw, ch, col, alpha, 1, 0);
    blit_mask_buf(rbuf, rcap, sx - blur,     sy + h - r,    T, T, m, cw, ch, col, alpha, 0, 1);
    blit_mask_buf(rbuf, rcap, sx + w - r,    sy + h - r,    T, T, m, cw, ch, col, alpha, 1, 1);
    /* The offset exposes a sliver of the shadow box's INTERIOR below the caster,
     * and the 8 slices deliberately do not paint the interior. Left out, every
     * elevated card in the system shows a `dy`-pixel gap of clean background
     * between itself and its own shadow -- which is exactly what a shadow never
     * does. One flat band closes it, still O(1). (Caught by the "falls off with
     * distance" assertion in tests/qmp/qmp_gallery.py, which read background
     * where the darkest part of the shadow should have been.) */
    if (dy > 0) aui_fill_a(x + r, y + h, w - 2 * r, dy, col, alpha);
    int bd = devlen(sy - blur, blur);
    shadow_edge(sx + r, sy - blur, w - 2 * r, blur, bd, 1, 0, col, alpha);
    shadow_edge(sx + r, sy + h,    w - 2 * r, blur, bd, 1, 1, col, alpha);
    bd = devlen(sx - blur, blur);
    shadow_edge(sx - blur, sy + r, blur, h - 2 * r, bd, 0, 0, col, alpha);
    shadow_edge(sx + w,    sy + r, blur, h - 2 * r, bd, 0, 1, col, alpha);
#endif
}

/* ---------------------------------------------------------------------------
 * GUI ABI v2 shadows (SYS_GUI_SHADOW2).
 *
 * When SYS_GUI2_INFO(0) reports LOGIT_GUI2_F_SHADOW2 every shadow here is ONE
 * ol_window_shadow2: the kernel's analytic gaussian (display_shadow_ex), with a
 * colour, an offset on both axes, a spread, an inset mode, and NO ceiling on
 * the blur (the tile path tops out at BIG_MASK and shrinks the shadow past
 * it). The rect and radii are decided in DEVICE pixels exactly as the tile
 * path decides them (aui_dev edges, LOGIT_RECT_SNAP) so the caster's edge is
 * the same pixel at 125/150/200%. Offset, spread and sigma are points; the
 * kernel scales them. -DAUI_SHADOW_LEGACY (the negative control) pretends the
 * kernel never reports SHADOW2. */
#if defined(AUI_NO_AA) || defined(AUI_SHADOW_LEGACY)
#define shadow2_ok() 0
#else
static int shadow2_caps = -1;         /* -1 not asked yet, 0 no, 1 yes */
static int shadow2_ok(void)
{
    if (shadow2_caps < 0) {
        long i = ol_gui2_info(0);
        shadow2_caps = (i > 0 && (i & LOGIT_GUI2_F_SHADOW2)) ? 1 : 0;
    }
    return shadow2_caps;
}
#endif

static int sh_up100(long long v, int pct)       /* ceil(v * 100 / pct) for v >= 0 */
{ return (int)((v * 100 + pct - 1) / pct); }

/* The damage note: the caster's rect grown by ad_halo_shadow() (3 sigma + 2 px +
 * spread, plus the offset on the side it moves toward), computed in DEVICE
 * pixels and converted back to points rounding up. An inset shadow stays
 * inside its rect. */
static void ad_note_shadow2(int x, int y, int w, int h, int r, int dx, int dy, int sigma_q8,
                            int spread, unsigned argb, int inset)
{
    int pct = aui_scale(), hl[4] = { 0, 0, 0, 0 }, k;
    unsigned sig = 2166136261u;
    if (!inset) {
        long long vx = (long long)dx * pct, vy = (long long)dy * pct;
        int dxd = (int)(vx >= 0 ? (vx + 99) / 100 : -((-vx + 99) / 100));
        int dyd = (int)(vy >= 0 ? (vy + 99) / 100 : -((-vy + 99) / 100));
        int sgd = (int)(((long long)sigma_q8 * pct + 25599) / 25600);
        int spd = spread > 0 ? (int)(((long long)spread * pct + 99) / 100) : 0;
        ad_halo_shadow(dxd, dyd, sgd, spd, hl);
        for (k = 0; k < 4; k++) hl[k] = sh_up100(hl[k], pct) + 1;
    }
    AD_MIX(sig, r); AD_MIX(sig, dx); AD_MIX(sig, dy); AD_MIX(sig, sigma_q8);
    AD_MIX(sig, spread); AD_MIX(sig, argb); AD_MIX(sig, inset);
    {
        struct ad_set s;
        s.n = 0;
        ad_add_halo(&s, x, y, w, h, hl[0], hl[1], hl[2], hl[3]);
        if (s.n) ad_record(s.r[0].x0, s.r[0].y0, s.r[0].x1 - s.r[0].x0, s.r[0].y1 - s.r[0].y0, sig);
    }
}

/* One SYS_GUI_SHADOW2. Points in, the kernel's status out (OL_UNSUPPORTED etc.
 * without the capability; the caller falls back to the tile path). */
static long shadow2_emit(int x, int y, int w, int h, int r, int dx, int dy, int sigma_q8,
                         int spread, unsigned argb, int inset)
{
    struct logit_shadow2 s;
    long long x0 = dev_ceil_pt24((long long)aui_dev(X_(x)) * 256), x1 = dev_ceil_pt24((long long)aui_dev(X_(x + w)) * 256);
    long long y0 = dev_ceil_pt24((long long)aui_dev(Y_(y)) * 256), y1 = dev_ceil_pt24((long long)aui_dev(Y_(y + h)) * 256);
    int rr = (int)dev_ceil_pt24((long long)imax(0, aui_dev(r)) * 256), k;
    gfx_zero(&s, sizeof s);
    s.x = (int)x0; s.y = (int)y0; s.w = (int)(x1 - x0); s.h = (int)(y1 - y0);
    for (k = 0; k < 4; k++) s.radius[k] = rr;
    s.dx = dx * 256; s.dy = dy * 256; s.blur = sigma_q8; s.spread = spread * 256;
    s.argb = argb;
    s.flags = LOGIT_RECT_SNAP | (inset ? LOGIT_SHADOW2_INSET : 0);
    ad_note_shadow2(x, y, w, h, r, dx, dy, sigma_q8, spread, argb, inset);
    return ol_window_shadow2(&s);
}

static unsigned sh_argb(unsigned col, int alpha)
{ return ((unsigned)(alpha > 255 ? 255 : alpha) << 24) | (col & 0xFFFFFFu); }

/* aui_shadow_ex keeps its signature: `blur` is the falloff extent in points, and
 * on a SHADOW2 kernel it becomes a gaussian of sigma = blur / 2 (the CSS
 * convention), colour AUI_SHADOW, no spread. */
void aui_shadow_ex(int x, int y, int w, int h, int r, int dy, int blur, int alpha)
{
    unsigned col = AUI_SHADOW;
    if (w <= 0 || h <= 0 || blur <= 0 || alpha <= 0) return;
#ifndef AUI_NO_AA
    if (shadow2_ok() &&
        shadow2_emit(x, y, w, h, clamp_radius(w, h, r), 0, dy, blur * 128, 0, sh_argb(col, alpha), 0) == 0)
        return;
#endif
    shadow_tiles(x, y, w, h, r, dy, blur, alpha, col);
}

/* A general shadow: offset (dx,dy) and spread in points (spread grows the
 * shadow's shape, negative shrinks it), `sigma` the gaussian standard deviation
 * in points (0..64), ARGB colour (alpha is the peak opacity). inset != 0 draws
 * the shadow INSIDE the rect (the blurred complement of the offset shape).
 * Without SHADOW2 the tile fallback draws the outer form only (extent 2*sigma,
 * dx and spread folded into the caster rect) and an inset draws nothing. */
void aui_shadow2(struct aui_rect rect, int radius, int dx, int dy, int sigma, int spread,
                 unsigned argb, int inset)
{
    int alpha = (int)(argb >> 24);
    if (rect.w <= 0 || rect.h <= 0 || alpha <= 0 || sigma < 0) return;
    if (sigma > 64) sigma = 64;
#ifndef AUI_NO_AA
    if (shadow2_ok() &&
        shadow2_emit(rect.x, rect.y, rect.w, rect.h, radius < 0 ? 0 : radius, dx, dy, sigma * 256,
                     spread, argb, inset) == 0)
        return;
#endif
    if (inset || sigma == 0) return;
    shadow_tiles(rect.x + dx - spread, rect.y - spread, rect.w + 2 * spread, rect.h + 2 * spread,
                 radius + spread, dy, 2 * sigma, alpha, argb & 0xFFFFFFu);
}

/* The legacy elevation calls on the design's ladder (3.3): aui_elevation's
 * levels 1..4 are E1 / E2 / E3 / E6, each EXACTLY two SHADOW2 (key, then
 * ambient) in the shadow ink with the mode's alphas (aui_elev, logit_elevation.h).
 * Level 0 draws nothing, levels above 4 are 4.
 *
 *   level   ladder   used for
 *     1      E1      cards (light), Secondary buttons, thumbs and knobs
 *     2      E2      hover-lifted cards, tooltips, dragged rows
 *     3      E3      menus, popovers, select popups, HUDs
 *     4      E6      alerts and sheets, Spotlight, About
 */
static const unsigned char aui_elev_of_level[4] = { LOGIT_E1, LOGIT_E2, LOGIT_E3, LOGIT_E6 };

void aui_elevation(struct aui_rect rect, int radius, int level)
{
    if (level <= 0) return;
    aui_elev(rect, radius, (enum logit_elev)aui_elev_of_level[(level > 4 ? 4 : level) - 1]);
}

/* A glow: a soft halo of `argb` (alpha = peak opacity) around the rect, sigma
 * 6 pt, spread 1 pt, no offset. */
void aui_glow(struct aui_rect rect, int radius, unsigned argb)
{
    aui_shadow2(rect, radius, 0, 0, 6, 1, argb, 0);
}

/* AUI_ELEV_1/2/3 are E1/E2/E3; anything else draws nothing, as it always has. */
void aui_shadow(int x, int y, int w, int h, int r, int elev)
{
    if (elev >= AUI_ELEV_1 && elev <= AUI_ELEV_3)
        aui_elev(aui_r(x, y, w, h), r, (enum logit_elev)aui_elev_of_level[elev - 1]);
}

void aui_panel(int x, int y, int w, int h, unsigned color) { aui_fill(x, y, w, h, color); }

/* A card's edge, 1 DEVICE pixel at any scale (design 3.2, "Dark rim on opaque
 * raised surfaces"): dark, an INNER stroke whose paint is a LINEAR gradient
 * top -> bottom, #FFFFFF @ 38 -> @ 10 at 40 % -> @ 18, so the card catches the
 * light along its top edge; light, an OUTER stroke of #141428 @ 20 that keeps a
 * white card crisp on bg.window. ONE SYS_GUI_PATH either way. The rrect is the
 * card's own (same device edges and radius as its fill and its shadow: the
 * one-rrect rule of 3.4), its centre line half a device pixel inside (dark) or
 * outside (light). Without the PATH capability: a whole-point opaque stroke of
 * the same colours flattened over what they sit on. */
#define CARD_STOPS 3
static void card_edge(int x, int y, int w, int h, int r, int dark)
{
    if (w <= 0 || h <= 0) return;
    r = clamp_radius(w, h, r);
    if (!path_ok()) {
        if (dark) aui_stroke(x, y, w, h, r, 1, tok_over(LOGIT_DARK_RIM_BOTTOM, aui_t.bg_card));
        else      aui_stroke(x - 1, y - 1, w + 2, h + 2, r + 1, 1, tok_over(LOGIT_LIGHT_OUTLINE, aui_t.bg));
        return;
    }
    static const unsigned pos[CARD_STOPS] = { 0, (65536u * LOGIT_DARK_RIM_MID_POS + 50) / 100, 65536 };
    static const unsigned col[CARD_STOPS] = { LOGIT_DARK_RIM_TOP, LOGIT_DARK_RIM_MID, LOGIT_DARK_RIM_BOTTOM };
    struct ap p;
    struct logit_path2 pa;
    struct logit_paint2 pt;
    struct logit_stroke2 st;
    long long rx[4], ry[4];
    int X = X_(x), Y = Y_(y);
    long long g = dark ? 128 : -128;                     /* half a device px in, or out */
    long long x0 = (long long)aui_dev(X) * 256 + g, x1 = (long long)aui_dev(X + w) * 256 - g;
    long long y0 = (long long)aui_dev(Y) * 256 + g, y1 = (long long)aui_dev(Y + h) * 256 - g;
    long long rdx = (long long)devlen(X, r) * 256 - g, rdy = (long long)devlen(Y, r) * 256 - g;
    if (rdx < 0 || rdy < 0) rdx = rdy = 0;
    for (int i = 0; i < 4; i++) { rx[i] = rdx; ry[i] = rdy; }
    ap_begin(&p, 1);
    ap_rrect(&p, x0, y0, x1, y1, rx, ry);
    gfx_zero(&pa, sizeof pa); gfx_zero(&pt, sizeof pt); gfx_zero(&st, sizeof st);
    pa.verbs = p.v; pa.xy = p.xy; pa.nverb = (unsigned)p.nv; pa.nxy = (unsigned)p.nxy;
    pa.rule = LOGIT_PATH2_NONZERO;
    if (dark) {
        pt.kind = LOGIT_PAINT2_LINEAR;
        pt.x0 = X * 256; pt.y0 = Y * 256; pt.x1 = X * 256; pt.y1 = (Y + h) * 256;
        pt.nstop = CARD_STOPS; pt.stop_pos = pos; pt.stop_argb = col;
        pt.argb = LOGIT_DARK_RIM_TOP;
    } else {
        pt.kind = LOGIT_PAINT2_SOLID;
        pt.argb = LOGIT_LIGHT_OUTLINE;
    }
    st.width = (int)dev_ceil_pt24(256);                  /* one device pixel */
    st.cap = LOGIT_STROKE2_CAP_BUTT; st.join = LOGIT_STROKE2_JOIN_MITER; st.miter = 4 * 65536;
    ad_note_path(X, Y, w, h, 1, p.v, p.nv, p.xy, p.nxy, (dark ? 0x72696du : 0x6f75746cu) ^ pt.argb);
    ol_window_path2(&pa, &pt, &st);
}

void aui_card(int x, int y, int w, int h, int elev)
{
    if (w <= 0 || h <= 0) return;
    aui_ensure();
    /* E1 is a light-mode level (3.3: "cards (light)"): a resting dark card is
     * lit by its rim instead. A card raised above rest keeps its shadow. */
    if (!theme_dark || elev >= AUI_ELEV_2) aui_shadow(x, y, w, h, AUI_R_LG, elev);
    aui_round(x, y, w, h, AUI_R_LG, aui_t.bg_card);
    card_edge(x, y, w, h, AUI_R_LG, theme_dark);
}

/* The legacy glass call is the design's REGULAR material (P4): one GLASS2 with
 * the SYMMETRIC rim, or its Reduce Transparency solid. */
void aui_glass(int x, int y, int w, int h, int radius)
{
    aui_material(aui_r(x, y, w, h), radius < 0 ? 0 : radius, LOGIT_MAT_REGULAR);
}

#endif /* AUI_PAINT_MASK */

#ifdef AUI_PAINT_API2
/* ===========================================================================
 * 「澄」 materials, backdrops, images and elevation (design P4, 3.2, 3.3; added
 * by ui-aui-core). aui.c includes this file a THIRD time for these, after the
 * glass capability probe (glass_ctl_ok / glass_reduced) they share with the
 * glass control faces. The legacy aui_glass / aui_elevation / aui_shadow /
 * aui_card above are built on these (ui-aui-theme).
 *
 * THE CONTROL: -DUI_AUI_CORE_NEGCTL asks the kernel for the DIRECTIONAL rim,
 * whose upper-left light is the dated Dock glint principle 2 retires; make
 * test-ui-aui-core-api-negctl must report 'FAIL material-rim'. */
#ifdef UI_AUI_CORE_NEGCTL
#define MAT_RIM OL_GLASS_RIM_DIRECTIONAL
#else
#define MAT_RIM OL_GLASS_RIM_SYMMETRIC
#endif

/* How far a material of a tier reaches past its rect, device px, for the
 * frame's damage (index LOGIT_TIER_*: REGULAR, THIN, THICK, ULTRA). */
static int mat_halo_dev(int tier)
{
    static const unsigned char reach[4] = { 6, 3, 12, 20 };
    return reach[tier >= 0 && tier <= 3 ? tier : 0];
}

/* r < 0: the material's own radius (a capsule is half the short side). */
static int mat_radius(struct aui_rect rc, int r, const struct logit_material *m)
{
    if (r < 0) r = m->radius_pt == LOGIT_R_CAPSULE ? imin(rc.w, rc.h) / 2
                 : m->radius_pt > 0 ? m->radius_pt : 0;
    return clamp_radius(rc.w, rc.h, r);
}
/* TL TR BR BL: a cut side squares both of its corners (the menu bar). */
static void mat_radii(int r, unsigned cut, int rad[4])
{
    rad[0] = (cut & (LOGIT_CUT_TOP | LOGIT_CUT_LEFT)) ? 0 : r;
    rad[1] = (cut & (LOGIT_CUT_TOP | LOGIT_CUT_RIGHT)) ? 0 : r;
    rad[2] = (cut & (LOGIT_CUT_BOTTOM | LOGIT_CUT_RIGHT)) ? 0 : r;
    rad[3] = (cut & (LOGIT_CUT_BOTTOM | LOGIT_CUT_LEFT)) ? 0 : r;
}

/* The damage note of a material or backdrop drawn at (X, Y, w, h) window
 * points: the rect grown by its tier's reach (ad_add_halo), the device px
 * converted back to points rounding up, +1 for the device-edge rounding, as
 * ad_note_glass2 does. `sample`: a GLASS2 panel blurs the window's own pixels
 * under it, so it is also registered with its sample halo (ad_halo_glass) and
 * ad_finish re-flushes the whole panel when anything changes within reach. */
static void ad_note_mat(int X, int Y, int w, int h, int r, int tier, unsigned sig, int sample)
{
    int pct = aui_scale();
    int hp = sh_up100(mat_halo_dev(tier), pct) + 1;
    struct ad_set s;
    s.n = 0;
    ad_add_halo(&s, X, Y, w, h, hp, hp, hp, hp);
    if (s.n) ad_record(s.r[0].x0, s.r[0].y0, s.r[0].x1 - s.r[0].x0, s.r[0].y1 - s.r[0].y0, sig);
    if (sample) {
        int hs = sh_up100(ad_halo_glass(aui_dev(w), aui_dev(h), aui_dev(r), tier), pct) + 1;
        if (g_ad_gl_n < AD_GLMAX) {
            struct ad_gl *g = &g_ad_gl[g_ad_gl_n++];
            g->x = X; g->y = Y; g->w = w; g->h = h; g->halo = hs;
        } else g_ad_overflow = 1;
    }
}

/* The device-snapped 24.8 rect and the one device radius of a shape at window
 * points (X, Y, w, h), the way glass_face and shadow2_emit decide theirs. */
static void mat_geom(int X, int Y, int w, int h, int r, int out[5])
{
    out[0] = (int)dev_ceil_pt24((long long)aui_dev(X) * 256);
    out[1] = (int)dev_ceil_pt24((long long)aui_dev(Y) * 256);
    out[2] = (int)dev_ceil_pt24((long long)aui_dev(X + w) * 256) - out[0];
    out[3] = (int)dev_ceil_pt24((long long)aui_dev(Y + h) * 256) - out[1];
    out[4] = (int)dev_ceil_pt24((long long)imax(0, aui_dev(r)) * 256);
}

static unsigned mat_sig(unsigned kind, int r, unsigned tint, unsigned fill, int tier)
{
    unsigned sig = 2166136261u;
    AD_MIX(sig, kind); AD_MIX(sig, r); AD_MIX(sig, tint); AD_MIX(sig, fill);
    AD_MIX(sig, tier); AD_MIX(sig, MAT_RIM);
    return sig;
}

/* The Reduce Transparency solid: ONE rounded rect (one SYS_GUI_PATH on a
 * PATH kernel) of the material's RT colour, which may carry alpha. */
static void mat_solid(struct aui_rect rc, int r, const struct logit_material *m, int dark)
{
    int rad[4];
    mat_radii(r, m->cut, rad);
    aui_round4(rc, rad, dark ? m->rt_d : m->rt_l);
}

#ifndef AUI_NO_AA
/* One SYS_GUI_GLASS2; 1 when the kernel drew it. */
static int mat_glass2(struct aui_rect rc, int r, const struct logit_material *m, int dark)
{
    struct logit_glass2 g;
    unsigned tint = dark ? m->tint_d : m->tint_l;
    int X = X_(rc.x), Y = Y_(rc.y), q[5], k;
    mat_geom(X, Y, rc.w, rc.h, r, q);
    gfx_zero(&g, sizeof g);
    g.x = q[0]; g.y = q[1]; g.w = q[2]; g.h = q[3];
    for (k = 0; k < 4; k++) g.radius[k] = q[4];
    g.cut = m->cut;
    g.tint_argb = tint & 0xFFFFFFu;
    g.opacity = (int)(tint >> 24); g.tone = 0;
    g.blur_level = m->tier; g.rim = MAT_RIM;
    g.flags = LOGIT_RECT_SNAP;
    if (ol_window_glass2(&g) != 0) return 0;
    ad_note_mat(X, Y, rc.w, rc.h, r, m->tier, mat_sig(0x6d61u, r, tint, m->cut, m->tier), 1);
    return 1;
}
#endif

void aui_material(struct aui_rect rc, int r, enum logit_mat mat)
{
    const struct logit_material *m = logit_material(mat);
    int dark;
    if (rc.w <= 0 || rc.h <= 0) return;
    aui_ensure();
    dark = theme_dark;
    r = mat_radius(rc, r, m);
#ifndef AUI_NO_AA
    if (glass_ctl_ok() && mat_glass2(rc, r, m, dark)) return;
#endif
    mat_solid(rc, r, m, dark);
}

/* ---- backdrops (SYS_GUI_BACKDROP) ---- */
#if defined(AUI_NO_AA)
#define bd_ok() 0
#else
static int bd_caps = -1;                  /* -1 not asked yet, 0 no, 1 yes */
static int bd_ok(void)
{
    if (bd_caps < 0) {
        long i = ol_gui2_info(0);
        bd_caps = (i > 0 && (i & LOGIT_GUI2_F_BACKDROP)) ? 1 : 0;
    }
    return bd_caps && !glass_reduced;
}
#endif
static unsigned bd_live;                  /* regions this process registered, bit per id */

void aui_backdrop_clear(int id)
{
    if (id < -1 || id >= LOGIT_BACKDROP_MAX) return;
    if (id < 0) bd_live = 0; else bd_live &= ~(1u << id);
    ol_window_backdrop_clear2(id);
}

void aui_backdrop(int id, struct aui_rect rc, int r, enum logit_mat mat)
{
    const struct logit_material *m = logit_material(mat);
    int dark;
    if (id < 0 || id >= LOGIT_BACKDROP_MAX || rc.w <= 0 || rc.h <= 0) return;
    aui_ensure();
    dark = theme_dark;
    r = mat_radius(rc, r, m);
    if (bd_ok()) {
        struct logit_backdrop2 b;
        unsigned tint = dark ? m->tint_d : m->tint_l, fill = (dark ? m->rt_d : m->rt_l) & 0xFFFFFFu;
        int X = X_(rc.x), Y = Y_(rc.y), q[5], rad[4], k;
        mat_geom(X, Y, rc.w, rc.h, r, q);
        mat_radii(q[4], m->cut, rad);
        gfx_zero(&b, sizeof b);
        b.id = id;
        b.x = q[0]; b.y = q[1]; b.w = q[2]; b.h = q[3];
        for (k = 0; k < 4; k++) b.radius[k] = rad[k];
        b.tint_argb = tint & 0xFFFFFFu;
        b.opacity = (int)(tint >> 24); b.tone = 0;
        b.blur_level = m->tier; b.rim = MAT_RIM;
        b.fill_rgb = fill;
        b.flags = LOGIT_RECT_SNAP;
        if (ol_window_backdrop2(&b) == 0) {
            bd_live |= 1u << id;
            ad_note_mat(X, Y, rc.w, rc.h, r, m->tier, mat_sig(0x6264u + (unsigned)id, r, tint, fill, m->tier), 0);
            return;
        }
    }
    /* Solid. A region registered while glass was allowed would keep showing
     * the desktop through this solid's pixels' neighbours: drop it. */
    if (bd_live & (1u << id)) aui_backdrop_clear(id);
    mat_solid(rc, r, m, dark);
}

/* ---- images (SYS_GUI_BLIT2) ---- */
static int blit2_caps = -1;
static int blit2_ok(void)
{
    if (blit2_caps < 0) {
        long i = ol_gui2_info(0);
        blit2_caps = (i > 0 && (i & LOGIT_GUI2_F_BLIT2)) ? 1 : 0;
    }
    return blit2_caps;
}

void aui_image(const unsigned char *rgba, int sw, int sh, struct aui_rect d, int filter)
{
    int X, Y;
    if (!rgba || sw <= 0 || sh <= 0 || d.w <= 0 || d.h <= 0) return;
    if (filter < LOGIT_FILTER_NEAREST || filter > LOGIT_FILTER_BOX) filter = LOGIT_FILTER_NEAREST;
    X = X_(d.x); Y = Y_(d.y);
    if (blit2_ok()) {
        struct logit_blit2 b;
        int q[5];
        mat_geom(X, Y, d.w, d.h, 0, q);
        gfx_zero(&b, sizeof b);
        b.rgba = rgba; b.sw = sw; b.sh = sh; b.sstride = sw * 4;
        b.src_x = 0; b.src_y = 0; b.src_w = sw; b.src_h = sh;
        b.x = q[0]; b.y = q[1]; b.w = q[2]; b.h = q[3];
        b.opacity = 255; b.filter = filter; b.flags = LOGIT_RECT_SNAP;
        if (ol_window_blit2(&b) == 0) {
            /* The whole buffer is hashed, as ad_note_blit does: a sampled
             * hash could miss a change (an under-report). */
            unsigned sig = 2166136261u;
            AD_MIX(sig, sw); AD_MIX(sig, sh); AD_MIX(sig, filter);
            sig = ad_hash_bytes(rgba, (long)sw * sh * 4, sig);
            ad_record(X, Y, d.w, d.h, sig);
            return;
        }
    }
    gui_blit(X, Y, d.w, d.h, rgba, sw, sh);
}

/* ---- elevation (two SHADOW2 per level) ---- */
static void elev_layer(struct aui_rect rc, int r, const struct logit_shadow_layer *l, unsigned argb)
{
    int sp = l->spread / 256, blur;
#ifndef AUI_NO_AA
    if (shadow2_ok()) {
        struct logit_shadow2 s;
        int X = X_(rc.x), Y = Y_(rc.y), q[5], k;
        mat_geom(X, Y, rc.w, rc.h, r, q);
        gfx_zero(&s, sizeof s);
        s.x = q[0]; s.y = q[1]; s.w = q[2]; s.h = q[3];
        for (k = 0; k < 4; k++) s.radius[k] = q[4];
        s.dx = l->dx; s.dy = l->dy; s.blur = l->sigma_pt; s.spread = l->spread;
        s.argb = argb;
        s.flags = LOGIT_RECT_SNAP;
        if (ol_window_shadow2(&s) == 0) {
            ad_note_shadow2(X, Y, rc.w, rc.h, r, l->dx / 256, l->dy / 256, l->sigma_pt, sp, argb, 0);
            return;
        }
    }
#endif
    /* The tile fallback, aui_shadow2's: extent 2 sigma, dx and spread folded
     * into the caster rect. */
    blur = (2 * l->sigma_pt + 255) / 256;
    if (blur <= 0) return;
    shadow_tiles(rc.x + l->dx / 256 - sp, rc.y - sp, rc.w + 2 * sp, rc.h + 2 * sp, imax(0, r + sp),
                 l->dy / 256, blur, (int)(argb >> 24), argb & 0xFFFFFFu);
}

void aui_elev(struct aui_rect rc, int r, enum logit_elev e)
{
    const struct logit_elevation *v = logit_elevation(e);
    int dark;
    if (rc.w <= 0 || rc.h <= 0 || v->n < 2) return;
    aui_ensure();
    dark = theme_dark;
    r = clamp_radius(rc.w, rc.h, r);
    elev_layer(rc, r, &v->key, logit_shadow_argb(&v->key, dark));
    elev_layer(rc, r, &v->ambient, logit_shadow_argb(&v->ambient, dark));
}
#endif /* AUI_PAINT_API2 */
