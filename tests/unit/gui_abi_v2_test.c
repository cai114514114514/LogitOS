/* GUI ABI v2 transport and stubs (gui-abi-v2): the contract every later v2
 * item builds on, checked before any of them exists.
 *
 *   layout        sizeof/offsetof of every v2 struct (LP64), at compile time
 *   wire          the ring-3 wrappers (tests/unit/gui_abi_v2_wire.c)
 *   scale-parity  wm2_pt == fb_pt for p in [-2000, 2000] at 100..300%
 *   pt24-floor    wm2_pt24 floors like fb_pt, fraction kept
 *   rect24        abutting 5-pt columns at 150% are 7 and 8 px with SNAP,
 *                 7.5 and 7.5 without; SNAP edges equal the legacy S() idiom
 *   router        OL_UNSUPPORTED per number, OL_ARGUMENT for a wrong size,
 *                 INFO(0/1/2); no handler touches a pixel
 *   fb-clip       fb_clip_push is OL_UNSUPPORTED, depth 0, clip untouched
 *   wm-switch     every new number is in wm.c's wm_domain_call, and the
 *                 pixel-writing ones (only those) in is_draw_call
 *
 * Linked host-side with fb.c, glass.c, $(GFX_SRC), wm_draw2*.c and
 * wm_backdrop.c, like test-fb-clip. text.c is NOT linked, so this file
 * defines the carets INFO constant text.c owns in the kernel.
 *
 * THE CONTROL (-DGUI_ABI_NEGCTL) makes wm2_pt truncate toward zero -- the
 * shear fb.c's fb_pt comment documents -- and must fail exactly
 * 'FAIL scale-parity'. Every check prints `FAIL <check>` on failure, and the
 * process exits non-zero. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <ctype.h>

#include "fb.h"
#include "wm_draw2.h"
#include "wm2_scale.h"

const unsigned wm2_carets_info = 0;   /* text.c's, in the kernel */
int gav2_wire_run(void);              /* gui_abi_v2_wire.c */

/* ---- the world fb.c expects, stubbed (as tests/unit/fb_clip_test.c) ---- */
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

