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
    struct image result = {0};
    result.storage = malloc(ol_surface_size());
    result.front = calloc(width * height, 4);
    result.work = malloc(width * height * 4);
    struct ol_surface_desc desc = {sizeof desc,
                                   OL_FORMAT_RGBA8_STRAIGHT,
                                   width,
                                   height,
                                   width * 4,
                                   result.front,
                                   result.work,
                                   width * height * 4,
                                   width * height * 4};
    if (ol_surface_create(device, result.storage, ol_surface_size(), &desc, &result.surface))
        abort();
    return result;
}
static void destroy(struct image *image)
{
    ol_surface_destroy(image->surface);
    free(image->storage);
    free(image->front);
    free(image->work);
}
static int submit(struct image *image)
{
    return ol_list_close(list) == OL_OK && ol_submit(device, list, image->surface, NULL) == OL_OK;
}
static void canvas(void)
{
    struct image target = image(64, 48);
    struct ol_canvas canvas;
    struct gfx_paint paint;
    ol_list_reset(list);
    ol_canvas_init(&canvas, list, 64, 48);
    ol_cmd_clear(list, 0, 0);
    ol_canvas_save(&canvas);
    struct gfx_matrix transform = {GFX_MONE, 0, 0, GFX_MONE, GFX_PX(12), GFX_PX(5)};
    ol_canvas_transform(&canvas, &transform);
    ol_canvas_clip(&canvas, (struct gfx_rect){15, 8, 10, 7});
    ol_canvas_opacity(&canvas, 128);
    gfx_paint_solid(&paint, 0xff0000, 255);
    ol_canvas_round_rect(&canvas, (struct gfx_rect){0, 0, 30, 30}, 0, &paint);
    ol_canvas_restore(&canvas);
    gfx_paint_solid(&paint, 0x00ff00, 255);
    ol_canvas_round_rect(&canvas, (struct gfx_rect){1, 1, 3, 3}, 0, &paint);
    check(submit(&target), "canvas frame submits");
    check(target.front[(10 * 64 + 16) * 4] == 255 && target.front[(10 * 64 + 16) * 4 + 3] == 128 &&
              target.front[(10 * 64 + 14) * 4 + 3] == 0,
          "saved transform opacity and device clip produce analytic pixels");
    check(target.front[(2 * 64 + 2) * 4 + 1] == 255 && target.front[(2 * 64 + 2) * 4 + 3] == 255,
          "restore returns transform opacity and clip to previous state");
    ol_list_reset(list);
    ol_cmd_clear_rect(list, (struct gfx_rect){15, 8, 10, 7}, 0, 0);
    check(submit(&target) && target.front[(10 * 64 + 16) * 4 + 3] == 0 &&
              target.front[(2 * 64 + 2) * 4 + 1] == 255,
          "transparent clear replaces only requested rectangle");
    ol_list_reset(list);
    ol_canvas_init(&canvas, list, 64, 48);
    ol_cmd_clear(list, 0xabcdef, 255);
    check(ol_canvas_restore(&canvas) == OL_STATE && ol_list_close(list) == OL_STATE &&
              target.front[(2 * 64 + 2) * 4 + 1] == 255,
          "unbalanced restore poisons entire pending frame");
    ol_list_reset(list);
    destroy(&target);
}

static void padded_copy(void)
{
    /* Odd stride alternates aligned and unaligned rows. Sentinels independently
     * check both the word-copy fast path and its byte fallback/tail. */
    unsigned char front[3 * 21], work[3 * 21];
    memset(front, 0xa5, sizeof front);
    memset(work, 0x5a, sizeof work);
    void *storage = malloc(ol_surface_size());
    struct ol_surface *surface;
    struct ol_surface_desc desc = {
        sizeof desc, OL_FORMAT_RGBA8_STRAIGHT, 5, 3, 21, front, work, sizeof front, sizeof work};
    if (ol_surface_create(device, storage, ol_surface_size(), &desc, &surface))
        abort();
    ol_list_reset(list);
    ol_cmd_clear(list, 0x123456, 255);
    ol_list_close(list);
    struct ol_submit_info info = {.size = sizeof info};
    int result = ol_submit_damage(device, list, surface, &info);
    int pixels = 1, padding = 1;
    for (int y = 0; y < 3; y++) {
        for (int x = 0; x < 5; x++) {
            unsigned char *p = front + y * 21 + x * 4;
            pixels &= p[0] == 0x12 && p[1] == 0x34 && p[2] == 0x56 && p[3] == 255;
        }
        padding &= front[y * 21 + 20] == 0xa5 && work[y * 21 + 20] == 0x5a;
    }
    check(result == OL_OK && pixels && padding,
          "word copy preserves odd-stride pixels and row padding");
    check(info.copied_bytes == 5 * 3 * 4, "leading clear skips redundant front-to-work copy");
    ol_list_reset(list);
    ol_cmd_clear_rect(list, (struct gfx_rect){1, 1, 3, 1}, 0, 0);
    ol_list_close(list);
    result = ol_submit_damage(device, list, surface, &info);
    check(result == OL_OK && info.copied_bytes == 3 * 4 * 2 && front[21 + 4 + 3] == 0 &&
              front[21 + 3] == 255 && front[21 + 20] == 0xa5,
          "partial clear retains transactional initialization and untouched pixels");
    ol_list_reset(list);
    ol_surface_destroy(surface);
    free(storage);
}

