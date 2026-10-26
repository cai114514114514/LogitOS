/* ol_display_shadow_ex against a brute-force double reference: the shape is
 * supersampled, convolved with a separable gaussian and read at pixel centres.
 * Built with -DGFX_NCTX=2 and without fb.c, so it defines gfx_ctx_hook. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openlogit_display_shadow.h"

int (*gfx_ctx_hook)(void);
static int g_ctx;
static int hook(void) { return g_ctx; }

static int bad;
#define FAILF(...) do { printf("FAIL " __VA_ARGS__); printf("\n"); bad = 1; } while (0)

static int fail_alloc;
static void *t_alloc(unsigned long n) { return fail_alloc ? 0 : malloc(n); }
static void t_free(void *p) { free(p); }

struct img { struct ol_display d; uint32_t *px; int w, h; };
static void img_init(struct img *m, int w, int h)
{
    memset(m, 0, sizeof *m);
    m->w = w; m->h = h;
    m->px = calloc((size_t)w * h, 4);
    m->d.target.px = m->px; m->d.target.w = w; m->d.target.h = h;
    m->d.rpos = 16; m->d.gpos = 8; m->d.bpos = 0;
    m->d.allocate = t_alloc; m->d.release = t_free;
}
static void img_free(struct img *m) { ol_display_destroy(&m->d); free(m->px); }
static int chan(const struct img *m, int x, int y, int sh) { return (m->px[(size_t)y * m->w + x] >> sh) & 255; }

/* ---- reference ---- */
struct shape { double x0, y0, x1, y1, r[4]; };
static void mk_shape(struct shape *s, int x, int y, int w, int h, const int *rad,
                     double ox, double oy, double grow)
{
    s->x0 = x + ox - grow; s->y0 = y + oy - grow;
    s->x1 = x + w + ox + grow; s->y1 = y + h + oy + grow;
    for (int i = 0; i < 4; i++) {
        double r = rad[i] > 0 ? rad[i] + grow : 0;
        s->r[i] = r < 0 ? 0 : r;
    }
    double f = 1, W = s->x1 - s->x0, H = s->y1 - s->y0, sum;
    if ((sum = s->r[0] + s->r[1]) > W && W / sum < f) f = W / sum;
    if ((sum = s->r[3] + s->r[2]) > W && W / sum < f) f = W / sum;
    if ((sum = s->r[0] + s->r[3]) > H && H / sum < f) f = H / sum;
    if ((sum = s->r[1] + s->r[2]) > H && H / sum < f) f = H / sum;
    for (int i = 0; i < 4; i++) s->r[i] *= f;
}
static int in_shape(const struct shape *s, double px, double py)
{
    if (s->x1 <= s->x0 || s->y1 <= s->y0) return 0;
    if (px < s->x0 || px >= s->x1 || py < s->y0 || py >= s->y1) return 0;
    for (int i = 0; i < 4; i++) {
        double r = s->r[i];
        if (r <= 0) continue;
        double cx = (i == 0 || i == 3) ? s->x0 + r : s->x1 - r;
        double cy = i < 2 ? s->y0 + r : s->y1 - r;
        if (((i == 0 || i == 3) ? px < cx : px > cx) && (i < 2 ? py < cy : py > cy))
            return (px - cx) * (px - cx) + (py - cy) * (py - cy) <= r * r;
    }
    return 1;
}
/* Blurred coverage 0..1 of every pixel of a CWxCH canvas. */
static double *ref_cover(int CW, int CH, const struct shape *s, double sig, int ss)
{
    int R = (int)ceil(4.6 * sig * ss);
    double *k = malloc(sizeof(double) * 2 * R), ksum = 0;
    for (int j = -R; j < R; j++) {
        double d = (j + 0.5) / ss;
        k[j + R] = exp(-d * d / (2 * sig * sig));
        ksum += k[j + R];
    }
    for (int j = 0; j < 2 * R; j++) k[j] /= ksum;
    int SW = CW * ss, SH = CH * ss;
    unsigned char *m = malloc((size_t)SW * SH);
    for (int sy = 0; sy < SH; sy++)
        for (int sx = 0; sx < SW; sx++)
            m[(size_t)sy * SW + sx] = (unsigned char)in_shape(s, (sx + 0.5) / ss, (sy + 0.5) / ss);
    double *T = calloc((size_t)SH * CW, sizeof(double));
    for (int sy = 0; sy < SH; sy++)
        for (int px = 0; px < CW; px++) {
            int c = px * ss + ss / 2;
            int lo = c - R < 0 ? 0 : c - R, hi = c + R > SW ? SW : c + R;
            double a = 0;
            for (int sx = lo; sx < hi; sx++) if (m[(size_t)sy * SW + sx]) a += k[sx - c + R];
            T[(size_t)sy * CW + px] = a;
        }
    double *out = calloc((size_t)CW * CH, sizeof(double));
    for (int py = 0; py < CH; py++) {
        int c = py * ss + ss / 2;
        int lo = c - R < 0 ? 0 : c - R, hi = c + R > SH ? SH : c + R;
        for (int px = 0; px < CW; px++) {
            double a = 0;
            for (int sy = lo; sy < hi; sy++) a += T[(size_t)sy * CW + px] * k[sy - c + R];
            out[(size_t)py * CW + px] = a;
        }
    }
    free(k); free(m); free(T);
    return out;
}

