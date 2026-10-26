/* Budget for ol_display snapshot groups, against the plain content blit they
 * wrap (the WM compositing one window: a 1280x800 ol_display_blit_surface).
 *
 *   display-group-corners   opacity 255, radius 24 (the corner tiles only):
 *                           <= 1.10x the plain blit
 *   display-group-opacity   opacity 160 (full-rect snapshot and blend):
 *                           <= 3.0x the plain blit
 *
 * Each call composites the next of DGB_FRAMES destination/source pairs and
 * the pairing rotates, so the destination before a blit always holds another
 * image and the blend really mixes two values (an unchanged pixel takes a
 * shortcut in the scalar path). An "info" line splits the opacity group into
 * its parts -- begin (the snapshot: one frame copy) and end (the blend) --
 * measured around no children, so a failing ratio says which part grew. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "bench_budget.h"
#include "openlogit_display.h"
#include "openlogit_display_group.h"

#define BW 1280
#define BH 800
#define DGB_FRAMES 4

static struct ol_display d;
static struct ol_pixel_target src[DGB_FRAMES];
static uint32_t *dst[DGB_FRAMES];
static uint32_t *snap;
static unsigned long snap_n;
static unsigned long calls;
static long failures;
static const int r24[4] = { 24, 24, 24, 24 };

static void *b_alloc(unsigned long n) { return malloc(n); }
static void b_free(void *p) { free(p); }

/* Bind the next destination; return the source to blit into it. */
static const struct ol_pixel_target *next_pair(void)
{
    int k = (int)(calls % DGB_FRAMES);
    int s = (int)((calls + calls / DGB_FRAMES + 1) % DGB_FRAMES);
    calls++;
    struct ol_pixel_target t = { dst[k], BW, BH, 0, 0, 0, 0, 0 };
    if (ol_display_bind(&d, &t, 16, 8, 0) != OL_OK) failures++;
    return &src[s];
}

static void plain(void *arg)
{
    (void)arg;
    ol_display_blit_surface(&d, 0, 0, next_pair());
    bb_sink += d.target.px[(BH / 2) * BW + BW / 2];
}

static void grouped(int opacity, int children)
{
    const struct ol_pixel_target *s = next_pair();
    struct ol_display_group g;
    if (ol_display_group_begin(&d, &g, 0, 0, BW, BH, r24, opacity, snap, snap_n) != OL_OK) failures++;
    if (children) ol_display_blit_surface(&d, 0, 0, s);
    if (ol_display_group_end(&d, &g) != OL_OK) failures++;
    bb_sink += d.target.px[(BH / 2) * BW + BW / 2];
}

static void corners(void *arg) { (void)arg; grouped(255, 1); }
static void opacity(void *arg) { (void)arg; grouped(160, 1); }
static void opacity_empty(void *arg) { (void)arg; grouped(160, 0); }

int main(void)
{
    unsigned s = 12345;
    for (int k = 0; k < DGB_FRAMES; k++) {
        src[k].px = malloc((size_t)BW * BH * 4);
        src[k].w = BW;
        src[k].h = BH;
        dst[k] = malloc((size_t)BW * BH * 4);
        for (int i = 0; i < BW * BH; i++) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            src[k].px[i] = s & 0xFFFFFFu;
            dst[k][i] = (s * 2654435761u) & 0xFFFFFFu;
        }
    }
    memset(&d, 0, sizeof d);
    d.allocate = b_alloc;
    d.release = b_free;
    snap_n = ol_display_group_bytes(BW, BH, r24, 160);
    snap = malloc(snap_n);

    struct bb_stats ref = bb_measure(plain, 0, 20);
    struct bb_stats c = bb_measure(corners, 0, 20);
    struct bb_stats o = bb_measure(opacity, 0, 20);
    struct bb_stats e = bb_measure(opacity_empty, 0, 20);
    printf("info: plain blit %.1f us, corner group %.1f us, opacity group %.1f us, "
           "opacity group without children (snapshot + blend) %.1f us (medians)\n",
           ref.median_ns / 1e3, c.median_ns / 1e3, o.median_ns / 1e3, e.median_ns / 1e3);
    bb_ratio("display-group-corners", c, ref, 1.10);
    bb_ratio("display-group-opacity", o, ref, 3.0);
    bb_count("display-group-status-failures", failures, 0);
    ol_display_destroy(&d);
    return bb_exit();
}
