/* Open Logit -- image paints in vector fills: CLAMP/REPEAT/REFLECT spread and BOX downscale
 * (c/lib/gfx/raster/gfx_image_sample.c, core/ol_record.c, core/ol_exec.c).
 *
 *   image-fill        a circle filled with a retained ol_image equals image x circle coverage
 *                     (<= 2/255); a borrowed or foreign image paint is still refused
 *   image-fill-stale  updating the ol_image makes the recorded list OL_STALE_RESOURCE
 *   image-repeat      REPEAT tiles are exactly periodic (nearest and bilinear), REFLECT mirrors,
 *                     CLAMP holds the edge and flags 0 stays transparent outside (legacy)
 *   image-box         a 1024 -> 48 BOX downscale matches a double area average (<= 2/255), and
 *                     BOX at <= 2x shrink is bit-identical to the ordinary filter
 *
 * -DOL_IMAGE_PAINT_REJECT (the legacy paint_ok) is the control: 'image-fill' must FAIL.
 * Exits 1 on any failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gfx.h"
#include "gfx_paint_internal.h"
#include "openlogit.h"

int (*gfx_ctx_hook)(void);   /* no fb.c here */

static int fails, checks;
static void ck(int cond, const char *what, const char *detail)
{
    checks++;
    printf("%-4s %s%s%s\n", cond ? "ok" : "FAIL", what, detail && *detail ? "  " : "", detail ? detail : "");
    if (!cond) fails++;
}

static unsigned rnd_state = 777;
static unsigned rnd(void) { rnd_state = rnd_state * 1664525u + 1013904223u; return rnd_state >> 8; }

#define N 64
static unsigned char img[N * N * 4];
static unsigned char cover[N * N * 4], out[N * N * 4], work[N * N * 4], front[N * N * 4];

static void make_image(void)
{
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            unsigned char *p = img + (y * N + x) * 4;
            p[0] = x * 4; p[1] = y * 4; p[2] = (x ^ y) * 4; p[3] = 100 + ((x + y) * 2) % 156;
        }
}

/* ------------------------------------------------------------- image-fill -- */
static void image_fill(void)
{
    static unsigned char dm[8192], lm[1 << 16], sm[1024], im[1024];
    struct ol_device *dev; struct ol_list *l; struct ol_surface *s; struct ol_image *im_;
    if (ol_device_create(dm, ol_device_size(), OL_API_VERSION, OL_CAP_ATOMIC_FRAME, &dev) ||
        ol_list_create(dev, lm, sizeof lm, &l)) abort();
    struct ol_surface_desc sd = { sizeof sd, OL_FORMAT_RGBA8_STRAIGHT, N, N, N * 4, front, work,
                                  sizeof front, sizeof work };
    struct ol_image_desc id = { sizeof id, OL_FORMAT_RGBA8_STRAIGHT, N, N, N * 4, img, sizeof img };
    if (ol_surface_create(dev, sm, sizeof sm, &sd, &s) || ol_image_create(dev, im, sizeof im, &id, &im_)) abort();

    static int pt[2048], sub[8]; struct gfx_path circle;
    gfx_path_init(&circle, pt, 1024, sub, 8);
    gfx_path_circle(&circle, 32 * 256, 32 * 256, 28 * 256);

    /* coverage oracle: the same circle, solid white, through the same rasterizer */
    struct gfx_paint white; gfx_paint_solid(&white, 0xFFFFFF, 255);
    memset(front, 0, sizeof front); memset(work, 0, sizeof work);
    ol_list_reset(l);
    ol_cmd_fill(l, &circle, GFX_NONZERO, &white, 0, 4);
    ol_list_close(l);
    ol_submit(dev, l, s, 0);
    memcpy(cover, front, sizeof cover);

    struct gfx_paint ip;
    gfx_paint_image(&ip, img, N, N, N * 4, 0, 0);
    ip.img_ref = im_;
    ol_list_reset(l);
    memset(front, 0, sizeof front); memset(work, 0, sizeof work);
    int rr = ol_cmd_fill(l, &circle, GFX_NONZERO, &ip, 0, 4);
    ck(rr == OL_OK, "image-fill", "ol_cmd_fill records a retained image paint");
    ol_list_close(l);
    int sr = ol_submit(dev, l, s, 0);
    ck(sr == OL_OK, "image-fill", "the list submits");
    memcpy(out, front, sizeof out);
    int worst = 0, covered = 0;
    for (int i = 0; i < N * N; i++) {
        int c = cover[i * 4 + 3], a = img[i * 4 + 3] * c / 255;
        int da = abs(out[i * 4 + 3] - a);
        if (da > worst) worst = da;
        if (c) covered++;
        if (out[i * 4 + 3] > 8 || a > 8)
            for (int k = 0; k < 3; k++) { int d = abs(out[i * 4 + k] - img[i * 4 + k]); if (d > worst) worst = d; }
    }
    char msg[96]; snprintf(msg, sizeof msg, "max error %d/255 over %d covered pixels", worst, covered);
    ck(rr == OL_OK && sr == OL_OK && worst <= 2 && covered > 2000, "image-fill", msg);

    /* the legacy refusals stand: borrowed pixels, a foreign resource, a stray flag */
    struct gfx_paint bad; gfx_paint_image(&bad, img, N, N, N * 4, 0, 0);
    ol_list_reset(l);
    ck(ol_cmd_fill(l, &circle, GFX_NONZERO, &bad, 0, 4) == OL_ARGUMENT, "image-fill-refuse", "borrowed pixels refused");
    bad = ip; bad.img = img + N * N * 4;
    ol_list_reset(l);
    ck(ol_cmd_fill(l, &circle, GFX_NONZERO, &bad, 0, 4) == OL_ARGUMENT, "image-fill-refuse", "pixels outside the image refused");
    bad = ip; bad.flags = 4;
    ol_list_reset(l);
    ck(ol_cmd_fill(l, &circle, GFX_NONZERO, &bad, 0, 4) == OL_ARGUMENT, "image-fill-refuse", "unknown flag refused");

    /* stale: the image changes after recording */
    ol_list_reset(l);
    rr = ol_cmd_fill(l, &circle, GFX_NONZERO, &ip, 0, 4);
    ol_list_close(l);
    int first = rr ? rr : ol_submit(dev, l, s, 0);
    unsigned char px4[4] = { 1, 2, 3, 255 };
    int up = ol_image_update(im_, (struct gfx_rect){ 0, 0, 1, 1 }, px4, 4, 4);
    int second = ol_submit(dev, l, s, 0);
    ck(first == OL_OK && up == OL_OK && second == OL_STALE_RESOURCE, "image-fill-stale",
       "updating the ol_image after recording gives OL_STALE_RESOURCE");
    ck(ol_image_destroy(im_) == OL_BUSY, "image-fill-stale", "the list retains the image");
    ol_list_reset(l);
    ck(ol_image_destroy(im_) == OL_OK, "image-fill-stale", "reset releases it");
}