/* One shadow case on a fresh black canvas, white shadow. */
struct cs {
    int w, h, radius[4], dx24, dy24, sig24, spread24, alpha, inset;
    unsigned flags;
    uint32_t rgb;
};
struct run { struct img im; int mx, my; double *ref; struct shape sh, box; };

static void run_case(struct run *r, const struct cs *c)
{
    double sig = c->sig24 / 256.0;
    int margin = (int)(5 * sig) + 8;
    int CW = c->w + 2 * margin, CH = c->h + 2 * margin;
    r->mx = r->my = margin;
    img_init(&r->im, CW, CH);
    int rc = ol_display_shadow_ex_opt(&r->im.d, margin, margin, c->w, c->h, c->radius,
                                      c->dx24, c->dy24, c->sig24, c->spread24,
                                      c->rgb, c->alpha, c->inset, c->flags);
    if (rc) FAILF("call rc=%d", rc);
    double grow = c->spread24 / 256.0;
    mk_shape(&r->sh, margin, margin, c->w, c->h, c->radius, c->dx24 / 256.0, c->dy24 / 256.0,
             c->inset ? -grow : grow);
    mk_shape(&r->box, margin, margin, c->w, c->h, c->radius, 0, 0, 0);
    r->ref = ref_cover(CW, CH, &r->sh, sig, sig <= 3 ? 16 : sig <= 12 ? 8 : 2);
}
static void run_free(struct run *r) { img_free(&r->im); free(r->ref); }

/* Max |pixel - alpha*coverage*255| over pixels where use(x,y) != 0. */
static int max_err(const struct run *r, const struct cs *c, int sh, int chv)
{
    int worst = 0;
    for (int y = 0; y < r->im.h; y++)
        for (int x = 0; x < r->im.w; x++) {
            double cov = r->ref[(size_t)y * r->im.w + x];
            if (c->inset) {
                /* box AA is approximated: compare away from box corners only */
                int near = 0;
                for (int i = 0; i < 4; i++) {
                    double rr = r->box.r[i];
                    double cx = (i == 0 || i == 3) ? r->box.x0 : r->box.x1 - rr;
                    double cy = i < 2 ? r->box.y0 : r->box.y1 - rr;
                    if (rr > 0 && x + 1 >= cx && x <= cx + rr && y + 1 >= cy && y <= cy + rr) near = 1;
                }
                if (near) continue;
                cov = in_shape(&r->box, x + 0.5, y + 0.5) ? 1 - cov : 0;
            }
            double want = cov * c->alpha * chv / 255.0;
            int e = (int)fabs(chan(&r->im, x, y, sh) - want + (want < 0 ? -0.5 : 0.0));
            double d = fabs(chan(&r->im, x, y, sh) - want);
            e = (int)ceil(d - 0.5);
            if (e > worst) worst = e;
        }
    return worst;
}

