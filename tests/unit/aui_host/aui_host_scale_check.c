/* aui_host_pt is a COPY of fb_pt, because the harness must not link fb.c into
 * every scene binary. A copy is only safe if it cannot drift, so this links the
 * real c/kernel/gui/fb/fb.c (test-fb-clip's recipe) and asserts equality for
 * every p in [-2000, 2000] at 100/125/150/175/200/300%. fb_dev2pt, which the
 * transport's text measurement answers through, gets the same treatment. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "fb.h"
#include "aui_host.h"

#define W 64
#define H 48

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

static uint8_t mbi[128];
static uint32_t scratch_fb[W * H];

int main(void)
{
    memset(mbi, 0, sizeof mbi);
    *(uint32_t *)mbi = sizeof mbi;
    uint8_t *p = mbi + 8;
    *(uint32_t *)(p + 0) = 8;  *(uint32_t *)(p + 4) = 40;
    *(uint64_t *)(p + 8) = (uint64_t)(uintptr_t)scratch_fb;
    *(uint32_t *)(p + 16) = W * 4; *(uint32_t *)(p + 20) = W; *(uint32_t *)(p + 24) = H;
    p[28] = 32; p[29] = 1; p[32] = 16; p[33] = 8; p[34] = 8; p[35] = 8; p[36] = 0; p[37] = 8;
    if (!fb_init((uint64_t)(uintptr_t)mbi)) { printf("FAIL fb_init refused the synthetic framebuffer tag\n"); return 1; }

    static const int pcts[] = { 100, 125, 150, 175, 200, 300 };
    long checked = 0, bad = 0;
    for (unsigned i = 0; i < sizeof pcts / sizeof pcts[0]; i++) {
        fb_set_scale(pcts[i]);
        if (fb_scale() != pcts[i]) { printf("FAIL fb_set_scale(%d) -> %d\n", pcts[i], fb_scale()); return 1; }
        for (int v = -2000; v <= 2000; v++) {
            int a = aui_host_pt(v, pcts[i]), b = fb_pt(v);
            int c = aui_host_dev2pt(v, pcts[i]), d = fb_dev2pt(v);
            checked++;
            if (a != b) { if (bad++ < 8) printf("FAIL pt %d%% p=%d host=%d fb=%d\n", pcts[i], v, a, b); }
            if (c != d) { if (bad++ < 8) printf("FAIL dev2pt %d%% p=%d host=%d fb=%d\n", pcts[i], v, c, d); }
        }
    }
    if (bad) { printf("FAIL scale: %ld mismatches in %ld comparisons\n", bad, checked); return 1; }
    printf("ok   aui_host_pt == fb_pt and aui_host_dev2pt == fb_dev2pt (%ld comparisons, 6 scales)\n", checked * 2);
    return 0;
}
