#include "text.h"
#include "ttf.h"

/* Anti-aliased glyph rasterizer — integer/fixed-point only (the kernel is built
 * -mno-sse, so no floating point). Outline points are scaled to 8.8 fixed-point
 * pixel coordinates (256 = one pixel). Each pixel row is sampled by 4 sub-
 * scanlines; spans (nonzero winding) contribute fractional horizontal coverage;
 * the 4 samples average to a 0..255 alpha. */

#define SUB        4                  /* vertical sub-scanlines per pixel row */
#define MAXW       768                /* max glyph bitmap width  (px) */
#define MAXEDGE    8192               /* flattened line segments */
#define MAXCROSS   1024               /* crossings per scanline */
#define OUTLINE_SCRATCH (1 << 18)

struct edge { int x0, y0, x1, y1; };  /* 8.8 fixed, y grows down */

#define MAXEXP     8192               /* expanded contour points */
static struct edge edges[MAXEDGE];
static int nedges;
static int ex_x[MAXEXP], ex_y[MAXEXP];
static uint8_t ex_on[MAXEXP];
static int acc[MAXW];                 /* per-pixel coverage accumulator (0..1024) */
static int cross[MAXCROSS];           /* x (8.8) packed with winding dir in low bit-sign */
static uint8_t ol_scratch[OUTLINE_SCRATCH];

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

