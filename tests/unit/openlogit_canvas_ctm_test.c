/* canvas-path-ctm: ol_canvas_path_begin flattens under the CTM, and gfx_m_mul /
 * ol_canvas_transform share one rounding rule (half away from zero).
 * -DOL_CANVAS_POST_FLATTEN makes path_begin a no-op; "FAIL deviation" must fire. */
#include "openlogit.h"
#include "openlogit_canvas.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 200
#define H 200
static unsigned checks, failures;
static void check(int ok, const char *name)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}

/* The pre-change gfx_m_mul: >> 16 floors. Kept here as the old-vs-new oracle. */
static void old_mul(struct gfx_matrix *o, const struct gfx_matrix *l, const struct gfx_matrix *r)
{
    struct gfx_matrix t;
    t.a = (int)(((long long)l->a * r->a + (long long)l->c * r->b) >> 16);
    t.b = (int)(((long long)l->b * r->a + (long long)l->d * r->b) >> 16);
    t.c = (int)(((long long)l->a * r->c + (long long)l->c * r->d) >> 16);
    t.d = (int)(((long long)l->b * r->c + (long long)l->d * r->d) >> 16);
    t.e = (int)(((long long)l->a * r->e + (long long)l->c * r->f) >> 16) + l->e;
    t.f = (int)(((long long)l->b * r->e + (long long)l->d * r->f) >> 16) + l->f;
    *o = t;
}

static struct ol_device *device;
static struct ol_list *list;
static unsigned char front[W * H * 4], work[W * H * 4];
static struct ol_surface *surface;

static void deviation(void)
{
    struct ol_canvas canvas;
    ol_list_reset(list);
    ol_canvas_init(&canvas, list, W, H);
    struct gfx_matrix s;
    gfx_m_identity(&s);
    gfx_m_scale(&s, 8 * GFX_MONE, 8 * GFX_MONE);
    check(ol_canvas_transform(&canvas, &s) == OL_OK, "scale(8) accepted");
    static int pts[4096], subs[8];
    struct gfx_path path;
    gfx_path_init(&path, pts, 2048, subs, 8);
    check(ol_canvas_path_begin(&canvas, &path) == OL_OK, "path_begin");
    gfx_path_circle(&path, GFX_PX(12), GFX_PX(12), GFX_PX(10));
    check(!path.overflow && path.npt > 8, "circle recorded");
    int dev = ol_canvas_path_is_device(&canvas, &path);
    /* Device points: as stored when the path flattened under the CTM, else what
     * ol_cmd_fill_ex would produce by transforming user-space points. */
    double cx = 96.0, cy = 96.0, r = 80.0, worst = 0;
    int n = path.npt;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n, x[2], y[2], k[2] = {i, j};
        for (int q = 0; q < 2; q++) {
            if (dev) { x[q] = pts[k[q] * 2]; y[q] = pts[k[q] * 2 + 1]; }
            else gfx_m_apply(&s, pts[k[q] * 2], pts[k[q] * 2 + 1], &x[q], &y[q]);
        }
        /* The vertex, and the middle of the edge to the next one (the chord
         * sag is the largest deviation of a flattened arc). */
        double px[2] = {x[0] / 256.0, (x[0] + x[1]) / 512.0};
        double py[2] = {y[0] / 256.0, (y[0] + y[1]) / 512.0};
        for (int q = 0; q < 2; q++) {
            double d = fabs(hypot(px[q] - cx, py[q] - cy) - r);
            if (d > worst)
                worst = d;
        }
    }
    printf("max radial deviation %.5f px, %d points, device=%d\n", worst, n, dev);
    /* 1/32 px of flattening, plus the model error of the 4-cubic circle itself
     * (the control polygon overshoots a true circle by 2.7e-4 of r: 0.022 px at
     * r = 80), plus half an LSB of 24.8 rounding on the vertex. Post-flatten
     * under scale(8) is ~0.25 px, so this still separates the two. */
    check(worst <= 1.0 / 32 + r * 2.8e-4 + 1.0 / 512, "deviation");

    struct gfx_paint paint;
    gfx_paint_solid(&paint, 0xffffff, 255);
    ol_cmd_clear(list, 0, 255);
    check(ol_canvas_fill(&canvas, &path, GFX_NONZERO, &paint) == OL_OK, "fill records");
    check(ol_list_close(list) == OL_OK && ol_submit(device, list, surface, NULL) == OL_OK,
          "frame submits");
    double area = 0;
    for (int i = 0; i < W * H; i++)
        area += front[i * 4] / 255.0;
    double want = M_PI * r * r;
    printf("filled area %.1f want %.1f\n", area, want);
    check(front[(96 * W + 96) * 4] == 255 && front[(10 * W + 10) * 4] == 0, "interior and corner");
    check(fabs(area - want) <= want * 0.005, "area");
}

static unsigned lcg = 12345;
static int rnd(int n) { lcg = lcg * 1664525u + 1013904223u; return (int)((lcg >> 8) % (unsigned)n); }

