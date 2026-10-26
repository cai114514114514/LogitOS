/* fb_clip_push/pop/depth: per-context clip stack on the target surface.
 *
 * Built with -DGFX_NCTX=2 and linked with fb.c, which defines gfx_ctx_hook and
 * keys the context on sched_current_thread(). This test stubs that function and
 * registers "thread B" as context 1 with fb_ctx_register(1).
 *
 * Every check observes PIXELS (what a draw actually paints), never the surface
 * fields, so the same checks judge the control build, whose push writes only
 * the bound display. The control must lose the clip at a rebind: 'FAIL rebind'.
 * Exit 1 on any failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "fb.h"

#define W 120
#define H 90

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

/* sched_current_thread() stub: the "current thread" is test-selected. */
struct thread;
static char thread_obj[2];
static struct thread *g_cur;
struct thread *sched_current_thread(void) { return g_cur; }
#define AS_A() (g_cur = (struct thread *)&thread_obj[0])
#define AS_B() (g_cur = (struct thread *)&thread_obj[1])

static int fails;
static void ck(int cond, const char *what)
{
    printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) fails++;
}

static uint8_t mbi[128];
static uint32_t scratch_fb[W * H];
static void init_fb(void)
{
    memset(mbi, 0, sizeof mbi);
    *(uint32_t *)mbi = sizeof mbi;
    uint8_t *p = mbi + 8;
    *(uint32_t *)(p + 0)  = 8;
    *(uint32_t *)(p + 4)  = 40;
    *(uint64_t *)(p + 8)  = (uint64_t)(uintptr_t)scratch_fb;
    *(uint32_t *)(p + 16) = W * 4;
    *(uint32_t *)(p + 20) = W;
    *(uint32_t *)(p + 24) = H;
    p[28] = 32; p[29] = 1;
    p[32] = 16; p[33] = 8; p[34] = 8; p[35] = 8; p[36] = 0; p[37] = 8;
    if (!fb_init((uint64_t)(uintptr_t)mbi)) { printf("FAIL fb_init\n"); exit(1); }
}

static uint32_t pxa[W * H], pxb[W * H], pxo[W * H];
static struct surface sa, sb, so;
static void mk(struct surface *s, uint32_t *px)
{ memset(s, 0, sizeof *s); s->px = px; s->w = W; s->h = H; }

struct bb { int n, x0, y0, x1, y1; };      /* painted count + half-open bbox */
/* Paint the whole of the CURRENT target with 0xffffff over black, then report
 * which pixels changed. */
static struct bb paint(uint32_t *px)
{
    struct bb b = { 0, W, H, 0, 0 };
    memset(px, 0, sizeof(uint32_t) * W * H);
    fb_fill_rect(0, 0, W, H, 0xffffff);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (px[y * W + x]) {
                b.n++;
                if (x < b.x0) b.x0 = x;
                if (y < b.y0) b.y0 = y;
                if (x + 1 > b.x1) b.x1 = x + 1;
                if (y + 1 > b.y1) b.y1 = y + 1;
            }
    return b;
}
static int is(struct bb b, int x0, int y0, int x1, int y1)
{ return b.n == (x1 - x0) * (y1 - y0) && b.x0 == x0 && b.y0 == y0 && b.x1 == x1 && b.y1 == y1; }
static int is_none(struct bb b) { return b.n == 0; }
static int is_full(struct bb b) { return b.n == W * H; }

