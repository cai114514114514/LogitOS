/* GUI ABI v2 clip/group stacks and BLIT2 (c/kernel/gui/wm_draw2_state.c).
 *
 * The handlers run against a host wm2_env that behaves like wm.c + fb.c:
 * every "syscall" first rebinds the one display from the target surface (as
 * fb_target + fb_display_cur do), the env's clip_push/clip_pop are a copy of
 * fb_clip_push/fb_clip_pop -- a per-CONTEXT stack whose clip lives on the
 * target surface -- and children are drawn on the rebound display between
 * calls, as SYS_GUI_TEXT_RUN2 or SYS_GUI_PATH would. Oracles draw the same
 * children on a second display in one scope with ol_display_group_* and
 * ol_display_blit_rgba_ex directly; pixel checks are bit-exact.
 *
 *   rebind-clip     nested rounded clips with a square clip inside, a rebind
 *                   before every call: pixels equal the nested display_group
 *                   oracle, and clip and stack depth come back exactly
 *   group-oracle    an opacity-128 group over a glyph (text) and BLIT2
 *                   sequence at 125% equals the display_group oracle
 *   blit2-ref       BLIT2 with stride padding: BOX downscale (streamed in
 *                   bands), BILINEAR upscale under a clip, NEAREST + PREMUL,
 *                   each equal to one ol_display_blit_rgba_ex call; a band too
 *                   large for the tile is OL_LIMIT, a user fault OL_ARGUMENT
 *   arena-limit     arena exhaustion and allocation failure are OL_LIMIT with
 *                   the stack, clip and pixels intact; depth 16; arena growth
 *                   rebases open snapshots
 *   flush-autoclose an unbalanced frame is closed at flush (LIFO, pixels as
 *                   the oracle), wm2_state_err_count reports it, the arena
 *                   watermark resets, reset clears it and frees the arena
 *   hidpi-radius    rect edges and radii at 125% and 150% follow wm2_rect24 /
 *                   wm2_pt24 rounded to the nearest pixel
 *   ledger          two windows interleaving pushes on the per-context fb
 *                   stack; reset orphans and their reclaim
 *   args            malformed calls
 *
 * Links wm_draw2_state.c and $(GFX_SRC) only (no fb.c, no router), GFX_NCTX
 * left at 1: the state is kernel-only and serialised by graphics_lock.
 *
 * THE CONTROL (-DWM_DRAW2_CLIP_DISPLAY_ONLY) sets the clip on e->d only; the
 * rebind before the next call forgets it and 'FAIL rebind-clip' must fire. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "wm_draw2.h"
#include "wm2_scale.h"
#include "openlogit_display_group.h"
#include "openlogit_display_blit.h"

#define P(v) ((v) * 256)

static int failed;
static void fail(const char *check, const char *what)
{
    printf("FAIL %s (%s)\n", check, what);
    failed = 1;
}

/* ---- the fake world ------------------------------------------------------ */
struct fsurf { uint32_t *px; int w, h, clip_on, clx0, cly0, clx1, cly1; };
static struct fsurf SURF[WM2_MAX_WINDOWS];
static struct fsurf *T;
static struct { int on, x0, y0, x1, y1; } fstk[16];
static int fdepth;
static struct ol_display D, O;
static int pct = 100;
static long copy_budget = -1;     /* copyins left before a fault, -1 = never */
static int alloc_fail;
static long alloc_live;

static void *t_alloc(unsigned long n) { if (alloc_fail) return 0; alloc_live++; return malloc(n); }
static void t_release(void *p) { if (p) alloc_live--; free(p); }
static void *d_alloc(unsigned long n) { return malloc(n); }

static int t_copyin(void *dst, const void *src, unsigned long n)
{
    if (!src) return -1;
    if (copy_budget == 0) return -1;
    if (copy_budget > 0) copy_budget--;
    memcpy(dst, src, n);
    return 0;
}

/* fb_clip_push / fb_clip_pop (fb.c), on the current target. */
static int f_push(void *o, int x0, int y0, int x1, int y1)
{
    (void)o;
    struct fsurf *s = T;
    if (fdepth >= 16) return OL_LIMIT;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    fstk[fdepth].on = s->clip_on; fstk[fdepth].x0 = s->clx0; fstk[fdepth].y0 = s->cly0;
    fstk[fdepth].x1 = s->clx1; fstk[fdepth].y1 = s->cly1;
    fdepth++;
    if (s->clip_on) {
        if (x0 < s->clx0) x0 = s->clx0;
        if (y0 < s->cly0) y0 = s->cly0;
        if (x1 > s->clx1) x1 = s->clx1;
        if (y1 > s->cly1) y1 = s->cly1;
    }
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    s->clip_on = 1; s->clx0 = x0; s->cly0 = y0; s->clx1 = x1; s->cly1 = y1;
    return OL_OK;
}
static void f_pop(void *o)
{
    (void)o;
    struct fsurf *s = T;
    if (fdepth <= 0) return;
    fdepth--;
    s->clip_on = fstk[fdepth].on; s->clx0 = fstk[fdepth].x0; s->cly0 = fstk[fdepth].y0;
    s->clx1 = fstk[fdepth].x1; s->cly1 = fstk[fdepth].y1;
}

/* fb_target(&surf) + fb_display_cur(): the display is rebuilt from the
 * surface, clip included. */
static void bind(int slot)
{
    T = &SURF[slot];
    struct ol_pixel_target t = { T->px, T->w, T->h, T->clip_on, T->clx0, T->cly0, T->clx1, T->cly1 };
    D.allocate = d_alloc; D.release = free;
    ol_display_bind(&D, &t, 16, 8, 0);
}

