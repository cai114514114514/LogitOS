/* ol_display_fill_mask / fill_path / stroke_path against gfx_fill on a straight
 * RGBA surface with identical initial pixels. Built with -DGFX_NCTX=2; does not
 * link fb.c, so it defines gfx_ctx_hook itself. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openlogit_display_path.h"
#include "gfx_paint_internal.h"

int (*gfx_ctx_hook)(void);
static int g_ctx, g_toggle;
static int hook(void) { if (g_toggle) g_ctx ^= 1; return g_ctx; }

static int bad;
#define FAIL(name) do { printf("FAIL %s\n", name); bad = 1; } while (0)
#define CHECK(c, name) do { if (!(c)) FAIL(name); } while (0)

static void *t_alloc(unsigned long n) { return malloc(n); }
static void t_free(void *p) { free(p); }
#define FX(v) ((v) * 256)

struct rig {
    struct ol_display d; uint32_t *px; int w, h;
    unsigned char *ref; struct gfx_surface rs;
};

static uint32_t init_px(int x, int y)
{
    return (uint32_t)(((x * 7 + y * 3) & 255) << 16 | ((x * 3 + y * 11 + 40) & 255) << 8 | ((x * 5 + y * 5 + 90) & 255));
}

static void rig_init(struct rig *r, int w, int h, int refw)
{
    memset(r, 0, sizeof *r);
    r->w = w; r->h = h;
    r->px = malloc((size_t)w * h * 4);
    struct ol_pixel_target t = { r->px, w, h, 0, 0, 0, 0, 0 };
    r->d.allocate = t_alloc; r->d.release = t_free;
    ol_display_bind(&r->d, &t, 16, 8, 0);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) r->px[y * w + x] = init_px(x, y);
    if (refw) {
        r->ref = malloc((size_t)refw * h * 4);
        gfx_surface_init(&r->rs, r->ref, refw, h, refw * 4);
        for (int y = 0; y < h; y++) for (int x = 0; x < refw; x++) {
            uint32_t p = init_px(x, y); unsigned char *q = r->ref + ((size_t)y * refw + x) * 4;
            q[0] = p >> 16; q[1] = p >> 8; q[2] = p; q[3] = 255;
        }
    }
}
static void rig_free(struct rig *r) { ol_display_destroy(&r->d); free(r->px); free(r->ref); }

/* max channel error between display and reference over [0,cw) */
static int maxerr(const struct rig *r, int cw, int xoff)
{
    int m = 0;
    for (int y = 0; y < r->h; y++) for (int x = 0; x < cw; x++) {
        uint32_t p = r->px[y * r->w + x + xoff]; const unsigned char *q = r->ref + ((size_t)y * r->rs.w + x) * 4;
        int e[3] = { (int)((p >> 16) & 255) - q[0], (int)((p >> 8) & 255) - q[1], (int)(p & 255) - q[2] };
        for (int i = 0; i < 3; i++) { if (e[i] < 0) e[i] = -e[i]; if (e[i] > m) m = e[i]; }
    }
    return m;
}

enum { P_SOLID, P_LINEAR, P_RADIAL };
static void mkpaint(struct gfx_paint *p, int kind, int ox)
{
    if (kind == P_SOLID) gfx_paint_solid(p, 0x2060c0, 255);
    else if (kind == P_LINEAR) {
        gfx_paint_linear(p, FX(ox + 10), FX(6), FX(ox + 70), FX(46));
        gfx_paint_stop(p, 0, 0xff3010, 255); gfx_paint_stop(p, 65536, 0x10e0a0, 120);
    } else {
        gfx_paint_radial(p, FX(ox + 40), FX(30), FX(30));
        gfx_paint_stop(p, 0, 0xffffff, 255); gfx_paint_stop(p, 32768, 0xe0a010, 220);
        gfx_paint_stop(p, 65536, 0x401080, 255);
    }
}