int text_raster(const struct ttf_font *f, int gid, int px,
                uint8_t *cov, int covcap, int *wout, int *hout, int *ox, int *oy)
{
    struct ttf_outline o;
    if (ttf_glyph_outline(f, gid, &o, ol_scratch, sizeof ol_scratch)) return -1;

    int upem = f->units_per_em;
    /* font unit -> 8.8 fixed pixel (baseline origin, y up) */
    #define FX(v) ((int)(((long)(v) * px * 256) / upem))

    if (o.ncontours == 0) { *wout = 0; *hout = 0; *ox = 0; *oy = 0; return 0; }

    int fxmin = FX(o.xmin), fxmax = FX(o.xmax), fymin = FX(o.ymin), fymax = FX(o.ymax);
    int ox_i  = (fxmin >> 8) - 1;
    int top_i = (fymax >> 8) + 1;                       /* pixels above baseline */
    int w = ((fxmax + 255) >> 8) - ox_i + 1;
    int h = top_i - (fymin >> 8) + 1;
    if (w <= 0 || h <= 0) { *wout=0; *hout=0; *ox=0; *oy=0; return 0; }
    if (w > MAXW || w * h > covcap) return -1;

    /* Build edges in bitmap-local 8.8 fixed (x right, y down from bitmap top). */
    nedges = 0;
    int seg = px / 8; if (seg < 2) seg = 2; if (seg > 16) seg = 16;
    int start = 0;
    for (int ci = 0; ci < o.ncontours; ci++) {
        int end = o.contour_end[ci];
        int n = end - start + 1;
        if (n < 2) { start = end + 1; continue; }
        #define BX(i) (FX(o.x[i]) - ox_i*256)
        #define BY(i) (top_i*256 - FX(o.y[i]))
        /* find a starting on-curve point (or synthesize from two off points) */
        int s0 = -1;
        for (int i = start; i <= end; i++) if (o.on[i]) { s0 = i; break; }
        /* Build an expanded contour: starts on-curve, inserts implied on-curve
         * midpoints between consecutive off-curve points, ends back at start. */
        int sx, sy, len = 0;
        if (s0 < 0) { ex_x[0] = (BX(start)+BX(end))/2; ex_y[0] = (BY(start)+BY(end))/2;
                      ex_on[0] = 1; s0 = start; }
        else { ex_x[0] = BX(s0); ex_y[0] = BY(s0); ex_on[0] = 1; }
        sx = ex_x[0]; sy = ex_y[0]; len = 1;
        for (int j = 1; j <= n && len < MAXEXP - 2; j++) {
            int idx = start + ((s0 - start + j) % n);
            int px2 = BX(idx), py2 = BY(idx), on = o.on[idx];
            if (!on && !ex_on[len-1]) {                 /* two offs: insert midpoint */
                ex_x[len] = (ex_x[len-1] + px2)/2; ex_y[len] = (ex_y[len-1] + py2)/2;
                ex_on[len] = 1; len++;
            }
            ex_x[len] = px2; ex_y[len] = py2; ex_on[len] = on; len++;
        }
        ex_x[len] = sx; ex_y[len] = sy; ex_on[len] = 1; len++;   /* close back to start */
        /* walk: on->on = line, on->off->on = quad */
        int curx = ex_x[0], cury = ex_y[0], i = 1;
        while (i < len) {
            if (ex_on[i]) { add_edge(curx, cury, ex_x[i], ex_y[i]); curx = ex_x[i]; cury = ex_y[i]; i++; }
            else {        /* ex[i] off control, ex[i+1] is on (guaranteed) */
                quad(curx, cury, ex_x[i], ex_y[i], ex_x[i+1], ex_y[i+1], seg);
                curx = ex_x[i+1]; cury = ex_y[i+1]; i += 2;
            }
        }
        start = end + 1;
        #undef BX
        #undef BY
    }

    for (int i = 0; i < w * h; i++) cov[i] = 0;

    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) acc[c] = 0;
        for (int k = 0; k < SUB; k++) {
            int Y = r * 256 + 32 + k * 64;              /* sub-scanline y (8.8) */
            int nc = 0;
            for (int e = 0; e < nedges && nc + 1 < MAXCROSS; e++) {
                int y0 = edges[e].y0, y1 = edges[e].y1;
                int lo = y0 < y1 ? y0 : y1, hi = y0 < y1 ? y1 : y0;
                if (Y < lo || Y >= hi) continue;
                int x = edges[e].x0 + (int)(((long)(edges[e].x1 - edges[e].x0) *
                          (Y - y0)) / (y1 - y0));
                cross[nc++] = (x << 1) | (y1 > y0 ? 1 : 0);   /* pack dir in bit 0 */
            }
            /* heapsort by x (cross[i]>>1); O(nc log nc), in-place, no scratch */
            for (int hs = nc / 2 - 1; hs >= 0; hs--) {   /* heapify (max-heap) */
                int root = hs, v = cross[root];
                for (;;) {                               /* sift v down */
                    int child = 2 * root + 1;
                    if (child >= nc) break;
                    if (child + 1 < nc && (cross[child+1] >> 1) > (cross[child] >> 1)) child++;
                    if ((cross[child] >> 1) <= (v >> 1)) break;
                    cross[root] = cross[child]; root = child;
                }
                cross[root] = v;
            }
            for (int end = nc - 1; end > 0; end--) {     /* pop max -> end */
                int v = cross[end]; cross[end] = cross[0];
                int root = 0;
                for (;;) {                               /* sift v down in [0,end) */
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
                int xa = cross[a] >> 1, xb = cross[a+1] >> 1;       /* filled span */
                if (xb <= xa) continue;
                int ca = xa >> 8, cb = (xb - 1) >> 8;
                if (ca < 0) ca = 0; if (cb >= w) cb = w - 1;
                for (int c = ca; c <= cb; c++) {
                    int left = c << 8, right = (c + 1) << 8;
                    int l = xa > left ? xa : left, rr = xb < right ? xb : right;
                    if (rr > l) acc[c] += rr - l;       /* 0..256 */
                }
            }
        }
        for (int c = 0; c < w; c++) {
            int a = acc[c] >> 2;                         /* /SUB -> 0..256 */
            cov[r * w + c] = (uint8_t)(a > 255 ? 255 : a);
        }
    }

    *wout = w; *hout = h; *ox = ox_i; *oy = top_i;
    return 0;
    #undef FX
}