static void env_for(struct wm2_env *e, int slot)
{
    memset(e, 0, sizeof *e);
    e->d = &D;
    e->scale_pct = pct;
    e->surf_w = SURF[slot].w; e->surf_h = SURF[slot].h;
    e->slot = slot;
    e->copyin = t_copyin;
    e->clip_push = f_push;
    e->clip_pop = f_pop;
    e->alloc = t_alloc;
    e->release = t_release;
    e->opaque = &SURF[slot];
}

typedef long (*handler)(struct wm2_env *, long, long, long);
static long call(int slot, handler fn, const void *arg)
{
    bind(slot);
    struct wm2_env e;
    env_for(&e, slot);
    return fn(&e, (long)arg, 0, 0);
}
static void flush(int slot)
{
    bind(slot);
    struct wm2_env e;
    env_for(&e, slot);
    wm2_state_win_flush(&e);
}

static unsigned rng = 1;
static unsigned rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static void surf_init(int slot, int w, int h, unsigned seed)
{
    struct fsurf *s = &SURF[slot];
    free(s->px);
    memset(s, 0, sizeof *s);
    s->px = malloc((size_t)w * h * 4);
    s->w = w; s->h = h;
    rng = seed;
    for (int i = 0; i < w * h; i++) s->px[i] = rnd() & 0xFFFFFFu;
}

/* The oracle: a copy of the surface on display O, the given clip. */
static uint32_t *obuf;
static void oracle_init(int slot)
{
    struct fsurf *s = &SURF[slot];
    free(obuf);
    obuf = malloc((size_t)s->w * s->h * 4);
    memcpy(obuf, s->px, (size_t)s->w * s->h * 4);
    struct ol_pixel_target t = { obuf, s->w, s->h, s->clip_on, s->clx0, s->cly0, s->clx1, s->cly1 };
    O.allocate = d_alloc; O.release = free;
    ol_display_bind(&O, &t, 16, 8, 0);
}

static long diff_count(int slot, int *maxd)
{
    struct fsurf *s = &SURF[slot];
    long n = 0;
    int m = 0;
    for (int i = 0; i < s->w * s->h; i++) {
        if (s->px[i] == obuf[i]) continue;
        n++;
        for (int q = 0; q < 24; q += 8) {
            int dd = abs((int)((s->px[i] >> q) & 255) - (int)((obuf[i] >> q) & 255));
            if (dd > m) m = dd;
        }
    }
    if (maxd) *maxd = m;
    return n;
}

static int check_pixels(const char *check, const char *what, int slot)
{
    int m;
    long n = diff_count(slot, &m);
    if (!n) return 0;
    char buf[160];
    snprintf(buf, sizeof buf, "%s: %ld pixels differ from the oracle, max delta %d", what, n, m);
    fail(check, buf);
    return 1;
}

/* Group helper for oracles: its own snapshot buffer. */
struct og { struct ol_display_group g; uint32_t *buf; };
static int og_begin(struct og *o, int x, int y, int w, int h, const int rad[4], int opacity)
{
    unsigned long n = ol_display_group_bytes(w, h, rad, opacity);
    o->buf = n ? malloc(n) : 0;
    return ol_display_group_begin(&O, &o->g, x, y, w, h, rad, opacity, o->buf, n);
}
static int og_end(struct og *o)
{
    int rc = ol_display_group_end(&O, &o->g);
    free(o->buf);
    return rc;
}

/* Children: the same deterministic content on any display. */
static uint8_t cov[64 * 32];
static void child(struct ol_display *d, unsigned seed)
{
    rng = seed;
    int W = d->target.w, H = d->target.h;
    ol_display_fill_vgrad(d, 0, 0, W, H, rnd() & 0xFFFFFF, rnd() & 0xFFFFFF);
    ol_display_blend_rect(d, (int)(rnd() % 40), (int)(rnd() % 30), W / 2, H / 2,
                          (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)(64 + rnd() % 128));
    for (int i = 0; i < 64 * 32; i++) cov[i] = (uint8_t)rnd();
    ol_display_blit_glyph_a(d, (int)(rnd() % (unsigned)(W / 2)), (int)(rnd() % (unsigned)(H / 2)),
                            cov, 64, 32, 64, rnd() & 0xFFFFFF, 200);
}
static void child_on(int slot, unsigned seed) { bind(slot); child(&D, seed); }
/* After the last pop: a corner square outside every clip of the checks, so an
 * unclipped draw shows without repainting (and hiding) what came before. */
static void corner(struct ol_display *d) { ol_display_fill_rect(d, 0, 0, 4, 4, 0x00FF00FF); }
static void corner_on(int slot) { bind(slot); corner(&D); }

static struct logit_clip2 mkclip(int x, int y, int w, int h, int r0, int r1, int r2, int r3, unsigned flags)
{
    struct logit_clip2 c;
    memset(&c, 0, sizeof c);
    c.size = sizeof c;
    c.x = x; c.y = y; c.w = w; c.h = h;
    c.radius[0] = r0; c.radius[1] = r1; c.radius[2] = r2; c.radius[3] = r3;
    c.flags = flags;
    return c;
}
static struct logit_group2 mkgroup(int x, int y, int w, int h, int r, int opacity, unsigned flags)
{
    struct logit_group2 g;
    memset(&g, 0, sizeof g);
    g.size = sizeof g;
    g.x = x; g.y = y; g.w = w; g.h = h;
    for (int k = 0; k < 4; k++) g.radius[k] = r;
    g.opacity = opacity;
    g.flags = flags;
    return g;
}

static int surf_clip_is(int slot, int on, int x0, int y0, int x1, int y1)
{
    const struct fsurf *s = &SURF[slot];
    if (!on) return !s->clip_on;
    return s->clip_on && s->clx0 == x0 && s->cly0 == y0 && s->clx1 == x1 && s->cly1 == y1;
}

