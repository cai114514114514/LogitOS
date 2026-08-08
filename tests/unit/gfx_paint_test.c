/* Open Logit -- paints and Porter-Duff src-over, checked against arithmetic.
 *
 * Coverage is checked against a supersampled reference (gfx_raster_test.c);
 * compositing cannot be, because there is nothing to sample -- src-over is an
 * exact formula and the only honest reference is the formula itself, evaluated
 * independently in double precision. So that is what this file does: recompute
 * every blend, every gradient parameter and every image sample in floating
 * point from the definition, and require the integer engine to agree within one
 * quantisation step.
 *
 * Build: make test-gfx
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "gfx.h"

static int fails, checks;

static void ck(int cond, const char *what, const char *detail)
{
    checks++;
    printf("%-4s %s%s%s\n", cond ? "ok" : "FAIL", what,
           detail && *detail ? "  " : "", detail ? detail : "");
    if (!cond) fails++;
}

/* The definition, in double, owing nothing to gfx_over(). */
static void ref_over(double *dr, double *dg, double *db, double *da,
                     double sr, double sg, double sb, double sa)
{
    double oa = sa + *da * (1 - sa);
    if (oa <= 0) { *dr = *dg = *db = *da = 0; return; }
    *dr = (sr * sa + *dr * *da * (1 - sa)) / oa;
    *dg = (sg * sa + *dg * *da * (1 - sa)) / oa;
    *db = (sb * sa + *db * *da * (1 - sa)) / oa;
    *da = oa;
}

static int PT[2048 * 2];
static int SUB[32];
#define W 64
#define H 64
static unsigned char SURF[W * H * 4];
static unsigned char IMG[8 * 8 * 4];
static unsigned char STRIP[256 * 4];

static void surf_reset(struct gfx_surface *s)
{
    gfx_surface_init(s, SURF, W, H, W * 4);
    gfx_surface_clear(s);
}

static void fill_rect(struct gfx_surface *s, const struct gfx_paint *pt,
                      int x, int y, int w, int h)
{
    struct gfx_path p;
    gfx_path_init(&p, PT, 2048, SUB, 32);
    gfx_path_rect(&p, GFX_PX(x), GFX_PX(y), GFX_PX(w), GFX_PX(h));
    gfx_fill(s, &p, GFX_NONZERO, pt, 0);
}

static const unsigned char *px(int x, int y) { return SURF + ((long)y * W + x) * 4; }

