/* P16: the adaptive-tint flags reach the material from ring 3.
 *
 * SYS_GUI_GLASS2 (c/kernel/gui/wm_draw2_fx.c) and SYS_GUI_BACKDROP
 * (c/kernel/gui/wm_backdrop.c) are driven through their real handlers over
 * photo-like backdrops (the glass_adapt_test.c generator) and the pixels are
 * compared with ol_display_liquid_glass_ex called directly.
 *
 *   adapt-fg-glass2     GLASS2 + ADAPT_FG: the ink region keeps WCAG >= 4.5:1
 *                       against fg_rgb (white and black), and equals the direct
 *                       call with OL_GLASS_ADAPT_FG
 *   adapt-fg-backdrop   the same for BACKDROP (ink = black on a light fill,
 *                       white on a dark fill), measured on the composited region
 *   unflagged-glass2    without the flag the output equals the direct call with
 *                       flags 0 and the same backdrops DO violate 4.5:1, so the
 *                       checks above are not vacuous
 *   unflagged-backdrop  the same for BACKDROP
 *   smooth-glass2       ADAPT_SMOOTH reaches the material (equals the direct call)
 *   smooth-backdrop     bit 1 of the BACKDROP flags is still LOGIT_BACKDROP_ALPHA_LAYER
 *                       (OL_UNSUPPORTED): SMOOTH cannot reach a BACKDROP yet (see below)
 *   reduce-transp       reduce transparency wins: flagged equals unflagged
 *   unknown-bits        a flag bit above ADAPT_FG is still OL_ARGUMENT
 *
 * Controls: -DUI_ADAPT_NEGCTL drops the flag copy in wm_draw2_fx.c and must
 * print 'FAIL adapt-fg-glass2' (and only the glass2 checks); -DUI_ADAPT_NEGCTL_BD
 * drops it in wm_backdrop.c and must print 'FAIL adapt-fg-backdrop'. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "wm_draw2.h"
#include "wm2_scale.h"
#include "openlogit_glass_material.h"

#ifndef LOGIT_GLASS2_F_ADAPT_SMOOTH
#define LOGIT_GLASS2_F_ADAPT_SMOOTH (1u << 1)
#define LOGIT_GLASS2_F_ADAPT_FG (1u << 2)
#endif

enum { W = 160, H = 120, CX = 80, CY = 60, WX = 0, WY = 0 };
static int failed;
static void fail(const char *check, const char *what)
{
    printf("FAIL %s (%s)\n", check, what);
    failed = 1;
}

static uint32_t *canvas, *oracle, *photo_px;
static struct ol_display D, O;
static int reduce;

static int t_copyin(void *dst, const void *src, unsigned long n)
{
    if (!src) return -1;
    memcpy(dst, src, n);
    return 0;
}
static void *t_alloc(unsigned long n) { return malloc(n); }
static void t_release(void *p) { free(p); }
static void bind_to(struct ol_display *d, uint32_t *px)
{
    memset(d, 0, sizeof *d);
    d->allocate = malloc; d->release = free;
    struct ol_pixel_target t = { .px = px, .w = W, .h = H };
    ol_display_bind(d, &t, 16, 8, 0);
    d->reduce_transparency = reduce;
}
static void env_for(struct wm2_env *e)
{
    memset(e, 0, sizeof *e);
    e->d = &D; e->scale_pct = 100; e->surf_w = W; e->surf_h = H; e->slot = 2;
    e->copyin = t_copyin; e->alloc = t_alloc; e->release = t_release;
}

static double lin(int c8)
{
    double c = c8 / 255.0;
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
static double rel_lum(uint32_t p)
{
    return 0.2126 * lin(p >> 16 & 255) + 0.7152 * lin(p >> 8 & 255) + 0.0722 * lin(p & 255);
}
static double contrast(uint32_t a, uint32_t b)
{
    double x = rel_lum(a), y = rel_lum(b);
    if (x < y) { double t = x; x = y; y = t; }
    return (x + 0.05) / (y + 0.05);
}

static uint32_t rng = 0x9E3779B9u;
static int rnd(int n) { rng = rng * 1664525u + 1013904223u; return (int)((rng >> 8) % (unsigned)n); }
static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static void photo(uint32_t *out)
{
    int gray = rnd(256);
    int cast[3] = {rnd(61) - 30, rnd(61) - 30, rnd(61) - 30};
    int gx = rnd(61) - 30, gy = rnd(61) - 30;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int g = gray + gx * (x - W / 2) / W + gy * (y - H / 2) / H;
            int n = rnd(9) - 4;
            out[y * W + x] = (uint32_t)clamp8(g + cast[0] + n) << 16 |
                             (uint32_t)clamp8(g + cast[1] + n) << 8 | (uint32_t)clamp8(g + cast[2] + n);
        }
}

static const uint32_t TINTS[] = {0x000000, 0x808080, 0xFFFFFF, 0x3050A0};
static const int OPACS[] = {0, 30, 100, 200};

/* ---- GLASS2 ---------------------------------------------------------------- */
static struct logit_glass2 mkglass(uint32_t tint, int opacity, unsigned flags, uint32_t fg)
{
    struct logit_glass2 g;
    memset(&g, 0, sizeof g);
    g.size = sizeof g; g.x = 30 << 8; g.y = 20 << 8; g.w = 100 << 8; g.h = 80 << 8;
    for (int k = 0; k < 4; k++) g.radius[k] = 16 << 8;
    g.tint_argb = tint; g.opacity = opacity; g.blur_level = OL_GLASS_BLUR_REGULAR;
    g.rim = OL_GLASS_RIM_SYMMETRIC; g.fg_rgb = fg; g.flags = flags;
    return g;
}
static long glass_call(const struct logit_glass2 *g, uint32_t *px)
{
    bind_to(&D, px);
    struct wm2_env e;
    env_for(&e);
    return wm2_glass2(&e, (long)g, 0, 0);
}
static void glass_oracle(uint32_t tint, int opacity, uint8_t mflags, uint32_t fg, uint32_t *px)
{
    bind_to(&O, px);
    struct ol_glass_material m = {0};
    m.tint = tint; m.opacity = (uint8_t)opacity; m.blur = OL_GLASS_BLUR_REGULAR;
    m.rim = OL_GLASS_RIM_SYMMETRIC; m.flags = mflags; m.fg_rgb = fg;
    ol_display_liquid_glass_ex(&O, 30, 20, 100, 80, 16, 0, &m);
}

