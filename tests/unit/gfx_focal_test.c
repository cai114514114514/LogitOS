/* Gradient paint matrix, two-circle radial and ordered dither
 * (c/lib/gfx/raster/gfx_gradient.c, gfx_paint.c, canvas/state.c).
 *
 *   isoline-*     a gradient filled under a rotated / non-uniform canvas CTM follows the
 *                 shape: every opaque pixel matches the double reference computed in user
 *                 space, within 2/255 (linear, elliptical radial; radial2 and conic
 *                 through an explicit pmat)
 *   radial2-*     the two-circle gradient against a double reference, <= 3/255, for a
 *                 focal point inside, a cone (focal outside), the linear case a == 0, and
 *                 concentric circles; fr = 0 with a shared centre equals gfx_paint_radial
 *   dither-*      0x404040 -> 0x484848 over 1024 px: no constant run longer than 140 px
 *                 (here: 64), the mean stays put, and the plain ramp does have long runs
 *   flags0-*      flags 0 ignores pmat; a canvas fill under the identity CTM is untouched
 *
 * -DGFX_GRADIENT_IGNORE_PMAT (compiled into gfx_gradient.c) is the control: 'isoline' must
 * FAIL. Exits 1 on any failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gfx.h"
#include "gfx_paint_internal.h"
#include "openlogit.h"
#include "openlogit_canvas.h"

static int fails, checks;
static void ck(int cond, const char *what, const char *detail)
{
    checks++;
    printf("%-4s %s%s%s\n", cond ? "ok" : "FAIL", what, detail && *detail ? "  " : "", detail ? detail : "");
    if (!cond) fails++;
}

#define W 200
#define H 200
static unsigned char front[W * H * 4], work[W * H * 4];
static struct ol_device *device;
static struct ol_list *list;
static struct ol_surface *surface;

static void ramp_bw(struct gfx_paint *p)
{
    gfx_paint_stop(p, 0, 0x000000, 255);
    gfx_paint_stop(p, GFX_MONE, 0xFFFFFF, 255);
}

static double inv_pt(const struct gfx_matrix *m, double x, double y, double *oy)
{
    double a = m->a / 65536.0, b = m->b / 65536.0, c = m->c / 65536.0, d = m->d / 65536.0;
    double e = m->e / 256.0, f = m->f / 256.0, det = a * d - b * c;
    x -= e; y -= f;
    *oy = (-b * x + a * y) / det;
    return (d * x - c * y) / det;
}
static double clamp01(double t) { return t < 0 ? 0 : (t > 1 ? 1 : t); }

/* Fill `rect` (user space) under the CTM and check each opaque pixel against `t_of`. */
typedef double (*t_fn)(double ux, double uy);
static double t_linear(double ux, double uy) { (void)uy; return ux / 100.0; }
static double t_radial(double ux, double uy) { return hypot(ux, uy) / 40.0; }

static void canvas_isoline(const char *name, const struct gfx_matrix *steps, int nsteps,
                           struct gfx_paint *paint, struct gfx_rect rect, t_fn fn)
{
    struct ol_canvas canvas;
    ol_list_reset(list);
    ol_canvas_init(&canvas, list, W, H);
    for (int i = 0; i < nsteps; i++) ol_canvas_transform(&canvas, &steps[i]);
    ol_cmd_clear(list, 0, 0);
    int st = ol_canvas_round_rect(&canvas, rect, 0, paint);
    ol_list_close(list);
    ol_submit(device, list, surface, NULL);
    int worst = 0, n = 0, wx = 0, wy = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const unsigned char *q = front + (y * W + x) * 4;
            if (q[3] != 255) continue;
            double uy, ux = inv_pt(&canvas.state.transform, x + 0.5, y + 0.5, &uy);
            int want = (int)floor(clamp01(fn(ux, uy)) * 255.0 + 0.5);
            int e = abs(want - q[0]);
            if (e > worst) { worst = e; wx = x; wy = y; }
            n++;
        }
    char d[112];
    snprintf(d, sizeof d, "status %d, max err %d/255 at (%d,%d), %d px", st, worst, wx, wy, n);
    ck(st == OL_OK && worst <= 2 && n > 3000, name, d);
}