/* ---- layout (LP64): the numbers tools/gen_abi.py also asserts ---------- */
#define OFF(t, f, n) _Static_assert(offsetof(struct t, f) == (n), #t "." #f)
#define SZ(t, n)     _Static_assert(sizeof(struct t) == (n), "sizeof " #t)
SZ(logit_path2, 64);
OFF(logit_path2, size, 0); OFF(logit_path2, verbs, 8); OFF(logit_path2, xy, 16);
OFF(logit_path2, nverb, 24); OFF(logit_path2, nxy, 28); OFF(logit_path2, rule, 32);
OFF(logit_path2, m, 36); OFF(logit_path2, flags, 60);
SZ(logit_paint2, 168);
OFF(logit_paint2, size, 0); OFF(logit_paint2, kind, 4); OFF(logit_paint2, op, 5);
OFF(logit_paint2, spread, 6); OFF(logit_paint2, nstop, 7); OFF(logit_paint2, argb, 8);
OFF(logit_paint2, x0, 12); OFF(logit_paint2, y0, 16); OFF(logit_paint2, x1, 20);
OFF(logit_paint2, y1, 24); OFF(logit_paint2, r0, 28); OFF(logit_paint2, r1, 32);
OFF(logit_paint2, stop_pos, 36); OFF(logit_paint2, stop_argb, 100); OFF(logit_paint2, flags, 164);
SZ(logit_stroke2, 52);
OFF(logit_stroke2, size, 0); OFF(logit_stroke2, width, 4); OFF(logit_stroke2, cap, 8);
OFF(logit_stroke2, join, 9); OFF(logit_stroke2, ndash, 10); OFF(logit_stroke2, pad, 11);
OFF(logit_stroke2, miter, 12); OFF(logit_stroke2, dash, 16); OFF(logit_stroke2, dash_phase, 48);
SZ(logit_clip2, 40);
OFF(logit_clip2, size, 0); OFF(logit_clip2, x, 4); OFF(logit_clip2, y, 8); OFF(logit_clip2, w, 12);
OFF(logit_clip2, h, 16); OFF(logit_clip2, radius, 20); OFF(logit_clip2, flags, 36);
SZ(logit_group2, 44);
OFF(logit_group2, size, 0); OFF(logit_group2, x, 4); OFF(logit_group2, h, 16);
OFF(logit_group2, radius, 20); OFF(logit_group2, opacity, 36); OFF(logit_group2, flags, 40);
SZ(logit_blit2, 72);
OFF(logit_blit2, size, 0); OFF(logit_blit2, rgba, 8); OFF(logit_blit2, sw, 16); OFF(logit_blit2, sh, 20);
OFF(logit_blit2, sstride, 24); OFF(logit_blit2, src_x, 28); OFF(logit_blit2, src_y, 32);
OFF(logit_blit2, src_w, 36); OFF(logit_blit2, src_h, 40); OFF(logit_blit2, x, 44);
OFF(logit_blit2, y, 48); OFF(logit_blit2, w, 52); OFF(logit_blit2, h, 56);
OFF(logit_blit2, opacity, 60); OFF(logit_blit2, filter, 64); OFF(logit_blit2, flags, 68);
SZ(logit_run2, 48);
OFF(logit_run2, size, 0); OFF(logit_run2, x, 4); OFF(logit_run2, y, 8); OFF(logit_run2, px, 12);
OFF(logit_run2, face, 16); OFF(logit_run2, color, 20); OFF(logit_run2, s, 24); OFF(logit_run2, len, 32);
OFF(logit_run2, alpha, 36); OFF(logit_run2, weight, 40); OFF(logit_run2, flags, 44);
SZ(logit_shadow2, 60);
OFF(logit_shadow2, size, 0); OFF(logit_shadow2, x, 4); OFF(logit_shadow2, radius, 20);
OFF(logit_shadow2, dx, 36); OFF(logit_shadow2, dy, 40); OFF(logit_shadow2, blur, 44);
OFF(logit_shadow2, spread, 48); OFF(logit_shadow2, argb, 52); OFF(logit_shadow2, flags, 56);
SZ(logit_glass2, 76);
OFF(logit_glass2, size, 0); OFF(logit_glass2, x, 4); OFF(logit_glass2, radius, 20);
OFF(logit_glass2, cut, 36); OFF(logit_glass2, tint_argb, 40); OFF(logit_glass2, opacity, 44);
OFF(logit_glass2, tone, 48); OFF(logit_glass2, blur_level, 52); OFF(logit_glass2, rim, 56);
OFF(logit_glass2, fg_rgb, 60); OFF(logit_glass2, flags, 64); OFF(logit_glass2, reserved, 68);
SZ(logit_backdrop2, 84);
OFF(logit_backdrop2, size, 0); OFF(logit_backdrop2, id, 4); OFF(logit_backdrop2, x, 8);
OFF(logit_backdrop2, h, 20); OFF(logit_backdrop2, radius, 24); OFF(logit_backdrop2, tint_argb, 40);
OFF(logit_backdrop2, opacity, 44); OFF(logit_backdrop2, tone, 48); OFF(logit_backdrop2, blur_level, 52);
OFF(logit_backdrop2, rim, 56); OFF(logit_backdrop2, fill_rgb, 60); OFF(logit_backdrop2, flags, 64);
OFF(logit_backdrop2, reserved, 68);
SZ(logit_text_carets_query, 64);
OFF(logit_text_carets_query, size, 0); OFF(logit_text_carets_query, version, 4);
OFF(logit_text_carets_query, s, 8); OFF(logit_text_carets_query, len, 16);
OFF(logit_text_carets_query, px, 20); OFF(logit_text_carets_query, face, 24);
OFF(logit_text_carets_query, flags, 28); OFF(logit_text_carets_query, off, 32);
OFF(logit_text_carets_query, x24_8, 40); OFF(logit_text_carets_query, max, 48);
OFF(logit_text_carets_query, count_out, 56);
_Static_assert(OL_UNSUPPORTED == -3 && OL_ARGUMENT == -1, "status values in logit_abi.h's comment");
_Static_assert(LOGIT_TEXT_CARETS_QUERY == -2 && LOGIT_TEXT_METRICS_QUERY == -1, "query selectors");

/* ---- reporting ---------------------------------------------------------- */
static int fails;
static int check_fails(const char *name, int bad, const char *what)
{
    if (bad) { printf("FAIL %s: %d %s\n", name, bad, what); fails++; }
    else printf("ok   %s: %s\n", name, what);
    return bad;
}

