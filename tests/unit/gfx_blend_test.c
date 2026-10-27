/* gfx_compose against the double-precision W3C Compositing and Blending formulas,
 * op 0 against gfx_over, and display_path / gfx_surface agreement for every op.
 * Built with -DGFX_NCTX=2; does not link fb.c, so it defines gfx_ctx_hook itself.
 * Failures print 'FAIL <OPNAME>' (the negctl greps 'FAIL MULTIPLY'). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "openlogit_display_path.h"

int (*gfx_ctx_hook)(void);

static int bad;
static const char *const opname[GFX_OP_COUNT] = {
    "SRC_OVER", "SRC", "CLEAR", "DST_IN", "DST_OUT", "SRC_IN", "SRC_ATOP", "XOR",
    "PLUS_LIGHTER", "MULTIPLY", "SCREEN", "OVERLAY", "DARKEN", "LIGHTEN",
    "SOFT_LIGHT", "COLOR_DODGE"
};

static double bfn(int op, double cb, double cs)
{
    switch (op) {
    case GFX_OP_MULTIPLY: return cb * cs;
    case GFX_OP_SCREEN: return cb + cs - cb * cs;
    case GFX_OP_OVERLAY: return cb <= 0.5 ? cs * 2 * cb : (cs + (2 * cb - 1) - cs * (2 * cb - 1));
    case GFX_OP_DARKEN: return cb < cs ? cb : cs;
    case GFX_OP_LIGHTEN: return cb > cs ? cb : cs;
    case GFX_OP_COLOR_DODGE: return cb == 0 ? 0 : cs >= 1 ? 1 : fmin(1, cb / (1 - cs));
    case GFX_OP_SOFT_LIGHT: {
        if (cs <= 0.5) return cb - (1 - 2 * cs) * cb * (1 - cb);
        double d = cb <= 0.25 ? ((16 * cb - 12) * cb + 4) * cb : sqrt(cb);
        return cb + (2 * cs - 1) * (d - cb);
    }
    }
    return cs;
}

/* oracle: out[0..2] colour 0..255 (straight), out[3] alpha 0..255 */
static void oracle(int op, const unsigned char *dst, const int *sc, int sa8, int cov8, double *out)
{
    double ab = dst[3] / 255.0, as = sa8 / 255.0, c = cov8 / 255.0;
    double fa, fb, ar, col[3];
    int lerp = 0;
    if (op >= GFX_OP_MULTIPLY || op == GFX_OP_PLUS_LIGHTER) as *= c;
    switch (op) {
    case GFX_OP_SRC: fa = 1; fb = 0; lerp = 1; break;
    case GFX_OP_CLEAR: fa = 0; fb = 0; lerp = 1; break;
    case GFX_OP_DST_IN: fa = 0; fb = as; lerp = 1; break;
    case GFX_OP_DST_OUT: fa = 0; fb = 1 - as; lerp = 1; break;
    case GFX_OP_SRC_IN: fa = ab; fb = 0; lerp = 1; break;
    case GFX_OP_SRC_ATOP: fa = ab; fb = 1 - as; lerp = 1; break;
    case GFX_OP_XOR: fa = 1 - ab; fb = 1 - as; lerp = 1; break;
    case GFX_OP_PLUS_LIGHTER: fa = 1; fb = 1; break;
    default: fa = 1 - ab; fb = 1 - as; break;       /* blend modes + src-over */
    }
    if (op >= GFX_OP_MULTIPLY || op == GFX_OP_SRC_OVER) {
        ar = as + ab * (1 - as);
        for (int i = 0; i < 3; i++) {
            double cb = dst[i] / 255.0, cs = sc[i] / 255.0;
            double b = op == GFX_OP_SRC_OVER ? cs : bfn(op, cb, cs);
            double p = as * (1 - ab) * cs + as * ab * b + (1 - as) * ab * cb;
            col[i] = ar > 0 ? p / ar : 0;
        }
    } else {
        ar = as * fa + ab * fb;
        double arc = ar > 1 ? 1 : ar;
        for (int i = 0; i < 3; i++) {
            double p = as * fa * sc[i] / 255.0 + ab * fb * dst[i] / 255.0;
            if (p > 1) p = 1;
            col[i] = arc > 0 ? p / arc : 0;
        }
        ar = arc;
    }
    if (lerp) {
        double w0 = ab * (1 - c), w1 = ar * c, wt = w0 + w1;
        for (int i = 0; i < 3; i++)
            col[i] = wt > 0 ? (w0 * dst[i] / 255.0 + w1 * col[i]) / wt : 0;
        ar = wt;
    }
    for (int i = 0; i < 3; i++) out[i] = col[i] * 255.0;
    out[3] = ar * 255.0;
}

