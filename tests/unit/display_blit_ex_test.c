/* ol_display_blit_rgba_ex / ol_display_blit_glyph_a. Built with -DGFX_NCTX=2;
 * does not link fb.c, so it defines gfx_ctx_hook itself. Prints "FAIL <name>"
 * per failing check and exits 1 on any failure. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openlogit_display_blit.h"
#include "display_pixels.h"

int (*gfx_ctx_hook)(void);
static int g_ctx;
static int hook(void) { return g_ctx; }
static void *t_alloc(unsigned long n) { return malloc(n); }
static void t_free(void *p) { free(p); }

static int bad;
#define CHECK(c, name) do { if (!(c)) { printf("FAIL %s\n", name); bad = 1; } } while (0)

struct dsp { struct ol_display d; struct ol_pixel_target t; uint32_t *px; };
static void dsp_init(struct dsp *s, int w, int h, uint32_t fill)
{
    memset(s, 0, sizeof *s);
    s->px = malloc((size_t)w * h * 4);
    for (long i = 0; i < (long)w * h; i++) s->px[i] = fill;
    s->t.px = s->px; s->t.w = w; s->t.h = h;
    s->d.allocate = t_alloc; s->d.release = t_free;
    ol_display_bind(&s->d, &s->t, 16, 8, 0);
}
static void dsp_free(struct dsp *s) { ol_display_destroy(&s->d); free(s->px); }
static int chan(uint32_t p, int sh) { return (p >> sh) & 255; }
static int maxdiff_rgb(uint32_t a, uint32_t b)
{
    int m = 0;
    for (int sh = 0; sh <= 16; sh += 8) { int d = abs(chan(a, sh) - chan(b, sh)); if (d > m) m = d; }
    return m;
}
static uint32_t rnd_s = 12345;
static uint32_t rnd(void) { rnd_s = rnd_s * 1664525u + 1013904223u; return rnd_s >> 8; }

/* ---- box downscale vs double area-average reference ---- */
static void test_box_downscale(void)
{
    enum { S = 1536, D = 64, K = S / D };
    uint8_t *src = malloc((size_t)S * S * 4);
    double *lum = malloc(sizeof(double) * S * S);
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        double cx = x - S / 2.0, cy = y - S / 2.0;
        double v = 0.5 + 0.5 * cos(M_PI * (cx * cx + cy * cy) / (double)S * 0.5);
        int g = (int)(v * 255.0 + 0.5);
        uint8_t *p = src + ((size_t)y * S + x) * 4;
        p[0] = p[1] = p[2] = (uint8_t)g; p[3] = 255; lum[(size_t)y * S + x] = g;
    }
    struct dsp s; dsp_init(&s, D, D, 0);
    int rc = ol_display_blit_rgba_ex(&s.d, src, S, S, S * 4, 0, 0, S, S, 0, 0, D * 256, D * 256, 255, OL_FILTER_BOX);
    CHECK(rc == OL_OK, "box-rc");
    /* double area average: rows then columns (separable), then compare */
    double maxerr = 0, e_out = 0, e_ref = 0;
    static double ref[D][D];
    for (int j = 0; j < D; j++) for (int i = 0; i < D; i++) {
        double sum = 0;
        for (int y = 0; y < K; y++) for (int x = 0; x < K; x++) sum += lum[(size_t)(j * K + y) * S + i * K + x];
        ref[j][i] = sum / (K * K);
        double e = fabs(chan(s.px[j * D + i], 0) - ref[j][i]);
        if (e > maxerr) maxerr = e;
    }
    for (int j = 0; j < D; j++) for (int i = 1; i < D; i++) {
        e_out += fabs((double)chan(s.px[j * D + i], 0) - chan(s.px[j * D + i - 1], 0));
        e_ref += fabs(ref[j][i] - ref[j][i - 1]);
    }
    printf("box-downscale max err %.3f/255, hf energy out %.1f ref %.1f\n", maxerr, e_out, e_ref);
    CHECK(maxerr <= 2.0, "box-downscale");
    CHECK(e_out <= e_ref * 1.10 + 8.0, "box-hf-energy");
    dsp_free(&s);

    /* non-integer ratio 1000 -> 37 wide, 600 -> 23 tall with fractional box weights */
    enum { W2 = 1000, H2 = 600, DW = 37, DH = 23 };
    uint8_t *s2 = malloc((size_t)W2 * H2 * 4);
    for (long i = 0; i < (long)W2 * H2; i++) {
        s2[i * 4] = (uint8_t)rnd(); s2[i * 4 + 1] = (uint8_t)rnd(); s2[i * 4 + 2] = (uint8_t)rnd(); s2[i * 4 + 3] = 255;
    }
    dsp_init(&s, DW, DH, 0);
    ol_display_blit_rgba_ex(&s.d, s2, W2, H2, W2 * 4, 0, 0, W2, H2, 0, 0, DW * 256, DH * 256, 255, OL_FILTER_BOX);
    double me = 0;
    for (int j = 0; j < DH; j++) for (int i = 0; i < DW; i++) {
        double x0 = (double)i * W2 / DW, x1 = (double)(i + 1) * W2 / DW;
        double y0 = (double)j * H2 / DH, y1 = (double)(j + 1) * H2 / DH;
        for (int c = 0; c < 3; c++) {
            double sum = 0, wsum = 0;
            for (int y = (int)y0; y < (int)ceil(y1); y++) for (int x = (int)x0; x < (int)ceil(x1); x++) {
                double wx = fmin(x + 1, x1) - fmax(x, x0), wy = fmin(y + 1, y1) - fmax(y, y0);
                sum += wx * wy * s2[((size_t)y * W2 + x) * 4 + c]; wsum += wx * wy;
            }
            double e = fabs(chan(s.px[j * DW + i], 16 - 8 * c) - sum / wsum);
            if (e > me) me = e;
        }
    }
    printf("box-fractional max err %.3f/255\n", me);
    CHECK(me <= 1.5, "box-fractional");
    dsp_free(&s); free(s2); free(src); free(lum);
}

