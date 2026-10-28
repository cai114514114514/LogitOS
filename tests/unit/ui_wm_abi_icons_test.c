/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Icon set C and the Logit mark (ui-wm-abi; design 3.7.1, 3.7.5, P7): the
 * real gfx engine and c/kernel/gui/icons.c as shipped.
 *
 *   icon-nonempty <NAME>  every set C symbol inks at 12/16/20/32 px and the
 *                         125 %/150 % sizes 15/18/24, weights 1..3
 *   icon-box <NAME>       ...and leaves the bitmap's border ring clear at
 *                         every one of them (ink inside 10..90 of the box)
 *   icon-weight <NAME>    weight 3 inks more than weight 2, and 2 more than 1,
 *                         at 32 px; a fill-only symbol (STOP, SPARKLES) inks
 *                         exactly the same at every weight (one cache entry)
 *   icon-large <NAME>     drawn at wm.c's 512 px ceiling (the path scratch
 *                         holds it), every weight
 *   logit-ends            the mark's curve, at 200 px: its ink box lies inside
 *                         10..90 % of the box
 *   logit-gap             a clear ring between the curve and the circle
 *   logit-sigmoid         rising left to right, flat ends, steepest in the
 *                         middle: the column centroid falls monotonically and
 *                         its slope at the centre is >= 2x that near the ends
 *   logit-symmetry        point-symmetric about the centre (rotate 180, <= 12/255)
 *   ids                   ICON_* == GICON_* (static), set C appended after set B
 *
 * Prints "FAIL <check> [<NAME>]" per failing check and exits 1; an empty
 * symbol is reported once (icon-nonempty) and skipped by the other checks.
 * THE CONTROL (-DUI_ICONS_NEGCTL in icons.c) empties SPARKLES and must log
 * exactly 'FAIL icon-nonempty SPARKLES'. Built with -DGFX_NCTX=2 and no fb.c,
 * so it defines gfx_ctx_hook. `--dump` prints per-icon ink at 32 px. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "spinlock.h"
#include "icons.h"
#include "logit.h"

long uwa_test_sys(long n, long a, long b, long c) { (void)n; (void)a; (void)b; (void)c; return 0; }

#define SAME(a, b) _Static_assert((a) == (b), #a " != " #b)
SAME(ICON_PHOTOS, GICON_PHOTOS); SAME(ICON_SLIDERS, GICON_SLIDERS);
SAME(ICON_PHOTOS, 9);            SAME(ICON_SLIDERS, 10);
SAME(ICON_SUN, ICON_BELL + 1);   /* set C is appended: A and B ids unchanged */
SAME(ICON_SUN, GICON_SUN);       SAME(ICON_MOON, GICON_MOON);
SAME(ICON_WIFI, GICON_WIFI);     SAME(ICON_DISK, GICON_DISK);
SAME(ICON_APPS, GICON_APPS);     SAME(ICON_LIST, GICON_LIST);
SAME(ICON_SORT, GICON_SORT);     SAME(ICON_STOP, GICON_STOP);
SAME(ICON_XMARK_OCTAGON, GICON_XMARK_OCTAGON); SAME(ICON_SPARKLES, GICON_SPARKLES);
SAME(ICON_SIDEBAR_R, GICON_SIDEBAR_R); SAME(ICON_MUSIC, GICON_MUSIC);
SAME(ICON_FILM, GICON_FILM);     SAME(ICON_WAVEFORM, GICON_WAVEFORM);
SAME(ICON_CPU, GICON_CPU);       SAME(ICON_MEMORY, GICON_MEMORY);
SAME(ICON_PALETTE, GICON_PALETTE); SAME(ICON_CURSOR, GICON_CURSOR);
SAME(ICON_KEYBOARD, GICON_KEYBOARD); SAME(ICON_POWER, GICON_POWER);
SAME(ICON_RESTART, GICON_RESTART); SAME(ICON_BOLD, GICON_BOLD);
SAME(ICON_ITALIC, GICON_ITALIC); SAME(ICON_TEXT_SIZE, GICON_TEXT_SIZE);
SAME(ICON_COPY, GICON_COPY);     SAME(ICON_PERSON, GICON_PERSON);
SAME(ICON_DOCK, GICON_DOCK);     SAME(ICON_LOGIT, GICON_LOGIT);
SAME(ICON_COUNT, GICON_COUNT);   SAME(ICON_COUNT, ICON_LOGIT + 1);