/* ---- rebind-clip ----------------------------------------------------------- */
static void t_rebind_clip(void)
{
    const char *C = "rebind-clip";
    int bad = 0;
    pct = 100;
    surf_init(1, 200, 150, 11);
    oracle_init(1);
    struct logit_clip2 A = mkclip(P(10), P(10), P(150), P(100), P(20), P(20), P(20), P(20), LOGIT_RECT_SNAP);
    struct logit_clip2 B = mkclip(P(40), P(30), P(80), P(60), P(12), 0, P(30), P(6), LOGIT_RECT_SNAP);
    struct logit_clip2 Q = mkclip(P(50), P(40), P(30), P(20), 0, 0, 0, 0, LOGIT_RECT_SNAP);
    long rc = 0;
    rc |= call(1, wm2_clip_push, &A);
    child_on(1, 101);
    rc |= call(1, wm2_clip_push, &B);
    child_on(1, 102);
    rc |= call(1, wm2_clip_push, &Q);
    child_on(1, 103);
    rc |= call(1, wm2_clip_pop, 0);
    child_on(1, 104);
    rc |= call(1, wm2_clip_pop, 0);
    child_on(1, 105);
    rc |= call(1, wm2_clip_pop, 0);
    corner_on(1);                    /* after the last pop: unclipped again */

    /* oracle: one scope, no rebinds */
    struct og ga, gb;
    int ra[4] = { 20, 20, 20, 20 }, rb[4] = { 12, 0, 30, 6 };
    og_begin(&ga, 10, 10, 150, 100, ra, 255);
    child(&O, 101);
    og_begin(&gb, 40, 30, 80, 60, rb, 255);
    child(&O, 102);
    struct ol_pixel_target keep = O.target;
    if (O.target.clx0 < 50) O.target.clx0 = 50;
    if (O.target.cly0 < 40) O.target.cly0 = 40;
    if (O.target.clx1 > 80) O.target.clx1 = 80;
    if (O.target.cly1 > 60) O.target.cly1 = 60;
    child(&O, 103);
    O.target = keep;
    child(&O, 104);
    og_end(&gb);
    child(&O, 105);
    og_end(&ga);
    corner(&O);

    if (rc != OL_OK) { fail(C, "a push or pop did not return OL_OK"); bad++; }
    if (!surf_clip_is(1, 0, 0, 0, 0, 0) || fdepth != 0) { fail(C, "surface clip or fb depth not restored"); bad++; }
    bad += check_pixels(C, "nested rounded + square clip across rebinds", 1);
    if (!bad) printf("ok   %s: 2 rounded + 1 square level, 6 rebinds, bit-exact vs nested display_group\n", C);
}

/* ---- group-oracle ---------------------------------------------------------- */
static uint8_t *mkimg(int w, int h, int stride, unsigned seed, int binary_alpha)
{
    uint8_t *p = malloc((size_t)stride * h);
    rng = seed;
    for (long i = 0; i < (long)stride * h; i++) p[i] = (uint8_t)rnd();
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *q = p + (long)y * stride + x * 4;
            unsigned a = rnd() & 255;
            if (binary_alpha) a = (a & 1) ? 255 : 0;
            else if ((a & 7) == 0) a = 255;
            q[3] = (uint8_t)a;
        }
    return p;
}

static struct logit_blit2 mkblit(const uint8_t *img, int sw, int sh, int stride, int sx, int sy,
                                 int sww, int shh, int x, int y, int w, int h,
                                 int opacity, int filter, unsigned flags)
{
    struct logit_blit2 b;
    memset(&b, 0, sizeof b);
    b.size = sizeof b;
    b.rgba = img; b.sw = sw; b.sh = sh; b.sstride = stride;
    b.src_x = sx; b.src_y = sy; b.src_w = sww; b.src_h = shh;
    b.x = x; b.y = y; b.w = w; b.h = h;
    b.opacity = opacity; b.filter = filter; b.flags = flags;
    return b;
}

static int ref_blit(const struct logit_blit2 *b)
{
    struct wm2_rect24 r = wm2_rect24(b->x, b->y, b->w, b->h, pct, (b->flags & LOGIT_RECT_SNAP) != 0);
    return ol_display_blit_rgba_ex(&O, b->rgba, b->sw, b->sh, b->sstride, b->src_x, b->src_y,
                                   b->src_w, b->src_h, r.x, r.y, r.w, r.h, b->opacity,
                                   (enum ol_filter)b->filter);
}

static void t_group_oracle(void)
{
    const char *C = "group-oracle";
    int bad = 0;
    pct = 125;
    surf_init(2, 240, 180, 22);
    oracle_init(2);
    struct logit_group2 G = mkgroup(P(16), P(12), P(120), P(90), P(8), 128, LOGIT_RECT_SNAP);
    uint8_t *img = mkimg(64, 48, 64 * 4 + 20, 23, 0);
    struct logit_blit2 bl = mkblit(img, 64, 48, 64 * 4 + 20, 0, 0, 64, 48,
                                   P(20) + 64, P(20), P(60), P(40), 255, LOGIT_FILTER_BILINEAR, 0);
    long rc = 0;
    rc |= call(2, wm2_group_begin, &G);
    child_on(2, 201);
    rc |= call(2, wm2_blit2, &bl);
    bind(2);
    ol_display_blit_glyph_a(&D, 40, 60, cov, 64, 32, 64, 0x203040, 255);
    rc |= call(2, wm2_group_end, 0);

    /* device geometry by hand: SNAP floors each edge -- 16*1.25=20,
     * 136*1.25=170, 12*1.25=15, 102*1.25=127.5 -> 127; radius 8*1.25=10 */
    struct og g;
    int rad[4] = { 10, 10, 10, 10 };
    og_begin(&g, 20, 15, 150, 112, rad, 128);
    child(&O, 201);
    if (ref_blit(&bl) != OL_OK) { fail(C, "reference blit refused"); bad++; }
    ol_display_blit_glyph_a(&O, 40, 60, cov, 64, 32, 64, 0x203040, 255);
    og_end(&g);

    if (rc != OL_OK) { fail(C, "begin/blit/end did not return OL_OK"); bad++; }
    if (!surf_clip_is(2, 0, 0, 0, 0, 0) || fdepth != 0) { fail(C, "clip or depth not restored"); bad++; }
    bad += check_pixels(C, "opacity-128 group over glyph + BLIT2", 2);
    if (!bad) printf("ok   %s: opacity 128, r=10px at 125%%, bit-exact vs display_group\n", C);
    free(img);
}