/* ---- bilinear with alpha edges: no dark fringe ---- */
static void test_fringe(void)
{
    enum { N = 16 };
    uint8_t src[N * N * 4];
    memset(src, 0, sizeof src);                       /* transparent BLACK border */
    for (int y = 4; y < 12; y++) for (int x = 4; x < 12; x++) {
        uint8_t *p = src + (y * N + x) * 4; p[0] = p[1] = p[2] = 255; p[3] = 255;
    }
    struct dsp s; dsp_init(&s, 64, 64, 0xFFFFFF);     /* white backdrop */
    ol_display_blit_rgba_ex(&s.d, src, N, N, N * 4, 0, 0, N, N, 0, 0, 64 * 256, 64 * 256, 255, OL_FILTER_BILINEAR);
    int worst = 0;
    for (int i = 0; i < 64 * 64; i++) { int d = 255 - (int)(s.px[i] & 255); if (d > worst) worst = d; }
    for (int i = 0; i < 64 * 64; i++) { int d = maxdiff_rgb(s.px[i], 0xFFFFFF); if (d > worst) worst = d; }
    printf("bilinear fringe worst %d/255\n", worst);
    CHECK(worst <= 1, "bilinear-fringe");
    /* the same sprite on a blue backdrop keeps its centre white and the corner blue */
    for (int i = 0; i < 64 * 64; i++) s.px[i] = 0x0000FF;
    ol_display_blit_rgba_ex(&s.d, src, N, N, N * 4, 0, 0, N, N, 0, 0, 64 * 256, 64 * 256, 255, OL_FILTER_BILINEAR);
    CHECK(s.px[32 * 64 + 32] == 0xFFFFFF && s.px[0] == 0x0000FF, "bilinear-keeps-backdrop");
    dsp_free(&s);
}

