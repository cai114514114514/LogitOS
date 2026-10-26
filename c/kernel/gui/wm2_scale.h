#ifndef LOGIT_WM2_SCALE_H
#define LOGIT_WM2_SCALE_H
/* Points -> device pixels for the GUI ABI v2 handlers. Pure static inline,
 * with the scale passed in (struct wm2_env.scale_pct) rather than read from
 * fb.c, so a handler converts every coordinate of one call at ONE scale and
 * host tests can link these without fb.c.
 *
 * Every conversion FLOORS toward negative infinity, exactly like fb_pt (see
 * fb.c): a window dragged off the left edge has negative content
 * coordinates, and truncation toward zero there maps two adjacent logical
 * columns onto one device column -- the frame visibly shears at x = 0.
 * tests/unit/gui_abi_v2_test.c cross-checks wm2_pt against fb_pt over
 * [-2000, 2000] at 100..300%, and its control (-DGUI_ABI_NEGCTL) restores
 * the truncating version to prove the check can see that shear. */

/* floor(n / d) for d > 0, in 64 bits so 24.8 * 400% cannot overflow. */
static inline long long wm2_floordiv(long long n, long long d)
{
    long long q = n / d;
    return (n % d != 0 && n < 0) ? q - 1 : q;
}

/* Clamp a 64-bit intermediate back into int: out-of-range input saturates
 * instead of wrapping into a coordinate on the other side of the canvas. */
static inline int wm2_sat(long long v)
{
    if (v > 0x7FFFFFFFLL) return 0x7FFFFFFF;
    if (v < -0x7FFFFFFFLL - 1) return (int)(-0x7FFFFFFFLL - 1);
    return (int)v;
}

/* Integer points -> device pixels. Identical to fb_pt(points) when pct is
 * fb_scale(). */
static inline int wm2_pt(int points, int pct)
{
#ifdef GUI_ABI_NEGCTL
    /* THE CONTROL: C division truncates toward zero, the shear above. */
    return (int)((long long)points * pct / 100);
#else
    return wm2_sat(wm2_floordiv((long long)points * pct, 100));
#endif
}

/* 24.8 points -> 24.8 device pixels: floor(v * pct / 100), keeping the
 * fraction. Used for lengths and positions that stay fractional (blur sigma,
 * path coordinates, AA rect edges). */
static inline int wm2_pt24(int v24, int pct)
{
    return wm2_sat(wm2_floordiv((long long)v24 * pct, 100));
}

/* A 24.8 rectangle converted the only seam-free way: convert both EDGES and
 * take the difference, never convert the width on its own. Output is 24.8
 * device pixels.
 *
 * snap != 0 (LOGIT_RECT_SNAP) applies the legacy integer rule to each edge:
 * the converted edge is floored to a whole device pixel. For whole-point
 * input that is exactly fb_pt of the edge, so a snapped rect covers the same
 * pixels SYS_GUI_RECT does, and two abutting 5-point columns at 150% are 7
 * and 8 pixels wide (edges 0, 7, 15) -- abutting at every scale. Without
 * snap the edges keep their fractions (0, 7.5, 15) for AA drawing. */
struct wm2_rect24 { int x, y, w, h; };
static inline int wm2_edge24(int e24, int pct, int snap)
{
    long long v = wm2_floordiv((long long)e24 * pct, 100);
    if (snap) v = wm2_floordiv(v, 256) * 256;
    return wm2_sat(v);
}
static inline struct wm2_rect24 wm2_rect24(int x24, int y24, int w24, int h24,
                                           int pct, int snap)
{
    struct wm2_rect24 r;
    long long x1 = (long long)x24 + w24, y1 = (long long)y24 + h24;
    r.x = wm2_edge24(x24, pct, snap);
    r.y = wm2_edge24(y24, pct, snap);
    r.w = wm2_sat((long long)wm2_edge24(wm2_sat(x1), pct, snap) - r.x);
    r.h = wm2_sat((long long)wm2_edge24(wm2_sat(y1), pct, snap) - r.y);
    return r;
}
#endif /* LOGIT_WM2_SCALE_H */