/* ---- blit2-ref ------------------------------------------------------------- */
static void t_blit2_ref(void)
{
    const char *C = "blit2-ref";
    int bad = 0;
    char buf[160];

    /* (a) BOX downscale of a padded 1024x768, sub-rect, fractional 24.8 edges */
    pct = 125;
    surf_init(3, 400, 300, 31);
    oracle_init(3);
    int st = 1024 * 4 + 52;
    uint8_t *big = mkimg(1024, 768, st, 32, 0);
    struct logit_blit2 b = mkblit(big, 1024, 768, st, 13, 7, 1000, 750,
                                  P(10) + 128, P(8) + 64, P(100) + 32, P(75), 230, LOGIT_FILTER_BOX, 0);
    long rc = call(3, wm2_blit2, &b);
    int rr = ref_blit(&b);
    if (rc != OL_OK || rr != OL_OK) { snprintf(buf, sizeof buf, "BOX rc %ld ref %d", rc, rr); fail(C, buf); bad++; }
    bad += check_pixels(C, "BOX 1000x750 -> 125x94 px", 3);
    unsigned long tile = D.scratch_n[OL_DS_USER];
    if (!tile || tile >= 750UL * 4000UL) {
        snprintf(buf, sizeof buf, "OL_DS_USER tile is %lu bytes: the source was not streamed", tile);
        fail(C, buf); bad++;
    }

    /* (b) BILINEAR upscale under a square clip pushed through the stack */
    pct = 150;
    surf_init(3, 400, 300, 33);
    oracle_init(3);
    uint8_t *sm = mkimg(40, 30, 40 * 4 + 12, 34, 0);
    struct logit_clip2 cl = mkclip(P(20), P(10), P(200), P(150), 0, 0, 0, 0, LOGIT_RECT_SNAP);
    b = mkblit(sm, 40, 30, 40 * 4 + 12, 0, 0, 40, 30, P(5), P(3), P(240), P(180), 255,
               LOGIT_FILTER_BILINEAR, LOGIT_RECT_SNAP);
    rc = call(3, wm2_clip_push, &cl);
    rc |= call(3, wm2_blit2, &b);
    rc |= call(3, wm2_clip_pop, 0);
    O.target.clip_on = 1; O.target.clx0 = 30; O.target.cly0 = 15; O.target.clx1 = 330; O.target.cly1 = 240;
    rr = ref_blit(&b);
    O.target.clip_on = 0;
    if (rc != OL_OK || rr != OL_OK) { fail(C, "BILINEAR under clip refused"); bad++; }
    bad += check_pixels(C, "BILINEAR 40x30 -> 360x270 px under a clip", 3);

    /* (c) NEAREST, PREMUL: alpha 0/255 only, so premultiplied == straight */
    pct = 100;
    surf_init(3, 400, 300, 35);
    oracle_init(3);
    uint8_t *pm = mkimg(90, 70, 90 * 4 + 4, 36, 1);
    uint8_t *straight = malloc((size_t)(90 * 4 + 4) * 70);
    memcpy(straight, pm, (size_t)(90 * 4 + 4) * 70);
    for (int y = 0; y < 70; y++)
        for (int x = 0; x < 90; x++) {
            uint8_t *q = pm + (long)y * (90 * 4 + 4) + x * 4;
            if (!q[3]) q[0] = q[1] = q[2] = 0;
        }
    b = mkblit(pm, 90, 70, 90 * 4 + 4, 5, 6, 80, 60, P(30), P(20), P(170), P(130), 180,
               LOGIT_FILTER_NEAREST, LOGIT_BLIT2_PREMUL);
    rc = call(3, wm2_blit2, &b);
    b.rgba = straight;
    rr = ref_blit(&b);
    if (rc != OL_OK || rr != OL_OK) { fail(C, "NEAREST PREMUL refused"); bad++; }
    bad += check_pixels(C, "NEAREST + PREMUL", 3);

    /* (d) a band that cannot fit the tile: OL_LIMIT before any copyin */
    long before = copy_budget;
    copy_budget = 2;                 /* the struct only: a row copyin would fault */
    b = mkblit(big, 32768, 32768, 32768 * 4, 0, 0, 32768, 32768, 0, 0, P(2), P(2), 255,
               LOGIT_FILTER_BOX, LOGIT_RECT_SNAP);
    rc = call(3, wm2_blit2, &b);
    copy_budget = before;
    if (rc != OL_LIMIT) { snprintf(buf, sizeof buf, "32768^2 -> 2x2 returned %ld, want OL_LIMIT", rc); fail(C, buf); bad++; }

    /* (e) a fault in the middle of the stream */
    pct = 125;
    b = mkblit(big, 1024, 768, st, 13, 7, 1000, 750,
               P(10) + 128, P(8) + 64, P(100) + 32, P(75), 230, LOGIT_FILTER_BOX, 0);
    copy_budget = 2 + 300;           /* struct (2 copyins) + some rows */
    rc = call(3, wm2_blit2, &b);
    copy_budget = -1;
    if (rc != OL_ARGUMENT) { snprintf(buf, sizeof buf, "fault mid-stream returned %ld", rc); fail(C, buf); bad++; }

    if (!bad) printf("ok   %s: BOX streamed in a %lu-byte tile, BILINEAR+clip, NEAREST+PREMUL bit-exact; LIMIT, fault\n", C, tile);
    free(big); free(sm); free(pm); free(straight);
}

