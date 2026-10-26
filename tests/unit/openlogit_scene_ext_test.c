/* Retained scene2d extensions: per-layer 24.8 translate and 8.8 scale, a set of
 * up to 8 disjoint damage rects, effect extent, z-reorder and hit-testing.
 * Every check prints PASS or FAIL <name>; any FAIL makes the exit status 1.
 * The negative control (-DOL_SCENE_SINGLE_DAMAGE) must fail "damage-area". */
#include "openlogit_canvas.h"
#include "openlogit_scene2d.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures;
static struct ol_device *device;
static struct ol_list *list;
struct image {
    struct ol_surface *surface;
    void *storage;
    unsigned char *front, *work;
    unsigned w, h;
};
static void check(int ok, const char *name)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}
static struct image image(unsigned width, unsigned height)
{
    struct image r = {0};
    r.w = width;
    r.h = height;
    r.storage = malloc(ol_surface_size());
    r.front = calloc(width * height, 4);
    r.work = malloc(width * height * 4);
    struct ol_surface_desc desc = {sizeof desc, OL_FORMAT_RGBA8_STRAIGHT, width, height,
                                   width * 4, r.front, r.work, width * height * 4,
                                   width * height * 4};
    if (ol_surface_create(device, r.storage, ol_surface_size(), &desc, &r.surface))
        abort();
    return r;
}
static void destroy(struct image *i)
{
    ol_list_reset(list); /* drop the list's references first */
    ol_surface_destroy(i->surface);
    free(i->storage);
    free(i->front);
    free(i->work);
}
static void solid(struct image *im, unsigned rgb, int alpha)
{
    unsigned char *p = malloc(im->w * im->h * 4);
    for (unsigned i = 0; i < im->w * im->h; i++) {
        p[i * 4] = rgb >> 16;
        p[i * 4 + 1] = rgb >> 8;
        p[i * 4 + 2] = rgb;
        p[i * 4 + 3] = alpha;
    }
    if (ol_surface_upload(im->surface, p, im->w * im->h * 4, im->w * 4))
        abort();
    free(p);
}
static int render(struct ol_scene2d *s, struct image *t, struct ol_submit_info *info)
{
    *info = (struct ol_submit_info){.size = sizeof *info};
    return ol_scene2d_render(s, list, t->surface, NULL, info);
}
static long long total_area(const struct ol_scene2d *s)
{
    long long a = 0;
    for (int i = 0; i < s->damage_count; i++)
        a += (long long)s->damage[i].w * s->damage[i].h;
    return a;
}
static int covers(const struct ol_scene2d *s, struct gfx_rect r)
{
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) {
            int in = 0;
            for (int i = 0; i < s->damage_count; i++) {
                struct gfx_rect d = s->damage[i];
                in |= x >= d.x && x < d.x + d.w && y >= d.y && y < d.y + d.h;
            }
            if (!in)
                return 0;
        }
    return 1;
}
static int disjoint(const struct ol_scene2d *s)
{
    for (int i = 0; i < s->damage_count; i++)
        for (int j = i + 1; j < s->damage_count; j++) {
            struct gfx_rect a = s->damage[i], b = s->damage[j];
            if (a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h)
                return 0;
        }
    return 1;
}