static void isolines(void)
{
    struct gfx_matrix steps[3];
    gfx_m_identity(&steps[0]); gfx_m_translate(&steps[0], 60 * 256, 30 * 256);
    gfx_m_identity(&steps[1]); gfx_m_rotate(&steps[1], 30 * 256);
    struct gfx_paint p;
    /* rotated shape, linear ramp along the user x axis */
    gfx_paint_linear(&p, 0, 0, 100 * 256, 0);
    ramp_bw(&p);
    canvas_isoline("isoline-linear", steps, 2, &p, (struct gfx_rect){0, 0, 100, 40}, t_linear);
    /* the same, sheared: isolines stay perpendicular to the axis in USER space only */
    struct gfx_matrix sh[2];
    gfx_m_identity(&sh[0]); gfx_m_translate(&sh[0], 20 * 256, 10 * 256);
    gfx_m_set(&sh[1], GFX_MONE, 0, GFX_MONE / 2, GFX_MONE, 0, 0);
    canvas_isoline("isoline-shear", sh, 2, &p, (struct gfx_rect){0, 0, 100, 60}, t_linear);
    /* elliptical radial: a circle in user space under scale(2,1) */
    gfx_m_identity(&steps[0]); gfx_m_translate(&steps[0], 100 * 256, 100 * 256);
    gfx_m_set(&steps[1], 2 * GFX_MONE, 0, 0, GFX_MONE, 0, 0);
    gfx_paint_radial(&p, 0, 0, 40 * 256);
    ramp_bw(&p);
    canvas_isoline("isoline-radial", steps, 2, &p, (struct gfx_rect){-45, -45, 90, 90}, t_radial);
}

/* ------------------------------------------------- direct rows (pmat set) -- */
static unsigned char dpx[W * H * 4];
static void render_direct(const struct gfx_paint *p)
{
    struct gfx_surface s;
    unsigned char full[W];
    memset(dpx, 0, sizeof dpx);
    memset(full, 255, sizeof full);
    gfx_surface_init(&s, dpx, W, H, W * 4);
    for (int y = 0; y < H; y++) gfx_paint_row(p, &s, y, 0, W, full, 0);
}

/* Double reference of the two-circle gradient. Returns 0 when no circle covers the point;
 * *edge is set when the pixel is too close to a root / radius sign change to compare. */
static int ref_radial2(const double f[3], const double c[3], double x, double y, double *t, int *edge)
{
    double cdx = c[0] - f[0], cdy = c[1] - f[1], dr = c[2] - f[2];
    double pdx = x - f[0], pdy = y - f[1];
    double a = cdx * cdx + cdy * cdy - dr * dr, b = pdx * cdx + pdy * cdy + f[2] * dr;
    double c0 = pdx * pdx + pdy * pdy - f[2] * f[2];
    double t1, t2;
    *edge = 0;
    if (fabs(a) < 1e-9) {
        if (fabs(b) < 1e-9) return 0;
        t1 = t2 = c0 / (2 * b);
        if (fabs(b) < 20) *edge = 1;
    } else {
        double disc = b * b - a * c0;
        if (disc < 0) { if (disc > -1e-3 * b * b - 1) *edge = 1; return 0; }
        if (disc < 1e-3 * b * b + 1) *edge = 1;
        double s = sqrt(disc);
        t1 = (b + s) / a; t2 = (b - s) / a;
        if (t1 < t2) { double q = t1; t1 = t2; t2 = q; }
    }
    if (fabs(f[2] + t1 * dr) < 0.05) *edge = 1;
    if (f[2] + t1 * dr >= 0) { *t = t1; return 1; }
    if (fabs(f[2] + t2 * dr) < 0.05) *edge = 1;
    if (f[2] + t2 * dr >= 0) { *t = t2; return 1; }
    return 0;
}

