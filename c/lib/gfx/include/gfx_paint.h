#ifndef GFX_PAINT_H
#define GFX_PAINT_H

/* gfx_paint.h -- paint kinds, stops, the constructors, the gradient strip and colour.
 * Includes gfx_math.h for the image inverse matrix. The GFX_HEADER_NEGCTL guard is
 * the control for the header split: it drops that include, so the TU check must fail. */

#ifndef GFX_HEADER_NEGCTL
#include "gfx_math.h"
#endif

#define GFX_MAX_STOPS  16      /* gradient stops (was 8; no ABI struct embeds it) */

/* ----------------------------------------------------------------- paint -- */
enum { GFX_SOLID = 0, GFX_LINEAR, GFX_RADIAL, GFX_IMAGE, GFX_CONIC, GFX_RADIAL2 };
/* paint->spread: what a gradient does outside its 0..1 ramp. PAD holds the end
 * stops, REPEAT restarts the ramp, REFLECT runs it back and forth. For a conic
 * paint one turn IS the ramp: PAD and REPEAT are the same, and REFLECT runs the
 * ramp forward over the first half turn and back over the second, so it is
 * seamless at the start angle whatever the end stops are. */
enum { GFX_SPREAD_PAD = 0, GFX_SPREAD_REPEAT = 1, GFX_SPREAD_REFLECT = 2 };
enum { GFX_FILTER_NEAREST = 0, GFX_FILTER_LINEAR = 1, GFX_FILTER_LINEAR_CLAMP = 2 };

/* paint->flags bits for GFX_IMAGE. Flags 0 is the legacy image paint: nearest or bilinear,
 * TRANSPARENT outside the image. IMG_CLAMP with spread PAD holds the edge texels instead
 * (CLAMP_EDGE); spread REPEAT and REFLECT tile the image on their own. FILTER_BOX asks for
 * area averaging (a box over the pixel's footprint in image space, premultiplied) once the
 * inverse matrix shrinks by more than 2x (gfx_m_scale_of(inv) > 2.0); below that the
 * ordinary filter runs, so a 1:1 or magnified image is untouched by the flag. */
enum { GFX_PAINT_IMG_CLAMP = 1, GFX_PAINT_FILTER_BOX = 2 };

/* paint->flags bits for the gradient kinds (LINEAR, RADIAL, CONIC, RADIAL2). They share the
 * byte with the image bits above, which a gradient never sets.
 *   HAS_PMAT  `pmat` maps PAINT space (the space the gradient's x0..y1 / cx,cy,r are written
 *             in) to device space, 16.16 + 24.8 as everywhere. A row is evaluated by sending
 *             the device pixel centre through the inverse of pmat, so a gradient follows a
 *             rotation or a shear instead of staying axis aligned. A singular pmat paints
 *             nothing. Without the bit the paint coordinates ARE device coordinates (legacy).
 *   DITHER    a 4x4 Bayer threshold of +-0.5 LSB is added to each colour channel before it is
 *             quantised to 8 bits, which breaks the long constant runs of a shallow ramp
 *             into a fine pattern. The mean is zero, and an exactly representable colour is
 *             unchanged. Alpha is not dithered. */
enum { GFX_PAINT_HAS_PMAT = 4, GFX_PAINT_DITHER = 8 };

struct gfx_stop { int t; unsigned color; int alpha; };   /* t: 0..65536 */

