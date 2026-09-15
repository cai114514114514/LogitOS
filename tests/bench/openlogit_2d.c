/* Same source/object linked twice, once against each SDK archive. Only 1.1
 * entry points predating this batch are used. The guest clock brackets record,
 * render and window submission; screenshot acknowledgements are outside it. */
#include "openlogit_window.h"
#include "openlogit_ui.h"
#include "../../examples/openlogit/example_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct bitmap {
    struct ol_surface *surface;
    void *storage;
    unsigned char *front, *work;
};
static struct ol_device *device;
static struct ol_list *list;
static struct bitmap target, background, tile;

static int create(struct bitmap *bitmap, unsigned w, unsigned h)
{
    bitmap->storage = malloc(ol_surface_size());
    bitmap->front = calloc(w * h, 4);
    bitmap->work = malloc(w * h * 4);
    if (!bitmap->storage || !bitmap->front || !bitmap->work)
        return 0;
    struct ol_surface_desc desc = {
        sizeof desc, OL_FORMAT_RGBA8_STRAIGHT, w, h, w * 4, bitmap->front, bitmap->work, w * h * 4,
        w * h * 4};
    return ol_surface_create(device, bitmap->storage, ol_surface_size(), &desc, &bitmap->surface) ==
           OL_OK;
}

static void rectangle(int x, int y, int w, int h, int radius, unsigned color)
{
    int points[1024], contours[8];
    struct gfx_path path;
    struct gfx_paint paint;
    gfx_path_init(&path, points, 512, contours, 8);
    gfx_path_rrect(&path, GFX_PX(x), GFX_PX(y), GFX_PX(w), GFX_PX(h), GFX_PX(radius));
    gfx_paint_solid(&paint, color, 255);
    ol_cmd_fill(list, &path, GFX_NONZERO, &paint, NULL, 4);
}

static int publish(struct bitmap *bitmap)
{
    return ol_list_close(list) == OL_OK && ol_submit(device, list, bitmap->surface, NULL) == OL_OK;
}

static int initialize(void)
{
    void *device_storage = malloc(ol_device_size()), *list_storage = malloc(524288);
    if (!device_storage || !list_storage ||
        ol_device_create(device_storage, ol_device_size(), OL_API_VERSION, 0, &device) ||
        ol_list_create(device, list_storage, 524288, &list) || !create(&target, 600, 400) ||
        !create(&background, 600, 400) || !create(&tile, 64, 64))
        return 0;
    ol_cmd_clear(list, 0x172c3c, 255);
    for (int y = 0; y < 5; y++)
        for (int x = 0; x < 6; x++)
            rectangle(12 + x * 96, 40 + y * 68, 82, 54, 8, 0x29495d + (x + y) * 0x020301);
    if (!publish(&background))
        return 0;
    ol_list_reset(list);
    ol_cmd_clear(list, 0x73cbb2, 255);
    rectangle(4, 4, 56, 56, 12, 0x376b88);
    rectangle(18, 18, 28, 28, 6, 0xefd395);
    if (!publish(&tile))
        return 0;
    ol_list_reset(list);
    return 1;
}

static unsigned checksum(void)
{
    unsigned hash = 2166136261u;
    for (unsigned i = 0; i < 600 * 400 * 4; i++)
        hash = (hash ^ target.front[i]) * 16777619u;
    return hash;
}