int main(void)
{
    init_fb();
    mk(&sa, pxa); mk(&sb, pxb); mk(&so, pxo);
    AS_A();
    fb_target(&sa);
    ck(fb_clip_depth() == 0, "depth starts at 0");

    /* nested pushes intersect */
    ck(fb_clip_push(10, 10, 100, 80) == OL_OK, "push 1 accepted");
    ck(fb_clip_depth() == 1, "depth 1");
    ck(is(paint(pxa), 10, 10, 100, 80), "push 1 clips to its rect");
    ck(fb_clip_push(50, 40, 200, 200) == OL_OK, "push 2 accepted");
    ck(is(paint(pxa), 50, 40, 100, 80), "nested push intersects");
    ck(fb_clip_push(200, 200, 210, 210) == OL_OK && is_none(paint(pxa)), "disjoint push clips everything");
    fb_clip_pop();
    ck(is(paint(pxa), 50, 40, 100, 80), "pop restores the nested clip");
    fb_clip_pop();
    ck(is(paint(pxa), 10, 10, 100, 80), "pop restores push 1");
    fb_clip_pop();
    ck(fb_clip_depth() == 0 && is_full(paint(pxa)), "pop restores the unclipped state");
    fb_clip_pop();
    ck(fb_clip_depth() == 0, "pop at depth 0 is a no-op");

    /* pop restores a pre-existing set_clip exactly, and push intersects it */
    fb_set_clip(20, 20, 60, 50);                       /* 20..80 x 20..70 */
    fb_clip_push(0, 0, 50, 50);
    ck(is(paint(pxa), 20, 20, 50, 50), "push intersects an existing set_clip");
    fb_clip_pop();
    ck(is(paint(pxa), 20, 20, 80, 70), "pop restores the set_clip exactly");
    fb_clear_clip();

    /* a rebind keeps the pushed clip */
    fb_clip_push(30, 30, 90, 70);
    ck(is(paint(pxa), 30, 30, 90, 70), "clip active before rebind");
    fb_target(&sa);                                    /* dirty -> rebind */
    ck(is(paint(pxa), 30, 30, 90, 70), "rebind: same-target rebind keeps the pushed clip");
    fb_target(&so);                                    /* different surface, and back */
    fb_target(&sa);
    ck(is(paint(pxa), 30, 30, 90, 70), "rebind: switch target and back keeps the pushed clip");
    /* the control fails the two rebind checks: its clip lived only on the display */
    fb_clip_pop();
    ck(is_full(paint(pxa)), "pop after rebind restores unclipped");

    /* overflow */
    int ok = 1;
    for (int i = 0; i < 16; i++) ok &= (fb_clip_push(i, i, W, H) == OL_OK);
    ck(ok && fb_clip_depth() == 16, "16 pushes accepted");
    ck(fb_clip_push(0, 0, 5, 5) == OL_LIMIT, "17th push returns OL_LIMIT");
    ck(fb_clip_depth() == 16 && is(paint(pxa), 15, 15, W, H), "overflow changes nothing");
    for (int i = 0; i < 16; i++) fb_clip_pop();
    ck(fb_clip_depth() == 0 && is_full(paint(pxa)), "16 pops restore");

    /* two contexts keep independent stacks */
    fb_clip_push(10, 10, 60, 60);                      /* ctx 0 */
    AS_B();
    ck(fb_ctx_register(1) == 0, "register thread B as ctx 1");
    ck(fb_clip_depth() == 0, "ctx 1 starts empty");
    fb_target(&sb);
    fb_clip_push(70, 5, 110, 85);
    fb_clip_push(80, 0, 120, 40);
    ck(fb_clip_depth() == 2, "ctx 1 depth 2");
    AS_A();
    ck(fb_clip_depth() == 1, "ctx 0 depth still 1");
    ck(is(paint(pxa), 10, 10, 60, 60), "ctx 0 clip unaffected by ctx 1");
    AS_B();
    ck(is(paint(pxb), 80, 5, 110, 40), "ctx 1 clip unaffected by ctx 0");
    fb_clip_pop();
    AS_A();
    fb_clip_pop();
    ck(fb_clip_depth() == 0 && is_full(paint(pxa)), "ctx 0 popped independently");
    AS_B();
    ck(fb_clip_depth() == 1 && is(paint(pxb), 70, 5, 110, 85), "ctx 1 kept its remaining push");
    fb_clip_pop();
    ck(fb_clip_depth() == 0 && is_full(paint(pxb)), "ctx 1 popped");

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("fb clip stack: all checks passed\n");
    return 0;
}
