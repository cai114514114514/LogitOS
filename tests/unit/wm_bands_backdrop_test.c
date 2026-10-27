/* The band splitter (c/kernel/gui/wm_bands.h) never cuts a backdrop region's
 * sample extent: property test over randomised regions, placements, materials
 * and damage rectangles, through the real chain wm.c's par_nocut uses --
 * regions recorded by wm2_backdrop, their no-cut rows from wm_backdrop_nocut,
 * and wm_bands_split with those rows as both no-cut and glass cost (as
 * par_render passes them).
 *
 *   NO-CUT extent  every region whose sample box meets the rectangle is in the
 *                  list, with exactly the rows region +- its material's halo
 *                  (ol_display_glass_sample_halo_ex; 0 under Reduce
 *                  Transparency), and no other region is
 *   NO-CUT region  no band boundary falls strictly inside any of them
 *
 * The control (-DWM_BANDS_NEGCTL_IGNORE_GLASS: the splitter may cut anywhere)
 * must fail 'NO-CUT region' and nothing else. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "wm_draw2.h"
#include "wm_cover.h"
#include "wm_bands.h"
#include "openlogit_glass_material.h"

int (*gfx_ctx_hook)(void);

enum { SW = 400, SH = 600 };
static uint32_t surf[SW * SH];
static int copyin(void *d, const void *s, unsigned long n) { memcpy(d, s, n); return 0; }

static int fails;
static void verdict(int ok, const char *name, const char *detail)
{
    printf("%s %s %s\n", ok ? "PASS" : "FAIL", name, detail);
    if (!ok) fails++;
}

struct want { int y0, y1, x0, x1; };

int main(void)
{
    srand(20261027);
    struct ol_display d;
    memset(&d, 0, sizeof d);
    struct ol_pixel_target t = { .px = surf, .w = SW, .h = SH };
    if (ol_display_bind(&d, &t, 16, 8, 0) != OL_OK) { printf("FAIL harness bind\n"); return 2; }
    struct wm2_env e = { .d = &d, .scale_pct = 100, .surf_w = SW, .surf_h = SH, .copyin = copyin };

    long extent_bad = 0, cut_bad = 0, multi = 0, checked = 0, with_regions = 0;
    for (int trial = 0; trial < 20000; trial++) {
        int nwin = 1 + rand() % 3, reduce = (rand() % 5) == 0;
        struct wm_yrange nc[3 * LOGIT_BACKDROP_MAX];
        int nn = 0;
        struct want want[3 * LOGIT_BACKDROP_MAX];
        int nw = 0;
        int ry0 = rand() % 300, ry1 = ry0 + 128 + rand() % 900;
        int rx0 = rand() % 400, rx1 = rx0 + 50 + rand() % 900;
        struct drect R = { rx0, ry0, rx1, ry1 };
        for (int k = 0; k < nwin; k++) {
            int slot = k;
            wm2_backdrop_win_reset(slot);
            e.slot = slot;
            int nreg = 1 + rand() % 3;
            struct wm_bd_place pl = { rand() % 600 - 100, rand() % 700 - 100, SW, SH, SW, SH };
            for (int i = 0; i < nreg; i++) {
                struct logit_backdrop2 bd;
                memset(&bd, 0, sizeof bd);
                bd.size = sizeof bd;
                bd.id = i;
                bd.x = (rand() % 300) << 8;
                bd.y = (rand() % 500) << 8;
                bd.w = (8 + rand() % 200) << 8;
                bd.h = (8 + rand() % 300) << 8;
                int r = rand() % 20;
                for (int q = 0; q < 4; q++) bd.radius[q] = (rand() % 2 ? r : 0) << 8;
                bd.opacity = rand() % 256;
                bd.blur_level = rand() % 4;
                bd.rim = rand() % 2;
                bd.tint_argb = 0x808080;
                if (wm2_backdrop(&e, (long)&bd, 0, 0) != OL_OK) { printf("FAIL harness backdrop\n"); return 2; }
            }
            nn = wm_backdrop_nocut(slot, &pl, reduce, &R, nc, nn, (int)(sizeof nc / sizeof nc[0]));
            /* the independent expectation, from the recorded regions */
            struct wm2_region rg[LOGIT_BACKDROP_MAX];
            int n = wm_backdrop_regions(slot, rg, LOGIT_BACKDROP_MAX);
            for (int i = 0; i < n; i++) {
                /* the material is not exported: the random frames check the
                 * halo's shape (symmetric, >= 1, 0 under Reduce Transparency);
                 * the exact value is checked for one known material below */
                struct drect o = { pl.ox + rg[i].r.x0, pl.oy + rg[i].r.y0, pl.ox + rg[i].r.x1, pl.oy + rg[i].r.y1 };
                want[nw].x0 = o.x0; want[nw].x1 = o.x1;
                want[nw].y0 = o.y0; want[nw].y1 = o.y1;
                nw++;
            }
        }
        if (nw) with_regions++;
        /* NO-CUT extent: every listed range is a region +- a halo that is
         * >= the bare halo bound, and every region whose widest possible box
         * meets R is listed */
        for (int i = 0; i < nn; i++) {
            int found = 0;
            for (int k = 0; k < nw && !found; k++) {
                int h0 = want[k].y0 - nc[i].y0, h1 = nc[i].y1 - want[k].y1;
                if (h0 == h1 && (reduce ? h0 == 0 : h0 >= 1)) found = 1;
            }
            if (!found) extent_bad++;
        }
        for (int k = 0; k < nw; k++) {
            /* a region strictly inside R (no halo needed to meet it) must be listed */
            if (want[k].x1 > R.x0 && want[k].x0 < R.x1 && want[k].y1 > R.y0 && want[k].y0 < R.y1) {
                int listed = 0;
                for (int i = 0; i < nn; i++)
                    if (nc[i].y0 <= want[k].y0 && nc[i].y1 >= want[k].y1 &&
                        want[k].y0 - nc[i].y0 == nc[i].y1 - want[k].y1) listed = 1;
                if (!listed) extent_bad++;
            }
        }
        struct wm_band b[WM_BAND_MAX];
        int want_b = 2 + rand() % 7;
        int m = wm_bands_split(R.y0, R.y1, nc, nn, nc, nn, want_b, b);
        if (m > 1) multi++;
        for (int i = 1; i < m; i++)
            for (int k = 0; k < nn; k++) {
                checked++;
                if (b[i].y0 > nc[k].y0 && b[i].y0 < nc[k].y1) cut_bad++;
            }
    }
    /* the exact halo for one known material */
    {
        wm2_backdrop_win_reset(5);
        e.slot = 5;
        struct logit_backdrop2 bd;
        memset(&bd, 0, sizeof bd);
        bd.size = sizeof bd;
        bd.x = 10 << 8; bd.y = 100 << 8; bd.w = 160 << 8; bd.h = 300 << 8;
        bd.radius[1] = bd.radius[2] = 12 << 8;
        bd.blur_level = OL_GLASS_BLUR_THICK;
        bd.opacity = 90;
        wm2_backdrop(&e, (long)&bd, 0, 0);
        struct wm_bd_place pl = { 50, 40, SW, SH, SW, SH };
        struct drect R = { 0, 0, 800, 900 };
        struct wm_yrange one[2];
        int n = wm_backdrop_nocut(5, &pl, 0, &R, one, 0, 2);
        struct ol_glass_material m = { .opacity = 90, .blur = OL_GLASS_BLUR_THICK };
        int halo = ol_display_glass_sample_halo_ex(160, 300, 12, &m);
        if (!(n == 1 && one[0].y0 == 140 - halo && one[0].y1 == 440 + halo)) extent_bad++;
        printf("     THICK 160x300 r12 at y 140: no-cut rows [%d, %d), halo %d\n",
               n ? one[0].y0 : -1, n ? one[0].y1 : -1, halo);
        n = wm_backdrop_nocut(5, &pl, 1, &R, one, 0, 2);
        if (!(n == 1 && one[0].y0 == 140 && one[0].y1 == 440)) extent_bad++;
        /* a rectangle that misses the box + halo adds nothing */
        struct drect far = { 0, 440 + halo, 800, 900 };
        if (wm_backdrop_nocut(5, &pl, 0, &far, one, 0, 2) != 0) extent_bad++;
    }
    char msg[160];
    snprintf(msg, sizeof msg, "%ld mismatched no-cut ranges over 20000 frames (%ld with regions)",
             extent_bad, with_regions);
    verdict(extent_bad == 0, "NO-CUT extent", msg);
    snprintf(msg, sizeof msg, "%ld of %ld band boundaries inside a region's sample extent (%ld multi-band splits)",
             cut_bad, checked, multi);
    verdict(cut_bad == 0 && multi > 1000, "NO-CUT region", msg);
    ol_display_destroy(&d);
    if (fails) { printf("wm_bands_backdrop_test: %d check(s) FAILED\n", fails); return 1; }
    printf("wm_bands_backdrop_test: all checks passed\n");
    return 0;
}