static void rotations(void)
{
    double sum = 0, worst = 0;
    int count = 0;
    for (int chain = 0; chain < 50; chain++) {
        struct gfx_matrix m;
        gfx_m_identity(&m);
        double d[4] = {1, 0, 0, 1};
        for (int i = 0; i < 1000; i++) {
            int deg = rnd(360 * 256);
            int c = gfx_cos(deg), s = gfx_sin(deg);
            struct gfx_matrix rot;
            gfx_m_set(&rot, c, s, -s, c, 0, 0);
            gfx_m_mul(&m, &m, &rot);
            double r[4] = {c / 65536.0, s / 65536.0, -s / 65536.0, c / 65536.0};
            double t[4] = {d[0] * r[0] + d[2] * r[1], d[1] * r[0] + d[3] * r[1],
                           d[0] * r[2] + d[2] * r[3], d[1] * r[2] + d[3] * r[3]};
            memcpy(d, t, sizeof d);
        }
        int got[4] = {m.a, m.b, m.c, m.d};
        for (int k = 0; k < 4; k++) {
            double e = got[k] - d[k] * 65536.0; /* signed, in LSB */
            sum += e;
            count++;
            if (fabs(e) > worst)
                worst = fabs(e);
        }
    }
    printf("1000 rotations x 50 chains: max error %.2f LSB, mean signed error %.3f LSB\n", worst,
           sum / count);
    check(worst <= 64, "rotation error");
    check(fabs(sum / count) <= 3.0, "rotation bias");
}

static void mulfix(void)
{
    check(gfx_m_mulfix(32768) == 1 && gfx_m_mulfix(-32768) == -1, "half rounds away from zero");
    check(gfx_m_mulfix(32767) == 0 && gfx_m_mulfix(-32767) == 0, "below half rounds to zero");
    check(gfx_m_mulfix(65536 * 3 + 32768) == 4 && gfx_m_mulfix(-65536 * 3 - 32768) == -4,
          "symmetric");
}

static void old_vs_new(void)
{
    static const int s16[] = {65536, 32768, 6554, 3277, 1311, 655, 328, 262, 131, -655, 131072};
    struct gfx_matrix set[64];
    int n = 0;
    for (unsigned i = 0; i < sizeof s16 / sizeof *s16; i++)
        gfx_m_set(&set[n++], s16[i], 0, 0, s16[i], 0, 0);
    struct gfx_matrix m;
    gfx_m_identity(&m);
    gfx_m_translate(&m, 300 * 256, -120 * 256);
    gfx_m_rotate(&m, 30 * 256);
    gfx_m_scale(&m, -655, 655);
    set[n++] = m;
    for (int deg = 0; deg < 360; deg += 15) {
        gfx_m_set(&set[n++], gfx_cos(deg * 256), gfx_sin(deg * 256), -gfx_sin(deg * 256),
                  gfx_cos(deg * 256), 7 * 256 + 13, -5 * 256 - 77);
    }
    int worst = 0;
    long pairs = 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            struct gfx_matrix a, b;
            old_mul(&a, &set[i], &set[j]);
            gfx_m_mul(&b, &set[i], &set[j]);
            const int *pa = &a.a, *pb = &b.a;
            for (int k = 0; k < 6; k++) {
                int d = abs(pa[k] - pb[k]);
                if (d > worst)
                    worst = d;
            }
            pairs++;
        }
    printf("gfx_m_mul old vs new: %ld pairs, max element delta %d LSB\n", pairs, worst);
    check(worst <= 1, "old vs new within 1 LSB");
}

static void canvas_shares_rounding(void)
{
    struct ol_canvas canvas;
    ol_list_reset(list);
    ol_canvas_init(&canvas, list, W, H);
    struct gfx_matrix ref;
    gfx_m_identity(&ref);
    int ok = 1;
    for (int i = 0; i < 200 && ok; i++) {
        int deg = rnd(360 * 256), c = gfx_cos(deg), s = gfx_sin(deg);
        struct gfx_matrix rot;
        gfx_m_set(&rot, c, s, -s, c, rnd(512) - 256, rnd(512) - 256);
        gfx_m_mul(&ref, &ref, &rot);
        ok &= ol_canvas_transform(&canvas, &rot) == OL_OK;
        ok &= memcmp(&ref, &canvas.state.transform, sizeof ref) == 0;
    }
    check(ok, "ol_canvas_transform == gfx_m_mul");
}

int main(void)
{
    void *dev = malloc(ol_device_size()), *ls = malloc(262144), *ss = malloc(ol_surface_size());
    struct ol_surface_desc sd = {sizeof sd, OL_FORMAT_RGBA8_STRAIGHT, W, H, W * 4, front, work,
                                 sizeof front, sizeof work};
    if (ol_device_create(dev, ol_device_size(), OL_API_VERSION, 0, &device) ||
        ol_list_create(device, ls, 262144, &list) ||
        ol_surface_create(device, ss, ol_surface_size(), &sd, &surface))
        return 2;
    mulfix();
    old_vs_new();
    rotations();
    canvas_shares_rounding();
    deviation();
    printf("canvas path ctm: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
