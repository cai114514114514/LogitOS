/* SYS_GUI_SHADOW2 / SYS_GUI_GLASS2 (c/kernel/gui/wm_draw2_fx.c) against the
 * direct display calls, on a host env like wm_draw2_state_test.c's.
 *
 *   direct-shadow   outer and inset shadow equal ol_display_shadow_ex
 *   direct-glass    each blur tier / rim equals ol_display_liquid_glass_ex
 *   group-oracle    inside an open wm2 group (wm_draw2_state.c) both equal the
 *                   same calls inside an ol_display_group on the oracle
 *   args            bad sigma, radius, enums, flags, reserved, size: the
 *                   status is OL_ARGUMENT and no pixel changes
 *   sigma-scale     at 125/150/200% the shadow equals a direct call with the
 *                   24.8 sigma scaled by pct/100 (and 200% == 2x the 100% sigma)
 *   reduce-transp   reduce_transparency gives the solid glass, as the direct call
 *   info            wm2_fx_info carries both bits
 *
 * THE CONTROL (-DWM_DRAW2_FX_NO_SCALE) leaves sigma unscaled: 'FAIL sigma-scale'. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "wm_draw2.h"
#include "wm2_scale.h"
#include "openlogit_display_shadow.h"
#include "openlogit_glass_material.h"
#include "openlogit_display_group.h"

#define P(v) ((v) * 256)
#define W 160
#define H 120

static int failed;
static void fail(const char *check, const char *what)
{
    printf("FAIL %s (%s)\n", check, what);
    failed = 1;
}

static uint32_t *SURF, *OBUF;
static int clip_on, clx0, cly0, clx1, cly1;
static struct { int on, x0, y0, x1, y1; } fstk[16];
static int fdepth;
static struct ol_display D, O;
static int pct = 100, reduce;

static void *d_alloc(unsigned long n) { return malloc(n); }
static void *t_alloc(unsigned long n) { return malloc(n); }
static void t_release(void *p) { free(p); }
static int t_copyin(void *dst, const void *src, unsigned long n)
{
    if (!src) return -1;
    memcpy(dst, src, n);
    return 0;
}
static int f_push(void *o, int x0, int y0, int x1, int y1)
{
    (void)o;
    if (fdepth >= 16) return OL_LIMIT;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    fstk[fdepth].on = clip_on; fstk[fdepth].x0 = clx0; fstk[fdepth].y0 = cly0;
    fstk[fdepth].x1 = clx1; fstk[fdepth].y1 = cly1;
    fdepth++;
    if (clip_on) {
        if (x0 < clx0) x0 = clx0;
        if (y0 < cly0) y0 = cly0;
        if (x1 > clx1) x1 = clx1;
        if (y1 > cly1) y1 = cly1;
    }
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    clip_on = 1; clx0 = x0; cly0 = y0; clx1 = x1; cly1 = y1;
    return OL_OK;
}
static void f_pop(void *o)
{
    (void)o;
    if (fdepth <= 0) return;
    fdepth--;
    clip_on = fstk[fdepth].on; clx0 = fstk[fdepth].x0; cly0 = fstk[fdepth].y0;
    clx1 = fstk[fdepth].x1; cly1 = fstk[fdepth].y1;
}

static void bind(void)
{
    struct ol_pixel_target t = { SURF, W, H, clip_on, clx0, cly0, clx1, cly1 };
    D.allocate = d_alloc; D.release = free;
    ol_display_bind(&D, &t, 16, 8, 0);
    D.reduce_transparency = reduce;
}
static void env_for(struct wm2_env *e)
{
    memset(e, 0, sizeof *e);
    e->d = &D; e->scale_pct = pct; e->surf_w = W; e->surf_h = H; e->slot = 1;
    e->copyin = t_copyin; e->clip_push = f_push; e->clip_pop = f_pop;
    e->alloc = t_alloc; e->release = t_release;
}
typedef long (*handler)(struct wm2_env *, long, long, long);
static long call(handler fn, const void *arg)
{
    bind();
    struct wm2_env e;
    env_for(&e);
    return fn(&e, (long)arg, 0, 0);
}

static void reset(unsigned seed)
{
    unsigned r = seed;
    free(SURF); free(OBUF);
    SURF = malloc(W * H * 4); OBUF = malloc(W * H * 4);
    for (int i = 0; i < W * H; i++) { r = r * 1664525u + 1013904223u; SURF[i] = (r >> 8) & 0xFFFFFF; }
    memcpy(OBUF, SURF, W * H * 4);
    clip_on = 0; fdepth = 0;
}
static void obind(void)
{
    struct ol_pixel_target t = { OBUF, W, H, 0, 0, 0, 0, 0 };
    O.allocate = d_alloc; O.release = free;
    ol_display_bind(&O, &t, 16, 8, 0);
    O.reduce_transparency = reduce;
}
static long ndiff(void)
{
    long n = 0;
    for (int i = 0; i < W * H; i++) n += SURF[i] != OBUF[i];
    return n;
}
static int same(const char *check, const char *what)
{
    long n = ndiff();
    if (!n) return 0;
    char b[160];
    snprintf(b, sizeof b, "%s: %ld pixels differ from the oracle", what, n);
    fail(check, b);
    return 1;
}

static struct logit_shadow2 mkshadow(int x, int y, int w, int h, int r, int dx, int dy, int blur,
                                     int spread, unsigned argb, unsigned flags)
{
    struct logit_shadow2 s;
    memset(&s, 0, sizeof s);
    s.size = sizeof s; s.x = x; s.y = y; s.w = w; s.h = h;
    for (int k = 0; k < 4; k++) s.radius[k] = r;
    s.dx = dx; s.dy = dy; s.blur = blur; s.spread = spread; s.argb = argb; s.flags = flags;
    return s;
}
static struct logit_glass2 mkglass(int x, int y, int w, int h, int r, int blur_level, int rim)
{
    struct logit_glass2 g;
    memset(&g, 0, sizeof g);
    g.size = sizeof g; g.x = x; g.y = y; g.w = w; g.h = h;
    for (int k = 0; k < 4; k++) g.radius[k] = r;
    g.tint_argb = 0x80336699; g.opacity = 90; g.tone = 0; g.blur_level = blur_level; g.rim = rim;
    g.flags = LOGIT_RECT_SNAP;
    return g;
}

/* Oracle draws, at whole pixels. */
static void o_shadow(int x, int y, int w, int h, int r, int dx24, int dy24, int sig24, int spr24,
                     unsigned argb, int inset)
{
    int rad[4] = { r, r, r, r };
    ol_display_shadow_ex(&O, x, y, w, h, rad, dx24, dy24, sig24, spr24, argb & 0xFFFFFF,
                         (int)(argb >> 24), inset);
}
static void o_glass(int x, int y, int w, int h, int r, int blur, int rim)
{
    struct ol_glass_material m = {0};
    m.tint = 0x336699; m.opacity = 90; m.blur = (uint8_t)blur; m.rim = (uint8_t)rim;
    ol_display_liquid_glass_ex(&O, x, y, w, h, r, 0, &m);
}

