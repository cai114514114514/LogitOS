/* Open Logit -- what the engine costs, per primitive, measured.
 *
 * This is the HOST half of the cost story and it is the half that isolates the
 * engine: no syscalls, no compositor, no TCG. `make bench-gfx-frame` measures a
 * real frame on the machine, where the engine's share turns out to be small
 * next to gui_clear and text -- which is exactly why the two are separate
 * targets. Reporting only the frame total credits the rasterizer with a cost it
 * does not pay; reporting only this one pretends syscalls are free.
 *
 * The comparison that matters is the last one: the same rounded rect drawn as a
 * 9-slice (four cached corner tiles) and as one whole-path fill. That ratio IS
 * the O(r^2)-not-O(w*h) claim, in nanoseconds.
 *
 * Build: make bench-gfx
 */
#include <stdio.h>
#include <time.h>

#include "gfx.h"

static double now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static int PT[8192 * 2];
static int SUB[64];
static unsigned char MASK[512 * 512];
static unsigned char SURF[1280 * 64 * 4];
static unsigned char STRIP[2048 * 4];

static void path_begin(struct gfx_path *p)
{
    gfx_path_init(p, PT, 8192, SUB, 64);
    gfx_path_tolerance(p, GFX_ONE / 32);
}

static void report(const char *what, double us, int n, const char *unit)
{
    printf("  %-52s %9.2f us / %s\n", what, us / n, unit);
}