static struct cs base_case(int w, int h, int r, int sigpx)
{
    struct cs c;
    memset(&c, 0, sizeof c);
    c.w = w; c.h = h;
    for (int i = 0; i < 4; i++) c.radius[i] = r;
    c.dx24 = 5 * 256 + 128; c.dy24 = 9 * 256 + 64;
    c.sig24 = sigpx * 256;
    c.alpha = 255; c.rgb = 0xFFFFFF; c.flags = OL_SHADOW_EX_BODY;
    return c;
}

static void accuracy(void)
{
    static const int radii[] = { 0, 8, 24, 120 }, sigmas[] = { 2, 8, 40 };
    for (int si = 0; si < 3; si++)
        for (int ri = 0; ri < 4; ri++) {
            struct cs c = base_case(320, 260, radii[ri], sigmas[si]);
            struct run r;
            run_case(&r, &c);
            int e = max_err(&r, &c, 8, 255);
            printf("acc r=%d sigma=%d maxerr=%d\n", radii[ri], sigmas[si], e);
            if (e > 3) FAILF("sigma%d r%d maxerr=%d", sigmas[si], radii[ri], e);
            if (sigmas[si] == 40 && radii[ri] == 120) {
                /* Unclamped: 60px outside the left edge at mid height the
                 * gaussian tail is ~6.7%; a 72px mask budget renders ~0. */
                int x = (int)(r.sh.x0) - 60, y = r.my + c.h / 2;
                int v = chan(&r.im, x, y, 8);
                printf("sigma40 tail pixel=%d\n", v);
                if (v < 12) FAILF("sigma40 tail pixel=%d want >=12 (clamped?)", v);
            }
            run_free(&r);
        }
}

static void mixed(void)
{
    static const double sp[] = { 3.5, -4.0 };
    for (int i = 0; i < 2; i++) {
        struct cs c = base_case(220, 180, 0, 6);
        c.radius[0] = 4; c.radius[1] = 24; c.radius[2] = 60; c.radius[3] = 0;
        c.dx24 = -7 * 256; c.dy24 = 4 * 256 + 100; c.spread24 = (int)(sp[i] * 256);
        struct run r;
        run_case(&r, &c);
        int e = max_err(&r, &c, 8, 255);
        printf("mixed spread=%.1f maxerr=%d\n", sp[i], e);
        if (e > 3) FAILF("mixed spread=%.1f maxerr=%d", sp[i], e);
        run_free(&r);
    }
}

static void frac_scale(void)
{
    static const double sc[] = { 1.25, 1.5 };
    static const char *nm[] = { "frac125", "frac150" };
    for (int i = 0; i < 2; i++) {
        double s = sc[i];
        struct cs c = base_case((int)(100 * s), (int)(80 * s), (int)(10 * s), 1);
        c.sig24 = (int)(6 * s * 256 + 0.5); c.dx24 = (int)(3 * s * 256 + 0.5);
        c.dy24 = (int)(5 * s * 256 + 0.5); c.spread24 = (int)(2 * s * 256 + 0.5);
        struct run r;
        run_case(&r, &c);
        int e = max_err(&r, &c, 8, 255);
        printf("%s sigma24=%d maxerr=%d\n", nm[i], c.sig24, e);
        if (e > 3) FAILF("%s maxerr=%d", nm[i], e);
        run_free(&r);
    }
}

static void mirror(void)
{
    struct cs c = base_case(200, 150, 0, 7);
    c.radius[0] = c.radius[1] = 20; c.radius[2] = c.radius[3] = 6;
    c.flags = OL_SHADOW_EX_BODY;
    struct run a, b;
    c.dx24 = 9 * 256 + 128; run_case(&a, &c);
    c.dx24 = -(9 * 256 + 128); run_case(&b, &c);
    int worst = 0, W = a.im.w;
    for (int y = 0; y < a.im.h; y++)
        for (int x = 0; x < W; x++) {
            int d = abs(chan(&a.im, x, y, 8) - chan(&b.im, W - 1 - x, y, 8));
            if (d > worst) worst = d;
        }
    printf("mirror maxdiff=%d\n", worst);
    if (worst > 1) FAILF("mirror maxdiff=%d", worst);
    run_free(&a); run_free(&b);
}