/* ---- scale -------------------------------------------------------------- */
static const int pcts[] = { 100, 125, 150, 175, 200, 300 };
#define NPCT ((int)(sizeof pcts / sizeof pcts[0]))

static void test_scale_parity(void)
{
    int bad = 0;
    for (int i = 0; i < NPCT; i++) {
        fb_set_scale(pcts[i]);
        for (int p = -2000; p <= 2000; p++)
            if (wm2_pt(p, pcts[i]) != fb_pt(p)) {
                if (bad < 4) printf("     scale-parity: p=%d at %d%%: wm2_pt %d, fb_pt %d\n",
                                    p, pcts[i], wm2_pt(p, pcts[i]), fb_pt(p));
                bad++;
            }
    }
    fb_set_scale(100);
    check_fails("scale-parity", bad, "wm2_pt == fb_pt over [-2000,2000] x 6 scales");
}

static void test_pt24(void)
{
    int bad = 0;
    for (int i = 0; i < NPCT; i++) {
        fb_set_scale(pcts[i]);
        for (int p = -2000; p <= 2000; p++) {
            int v = wm2_pt24(p * 256, pcts[i]);
            /* floor(v / 256) (arithmetic shift floors) is the integer rule */
            if ((v >> 8) != fb_pt(p)) bad++;
            /* and the fraction is the exact one: v = floor(p*256*pct/100) */
            long long exact = (long long)p * 256 * pcts[i];
            long long fl = exact >= 0 ? exact / 100 : -((-exact + 99) / 100);
            if (v != fl) bad++;
        }
    }
    fb_set_scale(100);
    /* no 32-bit wrap: a large 24.8 value at 400% saturates, never flips sign */
    if (wm2_pt24(0x7FFFFF00, 400) != 0x7FFFFFFF) bad++;
    if (wm2_pt24(-0x7FFFFF00, 400) != (int)0x80000000) bad++;
    check_fails("pt24-floor", bad, "wm2_pt24 floors toward -inf, keeps the fraction, saturates");
}

static void test_rect24(void)
{
    int bad = 0;
    /* two abutting 5-point columns at 150% */
    struct wm2_rect24 a = wm2_rect24(0, 0, 5 * 256, 256, 150, 1);
    struct wm2_rect24 b = wm2_rect24(5 * 256, 0, 5 * 256, 256, 150, 1);
    if (a.x != 0 || a.w != 7 * 256 || b.x != 7 * 256 || b.w != 8 * 256) {
        printf("     rect24 SNAP: a=(%d,+%d) b=(%d,+%d), want (0,+1792) (1792,+2048)\n",
               a.x, a.w, b.x, b.w);
        bad++;
    }
    if (a.x + a.w != b.x) bad++;                       /* abutting */
    struct wm2_rect24 fa = wm2_rect24(0, 0, 5 * 256, 256, 150, 0);
    struct wm2_rect24 fbr = wm2_rect24(5 * 256, 0, 5 * 256, 256, 150, 0);
    if (fa.w != 1920 || fbr.x != 1920 || fbr.w != 1920) {
        printf("     rect24 AA: a.w=%d b=(%d,+%d), want 1920 (1920,+1920)\n", fa.w, fbr.x, fbr.w);
        bad++;
    }
    /* SNAP on whole points is the legacy SYS_GUI_RECT idiom, edge for edge */
    for (int i = 0; i < NPCT; i++) {
        fb_set_scale(pcts[i]);
        for (int x = -60; x <= 60; x += 3)
            for (int w = 0; w <= 23; w++) {
                struct wm2_rect24 r = wm2_rect24(x * 256, -x * 256, w * 256, w * 256, pcts[i], 1);
                int lx = fb_pt(x), lw = fb_pt(x + w) - lx;
                int ly = fb_pt(-x), lh = fb_pt(-x + w) - ly;
                if (r.x != lx * 256 || r.w != lw * 256 || r.y != ly * 256 || r.h != lh * 256) bad++;
            }
    }
    fb_set_scale(100);
    check_fails("rect24", bad, "edge-difference conversion, SNAP = legacy integer rule");
}