/* ---- arena-limit ----------------------------------------------------------- */
static void t_arena_limit(void)
{
    const char *C = "arena-limit";
    int bad = 0;
    char buf[160];
    pct = 100;
    surf_init(4, 64, 48, 41);          /* arena cap 2 x 64*48*4 = 24576 */
    oracle_init(4);
    struct logit_group2 G = mkgroup(0, 0, P(64), P(48), 0, 100, LOGIT_RECT_SNAP);
    struct logit_group2 G2 = mkgroup(P(4), P(4), P(56), P(40), 0, 180, LOGIT_RECT_SNAP);
    long rc = call(4, wm2_group_begin, &G);
    child_on(4, 401);
    rc |= call(4, wm2_group_begin, &G2);   /* 12288 + 8960 = 21248 */
    child_on(4, 402);
    if (rc != OL_OK) { fail(C, "two groups that fit returned an error"); bad++; }

    int d0 = fdepth;
    struct fsurf s0 = SURF[4];
    uint32_t *px0 = malloc(64 * 48 * 4);
    memcpy(px0, SURF[4].px, 64 * 48 * 4);
    long r3 = call(4, wm2_group_begin, &G);
    struct logit_clip2 rc2 = mkclip(P(8), P(8), P(40), P(30), P(6), P(6), P(6), P(6), LOGIT_RECT_SNAP);
    long r4 = call(4, wm2_clip_push, &rc2);   /* 4 corners * 36 px * 4 = 576 of 3328 left */
    long r5 = OL_OK;
    if (r4 == OL_OK) r5 = call(4, wm2_clip_pop, 0);
    if (r3 != OL_LIMIT) { snprintf(buf, sizeof buf, "third full group returned %ld, want OL_LIMIT", r3); fail(C, buf); bad++; }
    if (r4 != OL_OK || r5 != OL_OK) { fail(C, "a small rounded clip in the remaining arena failed"); bad++; }
    if (fdepth != d0 || SURF[4].clip_on != s0.clip_on || SURF[4].clx0 != s0.clx0 ||
        SURF[4].cly0 != s0.cly0 || SURF[4].clx1 != s0.clx1 || SURF[4].cly1 != s0.cly1 ||
        memcmp(px0, SURF[4].px, 64 * 48 * 4)) {
        fail(C, "OL_LIMIT changed the stack, the clip or the pixels"); bad++;
    }
    rc = call(4, wm2_group_end, 0);
    rc |= call(4, wm2_group_end, 0);
    if (rc != OL_OK) { fail(C, "ending the two groups failed"); bad++; }
    struct og a, bq;
    og_begin(&a, 0, 0, 64, 48, 0, 100);
    child(&O, 401);
    og_begin(&bq, 4, 4, 56, 40, 0, 180);
    child(&O, 402);
    og_end(&bq);
    og_end(&a);
    bad += check_pixels(C, "two nested groups around an OL_LIMIT", 4);

    /* allocation failure */
    wm2_state_win_reset(4);
    alloc_fail = 1;
    rc = call(4, wm2_group_begin, &G);
    alloc_fail = 0;
    if (rc != OL_LIMIT || fdepth != 0 || SURF[4].clip_on) { fail(C, "failed allocation is not a clean OL_LIMIT"); bad++; }

    /* depth 16 */
    struct logit_clip2 sq = mkclip(P(1), P(1), P(60), P(40), 0, 0, 0, 0, LOGIT_RECT_SNAP);
    rc = 0;
    for (int i = 0; i < 16; i++) rc |= call(4, wm2_clip_push, &sq);
    long r17 = call(4, wm2_clip_push, &sq);
    if (rc != OL_OK || r17 != OL_LIMIT || fdepth != 16) { fail(C, "depth 16 / 17th push"); bad++; }
    rc = 0;
    for (int i = 0; i < 16; i++) rc |= call(4, wm2_clip_pop, 0);
    if (rc != OL_OK || call(4, wm2_clip_pop, 0) != OL_STATE || fdepth != 0 || SURF[4].clip_on) {
        fail(C, "16 pops / pop of an empty stack"); bad++;
    }

    /* growth: a second, larger snapshot reallocates; the first is rebased */
    surf_init(5, 400, 300, 42);
    oracle_init(5);
    struct logit_group2 s1 = mkgroup(P(10), P(10), P(50), P(50), P(5), 90, LOGIT_RECT_SNAP);
    struct logit_group2 s2 = mkgroup(P(20), P(20), P(300), P(200), P(9), 160, LOGIT_RECT_SNAP);
    long al0 = alloc_live;
    rc = call(5, wm2_group_begin, &s1);
    child_on(5, 501);
    rc |= call(5, wm2_group_begin, &s2);   /* 240000 bytes > the 64 KiB first arena */
    child_on(5, 502);
    rc |= call(5, wm2_group_end, 0);
    child_on(5, 503);
    rc |= call(5, wm2_group_end, 0);
    int rad5[4] = { 5, 5, 5, 5 }, rad9[4] = { 9, 9, 9, 9 };
    og_begin(&a, 10, 10, 50, 50, rad5, 90);
    child(&O, 501);
    og_begin(&bq, 20, 20, 300, 200, rad9, 160);
    child(&O, 502);
    og_end(&bq);
    child(&O, 503);
    og_end(&a);
    if (rc != OL_OK) { fail(C, "groups across an arena growth failed"); bad++; }
    if (alloc_live != al0 + 1) { snprintf(buf, sizeof buf, "%ld live arenas after growth, want 1", alloc_live - al0); fail(C, buf); bad++; }
    bad += check_pixels(C, "outer snapshot after the arena grew", 5);
    wm2_state_win_reset(5);
    if (alloc_live != al0) { fail(C, "reset did not free the arena"); bad++; }

    if (!bad) printf("ok   %s: OL_LIMIT with the stack intact, alloc failure, depth 16, growth rebases\n", C);
    free(px0);
}