/* ----------------------------------------------------------- image-repeat -- */
#define W 96
static unsigned char surfpx[W * W * 4], full[W];
static struct gfx_surface surf;
static void render(const struct gfx_paint *p)
{
    memset(surfpx, 0, sizeof surfpx);
    gfx_surface_init(&surf, surfpx, W, W, W * 4);
    memset(full, 255, sizeof full);
    for (int y = 0; y < W; y++) gfx_paint_row(p, &surf, y, 0, W, full, 0);
}
static const unsigned char *at(int x, int y) { return surfpx + (y * W + x) * 4; }
static int same(const unsigned char *a, const unsigned char *b) { return !memcmp(a, b, 4); }

static void image_repeat(void)
{
    static unsigned char t[8 * 8 * 4];
    for (int i = 0; i < 64; i++) { t[i*4] = rnd(); t[i*4+1] = rnd(); t[i*4+2] = rnd(); t[i*4+3] = 255; }
    struct gfx_paint p; struct gfx_matrix m;
    /* 1:1, nearest */
    gfx_m_identity(&m); m.e = 3 * 256; m.f = 5 * 256;
    gfx_paint_image(&p, t, 8, 8, 32, &m, 0);
    p.spread = GFX_SPREAD_REPEAT;
    render(&p);
    int ok = 1;
    for (int y = 0; y < W - 8; y++) for (int x = 0; x < W - 8; x++)
        if (!same(at(x, y), at(x + 8, y)) || !same(at(x, y), at(x, y + 8))) ok = 0;
    ck(ok, "image-repeat", "nearest REPEAT is exactly periodic in x and y");
    int tex = 1;
    for (int y = 0; y < W; y++) for (int x = 0; x < W; x++) {
        int ix = ((x - 3) % 8 + 8) % 8, iy = ((y - 5) % 8 + 8) % 8;
        if (!same(at(x, y), t + (iy * 8 + ix) * 4)) tex = 0;
    }
    ck(tex, "image-repeat", "REPEAT shows the texel (x-3 mod 8, y-5 mod 8)");
    /* 3x magnified, bilinear: period 24 device pixels */
    gfx_m_identity(&m); gfx_m_scale(&m, 3 * 65536, 3 * 65536);
    gfx_paint_image(&p, t, 8, 8, 32, &m, 1);
    p.spread = GFX_SPREAD_REPEAT;
    render(&p);
    ok = 1;
    for (int y = 0; y < W - 24; y++) for (int x = 0; x < W - 24; x++)
        if (!same(at(x, y), at(x + 24, y)) || !same(at(x, y), at(x, y + 24))) ok = 0;
    ck(ok, "image-repeat", "bilinear REPEAT at 3x is exactly periodic (24 px)");
    /* REFLECT: nearest 1:1 mirrors about the image edge */
    gfx_m_identity(&m);
    gfx_paint_image(&p, t, 8, 8, 32, &m, 0);
    p.spread = GFX_SPREAD_REFLECT;
    render(&p);
    ok = 1;
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
        if (!same(at(8 + x, y), at(7 - x, y)) || !same(at(x, 8 + y), at(x, 7 - y)) ||
            !same(at(16 + x, y), at(x, y))) ok = 0;
    ck(ok, "image-repeat", "REFLECT mirrors at each edge with period 16");
    /* CLAMP vs legacy transparent, image shifted to (10,10) */
    gfx_m_identity(&m); m.e = 10 * 256; m.f = 10 * 256;
    gfx_paint_image(&p, t, 8, 8, 32, &m, 0);
    render(&p);
    ck(at(2, 2)[3] == 0 && at(40, 40)[3] == 0 && same(at(12, 12), t + (2 * 8 + 2) * 4),
       "image-repeat", "flags 0 and spread PAD stay transparent outside (legacy)");
    p.flags = GFX_PAINT_IMG_CLAMP;
    render(&p);
    ck(same(at(2, 2), t) && same(at(40, 40), t + (7 * 8 + 7) * 4) && same(at(40, 10), t + 7 * 4) &&
       same(at(12, 12), t + (2 * 8 + 2) * 4), "image-repeat", "IMG_CLAMP holds the edge texels");
}