/* ---- the router, against a host env bound to a real fb.c surface ------- */
#define SW 48
#define SH 32
static uint32_t surf_px[SW * SH];
static struct surface surf;
static uint8_t mbi[128];
static uint32_t screen_fb[64 * 48];
static int faulting;

static int host_copyin(void *dst, const void *src, unsigned long n)
{
    if (!src || faulting) return -1;
    memcpy(dst, src, n);
    return 0;
}
static int host_clip_push(void *o, int x0, int y0, int x1, int y1)
{ (void)o; return fb_clip_push(x0, y0, x1, y1); }
static void host_clip_pop(void *o) { (void)o; fb_clip_pop(); }
static void *host_alloc(unsigned long n) { return malloc(n); }
static void host_release(void *p) { free(p); }

static void init_fb(void)
{
    memset(mbi, 0, sizeof mbi);
    *(uint32_t *)mbi = sizeof mbi;
    uint8_t *p = mbi + 8;
    *(uint32_t *)(p + 0)  = 8;
    *(uint32_t *)(p + 4)  = 40;
    *(uint64_t *)(p + 8)  = (uint64_t)(uintptr_t)screen_fb;
    *(uint32_t *)(p + 16) = 64 * 4;
    *(uint32_t *)(p + 20) = 64;
    *(uint32_t *)(p + 24) = 48;
    p[28] = 32; p[29] = 1;
    p[32] = 16; p[33] = 8; p[34] = 8; p[35] = 8; p[36] = 0; p[37] = 8;
    if (!fb_init((uint64_t)(uintptr_t)mbi)) {
        printf("FAIL setup: fb_init refused the synthetic framebuffer tag\n");
        exit(1);
    }
}

static uint32_t crc_surface(void)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < SW * SH; i++) { h ^= surf_px[i]; h *= 16777619u; }
    return h;
}

static void env_init(struct wm2_env *e, int slot)
{
    memset(e, 0, sizeof *e);
    e->d = fb_display_cur();
    e->scale_pct = fb_scale();
    e->surf_w = surf.w; e->surf_h = surf.h;
    e->slot = slot;
    e->copyin = host_copyin;
    e->clip_push = host_clip_push;
    e->clip_pop = host_clip_pop;
    e->alloc = host_alloc;
    e->release = host_release;
    e->opaque = &surf;
}

static int expect(long got, long want, const char *what)
{
    if (got == want) return 0;
    printf("     router: %s returned %ld, want %ld\n", what, got, want);
    return 1;
}

/* One struct-taking number: right size -> UNSUPPORTED, wrong sizes, NULL and
 * a copyin fault -> ARGUMENT. */
static int struct_call(struct wm2_env *e, long num, const char *name, void *buf, unsigned size)
{
    int bad = 0;
    char what[96];
    unsigned *sz = (unsigned *)buf;
    memset(buf, 0, size);
    *sz = size;
    snprintf(what, sizeof what, "%s(size=%u)", name, size);
    bad += expect(wm2_dispatch(num, e, (long)buf, 0, 0), OL_UNSUPPORTED, what);
    const unsigned wrong[] = { 0, size - 4, size + 4, size + 1, 0xFFFFFFFFu };
    for (unsigned i = 0; i < sizeof wrong / sizeof wrong[0]; i++) {
        *sz = wrong[i];
        snprintf(what, sizeof what, "%s(size=%u)", name, wrong[i]);
        bad += expect(wm2_dispatch(num, e, (long)buf, 0, 0), OL_ARGUMENT, what);
    }
    *sz = size;
    snprintf(what, sizeof what, "%s(NULL)", name);
    bad += expect(wm2_dispatch(num, e, 0, 0, 0), OL_ARGUMENT, what);
    faulting = 1;
    snprintf(what, sizeof what, "%s(fault)", name);
    bad += expect(wm2_dispatch(num, e, (long)buf, 0, 0), OL_ARGUMENT, what);
    faulting = 0;
    return bad;
}