static void sprites(void)
{
    unsigned char pixels[8 * 4 * 4];
    for (unsigned y = 0; y < 4; y++)
        for (unsigned x = 0; x < 8; x++) {
            unsigned char *pixel = pixels + (y * 8 + x) * 4;
            pixel[0] = x < 4 ? 255 : 0;
            pixel[1] = x < 4 ? 0 : 255;
            pixel[2] = 0;
            pixel[3] = 255;
        }
    void *storage = malloc(ol_image_size());
    struct ol_image *atlas;
    struct ol_image_desc desc = {sizeof desc,  OL_FORMAT_RGBA8_STRAIGHT, 8, 4, 32, pixels,
                                 sizeof pixels};
    ol_image_create(device, storage, ol_image_size(), &desc, &atlas);
    struct image target = image(64, 48);
    ol_list_reset(list);
    ol_cmd_clear(list, 0, 0);
    ol_cmd_image_region(list, atlas, (struct gfx_rect){0, 0, 4, 4}, (struct gfx_rect){4, 4, 31, 19},
                        NULL, 255, 1);
    check(submit(&target) && target.front[(12 * 64 + 34) * 4] == 255 &&
              target.front[(12 * 64 + 34) * 4 + 1] == 0,
          "bilinear crop clamps atlas edge without neighbor bleeding");
    ol_list_reset(list);
    ol_cmd_clear(list, 0, 0);
    struct gfx_matrix flip = {-GFX_MONE, 0, 0, GFX_MONE, GFX_PX(12), GFX_PX(4)};
    ol_cmd_sprite(list, atlas, (struct gfx_rect){0, 0, 8, 4}, &flip, NULL, 255, 0);
    check(submit(&target) && target.front[(5 * 64 + 5) * 4 + 1] == 255 &&
              target.front[(5 * 64 + 11) * 4] == 255,
          "negative affine scale reflects cropped image");
    ol_list_reset(list);
    ol_cmd_clear(list, 0, 0);
    ol_cmd_nine_slice(list, atlas, (struct gfx_rect){0, 0, 8, 4}, (struct gfx_rect){2, 2, 39, 29},
                      (struct ol_borders){2, 1, 2, 1}, NULL, 255, 1);
    int status = submit(&target), opaque = 1;
    for (int y = 2; y < 31; y++)
        for (int x = 2; x < 41; x++)
            if (target.front[(y * 64 + x) * 4 + 3] != 255) {
                if (opaque)
                    fprintf(stderr, "nine-slice first nonopaque pixel %d,%d alpha=%u submit=%d\n",
                            x, y, target.front[(y * 64 + x) * 4 + 3], status);
                opaque = 0;
            }
    check(status && opaque, "nine-slice shares exact coverage edges without transparent seams");
    check(target.front[(2 * 64 + 2) * 4] == 255 && target.front[(2 * 64 + 40) * 4 + 1] == 255,
          "nine-slice preserves independently colored corner texels");
    ol_list_reset(list);
    ol_cmd_image_region(list, atlas, (struct gfx_rect){0, 0, 8, 4}, (struct gfx_rect){0, 0, 8, 4},
                        NULL, 255, 0);
    ol_list_close(list);
    unsigned char replacement[4] = {0, 0, 255, 255};
    ol_image_update(atlas, (struct gfx_rect){0, 0, 1, 1}, replacement, 4, 4);
    check(ol_submit(device, list, target.surface, NULL) == OL_STALE_RESOURCE,
          "atlas update invalidates recorded sprite generation");
    check(ol_image_destroy(atlas) == OL_BUSY, "sprite commands retain atlas until list reset");
    ol_list_reset(list);
    ol_image_destroy(atlas);
    free(storage);
    destroy(&target);
}