static void radial2_case(const char *name, int fx, int fy, int fr, int cx, int cy, int r,
                         const struct gfx_matrix *pmat)
{
    struct gfx_paint p;
    gfx_paint_radial2(&p, fx * 256, fy * 256, fr * 256, cx * 256, cy * 256, r * 256);
    ramp_bw(&p);
    if (pmat) { p.pmat = *pmat; p.flags |= GFX_PAINT_HAS_PMAT; }
    render_direct(&p);
    const double f[3] = {fx, fy, fr}, c[3] = {cx, cy, r};
    int worst = 0, painted = 0, skipped = 0, wrong = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            double ux = x + 0.5, uy = y + 0.5, t;
            if (pmat) {
                double vy, vx = inv_pt(pmat, ux, uy, &vy);
                ux = vx; uy = vy;
            }
            int edge, has = ref_radial2(f, c, ux, uy, &t, &edge);
            const unsigned char *q = dpx + (y * W + x) * 4;
            if (edge) { skipped++; continue; }
            if (has != (q[3] == 255)) { if (wrong++ < 3) printf("   mismatch (%d,%d) ref=%d alpha=%d\n", x, y, has, q[3]); continue; }
            if (!has) continue;
            painted++;
            int want = (int)floor(clamp01(t) * 255.0 + 0.5), e = abs(want - q[0]);
            if (e > worst) worst = e;
        }
    char d[128];
    snprintf(d, sizeof d, "max err %d/255, %d painted, %d unpainted-mismatch, %d edge px skipped",
             worst, painted, wrong, skipped);
    ck(worst <= 3 && wrong == 0 && painted > 500, name, d);
}

static void radial2(void)
{
    radial2_case("radial2-focal-inside", 100, 110, 0, 128, 128, 90, 0);
    radial2_case("radial2-cone", 40, 100, 10, 160, 100, 40, 0);
    radial2_case("radial2-linear-a0", 50, 100, 0, 150, 100, 100, 0);
    radial2_case("radial2-concentric", 100, 100, 20, 100, 100, 90, 0);
    radial2_case("radial2-fr-grows", 100, 100, 30, 120, 100, 30, 0);
    /* under a pmat: paint space is rotated 25 degrees about the centre of the surface */
    struct gfx_matrix m;
    gfx_m_identity(&m);
    gfx_m_translate(&m, 100 * 256, 100 * 256);
    gfx_m_rotate(&m, 25 * 256);
    gfx_m_translate(&m, -100 * 256, -100 * 256);
    radial2_case("isoline-radial2", 70, 100, 0, 110, 100, 60, &m);
    /* the degenerate cone fr=0 on one centre is the plain radial gradient */
    struct gfx_paint a, b;
    gfx_paint_radial2(&a, 100 * 256, 90 * 256, 0, 100 * 256, 90 * 256, 70 * 256); ramp_bw(&a);
    gfx_paint_radial (&b, 100 * 256, 90 * 256, 70 * 256);                      ramp_bw(&b);
    render_direct(&a);
    static unsigned char keep[W * H * 4];
    memcpy(keep, dpx, sizeof keep);
    render_direct(&b);
    int worst = 0;
    for (int i = 0; i < W * H * 4; i++) { int e = abs(keep[i] - dpx[i]); if (e > worst) worst = e; }
    char d[64];
    snprintf(d, sizeof d, "max diff %d/255", worst);
    ck(worst <= 1, "radial2-equals-radial", d);
}

