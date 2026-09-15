#include "app.h"
#include <stdlib.h>
#include <string.h>

int bitmap_create(struct vector_app *app, struct bitmap *bitmap, unsigned w, unsigned h)
{
    bitmap->w = w;
    bitmap->h = h;
    bitmap->storage = malloc(ol_surface_size());
    bitmap->front = calloc(w * h, 4);
    bitmap->work = malloc(w * h * 4);
    if (!bitmap->storage || !bitmap->front || !bitmap->work)
        return 0;
    struct ol_surface_desc desc = {
        sizeof desc, OL_FORMAT_RGBA8_STRAIGHT, w, h, w * 4, bitmap->front, bitmap->work, w * h * 4,
        w * h * 4};
    return ol_surface_create(app->device, bitmap->storage, ol_surface_size(), &desc,
                             &bitmap->surface) == OL_OK;
}

void bitmap_destroy(struct bitmap *bitmap)
{
    if (bitmap->surface)
        ol_surface_destroy(bitmap->surface);
    free(bitmap->storage);
    free(bitmap->front);
    free(bitmap->work);
}

static int publish(struct vector_app *app, struct bitmap *bitmap)
{
    return ol_list_close(app->list) == OL_OK &&
           ol_submit(app->device, app->list, bitmap->surface, NULL) == OL_OK;
}

static void rectangle(struct ol_canvas *canvas, int x, int y, int w, int h, int radius,
                      unsigned rgb, int alpha)
{
    struct gfx_paint paint;
    gfx_paint_solid(&paint, rgb, alpha);
    ol_canvas_round_rect(canvas, (struct gfx_rect){x, y, w, h}, radius, &paint);
}

int artwork_init(struct vector_app *app)
{
    struct ol_canvas canvas;
    ol_list_reset(app->list);
    ol_cmd_clear(app->list, 0x101b2a, 255);
    if (!publish(app, &app->background))
        return 0;
    ol_list_reset(app->list);
    ol_canvas_init(&canvas, app->list, app->card.w, app->card.h);
    ol_cmd_clear(app->list, 0, 0);
    rectangle(&canvas, 6, 7, 148, 100, 14, 0x000000, 90);
    rectangle(&canvas, 0, 0, 148, 100, 14, 0x33766b, 255);
    rectangle(&canvas, 16, 16, 72, 8, 4, 0xc8f1dc, 255);
    rectangle(&canvas, 16, 36, 110, 5, 2, 0x83bca9, 255);
    rectangle(&canvas, 16, 52, 92, 5, 2, 0x83bca9, 255);
    rectangle(&canvas, 16, 74, 32, 10, 5, 0xe9c578, 255);
    if (!publish(app, &app->card))
        return 0;
    struct bitmap source = {0};
    if (!bitmap_create(app, &source, 128, 32))
        return 0;
    ol_list_reset(app->list);
    ol_canvas_init(&canvas, app->list, 128, 32);
    ol_cmd_clear(app->list, 0, 0);
    rectangle(&canvas, 2, 2, 28, 28, 7, 0x58cdb6, 255);
    rectangle(&canvas, 8, 8, 16, 16, 4, 0xe0fff1, 255);
    rectangle(&canvas, 34, 2, 28, 28, 14, 0xf0b565, 255);
    rectangle(&canvas, 42, 10, 12, 12, 6, 0x713b41, 255);
    rectangle(&canvas, 66, 2, 28, 28, 3, 0x7d91e8, 255);
    rectangle(&canvas, 71, 7, 18, 18, 8, 0xb7c7ff, 255);
    rectangle(&canvas, 96, 0, 32, 32, 8, 0x4ea18f, 255);
    rectangle(&canvas, 100, 4, 24, 24, 5, 0x213e4e, 255);
    if (!publish(app, &source)) {
        bitmap_destroy(&source);
        return 0;
    }
    app->atlas_pixels = malloc(128 * 32 * 4);
    app->atlas_storage = malloc(ol_image_size());
    app->mask_pixels = calloc(128 * 128, 1);
    app->mask_storage = malloc(ol_image_size());
    if (!app->atlas_pixels || !app->atlas_storage || !app->mask_pixels || !app->mask_storage) {
        bitmap_destroy(&source);
        return 0;
    }
    memcpy(app->atlas_pixels, source.front, 128 * 32 * 4);
    ol_list_reset(app->list);
    bitmap_destroy(&source);
    struct ol_image_desc desc = {sizeof desc, OL_FORMAT_RGBA8_STRAIGHT, 128,         32,
                                 512,         app->atlas_pixels,        128 * 32 * 4};
    if (ol_image_create(app->device, app->atlas_storage, ol_image_size(), &desc, &app->atlas))
        return 0;
    int points[2048], contours[8];
    struct gfx_path path;
    gfx_path_init(&path, points, 1024, contours, 8);
    gfx_path_ellipse(&path, GFX_PX(64), GFX_PX(64), GFX_PX(62), GFX_PX(62));
    if (!ol_raster_mask_subs(&path, GFX_NONZERO, app->mask_pixels, 128, 128, 0, 0, 8))
        return 0;
    desc = (struct ol_image_desc){sizeof desc, OL_FORMAT_A8,     128,      128,
                                  128,         app->mask_pixels, 128 * 128};
    return ol_image_create(app->device, app->mask_storage, ol_image_size(), &desc, &app->mask) ==
           OL_OK;
}

