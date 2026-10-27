/* SYS_GUI_PATH handler (wm2_path) against direct ol_display_fill_path /
 * ol_display_stroke_path, with a host env whose copyin enforces bounds.
 *
 *   oracle     a circle (solid, radial) and a stroked rect (solid, linear) are
 *              pixel-identical to the direct display calls; a matrix and the
 *              surface clip too
 *   malformed  a table of bad calls: exact status and the target CRC unchanged
 *              ('FAIL malformed <case> touched pixels' otherwise)
 *   scale      125/150/200%: coverage equals a device-space reference; an
 *              integer-point rect with SNAP lands on the wm2_rect24 edges
 *   unsupported conic, op, spread, inner radius, 9 stops: OL_UNSUPPORTED, no pixel
 *   scratch    the copy-in lives in the display's own OL_DS_USER slot
 *
 * Does not link fb.c. THE CONTROL (-DWM_DRAW2_NO_VALIDATE) drops the verb
 * sequence / count checks; the malformed table must then report
 * 'FAIL malformed <case> touched pixels'. Exits 1 on any failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "wm_draw2.h"
#include "wm2_scale.h"
#include "openlogit_display_path.h"

int (*gfx_ctx_hook)(void);   /* no fb.c here */

static int bad;
#define FAIL(...) do { printf("FAIL " __VA_ARGS__); printf("\n"); bad = 1; } while (0)

static void *t_alloc(unsigned long n) { return malloc(n); }
static void t_free(void *p) { free(p); }

/* ---- the env: copyin that only reads what the test declared -------------- */
#define MAXREG 16
static struct { const char *p; size_t n; } reg[MAXREG];
static int nreg;
static const char *fault_lo, *fault_hi;      /* any read touching [lo,hi) faults */
static int oob_reads;

static void declare(const void *p, size_t n) { if (nreg < MAXREG) { reg[nreg].p = p; reg[nreg].n = n; nreg++; } }
static int t_copyin(void *dst, const void *src, unsigned long n)
{
    const char *s = src;
    if (fault_lo && s < fault_hi && s + n > fault_lo) return -1;
    for (int i = 0; i < nreg; i++)
        if (s >= reg[i].p && s + n <= reg[i].p + reg[i].n) { memcpy(dst, src, n); return 0; }
    oob_reads++;
    return -1;
}

enum { W = 96, H = 64 };
struct rig { struct ol_display d; uint32_t px[W * H]; struct wm2_env e; };

static uint32_t init_px(int x, int y)
{
    return (uint32_t)(((x * 7 + y * 3) & 255) << 16 | ((x * 3 + y * 11 + 40) & 255) << 8 | ((x * 5 + y * 5 + 90) & 255));
}
static void rig_init(struct rig *r, int pct)
{
    memset(r, 0, sizeof *r);
    struct ol_pixel_target t = { r->px, W, H, 0, 0, 0, 0, 0 };
    r->d.allocate = t_alloc; r->d.release = t_free;
    ol_display_bind(&r->d, &t, 16, 8, 0);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) r->px[y * W + x] = init_px(x, y);
    r->e.d = &r->d; r->e.scale_pct = pct; r->e.surf_w = W; r->e.surf_h = H;
    r->e.copyin = t_copyin; r->e.alloc = malloc; r->e.release = free;
}
static void rig_free(struct rig *r) { ol_display_destroy(&r->d); }
static void rig_clip(struct rig *r, int x0, int y0, int x1, int y1)
{
    r->d.target.clip_on = 1; r->d.target.clx0 = x0; r->d.target.cly0 = y0;
    r->d.target.clx1 = x1; r->d.target.cly1 = y1;
}
static uint32_t crc(const struct rig *r)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < W * H; i++) { h ^= r->px[i]; h *= 16777619u; }
    return h;
}
static int same(const struct rig *a, const struct rig *b) { return !memcmp(a->px, b->px, sizeof a->px); }