static int draw(unsigned kind, unsigned frame, struct ol_submit_info *info)
{
    ol_list_reset(list);
    if (kind < 2) {
        ol_cmd_clear(list, 0x172c3c, 255);
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 6; x++) {
                int left = 12 + x * 96, top = 56 + y * 78;
                if (kind == 1)
                    ol_cmd_image(list, tile.surface, (struct gfx_rect){left, top, 80, 60}, 220, 1);
                else {
                    int points[1024], contours[8];
                    struct gfx_path path;
                    struct gfx_paint paint;
                    gfx_path_init(&path, points, 512, contours, 8);
                    gfx_path_rrect(&path, GFX_PX(left), GFX_PX(top), GFX_PX(80), GFX_PX(60),
                                   GFX_PX(9));
                    gfx_paint_linear(&paint, GFX_PX(left), GFX_PX(top), GFX_PX(left + 80),
                                     GFX_PX(top + 60));
                    gfx_paint_stop(&paint, 0, 0x67cdb3, 255);
                    gfx_paint_stop(&paint, 65536, 0x506cb8, 255);
                    ol_cmd_fill(list, &path, GFX_NONZERO, &paint, NULL, 4);
                }
            }
        }
    } else {
        int position = 80 + (int)(frame % 12) * 12;
        int previous = frame ? 80 + (int)((frame - 1) % 12) * 12 : position;
        struct gfx_rect damage =
            frame ? ol_damage_move((struct gfx_rect){previous, 160, 64, 64},
                                   (struct gfx_rect){position, 160, 64, 64}, 0, 600, 400)
                  : (struct gfx_rect){0, 0, 600, 400};
        struct ol_layer_options options = {
            .size = sizeof options, .clip = &damage, .opacity = 255, .bilinear = 0};
        ol_cmd_layer(list, background.surface, (struct gfx_rect){0, 0, 600, 400}, &options);
        ol_cmd_layer(list, tile.surface, (struct gfx_rect){position, 160, 64, 64}, &options);
    }
    /* A unique opaque marker lets the host count frames actually observed in
     * the guest display, rather than trusting present calls or serial traces. */
    rectangle(8, 8, 16, 16, 0, ((frame + 1) << 16) | ((kind + 1) * 50 << 8) | 180);
    if (ol_list_close(list) || ol_submit_damage(device, list, target.surface, info))
        return 0;
    if (ol_window_composite(target.surface, 0, 0, 600, 400))
        return 0;
    ol_window_text_run(42, 10, 13, 0, 0xe4f1f6, "OpenLogit 2D / identical workload", 32);
    return ol_window_present() == OL_OK;
}

int main(int argc, char **argv)
{
    const char *tag = argc > 1 ? argv[1] : "unknown";
    unsigned kind = argc > 2 ? (unsigned)atoi(argv[2]) : 0;
    if (kind > 2 || !initialize())
        return 1;
    gui_create("OpenLogit 2D Bench", 600, 400);
    _sys(SYS_GUI_WIN_MIN, (600L << 16) | 400, 0, 0);
    example_log("BENCH READY tag=%s case=%u rss=%lu\n", tag, kind,
                (unsigned long)_sys(SYS_RUSAGE, RUCTL_GET_RSS_FRAMES, 0, 0));
    for (unsigned index = 0; index < 48; index++) {
        int next = 0;
        while (!next) {
            struct logit_event event;
            while (poll_event(&event)) {
                if (event.type == EV_CLOSE)
                    return 0;
                if (event.type == EV_KEY && event.a == 'n')
                    next = 1;
            }
            if (!next)
                wait_idle(0);
        }
        struct ol_submit_info info = {.size = sizeof info};
        uint64_t start = monotonic_ns();
        if (!draw(kind, index, &info)) {
            example_log("BENCH ERROR draw\n");
            return 1;
        }
        uint64_t elapsed = monotonic_ns() - start;
        unsigned hash = checksum();
        example_log(
            "BENCH FRAME tag=%s case=%u n=%u ns=%lu hash=%u copied=%lu damage=%d,%d,%d,%d\n", tag,
            kind, index, (unsigned long)elapsed, hash, (unsigned long)info.copied_bytes,
            info.damage.x, info.damage.y, info.damage.w, info.damage.h);
    }
    example_log("BENCH DONE tag=%s case=%u rss=%lu\n", tag, kind,
                (unsigned long)_sys(SYS_RUSAGE, RUCTL_GET_RSS_FRAMES, 0, 0));
    /* Keep the final frame visible until the harness has observed it. */
    for (;;) {
        struct logit_event event;
        while (poll_event(&event))
            if (event.type == EV_CLOSE || (event.type == EV_KEY && event.a == 27))
                return 0;
        wait_idle(0);
    }
}