/* The conic through a pmat: rotating the paint by R rotates the angle by R. */
static void conic_pmat(void)
{
    struct gfx_paint a, b;
    gfx_paint_conic(&a, 100 * 256, 100 * 256, 0); ramp_bw(&a);
    gfx_paint_conic(&b, 100 * 256, 100 * 256, 0); ramp_bw(&b);
    struct gfx_matrix m;
    gfx_m_identity(&m);
    gfx_m_translate(&m, 100 * 256, 100 * 256);
    gfx_m_rotate(&m, 90 * 256);
    gfx_m_translate(&m, -100 * 256, -100 * 256);
    b.pmat = m; b.flags |= GFX_PAINT_HAS_PMAT;
    render_direct(&b);
    int worst = 0, n = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            double dx = x + 0.5 - 100, dy = y + 0.5 - 100;
            if (hypot(dx, dy) < 5) continue;
            double ang = atan2(dy, dx) * 180.0 / M_PI;          /* clockwise on screen */
            double t = fmod(ang - 90.0 + 720.0, 360.0) / 360.0;  /* pmat turns the pattern by 90 */
            if (t < 0.01 || t > 0.99) continue;
            int want = (int)floor(t * 255.0 + 0.5), e = abs(want - dpx[(y * W + x) * 4]);
            if (e > worst) worst = e;
            n++;
        }
    char d[64];
    snprintf(d, sizeof d, "max err %d/255, %d px", worst, n);
    ck(worst <= 3 && n > 30000, "isoline-conic", d);
    (void)a;
}

/* ---------------------------------------------------------------- dither -- */
#define DW 1024
static unsigned char drow[DW * 4 * 4];
static int longest_run(const unsigned char *row, int w)
{
    int best = 1, run = 1;
    for (int x = 1; x < w; x++) {
        if (row[x * 4] == row[(x - 1) * 4]) { if (++run > best) best = run; }
        else run = 1;
    }
    return best;
}
static void dither(void)
{
    struct gfx_paint p;
    struct gfx_surface s;
    unsigned char full[DW];
    memset(full, 255, sizeof full);
    gfx_surface_init(&s, drow, DW, 4, DW * 4);
    gfx_paint_linear(&p, 0, 0, DW * 256, 0);
    gfx_paint_stop(&p, 0, 0x404040, 255);
    gfx_paint_stop(&p, GFX_MONE, 0x484848, 255);

    memset(drow, 0, sizeof drow);
    for (int y = 0; y < 4; y++) gfx_paint_row(&p, &s, y, 0, DW, full, 0);
    int plain = 0;
    for (int y = 0; y < 4; y++) { int r = longest_run(drow + y * DW * 4, DW); if (r > plain) plain = r; }
    char d[96];
    snprintf(d, sizeof d, "undithered longest run %d px", plain);
    ck(plain >= 100, "dither-plain-has-runs", d);

    p.flags |= GFX_PAINT_DITHER;
    memset(drow, 0, sizeof drow);
    for (int y = 0; y < 4; y++) gfx_paint_row(&p, &s, y, 0, DW, full, 0);
    int worst = 0;
    double sum = 0, ideal = 0;
    int chan_equal = 1;
    for (int y = 0; y < 4; y++) {
        const unsigned char *row = drow + y * DW * 4;
        int r = longest_run(row, DW);
        if (r > worst) worst = r;
        for (int x = 0; x < DW; x++) {
            sum += row[x * 4];
            ideal += 0x40 + 8.0 * (x + 0.5) / DW;
            chan_equal &= row[x * 4] == row[x * 4 + 1] && row[x * 4] == row[x * 4 + 2];
        }
    }
    snprintf(d, sizeof d, "dithered longest run %d px", worst);
    ck(worst <= 140, "dither-run", d);
    ck(worst <= 64, "dither-run-tight", d);
    snprintf(d, sizeof d, "mean %.3f ideal %.3f", sum / (4 * DW), ideal / (4 * DW));
    ck(fabs(sum - ideal) / (4 * DW) < 0.1, "dither-mean", d);
    ck(chan_equal, "dither-grey", "a grey ramp stays grey");
    int lo = 255, hi = 0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < DW; x++) {
            int v = drow[y * DW * 4 + x * 4];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    ck(lo >= 0x40 && hi <= 0x48, "dither-range", "no value outside the end stops");
    /* an exactly representable colour is not disturbed: a flat ramp stays flat */
    struct gfx_paint f;
    gfx_paint_linear(&f, 0, 0, DW * 256, 0);
    gfx_paint_stop(&f, 0, 0x336699, 255);
    gfx_paint_stop(&f, GFX_MONE, 0x336699, 255);
    f.flags |= GFX_PAINT_DITHER;
    memset(drow, 0, sizeof drow);
    for (int y = 0; y < 4; y++) gfx_paint_row(&f, &s, y, 0, DW, full, 0);
    int flat = 1;
    for (int i = 0; i < DW * 4; i++)
        flat &= drow[i * 4] == 0x33 && drow[i * 4 + 1] == 0x66 && drow[i * 4 + 2] == 0x99;
    ck(flat, "dither-flat", "a flat colour is unchanged");
}