static void test_router(void)
{
    int bad = 0;
    for (int i = 0; i < SW * SH; i++) surf_px[i] = 0x00102030u + (uint32_t)i * 2654435761u % 0xFFFFFFu;
    surf.px = surf_px; surf.w = SW; surf.h = SH; surf.clip_on = 0;
    fb_target(&surf);
    struct wm2_env e;
    env_init(&e, 3);
    if (!e.d || e.d->target.px != surf_px) {
        printf("     router: fb_display_cur is not bound to the targeted surface\n");
        bad++;
    }
    uint32_t before = crc_surface();

    bad += expect(wm2_dispatch(SYS_GUI2_INFO, &e, 0, 0, 0), (long)LOGIT_GUI2_VERSION << 16, "INFO(0)");
    bad += expect(wm2_dispatch(SYS_GUI2_INFO, &e, 1, 0, 0), 0, "INFO(1)");
    bad += expect(wm2_dispatch(SYS_GUI2_INFO, &e, 2, 0, 0), OL_ARGUMENT, "INFO(2)");
    bad += expect(wm2_dispatch(SYS_GUI2_INFO, &e, -1, 0, 0), OL_ARGUMENT, "INFO(-1)");

    union { struct logit_path2 p; struct logit_paint2 q; struct logit_stroke2 s;
            struct logit_clip2 c; struct logit_group2 g; struct logit_blit2 b;
            struct logit_run2 r; struct logit_shadow2 sh; struct logit_glass2 gl;
            struct logit_backdrop2 bd; char pad[256]; } u;
    bad += struct_call(&e, SYS_GUI_CLIP_PUSH, "CLIP_PUSH", &u, sizeof(struct logit_clip2));
    bad += struct_call(&e, SYS_GUI_GROUP_BEGIN, "GROUP_BEGIN", &u, sizeof(struct logit_group2));
    bad += struct_call(&e, SYS_GUI_BLIT2, "BLIT2", &u, sizeof(struct logit_blit2));
    bad += struct_call(&e, SYS_GUI_TEXT_RUN2, "TEXT_RUN2", &u, sizeof(struct logit_run2));
    bad += struct_call(&e, SYS_GUI_SHADOW2, "SHADOW2", &u, sizeof(struct logit_shadow2));
    bad += struct_call(&e, SYS_GUI_GLASS2, "GLASS2", &u, sizeof(struct logit_glass2));
    bad += struct_call(&e, SYS_GUI_BACKDROP, "BACKDROP", &u, sizeof(struct logit_backdrop2));

    /* PATH: three structs, each size-checked; c = 0 means fill. */
    struct logit_path2 p; struct logit_paint2 q; struct logit_stroke2 s;
    memset(&p, 0, sizeof p); memset(&q, 0, sizeof q); memset(&s, 0, sizeof s);
    p.size = sizeof p; q.size = sizeof q; s.size = sizeof s;
    bad += expect(wm2_dispatch(SYS_GUI_PATH, &e, (long)&p, (long)&q, 0), OL_UNSUPPORTED, "PATH fill");
    bad += expect(wm2_dispatch(SYS_GUI_PATH, &e, (long)&p, (long)&q, (long)&s), OL_UNSUPPORTED, "PATH stroke");
    q.kind = LOGIT_PAINT2_CONIC; q.nstop = 16; q.op = 1; q.spread = 1;
    bad += expect(wm2_dispatch(SYS_GUI_PATH, &e, (long)&p, (long)&q, 0), OL_UNSUPPORTED, "PATH conic/op/spread/16 stops");
    q.kind = 0; q.nstop = 0; q.op = 0; q.spread = 0;
    p.size = 60;
    bad += expect(wm2_dispatch(SYS_GUI_PATH, &e, (long)&p, (long)&q, 0), OL_ARGUMENT, "PATH path.size=60");
    p.size = sizeof p; q.size = sizeof q + 4;
    bad += expect(wm2_dispatch(SYS_GUI_PATH, &e, (long)&p, (long)&q, 0), OL_ARGUMENT, "PATH paint.size+4");
    q.size = sizeof q; s.size = 0;
    bad += expect(wm2_dispatch(SYS_GUI_PATH, &e, (long)&p, (long)&q, (long)&s), OL_ARGUMENT, "PATH stroke.size=0");
    bad += expect(wm2_dispatch(SYS_GUI_PATH, &e, (long)&p, 0, 0), OL_ARGUMENT, "PATH paint NULL");

    bad += expect(wm2_dispatch(SYS_GUI_CLIP_POP, &e, 0, 0, 0), OL_UNSUPPORTED, "CLIP_POP");
    bad += expect(wm2_dispatch(SYS_GUI_GROUP_END, &e, 0, 0, 0), OL_UNSUPPORTED, "GROUP_END");
    for (long id = -1; id < LOGIT_BACKDROP_MAX; id++)
        bad += expect(wm2_dispatch(SYS_GUI_BACKDROP_CLEAR, &e, id, 0, 0), OL_UNSUPPORTED, "BACKDROP_CLEAR(id)");
    bad += expect(wm2_dispatch(SYS_GUI_BACKDROP_CLEAR, &e, LOGIT_BACKDROP_MAX, 0, 0), OL_ARGUMENT, "BACKDROP_CLEAR(8)");
    bad += expect(wm2_dispatch(SYS_GUI_BACKDROP_CLEAR, &e, -2, 0, 0), OL_ARGUMENT, "BACKDROP_CLEAR(-2)");

    /* not a v2 number; a slot outside the side tables */
    bad += expect(wm2_dispatch(SYS_GUI_RECT, &e, 0, 0, 0), OL_ARGUMENT, "legacy number");
    e.slot = WM2_MAX_WINDOWS;
    bad += expect(wm2_dispatch(SYS_GUI2_INFO, &e, 0, 0, 0), OL_STATE, "slot 16");
    e.slot = -1;
    bad += expect(wm2_dispatch(SYS_GUI2_INFO, &e, 0, 0, 0), OL_STATE, "slot -1");
    e.slot = 3;

    /* hooks and compositor queries are callable no-ops */
    for (int i = -1; i <= WM2_MAX_WINDOWS; i++) wm2_win_reset(i);
    wm2_win_flush(&e);
    bad += expect(wm2_state_err_count(3), 0, "wm2_state_err_count");
    struct wm2_region rg[LOGIT_BACKDROP_MAX];
    bad += expect(wm_backdrop_regions(3, rg, LOGIT_BACKDROP_MAX), 0, "wm_backdrop_regions");
    struct drect R = { 0, 0, SW, SH };
    wm_backdrop_composite(&R, 3, 0, 0, e.d);

    if (crc_surface() != before) {
        printf("     router: a stub handler touched the surface\n");
        bad++;
    }
    fb_target(NULL);
    check_fails("router", bad, "every v2 number: UNSUPPORTED / ARGUMENT / INFO, no pixel written");
}