/* ---- flush-autoclose ------------------------------------------------------- */
static void t_flush(void)
{
    const char *C = "flush-autoclose";
    int bad = 0;
    char buf[160];
    pct = 100;
    surf_init(6, 120, 90, 61);
    oracle_init(6);
    struct logit_clip2 R = mkclip(P(5), P(5), P(100), P(70), P(14), P(14), P(14), P(14), LOGIT_RECT_SNAP);
    struct logit_group2 G = mkgroup(P(15), P(10), P(80), P(60), P(4), 200, LOGIT_RECT_SNAP);
    struct logit_clip2 Q = mkclip(P(30), P(20), P(40), P(30), 0, 0, 0, 0, LOGIT_RECT_SNAP);
    long rc = call(6, wm2_clip_push, &R);
    child_on(6, 601);
    rc |= call(6, wm2_group_begin, &G);
    child_on(6, 602);
    rc |= call(6, wm2_clip_push, &Q);
    child_on(6, 603);
    flush(6);                          /* frame ends with three levels open */
    corner_on(6);                      /* the next frame draws unclipped */
    unsigned n = wm2_state_err_count(6);

    struct og r, g;
    int r14[4] = { 14, 14, 14, 14 }, r4[4] = { 4, 4, 4, 4 };
    og_begin(&r, 5, 5, 100, 70, r14, 255);
    child(&O, 601);
    og_begin(&g, 15, 10, 80, 60, r4, 200);
    child(&O, 602);
    struct ol_pixel_target keep = O.target;
    if (O.target.clx0 < 30) O.target.clx0 = 30;
    if (O.target.cly0 < 20) O.target.cly0 = 20;
    if (O.target.clx1 > 70) O.target.clx1 = 70;
    if (O.target.cly1 > 50) O.target.cly1 = 50;
    child(&O, 603);
    O.target = keep;
    og_end(&g);
    og_end(&r);
    corner(&O);

    if (rc != OL_OK) { fail(C, "opening the levels failed"); bad++; }
    if (n != 3) { snprintf(buf, sizeof buf, "wm2_state_err_count = %u, want 3", n); fail(C, buf); bad++; }
    if (fdepth != 0 || SURF[6].clip_on) { fail(C, "flush left a clip pushed"); bad++; }
    bad += check_pixels(C, "auto-closed rounded clip + group + clip", 6);
    flush(6);
    rc = call(6, wm2_group_begin, &G);
    rc |= call(6, wm2_group_end, 0);
    flush(6);
    if (rc != OL_OK || wm2_state_err_count(6) != 3) { fail(C, "a balanced frame changed the count"); bad++; }
    if (call(6, wm2_clip_pop, 0) != OL_STATE) { fail(C, "pop after flush is not OL_STATE"); bad++; }

    /* the watermark resets: two arena-filling groups, flush, two again */
    surf_init(7, 64, 48, 62);
    struct logit_group2 F = mkgroup(0, 0, P(64), P(48), 0, 100, LOGIT_RECT_SNAP);
    rc = call(7, wm2_group_begin, &F);
    rc |= call(7, wm2_group_begin, &F);
    flush(7);
    rc |= call(7, wm2_group_begin, &F);
    rc |= call(7, wm2_group_begin, &F);
    flush(7);
    if (rc != OL_OK || wm2_state_err_count(7) != 4) {
        snprintf(buf, sizeof buf, "watermark not reset at flush (rc %ld, count %u)", rc, wm2_state_err_count(7));
        fail(C, buf); bad++;
    }
    wm2_state_win_reset(6);
    wm2_state_win_reset(7);
    if (wm2_state_err_count(6) || wm2_state_err_count(7)) { fail(C, "reset did not clear the count"); bad++; }
    if (!bad) printf("ok   %s: 3 levels auto-closed LIFO (count 3), watermark reset, reset clears\n", C);
}

/* ---- hidpi-radius ---------------------------------------------------------- */
static int near_px(double v) { return (int)floor(v + 0.5); }