enum { S_CIRCLE, S_BLOB, S_STROKE };
static int ptb[4096], sb[64], ob[16384], osb[256];
static int shape(struct gfx_path *p, struct gfx_path *o, struct gfx_stroke *st, int s, int ox)
{
    gfx_path_init(p, ptb, 2048, sb, 64);
    gfx_path_init(o, ob, 8192, osb, 128);
    memset(st, 0, sizeof *st);
    if (s == S_CIRCLE) gfx_path_circle(p, FX(ox + 40) + 77, FX(30) + 31, FX(26));
    else if (s == S_BLOB) {
        gfx_move_to(p, FX(ox + 12) + 40, FX(30));
        gfx_cubic_to(p, FX(ox + 14), FX(2), FX(ox + 60), FX(4), FX(ox + 70) + 9, FX(28));
        gfx_cubic_to(p, FX(ox + 80), FX(60), FX(ox + 30), FX(58), FX(ox + 20), FX(48));
        gfx_cubic_to(p, FX(ox + 4), FX(40), FX(ox + 6), FX(34), FX(ox + 12) + 40, FX(30));
        gfx_close(p);
    } else {
        gfx_move_to(p, FX(ox + 8), FX(10)); gfx_line_to(p, FX(ox + 40) + 100, FX(50));
        gfx_line_to(p, FX(ox + 72), FX(14)); gfx_line_to(p, FX(ox + 74), FX(52));
        st->width = FX(5) + 64; st->cap = GFX_CAP_ROUND; st->join = GFX_JOIN_ROUND; st->miter_limit = 4 << 16;
    }
    return s == S_STROKE;
}

static const char *pname[] = { "solid", "linear", "radial" }, *sname[] = { "circle", "blob", "stroke" };

static void run_case(int s, int k, int opacity, int clip, const char *tag)
{
    struct rig r; rig_init(&r, 96, 64, 96);
    struct gfx_path p, o; struct gfx_stroke st; struct gfx_paint pa;
    int stroke = shape(&p, &o, &st, s, 0);
    mkpaint(&pa, k, 0);
    struct gfx_rect cr = { 0, 0, 96, 64 };
    if (clip) {
        r.d.target.clip_on = 1; r.d.target.clx0 = 20; r.d.target.cly0 = 12; r.d.target.clx1 = 77; r.d.target.cly1 = 51;
        cr.x = 20; cr.y = 12; cr.w = 57; cr.h = 39;
    }
    int rc;
    if (stroke) rc = ol_display_stroke_path(&r.d, &p, &st, &o, &pa, opacity);
    else rc = ol_display_fill_path(&r.d, &p, GFX_NONZERO, &pa, opacity, 0);
    CHECK(rc == OL_OK, "status");
    struct gfx_paint rp = pa; rp.global_alpha = opacity;
    if (stroke) { gfx_path_reset(&o); gfx_stroke_path(&o, &p, &st); gfx_fill_subs(&r.rs, &o, GFX_NONZERO, &rp, &cr, GFX_SUBS); }
    else gfx_fill_subs(&r.rs, &p, GFX_NONZERO, &rp, &cr, GFX_SUBS);
    int e = maxerr(&r, 96, 0);
    char nm[96];
    if (e > 1) { snprintf(nm, sizeof nm, "%s", clip ? "scissor" : tag ? tag : pname[k]); FAIL(nm); printf("  %s/%s op=%d err=%d\n", sname[s], pname[k], opacity, e); }
    rig_free(&r);
}