static void t_glass2(void)
{
    static const uint32_t FGS[2] = {0xFFFFFF, 0x000000};
    int unflagged_bad = 0, contrast_bad = 0, exact_bad = 0, plain_bad = 0, smooth_bad = 0;
    double worst = 1e9;
    rng = 0x9E3779B9u;
    for (int n = 0; n < 24; n++) {
        photo(photo_px);
        for (int f = 0; f < 2; f++)
            for (unsigned t = 0; t < 4; t++)
                for (unsigned o = 0; o < 4; o++) {
                    uint32_t tint = TINTS[t];
                    int op = OPACS[o];
                    struct logit_glass2 g = mkglass(tint, op, LOGIT_RECT_SNAP | LOGIT_GLASS2_F_ADAPT_FG, FGS[f]);
                    memcpy(canvas, photo_px, W * H * 4);
                    if (glass_call(&g, canvas) != OL_OK) { fail("adapt-fg-glass2", "status"); return; }
                    double c = contrast(canvas[CY * W + CX], FGS[f]);
                    if (c < worst) worst = c;
                    if (c < 4.5) contrast_bad++;
                    memcpy(oracle, photo_px, W * H * 4);
                    glass_oracle(tint, op, OL_GLASS_ADAPT_FG, FGS[f], oracle);
                    if (memcmp(canvas, oracle, W * H * 4)) exact_bad++;
                    /* flag absent: equal to flags 0, and the contrast guarantee is gone */
                    g.flags = LOGIT_RECT_SNAP;
                    memcpy(canvas, photo_px, W * H * 4);
                    glass_call(&g, canvas);
                    memcpy(oracle, photo_px, W * H * 4);
                    glass_oracle(tint, op, 0, FGS[f], oracle);
                    if (memcmp(canvas, oracle, W * H * 4)) plain_bad++;
                    if (contrast(canvas[CY * W + CX], FGS[f]) < 4.5) unflagged_bad++;
                    /* SMOOTH */
                    g.flags = LOGIT_RECT_SNAP | LOGIT_GLASS2_F_ADAPT_SMOOTH;
                    memcpy(canvas, photo_px, W * H * 4);
                    glass_call(&g, canvas);
                    memcpy(oracle, photo_px, W * H * 4);
                    glass_oracle(tint, op, OL_GLASS_ADAPT_SMOOTH, FGS[f], oracle);
                    if (memcmp(canvas, oracle, W * H * 4)) smooth_bad++;
                }
    }
    char b[96];
    if (contrast_bad) { snprintf(b, sizeof b, "%d of 768 below 4.5:1, worst %.2f", contrast_bad, worst); fail("adapt-fg-glass2", b); }
    if (exact_bad) { snprintf(b, sizeof b, "%d renders differ from OL_GLASS_ADAPT_FG", exact_bad); fail("adapt-fg-glass2", b); }
    if (plain_bad) { snprintf(b, sizeof b, "%d unflagged renders differ from flags 0", plain_bad); fail("unflagged-glass2", b); }
    if (unflagged_bad < 100) { snprintf(b, sizeof b, "only %d unflagged violations: vacuous", unflagged_bad); fail("unflagged-glass2", b); }
    if (smooth_bad) { snprintf(b, sizeof b, "%d renders differ from OL_GLASS_ADAPT_SMOOTH", smooth_bad); fail("smooth-glass2", b); }
    printf("glass2: worst flagged contrast %.2f, unflagged violations %d of 768\n", worst, unflagged_bad);
}

