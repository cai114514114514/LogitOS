/* Glyph rasterization: the bridge onto Open Logit, against an oracle that is
 * neither rasterizer.
 *
 * WHY THIS EXISTS. c/kernel/gui/raster.c was a second, complete scanline
 * coverage rasterizer and it drew every glyph in the machine. Deleting it means
 * every label, menu, title and CJK page changes producer, so the migration
 * needed a number and not a screenshot. This file is that number.
 *
 * THE ORACLE IS BUILT, NOT BORROWED, and it is deliberately unlike both
 * rasterizers. Both of them sample 16 sub-scanlines per pixel row and compute
 * EXACT fractional horizontal coverage along each; an oracle sharing that
 * construction would agree with a vertical sampling bug for structural reasons.
 * So the reference here is a plain 32x32 SUPERSAMPLED POINT SAMPLE in double:
 * the outline is flattened to 0.01 px (six times finer than the engine's
 * working tolerance), and each pixel's coverage is the fraction of 1024 sample
 * points inside the nonzero-winding region. The 32-grid's sample offsets
 * ((m+0.5)/32) are not a superset of the 16-grid's ((k+0.5)/16 = (4k+2)/64,
 * even numerators over 64; the 32-grid's are odd over 64) -- so neither axis is
 * structurally aligned with what it is judging. This is the same method
 * tests/unit/gfx_raster_test.c uses for shapes, applied to type.
 *
 * It is also NOT the FreeType differential, and both are wanted: `make
 * test-font` already compares this same coverage with FreeType at a stated
 * tolerance, which is the third-party check. This one is an exactly computable
 * check that runs without python, freetype-py, or a reference file, and -- the
 * point of the exercise -- it can be pointed at BOTH rasterizers in the same
 * run, which is the one moment in history that comparison exists.
 *
 * Build:
 *   glyph_agree_test FONT [FONT...]            the bridge vs the oracle
 *   -DGLYPH_AGREE_LEGACY                       also links c/kernel/gui/raster.c
 *                                              (under renamed symbols) and
 *                                              prints legacy-vs-oracle and
 *                                              legacy-vs-bridge alongside
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "ttf.h"
#include "fontpath.h"
#include "glyphras.h"

#ifdef GLYPH_AGREE_LEGACY
/* c/kernel/gui/raster.c compiled with -Dtext_raster=raster_legacy etc, so the
 * doomed rasterizer and its replacement can be linked into one binary. */
int raster_legacy(const struct ttf_font *f, int gid, int px,
                  uint8_t *cov, int covcap, int *w, int *h, int *ox, int *oy);
#endif

static int fails;
#define FAILF(...) do { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } while (0)

/* --------------------------------------------------------------- oracle -- */

#define SS          32                 /* supersamples per axis            */
#define REF_TOL     0.01               /* flattening tolerance, device px  */
#define REF_MAXE    200000
#define REF_MAXW    1024

static double e_x0[REF_MAXE], e_y0[REF_MAXE], e_x1[REF_MAXE], e_y1[REF_MAXE];
static int ref_ne, ref_over;
static long ref_acc[REF_MAXW];

static void ref_edge(double x0, double y0, double x1, double y1)
{
    if (y0 == y1) return;
    if (ref_ne >= REF_MAXE) { ref_over = 1; return; }
    e_x0[ref_ne] = x0; e_y0[ref_ne] = y0; e_x1[ref_ne] = x1; e_y1[ref_ne] = y1;
    ref_ne++;
}

/* De Casteljau to a flatness bound, in double. Depth-capped so a degenerate
 * control net cannot recurse forever. */
static void ref_quad(double x0, double y0, double cx, double cy,
                     double x1, double y1, int d)
{
    double dx = 2 * cx - x0 - x1, dy = 2 * cy - y0 - y1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (d >= 16 || (dx + dy) / 4 <= REF_TOL) { ref_edge(x0, y0, x1, y1); return; }
    double ax = (x0 + cx) / 2, ay = (y0 + cy) / 2;
    double bx = (cx + x1) / 2, by = (cy + y1) / 2;
    double mx = (ax + bx) / 2, my = (ay + by) / 2;
    ref_quad(x0, y0, ax, ay, mx, my, d + 1);
    ref_quad(mx, my, bx, by, x1, y1, d + 1);
}

