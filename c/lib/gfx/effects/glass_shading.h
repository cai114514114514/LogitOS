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

#ifndef GLASS_STATIC_TINT
/* sRGB decode, 8-bit code -> linear luminance in 0..65535 (WCAG 2.x). Const,
 * so it is shared by every context and core. */
static const unsigned short glass_linear16[256] = {
    0, 20, 40, 60, 80, 99, 119, 139, 159, 179, 199, 219,
    241, 264, 288, 313, 340, 367, 396, 427, 458, 491, 526, 562,
    599, 637, 677, 718, 761, 805, 851, 898, 947, 997, 1048, 1101,
    1156, 1212, 1270, 1330, 1391, 1453, 1517, 1583, 1651, 1720, 1790, 1863,
    1937, 2013, 2090, 2170, 2250, 2333, 2418, 2504, 2592, 2681, 2773, 2866,
    2961, 3058, 3157, 3258, 3360, 3464, 3570, 3678, 3788, 3900, 4014, 4129,
    4247, 4366, 4488, 4611, 4736, 4864, 4993, 5124, 5257, 5392, 5530, 5669,
    5810, 5953, 6099, 6246, 6395, 6547, 6700, 6856, 7014, 7174, 7335, 7500,
    7666, 7834, 8004, 8177, 8352, 8528, 8708, 8889, 9072, 9258, 9445, 9635,
    9828, 10022, 10219, 10417, 10619, 10822, 11028, 11235, 11446, 11658, 11873, 12090,
    12309, 12530, 12754, 12980, 13209, 13440, 13673, 13909, 14146, 14387, 14629, 14874,
    15122, 15371, 15623, 15878, 16135, 16394, 16656, 16920, 17187, 17456, 17727, 18001,
    18277, 18556, 18837, 19121, 19407, 19696, 19987, 20281, 20577, 20876, 21177, 21481,
    21787, 22096, 22407, 22721, 23038, 23357, 23678, 24002, 24329, 24658, 24990, 25325,
    25662, 26001, 26344, 26688, 27036, 27386, 27739, 28094, 28452, 28813, 29176, 29542,
    29911, 30282, 30656, 31033, 31412, 31794, 32179, 32567, 32957, 33350, 33745, 34143,
    34544, 34948, 35355, 35764, 36176, 36591, 37008, 37429, 37852, 38278, 38706, 39138,
    39572, 40009, 40449, 40891, 41337, 41785, 42236, 42690, 43147, 43606, 44069, 44534,
    45002, 45473, 45947, 46423, 46903, 47385, 47871, 48359, 48850, 49344, 49841, 50341,
    50844, 51349, 51858, 52369, 52884, 53401, 53921, 54445, 54971, 55500, 56032, 56567,
    57105, 57646, 58190, 58737, 59287, 59840, 60396, 60955, 61517, 62082, 62650, 63221,
    63795, 64372, 64952, 65535,
};

/* Relative luminance (0..65535) of an 8-bit sRGB colour. */
static inline int glass_luminance16(int red, int green, int blue)
{
    return (int)((13933u * glass_linear16[red] + 46871u * glass_linear16[green] +
                  4731u * glass_linear16[blue]) >> 16);
}

/* OL_GLASS_ADAPT_FG. The panel keeps a WCAG contrast of at least 4.7:1 (4.5
 * plus rounding margin) against fg once the backdrop has been tinted. The
 * table is indexed by the pixel's relative luminance >> 8, so it follows the
 * backdrop's real brightness, not the gamma luma that colour casts distort.
 *
 * A light fg needs a dark panel (Y <= ymax): the tint is darkened until it can
 * reach ymax, and each bucket gets the opacity that brings its brightest
 * member there. lin() is convex, so a per-channel blend toward the tint is at
 * or below the linear mix in Y and the bound holds for coloured backdrops.
 * A dark fg needs a light panel (Y >= ymin) and is solved on the gray with the
 * bucket's luminance; saturated backdrops can fall a little short of it.
 * The environment/rim layers added afterwards are not clamped: the guarantee is
 * for the panel interior. Opacity 255 is an explicit opaque tint and is left. */
