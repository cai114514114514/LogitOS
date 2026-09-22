/* Backdrop material entry points: prepare bounds and scratch, blur, then draw.
 * glass_blur.c owns sampling; glass_render.c owns coverage and shading.
 * The optical lookup tables remain in openlogit_glass.c. */
#include "glass_internal.h"

static int prepare_panel(struct glass_panel *panel, struct ol_display *display,
                         int x, int y, int width, int height, int radius,
                         unsigned cut)
{
    const struct ol_pixel_target *target = &display->target;
    if (!target->px || width <= 0 || height <= 0 || radius < 0) return 0;
#ifdef GLASS_SQUARE_NOOP
    /* Negative control: the old radius-zero guard hid Browser DevTools. */
    if (radius == 0) return 0;
#endif
#ifdef GLASS_SQUARE_ROUND
    /* Negative control: 1px rounding paints, but does not fill a square. */
    if (radius == 0) radius = 1;
#endif

    int shape_width = width;
    int shape_height = height;
    int origin_x = x;
    int origin_y = y;
    long right = (long)x + width;
    long bottom = (long)y + height;
    if (right <= 0 || bottom <= 0 || x >= target->w || y >= target->h) return 0;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > target->w) right = target->w;
    if (bottom > target->h) bottom = target->h;
    width = (int)(right - x);
    height = (int)(bottom - y);

    /* The old implementation re-centered the lens on the visible rectangle.
     * Dragging it offscreen created a new rim at the screen edge. Crop samples
     * and writes, but keep the original shape. Unavailable backdrop samples
     * still clamp to the screen: there is no offscreen framebuffer to read. */
#ifdef GLASS_CROP_REBASE
    shape_width = width;
    shape_height = height;
    origin_x = x;
    origin_y = y;
#endif
    int short_side = shape_width < shape_height ? shape_width : shape_height;
    if (radius > short_side / 2) radius = short_side / 2;

    *panel = (struct glass_panel){
        .display = display,
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .clip_right = width,
        .clip_bottom = height,
        /* A square panel needs all four ORIGINAL edges even when cropped.
         * Reusing the visible rectangle would draw a new rim at the screen. */
        .shape_left = origin_x - x,
        .shape_top = origin_y - y,
        .shape_right = origin_x - x + shape_width,
        .shape_bottom = origin_y - y + shape_height,
        .center_x = shape_width / 2 - (x - origin_x),
        .center_y = shape_height / 2 - (y - origin_y),
        .inner_x = shape_width / 2 - radius,
        .inner_y = shape_height / 2 - radius,
        .radius = radius,
        .cut = cut,
    };

    if (target->clip_on) {
        panel->clip_left = glass_clamp(target->clx0 - x, 0, width);
        panel->clip_top = glass_clamp(target->cly0 - y, 0, height);
        panel->clip_right = glass_clamp(target->clx1 - x, 0, width);
        panel->clip_bottom = glass_clamp(target->cly1 - y, 0, height);
    }
    if (panel->clip_left >= panel->clip_right ||
        panel->clip_top >= panel->clip_bottom) return 0;

    /* A fixed 22px bevel overwhelmed 1px sidebars and 10px buttons. Bound
     * optical thickness by the corner; the dock keeps its wider lens. */
    int edge_width = radius < 22 ? radius : 22;
    if (edge_width > short_side / 2) edge_width = short_side / 2;
    panel->edge_width = edge_width < 4 ? 4 : edge_width;
    return 1;
}

static int reserve_scratch(struct glass_panel *panel)
{
    struct ol_display *display = panel->display;
    int pixels = panel->width * panel->height;
    int columns = panel->width > panel->height ? panel->width : panel->height;

    if (pixels > display->glass_buf_n) {
        display_free(display, display->glass_buf);
        display->glass_buf = display_alloc(display, (unsigned long)pixels * sizeof(uint32_t));
        display->glass_buf_n = display->glass_buf ? pixels : 0;
    }
    if (columns > display->glass_line_n) {
        display_free(display, display->glass_line);
        display->glass_line = display_alloc(display, (unsigned long)columns * sizeof(int));
        display->glass_line_n = display->glass_line ? columns : 0;
    }
    return display->glass_buf && display->glass_line;
}

void ol_display_liquid_glass_cut(struct ol_display *display,
                                int x, int y, int width, int height, int radius,
                                uint8_t red, uint8_t green, uint8_t blue,
                                uint8_t opacity, unsigned cut)
{
    struct glass_panel panel;
    if (!prepare_panel(&panel, display, x, y, width, height, radius, cut)) return;

    if (!reserve_scratch(&panel)) {
        /* The former frost-only fallback re-rounded the cropped rectangle:
         * at x=100 on a 128px target it painted 227 pixels outside the lens.
         * Without both scratch buffers, leave the backdrop intact and report
         * the allocator's OL_LIMIT instead of drawing a different shape. */
#ifdef GLASS_FALLBACK_BLUR
        ol_display_blur_rect(display, panel.x, panel.y, panel.width, panel.height,
                            GLASS_BLUR_RADIUS, panel.radius);
#endif
        return;
    }

    /* A partial write clip still needs the whole visible backdrop for blur
     * and refraction. Only a wholly disjoint panel may skip this work. */
    glass_blur(&panel);
    glass_render(&panel, (struct glass_color){red, green, blue}, opacity);
}

void ol_display_liquid_glass(struct ol_display *display,
                            int x, int y, int width, int height, int radius,
                            uint8_t red, uint8_t green, uint8_t blue,
                            uint8_t opacity)
{
    ol_display_liquid_glass_cut(display, x, y, width, height, radius,
                               red, green, blue, opacity, 0);
}