static int hook_fn(void) { return 0; }
int (*gfx_ctx_hook)(void) = hook_fn;
void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
void spin_lock(spinlock_t *l) { (void)l; }
void spin_unlock(spinlock_t *l) { (void)l; }

static uint8_t g_cov[512 * 512];
static int g_w, g_h;
void fb_blit_glyph(int x, int y, const uint8_t *cov, int w, int h, uint32_t color)
{ (void)x; (void)y; (void)color; g_w = w; g_h = h; memcpy(g_cov, cov, (size_t)w * h); }

static const char *const NAME[] = {
    "SUN", "MOON", "WIFI", "DISK", "APPS", "LIST", "SORT", "STOP", "XMARK_OCTAGON",
    "SPARKLES", "SIDEBAR_R", "MUSIC", "FILM", "WAVEFORM", "CPU", "MEMORY", "PALETTE",
    "CURSOR", "KEYBOARD", "POWER", "RESTART", "BOLD", "ITALIC", "TEXT_SIZE", "COPY",
    "PERSON", "DOCK", "LOGIT",
};
_Static_assert(sizeof NAME / sizeof *NAME == ICON_COUNT - ICON_SUN, "one name per set C id");

static int bad;
static void fail(const char *check, const char *name, const char *detail)
{
    printf("FAIL %s%s%s%s%s\n", check, name ? " " : "", name ? name : "",
           detail ? ": " : "", detail ? detail : "");
    bad = 1;
}

static long ink(void) { long s = 0; for (int i = 0; i < g_w * g_h; i++) s += g_cov[i]; return s; }
static int ring_clear(void)
{
    for (int i = 0; i < g_w; i++)
        if (g_cov[i] || g_cov[(g_h - 1) * g_w + i] || g_cov[i * g_w] || g_cov[i * g_w + g_w - 1]) return 0;
    return 1;
}
static int draw(int id, int px, int weight)
{
    g_w = g_h = 0;
    icon_draw_w(id, 0, 0, px, 0xFFFFFF, weight);
    return g_w == px && g_h == px;
}

static int fill_only(int id) { return id == ICON_STOP || id == ICON_SPARKLES; }

static void test_set_c(int dump)
{
    static const int px[] = { 12, 16, 20, 32, 15, 18, 24 };
    for (int id = ICON_SUN; id < ICON_COUNT; id++) {
        const char *nm = NAME[id - ICON_SUN];
        int empty = 0, outside = 0;
        char why[96] = "";
        for (unsigned k = 0; k < sizeof px / sizeof *px; k++)
            for (int w = 1; w <= 3; w++) {
                if (!draw(id, px[k], w) || ink() == 0) {
                    if (!empty) snprintf(why, sizeof why, "nothing at %d px weight %d", px[k], w);
                    empty++;
                } else if (!ring_clear() && !outside++) {
                    snprintf(why, sizeof why, "ink on the border at %d px weight %d", px[k], w);
                }
            }
        if (empty) { fail("icon-nonempty", nm, why); continue; }
        if (outside) fail("icon-box", nm, why);
        long a[4];
        for (int w = 1; w <= 3; w++) { draw(id, 32, w); a[w] = ink(); }
        if (dump) printf("info %-14s ink32 w1 %6ld w2 %6ld w3 %6ld\n", nm, a[1] / 255, a[2] / 255, a[3] / 255);
        if (fill_only(id) ? !(a[1] == a[2] && a[2] == a[3]) : !(a[1] < a[2] && a[2] < a[3])) {
            snprintf(why, sizeof why, "ink at 32 px w1 %ld w2 %ld w3 %ld", a[1], a[2], a[3]);
            fail("icon-weight", nm, why);
        }
        for (int w = 1; w <= 3; w++)
            if (!draw(id, 512, w) || ink() == 0) {
                snprintf(why, sizeof why, "refused at 512 px weight %d", w);
                fail("icon-large", nm, why);
                break;
            }
    }
}