static void test_fb_clip(void)
{
    int bad = 0;
    surf.px = surf_px; surf.w = SW; surf.h = SH;
    surf.clip_on = 1; surf.clx0 = 2; surf.cly0 = 3; surf.clx1 = 40; surf.cly1 = 30;
    fb_target(&surf);
    if (fb_clip_depth() != 0) bad++;
    if (fb_clip_push(5, 5, 10, 10) != OL_UNSUPPORTED) bad++;
    if (fb_clip_depth() != 0) bad++;
    fb_clip_pop();
    if (fb_clip_depth() != 0) bad++;
    if (!surf.clip_on || surf.clx0 != 2 || surf.cly0 != 3 || surf.clx1 != 40 || surf.cly1 != 30) bad++;
    fb_target(NULL);
    surf.clip_on = 0;
    check_fails("fb-clip", bad, "fb_clip_push OL_UNSUPPORTED, depth 0, clip unchanged");
}

/* ---- static check over wm.c's switches --------------------------------- */
static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    if (!s || fread(s, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(s); return NULL; }
    s[n] = 0;
    fclose(f);
    /* blank comments so a name in a comment never counts as a case */
    for (char *p = s; *p; p++) {
        if (p[0] == '/' && p[1] == '*') {
            char *q = strstr(p + 2, "*/");
            char *end = q ? q + 2 : p + strlen(p);
            for (; p < end; p++) if (*p != '\n') *p = ' ';
            p--;
        } else if (p[0] == '/' && p[1] == '/') {
            for (; *p && *p != '\n'; p++) *p = ' ';
            if (!*p) break;
        }
    }
    return s;
}
/* The body of `static int <fn>(long num)` up to the first "\n}" */
static int body_of(const char *src, const char *sig, const char **b, const char **e)
{
    const char *p = strstr(src, sig);
    if (!p) return 0;
    p = strchr(p, '{');
    if (!p) return 0;
    const char *q = strstr(p, "\n}");
    if (!q) return 0;
    *b = p; *e = q;
    return 1;
}
/* `case NAME:` -- NAME as a whole token, so SYS_GUI_BLIT never matches
 * SYS_GUI_BLIT2 and SYS_GUI_BACKDROP never matches SYS_GUI_BACKDROP_CLEAR. */
