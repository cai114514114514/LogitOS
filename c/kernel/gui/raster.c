#include "text.h"
#include "ttf.h"
#include "fontpath.h"
#include "vg.h"

/* Anti-aliased glyph rasterizer — integer/fixed-point only (the kernel is built
 * -mno-sse, so no floating point). Outline points are scaled to 8.8 fixed-point
 * pixel coordinates (256 = one pixel). Each pixel row is sampled by 4 sub-
 * scanlines; spans (nonzero winding) contribute fractional horizontal coverage;
 * the 4 samples average to a 0..255 alpha. */

/* Vertical sub-scanlines per pixel row, as a power of two. Horizontal coverage
 * is computed exactly (fractional span overlap), so this is the only place the
 * rasterizer is approximate -- and it showed: at SUB=4 a thin horizontal bar
 * such as the underscore, whose top and bottom edges both land inside one
 * sub-scanline band, came out 11/255 away from FreeType on average. At SUB=16
 * that drops to about 3. The cost is four times the scanline passes, paid once
 * per (glyph, size) because kernel/gui/text.c caches the coverage bitmap. */
#ifndef SUB_SHIFT
#define SUB_SHIFT  4
#endif
#define SUB        (1 << SUB_SHIFT)   /* vertical sub-scanlines per pixel row */
#define SUB_STEP   (256 >> SUB_SHIFT) /* 8.8 fixed distance between them */
#define MAXW       768                /* max glyph bitmap width  (px) */
#define MAXEDGE    8192               /* flattened line segments */
#define MAXCROSS   1024               /* crossings per scanline */
#define OUTLINE_SCRATCH (1 << 18)

struct edge { int x0, y0, x1, y1; };  /* 8.8 fixed, y grows down */

#define MAXPATH    4096               /* drawing commands in one glyph outline */
static struct edge edges[MAXEDGE];
static int nedges;
static int acc[MAXW];                 /* per-pixel coverage accumulator (0..1024) */
static int cross[MAXCROSS];           /* x (8.8) packed with winding dir in low bit-sign */
static uint8_t ol_scratch[OUTLINE_SCRATCH];
static struct fp_cmd path_cmds[MAXPATH];

static void add_edge(int x0, int y0, int x1, int y1)
{
    if (y0 == y1) return;                              /* horizontal: ignored */
    if (nedges < MAXEDGE) { edges[nedges].x0=x0; edges[nedges].y0=y0;
                            edges[nedges].x1=x1; edges[nedges].y1=y1; nedges++; }
}

/* Flatten a quadratic (p0..pc..p1, all 8.8 fixed) into line segments. */
static void quad(int x0,int y0,int cx,int cy,int x1,int y1,int seg)
{
    int px = x0, py = y0;
    for (int i = 1; i <= seg; i++) {
        /* B(t) with t = i/seg, integer math (t scaled by seg) */
        int s = seg, t = i, u = s - t;
        /* (u*u*P0 + 2*u*t*C + t*t*P1) / (s*s) */
        long nx = (long)u*u*x0 + 2L*u*t*cx + (long)t*t*x1;
        long ny = (long)u*u*y0 + 2L*u*t*cy + (long)t*t*y1;
        int qx = (int)(nx / ((long)s*s)), qy = (int)(ny / ((long)s*s));
        add_edge(px, py, qx, qy);
        px = qx; py = qy;
    }
}

/* Flatten a cubic Bezier (p0..c1..c2..p1, all 8.8 fixed) into line segments. */
static void cubic(int x0,int y0,int c1x,int c1y,int c2x,int c2y,int x1,int y1,int seg)
{
    int px = x0, py = y0;
    for (int i = 1; i <= seg; i++) {
        long s = seg, t = i, u = s - t;
        long uu = u*u, tt = t*t, ss = s*s*s;
        long nx = u*uu*x0 + 3*uu*t*c1x + 3*u*tt*c2x + t*tt*x1;
        long ny = u*uu*y0 + 3*uu*t*c1y + 3*u*tt*c2y + t*tt*y1;
        int qx = (int)(nx / ss), qy = (int)(ny / ss);
        add_edge(px, py, qx, qy); px = qx; py = qy;
    }
}

/* Scanline-fill the current edges[] (nonzero winding, 4x vertical AA) into the
 * w*h coverage bitmap cov (0..255). Shared by text_raster + vg_render_path. */