static void t_direct(void)
{
    int bad = 0;
    pct = 100; reduce = 0;
    reset(5); obind();
    struct logit_shadow2 s1 = mkshadow(P(30), P(25), P(70), P(50), P(10), P(3), P(4), P(6) + 77, P(2), 0xC0102030, LOGIT_RECT_SNAP);
    struct logit_shadow2 s2 = mkshadow(P(20), P(15), P(90), P(60), P(12), P(-2), P(3), P(5), P(-1), 0x90000000,
                                       LOGIT_RECT_SNAP | LOGIT_SHADOW2_INSET);
    if (call(wm2_shadow2, &s1) != OL_OK || call(wm2_shadow2, &s2) != OL_OK) { fail("direct-shadow", "status"); bad++; }
    o_shadow(30, 25, 70, 50, 10, P(3), P(4), P(6) + 77, P(2), 0xC0102030, 0);
    o_shadow(20, 15, 90, 60, 12, P(-2), P(3), P(5), P(-1), 0x90000000, 1);
    bad += same("direct-shadow", "outer + inset shadow");
    if (!bad) printf("ok   direct-shadow: outer and inset bit-exact vs ol_display_shadow_ex\n");

    bad = 0;
    for (int blur = 0; blur < 4; blur++) {
        for (int rim = 0; rim < 2; rim++) {
            reset(7 + blur); obind();
            struct logit_glass2 g = mkglass(P(25), P(20), P(80), P(50), P(14), blur, rim);
            if (call(wm2_glass2, &g) != OL_OK) { fail("direct-glass", "status"); bad++; }
            o_glass(25, 20, 80, 50, 14, blur, rim);
            char b[32];
            snprintf(b, sizeof b, "blur %d rim %d", blur, rim);
            bad += same("direct-glass", b);
        }
    }
    if (!bad) printf("ok   direct-glass: 4 blur tiers x 2 rims bit-exact vs ol_display_liquid_glass_ex\n");
}

