/* Canvas extensions: save_layer group opacity, image/sprite wrappers,
 * configurable stack depth and samples. */
#include "ol_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 48
static unsigned checks, failures;
static struct ol_device *device;
static struct ol_list *list;
static unsigned char front[W * H * 4], work[W * H * 4];
static struct ol_surface *surface;
static void *surface_storage;
static unsigned long scratch[(512 * 1024) / 8];

static void check(int ok, const char *name)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}
static int submit_to(struct ol_surface *s)
{
    return ol_list_close(list) == OL_OK && ol_submit(device, list, s, NULL) == OL_OK;
}
static void begin(struct ol_canvas *canvas)
{
    ol_list_reset(list);
    ol_canvas_init(canvas, list, W, H);
    ol_canvas_init_layers(canvas, scratch, sizeof scratch);
    ol_cmd_clear(list, 0, 255);
}
static void rect(struct ol_canvas *canvas, int x, int y, int w, int h, unsigned rgb)
{
    struct gfx_paint paint;
    gfx_paint_solid(&paint, rgb, 255);
    ol_canvas_round_rect(canvas, (struct gfx_rect){x, y, w, h}, 0, &paint);
}
static const unsigned char *px(int x, int y)
{
    return front + (y * W + x) * 4;
}

static void overlap_case(void)
{
    struct ol_canvas canvas;
    begin(&canvas);
    int ok = ol_canvas_save_layer(&canvas, (struct gfx_rect){0, 0, W, H}, 128, GFX_OP_SRC_OVER) ==
             OL_OK;
    rect(&canvas, 8, 8, 24, 24, 0xff0000);
    rect(&canvas, 20, 20, 24, 24, 0x0000ff);
    ok &= ol_canvas_restore(&canvas) == OL_OK && canvas.list == list && canvas.depth == 0;
    check(ok && submit_to(surface), "layer frame submits");
    /* Group first, then one 0.5 composite over opaque black: the overlap shows
     * the top child only, at half strength. */
    int red = abs(px(10, 10)[0] - 128) <= 1 && px(10, 10)[2] == 0;
    int blue = abs(px(40, 40)[2] - 128) <= 1 && px(40, 40)[0] == 0;
    int both = abs(px(25, 25)[2] - 128) <= 1 && px(25, 25)[0] == 0;
    check(red && blue && px(2, 2)[0] == 0 && px(2, 2)[3] == 255, "layer regions");
    check(both, "overlap");

    /* Oracle: render the group on its own transparent surface with a plain
     * canvas, then composite it at 128 with ol_cmd_layer. */
    static unsigned char gf[W * H * 4], gw[W * H * 4], keep[W * H * 4];
    void *gs = malloc(ol_surface_size());
    struct ol_surface *group;
    struct ol_surface_desc d = {sizeof d, OL_FORMAT_RGBA8_STRAIGHT, W, H, W * 4,
                                gf, gw, sizeof gf, sizeof gw};
    if (ol_surface_create(device, gs, ol_surface_size(), &d, &group))
        abort();
    ol_list_reset(list);
    ol_canvas_init(&canvas, list, W, H);
    ol_cmd_clear(list, 0, 0);
    rect(&canvas, 8, 8, 24, 24, 0xff0000);
    rect(&canvas, 20, 20, 24, 24, 0x0000ff);
    submit_to(group);
    ol_list_reset(list);
    ol_cmd_clear(list, 0, 255);
    struct ol_layer_options o = {.size = sizeof o, .opacity = 128};
    ol_cmd_layer(list, group, (struct gfx_rect){0, 0, W, H}, &o);
    submit_to(surface);
    memcpy(keep, front, sizeof keep);
    begin(&canvas);
    ol_canvas_save_layer(&canvas, (struct gfx_rect){0, 0, W, H}, 128, GFX_OP_SRC_OVER);
    rect(&canvas, 8, 8, 24, 24, 0xff0000);
    rect(&canvas, 20, 20, 24, 24, 0x0000ff);
    ol_canvas_restore(&canvas);
    submit_to(surface);
    int worst = 0;
    for (int i = 0; i < W * H * 4; i++)
        worst = abs(keep[i] - front[i]) > worst ? abs(keep[i] - front[i]) : worst;
    printf("layer vs render-then-composite oracle: max delta %d\n", worst);
    check(worst == 0, "layer equals oracle");
    ol_list_reset(list);
    free(gs);
}