static void inset_cases(void)
{
    static const int rr[] = { 0, 10 };
    for (int k = 0; k < 2; k++) {
        struct cs c = base_case(160, 120, rr[k], 5);
        c.inset = 1; c.flags = 0; c.spread24 = 3 * 256; c.dx24 = 2 * 256; c.dy24 = 3 * 256;
        struct run in, out;
        run_case(&in, &c);
        int e = max_err(&in, &c, 8, 255);
        /* inset(s) + outer(-s) == 1 inside the box */
        struct cs o = c;
        o.inset = 0; o.spread24 = -c.spread24; o.flags = OL_SHADOW_EX_BODY;
        run_case(&out, &o);
        int worst = 0;
        for (int y = in.my; y < in.my + c.h; y++)
            for (int x = in.mx; x < in.mx + c.w; x++) {
                int cx = x - in.mx, cy = y - in.my, corner = 0;
                if (rr[k] && (cx < rr[k] || cx >= c.w - rr[k]) && (cy < rr[k] || cy >= c.h - rr[k])) corner = 1;
                if (corner) continue;
                int d = abs(chan(&in.im, x, y, 8) + chan(&out.im, x, y, 8) - 255);
                if (d > worst) worst = d;
            }
        printf("inset r=%d vs ref maxerr=%d, inset+outer(-s) maxdev=%d\n", rr[k], e, worst);
        if (e > 3) FAILF("inset r%d vs reference maxerr=%d", rr[k], e);
        if (worst > 3) FAILF("inset r%d complement maxdev=%d", rr[k], worst);
        run_free(&in); run_free(&out);
    }
}

static void linear(void)
{
    struct cs c = base_case(150, 110, 12, 6);
    struct run a, h, q, col;
    run_case(&a, &c);
    struct cs c2 = c; c2.alpha = 128; run_case(&h, &c2);
    struct cs c3 = c; c3.alpha = 64; run_case(&q, &c3);
    struct cs c4 = c; c4.rgb = 0xC86432; run_case(&col, &c4);
    int w2 = 0, w3 = 0, wr = 0, wg = 0, wb = 0;
    for (int y = 0; y < a.im.h; y++)
        for (int x = 0; x < a.im.w; x++) {
            int v = chan(&a.im, x, y, 8);
            int d2 = abs(chan(&h.im, x, y, 8) - (int)(v * 128 / 255.0 + 0.5));
            int d3 = abs(chan(&q.im, x, y, 8) - (int)(v * 64 / 255.0 + 0.5));
            if (d2 > w2) w2 = d2;
            if (d3 > w3) w3 = d3;
            int dr = abs(chan(&col.im, x, y, 16) - (int)(v * 200 / 255.0 + 0.5));
            int dg = abs(chan(&col.im, x, y, 8) - (int)(v * 100 / 255.0 + 0.5));
            int db = abs(chan(&col.im, x, y, 0) - (int)(v * 50 / 255.0 + 0.5));
            if (dr > wr) wr = dr;
            if (dg > wg) wg = dg;
            if (db > wb) wb = db;
        }
    printf("linear alpha128 dev=%d alpha64 dev=%d colour dev=%d/%d/%d\n", w2, w3, wr, wg, wb);
    if (w2 > 2 || w3 > 2) FAILF("linear alpha dev=%d/%d", w2, w3);
    if (wr > 2 || wg > 2 || wb > 2) FAILF("linear colour dev=%d/%d/%d", wr, wg, wb);
    run_free(&a); run_free(&h); run_free(&q); run_free(&col);
}

