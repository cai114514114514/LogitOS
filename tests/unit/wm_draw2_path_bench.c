/* Budget: one wm2_path rounded-rect fill (copy-in, validation, path build,
 * ol_display_fill_path) costs at most 1.5x the 7 legacy calls the app side
 * issues today for the same shape (a layered rounded rect: seven
 * SYS_GUI_RRECT-class draws, here ol_display_blend_round_rect, the routine
 * behind SYS_GUI_RRECT), both measured on the host display in this binary. */
#include <stdlib.h>
#include <string.h>
#include "bench_budget.h"
#include "wm_draw2.h"
#include "openlogit_display_path.h"

int (*gfx_ctx_hook)(void);

static void *t_alloc(unsigned long n) { return malloc(n); }
static void t_free(void *p) { free(p); }
static int t_copyin(void *dst, const void *src, unsigned long n) { memcpy(dst, src, n); return 0; }

static struct ol_display d;
static struct wm2_env env;
static struct logit_path2 path;
static struct logit_paint2 paint;
static unsigned char verbs[16];
static int xy[64];
static int nv, nx;
static int bx = 10, by = 6, bw, bh, br;

static void V(int v, int n, const int *p) { verbs[nv++] = (unsigned char)v; for (int i = 0; i < n * 2; i++) xy[nx++] = p[i]; }
static void build_rrect(int x, int y, int w, int h, int r)
{
    int k = (int)((long long)r * 36195 >> 16);
    nv = nx = 0;
    int p0[2] = { x + r, y }; V(LOGIT_VERB_M, 1, p0);
    int p1[2] = { x + w - r, y }; V(LOGIT_VERB_L, 1, p1);
    int c1[6] = { x + w - r + k, y, x + w, y + r - k, x + w, y + r }; V(LOGIT_VERB_C, 3, c1);
    int p2[2] = { x + w, y + h - r }; V(LOGIT_VERB_L, 1, p2);
    int c2[6] = { x + w, y + h - r + k, x + w - r + k, y + h, x + w - r, y + h }; V(LOGIT_VERB_C, 3, c2);
    int p3[2] = { x + r, y + h }; V(LOGIT_VERB_L, 1, p3);
    int c3[6] = { x + r - k, y + h, x, y + h - r + k, x, y + h - r }; V(LOGIT_VERB_C, 3, c3);
    int p4[2] = { x, y + r }; V(LOGIT_VERB_L, 1, p4);
    int c4[6] = { x, y + r - k, x + r - k, y, x + r, y }; V(LOGIT_VERB_C, 3, c4);
    V(LOGIT_VERB_Z, 0, 0);
    path.nverb = (unsigned)nv; path.nxy = (unsigned)nx;
}

static void ref_fn(void *a)
{
    (void)a;
    for (int i = 0; i < 7; i++)
        ol_display_blend_round_rect(&d, bx, by, bw, bh, br, 0x20 + i, 0x60, 0xc0, 255);
    bb_sink += d.target.px[500];
}
static void cand_fn(void *a)
{
    (void)a;
    long rc = wm2_path(&env, (long)&path, (long)&paint, 0);
    bb_sink += d.target.px[500] + (unsigned long)rc;
}

int main(void)
{
    static uint32_t px[128 * 64];
    static const int sz[][3] = { { 40, 24, 8 }, { 64, 32, 10 }, { 100, 48, 12 }, { 24, 24, 12 } };
    struct ol_pixel_target t = { px, 128, 64, 0, 0, 0, 0, 0 };
    d.allocate = t_alloc; d.release = t_free;
    ol_display_bind(&d, &t, 16, 8, 0);
    env.d = &d; env.scale_pct = 100; env.surf_w = 128; env.surf_h = 64; env.copyin = t_copyin;
    path.size = sizeof path; path.verbs = verbs; path.xy = xy;
    paint.size = sizeof paint; paint.argb = 0xFF2060C0u;
    for (unsigned i = 0; i < sizeof sz / sizeof sz[0]; i++) {
        char nm[64];
        bw = sz[i][0]; bh = sz[i][1]; br = sz[i][2];
        build_rrect(bx * 256, by * 256, bw * 256, bh * 256, br * 256);
        if (wm2_path(&env, (long)&path, (long)&paint, 0) != OL_OK) { printf("FAIL wm2_path rrect\n"); return 1; }
        struct bb_stats ref = bb_measure(ref_fn, 0, 1000);
        struct bb_stats cand = bb_measure(cand_fn, 0, 1000);
        printf("rrect %dx%d r=%d: 7 legacy %.0f ns, one wm2_path %.0f ns\n", bw, bh, br, ref.median_ns, cand.median_ns);
        snprintf(nm, sizeof nm, "wm2_path-vs-7-legacy-%dx%d", bw, bh);
        bb_ratio(nm, cand, ref, 1.5);
    }
    return bb_exit();
}
