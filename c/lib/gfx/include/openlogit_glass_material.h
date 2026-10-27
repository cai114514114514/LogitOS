/* Glass material description and blur tiers for the Liquid Glass panel.
 *
 * REGULAR (blur = 0, the zeroed default) is the long-standing single radius-6
 * box blur and renders bit-identically to ol_display_liquid_glass[_cut]. The
 * other tiers run three box passes per axis, which approximates a gaussian of
 * sigma ~3 (THIN), ~12 (THICK) and ~20 (ULTRA) pixels. A tier's blur reaches
 * further, so its sampling halo is wider: use ol_display_glass_sample_halo_ex
 * (not ol_display_glass_sample_halo) to size damage for a non-REGULAR panel.
 *
 * Reduce Transparency (ol_display.reduce_transparency) draws the solid panel
 * for every tier. */
#ifndef OPENLOGIT_GLASS_MATERIAL_H
#define OPENLOGIT_GLASS_MATERIAL_H

#include <stdint.h>

struct ol_display;

enum {
    OL_GLASS_BLUR_REGULAR = 0,
    OL_GLASS_BLUR_THIN    = 1,
    OL_GLASS_BLUR_THICK   = 2,
    OL_GLASS_BLUR_ULTRA   = 3,
};

enum {
    OL_GLASS_RIM_DIRECTIONAL = 0,
    /* View-independent ring: edge-distance brightening with no dark side,
     * reflection damped by backdrop variance; for small controls. */
    OL_GLASS_RIM_SYMMETRIC   = 1,
};

/* ol_glass_material.flags. 0 renders the long-standing adaptive tint
 * (hard thresholds on the tint's tone, spread and opacity).
 * SMOOTH: the adaptive strength ramps continuously with the tint's tone and
 * distance from neutral instead of switching on thresholds.
 * FG: fg_rgb (0xRRGGBB, high byte ignored; 0 is black text) is the colour
 * drawn over the panel. The tinted backdrop is pushed (tint darkened or
 * lightened as needed, opacity raised per backdrop luminance) until fg has a
 * WCAG contrast of at least 4.5:1 against the panel interior; an opaque
 * (255) tint is left alone. FG takes precedence over SMOOTH. */
enum {
    OL_GLASS_ADAPT_SMOOTH = 1,
    OL_GLASS_ADAPT_FG     = 2,
};

/* tint is 0xRRGGBB (high byte ignored). flags and fg_rgb are described above;
 * tone is reserved. A zeroed struct is REGULAR blur with a directional rim. */
struct ol_glass_material {
    uint32_t tint;
    uint8_t opacity, tone, blur, rim, flags, reserved;
    uint32_t fg_rgb;
};

/* mat == NULL is a zeroed material (black, opacity 0, REGULAR). */
void ol_display_liquid_glass_ex(struct ol_display *ctx, int x, int y, int w, int h,
                                int radius, unsigned cut,
                                const struct ol_glass_material *mat);

/* Backdrop reach beyond the panel for this material: the tier's blur support
 * plus refraction. For REGULAR (or NULL) it equals ol_display_glass_sample_halo. */
int ol_display_glass_sample_halo_ex(int w, int h, int radius,
                                    const struct ol_glass_material *mat);

#endif
