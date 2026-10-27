/* Open Logit -- conic gradient, repeat/reflect spread, 16 stops and premultiplied
 * stop interpolation (c/lib/gfx/raster/gfx_gradient.c), against double references.
 *
 *   conic-angle   a 256x256 conic against libm atan2: <= 2/255 colour error
 *   conic-seam    periodic stops are continuous across the start angle
 *   spread-*      repeat is periodic and reflect mirror-symmetric (linear, radial, conic)
 *   fade-hue      red -> transparent black at its midpoint is alpha 128 +-1 and
 *                 UNPREMULTIPLIED rgb 255,0,0 +-1 (a straight lerp gives 128,0,0)
 *   stops-16      16 stops are kept, a 17th is dropped, ol_cmd_fill accepts them
 *   opaque-bit    alpha 255 and equal-alpha stops match the old straight formula exactly
 *   paint-ok      ol_cmd_fill takes GFX_CONIC, refuses a bad spread and GFX_IMAGE
 *   wm2-path      SYS_GUI_PATH accepts conic, spread and 16 stops and advertises PATH_EXT
 *
 * -DGFX_STOPS_STRAIGHT_LERP (compiled into gfx_gradient.c) is the control: the
 * 'fade-hue' check must FAIL. Exits 1 on any failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "gfx.h"
#include "gfx_paint_internal.h"
#include "wm_draw2.h"
#include "openlogit_display.h"

int (*gfx_ctx_hook)(void);   /* no fb.c here */
extern const unsigned wm2_path_info;

static int fails, checks;
static void ck(int cond, const char *what, const char *detail)
{
    checks++;
    printf("%-4s %s%s%s\n", cond ? "ok" : "FAIL", what, detail && *detail ? "  " : "", detail ? detail : "");
    if (!cond) fails++;
}

#define PI 3.14159265358979323846
#define SZ 256
static unsigned char px[SZ * SZ * 4];
static unsigned char full[SZ];
static struct gfx_surface surf;

static void clear(void) { memset(px, 0, sizeof px); }
static void render(const struct gfx_paint *p)
{
    clear();
    gfx_surface_init(&surf, px, SZ, SZ, SZ * 4);
    memset(full, 255, sizeof full);
    for (int y = 0; y < SZ; y++) gfx_paint_row(p, &surf, y, 0, SZ, full, 0);
}
static const unsigned char *at(int x, int y) { return px + (y * SZ + x) * 4; }
static int maxdiff(const unsigned char *a, const unsigned char *b)
{
    int m = 0;
    for (int i = 0; i < 4; i++) { int d = abs(a[i] - b[i]); if (d > m) m = d; }
    return m;
}

/* ------------------------------------------------------------ conic-angle -- */
static void conic_angle(void)
{
    const int cx = 128 * 256 + 37, cy = 128 * 256 - 91;
    const int starts[] = { 0, 30 * 256, -75 * 256 - 100, 400 * 256 };
    for (unsigned s = 0; s < sizeof starts / sizeof *starts; s++) {
        struct gfx_paint p;
        gfx_paint_conic(&p, cx, cy, starts[s]);
        gfx_paint_stop(&p, 0, 0x000000, 255);
        gfx_paint_stop(&p, GFX_MONE, 0xFFFFFF, 255);
        render(&p);
        int worst = 0, n = 0, wx = 0, wy = 0;
        for (int y = 0; y < SZ; y++)
            for (int x = 0; x < SZ; x++) {
                double dx = (x * 256 + 128 - cx) / 256.0, dy = (y * 256 + 128 - cy) / 256.0;
                double deg = atan2(dy, dx) * 180.0 / PI - starts[s] / 256.0;
                deg = fmod(deg, 360.0); if (deg < 0) deg += 360.0;
                double t = deg / 360.0;
                if (t < 1e-4 || t > 1 - 1e-4) continue;          /* the seam itself */
                int want = (int)floor(t * 255.0 + 0.5), got = at(x, y)[0];
                int e = abs(want - got);
                if (e > worst) { worst = e; wx = x; wy = y; }
                n++;
            }
        char d[96];
        snprintf(d, sizeof d, "start %d: max err %d/255 at (%d,%d), %d px", starts[s], worst, wx, wy, n);
        ck(worst <= 2 && n > 60000, "conic-angle", d);
    }
}