int main(void)
{
    char d[192];
    struct gfx_surface s;
    struct gfx_paint pt;

    printf("=== Open Logit: paint + src-over ===\n");

    /* -------------------------------------------------------- src-over -- */
    printf("\n-- Porter-Duff src-over, against the formula in double --\n");
    {
        int worst = 0;
        int sa_v[] = { 0, 1, 37, 128, 200, 254, 255 };
        int da_v[] = { 0, 1, 60, 128, 255 };
        int cv_v[] = { 0, 1, 64, 128, 255 };
        for (unsigned i = 0; i < sizeof sa_v / sizeof *sa_v; i++)
        for (unsigned j = 0; j < sizeof da_v / sizeof *da_v; j++)
        for (unsigned k = 0; k < sizeof cv_v / sizeof *cv_v; k++) {
            unsigned char dst[4] = { 200, 100, 40, (unsigned char)da_v[j] };
            int sr = 20, sg = 220, sb = 130;
            gfx_over(dst, sr, sg, sb, sa_v[i], cv_v[k]);
            double rr = 200 / 255.0, rg = 100 / 255.0, rb = 40 / 255.0, ra = da_v[j] / 255.0;
            /* coverage modulates the SOURCE alpha, which is what makes an
             * antialiased edge composite the same as a partly transparent
             * fill -- the property the whole engine leans on. */
            double sa = (double)((sa_v[i] * cv_v[k] + 127) / 255) / 255.0;
            ref_over(&rr, &rg, &rb, &ra, sr / 255.0, sg / 255.0, sb / 255.0, sa);
            int e0 = abs(dst[3] - (int)(ra * 255 + 0.5));
            if (e0 > worst) worst = e0;
            if (ra > 0.02) {           /* colour is meaningless at ~zero alpha */
                int e1 = abs(dst[0] - (int)(rr * 255 + 0.5));
                int e2 = abs(dst[1] - (int)(rg * 255 + 0.5));
                int e3 = abs(dst[2] - (int)(rb * 255 + 0.5));
                if (e1 > worst) worst = e1;
                if (e2 > worst) worst = e2;
                if (e3 > worst) worst = e3;
            }
        }
        snprintf(d, sizeof d, "175 combinations, worst channel error %d/255", worst);
        ck(worst <= 2, "src-over matches its definition", d);

        /* The two shortcuts have to be the general case, not near it. */
        unsigned char q[4] = { 10, 20, 30, 255 };
        gfx_over(q, 90, 91, 92, 255, 255);
        ck(q[0] == 90 && q[1] == 91 && q[2] == 92 && q[3] == 255,
           "an opaque source replaces outright", "");
        unsigned char r[4] = { 0, 0, 0, 0 };
        gfx_over(r, 90, 91, 92, 128, 255);
        ck(r[0] == 90 && r[3] == 128, "over an empty destination, the source stands", "");
        unsigned char t[4] = { 1, 2, 3, 4 };
        gfx_over(t, 90, 91, 92, 255, 0);
        ck(t[0] == 1 && t[3] == 4, "zero coverage touches nothing", "");
    }

    /* -------------------------------------------------- solid + alpha -- */
    printf("\n-- solid paint --\n");
    {
        surf_reset(&s);
        gfx_paint_solid(&pt, 0x3366CC, 255);
        fill_rect(&s, &pt, 8, 8, 20, 20);
        ck(px(10, 10)[0] == 0x33 && px(10, 10)[1] == 0x66 && px(10, 10)[2] == 0xCC &&
           px(10, 10)[3] == 255, "an opaque solid fill is exact", "");
        ck(px(7, 10)[3] == 0 && px(28, 10)[3] == 0, "and stops at its edges", "");

        /* Two 50% blacks over an opaque white: 1 - 0.5^2 = 75% ink. */
        surf_reset(&s);
        gfx_paint_solid(&pt, 0xFFFFFF, 255);
        fill_rect(&s, &pt, 0, 0, W, H);
        gfx_paint_solid(&pt, 0x000000, 128);
        fill_rect(&s, &pt, 4, 4, 20, 20);
        int one = px(10, 10)[0];
        fill_rect(&s, &pt, 4, 4, 20, 20);
        int two = px(10, 10)[0];
        snprintf(d, sizeof d, "one layer %d (want 127), two %d (want 63)", one, two);
        ck(abs(one - 127) <= 2 && abs(two - 63) <= 3,
           "translucent layers compose the way alpha says they do", d);
    }

    /* ------------------------------------------------ linear gradient -- */
    printf("\n-- linear gradient --\n");
    {
        surf_reset(&s);
        gfx_paint_linear(&pt, GFX_PX(8), 0, GFX_PX(56), 0);
        gfx_paint_stop(&pt, 0, 0x000000, 255);
        gfx_paint_stop(&pt, GFX_MONE, 0xFFFFFF, 255);
        fill_rect(&s, &pt, 0, 0, W, 20);
        int worst = 0;
        for (int x = 8; x < 56; x++) {
            double t = (x + 0.5 - 8.0) / 48.0;
            int want = (int)(t * 255);
            int got = px(x, 5)[0];
            if (abs(got - want) > worst) worst = abs(got - want);
        }
        snprintf(d, sizeof d, "worst channel error %d/255 across 48 px", worst);
        ck(worst <= 2, "the ramp is linear in the axis parameter", d);
        ck(px(2, 5)[0] == 0 && px(60, 5)[0] == 255,
           "and it PADS outside the axis rather than repeating", "");
        ck(px(20, 3)[0] == px(20, 15)[0],
           "colour is constant perpendicular to the axis", "");

        /* A diagonal axis: the parameter is a projection, not an x ramp. */
        surf_reset(&s);
        gfx_paint_linear(&pt, 0, 0, GFX_PX(40), GFX_PX(40));
        gfx_paint_stop(&pt, 0, 0x000000, 255);
        gfx_paint_stop(&pt, GFX_MONE, 0xFFFFFF, 255);
        fill_rect(&s, &pt, 0, 0, W, H);
        worst = 0;
        for (int k = 0; k < 30; k++) {
            int a = px(k, 30 - k)[0], b = px(0, 30)[0];
            if (abs(a - b) > worst) worst = abs(a - b);
        }
        snprintf(d, sizeof d, "worst deviation along the isoline %d/255", worst);
        ck(worst <= 3, "a diagonal gradient is constant along its perpendicular", d);

        /* Three stops: the middle one has to actually be hit. */
        gfx_paint_linear(&pt, 0, 0, GFX_PX(60), 0);
        gfx_paint_stop(&pt, 0, 0xFF0000, 255);
        gfx_paint_stop(&pt, GFX_MONE / 2, 0x00FF00, 255);
        gfx_paint_stop(&pt, GFX_MONE, 0x0000FF, 255);
        int a2;
        unsigned mid = gfx_paint_sample(&pt, GFX_MONE / 2, &a2);
        unsigned q1 = gfx_paint_sample(&pt, GFX_MONE / 4, &a2);
        snprintf(d, sizeof d, "mid %06X, quarter %06X", mid, q1);
        ck(mid == 0x00FF00 && GFX_R(q1) > 100 && GFX_R(q1) < 155 &&
           GFX_G(q1) > 100 && GFX_G(q1) < 155 && GFX_B(q1) == 0,
           "a three-stop ramp interpolates between the right pair", d);

        /* Stops added out of order are sorted, not honoured in call order. */
        gfx_paint_linear(&pt, 0, 0, GFX_PX(10), 0);
        gfx_paint_stop(&pt, GFX_MONE, 0x0000FF, 255);
        gfx_paint_stop(&pt, 0, 0xFF0000, 255);
        ck(gfx_paint_sample(&pt, 0, &a2) == 0xFF0000,
           "out-of-order stops are sorted", "");

        /* Alpha interpolates too -- a fade-out gradient is a real thing. */
        surf_reset(&s);
        gfx_paint_linear(&pt, 0, 0, GFX_PX(40), 0);
        gfx_paint_stop(&pt, 0, 0x000000, 255);
        gfx_paint_stop(&pt, GFX_MONE, 0x000000, 0);
        fill_rect(&s, &pt, 0, 0, 40, 10);
        snprintf(d, sizeof d, "a(0)=%d a(20)=%d a(39)=%d",
                 px(0, 5)[3], px(20, 5)[3], px(39, 5)[3]);
        ck(px(0, 5)[3] > 250 && abs(px(20, 5)[3] - 128) < 12 && px(39, 5)[3] < 12,
           "gradient alpha ramps as well as colour", d);
    }

    /* ------------------------------------------------ radial gradient -- */
    printf("\n-- radial gradient --\n");
    {
        surf_reset(&s);
        /* The centre is put on a PIXEL CENTRE (32.5), not a pixel corner.
         * At 32.0 the samples at 32+12 and 32-12 sit 12.5 and 11.5 away and a
         * symmetry check fails against a rasterizer that is perfectly right --
         * a reference has to be told where the pixel grid is. */
        gfx_paint_radial(&pt, GFX_PX(32) + 128, GFX_PX(32) + 128, GFX_PX(24));
        gfx_paint_stop(&pt, 0, 0xFFFFFF, 255);
        gfx_paint_stop(&pt, GFX_MONE, 0x000000, 255);
        fill_rect(&s, &pt, 0, 0, W, H);
        int worst = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                double dx = x + 0.5 - 32.5, dy = y + 0.5 - 32.5;
                double t = sqrt(dx * dx + dy * dy) / 24.0;
                if (t > 1) t = 1;
                int want = (int)((1 - t) * 255);
                int e = abs(px(x, y)[0] - want);
                if (e > worst) worst = e;
            }
        snprintf(d, sizeof d, "worst channel error %d/255 over 4096 px", worst);
        ck(worst <= 3, "radial t is the true distance / radius", d);
        /* Concentric, not elliptical: four points at the same radius agree. */
        int p0 = px(32 + 12, 32)[0], p1 = px(32, 32 + 12)[0];
        int p2 = px(32 - 12, 32)[0], p3 = px(32, 32 - 12)[0];
        snprintf(d, sizeof d, "%d %d %d %d", p0, p1, p2, p3);
        ck(abs(p0 - p1) <= 2 && abs(p1 - p2) <= 2 && abs(p2 - p3) <= 2,
           "the rings are circular", d);
    }

    /* ------------------------------------------------------ image paint -- */
    printf("\n-- image paint --\n");
    {
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                unsigned char *q = IMG + (y * 8 + x) * 4;
                q[0] = (unsigned char)(x * 32); q[1] = (unsigned char)(y * 32);
                q[2] = 0; q[3] = 255;
            }
        struct gfx_matrix m;
        gfx_m_identity(&m);
        gfx_m_translate(&m, GFX_PX(10), GFX_PX(10));
        surf_reset(&s);
        ck(gfx_paint_image(&pt, IMG, 8, 8, 0, &m, 0), "an image paint sets up", "");
        fill_rect(&s, &pt, 10, 10, 8, 8);
        int okk = 1;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                const unsigned char *q = px(10 + x, 10 + y);
                if (q[0] != x * 32 || q[1] != y * 32 || q[3] != 255) okk = 0;
            }
        ck(okk, "at 1:1 an image is copied pixel for pixel", "");

        /* Outside the image is TRANSPARENT, not the clamped edge smeared. */
        surf_reset(&s);
        fill_rect(&s, &pt, 4, 4, 24, 24);
        snprintf(d, sizeof d, "outside alpha %d", px(5, 5)[3]);
        ck(px(5, 5)[3] == 0 && px(20, 20)[3] == 0, "outside the image is empty", d);

        /* 4x nearest: whole 4x4 blocks of one source texel. */
        gfx_m_identity(&m);
        gfx_m_scale(&m, 4 * GFX_MONE, 4 * GFX_MONE);
        surf_reset(&s);
        gfx_paint_image(&pt, IMG, 8, 8, 0, &m, 0);
        fill_rect(&s, &pt, 0, 0, 32, 32);
        int blocky = 1;
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                if (px(4 + x, 4 + y)[0] != px(4, 4)[0]) blocky = 0;
        ck(blocky, "nearest sampling gives flat blocks", "");

        /* Bilinear at the same scale must NOT be flat, and must be monotone
         * between the two source texels it is interpolating. */
        surf_reset(&s);
        gfx_paint_image(&pt, IMG, 8, 8, 0, &m, 1);
        fill_rect(&s, &pt, 0, 0, 32, 32);
        int rising = 1, distinct = 0, prev = -1;
        for (int x = 6; x <= 26; x++) {
            int v = px(x, 16)[0];
            if (v < prev) rising = 0;
            if (v != prev) distinct++;
            prev = v;
        }
        snprintf(d, sizeof d, "%d distinct tones across 21 px, monotone=%d", distinct, rising);
        ck(distinct >= 12 && rising, "bilinear interpolates between texels", d);
        /* ...and the half-pixel offset is right. At scale 5 the DEVICE pixel
         * centre 7.5 maps to image coordinate 1.5, which is the exact centre of
         * source texel (1,1), so bilinear must return that texel unmixed. (At
         * scale 4 no device pixel centre lands on a texel centre at all, which
         * is why the scale is 5 and not the 4 used above -- a drift test needs
         * a sample point where the right answer is unambiguous.) */
        gfx_m_identity(&m);
        gfx_m_scale(&m, 5 * GFX_MONE, 5 * GFX_MONE);
        surf_reset(&s);
        gfx_paint_image(&pt, IMG, 8, 8, 0, &m, 1);
        fill_rect(&s, &pt, 0, 0, 40, 40);
        const unsigned char *c = px(7, 7);
        snprintf(d, sizeof d, "got (%d,%d) want (32,32)", c[0], c[1]);
        ck(abs(c[0] - 32) <= 1 && abs(c[1] - 32) <= 1,
           "a texel centre samples that texel exactly (no half-pixel drift)", d);

        struct gfx_matrix sing;
        gfx_m_set(&sing, 0, 0, 0, 0, 0, 0);
        ck(!gfx_paint_image(&pt, IMG, 8, 8, 0, &sing, 0),
           "a singular image matrix is refused", "");
    }

    /* --------------------------------------------------- gradient strip -- */
    printf("\n-- the 1-pixel gradient strip (a gradient costs a flat fill) --\n");
    {
        gfx_gradient_strip(STRIP, 64, 0x000000, 0xFFFFFF, 255);
        int worst = 0;
        for (int i = 0; i < 64; i++) {
            int want = i * 255 / 63;
            int e = abs(STRIP[i * 4] - want);
            if (e > worst) worst = e;
        }
        snprintf(d, sizeof d, "worst %d/255", worst);
        ck(worst <= 1 && STRIP[3] == 255, "the strip is the same ramp", d);

        gfx_paint_linear(&pt, 0, 0, GFX_PX(1), 0);
        gfx_paint_stop(&pt, 0, 0xFF0000, 255);
        gfx_paint_stop(&pt, GFX_MONE / 2, 0x00FF00, 128);
        gfx_paint_stop(&pt, GFX_MONE, 0x0000FF, 0);
        gfx_gradient_strip_paint(STRIP, 65, &pt);
        snprintf(d, sizeof d, "start %02X%02X%02X/%d mid %02X%02X%02X/%d end %02X%02X%02X/%d",
                 STRIP[0], STRIP[1], STRIP[2], STRIP[3],
                 STRIP[32*4], STRIP[32*4+1], STRIP[32*4+2], STRIP[32*4+3],
                 STRIP[64*4], STRIP[64*4+1], STRIP[64*4+2], STRIP[64*4+3]);
        ck(STRIP[0] == 255 && STRIP[3] == 255 &&
           STRIP[32 * 4 + 1] == 255 && abs(STRIP[32 * 4 + 3] - 128) <= 2 &&
           STRIP[64 * 4 + 2] == 255 && STRIP[64 * 4 + 3] == 0,
           "a multi-stop paint strips with its alpha intact", d);
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    printf(fails ? "FAILED\n" : "all checks passed\n");
    return fails ? 1 : 0;
}
