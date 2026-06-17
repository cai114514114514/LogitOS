/* Host reproduction of the cross-app framebuffer clip leak (the "white Terminal"
 * Schroedinger bug). Compiles the REAL c/kernel/gui/fb.c.
 *
 * The clip rectangle is global state in fb.c. An app sets it via gui_clip(set)
 * and clears it later via gui_clip(clear) -- but those are two separate
 * syscalls, and the scheduler can switch to ANOTHER app in between. That app's
 * draw into its OWN window surface then inherits the first app's clip. The
 * Finder (files.c, always open) holds the clip across a long multi-syscall draw
 * loop, so a freshly-opened Terminal's gui_clear gets clipped and most of its
 * surface stays the white window-create fill -> a white Terminal.
 *
 * This test reproduces that: set a clip while targeting surface A, then clear
 * surface B (a different app's window) -- B must end up fully cleared.
 *   - BUG (global clip):  B is clipped to A's rect  -> FAIL
 *   - FIX (per-surface):  B is fully cleared        -> PASS
 */
#include "fb.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- stubs for fb.c's externs (the clip path itself calls none of them) ---- */
void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
int   text_draw(int x, int y, const char *u, uint32_t c) { (void)x;(void)y;(void)u;(void)c; return 0; }
int   text_width(const char *u) { (void)u; return 0; }
int   text_width_sz(const char *u, int px) { (void)u;(void)px; return 0; }
int   text_line_height(int px) { (void)px; return 16; }
int   virtio_gpu_init(void) { return -1; }
uint32_t *virtio_gpu_fb(void) { return 0; }
uint32_t  virtio_gpu_width(void) { return 0; }
uint32_t  virtio_gpu_height(void) { return 0; }
void      virtio_gpu_flush(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
void      vmm_map_range(uint64_t v,uint64_t p,uint64_t s,uint64_t f){(void)v;(void)p;(void)s;(void)f;}

#define WHITE 0x00FCFCFAu   /* the rgb(250,250,252) gui_create fill sentinel */
#define DARK  0x00121212u

static int checks, fails;
static void ok(int cond, const char *msg) {
    checks++;
    if (!cond) { fails++; printf("  FAIL: %s\n", msg); }
}

int main(void) {
    enum { N = 16 };
    uint32_t bufA[N*N], bufB[N*N];
    struct surface A = { .px = bufA, .w = N, .h = N }, B = { .px = bufB, .w = N, .h = N };
    for (int i = 0; i < N*N; i++) bufA[i] = bufB[i] = WHITE;

    /* App 1 (Finder): begins a clipped draw into ITS surface, then is preempted
     * BEFORE clearing the clip (the long draw_grid/draw_list loop). */
    fb_target(&A);
    fb_set_clip(5, 5, 3, 3);          /* keep only A[5..8) x [5..8) */

    /* App 2 (Terminal): first frame -- clears its WHOLE surface to dark. */
    fb_target(&B);
    fb_clear(DARK);

    /* The Terminal's surface must be fully dark: the Finder's clip must not
     * touch a different app's window surface. */
    int leaked = 0;
    for (int i = 0; i < N*N; i++) if (bufB[i] != DARK) leaked++;
    ok(leaked == 0, "a Terminal gui_clear must not inherit the Finder's clip");
    if (leaked) printf("    (%d/%d Terminal px left at the white window-create fill)\n", leaked, N*N);

    /* Sanity: an app's clip still bounds a draw into its OWN surface. */
    fb_target(&A);
    fb_fill_rect(0, 0, N, N, DARK);   /* only [5,8) x [5,8) should darken */
    int a_in = 0, a_out_changed = 0;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            int inside = (x >= 5 && x < 8 && y >= 5 && y < 8);
            if (inside && bufA[y*N+x] == DARK) a_in++;
            if (!inside && bufA[y*N+x] != WHITE) a_out_changed++;
        }
    ok(a_in == 9 && a_out_changed == 0, "an app's own clip still bounds its own draw");

    printf("fb_clip_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
