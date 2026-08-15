/* fb_blit_surface_scaled vs. fb_blit_surface_scaled_bl -- cost and a picture.
 *
 * This is not a pass/fail gate (there is no oracle for "looks less cheap");
 * it is the demonstration the bilinear path's CLAUDE.md unit asked for:
 * price both paths on a representative window-sized blit, and produce a
 * frame a human can look at that shows nearest's aliasing next to bilinear's
 * smoothing at the same scale. tests/unit/fb_clip_test.c established the
 * pattern this follows -- compile fb.c itself against host stubs for the six
 * externs it expects (virtio-gpu, kmalloc/kfree, text) so what is measured
 * and pictured is the REAL fb.c, not a reimplementation of it.
 *
 * Build + run (no make target; self-contained host program):
 *   cc -O2 -g -Wall -Wextra -o /tmp/fb_scale_bl_bench tests/unit/fb_scale_bl_bench.c \
 *      c/kernel/gui/fb.c -Ic/kernel/gui -Ic/drivers/virtio -Ic/kernel/mm \
 *      -Ic/lib/text && /tmp/fb_scale_bl_bench
 *
 * Writes two PPM files next to the binary's cwd: fb_scale_bl_compare.ppm (a
 * 1180x620 source scaled to ~0.4x by both paths, side by side) and
 * fb_scale_bl_zoom.ppm (a 4x nearest-upsampled crop of the seam between
 * them, so the difference is visible without squinting at a 472px image).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "fb.h"
#include "gfx.h"      /* GFX_MASK_* -- for the gfx_mask_corner stub's signature only */
#include "glass.h"    /* GLASS_E_MAX -- for the glass_disp/glass_fres stub arrays */