static void t_group(void)
{
    int bad = 0;
    pct = 100; reduce = 0;
    reset(11); obind();
    struct logit_group2 gr;
    memset(&gr, 0, sizeof gr);
    gr.size = sizeof gr; gr.x = P(15); gr.y = P(10); gr.w = P(120); gr.h = P(90);
    for (int k = 0; k < 4; k++) gr.radius[k] = P(8);
    gr.opacity = 200; gr.flags = LOGIT_RECT_SNAP;
    struct logit_shadow2 s = mkshadow(P(30), P(25), P(70), P(50), P(10), P(3), P(4), P(6), P(0), 0xC0102030, LOGIT_RECT_SNAP);
    struct logit_glass2 g = mkglass(P(40), P(30), P(60), P(40), P(12), 1, 1);
    long rc = call(wm2_group_begin, &gr);
    rc |= call(wm2_shadow2, &s);
    rc |= call(wm2_glass2, &g);
    rc |= call(wm2_group_end, 0);
    if (rc != OL_OK) { fail("group-oracle", "a call did not return OL_OK"); bad++; }
    if (fdepth != 0 || clip_on) { fail("group-oracle", "effects changed the clip stack"); bad++; }

    int rad[4] = { 8, 8, 8, 8 };
    unsigned long n = ol_display_group_bytes(120, 90, rad, 200);
    uint32_t *buf = malloc(n);
    struct ol_display_group og;
    ol_display_group_begin(&O, &og, 15, 10, 120, 90, rad, 200, buf, n);
    o_shadow(30, 25, 70, 50, 10, P(3), P(4), P(6), 0, 0xC0102030, 0);
    o_glass(40, 30, 60, 40, 12, 1, 1);
    ol_display_group_end(&O, &og);
    free(buf);
    bad += same("group-oracle", "shadow + glass inside a group");
    if (!bad) printf("ok   group-oracle: effects inside an open wm2 group equal the display_group oracle\n");
}

static unsigned crc(void)
{
    unsigned h = 2166136261u;
    for (int i = 0; i < W * H; i++) h = (h ^ SURF[i]) * 16777619u;
    return h;
}

static void t_args(void)
{
    int bad = 0;
    pct = 100; reduce = 0;
    reset(3);
    unsigned before = crc();
    struct logit_shadow2 ok = mkshadow(P(30), P(25), P(70), P(50), P(10), 0, 0, P(6), 0, 0xFF000000, LOGIT_RECT_SNAP);
    struct logit_glass2 gok = mkglass(P(30), P(25), P(70), P(50), P(10), 1, 0);
    struct logit_shadow2 s[8];
    for (int i = 0; i < 8; i++) s[i] = ok;
    s[0].blur = P(65);                  /* sigma > 64 pt */
    s[1].blur = -1;
    s[2].radius[2] = -P(1);
    s[3].flags = 1u << 5;
    s[4].w = -P(1);
    s[5].size = sizeof ok - 4;
    s[6].spread = 0x7FFFFFFF;
    s[7].radius[0] = 0x7FFFFFFF;
    for (int i = 0; i < 8; i++)
        if (call(wm2_shadow2, &s[i]) != OL_ARGUMENT) { char b[24]; snprintf(b, sizeof b, "shadow case %d", i); fail("args", b); bad++; }
    struct logit_glass2 g[9];
    for (int i = 0; i < 9; i++) g[i] = gok;
    g[0].blur_level = 4;
    g[1].rim = 2;
    g[2].cut = 16;
    g[3].reserved[1] = 1;
    g[4].opacity = 256;
    g[5].radius[0] = -1;
    g[6].flags = 2;
    g[7].tone = -1;
    g[8].h = -1;
    for (int i = 0; i < 9; i++)
        if (call(wm2_glass2, &g[i]) != OL_ARGUMENT) { char b[24]; snprintf(b, sizeof b, "glass case %d", i); fail("args", b); bad++; }
    if (crc() != before) { fail("args", "a rejected call touched pixels"); bad++; }
    if (call(wm2_shadow2, 0) != OL_ARGUMENT) { fail("args", "null pointer"); bad++; }
    /* sigma exactly 64 pt is accepted */
    s[0].blur = P(64);
    if (call(wm2_shadow2, &s[0]) != OL_OK) { fail("args", "sigma 64 pt rejected"); bad++; }
    if (!bad) printf("ok   args: 17 malformed calls -> OL_ARGUMENT, surface untouched\n");
}

