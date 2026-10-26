/* Canvas clip stack: rrect/path clips multiply with the saved mask. */
#include "ol_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 160
#define H 120
static unsigned checks, failures;
static struct ol_device *device;
static struct ol_list *list;
static unsigned char front[W * H * 4], work[W * H * 4];
static struct ol_surface *surface;
static void *surface_storage;
static unsigned long arena_words[(256 * 1024) / 8];

static void check(int ok, const char *name)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}

static int render(struct ol_canvas *canvas, const struct gfx_rect *rect)
{
    struct gfx_paint paint;
    gfx_paint_solid(&paint, 0xffffff, 255);
    int r = ol_canvas_round_rect(canvas, *rect, 0, &paint);
    return r == OL_OK && ol_list_close(list) == OL_OK &&
           ol_submit(device, list, surface, NULL) == OL_OK;
}
static void begin(struct ol_canvas *canvas, int w, int h)
{
    ol_list_reset(list);
    ol_canvas_init(canvas, list, w, h);
    ol_canvas_init_clip_arena(canvas, arena_words, sizeof arena_words);
    ol_cmd_clear(list, 0, 255);
}

/* Independent oracle: NxN point samples of a rounded rect in user space. */
static double rr_cov(double px, double py, struct gfx_rect r, double rad)
{
    const int N = 16;
    int in = 0;
    for (int j = 0; j < N; j++)
        for (int i = 0; i < N; i++) {
            double x = px + (i + 0.5) / N, y = py + (j + 0.5) / N;
            if (x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h)
                continue;
            double cx = x < r.x + rad ? r.x + rad : x > r.x + r.w - rad ? r.x + r.w - rad : x;
            double cy = y < r.y + rad ? r.y + rad : y > r.y + r.h - rad ? r.y + r.h - rad : y;
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= rad * rad)
                in++;
        }
    return (double)in / (N * N);
}

/* Engine coverage of ONE rrect clip, for isolating the multiply from the
 * rasteriser's own single-layer accuracy. */
static void single_layer(struct gfx_rect r, const int rad[4], const struct gfx_matrix *t,
                         unsigned char *out)
{
    struct ol_canvas canvas;
    begin(&canvas, W, H);
    canvas.state.samples = 32;
    ol_canvas_transform(&canvas, t);
    ol_canvas_clip_rrect(&canvas, r, rad);
    render(&canvas, &(struct gfx_rect){-20, -20, 200, 160});
    for (int i = 0; i < W * H; i++)
        out[i] = front[i * 4];
}

static void nested(void)
{
    struct ol_canvas canvas;
    const double tx = 7 + 77 / 256.0, ty = 5 + 143 / 256.0;
    struct gfx_rect win = {0, 0, 120, 90}, card = {2, 2, 116, 86},
                    img = {3, 3, 114, 84}; /* each corner crosses the previous one */
    int r24[4] = {24, 24, 24, 24}, r12[4] = {12, 12, 12, 12}, r6[4] = {6, 6, 6, 6};
    struct gfx_matrix t = {GFX_MONE, 0, 0, GFX_MONE, GFX_PX(7) + 77, GFX_PX(5) + 143};
    static unsigned char one[3][W * H];
    single_layer(win, r24, &t, one[0]);
    single_layer(card, r12, &t, one[1]);
    single_layer(img, r6, &t, one[2]);
    begin(&canvas, W, H);
    canvas.state.samples = 32;
    int ok = ol_canvas_transform(&canvas, &t) == OL_OK;
    ok &= ol_canvas_clip_rrect(&canvas, win, r24) == OL_OK;
    ok &= ol_canvas_save(&canvas) == OL_OK;
    ok &= ol_canvas_clip_rrect(&canvas, card, r12) == OL_OK;
    ok &= ol_canvas_save(&canvas) == OL_OK;
    ok &= ol_canvas_clip_rrect(&canvas, img, r6) == OL_OK;
    check(ok, "nested clips accepted");
    check(render(&canvas, &(struct gfx_rect){-20, -20, 200, 160}), "nested frame submits");
    int product = 0, analytic = 0, partial = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int i = y * W + x, got = front[i * 4];
            double want = one[0][i] * (double)one[1][i] * one[2][i] / 65025.0;
            int e = abs((int)(want + 0.5) - got);
            product = e > product ? e : product;
            partial += one[0][i] && one[0][i] < 255;
            double ux = x - tx, uy = y - ty;
            double c = rr_cov(ux, uy, win, 24) * rr_cov(ux, uy, card, 12) * rr_cov(ux, uy, img, 6);
            e = abs((int)(c * 255 + 0.5) - got);
            analytic = e > analytic ? e : analytic;
        }
    printf("nested max error vs product of engine coverages %d/255, vs supersampled %d/255\n",
           product, analytic);
    check(product <= 3 && partial > 100, "nested");
    /* The engine's own single-layer edge sits up to 6/255 off the 16x16
     * supersample (gfx_raster horizontal precision); three layers add little. */
    check(analytic <= 8, "nested vs supersampled oracle");
}

