#ifndef OPENLOGIT_GLASS_SHADING_H
#define OPENLOGIT_GLASS_SHADING_H

#include "glass_internal.h"

/* These helpers stay beside the raster loop at compilation: out-of-line
 * per-pixel packing previously added seven call sites and a guest regression.
 * Color decisions remain separate from geometry without paying that cost. */
static inline int color_luma(struct glass_color color)
{
    return (54 * color.red + 183 * color.green + 19 * color.blue) >> 8;
}

static void prepare_style(struct glass_style *style, const struct glass_panel *panel,
                          struct glass_color tint, int opacity)
{
    style->fres = panel->lut->fres;
    style->tint = tint;
    style->opacity = opacity;
    style->adaptive = 0;
    int edge_width = panel->edge_width;
    int highlight_width = edge_width * 7 / 10;
    if (highlight_width < 1) highlight_width = 1;
    for (int depth = 0; depth <= GLASS_E_MAX; depth++) {
        if (depth < edge_width) {
            int band = 256 - depth * 256 / highlight_width;
            style->band[depth] = band > 0 ? band : 0;
            style->tilt[depth] = (edge_width - depth) * 256 / edge_width;
        } else {
            style->band[depth] = 0;
            style->tilt[depth] = 0;
        }
    }

    /* SYMMETRIC: a ring lit by edge distance alone. Grazing incidence near
     * the outline reflects more (Fresnel), so the outer two pixels share the
     * peak and the next few fall off quadratically. Two equal outer levels
     * matter: straight runs place depth 0 on the half-covered pixel while a
     * corner can cover a depth-0 pixel fully, and a single-pixel peak would
     * make corners brighter than sides. The width follows the edge so the
     * ring keeps its proportion at 125% and 150%. reflect_gain and mean are
     * measured from the blurred halo by glass_render. */
    style->symmetric = glass_rim_symmetric(panel->mat);
    style->reflect_gain = 0;
    style->mean = (struct glass_color){0, 0, 0};
    if (style->symmetric) {
        int ring_width = glass_clamp(edge_width / 4, 2, 6);
        int falloff = (ring_width - 1) * (ring_width - 1);
        for (int depth = 0; depth <= GLASS_E_MAX; depth++) {
            int span = ring_width - depth;
            if (depth <= 1) style->ring[depth] = GLASS_RING_PEAK;
            else if (span > 0) style->ring[depth] = GLASS_RING_PEAK * span * span / falloff;
            else style->ring[depth] = 0;
        }
    }

#ifndef GLASS_STATIC_TINT
    int low = tint.red < tint.green ? tint.red : tint.green;
    int high = tint.red > tint.green ? tint.red : tint.green;
    if (tint.blue < low) low = tint.blue;
    if (tint.blue > high) high = tint.blue;
    int tone = color_luma(tint);
    if (opacity == 0 || opacity == 255 || high - low > 32 ||
        (tone > 64 && tone < 224)) return;

    /* WWDC25 describes adaptive tint and dynamic range. These are our SDR
     * bounds, not Apple's shader: light >=176, dark <=88 before reflection.
     * The API lacks a foreground color, so retain the caller's polarity and
     * leave colored, clear and opaque tints alone. Blurred luminance provides
     * local adaptation; this table removes a variable divide per pixel. */
    style->adaptive = 1;
    for (int luma = 0; luma < 256; luma++) {
        int required = opacity;
        if (tone >= 224 && luma < 176) {
            int span = tone - luma;
            int minimum = ((176 - luma) * 255 + span - 1) / span;
            if (minimum > required) required = minimum;
        } else if (tone <= 64 && luma > 88) {
            int span = luma - tone;
            int minimum = ((luma - 88) * 255 + span - 1) / span;
            if (minimum > required) required = minimum;
        }
        style->opacity_by_luma[luma] = (unsigned char)required;
    }
#endif
}

static inline struct glass_light lighting(const struct glass_style *style,
                                          struct glass_field field)
{
    int depth = glass_clamp(field.depth, 0, GLASS_E_MAX);
    if (style->symmetric) {
        /* No facing/opposite split and no shadow: the normal only steers
         * refraction and the reflected sample. Over a flat backdrop the gain
         * is zero, so the rim is the tinted backdrop plus the ring. */
        return (struct glass_light){
            .environment = style->mean,
            .reflection = style->fres[depth] * style->reflect_gain / 256,
            .highlight = style->ring[depth],
            .shadow = 0,
        };
    }
    int band = style->band[depth];
    int facing = (-154 * field.normal_x - 205 * field.normal_y) / 256;
    facing = glass_clamp(facing, 0, 256);
    int opposite = (154 * field.normal_x + 205 * field.normal_y) / 256;
    if (opposite < 0) opposite = 0;

    /* A flat interior has no meaningful edge normal. Its tilt must be zero,
     * otherwise the horizontal/vertical normal choice draws a diagonal seam:
     * the old version exposed about three luminance levels on flat white. */
    int environment = 176 + (facing - 128) * style->tilt[depth] / 256;
    int highlight = facing * band / 256;
    highlight = highlight * highlight / 256;
    int shadow = opposite * band / 256;
    shadow = shadow * shadow / 256;

    environment = glass_clamp(environment, 96, 250);
    return (struct glass_light){
        .environment = {environment, environment, environment},
        .reflection = style->fres[depth],
        .highlight = highlight * 64 / 256,
        .shadow = shadow * 46 / 256,
    };
}

static inline int shade_channel(int value, int tint, int opacity,
                                int environment, struct glass_light light)
{
    /* Each layer blends against the previous result. Reordering these integer
     * operations changes both the material and its rounding. */
    value += (tint - value) * opacity / 255;
    value += (environment - value) * light.reflection / 255;
    value += (255 - value) * light.highlight / 256;
    value -= value * light.shadow / 256;
    return glass_clamp(value, 0, 255);
}

static inline struct glass_color shade_color(struct glass_color color,
                                             const struct glass_style *style,
                                             struct glass_light light)
{
    int opacity = style->opacity;
    if (style->adaptive) opacity = style->opacity_by_luma[color_luma(color)];
    return (struct glass_color){
        shade_channel(color.red, style->tint.red, opacity, light.environment.red, light),
        shade_channel(color.green, style->tint.green, opacity, light.environment.green, light),
        shade_channel(color.blue, style->tint.blue, opacity, light.environment.blue, light),
    };
}

static inline void store_pixel(struct ol_display *display, uint32_t *destination,
                               struct glass_color color, int coverage)
{
    if (coverage < 255) {
        struct glass_color backdrop;
        unpack(display, *destination, &backdrop.red, &backdrop.green, &backdrop.blue);
        color.red = (color.red * coverage + backdrop.red * (255 - coverage)) / 255;
        color.green = (color.green * coverage + backdrop.green * (255 - coverage)) / 255;
        color.blue = (color.blue * coverage + backdrop.blue * (255 - coverage)) / 255;
    }
    *destination = display_rgb(display, color.red, color.green, color.blue);
}

#endif