/* 5000 wide: gfx_fill alone refuses; ours tiles. Reference = two narrower fills. */
static void wide(void)
{
    enum { W = 5000, H = 64, SPLIT = 4096 };
    for (int k = P_SOLID; k <= P_RADIAL; k++) {
        struct rig r; rig_init(&r, W, H, SPLIT);
        struct gfx_path p; struct gfx_paint pa;
        /* a blob spanning the split: scale by drawing the circle wide */
        gfx_path_init(&p, ptb, 2048, sb, 64);
        gfx_path_ellipse(&p, FX(4090), FX(30), FX(900), FX(26));
        if (k == P_SOLID) gfx_paint_solid(&pa, 0x2060c0, 255);
        else if (k == P_LINEAR) { gfx_paint_linear(&pa, FX(4000), 0, FX(4000), FX(60)); /* vertical axis: the painter's incremental x step (dt) truncates, so a long horizontal ramp drifts by span start, which differs between tilings */ gfx_paint_stop(&pa, 0, 0xff3010, 255); gfx_paint_stop(&pa, 65536, 0x10e0a0, 120); }
        else { gfx_paint_radial(&pa, FX(4090), FX(30), FX(900)); gfx_paint_stop(&pa, 0, 0xffffff, 255); gfx_paint_stop(&pa, 65536, 0x401080, 255); }
        int rc = ol_display_fill_path(&r.d, &p, GFX_NONZERO, &pa, 255, 0);
        CHECK(rc == OL_OK, "wide status");
        /* left part: reference over x<4096 */
        gfx_fill_subs(&r.rs, &p, GFX_NONZERO, &pa, 0, GFX_SUBS);
        int e = maxerr(&r, SPLIT, 0);
        /* right part: shift path and paint by -4096 into a 904-wide surface */
        int ew = 0;
        {
            int pw = W - SPLIT; unsigned char *rr = malloc((size_t)pw * H * 4);
            struct gfx_surface rs2; gfx_surface_init(&rs2, rr, pw, H, pw * 4);
            for (int y = 0; y < H; y++) for (int x = 0; x < pw; x++) {
                uint32_t v = init_px(x + SPLIT, y); unsigned char *c = rr + ((size_t)y * pw + x) * 4;
                c[0] = v >> 16; c[1] = v >> 8; c[2] = v; c[3] = 255;
            }
            struct gfx_path p2; static int pt2[4096], s2[64];
            gfx_path_init(&p2, pt2, 2048, s2, 64);
            gfx_path_ellipse(&p2, FX(4090 - SPLIT), FX(30), FX(900), FX(26));
            struct gfx_paint pb = pa;
            pb.x0 -= FX(SPLIT); pb.x1 -= FX(SPLIT); pb.cx -= FX(SPLIT);
            gfx_fill_subs(&rs2, &p2, GFX_NONZERO, &pb, 0, GFX_SUBS);
            for (int y = 0; y < H; y++) for (int x = 0; x < pw; x++) {
                uint32_t v = r.px[y * W + SPLIT + x]; const unsigned char *c = rr + ((size_t)y * pw + x) * 4;
                int d0 = (int)((v >> 16) & 255) - c[0], d1 = (int)((v >> 8) & 255) - c[1], d2 = (int)(v & 255) - c[2];
                if (d0 < 0) d0 = -d0; if (d1 < 0) d1 = -d1; if (d2 < 0) d2 = -d2;
                if (d0 > ew) ew = d0; if (d1 > ew) ew = d1; if (d2 > ew) ew = d2;
            }
            free(rr);
        }
        if (e > 1 || ew > 1) { FAIL(k == P_SOLID ? "wide solid" : k == P_LINEAR ? "wide linear" : "wide radial"); printf("  err L=%d R=%d\n", e, ew); }
        rig_free(&r);
    }
    /* gfx_fill alone refuses a 5000-px row */
    unsigned char *big = malloc((size_t)W * 4 * 8); struct gfx_surface bs; gfx_surface_init(&bs, big, W, 8, W * 4);
    struct gfx_path p; struct gfx_paint pa; gfx_path_init(&p, ptb, 2048, sb, 64);
    gfx_path_rect(&p, 0, 0, FX(W), FX(8)); gfx_paint_solid(&pa, 0xffffff, 255);
    CHECK(gfx_fill_subs(&bs, &p, GFX_NONZERO, &pa, 0, GFX_SUBS) == 0, "gfx_fill refuses wide");
    free(big);
}