int main(void)
{
    struct gfx_path p;
    struct gfx_paint pt;
    struct gfx_surface s;
    double t0;
    int N;

    printf("=== Open Logit: per-primitive cost (host, -O2) ===\n\n");

    printf("-- corner tiles: a CACHE MISS, i.e. the rasterizer running --\n");
    for (int r = 8; r <= 48; r *= 2) {
        N = 20000 / r;
        t0 = now_us();
        for (int i = 0; i < N; i++) gfx_corner_fill(MASK, r, r);
        char b[64]; snprintf(b, sizeof b, "gfx_corner_fill r=%d (%d px tile)", r, r * r);
        report(b, now_us() - t0, N, "tile");
    }
    for (int r = 8; r <= 48; r *= 2) {
        N = 20000 / r;
        t0 = now_us();
        for (int i = 0; i < N; i++) gfx_corner_ring(MASK, r, r, 2);
        char b[64]; snprintf(b, sizeof b, "gfx_corner_ring  r=%d t=2", r);
        report(b, now_us() - t0, N, "tile");
    }
    N = 2000;
    t0 = now_us();
    for (int i = 0; i < N; i++) gfx_corner_shadow(MASK, 32, 32, 10);
    report("gfx_corner_shadow 32x32 r=10 (distance ramp)", now_us() - t0, N, "tile");

    printf("\n-- and a cache HIT, which is what the 2nd..nth control pays --\n");
    gfx_mask_corner(GFX_MASK_FILL, 8, 8, 0);
    N = 2000000;
    t0 = now_us();
    volatile const unsigned char *sink = 0;
    for (int i = 0; i < N; i++) sink = gfx_mask_corner(GFX_MASK_FILL, 8, 8, 0);
    (void)sink;
    report("gfx_mask_corner hit (16-slot linear scan)", now_us() - t0, N, "lookup");

    printf("\n-- expanding a tile for the blit --\n");
    N = 200000;
    t0 = now_us();
    for (int i = 0; i < N; i++)
        gfx_mask_to_rgba(SURF, MASK, 12, 12, 0x3366CC, 255, 1, 0);
    report("gfx_mask_to_rgba 12x12 mirrored", now_us() - t0, N, "tile");

    printf("\n-- gradient strips (one blit, the compositor replicates) --\n");
    N = 200000;
    t0 = now_us();
    for (int i = 0; i < N; i++) gfx_gradient_strip(STRIP, 64, 0x102030, 0xE0E0F0, 255);
    report("gfx_gradient_strip n=64", now_us() - t0, N, "strip");
    t0 = now_us();
    for (int i = 0; i < N; i++) gfx_gradient_strip(STRIP, 1200, 0x102030, 0xE0E0F0, 255);
    report("gfx_gradient_strip n=1200 (a full-height gradient)", now_us() - t0, N, "strip");

    printf("\n-- filling a paint through coverage into a surface --\n");
    gfx_surface_init(&s, SURF, 1280, 64, 1280 * 4);
    {
        struct { const char *name; int kind; } K[] = {
            { "solid",           GFX_SOLID },
            { "linear gradient", GFX_LINEAR },
            { "radial gradient", GFX_RADIAL },
            { "image (nearest)", GFX_IMAGE },
            { "image (bilinear)",GFX_IMAGE },
        };
        static unsigned char img[64 * 64 * 4];
        for (int i = 0; i < 64 * 64 * 4; i++) img[i] = (unsigned char)i;
        struct gfx_matrix m;
        for (unsigned k = 0; k < sizeof K / sizeof *K; k++) {
            if (K[k].kind == GFX_SOLID) gfx_paint_solid(&pt, 0x3366CC, 200);
            else if (K[k].kind == GFX_LINEAR) {
                gfx_paint_linear(&pt, 0, 0, GFX_PX(1280), 0);
                gfx_paint_stop(&pt, 0, 0x000000, 255);
                gfx_paint_stop(&pt, GFX_MONE, 0xFFFFFF, 255);
            } else if (K[k].kind == GFX_RADIAL) {
                gfx_paint_radial(&pt, GFX_PX(640), GFX_PX(32), GFX_PX(600));
                gfx_paint_stop(&pt, 0, 0x000000, 255);
                gfx_paint_stop(&pt, GFX_MONE, 0xFFFFFF, 255);
            } else {
                gfx_m_identity(&m);
                gfx_m_scale(&m, 4 * GFX_MONE, 4 * GFX_MONE);
                gfx_paint_image(&pt, img, 64, 64, 0, &m, k == 4);
            }
            path_begin(&p);
            gfx_path_rrect(&p, 0, 0, GFX_PX(1280), GFX_PX(64), GFX_PX(12));
            N = 200;
            t0 = now_us();
            for (int i = 0; i < N; i++) gfx_fill(&s, &p, GFX_NONZERO, &pt, 0);
            char b[80];
            snprintf(b, sizeof b, "gfx_fill 1280x64 rrect, %s", K[k].name);
            report(b, now_us() - t0, N, "fill (81920 px)");
        }
    }

    printf("\n-- THE 9-SLICE CLAIM: O(r^2), not O(w*h) --\n");
    {
        /* A 200x40 card with r=8: the four cached corner tiles are all the
         * rasterizer ever sees; the three interior bands are opaque rect calls
         * the compositor does. Against that, the same shape rasterized whole. */
        N = 100000;
        t0 = now_us();
        for (int i = 0; i < N; i++) {
            gfx_mask_corner(GFX_MASK_FILL, 8, 8, 0);
            gfx_mask_to_rgba(SURF, MASK, 8, 8, 0xFFFFFF, 255, 0, 0);
            gfx_mask_to_rgba(SURF, MASK, 8, 8, 0xFFFFFF, 255, 1, 0);
            gfx_mask_to_rgba(SURF, MASK, 8, 8, 0xFFFFFF, 255, 0, 1);
            gfx_mask_to_rgba(SURF, MASK, 8, 8, 0xFFFFFF, 255, 1, 1);
        }
        double slice = (now_us() - t0) / N;
        printf("  %-52s %9.3f us\n", "200x40 r=8 as a 9-slice (4 cached tiles)", slice);

        gfx_surface_init(&s, SURF, 200, 40, 200 * 4);
        gfx_paint_solid(&pt, 0xFFFFFF, 255);
        path_begin(&p);
        gfx_path_rrect(&p, 0, 0, GFX_PX(200), GFX_PX(40), GFX_PX(8));
        N = 20000;
        t0 = now_us();
        for (int i = 0; i < N; i++) gfx_fill(&s, &p, GFX_NONZERO, &pt, 0);
        double whole = (now_us() - t0) / N;
        printf("  %-52s %9.3f us\n", "200x40 r=8 as one whole-path fill", whole);
        printf("  %-52s %9.1fx\n", "ratio", whole / slice);
        printf("\n  The 9-slice does not draw fewer pixels -- the compositor still\n"
               "  fills the interior. It rasterizes fewer: 256 blended pixels\n"
               "  instead of 8000, and zero of them on the second card.\n");
    }

    printf("\n-- path construction (flattening is where curves are paid for) --\n");
    N = 100000;
    t0 = now_us();
    for (int i = 0; i < N; i++) {
        path_begin(&p);
        gfx_path_rrect(&p, 0, 0, GFX_PX(200), GFX_PX(40), GFX_PX(8));
    }
    printf("  %-52s %9.3f us (%d pts)\n", "build a 200x40 r=8 rounded rect",
           (now_us() - t0) / N, p.npt);
    t0 = now_us();
    for (int i = 0; i < N; i++) {
        path_begin(&p);
        gfx_path_circle(&p, GFX_PX(100), GFX_PX(100), GFX_PX(96));
    }
    printf("  %-52s %9.3f us (%d pts)\n", "build an r=96 circle",
           (now_us() - t0) / N, p.npt);

    int hits, misses;
    gfx_mask_stats(&hits, &misses);
    printf("\nmask cache over the whole run: %d hits, %d misses\n", hits, misses);
    return 0;
}