static void damage_set(void)
{
    struct image a = image(16, 16), b = image(16, 16), t = image(200, 100);
    solid(&a, 0xff0000, 255);
    solid(&b, 0x0000ff, 255);
    struct ol_scene2d s;
    struct ol_submit_info info;
    ol_scene2d_init(&s, device, 200, 100);
    ol_scene2d_set(&s, 0, a.surface, (struct gfx_rect){4, 4, 16, 16}, 255, 1);
    ol_scene2d_set(&s, 1, b.surface, (struct gfx_rect){170, 70, 16, 16}, 255, 1);
    check(render(&s, &t, &info) == OL_OK && s.damage_count == 1, "first frame is one full rect");
    /* Two distant layers each move a few pixels. */
    ol_scene2d_set(&s, 0, a.surface, (struct gfx_rect){7, 5, 16, 16}, 255, 1);
    ol_scene2d_set(&s, 1, b.surface, (struct gfx_rect){167, 72, 16, 16}, 255, 1);
    int ok = render(&s, &t, &info) == OL_OK;
    struct gfx_rect uni = {4, 4, 186 - 4, 88 - 4};
    check(ok && s.damage_count == 2 && disjoint(&s) && total_area(&s) < (long long)uni.w * uni.h,
          "damage-area");
    check(ok && covers(&s, (struct gfx_rect){4, 4, 19, 17}) &&
              covers(&s, (struct gfx_rect){167, 70, 19, 18}),
          "damage rects cover every old and new placement");
    check(t.front[(6 * 200 + 8) * 4] == 255 && t.front[(6 * 200 + 3) * 4 + 3] == 0 &&
              t.front[(80 * 200 + 175) * 4 + 2] == 255 && t.front[(80 * 200 + 186) * 4 + 3] == 0,
          "both moved layers drawn and old places erased");
    /* Overlapping old/new of a nearby move merge into one rect. */
    ol_scene2d_set(&s, 0, a.surface, (struct gfx_rect){9, 5, 16, 16}, 255, 1);
    check(render(&s, &t, &info) == OL_OK && s.damage_count == 1, "overlapping placements merge");
    check(render(&s, &t, &info) == OL_OK && s.damage_count == 0 && !info.commands,
          "unchanged frame has no damage and no commands");
    /* More distinct places than slots: still 8 disjoint rects at most and all covered. */
    struct image c = image(6, 6);
    solid(&c, 0x00ff00, 255);
    struct ol_scene2d m;
    ol_scene2d_init(&m, device, 200, 100);
    for (unsigned i = 0; i < 12; i++)
        ol_scene2d_set(&m, i, c.surface, (struct gfx_rect){(int)(i % 6) * 32 + 2, (int)(i / 6) * 50 + 2, 6, 6},
                       255, 1);
    render(&m, &t, &info);
    for (unsigned i = 0; i < 12; i++)
        ol_scene2d_set(&m, i, c.surface, (struct gfx_rect){(int)(i % 6) * 32 + 3, (int)(i / 6) * 50 + 3, 6, 6},
                       255, 1);
    int r = render(&m, &t, &info);
    int cov = 1;
    for (unsigned i = 0; i < 12; i++)
        cov &= covers(&m, (struct gfx_rect){(int)(i % 6) * 32 + 2, (int)(i / 6) * 50 + 2, 7, 7});
    check(r == OL_OK && m.damage_count >= 1 && m.damage_count <= OL_SCENE2D_DAMAGE && cov &&
              disjoint(&m),
          "twelve moving layers fit the 8 rect cap and stay covered");
    destroy(&c);
    ol_list_reset(list);
    ol_scene2d_release(&s);
    ol_scene2d_release(&m);
    destroy(&a);
    destroy(&b);
    destroy(&t);
}

/* 16x16 opaque source: three vertical colour bands, so every edge is hard. */
static void bands(struct image *im)
{
    unsigned char *p = malloc(im->w * im->h * 4);
    for (unsigned y = 0; y < im->h; y++)
        for (unsigned x = 0; x < im->w; x++) {
            unsigned char *q = p + (y * im->w + x) * 4;
            q[0] = x < 6 ? 255 : 0;
            q[1] = x >= 6 && x < 10 ? 255 : 0;
            q[2] = x >= 10 ? 255 : 0;
            q[3] = 255;
        }
    ol_surface_upload(im->surface, p, im->w * im->h * 4, im->w * 4);
    free(p);
}
/* Box-filtered supersampled reference: source pixels replicated 8x8 and the
 * layer origin placed at ox8,oy8 eighths. Returns the max error over all
 * pixels with reference alpha >= 64 (alpha is always compared). */
