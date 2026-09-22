/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "openlogit_canvas.h"
#include "openlogit_window.h"

/* Two cached GFX surfaces, 480x176 each (a 240x88 point preview at 2x).
 * Rebuild only on accent changes. Hover/slider animation reuses their pixels;
 * no private rasterizer and no image decoding is added to the app. */
static unsigned long preview_device_mem[32768], preview_list_mem[32768];
static struct ol_device *preview_device;
static struct ol_list *preview_list;
static struct {
    unsigned long storage[32];
    unsigned char pixels[480 * 176 * 4], work[480 * 176 * 4];
    struct ol_surface *surface;
    unsigned accent;
    int ready;
} previews[2];

static int preview_draw(int x, int y, int w, int dark)
{
    unsigned accent = preview_accent();
    if (!preview_device) {
        if (ol_device_create(preview_device_mem, sizeof preview_device_mem, OL_API_VERSION,
                             OL_CAP_PATH_FILL | OL_CAP_ATOMIC_FRAME, &preview_device) ||
            ol_list_create(preview_device, preview_list_mem, sizeof preview_list_mem, &preview_list))
            return -1;
    }
    if (!previews[dark].surface) {
        struct ol_surface_desc desc = {sizeof desc, OL_FORMAT_RGBA8_STRAIGHT, 480, 176, 480 * 4,
            previews[dark].pixels, previews[dark].work, sizeof previews[dark].pixels, sizeof previews[dark].work};
        if (ol_surface_create(preview_device, previews[dark].storage, sizeof previews[dark].storage,
                              &desc, &previews[dark].surface)) return -1;
    }
    if (!previews[dark].ready || previews[dark].accent != accent) {
        struct ol_canvas canvas;
        struct gfx_paint paint;
        ol_list_reset(preview_list);
        ol_canvas_init(&canvas, preview_list, 480, 176);
        ol_cmd_clear(preview_list, dark ? 0x152239 : 0xDBE8FA, 255);
        gfx_paint_linear(&paint, 0, 0, GFX_PX(480), GFX_PX(176));
        gfx_paint_stop(&paint, 0, dark ? 0x24385A : 0xB8D3FA, 255);
        gfx_paint_stop(&paint, GFX_ONE, accent, 255);
        int points[256], contours[8];
        struct gfx_path path;
        gfx_path_init(&path, points, 128, contours, 8);
        gfx_move_to(&path, 0, GFX_PX(120));
        gfx_cubic_to(&path, GFX_PX(160), GFX_PX(-50), GFX_PX(300), GFX_PX(220), GFX_PX(480), GFX_PX(30));
        gfx_line_to(&path, GFX_PX(480), GFX_PX(176));
        gfx_line_to(&path, 0, GFX_PX(176));
        gfx_close(&path);
        ol_canvas_fill(&canvas, &path, GFX_NONZERO, &paint);
        gfx_paint_solid(&paint, dark ? 0x202B3B : 0xFAFCFF, 255);
        ol_canvas_round_rect(&canvas, (struct gfx_rect){126, 24, 240, 132}, 12, &paint);
        gfx_paint_solid(&paint, dark ? 0x2F3D50 : 0xE5EBF3, 255);
        ol_canvas_round_rect(&canvas, (struct gfx_rect){134, 32, 58, 116}, 6, &paint);
        gfx_paint_solid(&paint, accent, 255);
        ol_canvas_round_rect(&canvas, (struct gfx_rect){140, 54, 46, 12}, 4, &paint);
        ol_canvas_round_rect(&canvas, (struct gfx_rect){308, 124, 46, 18}, 6, &paint);
        gfx_paint_solid(&paint, dark ? 0x586880 : 0xC3CCDA, 255);
        for (int row = 0; row < 3; row++)
            ol_canvas_round_rect(&canvas, (struct gfx_rect){206, 50 + row * 23, 132, 8}, 4, &paint);
        if (ol_list_close(preview_list) || ol_submit(preview_device, preview_list, previews[dark].surface, 0)) return -1;
        previews[dark].ready = 1;
        previews[dark].accent = accent;
    }
    return ol_window_composite(previews[dark].surface, x, y, w, 88);
}