/* ---- BACKDROP -------------------------------------------------------------- */
static struct logit_backdrop2 mkbd(uint32_t tint, int opacity, uint32_t fill, unsigned flags)
{
    struct logit_backdrop2 b;
    memset(&b, 0, sizeof b);
    b.size = sizeof b; b.id = 0; b.x = 0; b.y = 0; b.w = 120 << 8; b.h = H << 8;
    b.radius[1] = b.radius[2] = 12 << 8;
    b.tint_argb = tint; b.opacity = opacity; b.blur_level = OL_GLASS_BLUR_REGULAR;
    b.rim = OL_GLASS_RIM_SYMMETRIC; b.fill_rgb = fill; b.flags = flags;
    return b;
}
/* Register the region and composite its material over the desktop in px. */
static long bd_run(const struct logit_backdrop2 *b, uint32_t *desk)
{
    static uint32_t sd_px[W * H];
    struct ol_display sd;
    bind_to(&sd, sd_px);
    struct wm2_env e;
    env_for(&e);
    e.d = &sd;
    wm2_backdrop_win_reset(2);
    long rc = wm2_backdrop(&e, (long)b, 0, 0);
    if (rc != OL_OK) return rc;
    bind_to(&D, desk);
    struct drect R = {0, 0, W, H};
    wm_backdrop_composite(&R, 2, WX, WY, &D);
    return OL_OK;
}
static void bd_oracle(const struct logit_backdrop2 *b, uint8_t mflags, uint32_t fg, uint32_t *px)
{
    bind_to(&O, px);
    struct ol_glass_material m = {0};
    m.tint = b->tint_argb & 0xFFFFFF; m.opacity = (uint8_t)b->opacity; m.blur = OL_GLASS_BLUR_REGULAR;
    m.rim = OL_GLASS_RIM_SYMMETRIC; m.flags = mflags; m.fg_rgb = fg;
    ol_display_liquid_glass_ex(&O, 0, 0, 120, H, 12, OL_GLASS_CUT_LEFT, &m);
}

static void t_backdrop(void)
{
    /* light fill -> black ink, dark fill -> white ink */
    static const uint32_t FILL[2] = {0xE6E6EC, 0x28282E};
    static const uint32_t INK[2] = {0x000000, 0xFFFFFF};
    int unflagged_bad = 0, contrast_bad = 0, exact_bad = 0, plain_bad = 0, runs = 0;
    double worst = 1e9;
    rng = 0x51ED270Bu;
    for (int n = 0; n < 24; n++) {
        photo(photo_px);
        for (int f = 0; f < 2; f++)
            for (unsigned t = 0; t < 4; t++)
                for (unsigned o = 0; o < 4; o++) {
                    uint32_t tint = TINTS[t];
                    int op = OPACS[o];
                    struct logit_backdrop2 b = mkbd(tint, op, FILL[f], LOGIT_RECT_SNAP | LOGIT_GLASS2_F_ADAPT_FG);
                    runs++;
                    memcpy(canvas, photo_px, W * H * 4);
                    if (bd_run(&b, canvas) != OL_OK) { fail("adapt-fg-backdrop", "status"); return; }
                    double c = contrast(canvas[CY * W + 40], INK[f]);
                    if (c < worst) worst = c;
                    if (c < 4.5) contrast_bad++;
                    memcpy(oracle, photo_px, W * H * 4);
                    bd_oracle(&b, OL_GLASS_ADAPT_FG, INK[f], oracle);
                    if (memcmp(canvas, oracle, W * H * 4)) exact_bad++;
                    b.flags = LOGIT_RECT_SNAP;
                    memcpy(canvas, photo_px, W * H * 4);
                    bd_run(&b, canvas);
                    memcpy(oracle, photo_px, W * H * 4);
                    bd_oracle(&b, 0, 0, oracle);
                    if (memcmp(canvas, oracle, W * H * 4)) plain_bad++;
                    if (contrast(canvas[CY * W + 40], INK[f]) < 4.5) unflagged_bad++;
                }
    }
    char s[96];
    if (contrast_bad) { snprintf(s, sizeof s, "%d of %d below 4.5:1, worst %.2f", contrast_bad, runs, worst); fail("adapt-fg-backdrop", s); }
    if (exact_bad) { snprintf(s, sizeof s, "%d renders differ from OL_GLASS_ADAPT_FG", exact_bad); fail("adapt-fg-backdrop", s); }
    if (plain_bad) { snprintf(s, sizeof s, "%d unflagged renders differ from flags 0", plain_bad); fail("unflagged-backdrop", s); }
    if (unflagged_bad < 100) { snprintf(s, sizeof s, "only %d unflagged violations: vacuous", unflagged_bad); fail("unflagged-backdrop", s); }
    printf("backdrop: worst flagged contrast %.2f, unflagged violations %d of %d\n", worst, unflagged_bad, runs);
}

