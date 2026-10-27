/* gfx3d pipeline raster state: blend_op, depth_func, depth write mask, cull. The
 * scene hash at the end was recorded on the base commit (legacy fields only), so it
 * proves zero-valued new fields are bit-identical. Build with -DOL3D_STATE_BASELINE
 * on a tree without the new fields to print that hash. */
#include "openlogit_3d.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static int checks, failed;
#define CHECK(c, n)                                                                                \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(c)) {                                                                                \
            printf("FAIL %s\n", n);                                                                \
            failed++;                                                                              \
        } else                                                                                     \
            printf("PASS %s\n", n);                                                                \
    } while (0)
#define W 32
#ifdef OL3D_STATE_BASELINE
enum { OL3D_DEPTH_LESS, OL3D_DEPTH_LEQUAL, OL3D_DEPTH_ALWAYS, OL3D_DEPTH_NEVER, OL3D_DEPTH_GREATER };
#endif
static const char *vs = "ols 1 vertex\ninput r0 0\nposition r0\ninput r1 1\nvarying 0 r1\n";
static const char *ps = "ols 1 pixel\ninput r0 0\ncolor r0\n";
static struct ol_shader_program vert, pixel;
static struct ol3d_vertex vtx(float x, float y, float z, const float c[4])
{
    struct ol3d_vertex v = {0};
    float p[4] = {x, y, z, 1};
    memcpy(v.attribute[0], p, sizeof p);
    memcpy(v.attribute[1], c, 4 * sizeof(float));
    return v;
}
/* Quad spanning ndc x in [x0,x1], full height, at depth z. ccw=0 reverses winding. */
static int quad(struct ol3d_context *c, struct ol3d_pipeline pl, float x0, float x1, float z,
                const float col[4], int ccw)
{
    struct ol3d_vertex v[4] = {vtx(x0, -1, z, col), vtx(x1, -1, z, col), vtx(x1, 1, z, col),
                               vtx(x0, 1, z, col)};
    uint32_t cw[6] = {0, 2, 1, 0, 3, 2}, ow[6] = {0, 1, 2, 0, 2, 3};
    pl.vertex = &vert;
    pl.pixel = &pixel;
    pl.varying_count = 1;
    struct ol3d_draw d = {.pipeline = pl, .vertices = v, .vertex_count = 4,
                          .indices = ccw ? ow : cw, .index_count = 6};
    return ol3d_draw_indexed(c, &d);
}
static unsigned px(const unsigned char *img, int x, int y)
{
    const unsigned char *p = img + ((size_t)y * W + x) * 4;
    return (unsigned)p[0] << 16 | p[1] << 8 | p[2] | (unsigned)p[3] << 24;
}
static int finish(struct ol3d_context *c, unsigned char *img)
{
    return ol3d_end(c, 0) || ol3d_readback(c, img, W * W * 4, W * 4);
}
static const float RED[4] = {1, 0, 0, 1}, BLUE[4] = {0, 0, 1, 1};
static int ref_pass(int f, float z, float s)
{
    switch (f) {
    case OL3D_DEPTH_LESS: return z < s;
    case OL3D_DEPTH_LEQUAL: return z <= s;
    case OL3D_DEPTH_ALWAYS: return 1;
    case OL3D_DEPTH_NEVER: return 0;
    default: return z > s;
    }
}
static uint64_t scene_hash(struct ol3d_context *c)
{
    /* Legacy fields only: depth test + write, cull_back, blend flag. */
    const float a[4] = {1, .2f, 0, .6f}, b[4] = {0, .4f, 1, .5f}, d[4] = {.3f, 1, .3f, 1};
    unsigned char img[W * W * 4];
    ol3d_begin(c, 0x102030);
    struct ol3d_pipeline p0 = {.cull_back = 1, .depth_test = 1, .depth_write = 1, .blend = 1};
    struct ol3d_pipeline p1 = {.depth_test = 1, .depth_write = 0, .blend = 1};
    quad(c, p0, -1, .4f, .6f, d, 1);
    quad(c, p0, -.6f, 1, .4f, a, 1);
    quad(c, p1, -.3f, .9f, .5f, b, 1);
    quad(c, p0, -1, 1, .2f, b, 0); /* back-facing: culled */
    finish(c, img);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof img; i++)
        h = (h ^ img[i]) * 1099511628211ull;
    return h;
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
#ifdef OL3D_STATE_BASELINE
    printf("scene hash %llx\n", (unsigned long long)scene_hash(c));
    return 0;
