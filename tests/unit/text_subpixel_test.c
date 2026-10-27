/* text_subpixel_test.c -- quarter-pixel glyph positioning in c/kernel/gui/text.c
 * and the optional glyph coverage LUT in ol_display_blit_glyph_a.
 * Links the real text.c, the shaper, glyphras.c, the gfx library and the
 * shipped fonts; fb_blit_glyph is stubbed to record every mask. Prints
 * "FAIL <name>" per failing check and exits 1 on any.
 *   gap      "iiii" at 13 px: successive ink centroids differ by the 24.8
 *            advance to within 0.25 px (a whole-pixel pen is off by up to 0.9)
 *   phase    the phases of a glyph are distinct masks
 *   sub2     the subx=2 mask equals the supersampled oracle at +0.5 px (the
 *            test-glyph-agree method), and subx=0 is the old mask
 *   lut-*    a disabled LUT is the identity; an enabled one changes mid
 *            coverage only, keeps 0/255, is monotonic, and is per display
 * The control builds the same sources with -DTEXT_INTEGER_PEN: "FAIL gap". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include "logit_abi.h"
#include "text.h"
#include "ttf.h"
#include "fontpath.h"
#include "glyphras.h"
#include "spinlock.h"
#include "openlogit_display.h"
#include "openlogit_display_blit.h"

int text_raster_at_sub(const struct ttf_font *f, int gid, int px, int ox_i, int top_i,
                       int subx, uint8_t *cov, int w, int h);

static const char *root;
static int bad;
#define CHECK(c, name) do { if (!(c)) { printf("FAIL %s\n", name); bad = 1; } \
                            else printf("PASS %s\n", name); } while (0)

void *kmalloc(unsigned long n) { return malloc(n); }
void kfree(void *p) { free(p); }
void kprintf(const char *fmt, ...) { (void)fmt; }
void spin_lock(spinlock_t *p) { (void)p; }
void spin_unlock(spinlock_t *p) { (void)p; }
uint64_t spin_lock_irqsave(spinlock_t *p) { (void)p; return 0; }
void spin_unlock_irqrestore(spinlock_t *p, uint64_t f) { (void)p; (void)f; }
static FILE *font_file(const char *path)
{ char name[2048]; snprintf(name, sizeof name, "%s%s", root, path); return fopen(name, "rb"); }
int vfs_size(const char *path)
{ FILE *f = font_file(path); if (!f) return -1; fseek(f, 0, SEEK_END); int n = (int)ftell(f); fclose(f); return n; }
int vfs_read(const char *path, void *buf, int max)
{ FILE *f = font_file(path); if (!f) return -1; int n = (int)fread(buf, 1, (size_t)max, f); fclose(f); return n; }

/* ------------------------------------------------------- recorded blits -- */
#define MAXB 16
static struct { int x, y, w, h; uint8_t cov[64 * 64]; } rec[MAXB];
static int nrec;
void fb_blit_glyph(int x, int y, const unsigned char *cov, int w, int h, uint32_t color)
{
    (void)color;
    if (nrec >= MAXB || w * h > 64 * 64) return;
    rec[nrec].x = x; rec[nrec].y = y; rec[nrec].w = w; rec[nrec].h = h;
    memcpy(rec[nrec].cov, cov, (size_t)w * h);
    nrec++;
}
/* Ink centroid x (pixels, absolute) of a recorded mask. */
static double centroid(int k)
{
    double s = 0, m = 0;
    for (int j = 0; j < rec[k].h; j++) for (int i = 0; i < rec[k].w; i++) {
        double c = rec[k].cov[j * rec[k].w + i];
        s += c * (rec[k].x + i + 0.5); m += c;
    }
    return m > 0 ? s / m : -1;
}

/* --------------------------------------------------------------- oracle -- */
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
                  int ox_i, int top_i, int w, int h, double shift, uint8_t *cov)
{
    if (w <= 0 || w > REF_MAXW) return -1;
    double s = (double)px / (double)upem;
    #define RX(v) ((double)(v) * s - (double)ox_i + shift)
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


static uint8_t *slurp(const char *rel, long *len)
{
    FILE *f = font_file(rel); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)*len);
    if (fread(b, 1, (size_t)*len, f) != (size_t)*len) { free(b); b = NULL; }
    fclose(f); return b;
}

