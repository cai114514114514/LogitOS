/* The one property every partial frame rests on:
 *
 *     drawing X with a clip rectangle C produces exactly the pixels that
 *     drawing X with no clip would have produced INSIDE C, and leaves every
 *     pixel outside C exactly as it found them.
 *
 * The compositor now draws the entire scene clipped to a damage rectangle. If
 * any primitive is off by a pixel at the clip edge, or writes one pixel past
 * it, or computes a gradient row / a rounded corner / a glyph's coverage from
 * the CLIPPED index instead of the shape's own index, the result is a seam or a
 * smear -- and on a desktop it looks like a compositor bug rather than like the
 * one primitive that got it wrong. Bisecting that from a screenshot is
 * miserable. This asks each primitive the question directly.
 *
 * The interesting failure is not "it drew outside the clip". It is that
 * clamping a loop to the clip is only equivalent to testing every pixel when
 * the shape-local index is preserved -- fb_fill_vgrad's colour comes from j,
 * inside_round's corner comes from (i,j), fb_blit_glyph's alpha comes from
 * cov[j*w+i]. Rebasing j to the clip is the natural mistake and it shifts a
 * gradient or a corner by however much was clipped away.
 *
 * fb_blur_rect and fb_liquid_glass read a NEIGHBOURHOOD, so equality only holds
 * when they are given identical input; each case starts from a fresh copy of
 * the same scene, which is exactly the guarantee wm.c gives them (a damage
 * rectangle always contains a whole glass panel).
 *
 * Build + run (no make target; this is a self-contained host test):
 *   cc -O1 -g -Wall -Wextra -o /tmp/fb_clip_test tests/unit/fb_clip_test.c \
 *      c/kernel/gui/fb.c -Ic/kernel/gui -Ic/drivers/virtio -Ic/kernel/mm \
 *      -Ic/lib/text && /tmp/fb_clip_test
 * tests/qmp/qmp_damage.py runs it as its first step.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "fb.h"

#define W 200
#define H 140

/* ---- the world fb.c expects, stubbed ------------------------------------ */
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
/* Text is a separate layer with its own tests; fb.c only forwards to it, and
 * every glyph it ever paints arrives through fb_blit_glyph, which IS tested
 * here. Stubbing keeps this test about clipping. */
void text_draw_sz(int x, int y, const char *s, int px, uint32_t c)
{ (void)x; (void)y; (void)s; (void)px; (void)c; }
int  text_width_sz(const char *s, int px) { (void)s; (void)px; return 0; }
int  text_line_height(int px) { return px; }

static int fails;
static void ck(int cond, const char *what)
{
    printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) fails++;
}

/* A multiboot2 info block with just a framebuffer tag, so fb_init() takes the
 * LFB path and the real channel positions get programmed. */
static uint8_t mbi[128];
static uint32_t scratch_fb[W * H];
static void init_fb(void)
{
    memset(mbi, 0, sizeof mbi);
    *(uint32_t *)mbi = sizeof mbi;                 /* total_size */
    uint8_t *p = mbi + 8;
    *(uint32_t *)(p + 0)  = 8;                     /* type: framebuffer */
    *(uint32_t *)(p + 4)  = 40;                    /* size */
    *(uint64_t *)(p + 8)  = (uint64_t)(uintptr_t)scratch_fb;
    *(uint32_t *)(p + 16) = W * 4;                 /* pitch */
    *(uint32_t *)(p + 20) = W;
    *(uint32_t *)(p + 24) = H;
    p[28] = 32;                                    /* bpp */
    p[29] = 1;                                     /* RGB */
    p[32] = 16; p[33] = 8;                         /* red   pos/size */
    p[34] = 8;  p[35] = 8;                         /* green */
    p[36] = 0;  p[37] = 8;                         /* blue  */
    if (!fb_init((uint64_t)(uintptr_t)mbi)) {
        printf("FAIL fb_init refused the synthetic framebuffer tag\n");
        exit(1);
    }
}

/* A deterministic starting picture, so "unchanged outside the clip" is a real
 * statement about pixels and not about a field of zeroes. */
static void scene(uint32_t *px)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            px[y * W + x] = fb_rgb((uint8_t)(x * 7 + y * 3),
                                   (uint8_t)(y * 5 + 11),
                                   (uint8_t)(x * 3 + y * 11));
}

static uint32_t ref[W * H], got[W * H], base[W * H];
static struct surface tgt;

/* clip rectangle, chosen to cut every test shape rather than contain it */
#define CX 37
#define CY 29
#define CW 84
#define CH 61

typedef void (*draw_fn)(void);

static void compare(const char *what)
{
    int inside_bad = 0, outside_bad = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int in = (x >= CX && x < CX + CW && y >= CY && y < CY + CH);
            if (in) {
                if (got[y * W + x] != ref[y * W + x]) inside_bad++;
            } else {
                if (got[y * W + x] != base[y * W + x]) outside_bad++;
            }
        }
    char msg[256];
    snprintf(msg, sizeof msg,
             "%-22s clipped == unclipped inside the clip (%d wrong), and nothing "
             "written outside it (%d written)", what, inside_bad, outside_bad);
    ck(inside_bad == 0 && outside_bad == 0, msg);
}

static void run(const char *what, draw_fn fn)
{
    scene(base);

    memcpy(ref, base, sizeof ref);
    tgt.px = ref; tgt.w = W; tgt.h = H; tgt.clip_on = 0;
    fb_target(&tgt);
    fn();

    memcpy(got, base, sizeof got);
    tgt.px = got; tgt.w = W; tgt.h = H; tgt.clip_on = 0;
    fb_target(&tgt);
    fb_set_clip(CX, CY, CW, CH);
    fn();
    fb_clear_clip();
    fb_target(NULL);

    compare(what);
}