/* ---- a call, as the app would lay it out in its memory ------------------- */
#define FX(v) ((v) * 256)
struct call {
    struct logit_path2 p; struct logit_paint2 q; struct logit_stroke2 s; int has_s;
    unsigned char verbs[4200]; int xy[8300]; unsigned sp[16], sa[16];
    unsigned vdecl, xdecl;       /* bytes / ints the test declares readable */
};
static void call_init(struct call *c)
{
    memset(c, 0, sizeof *c);
    c->p.size = sizeof c->p; c->q.size = sizeof c->q; c->s.size = sizeof c->s;
    c->q.argb = 0xFF2060C0u;
    c->p.verbs = c->verbs; c->p.xy = c->xy;
    c->q.stop_pos = c->sp; c->q.stop_argb = c->sa;
}
static void vtx(struct call *c, int v, int n, const int *pts)
{
    c->verbs[c->p.nverb++] = (unsigned char)v;
    for (int i = 0; i < n * 2; i++) c->xy[c->p.nxy++] = pts[i];
    c->vdecl = c->p.nverb; c->xdecl = c->p.nxy;
}
static void M(struct call *c, int x, int y) { int p[2] = { x, y }; vtx(c, LOGIT_VERB_M, 1, p); }
static void L(struct call *c, int x, int y) { int p[2] = { x, y }; vtx(c, LOGIT_VERB_L, 1, p); }
static void Z(struct call *c) { vtx(c, LOGIT_VERB_Z, 0, 0); }
static void C3(struct call *c, int a, int b, int d, int e, int f, int g)
{ int p[6] = { a, b, d, e, f, g }; vtx(c, LOGIT_VERB_C, 3, p); }
static void circle(struct call *c, int cx, int cy, int r)    /* 24.8; k kept a multiple of 4 */
{
    int k = (int)((long long)r * 36195 >> 16) & ~3;
    M(c, cx + r, cy);
    C3(c, cx + r, cy + k, cx + k, cy + r, cx, cy + r);
    C3(c, cx - k, cy + r, cx - r, cy + k, cx - r, cy);
    C3(c, cx - r, cy - k, cx - k, cy - r, cx, cy - r);
    C3(c, cx + k, cy - r, cx + r, cy - k, cx + r, cy);
    Z(c);
}
static void rect(struct call *c, int x, int y, int w, int h)
{ M(c, x, y); L(c, x + w, y); L(c, x + w, y + h); L(c, x, y + h); Z(c); }
static void triangle(struct call *c) { M(c, FX(10), FX(10)); L(c, FX(60), FX(14)); L(c, FX(30), FX(50)); Z(c); }

static void stops2(struct call *c, int n)
{
    for (int i = 0; i < n; i++) { c->sp[i] = (unsigned)(i * 65536 / (n > 1 ? n - 1 : 1)); c->sa[i] = 0xFF000000u | (unsigned)(0x30 * i + 0x40) << 16 | 0x2080u; }
    c->q.nstop = (unsigned char)n;
}
static void linear(struct call *c)
{
    c->q.kind = LOGIT_PAINT2_LINEAR; c->q.x0 = FX(10); c->q.y0 = FX(6); c->q.x1 = FX(70); c->q.y1 = FX(46);
    c->sp[0] = 0; c->sa[0] = 0xFFFF3010u; c->sp[1] = 65536; c->sa[1] = 0x7810E0A0u; c->q.nstop = 2;
}
static void radial(struct call *c)
{
    c->q.kind = LOGIT_PAINT2_RADIAL; c->q.x0 = FX(40); c->q.y0 = FX(30); c->q.r1 = FX(30);
    c->sp[0] = 0; c->sa[0] = 0xFFFFFFFFu; c->sp[1] = 32768; c->sa[1] = 0xDCE0A010u;
    c->sp[2] = 65536; c->sa[2] = 0xFF401080u; c->q.nstop = 3;
}

static long run(struct rig *r, struct call *c)
{
    nreg = 0;
    declare(&c->p, sizeof c->p); declare(&c->q, sizeof c->q);
    if (c->has_s) declare(&c->s, sizeof c->s);
    declare(c->verbs, c->vdecl); declare(c->xy, c->xdecl * sizeof(int));
    declare(c->sp, sizeof c->sp); declare(c->sa, sizeof c->sa);
    return wm2_path(&r->e, (long)&c->p, (long)&c->q, c->has_s ? (long)&c->s : 0);
}