/* ---- bilinear against a double premultiplied reference, random alpha ---- */
static void test_bilinear_ref(void)
{
    enum { N = 11, M = 29 };
    uint8_t src[N * N * 4];
    for (int i = 0; i < N * N * 4; i++) src[i] = (uint8_t)rnd();
    struct dsp s; dsp_init(&s, M, M, 0x4080C0);
    ol_display_blit_rgba_ex(&s.d, src, N, N, N * 4, 0, 0, N, N, 0, 0, M * 256, M * 256, 255, OL_FILTER_BILINEAR);
    double worst = 0;
    for (int j = 0; j < M; j++) for (int i = 0; i < M; i++) {
        double u = (i + 0.5) * N / M - 0.5, v = (j + 0.5) * N / M - 0.5;
        if (u < 0) u = 0;
        if (v < 0) v = 0;
        if (u > N - 1) u = N - 1;
        if (v > N - 1) v = N - 1;
        int x0 = (int)u, y0 = (int)v, x1 = x0 + 1 < N ? x0 + 1 : x0, y1 = y0 + 1 < N ? y0 + 1 : y0;
        double fx = u - x0, fy = v - y0, pm[3] = {0, 0, 0}, al = 0;
        for (int q = 0; q < 4; q++) {
            int xx = (q & 1) ? x1 : x0, yy = (q & 2) ? y1 : y0;
            double w = ((q & 1) ? fx : 1 - fx) * ((q & 2) ? fy : 1 - fy);
            const uint8_t *p = src + (yy * N + xx) * 4;
            for (int c = 0; c < 3; c++) pm[c] += w * p[c] * p[3] / 255.0;
            al += w * p[3] / 255.0;
        }
        int bg[3] = {0x40, 0x80, 0xC0};
        for (int c = 0; c < 3; c++) {
            double ref = pm[c] + bg[c] * (1 - al);
            double e = fabs(chan(s.px[j * M + i], 16 - 8 * c) - ref);
            if (e > worst) worst = e;
        }
    }
    printf("bilinear-ref max err %.3f/255\n", worst);
    CHECK(worst <= 1.5, "bilinear-ref");
    dsp_free(&s);
}

/* ---- opacity ---- */
static void test_opacity(void)
{
    enum { N = 8 };
    uint8_t src[N * N * 4];
    for (int i = 0; i < N * N; i++) {
        src[i * 4] = (uint8_t)(i * 3 + 20); src[i * 4 + 1] = (uint8_t)(200 - i); src[i * 4 + 2] = (uint8_t)(i * 7);
        src[i * 4 + 3] = (uint8_t)(i % 2 ? 255 : 100);
    }
    struct dsp s; dsp_init(&s, N, N, 0x406080);
    ol_display_blit_rgba_ex(&s.d, src, N, N, N * 4, 0, 0, N, N, 0, 0, N * 256, N * 256, 128, OL_FILTER_NEAREST);
    int worst = 0;
    for (int i = 0; i < N * N; i++) {
        double a = src[i * 4 + 3] / 255.0 * 128.0 / 255.0;
        int bg[3] = {0x40, 0x60 - 0x20 + 0x20 - 0x00, 0x80};
        bg[1] = 0x60 - 0x00; bg[2] = 0x80; bg[0] = 0x40; bg[1] = 0x60; (void)bg;
        int d[3] = {0x40, 0x60, 0x80};
        for (int c = 0; c < 3; c++) {
            double ref = src[i * 4 + c] * a + d[c] * (1 - a);
            int got = chan(s.px[i], 16 - 8 * c);
            int e = (int)fabs(got - ref + (got > ref ? -0.0 : 0.0));
            if (fabs(got - ref) > worst) worst = (int)ceil(fabs(got - ref) - 1e-9); (void)e;
        }
    }
    printf("opacity128 vs half src-over worst %d\n", worst);
    CHECK(worst <= 1, "opacity-half");
    CHECK(ol_display_blit_rgba_ex(&s.d, src, N, N, N * 4, 0, 0, N, N, 0, 0, N * 256, N * 256, 0, OL_FILTER_NEAREST) == OL_OK, "opacity-zero-rc");
    dsp_free(&s);
}