static void t_scale(void)
{
    static const int pcts[] = { 125, 150, 200 };
    int bad = 0;
    reduce = 0;
    for (unsigned i = 0; i < 3; i++) {
        pct = pcts[i];
        reset(21); obind();
        int blur = P(7) + 100;
        struct logit_shadow2 s = mkshadow(P(20), P(16), P(60), P(40), P(6), P(2), P(3), blur, P(1), 0xB0204060, LOGIT_RECT_SNAP);
        if (call(wm2_shadow2, &s) != OL_OK) { fail("sigma-scale", "status"); bad++; }
        int sig = wm2_pt24(blur, pct);
        struct wm2_rect24 r = wm2_rect24(s.x, s.y, s.w, s.h, pct, 1);
        int rad = (wm2_pt24(P(6), pct) + 128) >> 8;
        o_shadow(r.x >> 8, r.y >> 8, r.w >> 8, r.h >> 8, rad, wm2_pt24(P(2), pct), wm2_pt24(P(3), pct),
                 sig, wm2_pt24(P(1), pct), 0xB0204060, 0);
        char b[40];
        snprintf(b, sizeof b, "%d%%: sigma %d -> %d", pct, blur, sig);
        bad += same("sigma-scale", b);
    }
    /* 200% sigma is exactly twice the 100% sigma. */
    pct = 200; reset(22); obind();
    struct logit_shadow2 s = mkshadow(P(20), P(16), P(60), P(40), P(6), 0, 0, P(5), 0, 0xB0204060, LOGIT_RECT_SNAP);
    call(wm2_shadow2, &s);
    o_shadow(40, 32, 120, 80, 12, 0, 0, P(10), 0, 0xB0204060, 0);
    bad += same("sigma-scale", "200% == 2x the 100% sigma");
    if (!bad) printf("ok   sigma-scale: 125/150/200%% sigma follows wm2_pt24\n");
    pct = 100;
}

static void t_reduce(void)
{
    int bad = 0;
    pct = 100; reduce = 1;
    reset(9); obind();
    struct logit_glass2 g = mkglass(P(25), P(20), P(80), P(50), P(14), 2, 0);
    if (call(wm2_glass2, &g) != OL_OK) { fail("reduce-transp", "status"); bad++; }
    o_glass(25, 20, 80, 50, 14, 2, 0);
    bad += same("reduce-transp", "solid glass");
    uint32_t *keep = malloc(W * H * 4);
    memcpy(keep, SURF, W * H * 4);
    reduce = 0; reset(9); obind();
    call(wm2_glass2, &g);
    if (!memcmp(keep, SURF, W * H * 4)) { fail("reduce-transp", "reduce changed nothing"); bad++; }
    free(keep);
    reduce = 0;
    if (!bad) printf("ok   reduce-transp: solid glass, parity with liquid_glass_ex\n");
}

int main(void)
{
    if ((wm2_fx_info & (LOGIT_GUI2_F_SHADOW2 | LOGIT_GUI2_F_GLASS2)) !=
        (LOGIT_GUI2_F_SHADOW2 | LOGIT_GUI2_F_GLASS2))
        fail("info", "wm2_fx_info lacks SHADOW2|GLASS2");
    t_direct();
    t_group();
    t_args();
    t_scale();
    t_reduce();
    printf(failed ? "wm_draw2_fx: FAILED\n" : "wm_draw2_fx: all ok\n");
    return failed;
}