/* ---- the oracle: an independent interpreter of the verbs ----------------- */
static int ptb[8192], sb[256], ob[32768], osb[512];
static void ref_build(struct gfx_path *gp, const struct call *c, int num, int den)
{
    gfx_path_init(gp, ptb, 4096, sb, 256);
    unsigned xi = 0;
    for (unsigned i = 0; i < c->p.nverb; i++) {
        int v[6], n = c->verbs[i] == LOGIT_VERB_C ? 3 : c->verbs[i] == LOGIT_VERB_Q ? 2 : c->verbs[i] == LOGIT_VERB_Z ? 0 : 1;
        for (int k = 0; k < n * 2; k++) v[k] = (int)((long long)c->xy[xi++] * num / den);
        switch (c->verbs[i]) {
        case LOGIT_VERB_M: gfx_move_to(gp, v[0], v[1]); break;
        case LOGIT_VERB_L: gfx_line_to(gp, v[0], v[1]); break;
        case LOGIT_VERB_C: gfx_cubic_to(gp, v[0], v[1], v[2], v[3], v[4], v[5]); break;
        case LOGIT_VERB_Z: gfx_close(gp); break;
        }
    }
}
static void ref_paint(struct gfx_paint *pa, const struct call *c, int num, int den)
{
    const struct logit_paint2 *q = &c->q;
    if (q->kind == LOGIT_PAINT2_SOLID) { gfx_paint_solid(pa, q->argb & 0xFFFFFF, q->argb >> 24); return; }
    if (q->kind == LOGIT_PAINT2_LINEAR)
        gfx_paint_linear(pa, q->x0 * num / den, q->y0 * num / den, q->x1 * num / den, q->y1 * num / den);
    else gfx_paint_radial(pa, q->x0 * num / den, q->y0 * num / den, q->r1 * num / den);
    for (unsigned i = 0; i < q->nstop; i++) gfx_paint_stop(pa, (int)c->sp[i], c->sa[i] & 0xFFFFFF, c->sa[i] >> 24);
}
/* Direct call, at the device scale num/den (coordinates are multiples of 4). */
static int direct(struct rig *r, const struct call *c, int num, int den)
{
    struct gfx_path gp, out; struct gfx_paint pa;
    ref_build(&gp, c, num, den);
    ref_paint(&pa, c, num, den);
    if (!c->has_s) return ol_display_fill_path(&r->d, &gp, c->p.rule ? GFX_EVENODD : GFX_NONZERO, &pa, 255, 0);
    struct gfx_stroke gs = { 0 };
    gs.width = (int)((long long)c->s.width * num / den); gs.cap = c->s.cap; gs.join = c->s.join; gs.miter_limit = c->s.miter;
    gfx_path_init(&out, ob, 16384, osb, 512);
    return ol_display_stroke_path(&r->d, &gp, &gs, &out, &pa, 255);
}

static void check_pair(const char *name, struct call *c, int pct, int clip)
{
    struct rig a, b;
    rig_init(&a, pct); rig_init(&b, pct);
    if (clip) { rig_clip(&a, 20, 12, 77, 51); rig_clip(&b, 20, 12, 77, 51); }
    long rc = run(&a, c);
    int rd = direct(&b, c, pct, 100);
    if (rc != OL_OK || rd != OL_OK) FAIL("%s status %ld direct %d", name, rc, rd);
    if (!same(&a, &b)) FAIL("%s differs from the direct display call", name);
    struct rig z; rig_init(&z, pct);
    if (same(&a, &z)) FAIL("%s drew nothing", name);
    if (clip) {   /* nothing outside the clip changed */
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
            if ((x < 20 || x >= 77 || y < 12 || y >= 51) && a.px[y * W + x] != z.px[y * W + x]) { FAIL("%s wrote outside the clip", name); y = H; break; }
    }
    rig_free(&a); rig_free(&b); rig_free(&z);
}