/* -------------------------------------------------------------- conic-seam -- */
static void conic_seam(void)
{
    struct gfx_paint p;
    const int cx = 128 * 256, cy = 128 * 256;
    gfx_paint_conic(&p, cx, cy, 0);
    gfx_paint_stop(&p, 0, 0xFF0000, 255);
    gfx_paint_stop(&p, GFX_MONE / 2, 0x0000FF, 255);
    gfx_paint_stop(&p, GFX_MONE, 0xFF0000, 255);
    int a0, a1;
    unsigned c0 = gfx_paint_sample(&p, 0, &a0), c1 = gfx_paint_sample(&p, GFX_MONE, &a1);
    ck(c0 == c1 && a0 == a1, "conic-seam", "stop table closes on itself");
    render(&p);
    /* the +x axis is the start: the rows just above and below it straddle the seam */
    int worst = 0;
    for (int x = 140; x < 250; x++) {
        int d = maxdiff(at(x, 127), at(x, 128));
        if (d > worst) worst = d;
    }
    char d[64];
    snprintf(d, sizeof d, "max step across the seam %d/255", worst);
    ck(worst <= 6, "conic-seam", d);
}

/* ---------------------------------------------------------------- spreads -- */
static void spreads(void)
{
    struct gfx_paint p;
    /* linear: 64 px ramp along x */
    gfx_paint_linear(&p, 0, 0, 64 * 256, 0);
    gfx_paint_stop(&p, 0, 0xFF0000, 255);
    gfx_paint_stop(&p, GFX_MONE / 2, 0x00FF00, 128);
    gfx_paint_stop(&p, GFX_MONE, 0x0000FF, 255);
    p.spread = GFX_SPREAD_REPEAT;
    render(&p);
    int bad = 0;
    for (int x = 0; x < 192; x++) bad += maxdiff(at(x, 5), at(x + 64, 5)) != 0;
    ck(bad == 0 && maxdiff(at(3, 5), at(40, 5)) > 20, "spread-repeat-linear", "period 64 px");
    p.spread = GFX_SPREAD_REFLECT;
    render(&p);
    bad = 0;
    for (int x = 0; x < 128; x++) bad += maxdiff(at(x, 5), at(127 - x, 5)) != 0;
    for (int x = 0; x < 128; x++) bad += maxdiff(at(x, 5), at(x + 128, 5)) != 0;
    ck(bad == 0, "spread-reflect-linear", "mirror about 64 px, period 128");
    p.spread = GFX_SPREAD_PAD;
    render(&p);
    ck(maxdiff(at(200, 5), at(255, 5)) == 0 && at(200, 5)[2] == 255, "spread-pad-linear", "holds the end stop");

    /* radial: distances along the centre row are exact multiples of a pixel */
    gfx_paint_radial(&p, 100 * 256 + 128, 100 * 256 + 128, 32 * 256);
    gfx_paint_stop(&p, 0, 0xFF0000, 255);
    gfx_paint_stop(&p, GFX_MONE, 0x0000FF, 255);
    p.spread = GFX_SPREAD_REPEAT;
    render(&p);
    bad = 0;
    for (int k = 0; k < 100; k++) bad += maxdiff(at(100 + k, 100), at(100 + k + 32, 100)) != 0 && 100 + k + 32 < SZ;
    ck(bad == 0, "spread-repeat-radial", "period r");
    p.spread = GFX_SPREAD_REFLECT;
    render(&p);
    bad = 0;
    for (int k = 0; k <= 64; k++) bad += maxdiff(at(100 + k, 100), at(100 + 64 - k, 100)) != 0;
    ck(bad == 0, "spread-reflect-radial", "mirror about r");

    /* conic: reflect runs the ramp out and back, so +-theta match (+-2 levels of rounding) */
    gfx_paint_conic(&p, 128 * 256, 128 * 256, 0);
    gfx_paint_stop(&p, 0, 0xFF0000, 255);
    gfx_paint_stop(&p, GFX_MONE, 0x0000FF, 255);
    p.spread = GFX_SPREAD_REFLECT;
    render(&p);
    int worst = 0;
    for (int dy = 1; dy < 120; dy++)
        for (int dx = -120; dx < 120; dx += 7) {
            int d = maxdiff(at(128 + dx, 128 + dy - 1), at(128 + dx, 128 - dy));
            if (d > worst) worst = d;
        }
    char d[64];
    snprintf(d, sizeof d, "max mirror error %d/255", worst);
    ck(worst <= 2, "spread-reflect-conic", d);
    ck(maxdiff(at(240, 127), at(240, 128)) <= 3 && maxdiff(at(16, 127), at(16, 128)) <= 6,
       "spread-reflect-conic", "seamless at the start angle and its opposite");
}

