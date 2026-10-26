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

#else
/* ----------------------------------------------- 4. drawing primitives */

#define X_(v) ((v) + ox_)
#define Y_(v) ((v) + oy_)

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
void aui_separator(int x, int y, int w) { aui_hairline(x, y, w); }

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

void aui_shadow_ex(int x, int y, int w, int h, int r, int dy, int blur, int alpha)
{
    if (w <= 0 || h <= 0 || blur <= 0 || alpha <= 0) return;
#ifdef AUI_NO_AA
    (void)r; (void)dy;                    /* no shadows without alpha compositing */
    return;
#else
    unsigned col = AUI_SHADOW;
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

void aui_shadow(int x, int y, int w, int h, int r, int elev)
{
    switch (elev) {
        case AUI_ELEV_1: aui_shadow_ex(x, y, w, h, r, 1, 4,  aui_is_dark() ? 90  : 40); break;
        case AUI_ELEV_2: aui_shadow_ex(x, y, w, h, r, 3, 10, aui_is_dark() ? 120 : 55); break;
        case AUI_ELEV_3: aui_shadow_ex(x, y, w, h, r, 8, 22, aui_is_dark() ? 150 : 70); break;
        default: break;
    }
}

void aui_panel(int x, int y, int w, int h, unsigned color) { aui_fill(x, y, w, h, color); }

void aui_card(int x, int y, int w, int h, int elev)
{
    aui_shadow(x, y, w, h, AUI_R_LG, elev);
    aui_round(x, y, w, h, AUI_R_LG, AUI_SURFACE);
    aui_stroke(x, y, w, h, AUI_R_LG, 1, AUI_BORDER);
}

void aui_glass(int x, int y, int w, int h, int radius)
{
    aui_ensure();
    if (theme_dark) gui_glass(X_(x), Y_(y), w, h, radius, 34, 36, 46, 120);
    else            gui_glass(X_(x), Y_(y), w, h, radius, 255, 255, 255, 50);
}

#endif /* AUI_PAINT_MASK */