static int reference_error(const struct image *src, const struct image *t, int ox8, int oy8,
                           int *alpha_err)
{
    int worst = 0;
    *alpha_err = 0;
    for (unsigned py = 0; py < t->h; py++)
        for (unsigned px = 0; px < t->w; px++) {
            int cnt = 0, sum[3] = {0, 0, 0};
            for (int sy = 0; sy < 8; sy++)
                for (int sx = 0; sx < 8; sx++) {
                    int lx = (int)px * 8 + sx - ox8, ly = (int)py * 8 + sy - oy8;
                    if (lx < 0 || ly < 0 || lx >= (int)src->w * 8 || ly >= (int)src->h * 8)
                        continue;
                    const unsigned char *q = src->front + ((ly / 8) * src->w + lx / 8) * 4;
                    cnt++;
                    for (int k = 0; k < 3; k++)
                        sum[k] += q[k];
                }
            const unsigned char *o = t->front + (py * t->w + px) * 4;
            int ra = cnt * 255 / 64;
            int ea = abs(ra - o[3]);
            if (ea > *alpha_err)
                *alpha_err = ea;
            if (ra < 64)
                continue;
            for (int k = 0; k < 3; k++) {
                int e = abs(sum[k] / cnt - o[k]);
                if (e > worst)
                    worst = e;
            }
        }
    return worst;
}

static void subpixel(void)
{
    struct image a = image(16, 16), t = image(64, 40);
    bands(&a);
    struct ol_scene2d s;
    struct ol_submit_info info;
    ol_scene2d_init(&s, device, 64, 40);
    ol_scene2d_set(&s, 0, a.surface, (struct gfx_rect){20, 10, 16, 16}, 255, 1);
    ol_scene2d_set_xf(&s, 0, 10 * 256 + 128, 0, 0, 0);
    int rc = render(&s, &t, &info);
    int aerr, cerr = reference_error(&a, &t, (20 * 8) + 84, 10 * 8, &aerr);
    printf("  tx=10.5: rc=%d colour err %d alpha err %d\n", rc, cerr, aerr);
    check(rc == OL_OK && cerr <= 3 && aerr <= 3, "tx=10.5 matches the supersampled reference");
    /* The seam between two bands at a half-pixel offset is exactly 50/50. */
    const unsigned char *seam = t.front + (15 * 64 + 36) * 4; /* red|green seam */
    check(seam[0] >= 126 && seam[0] <= 129 && seam[1] >= 126 && seam[1] <= 129,
          "half-pixel seam blends 50/50");
    check(t.front[(15 * 64 + 30) * 4 + 3] >= 125 && t.front[(15 * 64 + 30) * 4 + 3] <= 130,
          "fractional leading edge is anti-aliased (alpha about 50%)");
    /* Both axes fractional. */
    ol_scene2d_set_xf(&s, 0, 3 * 256 + 64, 2 * 256 + 64, 0, 0);
    rc = render(&s, &t, &info);
    cerr = reference_error(&a, &t, (20 * 8) + 26, 10 * 8 + 18, &aerr);
    printf("  tx=3.25 ty=2.25: rc=%d colour err %d alpha err %d\n", rc, cerr, aerr);
    check(rc == OL_OK && cerr <= 3 && aerr <= 4, "fractional tx and ty match the reference");
    /* Scale 2x: bilinear, so a band boundary has an intermediate pixel. */
    ol_scene2d_set_xf(&s, 0, 0, 0, 512, 0);
    rc = render(&s, &t, &info);
    int mid = 0, hard = 0;
    for (int x = 20; x < 52; x++) {
        const unsigned char *q = t.front + (15 * 64 + x) * 4;
        if (q[3] == 255 && q[0] > 8 && q[0] < 247)
            mid++;
        if (q[3] == 255 && (q[0] == 255 || q[0] == 0))
            hard++;
    }
    check(rc == OL_OK && mid >= 1 && hard >= 20 && t.front[(15 * 64 + 51) * 4 + 3] == 255 &&
              t.front[(15 * 64 + 52) * 4 + 3] == 0,
          "2x scale is bilinear and covers 32 pixels");
    /* Scale 0.5. */
    ol_scene2d_set_xf(&s, 0, 0, 0, 128, 0);
    rc = render(&s, &t, &info);
    check(rc == OL_OK && t.front[(14 * 64 + 27) * 4 + 3] == 255 &&
              t.front[(14 * 64 + 28) * 4 + 3] == 0 && t.front[(18 * 64 + 22) * 4 + 3] == 0 &&
              t.front[(17 * 64 + 22) * 4 + 3] == 255,
          "half scale shrinks about the layer's top-left");
    check(ol_scene2d_hit(&s, 27, 17) == 0 && ol_scene2d_hit(&s, 28, 17) == -1,
          "hit follows the scaled rect");
    ol_scene2d_set_xf(&s, 0, 0, 0, 0, 0);
    ol_list_reset(list);
    ol_scene2d_release(&s);
    destroy(&a);
    destroy(&t);
}