static void ops(void)
{
    struct ol_canvas canvas;
    begin(&canvas);
    unsigned long used = list->used, arena = canvas.layer_used;
    int r = ol_canvas_save_layer(&canvas, (struct gfx_rect){0, 0, 10, 10}, 128,
                                 (enum gfx_op)1);
    check(r == OL_UNSUPPORTED && list->used == used && list->error == 0 && canvas.depth == 0 &&
              canvas.list == list && canvas.layer_used == arena,
          "non src-over op unsupported and records nothing");
    r = ol_canvas_save_layer(&canvas, (struct gfx_rect){0, 0, 10, 10}, 300, GFX_OP_SRC_OVER);
    check(r < 0 && list->error != 0 && canvas.depth == 0, "bad opacity latches");
}

static void layers_misc(void)
{
    struct ol_canvas canvas;
    /* Nested layers, clipped bounds, arena rewind, local coordinates. */
    begin(&canvas);
    int ok = ol_canvas_save_layer(&canvas, (struct gfx_rect){10, 10, 30, 30}, 255, GFX_OP_SRC_OVER) == OL_OK;
    unsigned long outer = canvas.layer_used;
    ok &= ol_canvas_save_layer(&canvas, (struct gfx_rect){0, 0, 12, 12}, 255, GFX_OP_SRC_OVER) == OL_OK;
    ok &= canvas.layer_used > outer;
    rect(&canvas, 0, 0, 100, 100, 0x00ff00); /* user space unchanged, clipped to the 12x12 inner group */
    ok &= ol_canvas_restore(&canvas) == OL_OK;
    unsigned long after_inner = canvas.layer_used;
    ok &= ol_canvas_restore(&canvas) == OL_OK && canvas.depth == 0 && canvas.layer_used < after_inner;
    check(ok && submit_to(surface), "nested layers");
    check(px(10, 10)[1] == 255 && px(21, 21)[1] == 255 && px(22, 22)[1] == 0 && px(9, 9)[1] == 0,
          "nested layer lands at its clipped bounds");
    /* A second sibling reuses the rewound space and layer_used keeps the first group. */
    begin(&canvas);
    ok = 1;
    for (int i = 0; i < 3; i++) {
        ok &= ol_canvas_save_layer(&canvas, (struct gfx_rect){i * 10, 0, 8, 8}, 255, GFX_OP_SRC_OVER) == OL_OK;
        rect(&canvas, i * 10, 0, 8, 8, 0xffffff); /* user space is unchanged by a layer */
        ok &= ol_canvas_restore(&canvas) == OL_OK;
    }
    ok &= submit_to(surface);
    check(ok && px(0, 0)[0] == 255 && px(10, 0)[0] == 255 &&
              px(20, 7)[0] == 255 && px(9, 0)[0] == 0,
          "sibling layers survive until list reset");
    /* Fully clipped bounds are a clip-only level. */
    begin(&canvas);
    ok = ol_canvas_save_layer(&canvas, (struct gfx_rect){W + 5, 0, 8, 8}, 255, GFX_OP_SRC_OVER) == OL_OK;
    rect(&canvas, 0, 0, W, H, 0xffffff);
    ok &= ol_canvas_restore(&canvas) == OL_OK && submit_to(surface) && px(3, 3)[0] == 0;
    check(ok, "offscreen layer draws nothing");
}

static void exhaustion(void)
{
    struct ol_canvas canvas;
    ol_list_reset(list);
    ol_canvas_init(&canvas, list, W, H);
    int none = ol_canvas_save_layer(&canvas, (struct gfx_rect){0, 0, 8, 8}, 128, GFX_OP_SRC_OVER);
    ol_canvas_init_layers(&canvas, scratch, 4096);
    unsigned long used = list->used;
    int small = ol_canvas_save_layer(&canvas, (struct gfx_rect){0, 0, 8, 8}, 128, GFX_OP_SRC_OVER);
    check(none == OL_LIMIT && small == OL_LIMIT && canvas.depth == 0 && list->error == 0 &&
              list->used == used && canvas.list == list && canvas.layer_used == 0,
          "layer scratch exhaustion returns OL_LIMIT and leaves state intact");
    ol_canvas_init_layers(&canvas, scratch, sizeof scratch);
    check(ol_canvas_save_layer(&canvas, (struct gfx_rect){0, 0, 8, 8}, 128, GFX_OP_SRC_OVER) == OL_OK,
          "same canvas recovers with room");
    ol_canvas_restore(&canvas);
}