/* -------------------------------------------------------------- fade-hue -- */
static void fade_hue(void)
{
    struct gfx_paint p;
    gfx_paint_linear(&p, 0, 0, SZ * 256, 0);
    gfx_paint_stop(&p, 0, 0xFF0000, 255);
    gfx_paint_stop(&p, GFX_MONE, 0x000000, 0);
    int a;
    unsigned c = gfx_paint_sample(&p, GFX_MONE / 2, &a);
    char d[96];
    snprintf(d, sizeof d, "midpoint alpha %d rgb %d,%d,%d", a, GFX_R(c), GFX_G(c), GFX_B(c));
    ck(abs(a - 128) <= 1 && abs(GFX_R(c) - 255) <= 1 && GFX_G(c) <= 1 && GFX_B(c) <= 1, "fade-hue", d);
    /* the same through the span loop onto a transparent surface (stores the colour as is) */
    render(&p);
    const unsigned char *q = at(128, 3);
    snprintf(d, sizeof d, "row pixel %d,%d,%d a%d", q[0], q[1], q[2], q[3]);
    ck(abs(q[3] - 128) <= 2 && q[0] >= 254 && q[1] <= 1 && q[2] <= 1, "fade-hue", d);
    /* and a conic carries the same ramp */
    gfx_paint_conic(&p, 128 * 256, 128 * 256, 0);
    gfx_paint_stop(&p, 0, 0xFF0000, 255);
    gfx_paint_stop(&p, GFX_MONE, 0x000000, 0);
    render(&p);
    q = at(128, 200);                                     /* half a turn clockwise... a quarter: 90 deg */
    snprintf(d, sizeof d, "conic quarter %d,%d,%d a%d", q[0], q[1], q[2], q[3]);
    ck(q[0] >= 254 && q[1] <= 1 && q[2] <= 1 && abs(q[3] - 191) <= 3, "fade-hue", d);
}