static struct fp_cmd cmdbuf[65536];
static uint8_t pathscratch[1 << 20];
static uint8_t cov_a[128 * 128], cov_ref[128 * 128];

static void check_gap(const struct ttf_font *tf)
{
    int px = 13;
    int gid = ttf_glyph_id(tf, 'i');
    double adv = (double)ttf_advance(tf, gid) * px / tf->units_per_em;
    double fr = adv - floor(adv);
    printf("i advance at %dpx = %.4f (frac %.3f)\n", px, adv, fr);
    CHECK(fr >= 0.15 && fr <= 0.85, "adv-fractional");   /* else the test proves nothing */
    nrec = 0;
    text_draw_run(10, 5, "iiii", 4, px, 0, 0xFFFFFF);
    CHECK(nrec == 4, "iiii-blits");
    double worst = 0;
    for (int k = 0; k + 1 < nrec; k++) {
        double d = fabs((centroid(k + 1) - centroid(k)) - adv);
        if (d > worst) worst = d;
    }
    printf("worst gap deviation %.3f px\n", worst);
    CHECK(nrec == 4 && worst <= 0.25, "gap");
    int distinct = 0;
    for (int k = 1; k < nrec; k++)
        if (rec[k].w != rec[0].w || memcmp(rec[k].cov, rec[0].cov, (size_t)rec[0].w * rec[0].h)) distinct++;
    CHECK(distinct >= 1, "phase");
}

static void check_sub2(const struct ttf_font *tf)
{
    static const int cps[] = { 'i', 'o', 'g', 'W', 'e', 'a' };
    static const int pxs[] = { 13, 24 };
    int worst = 0, worst0 = 0, differ = 0, n = 0;
    for (unsigned a = 0; a < sizeof cps / sizeof *cps; a++)
    for (unsigned b = 0; b < sizeof pxs / sizeof *pxs; b++) {
        int px = pxs[b], gid = ttf_glyph_id(tf, cps[a]);
        int ox, top, w, h;
        if (text_raster_extent(tf, gid, px, &ox, &top, &w, &h) || w * h > 128 * 128) continue;
        struct fp_path p; fp_init(&p, cmdbuf, (int)sizeof cmdbuf);
        if (ttf_glyph_path(tf, gid, &p, pathscratch, (int)sizeof pathscratch)) continue;
        if (text_raster_at_sub(tf, gid, px, ox, top, 2, cov_a, w, h)) { worst = 999; continue; }
        if (oracle(&p, px, tf->units_per_em, ox, top, w, h, 0.5, cov_ref)) continue;
        for (int i = 0; i < w * h; i++) { int d = abs(cov_a[i] - cov_ref[i]); if (d > worst) worst = d; }
        /* subx 0 is unchanged from text_raster_at, and subx 2 really moved */
        static uint8_t c0[128 * 128], c1[128 * 128];
        text_raster_at_sub(tf, gid, px, ox, top, 0, c0, w, h);
        text_raster_at(tf, gid, px, ox, top, c1, w, h);
        if (memcmp(c0, c1, (size_t)w * h)) worst0 = 999;
        if (memcmp(c0, cov_a, (size_t)w * h)) differ++;
        n++;
    }
    printf("sub2 vs oracle@+0.5: worst %d/255 over %d glyphs\n", worst, n);
    CHECK(n >= 8 && worst <= 20, "sub2");
    CHECK(worst0 == 0 && differ == n, "sub0-and-shift");
}