/* The mark at 200 px: split the coverage into the curve (radius <= 0.30 of
 * the box) and the ring (stroked at 0.37, inner edge 0.335), with the band
 * 0.305..0.328 between them -- clear of both edges' anti-aliasing -- the gap.
 * Units below are box fractions. */
static void test_logit(void)
{
    enum { P = 200 };
    static uint8_t A[P * P];
    if (!draw(ICON_LOGIT, P, 2) || ink() == 0) return;   /* reported as icon-nonempty */
    memcpy(A, g_cov, sizeof A);
    double c = (P - 1) / 2.0;
    int x0 = P, x1 = -1, y0 = P, y1 = -1, gap_max = 0;
    static double colsum[P], colw[P];
    for (int y = 0; y < P; y++)
        for (int x = 0; x < P; x++) {
            int v = A[y * P + x];
            if (!v) continue;
            double r = hypot(x - c, y - c) / P;
            if (r <= 0.30) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
                colsum[x] += (double)v * y; colw[x] += v;
            } else if (r > 0.305 && r < 0.328 && v > gap_max) {
                gap_max = v;
            }
        }
    char why[128];
    if (x1 < 0) { fail("logit-ends", NULL, "no curve inside the circle"); return; }
    if (x0 < P / 10 || x1 > P * 9 / 10 || y0 < P / 10 || y1 > P * 9 / 10) {
        snprintf(why, sizeof why, "curve ink box x %d..%d y %d..%d of %d", x0, x1, y0, y1, P);
        fail("logit-ends", NULL, why);
    }
    if (gap_max > 8) {
        snprintf(why, sizeof why, "coverage %d/255 between the curve and the ring", gap_max);
        fail("logit-gap", NULL, why);
    }
    /* centroid per column, only where the column has real ink */
    double cy[P];
    int n = 0, first = -1, last = -1;
    for (int x = x0; x <= x1; x++) {
        cy[x] = colw[x] > 255 * 2 ? colsum[x] / colw[x] : -1;
        if (cy[x] >= 0) { if (first < 0) first = x; last = x; n++; }
    }
    int mono = 1;
    double prev = 1e9;
    for (int x = first; x <= last; x++) {
        if (cy[x] < 0) continue;
        if (cy[x] > prev + 0.5) mono = 0;          /* y must fall as x grows */
        prev = cy[x];
    }
    int span = last - first, mid = first + span / 2, q = span / 8;
    double s_mid = fabs(cy[mid + q / 2] - cy[mid - q / 2]) / (q ? q : 1);
    double s_l = fabs(cy[first + q] - cy[first]) / (q ? q : 1);
    double s_r = fabs(cy[last] - cy[last - q]) / (q ? q : 1);
    printf("info logit curve box x %d..%d y %d..%d of %d, gap max %d, slope ends %.2f/%.2f centre %.2f\n",
           x0, x1, y0, y1, P, gap_max, s_l, s_r, s_mid);
    if (!n || !mono || !(cy[first] > cy[last]) || s_mid < 2 * s_l || s_mid < 2 * s_r) {
        snprintf(why, sizeof why, "monotone %d, rise %.1f, slope ends %.2f/%.2f centre %.2f",
                 mono, cy[first] - cy[last], s_l, s_r, s_mid);
        fail("logit-sigmoid", NULL, why);
    }
    int worst = 0;
    for (int y = 0; y < P; y++)
        for (int x = 0; x < P; x++) {
            int d = abs((int)A[y * P + x] - (int)A[(P - 1 - y) * P + (P - 1 - x)]);
            if (d > worst) worst = d;
        }
    /* x coverage is exact, y is sampled in 64 sub-rows: a curve edge and its
     * rotated twin differ by up to a few levels per edge */
    if (worst > 12) {
        snprintf(why, sizeof why, "rotate-180 max delta %d/255", worst);
        fail("logit-symmetry", NULL, why);
    }
}

int main(int argc, char **argv)
{
    int dump = argc > 1 && !strcmp(argv[1], "--dump");
    test_set_c(dump);
    test_logit();
    if (bad) { printf("ui-wm-abi-icons: FAILED\n"); return 1; }
    printf("ui-wm-abi-icons: ok (%d set C symbols + the Logit mark)\n", ICON_LOGIT - ICON_SUN);
    return 0;
}