/* ----------------------------------------------------------------- stops -- */
static void stops16(void)
{
    struct gfx_paint p;
    gfx_paint_conic(&p, 0, 0, 0);
    for (int i = 0; i < 17; i++) gfx_paint_stop(&p, i * 4096, GFX_RGB(i * 15, 255 - i * 15, 7), 255);
    ck(p.nstop == 16 && GFX_MAX_STOPS == 16, "stops-16", "16 kept, the 17th dropped");
    int a;
    unsigned c = gfx_paint_sample(&p, 15 * 4096, &a);
    ck(c == GFX_RGB(15 * 15, 255 - 15 * 15, 7), "stops-16", "last stop reachable");

    void *dm = calloc(1, ol_device_size()), *lm = calloc(1, 65536);
    struct ol_device *dev; struct ol_list *l; struct gfx_path gp;
    static int pt[64], sub[8];
    if (ol_device_create(dm, ol_device_size(), OL_API_VERSION, OL_CAP_ATOMIC_FRAME, &dev) ||
        ol_list_create(dev, lm, 65536, &l)) abort();
    gfx_path_init(&gp, pt, 32, sub, 8);
    gfx_path_rect(&gp, 0, 0, 10 * 256, 10 * 256);
    ol_list_reset(l);
    int r16 = ol_cmd_fill(l, &gp, GFX_NONZERO, &p, 0, 4);
    ck(r16 == OL_OK, "stops-16", "ol_cmd_fill accepts a 16-stop conic");
    struct gfx_paint bad = p;
    bad.nstop = 17;
    ol_list_reset(l);
    ck(ol_cmd_fill(l, &gp, GFX_NONZERO, &bad, 0, 4) == OL_ARGUMENT, "paint-ok", "17 stops refused");
    bad = p; bad.spread = 3;
    ol_list_reset(l);
    ck(ol_cmd_fill(l, &gp, GFX_NONZERO, &bad, 0, 4) == OL_ARGUMENT, "paint-ok", "spread 3 refused");
    bad = p; bad.spread = GFX_SPREAD_REFLECT;
    ol_list_reset(l);
    ck(ol_cmd_fill(l, &gp, GFX_NONZERO, &bad, 0, 4) == OL_OK, "paint-ok", "reflect accepted");
    bad = p; bad.kind = GFX_CONIC + 1;
    ol_list_reset(l);
    ck(ol_cmd_fill(l, &gp, GFX_NONZERO, &bad, 0, 4) == OL_ARGUMENT, "paint-ok", "kind past conic refused");
    bad = p; bad.kind = GFX_IMAGE;
    ol_list_reset(l);
    ck(ol_cmd_fill(l, &gp, GFX_NONZERO, &bad, 0, 4) == OL_ARGUMENT, "paint-ok", "image still refused");
    free(dm); free(lm);
}

/* The pre-change formula, written out independently. */
static unsigned old_sample(const struct gfx_paint *p, int t, int *alpha)
{
    if (t < 0) t = 0;
    if (t > GFX_MONE) t = GFX_MONE;
    if (t <= p->stop[0].t) { *alpha = p->stop[0].alpha; return p->stop[0].color; }
    for (int i = 1; i < p->nstop; i++)
        if (t <= p->stop[i].t) {
            int span = p->stop[i].t - p->stop[i - 1].t;
            int k = span > 0 ? (t - p->stop[i - 1].t) * 255 / span : 255;
            *alpha = p->stop[i - 1].alpha + (p->stop[i].alpha - p->stop[i - 1].alpha) * k / 255;
            unsigned a = p->stop[i - 1].color, b = p->stop[i].color;
            int r = GFX_R(a) + (GFX_R(b) - GFX_R(a)) * k / 255, g = GFX_G(a) + (GFX_G(b) - GFX_G(a)) * k / 255,
                bl = GFX_B(a) + (GFX_B(b) - GFX_B(a)) * k / 255;
            return GFX_RGB(r, g, bl);
        }
    *alpha = p->stop[p->nstop - 1].alpha;
    return p->stop[p->nstop - 1].color;
}

static unsigned rnd_state = 12345;
static unsigned rnd(void) { rnd_state = rnd_state * 1664525u + 1013904223u; return rnd_state >> 8; }

static void opaque_bit(void)
{
    int diff = 0, diff_eq = 0, samples = 0;
    for (int trial = 0; trial < 200; trial++) {
        struct gfx_paint p;
        if (trial & 1) gfx_paint_linear(&p, 0, 0, 256, 0); else gfx_paint_radial(&p, 0, 0, 256);
        int n = 2 + (int)(rnd() % 7);                       /* the old 2..8 range */
        int eq = trial % 3 == 0, al = 1 + (int)(rnd() % 255);
        for (int i = 0; i < n; i++)
            gfx_paint_stop(&p, (int)(rnd() % 65537), rnd() & 0xFFFFFF, eq ? (trial % 6 == 0 ? 255 : al) : 255);
        for (int t = -100; t <= GFX_MONE + 100; t += 331) {
            int a1, a2;
            unsigned c1 = gfx_paint_sample(&p, t, &a1), c2 = old_sample(&p, t, &a2);
            samples++;
            if (c1 != c2 || a1 != a2) { if (eq) diff_eq++; else diff++; }
        }
    }
    char d[80];
    snprintf(d, sizeof d, "%d samples, %d opaque / %d equal-alpha differ", samples, diff, diff_eq);
    ck(diff == 0 && diff_eq == 0, "opaque-bit", d);
}