/* -------------------------------------------------------------- image-box -- */
#define BW 1024
#define DW 48
static unsigned char big[BW * BW * 4];

static void image_box(void)
{
    for (int i = 0; i < BW * BW; i++) {
        big[i*4] = rnd(); big[i*4+1] = rnd(); big[i*4+2] = rnd(); big[i*4+3] = 128 + (rnd() & 127);
    }
    struct gfx_matrix m; gfx_m_identity(&m);
    m.a = m.d = DW * 65536 / BW;
    struct gfx_paint p;
    gfx_paint_image(&p, big, BW, BW, BW * 4, &m, 1);
    p.flags = GFX_PAINT_FILTER_BOX;
    render(&p);
    double s = (double)BW / DW;
    int worst = 0;
    for (int dy = 0; dy < DW; dy++) for (int dx = 0; dx < DW; dx++) {
        double sa = 0, sc[3] = { 0, 0, 0 }, area = 0;
        for (int j = (int)floor(dy * s); j < (int)ceil((dy + 1) * s) && j < BW; j++)
            for (int i = (int)floor(dx * s); i < (int)ceil((dx + 1) * s) && i < BW; i++) {
                double wx = fmin((dx + 1) * s, i + 1) - fmax(dx * s, i);
                double wy = fmin((dy + 1) * s, j + 1) - fmax(dy * s, j);
                if (wx <= 0 || wy <= 0) continue;
                const unsigned char *q = big + (j * BW + i) * 4;
                double w = wx * wy; area += w; sa += w * q[3];
                for (int k = 0; k < 3; k++) sc[k] += w * q[3] * q[k];
            }
        double ra = sa / area;
        const unsigned char *g = at(dx, dy);
        if (fabs(g[3] - ra) > worst) worst = (int)ceil(fabs(g[3] - ra));
        for (int k = 0; k < 3; k++) {
            double rc = sc[k] / sa;
            if (fabs(g[k] - rc) > worst) worst = (int)ceil(fabs(g[k] - rc));
        }
    }
    char msg[96]; snprintf(msg, sizeof msg, "1024->48 vs double area average, max error %d/255", worst);
    ck(worst <= 2, "image-box", msg);

    unsigned char boxed[DW * DW * 4];
    for (int y = 0; y < DW; y++) memcpy(boxed + y * DW * 4, at(0, y), DW * 4);
    p.flags = 0;
    render(&p);
    int differ = 0;
    for (int y = 0; y < DW; y++) for (int x = 0; x < DW; x++)
        if (memcmp(boxed + (y * DW + x) * 4, at(x, y), 4)) differ++;
    ck(differ > DW * DW / 2, "image-box", "without the flag a 21x shrink is the plain filter (BOX changes the result)");

    /* below the 2x threshold the flag does nothing */
    gfx_m_identity(&m); m.a = m.d = 65536 * 3 / 4;
    gfx_paint_image(&p, big, BW, BW, BW * 4, &m, 1);
    render(&p);
    unsigned char plain[W * W * 4]; memcpy(plain, surfpx, sizeof plain);
    p.flags = GFX_PAINT_FILTER_BOX;
    render(&p);
    ck(!memcmp(plain, surfpx, sizeof plain), "image-box", "BOX at a 4/3 shrink is bit-identical to the plain filter");
    gfx_m_identity(&m); m.a = m.d = 65536 / 2;     /* 0.5x: inverse scale 2.0 exactly, not > 2 */
    gfx_paint_image(&p, big, BW, BW, BW * 4, &m, 1);
    render(&p);
    memcpy(plain, surfpx, sizeof plain);
    p.flags = GFX_PAINT_FILTER_BOX;
    render(&p);
    ck(!memcmp(plain, surfpx, sizeof plain), "image-box", "BOX at exactly 2x is bit-identical to the plain filter");
}

int main(void)
{
    make_image();
    image_fill();
    image_repeat();
    image_box();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