/* ---- stride padding and source rect ---- */
static void test_stride(void)
{
    enum { W = 10, H = 7, PAD = 24 };
    uint8_t tight[W * H * 4], padded[(W * 4 + PAD) * H];
    memset(padded, 0xEE, sizeof padded);
    for (int y = 0; y < H; y++) for (int x = 0; x < W * 4; x++) {
        uint8_t v = (uint8_t)rnd(); if (x % 4 == 3) v = (uint8_t)(128 + (v >> 1));
        tight[y * W * 4 + x] = v; padded[y * (W * 4 + PAD) + x] = v;
    }
    for (int f = 0; f < 3; f++) {
        struct dsp a, b; dsp_init(&a, 40, 30, 0x123456); dsp_init(&b, 40, 30, 0x123456);
        ol_display_blit_rgba_ex(&a.d, tight, W, H, W * 4, 2, 1, 7, 5, 3 * 256 + 77, 2 * 256, 23 * 256 + 5, 17 * 256, 200, (enum ol_filter)f);
        ol_display_blit_rgba_ex(&b.d, padded, W, H, W * 4 + PAD, 2, 1, 7, 5, 3 * 256 + 77, 2 * 256, 23 * 256 + 5, 17 * 256, 200, (enum ol_filter)f);
        CHECK(memcmp(a.px, b.px, 40 * 30 * 4) == 0, "stride-padding");
        /* source rect: result depends only on the rect, not on pixels outside it */
        uint8_t other[W * H * 4]; memcpy(other, tight, sizeof other);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
            if (x < 2 || x >= 9 || y < 1 || y >= 6) { other[(y * W + x) * 4] ^= 0xFF; other[(y * W + x) * 4 + 3] = 255; }
        struct dsp c; dsp_init(&c, 40, 30, 0x123456);
        ol_display_blit_rgba_ex(&c.d, other, W, H, W * 4, 2, 1, 7, 5, 3 * 256 + 77, 2 * 256, 23 * 256 + 5, 17 * 256, 200, (enum ol_filter)f);
        CHECK(memcmp(a.px, c.px, 40 * 30 * 4) == 0, "src-rect-isolated");
        dsp_free(&a); dsp_free(&b); dsp_free(&c);
    }
    struct dsp s; dsp_init(&s, 8, 8, 0);
    CHECK(ol_display_blit_rgba_ex(&s.d, tight, W, H, W * 4 - 1, 0, 0, W, H, 0, 0, 256, 256, 255, OL_FILTER_NEAREST) == OL_ARGUMENT, "stride-too-small");
    CHECK(ol_display_blit_rgba_ex(&s.d, tight, W, H, W * 4, 5, 0, 6, H, 0, 0, 256, 256, 255, OL_FILTER_NEAREST) == OL_ARGUMENT, "rect-outside");
    dsp_free(&s);
}

/* ---- fractional destination edges at 125% / 150% ---- */
static int near(int a, int b, int tol) { return abs(a - b) <= tol; }
static void test_frac(void)
{
    uint8_t white[4 * 4 * 4]; memset(white, 255, sizeof white);
    for (int f = 0; f < 3; f++) {
        /* 125%: x = 3pt, w = 7pt -> 3.75 .. 12.5 px; y the same */
        struct dsp s; dsp_init(&s, 20, 20, 0);
        ol_display_blit_rgba_ex(&s.d, white, 4, 4, 16, 0, 0, 4, 4, 3 * 320, 3 * 320, 7 * 320, 7 * 320, 255, (enum ol_filter)f);
        int ok = near(chan(s.px[8 * 20 + 3], 0), 64, 1) && near(chan(s.px[8 * 20 + 4], 0), 255, 1) &&
                 near(chan(s.px[8 * 20 + 11], 0), 255, 1) && near(chan(s.px[8 * 20 + 12], 0), 128, 1) &&
                 chan(s.px[8 * 20 + 2], 0) == 0 && chan(s.px[8 * 20 + 13], 0) == 0 &&
                 near(chan(s.px[3 * 20 + 8], 0), 64, 1) && near(chan(s.px[12 * 20 + 8], 0), 128, 1) &&
                 near(chan(s.px[3 * 20 + 3], 0), 16, 1) &&            /* corner = 0.25*0.25 */
                 near(chan(s.px[12 * 20 + 12], 0), 64, 1);            /* 0.5*0.5 */
        CHECK(ok, "frac-125");
        dsp_free(&s);
        /* 150%: x = 1pt, w = 5pt -> 1.5 .. 9.0 px */
        dsp_init(&s, 20, 20, 0);
        ol_display_blit_rgba_ex(&s.d, white, 4, 4, 16, 0, 0, 4, 4, 1 * 384, 1 * 384, 5 * 384, 5 * 384, 255, (enum ol_filter)f);
        ok = near(chan(s.px[5 * 20 + 1], 0), 128, 1) && near(chan(s.px[5 * 20 + 2], 0), 255, 1) &&
             near(chan(s.px[5 * 20 + 8], 0), 255, 1) && chan(s.px[5 * 20 + 9], 0) == 0 &&
             near(chan(s.px[1 * 20 + 1], 0), 64, 1) && chan(s.px[0 * 20 + 5], 0) == 0;
        CHECK(ok, "frac-150");
        dsp_free(&s);
    }
}