static void t_hidpi(void)
{
    const char *C = "hidpi-radius";
    int bad = 0;
    char buf[200];
    const int pcts[2] = { 125, 150 };
    for (int i = 0; i < 2; i++) {
        pct = pcts[i];
        surf_init(8, 200, 160, 81 + i);
        oracle_init(8);
        /* 24.8 inputs chosen exact in 24.8 after scaling */
        int x = P(7) + 128, y = P(5), w = P(80) + 64, h = P(60) + 128;
        int rr[4] = { P(8), P(5), P(3) + 128, 0 };
        struct logit_clip2 c = mkclip(x, y, w, h, rr[0], rr[1], rr[2], rr[3], 0);
        double s = pct / 100.0;
        int ex0 = near_px(x / 256.0 * s), ey0 = near_px(y / 256.0 * s);
        int ex1 = near_px((x + w) / 256.0 * s), ey1 = near_px((y + h) / 256.0 * s);
        int er[4];
        for (int k = 0; k < 4; k++) er[k] = near_px(rr[k] / 256.0 * s);
        long rc = call(8, wm2_clip_push, &c);
        int clip_ok = surf_clip_is(8, 1, ex0, ey0, ex1, ey1);
        child_on(8, 801);
        rc |= call(8, wm2_clip_pop, 0);
        struct og g;
        og_begin(&g, ex0, ey0, ex1 - ex0, ey1 - ey0, er, 255);
        child(&O, 801);
        og_end(&g);
        if (rc != OL_OK) { fail(C, "push/pop failed"); bad++; }
        if (!clip_ok) {
            snprintf(buf, sizeof buf, "%d%%: surface clip %d,%d-%d,%d, want %d,%d-%d,%d", pct,
                     SURF[8].clx0, SURF[8].cly0, SURF[8].clx1, SURF[8].cly1, ex0, ey0, ex1, ey1);
            fail(C, buf); bad++;
        }
        snprintf(buf, sizeof buf, "%d%%: radii %d/%d/%d/%d px", pct, er[0], er[1], er[2], er[3]);
        bad += check_pixels(C, buf, 8);
        if (!bad) printf("     %s %d%%: rect %d,%d-%d,%d radii %d/%d/%d/%d px\n", C, pct,
                         ex0, ey0, ex1, ey1, er[0], er[1], er[2], er[3]);
    }
    if (!bad) printf("ok   %s: edges and radii at 125%% and 150%%\n", C);
}

/* ---- ledger ---------------------------------------------------------------- */
static void t_ledger(void)
{
    const char *C = "ledger";
    int bad = 0;
    pct = 100;
    surf_init(9, 50, 40, 91);
    surf_init(10, 50, 40, 92);
    struct logit_clip2 a1 = mkclip(P(2), P(2), P(40), P(30), 0, 0, 0, 0, LOGIT_RECT_SNAP);
    struct logit_clip2 a2 = mkclip(P(5), P(5), P(20), P(20), 0, 0, 0, 0, LOGIT_RECT_SNAP);
    struct logit_clip2 b1 = mkclip(P(10), P(10), P(10), P(10), 0, 0, 0, 0, LOGIT_RECT_SNAP);

    /* interleaved, equal replaced clips: out-of-order pop is exact */
    long rc = call(9, wm2_clip_push, &a1);
    rc |= call(10, wm2_clip_push, &b1);
    rc |= call(9, wm2_clip_pop, 0);
    int okA = surf_clip_is(9, 0, 0, 0, 0, 0) && surf_clip_is(10, 1, 10, 10, 20, 20) && fdepth == 1;
    rc |= call(10, wm2_clip_pop, 0);
    if (rc != OL_OK || !okA || SURF[10].clip_on || fdepth) { fail(C, "interleaved pops over equal clips"); bad++; }

    /* unequal: the pop that cannot be exact is refused, nothing changes */
    rc = call(9, wm2_clip_push, &a1);
    rc |= call(9, wm2_clip_push, &a2);
    rc |= call(10, wm2_clip_push, &b1);
    long st = call(9, wm2_clip_pop, 0);
    int okB = st == OL_STATE && surf_clip_is(9, 1, 5, 5, 25, 25) && fdepth == 3;
    rc |= call(10, wm2_clip_pop, 0);
    rc |= call(9, wm2_clip_pop, 0);
    int okC = surf_clip_is(9, 1, 2, 2, 42, 32);
    rc |= call(9, wm2_clip_pop, 0);
    if (rc != OL_OK || !okB || !okC || SURF[9].clip_on || fdepth) { fail(C, "unequal interleave"); bad++; }

    /* reset orphans the pushes; the next call reclaims them */
    rc = call(9, wm2_clip_push, &a1);
    rc |= call(9, wm2_clip_push, &a2);
    wm2_state_win_reset(9);
    int okD = fdepth == 2;
    SURF[9].clip_on = 0;                       /* wm.c clears the clip on resize */
    st = call(9, wm2_clip_pop, 0);
    if (rc != OL_OK || !okD || st != OL_STATE || fdepth || SURF[9].clip_on) { fail(C, "reset orphans / reclaim"); bad++; }

    /* flush cannot pop under another window's push: orphan, then reclaim */
    rc = call(9, wm2_clip_push, &a1);
    rc |= call(9, wm2_clip_push, &a2);
    rc |= call(10, wm2_clip_push, &b1);
    flush(9);
    int okE = fdepth == 3 && wm2_state_err_count(9) == 2;
    flush(10);
    if (rc != OL_OK || !okE || fdepth || SURF[10].clip_on) { fail(C, "flush under another push"); bad++; }
    wm2_state_win_reset(9);
    wm2_state_win_reset(10);
    if (!bad) printf("ok   %s: interleaved pops, refusal, orphans reclaimed\n", C);
}

