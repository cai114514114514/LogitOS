#ifndef LOGIT_FONTPATH_H
#define LOGIT_FONTPATH_H

#include <stdint.h>

/* One outline, in font units, as drawing commands rather than as the TrueType
 * point/flag arrays.
 *
 * The reason this type exists: glyf outlines are quadratic and CFF outlines are
 * cubic, and the rasterizer already knows how to flatten both. Converting CFF
 * cubics down to quadratics to reuse the old point-array shape would throw away
 * accuracy for nothing, so both parsers emit into this instead and the
 * rasterizer consumes one representation.
 *
 * Coordinates are integers in font units, y up, origin at the glyph origin --
 * the same space struct ttf_outline used. CFF charstrings can carry fractional
 * coordinates (the 255 operator is 16.16 fixed); those are accumulated at full
 * precision inside the interpreter and rounded once, here, at emit time. A half
 * font-unit is 1/2000 em, i.e. well under a hundredth of a pixel at any size we
 * draw, and keeping the path in font units is what lets the glyph cache key on
 * (gid, px) without re-running the interpreter per subpixel position. */

enum {
    FP_MOVE = 0,     /* start a new contour at (x0,y0) */
    FP_LINE,         /* line to (x0,y0) */
    FP_QUAD,         /* quadratic: control (x0,y0), end (x1,y1) */
    FP_CUBIC,        /* cubic: controls (x0,y0),(x1,y1), end (x2,y2) */
    FP_CLOSE         /* close back to the contour's start point */
};

struct fp_cmd {
    int32_t x[3], y[3];
    uint8_t op;
};

struct fp_path {
    struct fp_cmd *cmd;
    int n, cap;
    int overflow;                    /* set if a command did not fit */
    int xmin, ymin, xmax, ymax;      /* over the emitted points only */
};

static inline void fp_init(struct fp_path *p, void *buf, int bytes)
{
    p->cmd = (struct fp_cmd *)buf;
    p->cap = bytes / (int)sizeof(struct fp_cmd);
    p->n = 0; p->overflow = 0;
    p->xmin = p->ymin = 0x7FFFFFFF;
    p->xmax = p->ymax = -0x7FFFFFFF;
}

static inline void fp_bound(struct fp_path *p, int32_t x, int32_t y)
{
    if (x < p->xmin) p->xmin = x;
    if (x > p->xmax) p->xmax = x;
    if (y < p->ymin) p->ymin = y;
    if (y > p->ymax) p->ymax = y;
}

/* Append a command. `npt` points are taken from xs/ys. Returns 0, or -1 when the
 * path is full (and latches ->overflow so the caller can reject the glyph rather
 * than draw a truncated one). */
static inline int fp_add(struct fp_path *p, int op, const int32_t *xs, const int32_t *ys,
                         int npt)
{
    if (p->n >= p->cap) { p->overflow = 1; return -1; }
    struct fp_cmd *c = &p->cmd[p->n++];
    c->op = (uint8_t)op;
    c->x[0] = c->x[1] = c->x[2] = 0;
    c->y[0] = c->y[1] = c->y[2] = 0;
    for (int i = 0; i < npt; i++) { c->x[i] = xs[i]; c->y[i] = ys[i]; fp_bound(p, xs[i], ys[i]); }
    return 0;
}

static inline int fp_move(struct fp_path *p, int32_t x, int32_t y)
{ int32_t xs[1] = { x }, ys[1] = { y }; return fp_add(p, FP_MOVE, xs, ys, 1); }

static inline int fp_line(struct fp_path *p, int32_t x, int32_t y)
{ int32_t xs[1] = { x }, ys[1] = { y }; return fp_add(p, FP_LINE, xs, ys, 1); }

static inline int fp_quad(struct fp_path *p, int32_t cx, int32_t cy, int32_t x, int32_t y)
{ int32_t xs[2] = { cx, x }, ys[2] = { cy, y }; return fp_add(p, FP_QUAD, xs, ys, 2); }

static inline int fp_cubic(struct fp_path *p, int32_t ax, int32_t ay, int32_t bx,
                           int32_t by, int32_t x, int32_t y)
{ int32_t xs[3] = { ax, bx, x }, ys[3] = { ay, by, y }; return fp_add(p, FP_CUBIC, xs, ys, 3); }

static inline int fp_close(struct fp_path *p)
{ return fp_add(p, FP_CLOSE, 0, 0, 0); }

#endif /* LOGIT_FONTPATH_H */
