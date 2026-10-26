#ifndef GFX_PAINT_H
#define GFX_PAINT_H

/* gfx_paint.h -- paint kinds, stops, the constructors, the gradient strip and colour.
 * Includes gfx_math.h for the image inverse matrix. The GFX_HEADER_NEGCTL guard is
 * the control for the header split: it drops that include, so the TU check must fail. */

#ifndef GFX_HEADER_NEGCTL
#include "gfx_math.h"
#endif

#define GFX_MAX_STOPS  8       /* gradient stops                            */

/* ----------------------------------------------------------------- paint -- */
enum { GFX_SOLID = 0, GFX_LINEAR, GFX_RADIAL, GFX_IMAGE };
enum { GFX_FILTER_NEAREST = 0, GFX_FILTER_LINEAR = 1, GFX_FILTER_LINEAR_CLAMP = 2 };

struct gfx_stop { int t; unsigned color; int alpha; };   /* t: 0..65536 */

struct gfx_paint {
    int kind;
    unsigned color;  int alpha;              /* GFX_SOLID                     */
    struct gfx_stop stop[GFX_MAX_STOPS];
    int nstop;
    int x0, y0, x1, y1;                      /* GFX_LINEAR axis, device 24.8  */
    int cx, cy, r;                           /* GFX_RADIAL, device 24.8       */
    const unsigned char *img;                /* GFX_IMAGE, straight RGBA      */
    int iw, ih, istride;
    struct gfx_matrix inv;                   /* device -> image space         */
    int bilinear;
    int global_alpha;                        /* 0..255, multiplies the lot    */
    /* Trailing and zero by default: op 0 is GFX_OP_SRC_OVER (gfx_blend.h), spread 0
     * is pad, flags 0 is none. Constructors zero-fill, and so does every brace or
     * positional initialiser, so an existing paint composites exactly as before. */
    unsigned char op, spread, flags, reserved0;
};

void gfx_paint_solid (struct gfx_paint *p, unsigned color, int alpha);
void gfx_paint_linear(struct gfx_paint *p, int x0, int y0, int x1, int y1);
void gfx_paint_radial(struct gfx_paint *p, int cx, int cy, int r);
void gfx_paint_stop  (struct gfx_paint *p, int t, unsigned color, int alpha);
/* `img2dev` maps image pixels (24.8) to device (24.8); it is inverted here, so
 * a singular matrix is refused with 0 rather than dividing by zero per pixel. */
int  gfx_paint_image (struct gfx_paint *p, const unsigned char *rgba,
                      int w, int h, int stride,
                      const struct gfx_matrix *img2dev, int bilinear);
/* Sample the paint's colour ramp at t (0..65536). Exposed because the 1-pixel
 * gradient strip trick needs it and so do tests. */
unsigned gfx_paint_sample(const struct gfx_paint *p, int t, int *alpha_out);

/* The 1-pixel gradient strip. `n` entries of straight RGBA written to dst; the
 * compositor replicates along the constant axis, so a gradient costs what a
 * flat fill costs. */
void gfx_gradient_strip(unsigned char *dst, int n, unsigned c0, unsigned c1, int alpha);
void gfx_gradient_strip_paint(unsigned char *dst, int n, const struct gfx_paint *p);

/* ---------------------------------------------------------------- colour -- */
unsigned gfx_mix(unsigned a, unsigned b, int t);      /* t 0..255 -> b */
#define GFX_R(c) (int)(((c) >> 16) & 255)
#define GFX_G(c) (int)(((c) >> 8) & 255)
#define GFX_B(c) (int)((c) & 255)
#define GFX_RGB(r,g,b) ((unsigned)(((r) << 16) | ((g) << 8) | (b)))

#endif /* GFX_PAINT_H */