static void ref_cubic(double x0, double y0, double c1x, double c1y,
                      double c2x, double c2y, double x1, double y1, int d)
{
    double ux = 3 * c1x - 2 * x0 - x1, uy = 3 * c1y - 2 * y0 - y1;
    double vx = 3 * c2x - x0 - 2 * x1, vy = 3 * c2y - y0 - 2 * y1;
    double err = (ux > 0 ? ux : -ux) + (uy > 0 ? uy : -uy)
               + (vx > 0 ? vx : -vx) + (vy > 0 ? vy : -vy);
    if (d >= 16 || err / 6 <= REF_TOL) { ref_edge(x0, y0, x1, y1); return; }
    double ax = (x0 + c1x) / 2,  ay = (y0 + c1y) / 2;
    double bx = (c1x + c2x) / 2, by = (c1y + c2y) / 2;
    double cx = (c2x + x1) / 2,  cy = (c2y + y1) / 2;
    double dx = (ax + bx) / 2,   dy = (ay + by) / 2;
    double ex = (bx + cx) / 2,   ey = (by + cy) / 2;
    double mx = (dx + ex) / 2,   my = (dy + ey) / 2;
    ref_cubic(x0, y0, ax, ay, dx, dy, mx, my, d + 1);
    ref_cubic(mx, my, ex, ey, cx, cy, x1, y1, d + 1);
}

/* Coverage of `path` at `px` into the SAME w*h bitmap box the rasterizers use
 * (origin column ox_i, top row top_i above the baseline), by supersampling. */
static int oracle(const struct fp_path *path, int px, int upem,
                  int ox_i, int top_i, int w, int h, uint8_t *cov)
{
    if (w <= 0 || w > REF_MAXW) return -1;
    double s = (double)px / (double)upem;
    #define RX(v) ((double)(v) * s - (double)ox_i)
    #define RY(v) ((double)top_i - (double)(v) * s)
    ref_ne = 0; ref_over = 0;
    double curx = 0, cury = 0, sx = 0, sy = 0;
    for (int i = 0; i < path->n; i++) {
        const struct fp_cmd *c = &path->cmd[i];
        switch (c->op) {
        case FP_MOVE:
            curx = RX(c->x[0]); cury = RY(c->y[0]); sx = curx; sy = cury;
            break;
        case FP_LINE: {
            double nx = RX(c->x[0]), ny = RY(c->y[0]);
            ref_edge(curx, cury, nx, ny); curx = nx; cury = ny;
            break;
        }
        case FP_QUAD: {
            double qx = RX(c->x[0]), qy = RY(c->y[0]);
            double ex = RX(c->x[1]), ey = RY(c->y[1]);
            ref_quad(curx, cury, qx, qy, ex, ey, 0); curx = ex; cury = ey;
            break;
        }
        case FP_CUBIC: {
            double ax = RX(c->x[0]), ay = RY(c->y[0]);
            double bx = RX(c->x[1]), by = RY(c->y[1]);
            double ex = RX(c->x[2]), ey = RY(c->y[2]);
            ref_cubic(curx, cury, ax, ay, bx, by, ex, ey, 0); curx = ex; cury = ey;
            break;
        }
        case FP_CLOSE:
            ref_edge(curx, cury, sx, sy); curx = sx; cury = sy;
            break;
        }
    }
    /* An outline whose last contour was never CLOSEd would bleed sideways; the
     * font parsers always close, but close the walk anyway rather than trust it. */
    ref_edge(curx, cury, sx, sy);
    if (ref_over) return -1;
    #undef RX
    #undef RY

    static double xs[4096];
    static int    dirs[4096];
    memset(cov, 0, (size_t)w * h);
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) ref_acc[c] = 0;
        for (int k = 0; k < SS; k++) {
            double Y = r + (k + 0.5) / SS;
            int nc = 0;
            for (int e = 0; e < ref_ne && nc < 4096; e++) {
                double y0 = e_y0[e], y1 = e_y1[e];
                double lo = y0 < y1 ? y0 : y1, hi = y0 < y1 ? y1 : y0;
                if (Y < lo || Y >= hi) continue;       /* half-open in y */
                xs[nc] = e_x0[e] + (e_x1[e] - e_x0[e]) * (Y - y0) / (y1 - y0);
                dirs[nc] = y1 > y0 ? 1 : -1;
                nc++;
            }
            for (int a = 1; a < nc; a++) {             /* insertion sort by x */
                double vx = xs[a]; int vd = dirs[a]; int b = a - 1;
                while (b >= 0 && xs[b] > vx) { xs[b+1] = xs[b]; dirs[b+1] = dirs[b]; b--; }
                xs[b+1] = vx; dirs[b+1] = vd;
            }
            int wind = 0;
            for (int a = 0; a + 1 < nc; a++) {
                wind += dirs[a];
                if (wind == 0) continue;
                /* Sample m sits at (m + 0.5)/SS, so it is inside the half-open
                 * span [xs[a], xs[a+1]) exactly when
                 *     xs[a]*SS - 0.5 <= m < xs[a+1]*SS - 0.5. */
                int m0 = (int)ceil(xs[a] * SS - 0.5);
                int m1 = (int)ceil(xs[a+1] * SS - 0.5) - 1;
                if (m0 < 0) m0 = 0;
                if (m1 >= w * SS) m1 = w * SS - 1;
                for (int m = m0; m <= m1; m++) ref_acc[m / SS]++;
            }
        }
        for (int c = 0; c < w; c++) {
            long v = (ref_acc[c] * 255 + (SS * SS) / 2) / ((long)SS * SS);
            cov[r * w + c] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
    return 0;
}