static void grid(void)
{
    static const unsigned char D[5][4] = {
        { 200, 100, 50, 255 }, { 10, 240, 130, 255 }, { 128, 128, 128, 128 },
        { 255, 0, 64, 64 }, { 60, 180, 220, 200 } };
    static const int S[5][3] = { { 0, 0, 0 }, { 255, 255, 255 }, { 220, 40, 90 },
                                 { 30, 200, 120 }, { 128, 64, 250 } };
    static const int A[7] = { 0, 1, 64, 128, 200, 254, 255 };
    static const int COV[3] = { 255, 100, 1 };
    for (int op = 1; op < GFX_OP_COUNT; op++) {
        double worst = 0; int combos = 0;
        for (int ci = 0; ci < 3; ci++)
        for (int di = 0; di < 5; di++) for (int si = 0; si < 5; si++) for (int ai = 0; ai < 7; ai++) {
            unsigned char d[4]; double o[4];
            memcpy(d, D[di], 4);
            oracle(op, d, S[si], A[ai], COV[ci], o);
            gfx_compose((enum gfx_op)op, d, GFX_RGB(S[si][0], S[si][1], S[si][2]), A[ai], COV[ci]);
            double e = fabs(d[3] - o[3]);
            if (e > worst) worst = e;
            if (getenv("BLEND_DEBUG") && e > 1.5) printf("  a: d=%d s=%d a=%d cov=%d got=%d want=%.2f\n", di, si, A[ai], COV[ci], d[3], o[3]);
            if (o[3] > 0.01)
                for (int i = 0; i < 3; i++) { double ec = fabs(d[i] - o[i]); if (ec > worst) worst = ec;
                    if (getenv("BLEND_DEBUG") && ec > 1.5) printf("  c%d: d=%d s=%d a=%d cov=%d got=%d want=%.2f ar=%.2f\n", i, di, si, A[ai], COV[ci], d[i], o[i], o[3]); }
            combos++;
        }
        printf("op %-12s combos=%d worst error %.3f/255\n", opname[op], combos, worst);
        if (worst > 2.0) { printf("FAIL %s\n", opname[op]); bad = 1; }
    }
}

static void op0(void)
{
    int diff = 0;
    for (int cov = 0; cov < 256; cov++) for (int a = 0; a < 256; a += 5) for (int k = 0; k < 40; k++) {
        unsigned char x[4] = { (unsigned char)(k * 37), (unsigned char)(k * 11 + 3), (unsigned char)(k * 91), (unsigned char)(k * 53) };
        unsigned char y[4]; memcpy(y, x, 4);
        gfx_over(x, 200 - k, k * 6, 77, a, cov);
        gfx_compose(GFX_OP_SRC_OVER, y, GFX_RGB(200 - k, k * 6, 77), a, cov);
        if (memcmp(x, y, 4)) diff++;
    }
    if (diff) { printf("FAIL SRC_OVER identity (%d diffs)\n", diff); bad = 1; }
    else puts("op 0 bit-identical to gfx_over");
}

/* ---------------------------------------------- display_path vs gfx_surface -- */
static void *t_alloc(unsigned long n) { return malloc(n); }
static void t_free(void *p) { free(p); }
#define FX(v) ((v) * 256)
#define W 96
#define H 64

static uint32_t init_px(int x, int y)
{
    return (uint32_t)(((x * 7 + y * 3) & 255) << 16 | ((x * 3 + y * 11 + 40) & 255) << 8 | ((x * 5 + y * 5 + 90) & 255));
}

static void display_case(int op, int grad)
{
    static int pt[512], sb[8];
    static uint32_t px[W * H];
    static unsigned char ref[W * H * 4];
    struct ol_display d; struct gfx_surface rs; struct gfx_path path; struct gfx_paint p;
    memset(&d, 0, sizeof d);
    struct ol_pixel_target t = { px, W, H, 0, 0, 0, 0, 0 };
    d.allocate = t_alloc; d.release = t_free;
    ol_display_bind(&d, &t, 16, 8, 0);
    gfx_surface_init(&rs, ref, W, H, W * 4);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint32_t c = init_px(x, y); px[y * W + x] = c;
        unsigned char *q = ref + (y * W + x) * 4;
        q[0] = c >> 16; q[1] = c >> 8; q[2] = c; q[3] = 255;
    }
    if (grad) {
        gfx_paint_linear(&p, FX(10), FX(6), FX(80), FX(50));
        gfx_paint_stop(&p, 0, 0xff3010, 255); gfx_paint_stop(&p, 65536, 0x10e0a0, 120);
    } else gfx_paint_solid(&p, 0x2060c0, 200);
    p.op = (unsigned char)op;
    gfx_path_init(&path, pt, 256, sb, 8);
    gfx_path_rrect(&path, FX(8), FX(6), FX(78), FX(48), FX(12));
    gfx_fill(&rs, &path, GFX_NONZERO, &p, 0);
    ol_display_fill_path(&d, &path, GFX_NONZERO, &p, 255, 0);
    int m = 0, changed = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint32_t c = px[y * W + x]; const unsigned char *q = ref + (y * W + x) * 4;
        int e[3] = { (int)((c >> 16) & 255) - q[0], (int)((c >> 8) & 255) - q[1], (int)(c & 255) - q[2] };
        for (int i = 0; i < 3; i++) { if (e[i] < 0) e[i] = -e[i]; if (e[i] > m) m = e[i]; }
        if (c != init_px(x, y)) changed++;
    }
    /* The display target is opaque, so the reference alpha channel is not compared. */
    if (m > 1 || (!changed && op != GFX_OP_DST_IN && op != GFX_OP_DST_OUT &&
                     op != GFX_OP_XOR)) {
        printf("FAIL %s display (%s) maxerr=%d changed=%d\n", opname[op], grad ? "linear" : "solid", m, changed);
        printf("FAIL %s\n", opname[op]);
        bad = 1;
    } else if (op == GFX_OP_MULTIPLY)
        printf("display_path MULTIPLY (%s) matches gfx_surface: maxerr=%d changed=%d\n",
               grad ? "linear" : "solid", m, changed);
    ol_display_destroy(&d);
}

int main(void)
{
    gfx_ctx_hook = 0;
    grid();
    op0();
    for (int op = 1; op < GFX_OP_COUNT; op++) { display_case(op, 0); display_case(op, 1); }
    if (!bad) puts("gfx_blend_test: ok");
    return bad;
}