/* ---- the shapes ---------------------------------------------------------- */
/* Each straddles the clip on every side it can, so a one-pixel edge error and a
 * shape-index rebase both show up. */

static void d_fill_rect(void)   { fb_fill_rect(20, 15, 150, 100, fb_rgb(255, 0, 0)); }
static void d_round_rect(void)  { fb_round_rect(20, 15, 150, 100, 24, fb_rgb(0, 255, 0)); }
static void d_circle(void)      { fb_fill_circle(80, 60, 55, fb_rgb(0, 0, 255)); }
static void d_vgrad(void)       { fb_fill_vgrad(20, 15, 150, 100, fb_rgb(255, 0, 0), fb_rgb(0, 0, 255)); }
static void d_rr_vgrad(void)    { fb_round_rect_vgrad(20, 15, 150, 100, 24, fb_rgb(9, 200, 30), fb_rgb(240, 12, 90)); }
static void d_blend_rect(void)  { fb_blend_rect(20, 15, 150, 100, 250, 40, 10, 137); }
static void d_blend_rr(void)    { fb_blend_round_rect(20, 15, 150, 100, 24, 10, 240, 60, 91); }
static void d_blur(void)        { fb_blur_rect(20, 15, 150, 100, 6, 20); }
static void d_glass(void)       { fb_liquid_glass(20, 15, 150, 100, 24, 250, 250, 255, 110); }
static void d_clear(void)       { fb_clear(fb_rgb(3, 5, 7)); }

/* Negative offsets too: a window dragged off the left edge has negative
 * coordinates, and that path used to be the one with the `continue`s in it. */
static void d_offscreen(void)   { fb_fill_rect(-30, -20, 90, 80, fb_rgb(200, 200, 0)); }

static uint32_t src_px[64 * 48];
static struct surface src;
static void d_blit_surface(void)
{ fb_blit_surface(25, 20, &src); }
static void d_blit_surface_neg(void)
{ fb_blit_surface(-11, -7, &src); }
static void d_blit_scaled(void)
{ fb_blit_surface_scaled(18, 12, 151, 103, &src); }

static uint8_t cov[90 * 70];
static void d_glyph(void)
{ fb_blit_glyph(24, 18, cov, 90, 70, fb_rgb(255, 255, 255)); }

static uint8_t rgba[40 * 30 * 4];
static void d_blit_rgba(void)
{ fb_blit_rgba(22, 17, 146, 99, rgba, 40, 30); }

int main(void)
{
    init_fb();

    for (int i = 0; i < 64 * 48; i++)
        src_px[i] = fb_rgb((uint8_t)(i * 13), (uint8_t)(i * 7), (uint8_t)(i * 3));
    src.px = src_px; src.w = 64; src.h = 48; src.clip_on = 0;

    for (int i = 0; i < 90 * 70; i++) cov[i] = (uint8_t)((i * 37) & 0xFF);
    for (int i = 0; i < 40 * 30; i++) {
        rgba[i * 4 + 0] = (uint8_t)(i * 5);
        rgba[i * 4 + 1] = (uint8_t)(i * 9);
        rgba[i * 4 + 2] = (uint8_t)(i * 3);
        rgba[i * 4 + 3] = (uint8_t)((i * 17) & 0xFF);   /* every alpha, incl. 0 and 255 */
    }

    printf("=== fb clip equality: %dx%d target, clip (%d,%d %dx%d) ===\n",
           W, H, CX, CY, CW, CH);
    run("fb_fill_rect",            d_fill_rect);
    run("fb_fill_rect off-screen", d_offscreen);
    run("fb_round_rect",           d_round_rect);
    run("fb_fill_circle",          d_circle);
    run("fb_fill_vgrad",           d_vgrad);
    run("fb_round_rect_vgrad",     d_rr_vgrad);
    run("fb_blend_rect",           d_blend_rect);
    run("fb_blend_round_rect",     d_blend_rr);
    run("fb_blit_surface",         d_blit_surface);
    run("fb_blit_surface neg",     d_blit_surface_neg);
    run("fb_blit_surface_scaled",  d_blit_scaled);
    run("fb_blit_glyph",           d_glyph);
    run("fb_blit_rgba",            d_blit_rgba);
    run("fb_blur_rect",            d_blur);
    run("fb_liquid_glass",         d_glass);
    run("fb_clear",                d_clear);

    /* The present accounting is charged for what was really copied, not for
     * what was asked for -- a compositor that presents a rectangle hanging off
     * the edge must not be credited with the overhang. */
    uint64_t p0 = fb_present_px();
    fb_set_backbuffer(scratch_fb);
    fb_present_rect(-10, -10, 30, 30);
    ck(fb_present_px() - p0 == 20 * 20,
       "fb_present_px counts the CLAMPED rectangle (20x20 of a 30x30 ask at -10,-10)");
    p0 = fb_present_px();
    fb_present_rect(W - 5, H - 5, 40, 40);
    ck(fb_present_px() - p0 == 5 * 5, "...and clamps at the far edge too");
    p0 = fb_present_px();
    fb_present_rect(10, 10, 0, 5);
    ck(fb_present_px() - p0 == 0, "...and an empty rectangle costs nothing");

    if (fails) {
        printf("\nfb_clip_test: %d FAILED\n", fails);
        return 1;
    }
    printf("\nfb_clip_test: all checks passed\n");
    return 0;
}