#else
    unsigned char img[W * W * 4];
    /* Depth funcs: A (z=.5, LESS, writes) then B under test, over A and untouched depth. */
    int ok = 1;
    const float zs[3] = {.3f, .7f, .5f};
    for (int f = 0; f < OL3D_DEPTH_FUNC_COUNT; f++)
        for (int zi = 0; zi < 2; zi++) {
            ol3d_begin(c, 0);
            struct ol3d_pipeline pa = {.depth_test = 1, .depth_write = 1};
            struct ol3d_pipeline pb = {.depth_test = 1, .depth_write = 1, .depth_func = (unsigned char)f};
            quad(c, pa, -1, .5f, .5f, RED, 1);
            quad(c, pb, -.5f, 1, zs[zi], BLUE, 1);
            finish(c, img);
            unsigned over = ref_pass(f, zs[zi], .5f) ? 0x0000ff : 0xff0000;
            unsigned only = ref_pass(f, zs[zi], 1.f) ? 0x0000ff : 0;
            if ((px(img, 2, 16) & 0xffffff) != 0xff0000 || (px(img, 16, 16) & 0xffffff) != over ||
                (px(img, 29, 16) & 0xffffff) != only) {
                ok = 0;
                printf("  func %d z %.1f: A-only %06x overlap %06x (want %06x) B-only %06x (want %06x)\n", f,
                       zs[zi], px(img, 2, 16) & 0xffffff, px(img, 16, 16) & 0xffffff, over,
                       px(img, 29, 16) & 0xffffff, only);
            }
        }
    CHECK(ok, "depth-func");
    /* Equal depth: the identical quad redrawn has bit-equal z. */
    ok = 1;
    for (int f = 0; f < OL3D_DEPTH_FUNC_COUNT; f++) {
        ol3d_begin(c, 0);
        struct ol3d_pipeline pa = {.depth_test = 1, .depth_write = 1};
        struct ol3d_pipeline pb = {.depth_test = 1, .depth_write = 1, .depth_func = (unsigned char)f};
        quad(c, pa, -1, 1, zs[2], RED, 1);
        quad(c, pb, -1, 1, zs[2], BLUE, 1);
        finish(c, img);
        unsigned want = (f == OL3D_DEPTH_LEQUAL || f == OL3D_DEPTH_ALWAYS) ? 0x0000ff : 0xff0000;
        if ((px(img, 16, 16) & 0xffffff) != want)
            ok = 0;
    }
    CHECK(ok, "depth-func equal depth");
    /* Depth write mask: a masked near quad leaves depth at the clear, so a far quad shows. */
    for (int mode = 0; mode < 2; mode++) {
        ol3d_begin(c, 0);
        struct ol3d_pipeline pm = {.depth_test = 1, .depth_write = 1};
        if (mode)
            pm.no_depth_write = 1;
        else
            pm.depth_write = 0;
        struct ol3d_pipeline pf = {.depth_test = 1, .depth_write = 1};
        quad(c, pm, -1, 1, .2f, RED, 1);
        quad(c, pf, -1, 1, .7f, BLUE, 1);
        finish(c, img);
        CHECK((px(img, 16, 16) & 0xffffff) == 0x0000ff,
              mode ? "no_depth_write leaves depth unchanged" : "depth_write=0 leaves depth unchanged");
    }
    ol3d_begin(c, 0);
    {
        struct ol3d_pipeline pf = {.depth_test = 1, .depth_write = 1};
        quad(c, pf, -1, 1, .2f, RED, 1);
        quad(c, pf, -1, 1, .7f, BLUE, 1);
        finish(c, img);
        CHECK((px(img, 16, 16) & 0xffffff) == 0xff0000, "depth write enabled hides the far quad");
    }
    /* Cull. CCW draw = front face. */
    static const struct { int cull, cull_back, ccw, shown; const char *n; } cc[] = {
        {OL3D_CULL_FRONT, 0, 1, 0, "cull FRONT drops CCW"},
        {OL3D_CULL_FRONT, 0, 0, 1, "cull FRONT keeps CW"},
        {OL3D_CULL_FRONT, 1, 1, 0, "cull FRONT overrides cull_back"},
        {OL3D_CULL_BACK, 1, 0, 0, "cull BACK drops CW"},
        {OL3D_CULL_BACK, 1, 1, 1, "cull BACK keeps CCW"},
        {OL3D_CULL_BACK, 0, 0, 1, "cull BACK with cull_back=0 keeps CW (legacy)"},
        {OL3D_CULL_NONE, 1, 0, 1, "cull NONE keeps CW over cull_back"},
        {OL3D_CULL_NONE, 1, 1, 1, "cull NONE keeps CCW"},
    };
    for (unsigned i = 0; i < sizeof cc / sizeof *cc; i++) {
        ol3d_begin(c, 0);
        struct ol3d_pipeline p = {.cull = (unsigned char)cc[i].cull, .cull_back = cc[i].cull_back};
        quad(c, p, -1, 1, .5f, RED, cc[i].ccw);
        finish(c, img);
        CHECK(((px(img, 16, 16) & 0xffffff) == 0xff0000) == cc[i].shown, cc[i].n);
    }
    /* blend_op per fragment equals gfx_compose. */
    {
        const float col[4] = {.5f, .25f, 1.f, .5f};
        unsigned clear = 0x204060;
        int pass = 1;
        for (int op = 0; op < GFX_OP_COUNT; op++) {
            ol3d_begin(c, clear);
            struct ol3d_pipeline p = {.blend = 1, .blend_op = (unsigned char)op};
            quad(c, p, -1, 1, .5f, col, 1);
            finish(c, img);
            uint8_t ref[4] = {clear >> 16, clear >> 8, clear, 255};
            int r = (int)(col[0] * 255 + .5f), g = (int)(col[1] * 255 + .5f),
                b = (int)(col[2] * 255 + .5f), a = (int)(col[3] * 255 + .5f);
            gfx_compose((enum gfx_op)op, ref, (unsigned)r << 16 | g << 8 | b, a, 255);
            if (memcmp(ref, img + ((size_t)16 * W + 16) * 4, 4)) {
                pass = 0;
                printf("  op %d mismatch\n", op);
            }
        }
        CHECK(pass, "blend_op equals gfx_compose per fragment (all ops)");
        ol3d_begin(c, clear);
        struct ol3d_pipeline pl = {.blend_op = GFX_OP_PLUS_LIGHTER}, po = {.blend = 1};
        quad(c, pl, -1, 1, .5f, col, 1);
        finish(c, img);
        unsigned lighter = px(img, 16, 16);
        ol3d_begin(c, clear);
        quad(c, po, -1, 1, .5f, col, 1);
        finish(c, img);
        CHECK(lighter != px(img, 16, 16) && (lighter & 0xff) > (px(img, 16, 16) & 0xff),
              "PLUS_LIGHTER applies without the legacy blend flag and brightens");
    }
    /* Validation of the snapshot. */
    {
        struct ol3d_pipeline p = {.vertex = &vert, .pixel = &pixel, .varying_count = 1};
        struct ol3d_pipeline_object *o;
        CHECK(!ol3d_pipeline_create(&p, &o, &e), "valid raster state creates a snapshot");
        ol3d_pipeline_destroy(o);
        struct ol3d_pipeline bad[3] = {p, p, p};
        bad[0].blend_op = GFX_OP_COUNT;
        bad[1].depth_func = OL3D_DEPTH_FUNC_COUNT;
        bad[2].cull = OL3D_CULL_COUNT;
        int rej = 1;
        for (int i = 0; i < 3; i++)
            rej &= ol3d_pipeline_create(&bad[i], &o, &e) != OL_OK && !o;
        CHECK(rej, "out-of-range raster state rejected");
    }
    CHECK(scene_hash(c) == OL3D_STATE_SCENE_HASH, "zero defaults are bit-identical to base");
    ol3d_destroy(c);
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
#endif
}
