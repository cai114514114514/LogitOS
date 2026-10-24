#ifndef LOGIT_PAINT_GRADIENT_H
#define LOGIT_PAINT_GRADIENT_H

#include "css/gradient.h"

/* Prepared data owns numbers only: CSS tokens may die after prepare. Colors
 * are premultiplied sRGBA, with every component in 0..255. No GUI coordinate,
 * clipping or element opacity belongs here; a background layer supplies its
 * image coordinates after resolving size/position/repeat. */
struct paint_stop {
    double pos, rgba[4], hint;
};
struct paint_gradient {
    int kind, repeating, nstop, radial_mode;
    double cx, cy, ux, uy, length, rx, ry, rotation;
    double average[4];
    struct paint_stop stop[CG_MAXSTOP];
};

int css_gradient_prepare(const struct cgradient *source, double width,
                         double height, unsigned current_color,
                         struct paint_gradient *out);
/* Computed color includes alpha. The RGB-only wrapper above is for existing
 * numeric callers; CSS consumers must use this form or transparent
 * currentColor becomes an opaque mask-shaped rectangle. */
int css_gradient_prepare_rgba(const struct cgradient *source, double width,
                              double height, unsigned current_color,
                              int current_alpha, struct paint_gradient *out);
void css_gradient_sample(const struct paint_gradient *gradient, double x,
                         double y, unsigned *rgb, int *alpha);
/* A physical pixel footprint in image CSS px. A repeating period that cannot
 * be resolved at that scale must become its average color (CSS Images 3 §3.3),
 * rather than flicker according to a lucky center sample. */
void css_gradient_sample_pixel(const struct paint_gradient *gradient, double x,
                               double y, double pixel_width,
                               double pixel_height, unsigned *rgb, int *alpha);

#endif