/* ---- the world fb.c expects, stubbed (identical shape to fb_clip_test.c) - */
void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
void  vmm_map_range(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{ (void)a; (void)b; (void)c; (void)d; }
int       virtio_gpu_init(void)   { return -1; }   /* force the multiboot path */
uint32_t *virtio_gpu_fb(void)     { return NULL; }
uint32_t  virtio_gpu_width(void)  { return 0; }
uint32_t  virtio_gpu_height(void) { return 0; }
int       virtio_gpu_present(void) { return 0; }
void      virtio_gpu_flush(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
int       virtio_gpu_cursor_ready(void) { return 0; }
int       virtio_gpu_cursor_define(const uint32_t *a, int w, int h, int hx, int hy)
{ (void)a; (void)w; (void)h; (void)hx; (void)hy; return -1; }
void      virtio_gpu_cursor_move(int x, int y) { (void)x; (void)y; }
void text_draw_sz(int x, int y, const char *s, int px, uint32_t c)
{ (void)x; (void)y; (void)s; (void)px; (void)c; }
int  text_width_sz(const char *s, int px) { (void)s; (void)px; return 0; }
int  text_line_height(int px) { return px; }

/* Open Logit's corner-mask cache and the liquid-glass refraction table
 * (gfx_mask.c / glass.c) -- fb.c's ONE translation unit references both, so
 * a link of fb.c alone needs symbols for them regardless of whether THIS
 * file's main() ever reaches fb_round_rect/fb_shadow/fb_liquid_glass (it
 * doesn't: the two functions under test, fb_blit_surface_scaled and
 * fb_blit_surface_scaled_bl, touch neither). Returning "refused"/zero is
 * correct for that reason -- not a shortcut, since nothing here is ever
 * exercised. */
const unsigned char *gfx_mask_corner(int kind, int w, int h, int param)
{ (void)kind; (void)w; (void)h; (void)param; return NULL; }
int gfx_shadow_falloff(long d256, long blur256) { (void)d256; (void)blur256; return 0; }
void glass_build_lut(int E, int refract) { (void)E; (void)refract; }
unsigned gl_isqrt(unsigned long v) { (void)v; return 0; }
int glass_disp[3][GLASS_E_MAX + 1];
unsigned char glass_fres[GLASS_E_MAX + 1];

/* ---- rdtsc: the SAME primitive kbench.h's kb_rdtsc() is (c/kernel/sched/
 * kbench.h) -- two 32-bit halves off EDX:EAX, no lfence (matching the
 * uninstrumented-cost measurements kbench.c takes, not ktime.c's serializing
 * one). Host-side cycles are not guest TCG cycles -- nothing in this tree
 * claims otherwise -- but the RATIO between two host-cycle counts of the same
 * two functions is exactly the number the unit asked for. */
#if defined(__x86_64__) || defined(__i386__)
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#else
static inline uint64_t rdtsc(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec; }
#endif

/* A 1180x620 window surface -- the browser/Finder canvas size CLAUDE.md's
 * unit named -- filled with two kinds of fine detail so a single still frame
 * can show the aliasing nearest produces on BOTH axes: 1px vertical stripes
 * (top half) and a diagonal fine pattern that is off-axis from both the row
 * and column sampling grid (bottom half), which is the case that shows up
 * worst under a single-tap resample and is exactly fixed by blending four. */
#define SW 1180
#define SH 620
static uint32_t srcpx[SW * SH];

static void build_source(void)
{
    for (int y = 0; y < SH; y++) {
        for (int x = 0; x < SW; x++) {
            uint8_t v;
            if (y < SH / 2) {
                v = (x & 1) ? 235 : 20;                 /* 1px vertical stripes */
            } else {
                v = ((x + y * 2) % 5 < 2) ? 235 : 20;    /* off-axis diagonal stripes */
            }
            srcpx[y * SW + x] = fb_rgb(v, v, v);
        }
    }
}

/* rpos/gpos/bpos were programmed by fb_init to 16/8/0 (0x00RRGGBB) below --
 * PPM wants R,G,B bytes in that order, so unpack locally rather than reach
 * into fb.c's static unpack(). */
static void unpack_rgb(uint32_t c, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = (uint8_t)(c >> 16);
    *g = (uint8_t)(c >> 8);
    *b = (uint8_t)c;
}

static void write_ppm(const char *path, const uint32_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint8_t r, g, b;
        unpack_rgb(px[i], &r, &g, &b);
        fputc(r, f); fputc(g, f); fputc(b, f);
    }
    fclose(f);
}

/* A framebuffer big enough to hold the widest destination this file draws
 * into (the side-by-side comparison canvas). */
#define FBW 1024
#define FBH 1024
static uint32_t fb_backing[FBW * FBH];
static uint8_t mbi[128];

static void init_fb(void)
{
    memset(mbi, 0, sizeof mbi);
    *(uint32_t *)mbi = sizeof mbi;
    uint8_t *p = mbi + 8;
    *(uint32_t *)(p + 0)  = 8;
    *(uint32_t *)(p + 4)  = 40;
    *(uint64_t *)(p + 8)  = (uint64_t)(uintptr_t)fb_backing;
    *(uint32_t *)(p + 16) = FBW * 4;
    *(uint32_t *)(p + 20) = FBW;
    *(uint32_t *)(p + 24) = FBH;
    p[28] = 32; p[29] = 1;
    p[32] = 16; p[33] = 8;
    p[34] = 8;  p[35] = 8;
    p[36] = 0;  p[37] = 8;
    if (!fb_init((uint64_t)(uintptr_t)mbi)) { fprintf(stderr, "fb_init failed\n"); exit(1); }
}

#define REPS 15

/* Min over REPS runs of the whole blit -- min, not mean, for the reason
 * kbench.c's KB_REPS comment gives: the host this runs on (and doubly so
 * under WSL2) is shared, so any run can be lengthened by a scheduler
 * preemption or a host interrupt but no run can be SHORTENED below the true
 * cost. The minimum is the only one of the three fbench.c/kbench.c usually
 * report that a single call site needs to make its point. */
static uint64_t time_blit(void (*fn)(int, int, int, int, const struct surface *),
                          int dw, int dh, const struct surface *src)
{
    struct surface dst = { .px = fb_backing, .w = FBW, .h = FBH, .clip_on = 0 };
    uint64_t best = ~0ull;
    for (int r = 0; r < REPS; r++) {
        fb_target(&dst);
        uint64_t t0 = rdtsc();
        fn(0, 0, dw, dh, src);
        uint64_t dt = rdtsc() - t0;
        if (dt < best) best = dt;
    }
    return best;
}

int main(void)
{
    init_fb();
    build_source();
    struct surface src = { .px = srcpx, .w = SW, .h = SH, .clip_on = 0 };

    printf("=== fb_blit_surface_scaled vs _bl: cost, %dx%d source, host rdtsc, min of %d ===\n",
           SW, SH, REPS);

    struct { const char *label; int dw, dh; } cases[] = {
        { "~0.3x (354x186)",  354,  186 },
        { "~1.5x (1770x930)", 1770, 930 },
    };
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        uint64_t cn = time_blit(fb_blit_surface_scaled,    cases[c].dw, cases[c].dh, &src);
        uint64_t cb = time_blit(fb_blit_surface_scaled_bl, cases[c].dw, cases[c].dh, &src);
        double ratio = cn ? (double)cb / (double)cn : 0.0;
        printf("  %-18s nearest %10llu cyc   bilinear %10llu cyc   bilinear/nearest = %.2fx\n",
               cases[c].label, (unsigned long long)cn, (unsigned long long)cb, ratio);
    }

    /* ---- the picture: same source, same ~0.4x scale, both paths, side by side */
#define CMP_DW 472    /* 1180 * 0.4 */
#define CMP_DH 248    /*  620 * 0.4 */
#define CMP_GAP 8
#define CMP_W (CMP_DW * 2 + CMP_GAP)
    int dw = CMP_DW, dh = CMP_DH, gap = CMP_GAP, cw = CMP_W;
    static uint32_t cmp[CMP_W * CMP_DH];
    for (int i = 0; i < cw * dh; i++) cmp[i] = fb_rgb(255, 0, 255);  /* magenta gutter */

    struct surface half = { .px = fb_backing, .w = FBW, .h = FBH, .clip_on = 0 };
    fb_target(&half);
    fb_blit_surface_scaled(0, 0, dw, dh, &src);
    for (int y = 0; y < dh; y++)
        memcpy(cmp + (size_t)y * cw, fb_backing + (size_t)y * FBW, (size_t)dw * 4);

    fb_target(&half);
    fb_blit_surface_scaled_bl(0, 0, dw, dh, &src);
    for (int y = 0; y < dh; y++)
        memcpy(cmp + (size_t)y * cw + dw + gap, fb_backing + (size_t)y * FBW, (size_t)dw * 4);

    write_ppm("fb_scale_bl_compare.ppm", cmp, cw, dh);
    printf("wrote fb_scale_bl_compare.ppm (%dx%d): LEFT nearest, RIGHT bilinear, both %dx%d from the same %dx%d source\n",
           cw, dh, dw, dh, SW, SH);

    /* A 4x nearest-upsampled crop straddling the vertical-stripe region
     * (rows 0..dh/2) so the aliasing is legible without a 1:1 pixel-peep. */
#define ZW 90
#define ZH 90
#define ZOOM 4
#define ZCW (ZW * ZOOM * 2 + CMP_GAP)
#define ZCH (ZH * ZOOM)
    int zx = 20, zy = 20, zw = ZW, zh = ZH, zoom = ZOOM, zcw = ZCW, zch = ZCH;
    static uint32_t zbuf[ZCW * ZCH];
    for (int i = 0; i < zcw * zch; i++) zbuf[i] = fb_rgb(255, 0, 255);
    for (int side = 0; side < 2; side++) {
        int srcx0 = side == 0 ? zx : dw + gap + zx;
        for (int y = 0; y < zh; y++) {
            for (int x = 0; x < zw; x++) {
                uint32_t p = cmp[(size_t)(zy + y) * cw + srcx0 + x];
                int ox = side * (zw * zoom + gap) + x * zoom;
                int oy = y * zoom;
                for (int dy2 = 0; dy2 < zoom; dy2++)
                    for (int dx2 = 0; dx2 < zoom; dx2++)
                        zbuf[(size_t)(oy + dy2) * zcw + (ox + dx2)] = p;
            }
        }
    }
    write_ppm("fb_scale_bl_zoom.ppm", zbuf, zcw, zch);
    printf("wrote fb_scale_bl_zoom.ppm (%dx%d): 4x nearest-upsampled crop, LEFT nearest RIGHT bilinear\n",
           zcw, zch);

    return 0;
}