/* ------------------------------------------------------------------ LUT -- */
#define LW 8
#define LH 8
static uint32_t PA[LW * LH], PB[LW * LH];
static void *t_alloc(unsigned long n) { return malloc(n); }
static void t_free(void *p) { free(p); }
static void bind(struct ol_display *d, struct ol_pixel_target *t, uint32_t *px)
{
    memset(d, 0, sizeof *d); memset(t, 0, sizeof *t);
    t->px = px; t->w = LW; t->h = LH;
    d->allocate = t_alloc; d->release = t_free;
    ol_display_bind(d, t, 16, 8, 0);
}
static void fillpx(uint32_t *px, uint32_t v) { for (int i = 0; i < LW * LH; i++) px[i] = v; }

static void check_lut(void)
{
    static struct ol_display D1, D2, R;
    static struct ol_pixel_target T1, T2, TR;
    static uint32_t PR[LW * LH];
    bind(&D1, &T1, PA); bind(&D2, &T2, PB); bind(&R, &TR, PR);
    uint8_t cov[LW * LH];
    for (int i = 0; i < LW * LH; i++) cov[i] = (uint8_t)(i * 4 + (i & 1));
    cov[0] = 0; cov[1] = 255;

    /* disabled: identical to the legacy glyph blit */
    fillpx(PA, 0xF0F0F0); fillpx(PR, 0xF0F0F0);
    ol_display_blit_glyph_a(&D1, 0, 0, cov, LW, LH, LW, 0x202020, 255);
    ol_display_blit_glyph(&R, 0, 0, cov, LW, LH, 0x202020);
    CHECK(memcmp(PA, PR, sizeof PA) == 0, "lut-disabled-identity");

    uint8_t lut[256];
    ol_display_glyph_lut_for(lut, 0x202020, 0xF0F0F0);
    int mono = 1, fixed = lut[0] == 0 && lut[255] == 255, moved = 0;
    for (int a = 1; a < 256; a++) if (lut[a] < lut[a - 1]) mono = 0;
    for (int a = 0; a < 256; a++) if (lut[a] != a) moved++;
    CHECK(mono && fixed && moved > 100, "lut-shape");
    uint8_t lut2[256];
    ol_display_glyph_lut_for(lut2, 0xF0F0F0, 0x202020);
    CHECK(lut2[128] < 128 && lut[128] > 128, "lut-by-luma");
    ol_display_glyph_lut_for(lut2, 0x808080, 0x7C7C7C);
    int idn = 1; for (int a = 0; a < 256; a++) if (lut2[a] != a) idn = 0;
    CHECK(idn, "lut-near-equal-identity");

    /* enabled on D1 only: D1 changes, D2 (interleaved) does not */
    CHECK(ol_display_set_glyph_lut(&D1, lut) == OL_OK, "lut-set");
    fillpx(PA, 0xF0F0F0); fillpx(PB, 0xF0F0F0); fillpx(PR, 0xF0F0F0);
    ol_display_blit_glyph_a(&D1, 0, 0, cov, LW, LH, LW, 0x202020, 255);
    ol_display_blit_glyph_a(&D2, 0, 0, cov, LW, LH, LW, 0x202020, 255);
    ol_display_blit_glyph(&R, 0, 0, cov, LW, LH, 0x202020);
    CHECK(memcmp(PB, PR, sizeof PB) == 0, "lut-per-display");
    CHECK(memcmp(PA, PR, sizeof PA) != 0 && PA[0] == PR[0] && PA[1] == PR[1], "lut-enabled");

    /* and disabling restores the identity */
    ol_display_set_glyph_lut(&D1, NULL);
    fillpx(PA, 0xF0F0F0);
    ol_display_blit_glyph_a(&D1, 0, 0, cov, LW, LH, LW, 0x202020, 255);
    CHECK(memcmp(PA, PR, sizeof PA) == 0, "lut-redisabled-identity");
    ol_display_destroy(&D1); ol_display_destroy(&D2); ol_display_destroy(&R);
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    root = argv[1];
    text_init();
    long n = 0;
    uint8_t *buf = slurp("/fonts/ui.ttf", &n);
    static struct ttf_font tf;
    if (!buf || ttf_parse(buf, (int)n, &tf)) { printf("FAIL font-load\n"); return 1; }
    check_gap(&tf);
    check_sub2(&tf);
    check_lut();
    return bad;
}