/* ---- args ------------------------------------------------------------------ */
static void t_args(void)
{
    const char *C = "args";
    int bad = 0;
    char buf[120];
    pct = 100;
    surf_init(11, 40, 30, 111);
    uint8_t img[16 * 4];
    memset(img, 255, sizeof img);
    struct { const char *what; handler fn; long want; union { struct logit_clip2 c; struct logit_group2 g; struct logit_blit2 b; } u; } t[16];
    int n = 0;
#define CASE(W, FN, WANT) (t[n].what = (W), t[n].fn = (FN), t[n].want = (WANT), &t[n++].u)
    struct logit_clip2 *c;
    struct logit_group2 *g;
    struct logit_blit2 *b;
    c = &CASE("clip flags bit 1", wm2_clip_push, OL_ARGUMENT)->c; *c = mkclip(0, 0, P(4), P(4), 0, 0, 0, 0, 2);
    c = &CASE("clip w < 0", wm2_clip_push, OL_ARGUMENT)->c; *c = mkclip(0, 0, -1, P(4), 0, 0, 0, 0, 0);
    c = &CASE("clip radius < 0", wm2_clip_push, OL_ARGUMENT)->c; *c = mkclip(0, 0, P(4), P(4), 0, -1, 0, 0, 0);
    c = &CASE("clip size", wm2_clip_push, OL_ARGUMENT)->c; *c = mkclip(0, 0, P(4), P(4), 0, 0, 0, 0, 0); c->size -= 4;
    g = &CASE("group opacity 256", wm2_group_begin, OL_ARGUMENT)->g; *g = mkgroup(0, 0, P(4), P(4), 0, 256, 0);
    g = &CASE("group opacity -1", wm2_group_begin, OL_ARGUMENT)->g; *g = mkgroup(0, 0, P(4), P(4), 0, -1, 0);
    g = &CASE("group flags bit 3", wm2_group_begin, OL_ARGUMENT)->g; *g = mkgroup(0, 0, P(4), P(4), 0, 9, 8);
    b = &CASE("blit filter 3", wm2_blit2, OL_ARGUMENT)->b; *b = mkblit(img, 4, 4, 16, 0, 0, 4, 4, 0, 0, P(4), P(4), 255, 3, 0);
    b = &CASE("blit flags bit 2", wm2_blit2, OL_ARGUMENT)->b; *b = mkblit(img, 4, 4, 16, 0, 0, 4, 4, 0, 0, P(4), P(4), 255, 0, 4);
    b = &CASE("blit sw 0", wm2_blit2, OL_ARGUMENT)->b; *b = mkblit(img, 0, 4, 16, 0, 0, 4, 4, 0, 0, P(4), P(4), 255, 0, 0);
    b = &CASE("blit stride < sw*4", wm2_blit2, OL_ARGUMENT)->b; *b = mkblit(img, 4, 4, 12, 0, 0, 4, 4, 0, 0, P(4), P(4), 255, 0, 0);
    b = &CASE("blit src rect outside", wm2_blit2, OL_ARGUMENT)->b; *b = mkblit(img, 4, 4, 16, 1, 0, 4, 4, 0, 0, P(4), P(4), 255, 0, 0);
    b = &CASE("blit rgba NULL", wm2_blit2, OL_ARGUMENT)->b; *b = mkblit(0, 4, 4, 16, 0, 0, 4, 4, 0, 0, P(4), P(4), 255, 0, 0);
    b = &CASE("blit opacity 300", wm2_blit2, OL_ARGUMENT)->b; *b = mkblit(img, 4, 4, 16, 0, 0, 4, 4, 0, 0, P(4), P(4), 300, 0, 0);
    b = &CASE("blit empty dest", wm2_blit2, OL_OK)->b; *b = mkblit(img, 4, 4, 16, 0, 0, 4, 4, 0, 0, 0, P(4), 255, 0, 0);
    for (int i = 0; i < n; i++) {
        long rc = call(11, t[i].fn, &t[i].u);
        if (rc != t[i].want) { snprintf(buf, sizeof buf, "%s returned %ld, want %ld", t[i].what, rc, t[i].want); fail(C, buf); bad++; }
    }
    if (fdepth != 0 || SURF[11].clip_on) { fail(C, "a refused call changed the stack"); bad++; }
    /* kind mismatches */
    struct logit_clip2 sq = mkclip(0, 0, P(10), P(10), 0, 0, 0, 0, 0);
    struct logit_group2 gg = mkgroup(0, 0, P(10), P(10), 0, 50, 0);
    long r1 = call(11, wm2_group_end, 0);
    long r2 = call(11, wm2_clip_push, &sq);
    long r3 = call(11, wm2_group_end, 0);
    long r4 = call(11, wm2_group_begin, &gg);
    long r5 = call(11, wm2_clip_pop, 0);
    long r6 = call(11, wm2_group_end, 0);
    long r7 = call(11, wm2_clip_pop, 0);
    if (r1 != OL_STATE || r2 || r3 != OL_STATE || r4 || r5 != OL_STATE || r6 || r7 || fdepth) {
        fail(C, "LIFO kind checks"); bad++;
    }
    struct wm2_env e;
    env_for(&e, 11);
    e.slot = WM2_MAX_WINDOWS;
    if (wm2_clip_pop(&e, 0, 0, 0) != OL_STATE || wm2_state_err_count(-1) || wm2_state_err_count(WM2_MAX_WINDOWS)) {
        fail(C, "out-of-range slot"); bad++;
    }
    wm2_state_win_reset(-1);
    wm2_state_win_reset(WM2_MAX_WINDOWS);
    if (!bad) printf("ok   %s: %d malformed calls refused, LIFO kinds, slot range\n", C, n);
}

int main(void)
{
    if ((wm2_state_info & (LOGIT_GUI2_F_CLIP | LOGIT_GUI2_F_GROUP | LOGIT_GUI2_F_BLIT2)) !=
        (LOGIT_GUI2_F_CLIP | LOGIT_GUI2_F_GROUP | LOGIT_GUI2_F_BLIT2))
        fail("info", "wm2_state_info lacks CLIP|GROUP|BLIT2");
    t_rebind_clip();
    t_group_oracle();
    t_blit2_ref();
    t_arena_limit();
    t_flush();
    t_hidpi();
    t_ledger();
    t_args();
    if (failed) { printf("wm-draw2-state: FAILED\n"); return 1; }
    printf("wm-draw2-state: all checks passed\n");
    return 0;
}
