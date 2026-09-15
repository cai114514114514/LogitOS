#include "view.h"
#include "geometry.h"

/* Cache static dial art, then restore it and draw all hands in ONE atomic
 * list. An earlier version staged hands separately and composed that target:
 * it paid a second full-face submission without gaining reusable content. */
#define MAX 384
struct target {
    unsigned long storage[32];
    unsigned char front[MAX * MAX * 4], work[MAX * MAX * 4];
    struct ol_surface *surface;
};
static struct target background, composed;
static unsigned long device_storage[32768], list_storage[32768];
static struct ol_device *device;
static struct ol_list *list;
static int width, height, invalid = 1, last_second = -999999, last_minute = -1, last_hour = -1;
static int last_show = -1;
static unsigned palette[3];
static int points[CLOCK_PATH_CAP * 2], contours[64];
static const short sine[16] = {0,   107, 213, 316, 417, 512,  602,  685,
                               761, 828, 887, 936, 974, 1002, 1018, 1024};

static int sin_step(int step)
{
    step = (step % 60 + 60) % 60;
    if (step <= 15)
        return sine[step];
    if (step <= 30)
        return sine[30 - step];
    if (step <= 45)
        return -sine[step - 30];
    return -sine[60 - step];
}
static int sin_fraction(int angle)
{
    angle = (angle % 61440 + 61440) % 61440;
    int step = angle / 1024, fraction = angle % 1024;
    return sin_step(step) + (sin_step(step + 1) - sin_step(step)) * fraction / 1024;
}
static void needle(struct gfx_path *path, int angle, int tip, int tail, int thickness)
{
    int sx = sin_fraction(angle), sy = sin_fraction(angle + 15360);
    int cx = GFX_PX(width / 2), cy = GFX_PX(height / 2);
    int hx = sy * thickness / 8, hy = sx * thickness / 8;
    int tx = cx + sx * tip / 4, ty = cy - sy * tip / 4;
    int bx = cx - sx * tail / 4, by = cy + sy * tail / 4;
    gfx_move_to(path, bx + hx, by + hy);
    gfx_line_to(path, tx + hx, ty + hy);
    gfx_line_to(path, tx - hx, ty - hy);
    gfx_line_to(path, bx - hx, by - hy);
    gfx_close(path);
}
static int fill(struct gfx_path *path, unsigned color)
{
    struct ol_canvas canvas;
    struct gfx_paint paint;
    ol_canvas_init(&canvas, list, width, height);
    gfx_paint_solid(&paint, color, 255);
    return ol_canvas_fill(&canvas, path, GFX_NONZERO, &paint);
}
static int publish(struct target *target)
{
    int status = ol_list_close(list);
    return status ? status : ol_submit(device, list, target->surface, 0);
}
static int resize_target(struct target *target)
{
    if (target->surface) {
        int status = ol_surface_destroy(target->surface);
        if (status)
            return status;
        target->surface = 0;
    }
    struct ol_surface_desc desc = {sizeof desc,
                                   OL_FORMAT_RGBA8_STRAIGHT,
                                   width,
                                   height,
                                   width * 4,
                                   target->front,
                                   target->work,
                                   sizeof target->front,
                                   sizeof target->work};
    return ol_surface_create(device, target->storage, sizeof target->storage, &desc,
                             &target->surface);
}
int clock_dial_init(void)
{
    int status = ol_device_create(device_storage, sizeof device_storage, OL_API_VERSION,
                                  OL_CAP_PATH_FILL | OL_CAP_ATOMIC_FRAME, &device);
    return status ? status : ol_list_create(device, list_storage, sizeof list_storage, &list);
}
void clock_dial_invalidate(void)
{
    invalid = 1;
}

static int cache_background(void)
{
    struct gfx_path path;
    gfx_path_init(&path, points, CLOCK_PATH_CAP, contours, 64);
    ol_list_reset(list);
    ol_cmd_clear(list, AUI_SURFACE, 255);
    int radius = (width < height ? width : height) / 2;
    clock_face_path(&path, width / 2, height / 2, radius, 0);
    struct gfx_paint paint;
    gfx_paint_linear(&paint, 0, 0, GFX_PX(width), GFX_PX(height));
    gfx_paint_stop(&paint, 0, AUI_SURFACE_2, 255);
    gfx_paint_stop(&paint, GFX_ONE, AUI_FACE, 255);
    ol_cmd_fill(list, &path, GFX_NONZERO, &paint, 0, GFX_SUBS);
    for (int major = 0; major < 2; major++) {
        gfx_path_reset(&path);
        for (int step = 0; step < 60; step++) {
            if ((step % 5 == 0) != major)
                continue;
            needle(&path, step * 1024, radius - 9, -(radius - (major ? 19 : 13)), major ? 2 : 1);
        }
        fill(&path, major ? AUI_TEXT : AUI_BORDER);
    }
    int status = publish(&background);
    if (!status) {
        palette[0] = AUI_SURFACE;
        palette[1] = AUI_TEXT;
        palette[2] = AUI_ACCENT;
        invalid = 0;
    }
    return status;
}

int clock_dial_draw(struct clock_layout layout, const struct logit_time *time, int second,
                    int show_seconds)
{
    if (!layout.face_size)
        return OL_OK;
    int w = aui_dev(layout.face_x + layout.face_size) - aui_dev(layout.face_x);
    int h = aui_dev(layout.face_y + layout.face_size) - aui_dev(layout.face_y);
    if (w <= 0 || h <= 0 || w > MAX || h > MAX)
        return OL_LIMIT;
    int changed_size = w != width || h != height;
    if (changed_size) {
        ol_list_reset(list); /* Release retained layers before replacing targets. */
        width = w;
        height = h;
        int status = resize_target(&background);
        if (!status)
            status = resize_target(&composed);
        if (status)
            return status;
        invalid = 1;
    }
    int redraw =
        invalid || palette[0] != AUI_SURFACE || palette[1] != AUI_TEXT || palette[2] != AUI_ACCENT;
    if (redraw) {
        int status = cache_background();
        if (status)
            return status;
    }
    if (redraw || changed_size || time->hour != last_hour || time->minute != last_minute ||
        show_seconds != last_show || (show_seconds && second != last_second)) {
        ol_list_reset(list);
        struct ol_layer_options options = {.size = sizeof options, .opacity = 255};
        ol_cmd_layer(list, background.surface, (struct gfx_rect){0, 0, w, h}, &options);
        struct gfx_path path;
        gfx_path_init(&path, points, CLOCK_PATH_CAP, contours, 64);
        int radius = (w < h ? w : h) / 2;
        needle(&path, ((time->hour % 12) * 5 * 1024 + time->minute * 1024 / 12), radius * 52 / 100,
               radius / 9, radius / 16);
        needle(&path, time->minute * 1024, radius * 74 / 100, radius / 9, radius / 24);
        fill(&path, AUI_TEXT);
        gfx_path_reset(&path);
        if (show_seconds)
            needle(&path, second, radius * 82 / 100, radius / 5, 2);
        gfx_path_circle(&path, GFX_PX(w / 2), GFX_PX(h / 2), GFX_PX(4));
        fill(&path, AUI_ACCENT);
        int status = publish(&composed);
        if (status)
            return status;
        last_hour = time->hour;
        last_minute = time->minute;
        last_second = second;
        last_show = show_seconds;
    }
    return ol_window_composite(composed.surface, layout.face_x, layout.face_y, layout.face_size,
                               layout.face_size);
}
