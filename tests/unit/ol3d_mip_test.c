/* gfx3d mip chain and LOD selection. Checks: the chain equals an independent double-precision
 * box reference; a checkerboard plane receding to the horizon is filtered (no far-half
 * high-frequency energy) with LINEAR mips; level choice follows the quad derivatives;
 * mip_filter NONE is bit-identical to the base (recorded scene hash). Build with
 * -DOL3D_MIP_BASELINE on a tree without the mip fields to print that hash. */
#include "openlogit_3d.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int failed;
#define CHECK(c, n)                                                                                \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("FAIL %s\n", n);                                                                \
            failed++;                                                                              \
        } else                                                                                     \
            printf("PASS %s\n", n);                                                                \
    } while (0)
#define W 128
static const char *vs = "ols 1 vertex\ninput r0 0\nposition r0\ninput r1 1\nvarying 0 r1\n";
static const char *ps = "ols 1 pixel\ninput r0 0\ntex2d r1 r0 0\ncolor r1\n";
static struct ol_shader_program vert, pixel;
static struct ol3d_vertex vtx(float x, float y, float z, float w, float u, float v)
{
    struct ol3d_vertex r = {0};
    float p[4] = {x, y, z, w}, t[4] = {u, v, 0, 1};
    memcpy(r.attribute[0], p, sizeof p);
    memcpy(r.attribute[1], t, sizeof t);
    return r;
}
static int draw(struct ol3d_context *c, const struct ol3d_texture *tex, const struct ol3d_vertex v[4])
{
    uint32_t ix[6] = {0, 1, 2, 0, 2, 3};
    struct ol3d_draw d = {0};
    d.pipeline.vertex = &vert;
    d.pipeline.pixel = &pixel;
    d.pipeline.varying_count = 1;
    d.bindings.textures = tex;
    d.bindings.texture_count = 1;
    d.vertices = v;
    d.vertex_count = 4;
    d.indices = ix;
    d.index_count = 6;
    return ol3d_draw_indexed(c, &d);
}
static void render(struct ol3d_context *c, const struct ol3d_texture *tex, const struct ol3d_vertex v[4],
                   unsigned char *img)
{
    ol3d_begin(c, 0);
    draw(c, tex, v);
    ol3d_end(c, 0);
    ol3d_readback(c, img, W * W * 4, W * 4);
}
/* Ground plane at y=-1 seen from depth d in [1, 50]: clip (+-d, -1, .5, d). */
static void plane(struct ol3d_vertex v[4], float uscale)
{
    v[0] = vtx(-1, -1, .5f, 1, -.5f * uscale, 1 * uscale);
    v[1] = vtx(1, -1, .5f, 1, .5f * uscale, 1 * uscale);
    v[2] = vtx(50, -1, .5f, 50, 25 * uscale, 50 * uscale);
    v[3] = vtx(-50, -1, .5f, 50, -25 * uscale, 50 * uscale);
}
static void checker(unsigned char *p, unsigned w, unsigned h, unsigned sq)
{
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
            unsigned char g = ((x / sq + y / sq) & 1) ? 255 : 0;
            unsigned char *q = p + ((size_t)y * w + x) * 4;
            q[0] = q[1] = q[2] = g;
            q[3] = 255;
        }
}
static double lum(const unsigned char *img, int x, int y)
{
    return img[((size_t)y * W + x) * 4];
}
/* Mean absolute horizontal luminance step over the far half of the plane. */
static double far_energy(const unsigned char *img)
{
    double sum = 0;
    int n = 0;
    for (int y = 68; y < 82; y++)
        for (int x = 1; x < W; x++) {
            sum += fabs(lum(img, x, y) - lum(img, x - 1, y));
            n++;
        }
    return sum / n;
}
static uint64_t fnv(const unsigned char *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
#ifndef OL3D_MIP_BASELINE
static int ref_chain(unsigned w, unsigned h)
{
    unsigned n, bad = 0;
    size_t bytes;
    unsigned char *base = malloc((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h * 4; i++)
        base[i] = (unsigned char)(i * 2654435761u >> 13);
    ol3d_texture_mip_size(w, h, &n, &bytes);
    unsigned char *store = malloc(bytes ? bytes : 1);
    struct ol3d_mip_level lv[16];
    struct ol3d_texture t = {.pixels = base, .width = w, .height = h, .stride = w * 4,
                             .bytes = (size_t)w * h * 4};
    unsigned got = 99;
    int r = ol3d_texture_build_mips(&t, lv, 16, store, bytes, &got);
    if (r || got != n)
        bad++;
    /* Independent reference: every level from the previous one, in double. */
    const unsigned char *src = base;
    unsigned sw = w, sh = h;
    for (unsigned l = 0; l < n && !bad; l++) {
        unsigned dw = sw > 1 ? sw / 2 : 1, dh = sh > 1 ? sh / 2 : 1;
        if (lv[l].width != dw || lv[l].height != dh)
            bad++;
        for (unsigned y = 0; y < dh && !bad; y++)
            for (unsigned x = 0; x < dw; x++)
                for (int k = 0; k < 4; k++) {
                    double s = 0;
                    for (unsigned j = 0; j < 2; j++)
                        for (unsigned i = 0; i < 2; i++) {
                            unsigned xx = 2 * x + i, yy = 2 * y + j;
                            xx = xx < sw ? xx : sw - 1;
                            yy = yy < sh ? yy : sh - 1;
                            s += src[((size_t)yy * sw + xx) * 4 + k];
                        }
                    if (lv[l].pixels[((size_t)y * dw + x) * 4 + k] != (unsigned char)floor(s / 4 + .5))
                        bad++;
                }
        src = lv[l].pixels;
        sw = dw;
        sh = dh;
    }
    /* Too little storage or too few level slots is refused, never overrun. */
    if (n && ol3d_texture_build_mips(&t, lv, n - 1, store, bytes, &got) != OL_LIMIT)
        bad++;
    if (bytes && ol3d_texture_build_mips(&t, lv, 16, store, bytes - 1, &got) != OL_LIMIT)
        bad++;
    free(base);
    free(store);
    return !bad;
}
#endif
static uint64_t none_scene_hash(struct ol3d_context *c)
{
    static unsigned char tex[16 * 16 * 4], img[W * W * 4];
    checker(tex, 16, 16, 3);
    struct ol3d_texture t = {.pixels = tex, .width = 16, .height = 16, .stride = 64, .bytes = sizeof tex,
                             .bilinear = 1, .repeat = 1};
    struct ol3d_vertex v[4];
    plane(v, .6f);
    render(c, &t, v, img);
    uint64_t h = fnv(img, sizeof img);
    t.bilinear = 0;
    render(c, &t, v, img);
    return h ^ fnv(img, sizeof img) * 31;
}
int main(void)
{
    struct ol_shader_error e;
    struct ol3d_context *c;
    if (ol_shader_compile(vs, strlen(vs), &vert, &e) || ol_shader_compile(ps, strlen(ps), &pixel, &e) ||
        ol3d_create(W, W, &c)) {
        puts("FAIL setup");
        return 1;
    }
#ifdef OL3D_MIP_BASELINE
    printf("scene hash %llx\n", (unsigned long long)none_scene_hash(c));
    return 0;
#else
    CHECK(ref_chain(64, 64) && ref_chain(37, 20) && ref_chain(1, 9) && ref_chain(8, 1) &&
              ref_chain(1, 1) && ref_chain(129, 130),
          "mip levels equal the double box reference");
    CHECK(none_scene_hash(c) == OL3D_MIP_SCENE_HASH, "NONE is bit-identical to the base");

    /* Receding checkerboard. */
    static unsigned char base[128 * 128 * 4], store[128 * 128 * 2], img0[W * W * 4], img1[W * W * 4];
    checker(base, 128, 128, 8);
    struct ol3d_mip_level lv[8];
    unsigned n = 0;
    struct ol3d_texture t = {.pixels = base, .width = 128, .height = 128, .stride = 512,
                             .bytes = sizeof base, .bilinear = 1, .repeat = 1};
    int br = ol3d_texture_build_mips(&t, lv, 8, store, sizeof store, &n);
    CHECK(!br && n == 7, "build_mips of 128x128 gives 7 levels");
    struct ol3d_vertex pv[4];
    plane(pv, 1);
    render(c, &t, pv, img0);
    t.mips = lv;
    t.mip_count = n;
    t.mip_filter = OL3D_MIP_NONE;
    render(c, &t, pv, img1);
    CHECK(!memcmp(img0, img1, sizeof img0), "NONE ignores an attached chain");
    double e0 = far_energy(img0);
    t.mip_filter = OL3D_MIP_LINEAR;
    render(c, &t, pv, img1);
    double e1 = far_energy(img1);
    printf("far-half energy: NONE %.2f LINEAR %.2f\n", e0, e1);
    CHECK(e0 > 20, "unfiltered far half is aliased (precondition)");
    CHECK(e1 < 6 && e1 < e0 / 4, "aliasing");
    t.mip_filter = OL3D_MIP_NEAREST;
    render(c, &t, pv, img1);
    CHECK(far_energy(img1) < e0 / 2, "NEAREST mip filter also reduces far energy");

    /* Analytic LOD: a screen-aligned quad whose u,v span k texture widths minifies k:1, so
     * lod = log2(k). Levels are flat greys 10 (0), 100, 150, 200 (1..3). */
    static unsigned char lvl[3][64 * 64 * 4], l0[128 * 128 * 4];
    struct ol3d_mip_level fl[3];
    for (int i = 0; i < 128 * 128; i++)
        memcpy(l0 + 4 * i, (unsigned char[4]){10, 10, 10, 255}, 4);
    unsigned sz[3] = {64, 32, 16};
    for (int l = 0; l < 3; l++) {
        for (unsigned i = 0; i < sz[l] * sz[l]; i++) {
            unsigned char g = (unsigned char)(50 * (l + 2));
            memcpy(lvl[l] + 4 * i, (unsigned char[4]){g, g, g, 255}, 4);
        }
        fl[l] = (struct ol3d_mip_level){lvl[l], sz[l], sz[l], sz[l] * 4, (size_t)sz[l] * sz[l] * 4};
    }
    struct ol3d_texture ft = {.pixels = l0, .width = 128, .height = 128, .stride = 512,
                              .bytes = sizeof l0, .repeat = 1, .mips = fl, .mip_count = 3,
                              .mip_filter = OL3D_MIP_NEAREST};
    static const struct { float k; int want; } nl[] = {{1, 10}, {2, 100}, {4, 150}, {8, 200}, {32, 200}};
    int ok = 1;
    for (unsigned i = 0; i < sizeof nl / sizeof *nl; i++) {
        float k = nl[i].k;
        struct ol3d_vertex q[4] = {vtx(-1, -1, .5f, 1, 0, k), vtx(1, -1, .5f, 1, k, k),
                                   vtx(1, 1, .5f, 1, k, 0), vtx(-1, 1, .5f, 1, 0, 0)};
        render(c, &ft, q, img0);
        if (img0[(64 * W + 64) * 4] != nl[i].want) {
            ok = 0;
            printf("  k=%g got %d want %d\n", k, img0[(64 * W + 64) * 4], nl[i].want);
        }
    }
    CHECK(ok, "NEAREST picks level log2(rho) from the quad derivatives");
    ft.mip_filter = OL3D_MIP_LINEAR;
    {
        struct ol3d_vertex q[4] = {vtx(-1, -1, .5f, 1, 0, 3), vtx(1, -1, .5f, 1, 3, 3),
                                   vtx(1, 1, .5f, 1, 3, 0), vtx(-1, 1, .5f, 1, 0, 0)};
        render(c, &ft, q, img0);
        int g = img0[(64 * W + 64) * 4];
        double want = 100 + 50 * (log2(3.0) - 1);
        printf("  LINEAR k=3 got %d want %.1f\n", g, want);
        CHECK(fabs(g - want) <= 1.5, "LINEAR blends adjacent levels by the fractional LOD");
        struct ol3d_vertex m[4] = {vtx(-1, -1, .5f, 1, 0, .25f), vtx(1, -1, .5f, 1, .25f, .25f),
                                   vtx(1, 1, .5f, 1, .25f, 0), vtx(-1, 1, .5f, 1, 0, 0)};
        render(c, &ft, m, img0);
        ft.mip_filter = OL3D_MIP_NONE;
        render(c, &ft, m, img1);
        CHECK(!memcmp(img0, img1, sizeof img0), "magnification (lod < 0) is level 0 exactly");
    }
    {
        ft.mip_filter = OL3D_MIP_FILTER_COUNT;
        struct ol3d_vertex q[4] = {vtx(-1, -1, .5f, 1, 0, 1), vtx(1, -1, .5f, 1, 1, 1),
                                   vtx(1, 1, .5f, 1, 1, 0), vtx(-1, 1, .5f, 1, 0, 0)};
        ol3d_begin(c, 0);
        CHECK(draw(c, &ft, q) != OL_OK, "bad mip_filter is rejected");
        ol3d_end(c, 0);
    }
    ol3d_destroy(c);
    printf("%s\n", failed ? "RESULT FAIL" : "RESULT PASS");
    return failed ? 1 : 0;
#endif
}