/* ----------------------------------------------------------------- flags0 -- */
static void flags0(void)
{
    struct gfx_paint a, b;
    static unsigned char keep[W * H * 4];
    int ok = 1;
    for (int k = 0; k < 3; k++) {
        if (k == 0) { gfx_paint_linear(&a, 10 * 256, 5 * 256, 180 * 256, 160 * 256); }
        if (k == 1) { gfx_paint_radial(&a, 90 * 256, 100 * 256, 70 * 256); a.spread = GFX_SPREAD_REFLECT; }
        if (k == 2) { gfx_paint_conic(&a, 90 * 256, 100 * 256, 20 * 256); }
        gfx_paint_stop(&a, 0, 0xFF0000, 255);
        gfx_paint_stop(&a, GFX_MONE / 2, 0x00FF00, 128);
        gfx_paint_stop(&a, GFX_MONE, 0x0000FF, 255);
        b = a;
        b.pmat = (struct gfx_matrix){3, 99999, -7, 12345, 777, -4242};   /* junk, flag clear */
        render_direct(&a);
        memcpy(keep, dpx, sizeof keep);
        render_direct(&b);
        ok &= memcmp(keep, dpx, sizeof keep) == 0;
    }
    ck(ok, "flags0-ignores-pmat", "pmat is read only under GFX_PAINT_HAS_PMAT");

    /* a canvas fill under the identity CTM records the paint as given */
    struct ol_canvas canvas;
    static unsigned char first[W * H * 4];
    gfx_paint_linear(&a, 20 * 256, 0, 180 * 256, 0);
    ramp_bw(&a);
    ol_list_reset(list);
    ol_canvas_init(&canvas, list, W, H);
    ol_cmd_clear(list, 0, 0);
    ol_canvas_round_rect(&canvas, (struct gfx_rect){10, 10, 180, 100}, 0, &a);
    ol_list_close(list);
    ol_submit(device, list, surface, NULL);
    memcpy(first, front, sizeof first);
    /* the same fill recorded straight through ol_cmd_fill, no canvas */
    static int pts[512], subs[8];
    struct gfx_path path;
    gfx_path_init(&path, pts, 256, subs, 8);
    gfx_path_rect(&path, GFX_PX(10), GFX_PX(10), GFX_PX(180), GFX_PX(100));
    ol_list_reset(list);
    ol_cmd_clear(list, 0, 0);
    ol_cmd_fill(list, &path, GFX_NONZERO, &a, NULL, 4);
    ol_list_close(list);
    ol_submit(device, list, surface, NULL);
    int diff = 0;
    for (int i = 0; i < W * H * 4; i++) {
        int e = abs(first[i] - front[i]);
        if (e > diff) diff = e;
    }
    char d[64];
    snprintf(d, sizeof d, "canvas vs ol_cmd_fill max diff %d", diff);
    ck(diff == 0, "flags0-identity-ctm", d);
}

int main(void)
{
    void *dev = malloc(ol_device_size()), *ls = malloc(262144), *ss = malloc(ol_surface_size());
    struct ol_surface_desc sd = {sizeof sd, OL_FORMAT_RGBA8_STRAIGHT, W, H, W * 4, front, work,
                                 sizeof front, sizeof work};
    if (ol_device_create(dev, ol_device_size(), OL_API_VERSION, 0, &device) ||
        ol_list_create(device, ls, 262144, &list) ||
        ol_surface_create(device, ss, ol_surface_size(), &sd, &surface))
        return 2;
    isolines();
    radial2();
    conic_pmat();
    dither();
    flags0();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