static void stack(void)
{
    static struct ol_canvas_state deep[64];
    struct ol_canvas canvas;
    ol_list_reset(list);
    int ok = ol_canvas_init_ex(&canvas, list, W, H, deep, 64) == OL_OK;
    for (int i = 0; i < 64; i++)
        ok &= ol_canvas_save(&canvas) == OL_OK;
    ok &= canvas.depth == 64;
    for (int i = 0; i < 64; i++)
        ok &= ol_canvas_restore(&canvas) == OL_OK;
    check(ok && canvas.depth == 0 && list->error == 0, "depth-64 custom stack");
    ok = 1;
    for (int i = 0; i < 64; i++)
        ok &= ol_canvas_save(&canvas) == OL_OK;
    check(ok && ol_canvas_save(&canvas) == OL_LIMIT && list->error == OL_LIMIT, "depth 65 refused");
    ol_list_reset(list);
    ol_canvas_init(&canvas, list, W, H);
    for (int i = 0; i < OL_CANVAS_STACK; i++)
        ok &= ol_canvas_save(&canvas) == OL_OK;
    check(ol_canvas_save(&canvas) == OL_LIMIT, "default stack stays 16");
    ol_list_reset(list);
    check(ol_canvas_init_ex(&canvas, list, W, H, deep, 0) == OL_ARGUMENT &&
              ol_canvas_init_ex(&canvas, list, W, H, (struct ol_canvas_state *)((char *)deep + 1), 4) ==
                  OL_ARGUMENT,
          "init_ex rejects bad stacks");
}

/* Slanted near-horizontal edge, coverage vs a supersampled area oracle. */
static int edge_error(int samples)
{
    struct ol_canvas canvas;
    begin(&canvas);
    ol_canvas_set_samples(&canvas, samples);
    int pts[32], subs[4];
    struct gfx_path path;
    gfx_path_init(&path, pts, 16, subs, 4);
    gfx_move_to(&path, GFX_PX(4), GFX_PX(10));
    gfx_line_to(&path, GFX_PX(60), GFX_PX(13));
    gfx_line_to(&path, GFX_PX(60), GFX_PX(30));
    gfx_line_to(&path, GFX_PX(4), GFX_PX(30));
    gfx_close(&path);
    struct gfx_paint paint;
    gfx_paint_solid(&paint, 0xffffff, 255);
    ol_canvas_fill(&canvas, &path, GFX_NONZERO, &paint);
    submit_to(surface);
    int worst = 0;
    for (int y = 8; y < 16; y++)
        for (int x = 6; x < 58; x++) {
            int in = 0;
            for (int j = 0; j < 32; j++)
                for (int i = 0; i < 32; i++) {
                    double sx = x + (i + 0.5) / 32, sy = y + (j + 0.5) / 32;
                    in += sy >= 10 + (sx - 4) * 3.0 / 56;
                }
            int want = (int)(in * 255.0 / 1024 + 0.5), e = abs(want - px(x, y)[0]);
            worst = e > worst ? e : worst;
        }
    return worst;
}
static void samples(void)
{
    struct ol_canvas canvas;
    begin(&canvas);
    int bad = ol_canvas_set_samples(&canvas, 0) != OL_OK;
    check(bad && list->error != 0, "set_samples rejects 0");
    begin(&canvas);
    check(ol_canvas_set_samples(&canvas, 32) == OL_OK && ol_canvas_set_samples(&canvas, 1) == OL_OK &&
              ol_canvas_set_samples(&canvas, 33) != OL_OK,
          "set_samples range 1..32");
    begin(&canvas);
    ol_canvas_set_samples(&canvas, 16);
    ol_canvas_save(&canvas);
    ol_canvas_set_samples(&canvas, 2);
    ol_canvas_restore(&canvas);
    check(canvas.state.samples == 16, "samples are saved state");
    int e4 = edge_error(4), e16 = edge_error(16);
    printf("near-horizontal edge max error: 4 samples %d/255, 16 samples %d/255\n", e4, e16);
    check(e16 < e4 && e16 * 2 <= e4, "set_samples(16) beats 4 samples");
}