static void test_oracle(void)
{
    struct call c;
    call_init(&c); circle(&c, FX(40), FX(30), FX(22)); check_pair("oracle circle solid", &c, 100, 0);
    call_init(&c); circle(&c, FX(40), FX(30) + 64, FX(24)); radial(&c); check_pair("oracle circle radial", &c, 100, 0);
    call_init(&c); circle(&c, FX(40), FX(30), FX(26)); c.p.rule = LOGIT_PATH2_EVENODD;
    M(&c, FX(40), FX(18)); L(&c, FX(52), FX(42)); L(&c, FX(28), FX(42)); Z(&c);
    check_pair("oracle evenodd", &c, 100, 0);
    call_init(&c); rect(&c, FX(12), FX(10), FX(60), FX(36));
    c.has_s = 1; c.s.width = FX(5); c.s.cap = LOGIT_STROKE2_CAP_ROUND; c.s.join = LOGIT_STROKE2_JOIN_ROUND; c.s.miter = 4 << 16;
    check_pair("oracle stroked rect", &c, 100, 0);
    linear(&c); check_pair("oracle stroked rect linear", &c, 100, 0);
    call_init(&c); circle(&c, FX(48), FX(30), FX(30)); linear(&c); check_pair("oracle clipped", &c, 100, 1);

    /* an app matrix: scale 2 and a translation, against gfx_path_matrix directly */
    {
        struct call m; struct rig a, b;
        call_init(&m); circle(&m, FX(10), FX(8), FX(8));
        m.p.flags = LOGIT_PATH_MATRIX; int mm[6] = { 2 * 65536, 0, 0, 2 * 65536, FX(30), FX(20) };
        memcpy(m.p.m, mm, sizeof mm);
        rig_init(&a, 100); rig_init(&b, 100);
        long rc = run(&a, &m);
        struct gfx_path gp; struct gfx_paint pa; struct gfx_matrix gm = { mm[0], mm[1], mm[2], mm[3], mm[4], mm[5] };
        ref_build(&gp, &m, 1, 1);
        /* rebuild with the matrix set before the first point */
        gfx_path_init(&gp, ptb, 4096, sb, 256); gfx_path_matrix(&gp, &gm);
        gfx_move_to(&gp, m.xy[0], m.xy[1]);
        unsigned xi = 2;
        for (unsigned i = 1; i < m.p.nverb; i++) {
            if (m.verbs[i] == LOGIT_VERB_C) { gfx_cubic_to(&gp, m.xy[xi], m.xy[xi+1], m.xy[xi+2], m.xy[xi+3], m.xy[xi+4], m.xy[xi+5]); xi += 6; }
            else gfx_close(&gp);
        }
        gfx_paint_solid(&pa, 0x2060C0, 255);
        ol_display_fill_path(&b.d, &gp, GFX_NONZERO, &pa, 255, 0);
        if (rc != OL_OK || !same(&a, &b)) FAIL("oracle matrix differs (rc %ld)", rc);
        rig_free(&a); rig_free(&b);
    }
}

/* ---- malformed input ------------------------------------------------------ */
static void malformed_case(const char *name, struct call *c, long want, int fault_xy, int fault_verbs)
{
    struct rig r; rig_init(&r, 100);
    uint32_t before = crc(&r);
    if (fault_xy) { fault_lo = (const char *)c->xy; fault_hi = fault_lo + 4; }
    if (fault_verbs) { fault_lo = (const char *)c->verbs; fault_hi = fault_lo + 1; }
    long rc = run(&r, c);
    fault_lo = fault_hi = 0;
    if (crc(&r) != before) FAIL("malformed %s touched pixels (status %ld)", name, rc);
    else if (rc != want) FAIL("malformed %s status %ld want %ld", name, rc, want);
    rig_free(&r);
}

static void tri(struct call *c) { call_init(c); triangle(c); }