static void zero_defaults(void)
{
    struct image r = image(12, 12), b = image(12, 12), t1 = image(64, 40), t2 = image(64, 40),
                 t3 = image(64, 40);
    solid(&r, 0xff0000, 255);
    solid(&b, 0x0000ff, 128);
    struct ol_scene2d s1, s2;
    struct ol_submit_info info;
    ol_scene2d_init(&s1, device, 64, 40);
    ol_scene2d_init(&s2, device, 64, 40);
    ol_scene2d_set(&s1, 0, r.surface, (struct gfx_rect){5, 6, 12, 12}, 255, 1);
    ol_scene2d_set(&s1, 1, b.surface, (struct gfx_rect){12, 9, 12, 12}, 200, 1);
    ol_scene2d_set(&s2, 0, r.surface, (struct gfx_rect){5, 6, 12, 12}, 255, 1);
    ol_scene2d_set(&s2, 1, b.surface, (struct gfx_rect){12, 9, 12, 12}, 200, 1);
    ol_scene2d_set_xf(&s2, 0, 0, 0, 256, 0);
    int rc = render(&s1, &t1, &info);
    /* Direct composition with the pre-extension command sequence. */
    struct gfx_rect full = {0, 0, 64, 40};
    struct ol_layer_options o = {.size = sizeof o, .clip = &full, .opacity = 255};
    ol_list_reset(list);
    ol_cmd_clear_rect(list, full, 0, 0);
    ol_cmd_layer(list, r.surface, (struct gfx_rect){5, 6, 12, 12}, &o);
    o.opacity = 200;
    ol_cmd_layer(list, b.surface, (struct gfx_rect){12, 9, 12, 12}, &o);
    ol_list_close(list);
    ol_submit_damage(device, list, t3.surface, &info);
    check(rc == OL_OK && !memcmp(t1.front, t3.front, 64 * 40 * 4),
          "zero defaults are bit-identical to the plain command sequence");
    check(render(&s2, &t2, &info) == OL_OK && !memcmp(t1.front, t2.front, 64 * 40 * 4),
          "explicit scale 256 and zero translate are bit-identical");
    /* Whole-pixel translate == moving the bounds. */
    ol_scene2d_set_xf(&s2, 1, 3 * 256, -2 * 256, 0, 0);
    render(&s2, &t2, &info);
    ol_scene2d_set(&s1, 1, b.surface, (struct gfx_rect){15, 7, 12, 12}, 200, 1);
    render(&s1, &t1, &info);
    check(!memcmp(t1.front, t2.front, 64 * 40 * 4), "integer translate equals moved bounds");
    /* Extent only widens damage. */
    ol_scene2d_set_xf(&s2, 1, 3 * 256, -2 * 256, 0, 6);
    render(&s2, &t2, &info);
    ol_scene2d_set_xf(&s2, 1, 4 * 256, -2 * 256, 0, 6);
    render(&s2, &t2, &info);
    check(s2.damage_count == 1 && covers(&s2, (struct gfx_rect){12 + 3 - 6, 9 - 2 - 6, 12 + 1 + 12, 24}),
          "effect extent widens damage around old and new placement");
    ol_list_reset(list);
    ol_scene2d_release(&s1);
    ol_scene2d_release(&s2);
    destroy(&r);
    destroy(&b);
    destroy(&t1);
    destroy(&t2);
    destroy(&t3);
}