static void prepare_adapt_fg(struct glass_style *style, struct glass_color tint,
                             int opacity, uint32_t fg)
{
    int fg_y = glass_luminance16((int)(fg >> 16 & 255), (int)(fg >> 8 & 255),
                                 (int)(fg & 255));
    int dark_panel = fg_y >= 11700;
    int ymax = (fg_y + 3277) * 10 / 47 - 3277;
    int ymin = (fg_y + 3277) * 47 / 10 - 3277;
    if (ymax < 0) ymax = 0;
    if (ymin > 65535) ymin = 65535;
    int tint_y = glass_luminance16(tint.red, tint.green, tint.blue);

    if (dark_panel) {
        for (int step = 0; step < 40 && tint_y > ymax / 2; step++) {
            tint.red = tint.red * 3 / 4;
            tint.green = tint.green * 3 / 4;
            tint.blue = tint.blue * 3 / 4;
            tint_y = glass_luminance16(tint.red, tint.green, tint.blue);
        }
    } else {
        int goal = ymin + (65535 - ymin) / 2;
        for (int step = 0; step < 40 && tint_y < goal; step++) {
            tint.red = 255 - (255 - tint.red) * 3 / 4;
            tint.green = 255 - (255 - tint.green) * 3 / 4;
            tint.blue = 255 - (255 - tint.blue) * 3 / 4;
            tint_y = glass_luminance16(tint.red, tint.green, tint.blue);
        }
    }
    style->tint = tint;
    style->adaptive = 2;

    int goal_gray = 0, back_gray = 0, tint_gray = 0;
    if (!dark_panel) {
        while (goal_gray < 255 && glass_linear16[goal_gray] < ymin) goal_gray++;
        while (tint_gray < 255 && glass_linear16[tint_gray] < tint_y) tint_gray++;
    }
    for (int bucket = 0; bucket < 256; bucket++) {
        int required = opacity;
        if (dark_panel) {
            int top = bucket << 8 | 255;
            if (top > ymax) {
                int minimum = (255 * (top - ymax) + (top - tint_y) - 1) / (top - tint_y);
                if (minimum > required) required = minimum;
            }
        } else {
            int bottom = bucket << 8;
            while (back_gray < 255 && glass_linear16[back_gray + 1] <= bottom) back_gray++;
            if (bottom < ymin) {
                int span = tint_gray - back_gray;
                int minimum = span <= 0 ? 255
                    : ((goal_gray - back_gray) * 255 + span - 1) / span;
                if (minimum > required) required = minimum;
            }
        }
        style->opacity_by_luma[bucket] = (unsigned char)glass_clamp(required, 0, 255);
    }
}

#ifndef GLASS_ADAPT_HARD_THRESHOLD
/* OL_GLASS_ADAPT_SMOOTH: the same two rules as the hard adaptive tint (light
 * tint >= 176 luma, dark tint <= 88), but their strength s (0..256) ramps with
 * the tint's tone (light: 160..224, dark: 96..64), with its distance from
 * neutral (full under 16 levels of spread, none from 48) and with its opacity
 * (full from 32). At s = 256 the table equals the hard one. */
static void prepare_adapt_smooth(struct glass_style *style, int opacity, int tone,
                                 int spread)
{
    int ramp;
    if (tone >= 128) ramp = glass_clamp((tone - 160) * 256 / 64, 0, 256);
    else ramp = glass_clamp((96 - tone) * 256 / 32, 0, 256);
    int neutral = glass_clamp((48 - spread) * 256 / 32, 0, 256);
    int weight = glass_clamp(opacity, 0, 32) * 256 / 32;
    int strength = ramp * neutral / 256 * weight / 256;
    if (strength == 0 || opacity == 255) return;

    style->adaptive = 1;
    for (int luma = 0; luma < 256; luma++) {
        int minimum = 0;
        if (tone >= 128 && luma < 176) {
            int span = tone - luma;
            minimum = span <= 0 ? 255 : ((176 - luma) * 255 + span - 1) / span;
        } else if (tone < 128 && luma > 88) {
            int span = luma - tone;
            minimum = ((luma - 88) * 255 + span - 1) / span;
        }
        int required = opacity;
        if (minimum > opacity)
            required = opacity + (glass_clamp(minimum, 0, 255) - opacity) * strength / 256;
        style->opacity_by_luma[luma] = (unsigned char)required;
    }
}
#endif
#endif

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
    unsigned adapt_flags = panel->mat ? panel->mat->flags : 0;
    if ((adapt_flags & OL_GLASS_ADAPT_FG) && opacity != 255) {
        prepare_adapt_fg(style, tint, opacity, panel->mat->fg_rgb);
        return;
    }
#ifndef GLASS_ADAPT_HARD_THRESHOLD   /* test control: SMOOTH falls back to the hard rule */
    if (adapt_flags & OL_GLASS_ADAPT_SMOOTH) {
        prepare_adapt_smooth(style, opacity, tone, high - low);
        return;
    }
#endif
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
#ifndef GLASS_STATIC_TINT
    if (style->adaptive == 2)
        opacity = style->opacity_by_luma[glass_luminance16(color.red, color.green, color.blue) >> 8];
    else
#endif
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