/* ------------------------------------------------------------ comparison -- */

struct score { long sum; long npix; int max; };

static void score_add(struct score *s, const uint8_t *a, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) {
        int d = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
        s->sum += d; s->npix++;
        if (d > s->max) s->max = d;
    }
}
static double score_mean(const struct score *s) { return s->npix ? (double)s->sum / s->npix : 0.0; }

/* ------------------------------------------------------------- glyph set -- */

/* Is `gid` a glyf COMPOSITE (numberOfContours < 0)? Read straight out of loca +
 * glyf, because "composite glyphs" has to be a checked claim: in a subsetted
 * font the accented Latin may well have been flattened, and a test that merely
 * HOPED it was testing composites would be testing plain outlines. */
static uint32_t rd32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0]<<8)|p[1]); }

static int is_composite(const struct ttf_font *f, int gid)
{
    if (f->outline_fmt != TTF_OUTLINE_GLYF || !f->off_loca || !f->off_glyf) return 0;
    if (gid < 0 || gid >= f->num_glyphs) return 0;
    uint32_t a, b;
    if (f->loca_long) {
        if (f->off_loca + 4u * (uint32_t)(gid + 2) > (uint32_t)f->len) return 0;
        a = rd32(f->data + f->off_loca + 4u * gid);
        b = rd32(f->data + f->off_loca + 4u * (gid + 1));
    } else {
        if (f->off_loca + 2u * (uint32_t)(gid + 2) > (uint32_t)f->len) return 0;
        a = 2u * rd16(f->data + f->off_loca + 2u * gid);
        b = 2u * rd16(f->data + f->off_loca + 2u * (gid + 1));
    }
    if (b <= a || f->off_glyf + b > (uint32_t)f->len || b - a < 10) return 0;
    return (int16_t)rd16(f->data + f->off_glyf + a) < 0;
}

