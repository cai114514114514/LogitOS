/* Budget: one backdrop region per frame -- wm_backdrop_composite (the
 * material over the desktop) plus wm_backdrop_blit_content (the ink blit with
 * the marker test) over the region's rect -- costs at most 1.2x
 * ol_display_liquid_glass_ex on the same rect with the same material. The
 * region machinery must add bookkeeping, not a second pass over the glass. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "wm_draw2.h"
#include "wm_cover.h"
#include "openlogit_glass_material.h"
#include "bench_budget.h"

int (*gfx_ctx_hook)(void);

enum { BW = 1280, BH = 800, WX = 300, WY = 150, SW = 700, SH = 500, RW = 200 };
static uint32_t back[BW * BH], surf[SW * SH];
static struct ol_display bd, sd;
static struct drect R;
static struct ol_glass_material mat;
static struct ol_pixel_target src;
static int copyin(void *d, const void *s, unsigned long n) { memcpy(d, s, n); return 0; }

static void region_frame(void *arg)
{
    (void)arg;
    wm_backdrop_composite(&R, 1, WX, WY, &bd);
    wm_backdrop_blit_content(&R, 1, WX, WY, &bd, &src);
}
static void glass_only(void *arg)
{
    (void)arg;
    ol_display_liquid_glass_ex(&bd, R.x0, R.y0, R.x1 - R.x0, R.y1 - R.y0, 12, OL_GLASS_CUT_LEFT, &mat);
}

int main(void)
{
    uint32_t v = 11;
    for (int i = 0; i < BW * BH; i++) { v = v * 1664525u + 1013904223u; back[i] = (v >> 8) & 0xFFFFFF; }
    struct ol_pixel_target bt = { .px = back, .w = BW, .h = BH };
    struct ol_pixel_target st = { .px = surf, .w = SW, .h = SH };
    bd.allocate = sd.allocate = malloc;
    bd.release = sd.release = free;
    if (ol_display_bind(&bd, &bt, 16, 8, 0) != OL_OK || ol_display_bind(&sd, &st, 16, 8, 0) != OL_OK) return 2;
    ol_display_clear(&sd, 0xF4F4F6);
    struct wm2_env e = { .d = &sd, .scale_pct = 100, .surf_w = SW, .surf_h = SH, .slot = 1, .copyin = copyin };
    struct logit_backdrop2 b;
    memset(&b, 0, sizeof b);
    b.size = sizeof b;
    b.w = RW << 8; b.h = SH << 8;
    b.radius[1] = b.radius[2] = 12 << 8;
    b.tint_argb = 0xF0F2F8; b.opacity = 96; b.fill_rgb = 0xE6E6EC;
    if (wm2_backdrop(&e, (long)&b, 0, 0) != OL_OK) return 2;
    /* some app ink inside the sidebar, so the marker test has both outcomes */
    for (int y = 40; y < SH; y += 24) ol_display_fill_rect(&sd, 16, y, 120, 10, 0x202024);
    mat.tint = 0xF0F2F8; mat.opacity = 96;
    R.x0 = WX; R.y0 = WY; R.x1 = WX + RW; R.y1 = WY + SH;
    src.px = surf; src.w = SW; src.h = SH;
    bd.target.clip_on = 1;
    bd.target.clx0 = R.x0; bd.target.cly0 = R.y0; bd.target.clx1 = R.x1; bd.target.cly1 = R.y1;

    struct bb_stats ref = bb_measure(glass_only, 0, 6);
    struct bb_stats cand = bb_measure(region_frame, 0, 6);
    printf("median ns per %dx%d region: liquid_glass_ex %.0f, region composite + ink blit %.0f\n",
           RW, SH, ref.median_ns, cand.median_ns);
    bb_ratio("backdrop region vs liquid_glass_ex", cand, ref, 1.2);
    ol_display_destroy(&bd);
    ol_display_destroy(&sd);
    return bb_exit();
}
