#ifndef OPENLOGIT_GLASS_INTERNAL_H
#define OPENLOGIT_GLASS_INTERNAL_H

#include "../adapters/display_pixels.h"
#include "openlogit_glass.h"
#include "openlogit_glass_material.h"

enum { GLASS_BLUR_RADIUS = 6, GLASS_REFLECT_OFFSET = 6,
       GLASS_REFLECT_BAND = 4 };
/* Widest single box pass any blur tier uses. The reciprocal table and the
 * per-column stack rings in glass_blur.c are sized for it. */
enum { GLASS_BLUR_RADIUS_MAX = 20 };
/* SYMMETRIC rim: the ring's peak brightening (of 256, as glass_light's
 * highlight), and the luma variance of the blurred rim band below which the
 * reflection is off and at which it reaches full strength (2 and 20 levels
 * standard deviation). Sampling stops near GLASS_RIM_SAMPLES pixels. */
enum { GLASS_RING_PEAK = 64, GLASS_RIM_VARIANCE_FLOOR = 4,
       GLASS_RIM_VARIANCE_FULL = 400, GLASS_RIM_SAMPLES = 4096 };

/* Shape coordinates remain relative to the visible panel. The sample box is
 * separate: blur and refraction need neighbouring backdrop pixels, while only
 * the panel and its write clip may change. Both boxes are target-clipped. */
struct glass_panel {
    struct ol_display *display;
    int x, y;
    int width, height;
    int sample_x, sample_y;
    int sample_width, sample_height;
    int clip_left, clip_top, clip_right, clip_bottom;
    int shape_left, shape_top, shape_right, shape_bottom;
    int center_x, center_y;
    int inner_x, inner_y;
    int radius;
    int edge_width;
    unsigned cut;
    /* The refraction/Fresnel tables this render reads -- set once by
     * glass_render from glass_lut_get(), never the shared globals, so two
     * cores rendering two panels cannot see each other's tables. */
    const struct glass_lut *lut;
    struct glass_lut lut_own;
    /* NULL = legacy defaults (REGULAR blur, directional rim). Set only by
     * ol_display_liquid_glass_ex; later passes read rim, flags and fg_rgb
     * from here. */
    const struct ol_glass_material *mat;
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
    const unsigned char *fres;    /* panel->lut->fres, for lighting() */
    /* OL_GLASS_RIM_SYMMETRIC only (symmetric == 0 leaves them unread):
     * ring[] is the view-independent rim brightening by depth, reflect_gain
     * (0..256) scales the Fresnel reflection by the variance of the blurred
     * backdrop around the rim, and mean is that backdrop's average colour,
     * the environment the reflection blends toward. */
    int symmetric;
    int reflect_gain;
    struct glass_color mean;
    int ring[GLASS_E_MAX + 1];
};

struct glass_light {
    struct glass_color environment;
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

/* NULL and DIRECTIONAL (and any unknown value) render the directional rim.
 * The negative control forces it for every material. */
static inline int glass_rim_symmetric(const struct ol_glass_material *mat)
{
#ifdef GLASS_RIM_FORCE_DIRECTIONAL
    (void)mat;
    return 0;
#else
    return mat && mat->rim == OL_GLASS_RIM_SYMMETRIC;
#endif
}

void glass_blur(struct glass_panel *panel);
/* Summed pass radii of the material's blur tier (6 for NULL/REGULAR). */
int glass_blur_support(const struct ol_glass_material *mat);
void glass_copy_backdrop(struct glass_panel *panel);
/* Optical edge width of a panel: min(radius, 22, short side / 2), floored at
 * 4px, or at 2px for a SYMMETRIC panel whose short side is under 40px. */
int glass_panel_edge_width(int width, int height, int radius,
                           const struct ol_glass_material *mat);
void glass_render(struct glass_panel *panel, struct glass_color tint, int opacity);
void glass_render_solid(struct glass_panel *panel, struct glass_color tint);

#endif
