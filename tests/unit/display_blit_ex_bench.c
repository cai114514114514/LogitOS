/* Budget bench for ol_display_blit_rgba_ex (ratios measured in this binary). */
#include <stdlib.h>
#include <string.h>
#include "bench_budget.h"
#include "openlogit_display_blit.h"
#include "display_pixels.h"

int (*gfx_ctx_hook)(void);
static void *t_alloc(unsigned long n) { return malloc(n); }
static void t_free(void *p) { free(p); }

struct job {
    struct ol_display d; struct ol_pixel_target t;
    const uint8_t *rgba; int sw, sh, dw, dh; enum ol_filter f;
    const struct ol_pixel_target *psrc;
};
static void run_ex(void *a)
{
    struct job *j = a;
    ol_display_blit_rgba_ex(&j->d, j->rgba, j->sw, j->sh, j->sw * 4, 0, 0, j->sw, j->sh, 0, 0, j->dw * 256, j->dh * 256, 255, j->f);
}
static void run_bl(void *a)
{
    struct job *j = a;
    ol_display_blit_surface_scaled_bl(&j->d, 0, 0, j->dw, j->dh, j->psrc);
}
static void setup(struct job *j, int w, int h)
{
    memset(j, 0, sizeof *j);
    j->t.px = calloc((size_t)w * h, 4); j->t.w = w; j->t.h = h;
    j->d.allocate = t_alloc; j->d.release = t_free;
    ol_display_bind(&j->d, &j->t, 16, 8, 0);
}

int main(void)
{
    /* Budget 1: BOX 1536->64 reads every source pixel, so compare it with the
     * BILINEAR path working over the same number of pixels (1536->1536). */
    enum { S = 1536 };
    uint8_t *img = malloc((size_t)S * S * 4);
    for (long i = 0; i < (long)S * S * 4; i++) img[i] = (uint8_t)(i * 2654435761u >> 13);
    for (long i = 0; i < (long)S * S; i++) img[i * 4 + 3] = 255;
    struct job box, bil;
    setup(&box, 64, 64); setup(&bil, S, S);
    box.rgba = bil.rgba = img; box.sw = box.sh = bil.sw = bil.sh = S;
    box.dw = box.dh = 64; bil.dw = bil.dh = S;
    box.f = OL_FILTER_BOX; bil.f = OL_FILTER_BILINEAR;
    struct bb_stats sbox = bb_measure(run_ex, &box, 2), sbil = bb_measure(run_ex, &bil, 2);
    bb_ratio("blit-ex-box-1536to64-vs-bilinear-same-source-pixels", sbox, sbil, 1.5);

    /* Budget 2: RGBA BILINEAR vs ol_display_blit_surface_scaled_bl, equal pixels. */
    enum { N = 512, M = 768 };
    uint8_t *small = malloc((size_t)N * N * 4);
    uint32_t *packed = malloc((size_t)N * N * 4);
    for (long i = 0; i < (long)N * N; i++) {
        small[i * 4] = (uint8_t)(i * 7); small[i * 4 + 1] = (uint8_t)(i * 13 >> 3); small[i * 4 + 2] = (uint8_t)(i >> 4); small[i * 4 + 3] = 255;
        packed[i] = (uint32_t)small[i * 4] << 16 | (uint32_t)small[i * 4 + 1] << 8 | small[i * 4 + 2];
    }
    struct ol_pixel_target ps = { packed, N, N, 0, 0, 0, 0, 0 };
    struct job rb, sb;
    setup(&rb, M, M); setup(&sb, M, M);
    rb.rgba = small; rb.sw = rb.sh = N; rb.dw = rb.dh = M; rb.f = OL_FILTER_BILINEAR;
    sb.psrc = &ps; sb.dw = sb.dh = M;
    struct bb_stats srb = bb_measure(run_ex, &rb, 4), ssb = bb_measure(run_bl, &sb, 4);
    bb_ratio("blit-ex-rgba-bilinear-vs-surface-scaled-bl", srb, ssb, 1.3);
    return bb_exit();
}