/* ---------------------------------------------------------------- wm2_path -- */
enum { W = 96, H = 64 };
static uint32_t wpx[W * H];
static void *t_alloc(unsigned long n) { return malloc(n); }
static void t_free(void *p) { free(p); }
static int t_copyin(void *dst, const void *src, unsigned long n) { memcpy(dst, src, n); return 0; }

static long run_path(struct ol_display *d, unsigned kind, unsigned spread, unsigned nstop)
{
    struct wm2_env e;
    memset(&e, 0, sizeof e);
    e.d = d; e.scale_pct = 100; e.surf_w = W; e.surf_h = H;
    e.copyin = t_copyin; e.alloc = malloc; e.release = free;
    unsigned char verbs[4] = { LOGIT_VERB_M, LOGIT_VERB_L, LOGIT_VERB_L, LOGIT_VERB_Z };
    int xy[6] = { 10 * 256, 10 * 256, 60 * 256, 14 * 256, 30 * 256, 50 * 256 };
    unsigned sp[16], sa[16];
    for (unsigned i = 0; i < nstop; i++) {
        sp[i] = nstop > 1 ? i * 65536 / (nstop - 1) : 0;
        sa[i] = 0xFF000000u | (i * 16u) << 16 | 0x2080u;
    }
    struct logit_path2 pa; struct logit_paint2 q;
    memset(&pa, 0, sizeof pa); memset(&q, 0, sizeof q);
    pa.size = sizeof pa; pa.verbs = verbs; pa.xy = xy; pa.nverb = 4; pa.nxy = 6;
    q.size = sizeof q; q.kind = (unsigned char)kind; q.spread = (unsigned char)spread; q.nstop = (unsigned char)nstop;
    q.x0 = 30 * 256; q.y0 = 25 * 256; q.x1 = 60 * 256; q.y1 = 25 * 256; q.r1 = 20 * 256;
    q.r0 = kind == LOGIT_PAINT2_CONIC ? 45 * 256 : 0;
    q.stop_pos = sp; q.stop_argb = sa;
    return wm2_path(&e, (long)&pa, (long)&q, 0);
}

static void wm2(void)
{
    struct ol_display d;
    memset(&d, 0, sizeof d);
    struct ol_pixel_target t = { wpx, W, H, 0, 0, 0, 0, 0 };
    d.allocate = t_alloc; d.release = t_free;
    ol_display_bind(&d, &t, 16, 8, 0);
    ck((wm2_path_info & LOGIT_GUI2_F_PATH) && (wm2_path_info & LOGIT_GUI2_F_PATH_EXT), "wm2-path", "info advertises PATH and PATH_EXT");
    long r;
    r = run_path(&d, LOGIT_PAINT2_CONIC, 0, 3);  ck(r == OL_OK, "wm2-path", "conic accepted");
    r = run_path(&d, LOGIT_PAINT2_CONIC, 2, 16); ck(r == OL_OK, "wm2-path", "conic + reflect + 16 stops accepted");
    r = run_path(&d, LOGIT_PAINT2_LINEAR, 1, 12); ck(r == OL_OK, "wm2-path", "linear repeat, 12 stops accepted");
    r = run_path(&d, LOGIT_PAINT2_RADIAL, 2, 9); ck(r == OL_OK, "wm2-path", "radial reflect, 9 stops accepted");
    r = run_path(&d, LOGIT_PAINT2_LINEAR, 3, 2); ck(r == OL_ARGUMENT, "wm2-path", "spread 3 refused");
    r = run_path(&d, LOGIT_PAINT2_LINEAR, 0, 17); ck(r == OL_ARGUMENT, "wm2-path", "17 stops refused");
    ol_display_destroy(&d);
}

int main(void)
{
    conic_angle();
    conic_seam();
    spreads();
    fade_hue();
    stops16();
    opaque_bit();
    wm2();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