/* ---- reduce transparency and unknown bits ----------------------------------- */
static void t_reduce_unknown(void)
{
    uint32_t *a = malloc(W * H * 4);
    rng = 0xC0FFEEu;
    photo(photo_px);
    reduce = 1;
    struct logit_glass2 g = mkglass(0xFFFFFF, 60, LOGIT_RECT_SNAP, 0xFFFFFF);
    memcpy(a, photo_px, W * H * 4);
    glass_call(&g, a);
    g.flags |= LOGIT_GLASS2_F_ADAPT_FG | LOGIT_GLASS2_F_ADAPT_SMOOTH;
    memcpy(canvas, photo_px, W * H * 4);
    glass_call(&g, canvas);
    if (memcmp(a, canvas, W * H * 4)) fail("reduce-transp", "glass2 flags changed the solid panel");
    struct logit_backdrop2 b = mkbd(0xF0F2F8, 96, 0xE6E6EC, LOGIT_RECT_SNAP);
    memcpy(a, photo_px, W * H * 4);
    bd_run(&b, a);
    b.flags |= LOGIT_GLASS2_F_ADAPT_FG;
    memcpy(canvas, photo_px, W * H * 4);
    bd_run(&b, canvas);
    if (memcmp(a, canvas, W * H * 4)) fail("reduce-transp", "backdrop flags changed the solid panel");
    reduce = 0;

    g = mkglass(0xFFFFFF, 60, LOGIT_RECT_SNAP | (1u << 3), 0);
    memcpy(canvas, photo_px, W * H * 4);
    if (glass_call(&g, canvas) != OL_ARGUMENT) fail("unknown-bits", "glass2 accepted bit 3");
    b = mkbd(0xF0F2F8, 96, 0xE6E6EC, LOGIT_RECT_SNAP | (1u << 3));
    if (bd_run(&b, canvas) != OL_ARGUMENT) fail("unknown-bits", "backdrop accepted bit 3");
    /* smooth-backdrop: bit 1 of logit_backdrop2.flags is also
     * LOGIT_BACKDROP_ALPHA_LAYER (reserved, OL_UNSUPPORTED), so ADAPT_SMOOTH
     * cannot reach a BACKDROP until ui-wm-abi renumbers ALPHA_LAYER. Pin the
     * refusal so the collision is loud; when it is renumbered this check
     * becomes the exact-render check glass2 has. */
    b = mkbd(0xF0F2F8, 96, 0xE6E6EC, LOGIT_RECT_SNAP | LOGIT_GLASS2_F_ADAPT_SMOOTH);
    if (bd_run(&b, canvas) != OL_UNSUPPORTED) fail("smooth-backdrop", "bit 1 no longer refused as ALPHA_LAYER");
    free(a);
}

int main(void)
{
    canvas = malloc(W * H * 4); oracle = malloc(W * H * 4); photo_px = malloc(W * H * 4);
    t_glass2();
    t_backdrop();
    t_reduce_unknown();
    printf(failed ? "ui_glass_adapt: FAILED\n" : "ui_glass_adapt: all ok\n");
    return failed;
}