struct gfx_paint {
    int kind;
    unsigned color;  int alpha;              /* GFX_SOLID                     */
    struct gfx_stop stop[GFX_MAX_STOPS];
    int nstop;
    int x0, y0, x1, y1;                      /* GFX_LINEAR axis, device 24.8  */
    int cx, cy, r;                           /* GFX_RADIAL, device 24.8; GFX_CONIC: centre, and r is the
                                              * start angle in 24.8 degrees (y down, clockwise)     */
    const unsigned char *img;                /* GFX_IMAGE, straight RGBA      */
    int iw, ih, istride;
    struct gfx_matrix inv;                   /* device -> image space         */
    int bilinear;
    int global_alpha;                        /* 0..255, multiplies the lot    */
    /* Trailing and zero by default: op 0 is GFX_OP_SRC_OVER (gfx_blend.h), spread 0
     * is pad, flags 0 is none. Constructors zero-fill, and so does every brace or
     * positional initialiser, so an existing paint composites exactly as before. */
    unsigned char op, spread, flags, reserved0;
    /* GFX_IMAGE recorded through ol_cmd_fill/ol_cmd_fill_ex: the struct ol_image whose pixel
     * buffer `img` points into. The command retains it and reports OL_STALE_RESOURCE once the
     * image is updated. Zero for every constructor; a borrowed image paint without it is
     * still refused by the command list. */
    const void *img_ref;
    /* Gradient paint-to-device matrix, read only under GFX_PAINT_HAS_PMAT. Trailing, so
     * every existing initialiser leaves it zero (and unread). */
    struct gfx_matrix pmat;
};

void gfx_paint_solid (struct gfx_paint *p, unsigned color, int alpha);
void gfx_paint_linear(struct gfx_paint *p, int x0, int y0, int x1, int y1);
void gfx_paint_radial(struct gfx_paint *p, int cx, int cy, int r);
/* Angular sweep round (cx,cy), device 24.8. t runs 0..1 over one full turn,
 * clockwise on screen, starting at `start_deg24_8` (0 = +x axis, 90 = down). */
void gfx_paint_conic (struct gfx_paint *p, int cx, int cy, int start_deg24_8);
/* Two-point conical gradient, Skia / CSS semantics: t=0 is the circle (fx,fy,fr) and t=1 the
 * circle (cx,cy,r); the circle at t has centre and radius interpolated linearly, and a pixel
 * takes the LARGEST t whose circle passes through it and has a non-negative radius. Where no
 * such circle exists (a cone that does not cover the plane) the pixel is left unpainted. With
 * fr = 0 and the focal point inside the end circle this is the familiar focal radial; with
 * (fx,fy,fr) == (cx,cy,0) it is exactly gfx_paint_radial. Stored in x0,y0 (focal centre), x1
 * (fr), cx,cy,r. Negative radii are taken as 0. Coordinates in the 24.8 paint space, and
 * magnitudes beyond +-8192 px are clamped. */
void gfx_paint_radial2(struct gfx_paint *p, int fx, int fy, int fr, int cx, int cy, int r);
void gfx_paint_stop  (struct gfx_paint *p, int t, unsigned color, int alpha);
/* `img2dev` maps image pixels (24.8) to device (24.8); it is inverted here, so
 * a singular matrix is refused with 0 rather than dividing by zero per pixel. */
int  gfx_paint_image (struct gfx_paint *p, const unsigned char *rgba,
                      int w, int h, int stride,
                      const struct gfx_matrix *img2dev, int bilinear);
/* Sample the paint's colour ramp at t (0..65536). Exposed because the 1-pixel
 * gradient strip trick needs it and so do tests. Between two stops of different
 * alpha the colour is interpolated PREMULTIPLIED and then unpremultiplied with
 * rounding, so red fading to transparent stays red (the straight lerp, kept
 * under -DGFX_STOPS_STRAIGHT_LERP for the negative control only, goes dark). */
unsigned gfx_paint_sample(const struct gfx_paint *p, int t, int *alpha_out);

/* The conic span, declared here (not in gfx_paint_internal.h) so that a caller
 * outside raster/ that already dispatches linear and radial rows by hand, such as
 * adapters/display_path.c, can add the conic case. Same contract as
 * gfx_gradient_linear_row: composites src-over onto the row at drow + x*4. */
void gfx_gradient_conic_row(const struct gfx_paint *p, unsigned char *drow, int y,
                            int x0, int x1, const unsigned char *cov, int cov_ox);

void gfx_gradient_radial2_row(const struct gfx_paint *p, unsigned char *drow, int y,
                              int x0, int x1, const unsigned char *cov, int cov_ox);

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