static void vectors(struct vector_app *app, struct ol_canvas *canvas)
{
    struct gfx_paint paint;
    gfx_paint_linear(&paint, GFX_PX(30), 0, GFX_PX(300), GFX_PX(280));
    gfx_paint_stop(&paint, 0, 0x4cdec1, 255);
    gfx_paint_stop(&paint, 32768, 0x5a8dd0, 255);
    gfx_paint_stop(&paint, 65536, 0x926acf, 255);
    ol_canvas_save(canvas);
    struct gfx_matrix transform;
    gfx_m_identity(&transform);
    gfx_m_translate(&transform, GFX_PX(156), GFX_PX(164));
    gfx_m_rotate(&transform, (int)(app->phase * 360 * GFX_ONE));
    ol_canvas_transform(canvas, &transform);
    ol_canvas_round_rect(canvas, (struct gfx_rect){-92, -80, 184, 160}, 24, &paint);
    ol_canvas_restore(canvas);
    int points[4096], contours[32], outline_points[16384], outline_contours[256];
    struct gfx_path path, outline;
    gfx_path_init(&path, points, 2048, contours, 32);
    gfx_path_init(&outline, outline_points, 8192, outline_contours, 256);
    gfx_move_to(&path, GFX_PX(32), GFX_PX(250));
    gfx_cubic_to(&path, GFX_PX(110), GFX_PX(110), GFX_PX(180), GFX_PX(320), GFX_PX(292),
                 GFX_PX(200));
    int dash[] = {GFX_PX(12), GFX_PX(7)};
    struct gfx_stroke stroke = {GFX_PX(2 + (int)(app->amount * 10)),
                                GFX_CAP_ROUND,
                                GFX_JOIN_ROUND,
                                4 * GFX_MONE,
                                app->dashed ? dash : NULL,
                                app->dashed ? 2 : 0,
                                0};
    gfx_paint_solid(&paint, 0xf6d08a, 255);
    ol_canvas_stroke(canvas, &path, &stroke, &paint, &outline);
    ol_canvas_save(canvas);
    if (app->clipped)
        ol_canvas_mask(canvas, app->mask, 360, 100);
    gfx_paint_radial(&paint, GFX_PX(412), GFX_PX(148), GFX_PX(136));
    gfx_paint_stop(&paint, 0, 0xf9ce79, 255);
    gfx_paint_stop(&paint, 65536, 0xc95682, 255);
    ol_canvas_round_rect(canvas, (struct gfx_rect){330, 80, 190, 174}, 8, &paint);
    ol_canvas_restore(canvas);
    rectangle(canvas, 352, 280, 144, 6, 3, 0x587a8e, 255);
}

static void sprites(struct vector_app *app)
{
    struct gfx_rect panel = {28, 32, 360 + (int)(app->amount * 150), 264};
    ol_cmd_nine_slice(app->list, app->atlas, (struct gfx_rect){96, 0, 32, 32}, panel,
                      (struct ol_borders){8, 8, 8, 8}, NULL, 255, 1);
    for (int i = 0; i < 3; i++) {
        struct gfx_matrix transform;
        gfx_m_identity(&transform);
        gfx_m_translate(&transform, GFX_PX(122 + i * 128), GFX_PX(164));
        gfx_m_rotate(&transform, (int)(app->phase * 360 * GFX_ONE) + i * GFX_PX(18));
        gfx_m_scale(&transform, 3 * GFX_MONE, 3 * GFX_MONE);
        gfx_m_translate(&transform, -GFX_PX(16), -GFX_PX(16));
        int tile = (i + app->variant) % 3;
        ol_cmd_sprite(app->list, app->atlas, (struct gfx_rect){tile * 32, 0, 32, 32}, &transform,
                      NULL, 255, 1);
    }
}

int artwork_stage(struct vector_app *app)
{
    ol_list_reset(app->list);
    ol_cmd_clear(app->list, 0x192e40, 255);
    struct ol_canvas canvas;
    ol_canvas_init(&canvas, app->list, 576, 344);
    if (app->page == 0)
        vectors(app, &canvas);
    else if (app->page == 1)
        sprites(app);
    else {
        for (int x = 24; x < 576; x += 32)
            rectangle(&canvas, x, 0, 1, 344, 0, 0x294357, 255);
        for (int y = 24; y < 344; y += 32)
            rectangle(&canvas, 0, y, 576, 1, 0, 0x294357, 255);
        rectangle(&canvas, 84, 104, 172, 124, 16, 0x294d66, 255);
        rectangle(&canvas, 108, 126, 100, 8, 4, 0x688ea5, 255);
        rectangle(&canvas, 108, 148, 76, 8, 4, 0x688ea5, 255);
    }
    return publish(app, &app->stage);
}

void artwork_destroy(struct vector_app *app)
{
    if (app->atlas)
        ol_image_destroy(app->atlas);
    if (app->mask)
        ol_image_destroy(app->mask);
    free(app->atlas_storage);
    free(app->atlas_pixels);
    free(app->mask_storage);
    free(app->mask_pixels);
}
