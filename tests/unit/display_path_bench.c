/* Budget: ol_display_fill_path of a 40x24 r=8 rrect (solid, 100%) must cost at most
 * 4x ol_display_blend_round_rect on the same pixels, measured in this binary. */
#include <stdlib.h>
#include <string.h>
#include "bench_budget.h"
#include "openlogit_display_path.h"

static void *t_alloc(unsigned long n) { return malloc(n); }
static void t_free(void *p) { free(p); }

static struct ol_display d;
static struct gfx_path path;
static struct gfx_paint paint;

static void ref_fn(void *a) { (void)a; ol_display_blend_round_rect(&d, 20, 10, 40, 24, 8, 0x20, 0x60, 0xc0, 255); bb_sink += d.target.px[500]; }
static void cand_fn(void *a) { (void)a; ol_display_fill_path(&d, &path, GFX_NONZERO, &paint, 255, 0); bb_sink += d.target.px[500]; }

int main(void)
{
    static uint32_t px[128 * 64];
    static int pt[512], sb[8];
    struct ol_pixel_target t = { px, 128, 64, 0, 0, 0, 0, 0 };
    d.allocate = t_alloc; d.release = t_free;
    ol_display_bind(&d, &t, 16, 8, 0);
    gfx_path_init(&path, pt, 256, sb, 8);
    gfx_path_rrect(&path, 20 * 256, 10 * 256, 40 * 256, 24 * 256, 8 * 256);
    gfx_paint_solid(&paint, 0x2060c0, 255);
    struct bb_stats ref = bb_measure(ref_fn, 0, 2000);
    struct bb_stats cand = bb_measure(cand_fn, 0, 2000);
    printf("ref %.0f ns, cand %.0f ns\n", ref.median_ns, cand.median_ns);
    bb_ratio("fill_path-rrect-40x24", cand, ref, 4.0);
    return bb_exit();
}