static void ordering(void)
{
    struct image a = image(10, 10), b = image(10, 10), c = image(10, 10), t = image(40, 30);
    solid(&a, 0xff0000, 255);
    solid(&b, 0x00ff00, 255);
    solid(&c, 0x0000ff, 255);
    struct ol_scene2d s;
    struct ol_submit_info info;
    ol_scene2d_init(&s, device, 40, 30);
    ol_scene2d_set(&s, 0, a.surface, (struct gfx_rect){5, 5, 10, 10}, 255, 1);
    ol_scene2d_set(&s, 1, b.surface, (struct gfx_rect){5, 5, 10, 10}, 255, 1);
    ol_scene2d_set(&s, 2, c.surface, (struct gfx_rect){10, 5, 10, 10}, 255, 1);
    render(&s, &t, &info);
    check(ol_scene2d_hit(&s, 12, 8) == 2 && ol_scene2d_hit(&s, 7, 8) == 1 &&
              ol_scene2d_hit(&s, 30, 8) == -1 && ol_scene2d_hit(&s, 4, 8) == -1 &&
              ol_scene2d_hit(&s, 14, 14) == 2 && ol_scene2d_hit(&s, 15, 14) == 2 && ol_scene2d_hit(&s, 20, 8) == -1,
          "hit returns the topmost layer under the point");
    check(ol_scene2d_move(&s, 2, 0) == OL_OK && s.layers[0].surface == c.surface &&
              s.layers[1].surface == a.surface && s.layers[2].surface == b.surface,
          "move(2,0) rotates the stack");
    check(ol_scene2d_hit(&s, 12, 8) == 2 && s.layers[ol_scene2d_hit(&s, 12, 8)].surface == b.surface &&
              s.layers[ol_scene2d_hit(&s, 17, 8)].surface == c.surface,
          "hit order is respected after a move");
    check(render(&s, &t, &info) == OL_OK && t.front[(8 * 40 + 12) * 4 + 1] == 255 &&
              t.front[(8 * 40 + 17) * 4 + 2] == 255 && t.front[(8 * 40 + 7) * 4 + 1] == 255,
          "render follows the new z-order");
    check(ol_scene2d_move(&s, 0, 2) == OL_OK && s.layers[2].surface == c.surface &&
              render(&s, &t, &info) == OL_OK && t.front[(8 * 40 + 12) * 4 + 2] == 255,
          "move back restores the original paint order");
    check(ol_scene2d_move(&s, 0, OL_SCENE2D_LAYERS) == OL_ARGUMENT && ol_scene2d_move(&s, -1, 0) == OL_ARGUMENT,
          "move rejects bad indices");
    /* Translation moves the hit area; a hidden or transparent layer is skipped. */
    ol_scene2d_set_xf(&s, 2, 20 * 256, 0, 0, 0);
    check(ol_scene2d_hit(&s, 12, 8) == 1 && ol_scene2d_hit(&s, 32, 8) == 2, "hit follows tx");
    ol_scene2d_set(&s, 1, b.surface, (struct gfx_rect){5, 5, 10, 10}, 0, 1);
    check(ol_scene2d_hit(&s, 7, 8) == 0, "opacity 0 layers are not hit");
    ol_list_reset(list);
    ol_scene2d_release(&s);
    destroy(&a);
    destroy(&b);
    destroy(&c);
    destroy(&t);
}

int main(void)
{
    void *device_storage = malloc(ol_device_size()), *list_storage = malloc(262144);
    if (ol_device_create(device_storage, ol_device_size(), OL_API_VERSION, 0, &device) ||
        ol_list_create(device, list_storage, 262144, &list))
        return 2;
    damage_set();
    subpixel();
    zero_defaults();
    ordering();
    ol_list_destroy(list);
    ol_device_destroy(device);
    free(list_storage);
    free(device_storage);
    printf("scene2d-ext: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