static void test_malformed(void)
{
    struct call c;
    /* counts */
    tri(&c); c.p.nverb = 0; c.p.nxy = 0; M(&c, FX(10), FX(10)); L(&c, FX(60), FX(14)); L(&c, FX(30), FX(50));
    for (int i = 0; i < 4094; i++) c.verbs[c.p.nverb++] = LOGIT_VERB_Z;
    c.vdecl = c.p.nverb;
    malformed_case("verb overflow", &c, OL_LIMIT, 0, 0);
    tri(&c); c.p.nxy = 8194; c.xdecl = 8194;
    malformed_case("xy overflow", &c, OL_LIMIT, 0, 0);
    /* sequence */
    tri(&c); c.p.nverb = 3; c.p.nxy = 6; c.verbs[c.p.nverb++] = LOGIT_VERB_Q; c.xy[c.p.nxy++] = FX(20); c.xy[c.p.nxy++] = FX(20);
    c.vdecl = c.p.nverb; c.xdecl = c.p.nxy;
    malformed_case("Q missing points", &c, OL_ARGUMENT, 0, 0);
    call_init(&c); L(&c, FX(10), FX(10)); L(&c, FX(60), FX(14)); L(&c, FX(30), FX(50)); Z(&c);
    malformed_case("no leading M", &c, OL_ARGUMENT, 0, 0);
    tri(&c); c.verbs[c.p.nverb - 1] = 9;
    malformed_case("unknown verb", &c, OL_ARGUMENT, 0, 0);
    tri(&c); L(&c, FX(20), FX(20)); c.p.nverb--; c.p.nxy -= 2; c.p.nxy += 4; c.xdecl = c.p.nxy;   /* two spare points */
    malformed_case("nxy lies (too many)", &c, OL_ARGUMENT, 0, 0);
    tri(&c); c.p.nxy -= 2;
    malformed_case("nxy lies (too few)", &c, OL_ARGUMENT, 0, 0);
    tri(&c); c.p.nxy += 100;
    malformed_case("nxy past the buffer", &c, OL_ARGUMENT, 0, 0);
    tri(&c); c.p.nxy = 7; c.xdecl = 7;
    malformed_case("odd nxy", &c, OL_ARGUMENT, 0, 0);
    tri(&c); malformed_case("copyin fault xy", &c, OL_ARGUMENT, 1, 0);
    tri(&c); malformed_case("copyin fault verbs", &c, OL_ARGUMENT, 0, 1);
    tri(&c); c.p.verbs = 0;
    malformed_case("null verbs", &c, OL_ARGUMENT, 0, 0);
    /* sizes */
    tri(&c); c.p.size = 60; malformed_case("wrong path size", &c, OL_ARGUMENT, 0, 0);
    tri(&c); c.q.size += 4; malformed_case("wrong paint size", &c, OL_ARGUMENT, 0, 0);
    tri(&c); c.has_s = 1; c.s.width = FX(2); c.s.size = 0; malformed_case("wrong stroke size", &c, OL_ARGUMENT, 0, 0);
    /* matrix and coordinates */
    tri(&c); c.p.flags = LOGIT_PATH_MATRIX; c.p.m[0] = 65 * 65536; c.p.m[3] = 65536;
    malformed_case("huge matrix", &c, OL_LIMIT, 0, 0);
    tri(&c); c.p.flags = LOGIT_PATH_MATRIX; c.p.m[0] = c.p.m[3] = 65536; c.p.m[4] = FX(40000);
    malformed_case("huge translation", &c, OL_LIMIT, 0, 0);
    tri(&c); c.xy[0] = FX(40000); malformed_case("coordinate range", &c, OL_ARGUMENT, 0, 0);
    /* flags and enums */
    tri(&c); c.p.rule = 2; malformed_case("bad rule", &c, OL_ARGUMENT, 0, 0);
    tri(&c); c.p.flags = 2; malformed_case("bad path flag", &c, OL_ARGUMENT, 0, 0);
    tri(&c); c.q.flags = 2; malformed_case("bad paint flag", &c, OL_ARGUMENT, 0, 0);
    tri(&c); c.q.kind = 9; malformed_case("bad paint kind", &c, OL_ARGUMENT, 0, 0);
    /* stops */
    tri(&c); linear(&c); c.q.nstop = 17; malformed_case("nstop 17", &c, OL_ARGUMENT, 0, 0);
    tri(&c); linear(&c); c.q.nstop = 0; malformed_case("gradient without stops", &c, OL_ARGUMENT, 0, 0);
    tri(&c); linear(&c); c.sp[0] = 40000; c.sp[1] = 100; malformed_case("stops not monotonic", &c, OL_ARGUMENT, 0, 0);
    tri(&c); linear(&c); c.sp[1] = 65537; malformed_case("stop beyond 1.0", &c, OL_ARGUMENT, 0, 0);
    tri(&c); radial(&c); c.q.r1 = 0; malformed_case("radial r1 0", &c, OL_ARGUMENT, 0, 0);
    tri(&c); linear(&c); c.q.stop_pos = 0; malformed_case("null stops", &c, OL_ARGUMENT, 0, 0);
    /* stroke */
    tri(&c); c.has_s = 1; c.s.width = 0; malformed_case("stroke width 0", &c, OL_ARGUMENT, 0, 0);
    tri(&c); c.has_s = 1; c.s.width = FX(2); c.s.ndash = 3; c.s.dash[0] = c.s.dash[1] = c.s.dash[2] = FX(2);
    malformed_case("stroke odd dash count", &c, OL_ARGUMENT, 0, 0);
    tri(&c); c.has_s = 1; c.s.width = FX(2); c.s.cap = 3; malformed_case("stroke bad cap", &c, OL_ARGUMENT, 0, 0);
    tri(&c); c.has_s = 1; c.s.width = FX(2); c.s.join = 3; malformed_case("stroke bad join", &c, OL_ARGUMENT, 0, 0);
    tri(&c); c.has_s = 1; c.s.width = FX(2); c.s.ndash = 2; malformed_case("stroke zero dashes", &c, OL_ARGUMENT, 0, 0);
    /* not yet supported: a specific status, still no pixel */
    tri(&c); linear(&c); c.q.kind = LOGIT_PAINT2_CONIC; malformed_case("conic", &c, OL_UNSUPPORTED, 0, 0);
    tri(&c); c.q.op = 1; malformed_case("op != 0", &c, OL_UNSUPPORTED, 0, 0);
    tri(&c); linear(&c); c.q.spread = 1; malformed_case("spread != 0", &c, OL_UNSUPPORTED, 0, 0);
    tri(&c); radial(&c); c.q.r0 = FX(4); malformed_case("radial inner radius", &c, OL_UNSUPPORTED, 0, 0);
    tri(&c); linear(&c); stops2(&c, 9); malformed_case("nstop 9", &c, OL_UNSUPPORTED, 0, 0);
    tri(&c); linear(&c); stops2(&c, 16); c.q.op = 2; malformed_case("16 stops with op", &c, OL_UNSUPPORTED, 0, 0);
    /* a null paint or path pointer */
    {
        struct rig r; rig_init(&r, 100); uint32_t before = crc(&r);
        tri(&c); nreg = 0; declare(&c.p, sizeof c.p); declare(&c.q, sizeof c.q);
        if (wm2_path(&r.e, (long)&c.p, 0, 0) != OL_ARGUMENT || wm2_path(&r.e, 0, (long)&c.q, 0) != OL_ARGUMENT ||
            crc(&r) != before) FAIL("malformed null struct pointers");
        rig_free(&r);
    }
    /* sanity: the harness can tell a valid call from a refusal */
    {
        struct rig r; rig_init(&r, 100); uint32_t before = crc(&r);
        tri(&c); long rc = run(&r, &c);
        if (rc != OL_OK || crc(&r) == before) FAIL("harness: a valid triangle must draw (rc %ld)", rc);
        rig_free(&r);
    }
}