static void layers(void)
{
    struct image target = image(80, 60), red = image(12, 12), blue = image(12, 12);
    ol_list_reset(list);
    ol_cmd_clear(list, 0xff0000, 255);
    submit(&red);
    ol_list_reset(list);
    ol_cmd_clear(list, 0x0000ff, 255);
    submit(&blue);
    struct ol_scene2d scene;
    ol_scene2d_init(&scene, device, 80, 60);
    ol_scene2d_set(&scene, 0, red.surface, (struct gfx_rect){2, 3, 12, 12}, 255, 1);
    ol_scene2d_set(&scene, 1, blue.surface, (struct gfx_rect){30, 3, 12, 12}, 128, 1);
    struct ol_submit_info info = {.size = sizeof info};
    check(ol_scene2d_render(&scene, list, target.surface, NULL, &info) == OL_OK,
          "retained scene publishes initial layers");
    ol_scene2d_set(&scene, 0, red.surface, (struct gfx_rect){28, 3, 12, 12}, 255, 1);
    check(ol_scene2d_render(&scene, list, target.surface, NULL, &info) == OL_OK &&
              target.front[(4 * 80 + 3) * 4 + 3] == 0,
          "moving layer clears its previous position");
    unsigned char *mixed = target.front + (4 * 80 + 31) * 4;
    check(mixed[0] >= 126 && mixed[0] <= 128 && mixed[2] >= 127 && mixed[2] <= 129 &&
              mixed[3] == 255,
          "damage recomposes overlapping transparent layers in order");
    check(info.damage.x == 2 && info.damage.w == 38 && info.copied_bytes < 80 * 60 * 8,
          "moving layer copies bounded old/new damage instead of whole surface");
    uint64_t completed = ol_device_completed(device);
    check(ol_scene2d_render(&scene, list, target.surface, NULL, &info) == OL_OK && !info.commands &&
              ol_device_completed(device) == completed,
          "unchanged retained scene issues no submission");
    ol_scene2d_set(&scene, 0, red.surface, (struct gfx_rect){28, 3, 12, 12}, 255, 0);
    check(ol_scene2d_render(&scene, list, target.surface, NULL, &info) == OL_OK &&
              target.front[(4 * 80 + 29) * 4 + 3] == 0 &&
              target.front[(4 * 80 + 31) * 4 + 3] == 128,
          "hiding a layer restores transparent background and remaining alpha");
    ol_list_reset(list);
    ol_cmd_clear(list, 0x00ff00, 255);
    submit(&blue);
    check(ol_scene2d_render(&scene, list, target.surface, NULL, &info) == OL_OK &&
              target.front[(4 * 80 + 31) * 4 + 1] == 255,
          "surface generation change invalidates cached layer");
    /* One command fits, so clear recording succeeds and layer recording fails.
     * Retry must compare against the last published placement, not this attempt. */
    void *tiny_storage = malloc(ol_list_min_size());
    struct ol_list *tiny;
    if (ol_list_create(device, tiny_storage, ol_list_min_size(), &tiny))
        abort();
    unsigned char previous[80 * 60 * 4];
    memcpy(previous, target.front, sizeof previous);
    ol_scene2d_set(&scene, 1, blue.surface, (struct gfx_rect){50, 3, 12, 12}, 128, 1);
    check(ol_scene2d_render(&scene, tiny, target.surface, NULL, &info) == OL_LIMIT &&
              !memcmp(previous, target.front, sizeof previous),
          "late scene recording exhaustion preserves the published frame");
    check(ol_scene2d_render(&scene, list, target.surface, NULL, &info) == OL_OK &&
              target.front[(4 * 80 + 31) * 4 + 3] == 0 &&
              target.front[(4 * 80 + 51) * 4 + 1] == 255,
          "scene retry erases the last successful placement");
    ol_list_destroy(tiny);
    free(tiny_storage);
    ol_list_reset(list);
    destroy(&red);
    destroy(&blue);
    destroy(&target);
}

int main(void)
{
    void *device_storage = malloc(ol_device_size()), *list_storage = malloc(262144);
    if (ol_device_create(device_storage, ol_device_size(), OL_API_VERSION, 0, &device) ||
        ol_list_create(device, list_storage, 262144, &list))
        return 2;
    canvas();
    padded_copy();
    sprites();
    layers();
    ol_list_destroy(list);
    ol_device_destroy(device);
    free(list_storage);
    free(device_storage);
    printf("canvas/scene2d: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