static const uint32_t LATIN[] = {
    'A','B','E','G','M','Q','R','S','W','a','b','e','g','m','o','s','w','x',
    '0','3','8','_','@','#','&','.',',','/','(' ,'%'
};
/* Dense CJK on purpose: these are where the edge count and the fine interior
 * strokes bite, and they are what the Files sidebar and zh.wikipedia.org are
 * made of. */
static const uint32_t CJK[] = {
    0x4F60, 0x597D, 0x4E16, 0x754C, 0x6587, 0x4EF6, 0x7CFB, 0x7EDF, 0x8BBE,
    0x7F6E, 0x7EC8, 0x7AEF, 0x6D4F, 0x89C8, 0x5668, 0x9EBB, 0x9E93, 0x8FB9
};
/* Composite candidates: precomposed accented Latin, which in a glyf font is
 * normally a base glyph plus a mark component. Verified with is_composite(). */
static const uint32_t COMPOSITE[] = {
    0x00C0, 0x00C4, 0x00C7, 0x00C9, 0x00CB, 0x00CE, 0x00D1, 0x00D6, 0x00DC,
    0x00E0, 0x00E4, 0x00E7, 0x00E9, 0x00EB, 0x00EE, 0x00F1, 0x00F6, 0x00FC
};

/* ------------------------------------------------------------------ run -- */

static struct fp_cmd cmdbuf[65536];
static uint8_t pathscratch[1 << 20];
static uint8_t cov_new[512 * 512], cov_ref[512 * 512];
#ifdef GLYPH_AGREE_LEGACY
static uint8_t cov_old[512 * 512];
#endif

static struct score s_new;
#ifdef GLYPH_AGREE_LEGACY
static struct score s_old, s_cross;    /* only the side-by-side build fills these */
#endif
static int n_cmp;

/* One (font, gid, px): rasterize through the bridge, through the oracle, and --
 * when built with the legacy rasterizer -- through that too, all into the SAME
 * bitmap box so the comparison is pixel to pixel with no resampling. */
static void compare(const struct ttf_font *f, int gid, int px, const char *what)
{
    int w, h, ox, oy;
    int rc = text_raster(f, gid, px, cov_new, (int)sizeof cov_new, &w, &h, &ox, &oy);
    if (rc != 0) { FAILF("%s gid %d at %dpx: the bridge REFUSED the glyph", what, gid, px); return; }
    if (w <= 0 || h <= 0) return;                       /* blank glyph */
    if ((long)w * h > (long)sizeof cov_ref) return;     /* bigger than this test's buffers */

    struct fp_path p;
    fp_init(&p, cmdbuf, (int)sizeof cmdbuf);
    if (ttf_glyph_path(f, gid, &p, pathscratch, (int)sizeof pathscratch)) return;
    if (oracle(&p, px, f->units_per_em, ox, oy, w, h, cov_ref)) {
        FAILF("%s gid %d at %dpx: oracle overflowed", what, gid, px);
        return;
    }
    score_add(&s_new, cov_new, cov_ref, w * h);

#ifdef GLYPH_AGREE_LEGACY
    int lw, lh, lox, loy;
    if (raster_legacy(f, gid, px, cov_old, (int)sizeof cov_old, &lw, &lh, &lox, &loy) == 0
        && lw == w && lh == h && lox == ox && loy == oy) {
        score_add(&s_old, cov_old, cov_ref, w * h);
        score_add(&s_cross, cov_old, cov_new, w * h);
    } else {
        FAILF("%s gid %d at %dpx: legacy box %dx%d@%d,%d != bridge %dx%d@%d,%d",
              what, gid, px, lw, lh, lox, loy, w, h, ox, oy);
    }
#endif
    n_cmp++;
}

static void run_set(const struct ttf_font *f, const char *name,
                    const uint32_t *cps, int ncp, const int *sizes, int nsz,
                    const char *what, int want_composite)
{
    int found = 0;
    for (int i = 0; i < ncp; i++) {
        int gid = ttf_glyph_id(f, cps[i]);
        if (gid <= 0) continue;
        if (want_composite && !is_composite(f, gid)) continue;
        found++;
        for (int s = 0; s < nsz; s++) compare(f, gid, sizes[s], what);
    }
    printf("  %-22s %-10s %2d glyph(s) x %d size(s)\n", name, what, found, nsz);
}

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); return 0; }
    fclose(fp);
    *len = (size_t)n;
    return b;
}

