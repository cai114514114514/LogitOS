#ifndef OPENLOGIT_GLASS_INTERNAL_H
#define OPENLOGIT_GLASS_INTERNAL_H

#include "../adapters/display_pixels.h"
#include "openlogit_glass.h"

enum { GLASS_BLUR_RADIUS = 6 };

/* Samples use the visible rectangle; geometry keeps the original center.
 * Clip bounds are half-open local indices, even for an offscreen lens. */
struct glass_panel {
    struct ol_display *display;
    int x, y;
    int width, height;
    int clip_left, clip_top, clip_right, clip_bottom;
    int shape_left, shape_top, shape_right, shape_bottom;
    int center_x, center_y;
    int inner_x, inner_y;
    int radius;
    int edge_width;
    unsigned cut;
};

struct glass_color {
    int red, green, blue;
};

struct glass_field {
    int coverage;
    int depth;
    int normal_x, normal_y; /* Unit normal in 8.8 fixed point. */
};

struct glass_style {
    struct glass_color tint;
    int opacity;
    int adaptive;
    unsigned char opacity_by_luma[256];
    int band[GLASS_E_MAX + 1];
    int tilt[GLASS_E_MAX + 1];
};

struct glass_light {
    int environment;
    int reflection;
    int highlight;
    int shadow;
};

struct glass_span {
    int start, end;
};

static inline int glass_clamp(int value, int low, int high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

void glass_blur(struct glass_panel *panel);
void glass_render(struct glass_panel *panel, struct glass_color tint, int opacity);

#endif
