/* Budget: ol_display_fill_path of a rounded-rect path (solid, 100%) must cost at most
 * 2.0x the ordinary gfx surface path fill (gfx_fill into a same-size, same-format
 * surface with the same paint), measured in this binary. This isolates the display
 * adapter overhead. The old ol_display_blend_round_rect ratio is printed as info only
 * (it is now a dedicated analytic fast path, no longer a fair generic reference). */
#include <stdlib.h>
#include <string.h>
#include "bench_budget.h"
#include "openlogit_display_path.h"

static void *t_alloc(unsigned long n) { return malloc(n); }
static void t_free(void *p) { free(p); }

static struct ol_display d;
static struct gfx_path path;
static struct gfx_paint paint;
static struct gfx_surface surf;
static int bx, by, bw, bh, br;
static uint32_t spx[128 * 64];

static void ref_fn(void *a) { (void)a; gfx_fill(&surf, &path, GFX_NONZERO, &paint, 0); bb_sink += spx[500]; }
static void cand_fn(void *a) { (void)a; ol_display_fill_path(&d, &path, GFX_NONZERO, &paint, 255, 0); bb_sink += d.target.px[500]; }
static void info_fn(void *a) { (void)a; ol_display_blend_round_rect(&d, bx, by, bw, bh, br, 0x20, 0x60, 0xc0, 255); bb_sink += d.target.px[500]; }

int main(void)
{
    static uint32_t px[128 * 64];
    static int pt[512], sb[8];
    static const int sz[][3] = { { 40, 24, 8 }, { 64, 32, 10 }, { 100, 48, 12 }, { 24, 24, 12 } };
    struct ol_pixel_target t = { px, 128, 64, 0, 0, 0, 0, 0 };
    d.allocate = t_alloc; d.release = t_free;
    ol_display_bind(&d, &t, 16, 8, 0);
    gfx_surface_init(&surf, (unsigned char *)spx, 128, 64, 128 * 4);
    gfx_paint_solid(&paint, 0x2060c0, 255);
    for (unsigned i = 0; i < sizeof sz / sizeof sz[0]; i++) {
        char nm[48];
        bx = 10; by = 6; bw = sz[i][0]; bh = sz[i][1]; br = sz[i][2];
        gfx_path_init(&path, pt, 256, sb, 8);
        gfx_path_rrect(&path, bx * 256, by * 256, bw * 256, bh * 256, br * 256);
        struct bb_stats ref = bb_measure(ref_fn, 0, 2000);
        struct bb_stats cand = bb_measure(cand_fn, 0, 2000);
        struct bb_stats info = bb_measure(info_fn, 0, 2000);
        printf("rrect %dx%d r=%d: gfx_fill %.0f ns, display fill_path %.0f ns, blend_round_rect %.0f ns\n",
               bw, bh, br, ref.median_ns, cand.median_ns, info.median_ns);
        printf("INFO fill_path/blend_round_rect ratio=%.3f (not gated)\n",
               info.median_ns > 0 ? cand.median_ns / info.median_ns : 0.0);
        snprintf(nm, sizeof nm, "fill_path-vs-gfx_fill-%dx%d", bw, bh);
        bb_ratio(nm, cand, ref, 2.0);
    }
    return bb_exit();
}