static void skip_and_clip(void)
{
    struct cs c = base_case(200, 160, 16, 8);
    c.flags = 0;
    struct run sk, full;
    run_case(&sk, &c);
    struct cs f = c; f.flags = OL_SHADOW_EX_BODY;
    run_case(&full, &f);
    int mid = chan(&sk.im, sk.mx + 100, sk.my + 80, 8);
    int midfull = chan(&full.im, sk.mx + 100, sk.my + 80, 8);
    if (mid != 0 || midfull == 0) FAILF("skip body mid=%d full=%d", mid, midfull);
    int outside_diff = 0, inside_changed = 0;
    for (int y = 0; y < sk.im.h; y++)
        for (int x = 0; x < sk.im.w; x++) {
            int in_box = x >= sk.mx && x < sk.mx + c.w && y >= sk.my && y < sk.my + c.h;
            int d = chan(&sk.im, x, y, 8) != chan(&full.im, x, y, 8);
            if (!in_box && d) outside_diff++;
            if (in_box && d) inside_changed++;
        }
    printf("skip outside_diff=%d skipped_pixels=%d\n", outside_diff, inside_changed);
    if (outside_diff) FAILF("skip changed %d pixels outside the caster box", outside_diff);
    if (!inside_changed) FAILF("skip skipped nothing");

    /* clip */
    struct img cl;
    img_init(&cl, full.im.w, full.im.h);
    cl.d.target.clip_on = 1; cl.d.target.clx0 = 40; cl.d.target.cly0 = 50;
    cl.d.target.clx1 = 150; cl.d.target.cly1 = 130;
    ol_display_shadow_ex_opt(&cl.d, full.mx, full.my, c.w, c.h, c.radius, c.dx24, c.dy24,
                             c.sig24, 0, c.rgb, 255, 0, OL_SHADOW_EX_BODY);
    int cbad = 0;
    for (int y = 0; y < cl.h; y++)
        for (int x = 0; x < cl.w; x++) {
            int inside = x >= 40 && x < 150 && y >= 50 && y < 130;
            int want = inside ? chan(&full.im, x, y, 8) : 0;
            if (chan(&cl, x, y, 8) != want) cbad++;
        }
    printf("clip mismatches=%d\n", cbad);
    if (cbad) FAILF("clip mismatches=%d", cbad);
    img_free(&cl);

    /* scratch growth failure draws nothing and reports */
    struct img ff;
    img_init(&ff, 100, 100);
    fail_alloc = 1;
    int rc = ol_display_shadow_ex(&ff.d, 30, 30, 40, 40, c.radius, 0, 0, 6 * 256, 0, 0xFFFFFF, 255, 0);
    fail_alloc = 0;
    int any = 0;
    for (int i = 0; i < 100 * 100; i++) any |= ff.px[i] != 0;
    if (rc != OL_LIMIT || any) FAILF("alloc failure rc=%d drew=%d", rc, any);
    img_free(&ff);
    run_free(&sk); run_free(&full);
}

static void interleave(void)
{
    struct img a, b, a0, b0;
    int ra[4] = { 12, 12, 12, 12 }, rb[4] = { 30, 0, 30, 0 };
    img_init(&a, 260, 220); img_init(&b, 300, 240); img_init(&a0, 260, 220); img_init(&b0, 300, 240);
    g_ctx = 0;
    ol_display_shadow_ex(&a0.d, 50, 50, 150, 100, ra, 0, 512, 8 * 256, 0, 0x102030, 200, 0);
    g_ctx = 1;
    ol_display_shadow_ex(&b0.d, 60, 40, 160, 120, rb, 300, 0, 21 * 256, 256, 0xFFFFFF, 255, 0);
    for (int round = 0; round < 2; round++) {
        g_ctx = 0;
        if (round == 0) ol_display_shadow_ex(&a.d, 50, 50, 150, 100, ra, 0, 512, 8 * 256, 0, 0x102030, 200, 0);
        g_ctx = 1;
        if (round == 0) ol_display_shadow_ex(&b.d, 60, 40, 160, 120, rb, 300, 0, 21 * 256, 256, 0xFFFFFF, 255, 0);
    }
    int da = memcmp(a.px, a0.px, (size_t)a.w * a.h * 4) != 0;
    int db = memcmp(b.px, b0.px, (size_t)b.w * b.h * 4) != 0;
    int sep = a.d.scratch[OL_DS_SHADOW] && b.d.scratch[OL_DS_SHADOW] &&
              a.d.scratch[OL_DS_SHADOW] != b.d.scratch[OL_DS_SHADOW];
    if (da || db || !sep) FAILF("interleave a=%d b=%d separate=%d", da, db, sep);
    img_free(&a); img_free(&b); img_free(&a0); img_free(&b0);
}

int main(void)
{
    gfx_ctx_hook = hook;
    accuracy();
    mixed();
    frac_scale();
    mirror();
    inset_cases();
    linear();
    skip_and_clip();
    interleave();
    puts(bad ? "display_shadow_ex: FAILED" : "display_shadow_ex: OK");
    return bad;
}