static void mask_case(void)
{
    struct rig r; rig_init(&r, 96, 64, 96);
    static unsigned char m[30 * 20]; /* stride 30, w 25 */
    for (int j = 0; j < 20; j++) for (int i = 0; i < 30; i++) m[j * 30 + i] = (unsigned char)((i * 10 + j * 7) & 255);
    struct gfx_paint pa; mkpaint(&pa, P_LINEAR, 0);
    CHECK(ol_display_fill_mask(&r.d, 10, 5, m, 25, 20, 30, &pa, 255) == OL_OK, "mask status");
    /* off-screen and negative origins clip, never crash */
    CHECK(ol_display_fill_mask(&r.d, -8, -6, m, 25, 20, 30, &pa, 128) == OL_OK, "mask neg origin");
    CHECK(ol_display_fill_mask(&r.d, 90, 60, m, 25, 20, 30, &pa, 255) == OL_OK, "mask edge");
    CHECK(ol_display_fill_mask(&r.d, 0, 0, m, 25, 20, 10, &pa, 255) == OL_ARGUMENT, "mask bad stride");
    CHECK(ol_display_fill_mask(0, 0, 0, m, 25, 20, 30, &pa, 255) == OL_ARGUMENT, "mask null");
    /* oracle: gfx_paint_row over a full-size straight-RGBA surface, one row at a time */
    for (int pass = 0; pass < 3; pass++) {
        int ox = pass == 0 ? 10 : pass == 1 ? -8 : 90, oy = pass == 0 ? 5 : pass == 1 ? -6 : 60, op = pass == 1 ? 128 : 255;
        for (int j = 0; j < 20; j++) {
            int y = oy + j; if (y < 0 || y >= 64) continue;
            unsigned char cov[96]; memset(cov, 0, sizeof cov);
            for (int i = 0; i < 25; i++) { int x = ox + i; if (x >= 0 && x < 96) cov[x] = (unsigned char)((m[j * 30 + i] * op + 127) / 255); }
            gfx_paint_row(&pa, &r.rs, y, 0, 96, cov, 0);
        }
    }
    int e = maxerr(&r, 96, 0);
    if (e > 1) { FAIL("mask"); printf("  err=%d\n", e); }
    rig_free(&r);
}

static void interleave(void)
{
    /* serial result with a fixed context, then two displays whose every
     * raster call lands on the other context (hook toggles per call). */
    struct rig a[2], b[2];
    for (int pass = 0; pass < 2; pass++) {
        g_toggle = pass; g_ctx = 0;
        for (int i = 0; i < 2; i++) {
            struct rig *r = pass ? &b[i] : &a[i]; rig_init(r, 96, 64, 0);
        }
        for (int step = 0; step < 6; step++) {      /* alternate A/B, shapes and paints */
            for (int i = 0; i < 2; i++) {
                struct rig *r = pass ? &b[i] : &a[i];
                struct gfx_path p, o; struct gfx_stroke st; struct gfx_paint pa;
                int stroke = shape(&p, &o, &st, (step + i) % 3, 0);
                mkpaint(&pa, (step + 2 * i) % 3, 0);
                if (stroke) ol_display_stroke_path(&r->d, &p, &st, &o, &pa, 255 - step * 20);
                else ol_display_fill_path(&r->d, &p, GFX_EVENODD, &pa, 255 - step * 20, 0);
            }
        }
    }
    for (int i = 0; i < 2; i++) {
        CHECK(memcmp(a[i].px, b[i].px, 96 * 64 * 4) == 0, "interleave");
        rig_free(&a[i]); rig_free(&b[i]);
    }
    g_toggle = 0;
}

int main(void)
{
    gfx_ctx_hook = hook;
    for (int s = 0; s < 3; s++) for (int k = 0; k < 3; k++) run_case(s, k, 255, 0, 0);
    run_case(S_BLOB, P_RADIAL, 255, 1, 0);
    run_case(S_STROKE, P_LINEAR, 255, 1, 0);
    run_case(S_CIRCLE, P_SOLID, 255, 1, 0);
    run_case(S_BLOB, P_SOLID, 140, 0, "opacity");
    run_case(S_STROKE, P_SOLID, 90, 0, "opacity");
    mask_case();
    wide();
    interleave();
    if (!bad) puts("display_path_test: ok");
    return bad;
}