static void restore_exact(void)
{
    struct ol_canvas canvas;
    int r10[4] = {10, 10, 10, 10};
    begin(&canvas, W, H);
    int ok = ol_canvas_clip_rrect(&canvas, (struct gfx_rect){4, 4, 100, 80}, r10) == OL_OK;
    struct ol_canvas_state before = canvas.state;
    unsigned long used = canvas.clip_arena_used;
    unsigned char copy[W * H];
    struct ol_surface_view v;
    ok &= ol_image_view(before.coverage, &v) == OL_OK;
    memcpy(copy, v.pixels, v.width * v.height);
    ok &= ol_canvas_save(&canvas) == OL_OK;
    ok &= ol_canvas_clip_rrect(&canvas, (struct gfx_rect){30, 30, 40, 40}, r10) == OL_OK;
    ok &= canvas.state.coverage != before.coverage && canvas.clip_arena_used > used;
    ok &= ol_canvas_restore(&canvas) == OL_OK;
    ok &= !memcmp(&canvas.state, &before, sizeof before) && canvas.clip_arena_used == used;
    ok &= ol_image_view(canvas.state.coverage, &v) == OL_OK &&
          !memcmp(copy, v.pixels, v.width * v.height);
    check(ok, "save clip restore returns the previous mask exactly");

    /* A recorded fill pins the mask: restore must not hand its bytes out again. */
    ok = ol_canvas_save(&canvas) == OL_OK;
    ok &= ol_canvas_clip_rrect(&canvas, (struct gfx_rect){30, 30, 40, 40}, r10) == OL_OK;
    struct gfx_paint paint;
    gfx_paint_solid(&paint, 0xffffff, 255);
    ok &= ol_canvas_round_rect(&canvas, (struct gfx_rect){0, 0, 100, 100}, 0, &paint) == OL_OK;
    unsigned long pinned = canvas.clip_arena_used;
    ok &= ol_canvas_restore(&canvas) == OL_OK && canvas.clip_arena_used == pinned;
    check(ok, "restore keeps arena bytes referenced by recorded fills");
}

static void exhaustion(void)
{
    struct ol_canvas canvas;
    int r8[4] = {8, 8, 8, 8};
    begin(&canvas, W, H);
    /* header plus 20x20 mask fits, a second one does not */
    unsigned long one = align8(sizeof(struct ol_image)) + 400;
    ol_canvas_init_clip_arena(&canvas, arena_words, one + 64);
    int ok = ol_canvas_clip_rrect(&canvas, (struct gfx_rect){0, 0, 20, 20}, r8) == OL_OK;
    struct ol_canvas_state before = canvas.state;
    unsigned long used = canvas.clip_arena_used;
    int e = ol_canvas_clip_rrect(&canvas, (struct gfx_rect){0, 0, 20, 20}, r8);
    ok &= e == OL_LIMIT && !memcmp(&canvas.state, &before, sizeof before) &&
          canvas.clip_arena_used == used && canvas.depth == 0;
    struct gfx_paint paint;
    gfx_paint_solid(&paint, 0xffffff, 255);
    ok &= ol_canvas_round_rect(&canvas, (struct gfx_rect){0, 0, 10, 10}, 0, &paint) == OL_OK &&
          ol_list_close(list) == OL_OK;
    check(ok, "arena exhaustion returns OL_LIMIT with state and list intact");

    begin(&canvas, 3000, 100);
    e = ol_canvas_clip_rrect(&canvas, (struct gfx_rect){0, 0, 2500, 50}, r8);
    check(e == OL_LIMIT && canvas.state.coverage == NULL && ol_list_close(list) == OL_OK,
          "oversize mask returns OL_LIMIT without poisoning the list");
    begin(&canvas, W, H);
    ol_canvas_init_clip_arena(&canvas, NULL, 0);
    check(ol_canvas_clip_rrect(&canvas, (struct gfx_rect){0, 0, 20, 20}, r8) == OL_LIMIT &&
              ol_canvas_clip_rrect(&canvas, (struct gfx_rect){0, 0, 20, 20}, NULL) == OL_ARGUMENT,
          "no arena is OL_LIMIT, null radii are OL_ARGUMENT");
}

