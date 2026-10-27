/* Legacy gui_* packed coordinates are SIGNED 16-bit (gui-legacy-signed).
 *
 * Part 1 (FAIL signed): the real ring-3 encoders (c/apps/logit.h and
 * openlogit_wire.h, routed through the LOGIT_SYS_HOOK seam) and the generated
 * decoders (include/abi/logit_pack.h) round-trip x,y in {-5,-1,0,32767,-32768}
 * for RECT, RRECT, TEXT, TEXT_MONO, ICON, GLASS, CLIP and FLUSH_RECT. Under
 * -DGUI_LEGACY_UNSIGNED_NEGCTL the decode is the old unsigned mask, and -5 must
 * come back as 65531.
 * Part 2 (FAIL clip*): wm.c's SYS_GUI_CLIP case, transcribed with the generated
 * macros, drives the real fb.c: a clip at (-10,-10,50,50) intersects, an empty
 * clip draws nothing and is not a reset, ol_window_reset_clip resets.
 * Exit 1 on any failure. */
#define LOGIT_SYS_HOOK gls_sys_hook
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "fb.h"
#include "logit.h"
#include "openlogit_window.h"
#include "logit_pack.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
void  vmm_map_range(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{ (void)a; (void)b; (void)c; (void)d; }
int       virtio_gpu_init(void)   { return -1; }
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

#define W 120
#define H 90
static uint32_t px_surf[W * H];
static struct surface tgt;
static int fails;

/* The decode under test. The control swaps in the pre-change unsigned mask. */
#ifdef GUI_LEGACY_UNSIGNED_NEGCTL
#define DEC(v, sh) ((int)(((unsigned long long)(v) >> (sh)) & 0xFFFFULL))
#define DX(M, v) DEC(v, 16)
#define DY(M, v) DEC(v, 0)
#else
#define DX(M, v) M##_X(v)
#define DY(M, v) M##_Y(v)
#endif

static long cap_a, cap_b, cap_c, cap_n;
static int clip_mode;   /* 1: behave like wm.c for SYS_GUI_CLIP */
long gls_sys_hook(long n, long a, long b, long c)
{
    cap_n = n; cap_a = a; cap_b = b; cap_c = c;
    if (clip_mode && n == SYS_GUI_CLIP) {
        /* wm.c SYS_GUI_CLIP, with S() the identity at 100% */
        int x = fb_pt(LOGIT_GUI_CLIP_A_X(a)), y = fb_pt(LOGIT_GUI_CLIP_A_Y(a));
        int cw = LOGIT_GUI_CLIP_B_W(b), ch = LOGIT_GUI_CLIP_B_H(b);
        fb_target(&tgt);
        if (cw == 0 && ch == 0) fb_clear_clip();
        else fb_set_clip(x, y, fb_pt(LOGIT_GUI_CLIP_A_X(a) + cw) - x,
                               fb_pt(LOGIT_GUI_CLIP_A_Y(a) + ch) - y);
        fb_target(NULL);
    }
    return 0;
}

static void ck(int ok, const char *tag, const char *what, int got, int want)
{
    if (ok) return;
    printf("FAIL %s: %s got %d want %d\n", tag, what, got, want);
    fails++;
}

static const int XS[] = { -5, -1, 0, 32767, -32768 };

static void roundtrip(void)
{
    for (unsigned i = 0; i < sizeof XS / sizeof *XS; i++) {
        int x = XS[i], y = XS[(i + 2) % 5];
        int gx, gy;
#define CHK(call, SYM, M, argreg) \
        call; gx = DX(M, argreg); gy = DY(M, argreg); \
        ck(cap_n == SYM && gx == x && gy == y, "signed", #SYM, gx, x); \
        ck(gy == y, "signed", #SYM " y", gy, y)
        CHK(ol_window_rect(x, y, 7, 9, 0xFF0000), SYS_GUI_RECT, LOGIT_GUI_RECT_A, cap_a);
        CHK(ol_window_rrect(x, y, 7, 9, 3, 0xFF0000), SYS_GUI_RRECT, LOGIT_GUI_RRECT_A, cap_a);
        CHK(ol_window_text(x, y, 0xFFFFFF, "t"), SYS_GUI_TEXT, LOGIT_GUI_TEXT_A, cap_a);
        CHK(ol_window_text_mono(x, y, 0xFFFFFF, 8, "t"), SYS_GUI_TEXT_MONO, LOGIT_GUI_TEXT_MONO_A, cap_a);
        CHK(ol_window_icon(1, x, y, 16, 0xFFFFFF), SYS_GUI_ICON, LOGIT_GUI_ICON_A, cap_a);
        CHK(ol_window_glass(x, y, 7, 9, 3, 1, 2, 3, 4), SYS_GUI_GLASS, LOGIT_GUI_GLASS_A, cap_a);
        CHK(ol_window_flush_rect(x, y, 7, 9), SYS_GUI_FLUSH_RECT, LOGIT_GUI_FLUSH_RECT_A, cap_a);
        CHK(gui_clip(x, y, 7, 9), SYS_GUI_CLIP, LOGIT_GUI_CLIP_A, cap_a);
        /* the unsigned halves are untouched */
        ck(LOGIT_GUI_CLIP_B_W(cap_b) == 7 && LOGIT_GUI_CLIP_B_H(cap_b) == 9, "signed",
           "CLIP w/h stay unsigned", LOGIT_GUI_CLIP_B_W(cap_b), 7);
    }
    ol_window_rect(1, 2, 65535, 40000, 0);
    ck(LOGIT_GUI_RECT_B_W(cap_b) == 65535 && LOGIT_GUI_RECT_B_H(cap_b) == 40000, "signed",
       "RECT w/h are not sign-extended", LOGIT_GUI_RECT_B_W(cap_b), 65535);
}

static int count_painted(uint32_t col, int *minx, int *miny, int *maxx, int *maxy)
{
    int n = 0; *minx = *miny = 1 << 20; *maxx = *maxy = -1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
        if (px_surf[y * W + x] == col) {
            n++;
            if (x < *minx) *minx = x; if (x > *maxx) *maxx = x;
            if (y < *miny) *miny = y; if (y > *maxy) *maxy = y;
        }
    return n;
}

static void fill_all(void)
{
    fb_target(&tgt); fb_fill_rect(-20, -20, W + 40, H + 40, fb_rgb(255, 0, 0)); fb_target(NULL);
}

static void clip_tests(void)
{
    uint32_t red = fb_rgb(255, 0, 0), bg = fb_rgb(1, 2, 3);
    int a, b, c, d, n;
    clip_mode = 1;
    tgt.px = px_surf; tgt.w = W; tgt.h = H; tgt.clip_on = 0;

    for (int i = 0; i < W * H; i++) px_surf[i] = bg;
    gui_clip(-10, -10, 50, 50);
    fill_all(); gui_clip(0, 0, 0, 0);
    n = count_painted(red, &a, &b, &c, &d);
    ck(n == 40 * 40 && a == 0 && b == 0 && c == 39 && d == 39, "clip",
       "(-10,-10,50,50) paints exactly 0..39 square", n, 40 * 40);

    for (int i = 0; i < W * H; i++) px_surf[i] = bg;
    int rc = ol_window_clip_region((struct gfx_rect){ 5, 5, 0, 0 });
    ck(rc == OL_OK, "clip-empty", "empty region status", rc, OL_OK);
    fill_all();
    n = count_painted(red, &a, &b, &c, &d);
    ck(n == 0, "clip-empty", "empty clip admits no pixel", n, 0);
    ol_window_reset_clip();
    fill_all();
    n = count_painted(red, &a, &b, &c, &d);
    ck(n == W * H, "clip-reset", "reset restores full drawing", n, W * H);

    ck(ol_window_clip_region((struct gfx_rect){ -32769, 0, 1, 1 }) == OL_ARGUMENT, "clip-range",
       "x below -32768 rejected", 0, OL_ARGUMENT);
    ck(ol_window_clip_region((struct gfx_rect){ 0, 0, -1, 1 }) == OL_ARGUMENT, "clip-range",
       "negative width rejected", 0, OL_ARGUMENT);
    ol_window_reset_clip();
}

static uint8_t mbi[128];
int main(void)
{
    memset(mbi, 0, sizeof mbi);
    *(uint32_t *)mbi = sizeof mbi;
    uint8_t *p = mbi + 8;
    *(uint32_t *)(p + 0) = 8; *(uint32_t *)(p + 4) = 40;
    *(uint64_t *)(p + 8) = (uint64_t)(uintptr_t)px_surf;
    *(uint32_t *)(p + 16) = W * 4; *(uint32_t *)(p + 20) = W; *(uint32_t *)(p + 24) = H;
    p[28] = 32; p[29] = 1; p[32] = 16; p[33] = 8; p[34] = 8; p[35] = 8; p[36] = 0; p[37] = 8;
    if (!fb_init((uint64_t)(uintptr_t)mbi)) { printf("FAIL setup: fb_init\n"); return 2; }

    roundtrip();
    clip_tests();
    if (fails) { printf("gui_legacy_signed_test: %d FAILED\n", fails); return 1; }
    printf("gui_legacy_signed_test: all checks passed\n");
    return 0;
}