static int has_case(const char *b, const char *e, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = strstr(b, name); p && p < e; p = strstr(p + 1, name)) {
        const char *t = p + n;
        if (isalnum((unsigned char)*t) || *t == '_') continue;
        while (*t == ' ' || *t == '\t') t++;
        if (*t != ':') continue;
        const char *k = p;
        while (k > b && (k[-1] == ' ' || k[-1] == '\t' || k[-1] == '\n')) k--;
        if (k - b >= 4 && !strncmp(k - 4, "case", 4)) return 1;
    }
    return 0;
}

static void test_wm_switches(const char *wm_path)
{
    static const char *pixel[] = { "SYS_GUI_PATH", "SYS_GUI_GROUP_END", "SYS_GUI_BLIT2",
        "SYS_GUI_TEXT_RUN2", "SYS_GUI_SHADOW2", "SYS_GUI_GLASS2", "SYS_GUI_BACKDROP" };
    static const char *state[] = { "SYS_GUI2_INFO", "SYS_GUI_CLIP_PUSH", "SYS_GUI_CLIP_POP",
        "SYS_GUI_GROUP_BEGIN", "SYS_GUI_BACKDROP_CLEAR" };
    int bad = 0;
    char *src = slurp(wm_path);
    if (!src) { printf("FAIL wm-switch: cannot read %s\n", wm_path); fails++; return; }
    const char *db, *de, *pb, *pe, *gb, *ge;
    if (!body_of(src, "static int wm_domain_call(long num)\n{", &db, &de) ||
        !body_of(src, "static int is_draw_call(long num)\n{", &pb, &pe) ||
        !body_of(src, "static long wm_gui_dispatch(long num, long a, long b, long c)\n{", &gb, &ge)) {
        printf("FAIL wm-switch: wm_domain_call / is_draw_call / wm_gui_dispatch not found\n");
        fails++; free(src); return;
    }
    /* Each body ends at the first "\n}" -- the function's closing brace,
     * the only brace at column 0. */
    for (unsigned i = 0; i < sizeof pixel / sizeof *pixel; i++) {
        if (!has_case(db, de, pixel[i])) { printf("     wm-switch: %s missing from wm_domain_call\n", pixel[i]); bad++; }
        if (!has_case(pb, pe, pixel[i])) { printf("     wm-switch: %s missing from is_draw_call\n", pixel[i]); bad++; }
        if (!has_case(gb, ge, pixel[i])) { printf("     wm-switch: %s missing from wm_gui_dispatch\n", pixel[i]); bad++; }
    }
    for (unsigned i = 0; i < sizeof state / sizeof *state; i++) {
        if (!has_case(db, de, state[i])) { printf("     wm-switch: %s missing from wm_domain_call\n", state[i]); bad++; }
        if (has_case(pb, pe, state[i])) { printf("     wm-switch: %s writes no pixel but is in is_draw_call\n", state[i]); bad++; }
        if (!has_case(gb, ge, state[i])) { printf("     wm-switch: %s missing from wm_gui_dispatch\n", state[i]); bad++; }
    }
    /* the checker itself: a legacy pixel call is found, a non-call is not */
    if (!has_case(pb, pe, "SYS_GUI_BLIT") || has_case(pb, pe, "SYS_GUI_CLIP")) {
        printf("     wm-switch: the case matcher is broken\n");
        bad++;
    }
    free(src);
    check_fails("wm-switch", bad, "12 numbers in wm_domain_call + wm_gui_dispatch, 7 in is_draw_call");
}

int main(int argc, char **argv)
{
    const char *wm_path = argc > 1 ? argv[1] : "c/kernel/gui/wm.c";
    init_fb();
    printf("ok   layout: 11 v2 structs (static asserts)\n");
    if (gav2_wire_run()) fails++;
    test_scale_parity();
    test_pt24();
    test_rect24();
    test_router();
    test_fb_clip();
    test_wm_switches(wm_path);
    if (fails) { printf("gui-abi-v2: %d check(s) FAILED\n", fails); return 1; }
    printf("gui-abi-v2: all checks passed\n");
    return 0;
}