static void path_evenodd(void)
{
    struct ol_canvas canvas;
    int pts[128], subs[8];
    struct gfx_path p;
    gfx_path_init(&p, pts, 64, subs, 8);
    gfx_path_rect(&p, GFX_PX(10), GFX_PX(10), GFX_PX(60), GFX_PX(60));
    gfx_path_rect(&p, GFX_PX(30), GFX_PX(30), GFX_PX(20), GFX_PX(20));
    for (int rule = GFX_NONZERO; rule <= GFX_EVENODD; rule++) {
        begin(&canvas, W, H);
        int ok = ol_canvas_clip_path(&canvas, &p, rule) == OL_OK;
        ok &= render(&canvas, &(struct gfx_rect){0, 0, W, H});
        int hole = front[(40 * W + 40) * 4], ring = front[(15 * W + 15) * 4],
            out = front[(5 * W + 5) * 4];
        ok &= ring == 255 && out == 0 && hole == (rule == GFX_EVENODD ? 0 : 255);
        check(ok, rule == GFX_EVENODD ? "path clip even-odd leaves a hole"
                                      : "path clip nonzero fills the inner square");
    }
    /* path clip goes through the CTM and multiplies with an earlier rrect */
    begin(&canvas, W, H);
    int r20[4] = {20, 20, 20, 20};
    struct gfx_matrix t = {GFX_MONE, 0, 0, GFX_MONE, GFX_PX(50), 0};
    int ok = ol_canvas_clip_rrect(&canvas, (struct gfx_rect){0, 0, 100, 100}, r20) == OL_OK;
    ok &= ol_canvas_transform(&canvas, &t) == OL_OK;
    ok &= ol_canvas_clip_path(&canvas, &p, GFX_EVENODD) == OL_OK;
    ok &= render(&canvas, &(struct gfx_rect){-50, 0, W, H});
    ok &= front[(15 * W + 65) * 4] == 255 && front[(40 * W + 90) * 4] == 0 &&
          front[(15 * W + 15) * 4] == 0 && front[(11 * W + 61) * 4] == 255;
    check(ok, "path clip is transformed and multiplied with the rrect clip");
}

static void mask_ex(void)
{
    struct ol_canvas canvas;
    unsigned char px[40 * 40];
    memset(px, 128, sizeof px);
    void *st = malloc(ol_image_size());
    struct ol_image *im;
    struct ol_image_desc d = {sizeof d, OL_FORMAT_A8, 40, 40, 40, px, sizeof px};
    if (ol_image_create(device, st, ol_image_size(), &d, &im))
        abort();
    begin(&canvas, W, H);
    int ok = ol_canvas_mask_ex(&canvas, im, 10, 10, 0) == OL_OK && canvas.state.coverage == im;
    ok &= ol_canvas_mask_ex(&canvas, im, 20, 20, 1) == OL_OK && canvas.state.coverage != im;
    struct ol_surface_view v;
    ok &= ol_image_view(canvas.state.coverage, &v) == OL_OK && v.width == 30 && v.height == 30 &&
          canvas.state.mask_x == 20 && v.pixels[0] == (128 * 128 + 127) / 255;
    check(ok, "ol_canvas_mask_ex intersect multiplies, replace is legacy");
    ol_image_destroy(im);
    free(st);
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
    nested();
    restore_exact();
    exhaustion();
    path_evenodd();
    mask_ex();
    printf("canvas clip stack: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