static void fill_edges(uint8_t *cov, int w, int h)
{
    for (int i = 0; i < w * h; i++) cov[i] = 0;
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) acc[c] = 0;
        for (int k = 0; k < SUB; k++) {
            int Y = r * 256 + SUB_STEP / 2 + k * SUB_STEP;
            int nc = 0;
            for (int e = 0; e < nedges && nc + 1 < MAXCROSS; e++) {
                int y0 = edges[e].y0, y1 = edges[e].y1;
                int lo = y0 < y1 ? y0 : y1, hi = y0 < y1 ? y1 : y0;
                if (Y < lo || Y >= hi) continue;
                int x = edges[e].x0 + (int)(((long)(edges[e].x1 - edges[e].x0) *
                          (Y - y0)) / (y1 - y0));
                cross[nc++] = (x << 1) | (y1 > y0 ? 1 : 0);
            }
            for (int hs = nc / 2 - 1; hs >= 0; hs--) {
                int root = hs, v = cross[root];
                for (;;) {
                    int child = 2 * root + 1;
                    if (child >= nc) break;
                    if (child + 1 < nc && (cross[child+1] >> 1) > (cross[child] >> 1)) child++;
                    if ((cross[child] >> 1) <= (v >> 1)) break;
                    cross[root] = cross[child]; root = child;
                }
                cross[root] = v;
            }
            for (int end = nc - 1; end > 0; end--) {
                int v = cross[end]; cross[end] = cross[0];
                int root = 0;
                for (;;) {
                    int child = 2 * root + 1;
                    if (child >= end) break;
                    if (child + 1 < end && (cross[child+1] >> 1) > (cross[child] >> 1)) child++;
                    if ((cross[child] >> 1) <= (v >> 1)) break;
                    cross[root] = cross[child]; root = child;
                }
                cross[root] = v;
            }
            int wind = 0;
            for (int a = 0; a + 1 < nc; a++) {
                wind += (cross[a] & 1) ? 1 : -1;
                if (wind == 0) continue;
                int xa = cross[a] >> 1, xb = cross[a+1] >> 1;
                if (xb <= xa) continue;
                int ca = xa >> 8, cb = (xb - 1) >> 8;
                if (ca < 0) ca = 0; if (cb >= w) cb = w - 1;
                for (int c = ca; c <= cb; c++) {
                    int left = c << 8, right = (c + 1) << 8;
                    int l = xa > left ? xa : left, rr = xb < right ? xb : right;
                    if (rr > l) acc[c] += rr - l;
                }
            }
        }
        for (int c = 0; c < w; c++) {
            int a = acc[c] >> SUB_SHIFT;
            cov[r * w + c] = (uint8_t)(a > 255 ? 255 : a);
        }
    }
}

/* Flatten one outline path (font units, y up) into edges[] in bitmap-local 8.8
 * fixed coordinates. Shared by text_raster and the colour-layer path, which
 * needs the SAME bitmap origin across layers or the layers would not line up. */
static void path_to_edges(const struct fp_path *path, int px, int upem,
                          int ox_i, int top_i)
{
    #define FX(v)   ((int)(((long)(v) * px * 256) / upem))
    #define BX(v)   (FX(v) - ox_i * 256)
    #define BY(v)   (top_i * 256 - FX(v))
    nedges = 0;
    int qseg = px / 8; if (qseg < 2) qseg = 2;  if (qseg > 16) qseg = 16;
    int cseg = px / 6; if (cseg < 3) cseg = 3;  if (cseg > 24) cseg = 24;
    int curx = 0, cury = 0, sx = 0, sy = 0;
    for (int i = 0; i < path->n; i++) {
        const struct fp_cmd *c = &path->cmd[i];
        switch (c->op) {
        case FP_MOVE:
            curx = BX(c->x[0]); cury = BY(c->y[0]); sx = curx; sy = cury;
            break;
        case FP_LINE: {
            int nx = BX(c->x[0]), ny = BY(c->y[0]);
            add_edge(curx, cury, nx, ny); curx = nx; cury = ny;
            break;
        }
        case FP_QUAD: {
            int cx = BX(c->x[0]), cy = BY(c->y[0]);
            int ex = BX(c->x[1]), ey = BY(c->y[1]);
            quad(curx, cury, cx, cy, ex, ey, qseg); curx = ex; cury = ey;
            break;
        }
        case FP_CUBIC: {
            int ax = BX(c->x[0]), ay = BY(c->y[0]);
            int bx = BX(c->x[1]), by = BY(c->y[1]);
            int ex = BX(c->x[2]), ey = BY(c->y[2]);
            cubic(curx, cury, ax, ay, bx, by, ex, ey, cseg); curx = ex; cury = ey;
            break;
        }
        case FP_CLOSE:
            add_edge(curx, cury, sx, sy); curx = sx; cury = sy;
            break;
        }
    }
    #undef BX
    #undef BY
    #undef FX
}

/* Bitmap placement for a path at `px`: the origin column/top row, plus the
 * width and height that hold it. Returns 0, or -1 for an empty path. */
static int path_extent(const struct fp_path *path, int px, int upem,
                       int *ox_i, int *top_i, int *w, int *h)
{
    #define FX(v) ((int)(((long)(v) * px * 256) / upem))
    if (path->n == 0 || path->xmax < path->xmin) return -1;
    int fxmin = FX(path->xmin), fxmax = FX(path->xmax);
    int fymin = FX(path->ymin), fymax = FX(path->ymax);
    *ox_i  = (fxmin >> 8) - 1;
    *top_i = (fymax >> 8) + 1;                          /* pixels above baseline */
    *w = ((fxmax + 255) >> 8) - *ox_i + 1;
    *h = *top_i - (fymin >> 8) + 1;
    return 0;
    #undef FX
}