/* ---- glyph_a ---- */
static void test_glyph(void)
{
    enum { W = 13, H = 9 };
    uint8_t cov[W * H]; for (int i = 0; i < W * H; i++) cov[i] = (uint8_t)(i % 7 == 0 ? 0 : i % 11 == 0 ? 255 : rnd());
    struct dsp a, b; dsp_init(&a, 30, 20, 0x335577); dsp_init(&b, 30, 20, 0x335577);
    ol_display_blit_glyph(&a.d, 4, 3, cov, W, H, 0xE0A040);
    ol_display_blit_glyph_a(&b.d, 4, 3, cov, W, H, W, 0xE0A040, 255);
    CHECK(memcmp(a.px, b.px, 30 * 20 * 4) == 0, "glyph-a255-bitident");
    /* clipped and partly off-target as well */
    a.d.target.clip_on = b.d.target.clip_on = 1; a.d.target.clx0 = b.d.target.clx0 = 6; a.d.target.cly0 = b.d.target.cly0 = 5; a.d.target.clx1 = b.d.target.clx1 = 14; a.d.target.cly1 = b.d.target.cly1 = 10;
    ol_display_blit_glyph(&a.d, -3, 2, cov, W, H, 0x102030);
    ol_display_blit_glyph_a(&b.d, -3, 2, cov, W, H, W, 0x102030, 255);
    CHECK(memcmp(a.px, b.px, 30 * 20 * 4) == 0, "glyph-a255-clip");
    dsp_free(&a); dsp_free(&b);
    /* alpha 128 is half; stride is honoured */
    uint8_t padded[(W + 5) * H]; memset(padded, 0x77, sizeof padded);
    for (int y = 0; y < H; y++) memcpy(padded + y * (W + 5), cov + y * W, W);
    dsp_init(&a, 30, 20, 0x000000); dsp_init(&b, 30, 20, 0x000000);
    ol_display_blit_glyph_a(&a.d, 2, 2, cov, W, H, W, 0xFFFFFF, 128);
    ol_display_blit_glyph_a(&b.d, 2, 2, padded, W, H, W + 5, 0xFFFFFF, 128);
    CHECK(memcmp(a.px, b.px, 30 * 20 * 4) == 0, "glyph-stride");
    int worst = 0;
    for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
        double ref = cov[j * W + i] * 128.0 / 255.0;
        double got = chan(a.px[(2 + j) * 30 + 2 + i], 0);
        if (fabs(got - ref) > worst) worst = (int)ceil(fabs(got - ref));
    }
    printf("glyph alpha128 worst %d\n", worst);
    CHECK(worst <= 1, "glyph-half");
    /* outside the clip nothing changes */
    a.d.target.clip_on = 1; a.d.target.clx0 = 0; a.d.target.cly0 = 0; a.d.target.clx1 = 4; a.d.target.cly1 = 4;
    uint8_t full[W * H]; memset(full, 255, sizeof full);
    uint32_t *copy = malloc(30 * 20 * 4); memcpy(copy, a.px, 30 * 20 * 4);
    ol_display_blit_glyph_a(&a.d, 8, 8, full, W, H, W, 0xFFFFFF, 200);
    CHECK(memcmp(copy, a.px, 30 * 20 * 4) == 0, "glyph-clip");
    a.d.target.clx1 = 10; a.d.target.cly1 = 10;
    ol_display_blit_glyph_a(&a.d, 2, 2, full, W, H, W, 0xFFFFFF, 200);
    int in = 1, out = 1;
    for (int y = 0; y < 20; y++) for (int x = 0; x < 30; x++) {
        int inside = x >= 2 && x < 4 + 6 && y >= 2 && y < 4 + 6 && x < 10 && y < 10;
        if (inside && a.px[y * 30 + x] == copy[y * 30 + x]) in = 0;
        if (!inside && a.px[y * 30 + x] != copy[y * 30 + x]) out = 0;
    }
    CHECK(in && out, "glyph-clip-inside");
    free(copy);
    dsp_free(&a); dsp_free(&b);
}