static void images(void)
{
    static unsigned char pixels[4 * 4 * 4];
    for (int i = 0; i < 16; i++) {
        pixels[i * 4] = 255;
        pixels[i * 4 + 1] = 0;
        pixels[i * 4 + 2] = 0;
        pixels[i * 4 + 3] = 255;
    }
    void *st = malloc(ol_image_size());
    struct ol_image *im;
    struct ol_image_desc d = {sizeof d, OL_FORMAT_RGBA8_STRAIGHT, 4, 4, 16, pixels, sizeof pixels};
    if (ol_image_create(device, st, ol_image_size(), &d, &im))
        abort();
    struct ol_canvas canvas;
    begin(&canvas);
    int ok = ol_canvas_image(&canvas, im, (struct gfx_rect){0, 0, 4, 4}, (struct gfx_rect){10, 10, 8, 8}, 0) == OL_OK;
    struct gfx_matrix t = {GFX_MONE, 0, 0, GFX_MONE, GFX_PX(30), GFX_PX(20)};
    ol_canvas_save(&canvas);
    ol_canvas_transform(&canvas, &t);
    ok &= ol_canvas_image(&canvas, im, (struct gfx_rect){0, 0, 4, 4}, (struct gfx_rect){0, 0, 8, 8}, 1) == OL_OK;
    struct gfx_matrix s = {2 * GFX_MONE, 0, 0, 2 * GFX_MONE, GFX_PX(-20), GFX_PX(14)};
    ok &= ol_canvas_sprite(&canvas, im, (struct gfx_rect){0, 0, 4, 4}, &s, 0) == OL_OK;
    ol_canvas_restore(&canvas);
    check(ok && submit_to(surface), "image/sprite record and submit");
    check(px(10, 10)[0] == 255 && px(17, 17)[0] == 255 && px(18, 18)[0] == 0 && px(9, 10)[0] == 0,
          "image identity CTM");
    check(px(30, 20)[0] == 255 && px(37, 27)[0] == 255 && px(38, 28)[0] == 0,
          "image through translated CTM");
    check(px(10, 34)[0] == 255 && px(17, 41)[0] == 255 && px(18, 42)[0] == 0,
          "sprite transform composes with CTM");
    /* A mask cannot be honoured: refuse, record nothing. */
    begin(&canvas);
    static unsigned char a8[4 * 4];
    void *st2 = malloc(ol_image_size());
    struct ol_image *m;
    struct ol_image_desc md = {sizeof md, OL_FORMAT_A8, 4, 4, 4, a8, sizeof a8};
    if (ol_image_create(device, st2, ol_image_size(), &md, &m))
        abort();
    ol_canvas_mask(&canvas, m, 0, 0);
    unsigned long used = list->used;
    check(ol_canvas_image(&canvas, im, (struct gfx_rect){0, 0, 4, 4}, (struct gfx_rect){0, 0, 4, 4}, 0) ==
                  OL_UNSUPPORTED &&
              ol_canvas_sprite(&canvas, im, (struct gfx_rect){0, 0, 4, 4}, &s, 0) == OL_UNSUPPORTED &&
              list->used == used && list->error == 0,
          "image/sprite refuse a coverage mask");
    ol_list_reset(list);
    ol_image_destroy(m);
    ol_image_destroy(im);
    free(st);
    free(st2);
}

int main(void)
{
    void *dev = malloc(ol_device_size()), *ls = malloc(262144);
    surface_storage = malloc(ol_surface_size());
    struct ol_surface_desc sd = {sizeof sd, OL_FORMAT_RGBA8_STRAIGHT, W, H, W * 4, front, work,
                                 sizeof front, sizeof work};
    if (ol_device_create(dev, ol_device_size(), OL_API_VERSION, 0, &device) ||
        ol_list_create(device, ls, 262144, &list) ||
        ol_surface_create(device, surface_storage, ol_surface_size(), &sd, &surface))
        return 2;
    overlap_case();
    ops();
    layers_misc();
    exhaustion();
    stack();
    samples();
    images();
    printf("canvas ext: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