/* The largest pixel size c/kernel/gui/text.c's glyph cache can actually serve:
 * its scratch is 200x200, and text_raster refuses anything whose bitmap does
 * not fit. Found by asking rather than by assuming, because it is a property of
 * the caller's buffer and the font's own em box together. */
#define CACHE_COV_CAP (200 * 200)
static int cache_max_px_gid(const struct ttf_font *f, int gid)
{
    if (gid <= 0) return 0;
    int best = 0;
    for (int px = 8; px <= 400; px++) {
        int ox, top, w, h;
        if (text_raster_extent(f, gid, px, &ox, &top, &w, &h)) continue;
        if (w <= 0 || h <= 0 || w > 768 || (long)w * h > CACHE_COV_CAP) break;
        best = px;
    }
    return best;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: glyph_agree_test FONT [FONT...]\n"); return 2; }

    static const int LSZ[] = { 12, 16, 24, 48 };
    static const int CSZ[] = { 14, 24 };
    static const int XSZ[] = { 16, 32 };

#ifdef GLYPH_AGREE_LEGACY
    printf("glyph_agree_test: BOTH rasterizers vs a 32x32 supersampled oracle\n");
#else
    printf("glyph_agree_test: the Open Logit bridge vs a 32x32 supersampled oracle\n");
#endif

    for (int a = 1; a < argc; a++) {
        size_t flen = 0;
        uint8_t *fdata = slurp(argv[a], &flen);
        if (!fdata) { printf("no font %s\n", argv[a]); return 2; }
        struct ttf_font f;
        if (ttf_parse(fdata, (int)flen, &f) != 0) { printf("parse failed: %s\n", argv[a]); return 2; }
        const char *base = strrchr(argv[a], '/');
        base = base ? base + 1 : argv[a];
        printf("%s: %d glyphs, upem %d, %s outlines\n", base, f.num_glyphs, f.units_per_em,
               f.outline_fmt == TTF_OUTLINE_CFF ? "CFF" : "glyf");

        run_set(&f, base, LATIN, (int)(sizeof LATIN / sizeof LATIN[0]), LSZ, 4, "latin", 0);
        run_set(&f, base, CJK, (int)(sizeof CJK / sizeof CJK[0]), CSZ, 2, "cjk", 0);
        run_set(&f, base, COMPOSITE, (int)(sizeof COMPOSITE / sizeof COMPOSITE[0]),
                XSZ, 2, "composite", 1);

        /* THE POINT BUDGET, and it is found rather than assumed. glyphras.c has
         * no allocator: a glyph whose flattened outline does not fit its fixed
         * point buffer is REFUSED, and a refused glyph is a character missing
         * from a label. So the worst case has to be the actual worst case --
         * every glyph in the font is scanned for the one with the most drawing
         * commands (cheap: ttf_glyph_path does no rasterization), and THAT is
         * what gets rasterized, at the largest pixel size the kernel's 200x200
         * glyph cache can serve. Picking a dense-looking codepoint by hand
         * would only measure the author's guess about the font. */
        int densest = -1, densest_n = -1;
        for (int g = 0; g < f.num_glyphs; g++) {
            struct fp_path p;
            fp_init(&p, cmdbuf, (int)sizeof cmdbuf);
            if (ttf_glyph_path(&f, g, &p, pathscratch, (int)sizeof pathscratch)) continue;
            if (p.n > densest_n) { densest_n = p.n; densest = g; }
        }
        int maxpx = densest >= 0 ? cache_max_px_gid(&f, densest) : 0;
        if (maxpx > 0) {
            printf("  %-22s %-10s gid %d, %d commands (the font's densest), at %d px "
                   "(largest the 200x200 cache serves)\n",
                   base, "budget", densest, densest_n, maxpx);
            compare(&f, densest, maxpx, "budget");
            /* and at the sizes the UI actually draws, where the cache is hot */
            for (int s = 0; s < 4; s++) compare(&f, densest, LSZ[s], "budget");
        }
    }

    printf("\n%d (glyph, size) bitmaps compared, %ld pixels\n", n_cmp, s_new.npix);
    printf("  bridge  vs oracle : mean %.3f  max %d   (of 255)\n", score_mean(&s_new), s_new.max);
#ifdef GLYPH_AGREE_LEGACY
    printf("  raster.c vs oracle: mean %.3f  max %d   (of 255)\n", score_mean(&s_old), s_old.max);
    printf("  raster.c vs bridge: mean %.3f  max %d   (of 255)\n", score_mean(&s_cross), s_cross.max);
#endif
    printf("  peak flattened points in one glyph: %d of %d; outlines refused: %d\n",
           glyphras_peak_points(), 8192, glyphras_refused());

    /* THE BOUND, and what the numbers justify.
     *
     * Measured on the build where both rasterizers existed, over this exact set
     * (572 bitmaps, 363,650 pixels):
     *
     *     Open Logit bridge  mean 0.310 / 255   worst pixel 16
     *     c/kernel/gui/raster.c   mean 0.490 / 255   worst pixel 61
     *     the two against each other   mean 0.320   worst pixel 63
     *
     * They are NOT bit-identical and were never going to be -- different
     * flattening (adaptive against a tolerance, versus a fixed segment count
     * per curve), different accumulation. What the numbers say is that the
     * replacement is closer to the true geometry than the thing it replaced, by
     * 1.6x in the mean and 3.8x in the worst pixel.
     *
     * THE GATE IS 0.45, AND IT WAS 1.0 UNTIL IT WAS CHECKED AGAINST THE
     * REGRESSIONS IT CLAIMS TO CATCH. The argument written here for 1.0 was
     * "twice raster.c's own score, so a regression to merely-as-good-as-the-old
     * one still fails" -- which is exactly backwards: a bound of 1.0 PASSES
     * everything at or under 1.0, and raster.c's 0.490 is under it. A gate set
     * at twice a score lets that score through. So the bound is now set from
     * the regressions themselves, each one measured on this set rather than
     * imagined:
     *
     *     0.310  what this rasterizer scores today            must PASS
     *     0.490  raster.c, i.e. losing the migration's win    must FAIL
     *     0.911  the same bridge on gfx_raster.c BEFORE g_acc
     *            (the per-sub-scanline truncation)            must FAIL
     *     1.398  the bridge with its sampling shifted by ONE
     *            sub-scanline, 1/16 px                        must FAIL
     *
     * 0.45 is the only round number that separates the first from the rest. It
     * still leaves 45% headroom over a measurement that does not drift: 0.310
     * exactly, under gcc and clang, at -O0, -O2 and -Ofast (the oracle's double
     * arithmetic is the only place a compiler could move it, and it does not).
     * A bound that lets the code it replaced pass is not a gate, it is a note.
     *
     * The MAX is reported and not gated: a single pixel straddling a
     * near-horizontal edge legitimately reaches the tens under either sampling
     * grid, so gating it would be gating the reference's own sampling rather
     * than the rasterizer. */
    if (score_mean(&s_new) > 0.45) {
        printf("FAIL bridge mean |coverage difference| %.3f/255 exceeds 0.45\n", score_mean(&s_new));
        fails++;
    }
    if (glyphras_refused() != 0) {
        printf("FAIL %d outline(s) refused for want of point storage -- those glyphs do not draw\n",
               glyphras_refused());
        fails++;
    }
    if (n_cmp == 0) { printf("FAIL nothing was compared\n"); fails++; }

    printf(fails ? "SOME FAILED (%d)\n" : "ALL PASS\n", fails);
    return fails ? 1 : 0;
}