/* ---- clip honoured by the rgba blit ---- */
static void test_clip(void)
{
    uint8_t white[4 * 4 * 4]; memset(white, 255, sizeof white);
    struct dsp s; dsp_init(&s, 20, 20, 0);
    s.d.target.clip_on = 1; s.d.target.clx0 = 5; s.d.target.cly0 = 5; s.d.target.clx1 = 10; s.d.target.cly1 = 10;
    ol_display_blit_rgba_ex(&s.d, white, 4, 4, 16, 0, 0, 4, 4, 2 * 256, 2 * 256, 14 * 256 + 100, 14 * 256, 255, OL_FILTER_BILINEAR);
    int ok = 1;
    for (int y = 0; y < 20; y++) for (int x = 0; x < 20; x++) {
        int in = x >= 5 && x < 10 && y >= 5 && y < 10;
        if ((s.px[y * 20 + x] != 0) != in) ok = 0;
    }
    CHECK(ok, "clip-rgba");
    dsp_free(&s);
}

/* ---- two contexts interleaved (GFX_NCTX=2), scratch is per display ---- */
static void test_interleave(void)
{
    enum { N = 64 };
    uint8_t *sa = malloc(N * N * 4), *sb = malloc(N * N * 4);
    for (int i = 0; i < N * N * 4; i++) { sa[i] = (uint8_t)rnd(); sb[i] = (uint8_t)rnd(); }
    struct dsp a, b, ra, rb; dsp_init(&a, 50, 50, 0x111111); dsp_init(&b, 50, 50, 0x222222);
    dsp_init(&ra, 50, 50, 0x111111); dsp_init(&rb, 50, 50, 0x222222);
    gfx_ctx_hook = hook;
    g_ctx = 0; ol_display_blit_rgba_ex(&ra.d, sa, N, N, N * 4, 0, 0, N, N, 0, 0, 50 * 256, 50 * 256, 255, OL_FILTER_BOX);
    g_ctx = 1; ol_display_blit_rgba_ex(&rb.d, sb, N, N, N * 4, 0, 0, N, N, 0, 0, 20 * 256, 20 * 256, 255, OL_FILTER_BOX);
    /* interleave: bigger on B first, then A, then A and B again */
    g_ctx = 1; ol_display_blit_rgba_ex(&b.d, sb, N, N, N * 4, 0, 0, N, N, 0, 0, 20 * 256, 20 * 256, 255, OL_FILTER_BOX);
    g_ctx = 0; ol_display_blit_rgba_ex(&a.d, sa, N, N, N * 4, 0, 0, N, N, 0, 0, 50 * 256, 50 * 256, 255, OL_FILTER_BOX);
    CHECK(a.d.scratch[OL_DS_BLIT] && b.d.scratch[OL_DS_BLIT] && a.d.scratch[OL_DS_BLIT] != b.d.scratch[OL_DS_BLIT], "interleave-scratch");
    CHECK(memcmp(a.px, ra.px, 50 * 50 * 4) == 0 && memcmp(b.px, rb.px, 50 * 50 * 4) == 0, "interleave");
    dsp_free(&a); dsp_free(&b); dsp_free(&ra); dsp_free(&rb); free(sa); free(sb);
}

int main(void)
{
    gfx_ctx_hook = hook;
    test_box_downscale();
    test_fringe();
    test_bilinear_ref();
    test_opacity();
    test_stride();
    test_frac();
    test_glyph();
    test_clip();
    test_interleave();
    if (bad) { printf("display_blit_ex_test FAILED\n"); return 1; }
    printf("display_blit_ex_test ok\n");
    return 0;
}