int text_raster(const struct ttf_font *f, int gid, int px,
                uint8_t *cov, int covcap, int *wout, int *hout, int *ox, int *oy)
{
    struct fp_path path;
    fp_init(&path, path_cmds, (int)sizeof path_cmds);
    if (ttf_glyph_path(f, gid, &path, ol_scratch, sizeof ol_scratch)) return -1;

    int upem = f->units_per_em;
    if (upem <= 0) return -1;
    int ox_i, top_i, w, h;
    if (path_extent(&path, px, upem, &ox_i, &top_i, &w, &h)) {
        *wout = 0; *hout = 0; *ox = 0; *oy = 0; return 0;   /* blank glyph */
    }
    if (w <= 0 || h <= 0) { *wout = 0; *hout = 0; *ox = 0; *oy = 0; return 0; }
    if (w > MAXW || w * h > covcap) return -1;

    path_to_edges(&path, px, upem, ox_i, top_i);
    fill_edges(cov, w, h);

    *wout = w; *hout = h; *ox = ox_i; *oy = top_i;
    return 0;
}

/* Rasterize one COLR layer glyph into the bitmap box a PREVIOUS call reported
 * for the composite. Colour glyphs are several outlines that must be composited
 * in one buffer, so the caller fixes the box once (text_raster_extent) and every
 * layer paints into it. Returns 0, or -1 if the layer has no outline. */
int text_raster_at(const struct ttf_font *f, int gid, int px, int ox_i, int top_i,
                   uint8_t *cov, int w, int h)
{
    struct fp_path path;
    fp_init(&path, path_cmds, (int)sizeof path_cmds);
    if (ttf_glyph_path(f, gid, &path, ol_scratch, sizeof ol_scratch)) return -1;
    if (path.n == 0) return -1;
    int upem = f->units_per_em;
    if (upem <= 0 || w <= 0 || h <= 0 || w > MAXW) return -1;
    path_to_edges(&path, px, upem, ox_i, top_i);
    fill_edges(cov, w, h);
    return 0;
}

/* The bitmap box text_raster would choose for `gid` at `px`, without painting.
 * Used to size the composite before the first colour layer is drawn. */
int text_raster_extent(const struct ttf_font *f, int gid, int px,
                       int *ox_i, int *top_i, int *w, int *h)
{
    struct fp_path path;
    fp_init(&path, path_cmds, (int)sizeof path_cmds);
    if (ttf_glyph_path(f, gid, &path, ol_scratch, sizeof ol_scratch)) return -1;
    int upem = f->units_per_em;
    if (upem <= 0) return -1;
    return path_extent(&path, px, upem, ox_i, top_i, w, h);
}

/* Rasterize a vector path (coords in [0,unit]) into a px_size x px_size coverage
 * bitmap. Builds edges from move/line/quad/cubic/close, then the shared AA fill.
 * Nonzero winding: outer contours CW, holes CCW. */
int vg_render_path(const struct vg_cmd *cmds, int ncmd, int unit, int px_size,
                   uint8_t *cov, int covcap, int *wout, int *hout)
{
    int w = px_size, h = px_size;
    if (w <= 0 || h <= 0 || w > MAXW || w * h > covcap || unit <= 0) return -1;
    #define SX(v) ((int)(((long)(v) * px_size * 256) / unit))   /* unit -> 8.8 px */
    nedges = 0;
    int seg = px_size / 6; if (seg < 3) seg = 3; if (seg > 24) seg = 24;
    int cx = 0, cy = 0, sx0 = 0, sy0 = 0;
    for (int i = 0; i < ncmd; i++) {
        const struct vg_cmd *c = &cmds[i];
        switch (c->op) {
        case VG_MOVE: cx = SX(c->x[0]); cy = SX(c->y[0]); sx0 = cx; sy0 = cy; break;
        case VG_LINE: { int nx = SX(c->x[0]), ny = SX(c->y[0]); add_edge(cx, cy, nx, ny); cx = nx; cy = ny; } break;
        case VG_QUAD: { int qx = SX(c->x[0]), qy = SX(c->y[0]), ex = SX(c->x[1]), ey = SX(c->y[1]);
                        quad(cx, cy, qx, qy, ex, ey, seg); cx = ex; cy = ey; } break;
        case VG_CUBIC:{ int a = SX(c->x[0]), b = SX(c->y[0]), d = SX(c->x[1]), e = SX(c->y[1]), f = SX(c->x[2]), g = SX(c->y[2]);
                        cubic(cx, cy, a, b, d, e, f, g, seg); cx = f; cy = g; } break;
        case VG_CLOSE: add_edge(cx, cy, sx0, sy0); cx = sx0; cy = sy0; break;
        }
    }
    fill_edges(cov, w, h);
    *wout = w; *hout = h;
    return 0;
    #undef SX
}