/* ---- scale ----------------------------------------------------------------- */
static void test_scale(void)
{
    static const int pcts[] = { 125, 150, 200 };
    for (unsigned i = 0; i < 3; i++) {
        char nm[64];
        struct call c;
        snprintf(nm, sizeof nm, "scale %d%% circle linear", pcts[i]);
        call_init(&c); circle(&c, FX(30), FX(24), FX(16)); linear(&c); check_pair(nm, &c, pcts[i], 0);
        snprintf(nm, sizeof nm, "scale %d%% stroked rect", pcts[i]);
        call_init(&c); rect(&c, FX(8), FX(6), FX(24), FX(16)); c.has_s = 1; c.s.width = FX(3); c.s.join = LOGIT_STROKE2_JOIN_ROUND; c.s.cap = 1;
        check_pair(nm, &c, pcts[i], 0);

        /* SNAP: an integer-point rect covers exactly the wm2_rect24 edges */
        static const int pts[][4] = { { 5, 3, 7, 5 }, { 0, 0, 5, 5 }, { 11, 9, 13, 3 } };
        for (unsigned k = 0; k < 3; k++) {
            struct rig a, b;
            rig_init(&a, pcts[i]); rig_init(&b, pcts[i]);
            call_init(&c); c.q.flags = LOGIT_RECT_SNAP;
            rect(&c, FX(pts[k][0]), FX(pts[k][1]), FX(pts[k][2]), FX(pts[k][3]));
            long rc = run(&a, &c);
            struct wm2_rect24 r = wm2_rect24(FX(pts[k][0]), FX(pts[k][1]), FX(pts[k][2]), FX(pts[k][3]), pcts[i], 1);
            struct gfx_path gp; struct gfx_paint pa;
            gfx_path_init(&gp, ptb, 64, sb, 8); gfx_path_rect(&gp, r.x, r.y, r.w, r.h);
            gfx_paint_solid(&pa, 0x2060C0, 255);
            ol_display_fill_path(&b.d, &gp, GFX_NONZERO, &pa, 255, 0);
            snprintf(nm, sizeof nm, "snap %d%% rect %u", pcts[i], k);
            if (rc != OL_OK || !same(&a, &b)) FAIL("%s differs from wm2_rect24 edges (rc %ld)", nm, rc);
            rig_free(&a); rig_free(&b);
        }
        /* two abutting snapped rects leave no seam */
        {
            struct rig a, b; rig_init(&a, pcts[i]); rig_init(&b, pcts[i]);
            call_init(&c); c.q.flags = LOGIT_RECT_SNAP; rect(&c, 0, FX(2), FX(5), FX(4));
            long rc = run(&a, &c);
            call_init(&c); c.q.flags = LOGIT_RECT_SNAP; rect(&c, FX(5), FX(2), FX(5), FX(4));
            rc |= run(&a, &c);
            struct wm2_rect24 r = wm2_rect24(0, FX(2), FX(10), FX(4), pcts[i], 1);
            struct gfx_path gp; struct gfx_paint pa;
            gfx_path_init(&gp, ptb, 64, sb, 8); gfx_path_rect(&gp, r.x, r.y, r.w, r.h);
            gfx_paint_solid(&pa, 0x2060C0, 255);
            ol_display_fill_path(&b.d, &gp, GFX_NONZERO, &pa, 255, 0);
            if (rc != OL_OK || !same(&a, &b)) FAIL("snap %d%% abutting rects leave a seam", pcts[i]);
            rig_free(&a); rig_free(&b);
        }
    }
}

static void test_scratch(void)
{
    struct rig a, b; struct call c;
    rig_init(&a, 100); rig_init(&b, 100);
    tri(&c); run(&a, &c); run(&b, &c);
    if (!a.d.scratch[OL_DS_USER] || !b.d.scratch[OL_DS_USER] || a.d.scratch[OL_DS_USER] == b.d.scratch[OL_DS_USER])
        FAIL("scratch: copy-in must live in each display's own OL_DS_USER slot");
    if (a.d.scratch[OL_DS_GROUP] || a.d.scratch[OL_DS_BLIT]) FAIL("scratch: foreign slots touched");
    rig_free(&a); rig_free(&b);
}

int main(void)
{
    test_oracle();
    test_malformed();
    test_scale();
    test_scratch();
    if (oob_reads > 0) { /* expected only for the cases that over-declare nxy */ }
    if (!bad) puts("wm_draw2_path_test: ok");
    return bad;
}
