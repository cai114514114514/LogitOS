/* Open Logit -- arcs, SVG endpoint arcs, ring sectors, elliptical-corner and
 * smoothed ("squircle") rounded rectangles, and an integer atan2.
 *
 * Everything here is ordinary path construction: the shapes are emitted
 * through gfx_move_to / gfx_line_to / gfx_cubic_to, so the current matrix and
 * the flattening tolerance behave exactly as for gfx_path_circle. No state, no
 * allocation: builders write only into the caller's path and the atan table is
 * static const (so there is nothing to make per-context for GFX_NCTX).
 *
 * Every curve is a chain of cubic pieces of at most 90 degrees. One cubic is
 * a good circle only for a small angle: the radial error of a single piece of
 * angle A is roughly 2.7e-4 * r at 90 degrees and grows with the 6th power of
 * A, so a 180 degree piece is already ~1.8% off. -DGFX_ARC_NEGCTL raises the
 * piece limit to 180 degrees so the radial-error budget in
 * tests/unit/gfx_arc_test.c can be watched failing.
 *
 * Integer only, so output is bit-identical on every host and in the kernel.
 * Angles are 24.8 degrees; trig comes from gfx_sin/gfx_cos (16.16). */
#include "gfx.h"

#ifdef GFX_ARC_NEGCTL
#define ARC_PIECE_MAX (180 * GFX_ONE)
#else
#define ARC_PIECE_MAX (90 * GFX_ONE)
#endif
#define DEG(n)        ((n) * GFX_ONE)
#define KAPPA         36195          /* 4/3*tan(pi/8), 16.16 (as gfx_path.c) */
#define SQRT2_Q16     92682          /* sqrt(2) in 16.16 */

/* ------------------------------------------------------------- atan2 ----
 * atan(i/64) for i = 0..64 in 24.8 degrees. Linear interpolation between
 * entries is off by at most (1/64)^2/8 * 0.65 rad = 0.0011 degrees. */
static const unsigned short atan_lut[65] = {
       0,  229,  458,  687,  916, 1144, 1371, 1598, 1824, 2049,
    2273, 2497, 2719, 2939, 3159, 3377, 3593, 3808, 4021, 4233,
    4443, 4650, 4856, 5060, 5262, 5462, 5660, 5856, 6049, 6240,
    6429, 6616, 6801, 6983, 7163, 7340, 7516, 7689, 7859, 8027,
    8193, 8357, 8518, 8677, 8834, 8989, 9141, 9291, 9439, 9584,
    9728, 9869,10008,10145,10280,10413,10544,10672,10799,10924,
   11047,11168,11287,11405,11520
};

/* atan(t) for t in 16.16, 0..1, as 24.8 degrees. */
static int atan_unit(long long t)
{
    int i = (int)(t >> 10), f = (int)(t & 1023);
    if (i >= 64) return atan_lut[64];
    return atan_lut[i] + (atan_lut[i + 1] - atan_lut[i]) * f / 1024;
}

/* Angle of the vector (x, y) in 24.8 degrees, in (-180, 180]. y grows
 * downward in device space, so a positive angle runs clockwise on screen,
 * which is the direction gfx_arc sweeps. (0,0) returns 0. */
int gfx_atan2(int y, int x)
{
    long long ax = x < 0 ? -(long long)x : x, ay = y < 0 ? -(long long)y : y;
    int a;
    if (!ax && !ay) return 0;
    if (ay <= ax) a = atan_unit((ay << 16) / ax);
    else          a = DEG(90) - atan_unit((ax << 16) / ay);
    if (x < 0) a = DEG(180) - a;
    return y < 0 ? -a : a;
}

/* ------------------------------------------------------------------ arc -- */

/* Emits an elliptical arc of centre (cx,cy), radii (rx,ry), whose axes are
 * turned by the 16.16 rotation (rc, rs). `mode`: 0 = gfx_move_to the start,
 * 1 = gfx_line_to it, 2 = the pen is already there. `endpt`, if not NULL,
 * replaces the last piece's endpoint (used by the SVG form so the arc ends
 * exactly on the requested point). */
static void arc_core(struct gfx_path *p, int cx, int cy, int rx, int ry,
                     int rc, int rs, int start, int sweep, int mode,
                     const int *endpt)
{
    const long long lim = 360LL * GFX_ONE;
    long long sw = sweep;
    if (sw > lim) sw = lim;
    if (sw < -lim) sw = -lim;
    long long asw = sw < 0 ? -sw : sw;
    int n = (int)((asw + ARC_PIECE_MAX - 1) / ARC_PIECE_MAX);
    if (n < 1) n = 1;

#define ROTX(lx, ly) (cx + (int)(((long long)rc * (lx) - (long long)rs * (ly)) >> 16))
#define ROTY(lx, ly) (cy + (int)(((long long)rs * (lx) + (long long)rc * (ly)) >> 16))

    int a0 = start;
    int s0 = gfx_sin(a0), c0 = gfx_cos(a0);
    int px = (int)(((long long)rx * c0) >> 16), py = (int)(((long long)ry * s0) >> 16);
    if (mode == 0) gfx_move_to(p, ROTX(px, py), ROTY(px, py));
    else if (mode == 1) gfx_line_to(p, ROTX(px, py), ROTY(px, py));

    for (int i = 0; i < n; i++) {
        int a1 = start + (int)(sw * (i + 1) / n);
        int delta = a1 - a0;
        int s1 = gfx_sin(a1), c1 = gfx_cos(a1);
        int qs = gfx_sin(delta / 4), qc = gfx_cos(delta / 4);
        /* k = 4/3 * tan(delta/4), 16.16 */
        int k = qc ? (int)(4LL * qs * 65536 / (3LL * qc)) : 0;

        int qx = (int)(((long long)rx * c1) >> 16), qy = (int)(((long long)ry * s1) >> 16);
        /* tangent (per radian) at both ends, in the ellipse's own frame */
        int t0x = (int)(-((long long)rx * s0) >> 16), t0y = (int)(((long long)ry * c0) >> 16);
        int t1x = (int)(-((long long)rx * s1) >> 16), t1y = (int)(((long long)ry * c1) >> 16);
        int k0x = px + (int)(((long long)k * t0x) >> 16), k0y = py + (int)(((long long)k * t0y) >> 16);
        int k1x = qx - (int)(((long long)k * t1x) >> 16), k1y = qy - (int)(((long long)k * t1y) >> 16);

        int ex, ey;
        if (endpt && i == n - 1) { ex = endpt[0]; ey = endpt[1]; }
        else { ex = ROTX(qx, qy); ey = ROTY(qx, qy); }
        gfx_cubic_to(p, ROTX(k0x, k0y), ROTY(k0x, k0y),
                        ROTX(k1x, k1y), ROTY(k1x, k1y), ex, ey);
        a0 = a1; s0 = s1; c0 = c1; px = qx; py = qy;
    }
#undef ROTX
#undef ROTY
}

void gfx_arc(struct gfx_path *p, int cx, int cy, int rx, int ry,
             int start_deg24_8, int sweep_deg24_8, int connect)
{
    if (rx <= 0 || ry <= 0) return;
    arc_core(p, cx, cy, rx, ry, 65536, 0, start_deg24_8, sweep_deg24_8,
             connect ? 1 : 0, 0);
}

/* ------------------------------------------------------- SVG endpoint arc --
 * W3C SVG 1.1 appendix F.6.5 (endpoint to centre parameterisation) with F.6.6
 * radius scale-up, in integers. The unit-circle space of the algorithm is held
 * in Q24 (2^-24) so tiny arcs of large radii keep their sweep. */
void gfx_arc_to_svg(struct gfx_path *p, int rx, int ry, int xrot_deg24_8,
                    int large, int sweep, int x, int y)
{
    /* Current point in USER space: the path stores the device point. */
    int x1 = p->cx, y1 = p->cy;
    {
        struct gfx_matrix inv;
        if (gfx_m_invert(&inv, &p->m)) gfx_m_apply(&inv, p->cx, p->cy, &x1, &y1);
        else { gfx_line_to(p, x, y); return; }
    }
    if (x1 == x && y1 == y) return;                     /* F.6.2: omitted   */
    if (rx < 0) rx = -rx;
    if (ry < 0) ry = -ry;
    if (!rx || !ry) { gfx_line_to(p, x, y); return; }   /* F.6.2: a line    */

    int cp = gfx_cos(xrot_deg24_8), sp = gfx_sin(xrot_deg24_8);
    long long dx = (long long)x1 - x, dy = (long long)y1 - y;
    /* (x1', y1') = R(-phi) * (dx, dy)/2 ; these are 2*x1', 2*y1' in 24.24 */
    long long X2 = cp * dx + sp * dy, Y2 = -sp * dx + cp * dy;
    /* normalised by the radii, Q24 */
    long long ux = X2 * 128 / rx, uy = Y2 * 128 / ry;
    /* Keep the squares in 63 bits when the radii are tiny against the chord:
     * only the direction of u and the scale factor matter in that case. */
    int sh = 0;
    while (ux > (1LL << 30) || ux < -(1LL << 30) || uy > (1LL << 30) || uy < -(1LL << 30)) {
        ux >>= 1; uy >>= 1; sh++;
    }
    long long lam = (ux * ux + uy * uy) >> 24;          /* Q24 */
    long long cnx, cny;

    if (sh || lam > (1LL << 24)) {
        /* F.6.6: radii too small -- scale them up so the arc is a half ellipse */
        long long sc = (long long)gfx_isqrt((unsigned long long)lam << 24);   /* Q24 */
        if (sc < 1) sc = 1;
        long long sct = sc << sh;
        rx = (int)(((long long)rx * sct) >> 24);
        ry = (int)(((long long)ry * sct) >> 24);
        ux = (ux << 24) / sc;
        uy = (uy << 24) / sc;
        cnx = cny = 0;
    } else {
        /* |c'| = sqrt(1 - |u|^2), perpendicular to u: c' = +-sqrt(1-|u|^2) * (uy,-ux)/|u|.
         * Written this way (not via 1/|u|^2 - 1) so tiny chords on huge radii keep
         * their precision. */
        long long ulen = (long long)gfx_isqrt((unsigned long long)(ux * ux + uy * uy));   /* Q24 */
        if (ulen == 0) { gfx_line_to(p, x, y); return; }
        long long s1 = (long long)gfx_isqrt((unsigned long long)((1LL << 24) - lam) << 24); /* Q24 */
        long long sg = (large != sweep) ? 1 : -1;
        cnx = sg * (s1 * uy / ulen);
        cny = -sg * (s1 * ux / ulen);
    }

    int th1 = gfx_atan2((int)(uy - cny), (int)(ux - cnx));
    int th2 = gfx_atan2((int)(-uy - cny), (int)(-ux - cnx));
    int dth = th2 - th1;
    while (dth < 0) dth += DEG(360);
    while (dth >= DEG(360)) dth -= DEG(360);
    if (!sweep && dth > 0) dth -= DEG(360);

    /* centre: R(phi) * (rx*cnx, ry*cny) + midpoint */
    long long ccx = ((long long)rx * cnx) >> 24, ccy = ((long long)ry * cny) >> 24;
    int cx = (int)(((cp * ccx - sp * ccy) >> 16) + (((long long)x1 + x) >> 1));
    int cy = (int)(((sp * ccx + cp * ccy) >> 16) + (((long long)y1 + y) >> 1));

    int ep[2]; ep[0] = x; ep[1] = y;
    arc_core(p, cx, cy, rx, ry, cp, sp, th1, dth, 2, ep);
}

/* ----------------------------------------------------------------- sector -- */
void gfx_path_sector(struct gfx_path *p, int cx, int cy, int r_outer, int r_inner,
                     int start, int sweep)
{
    if (r_outer <= 0 || sweep == 0) return;
    if (r_inner < 0) r_inner = 0;
    if (r_inner >= r_outer) return;
    long long asw = sweep < 0 ? -(long long)sweep : sweep;
    if (asw >= DEG(360)) {
        int full = sweep < 0 ? -DEG(360) : DEG(360);
        gfx_arc(p, cx, cy, r_outer, r_outer, start, full, 0);
        gfx_close(p);
        if (r_inner > 0) {          /* the hole winds the other way */
            gfx_arc(p, cx, cy, r_inner, r_inner, start, -full, 0);
            gfx_close(p);
        }
        return;
    }
    gfx_arc(p, cx, cy, r_outer, r_outer, start, sweep, 0);
    if (r_inner > 0) gfx_arc(p, cx, cy, r_inner, r_inner, start + sweep, -sweep, 1);
    else             gfx_line_to(p, cx, cy);
    gfx_close(p);
}

/* ------------------------------------------------- elliptical-corner rrect --
 * Radii order is top-left, top-right, bottom-right, bottom-left (as rrect4).
 * Radii are clamped to half the box per axis and then scaled together by the
 * CSS rule (css-backgrounds 5.5) so two corners on one edge never overlap.
 * When every corner is circular (rx == ry) the call is exactly gfx_path_rrect4,
 * which clamps by min(w,h)/2 instead. */
void gfx_path_rrect_ell(struct gfx_path *p, int x, int y, int w, int h,
                        const int rx[4], const int ry[4])
{
    if (w <= 0 || h <= 0) return;
    if (rx[0] == ry[0] && rx[1] == ry[1] && rx[2] == ry[2] && rx[3] == ry[3]) {
        gfx_path_rrect4(p, x, y, w, h, rx[0], rx[1], rx[2], rx[3]);
        return;
    }
    long long ax[4], ay[4];
    for (int i = 0; i < 4; i++) {
        ax[i] = rx[i] < 0 ? 0 : rx[i];
        ay[i] = ry[i] < 0 ? 0 : ry[i];
        if (ax[i] > w / 2) ax[i] = w / 2;
        if (ay[i] > h / 2) ay[i] = h / 2;
    }
    /* f = min(side / sum of the two radii on it) over the four sides, as num/den */
    long long num = 1, den = 1;
    long long sums[4]  = { ax[0] + ax[1], ax[3] + ax[2], ay[0] + ay[3], ay[1] + ay[2] };
    long long sides[4] = { w, w, h, h };
    for (int i = 0; i < 4; i++)
        if (sums[i] > sides[i] && sides[i] * den < num * sums[i]) { num = sides[i]; den = sums[i]; }
    if (num != den)
        for (int i = 0; i < 4; i++) { ax[i] = ax[i] * num / den; ay[i] = ay[i] * num / den; }

    int Rx[4], Ry[4], Kx[4], Ky[4];
    for (int i = 0; i < 4; i++) {
        Rx[i] = (int)ax[i]; Ry[i] = (int)ay[i];
        Kx[i] = (int)((long long)Rx[i] * KAPPA >> 16);
        Ky[i] = (int)((long long)Ry[i] * KAPPA >> 16);
    }
    gfx_move_to(p, x + Rx[0], y);
    gfx_line_to(p, x + w - Rx[1], y);
    if (Rx[1] && Ry[1]) gfx_cubic_to(p, x + w - Rx[1] + Kx[1], y, x + w, y + Ry[1] - Ky[1],
                                     x + w, y + Ry[1]);
    gfx_line_to(p, x + w, y + h - Ry[2]);
    if (Rx[2] && Ry[2]) gfx_cubic_to(p, x + w, y + h - Ry[2] + Ky[2], x + w - Rx[2] + Kx[2], y + h,
                                     x + w - Rx[2], y + h);
    gfx_line_to(p, x + Rx[3], y + h);
    if (Rx[3] && Ry[3]) gfx_cubic_to(p, x + Rx[3] - Kx[3], y + h, x, y + h - Ry[3] + Ky[3],
                                     x, y + h - Ry[3]);
    gfx_line_to(p, x, y + Ry[0]);
    if (Rx[0] && Ry[0]) gfx_cubic_to(p, x, y + Ry[0] - Ky[0], x + Rx[0] - Kx[0], y,
                                     x + Rx[0], y);
    gfx_close(p);
}

/* --------------------------------------------------------------- squircle --
 * Continuous-curvature corners after Figma's "corner smoothing" (the geometry
 * published as figma-squircle). Each corner is: a cubic that eases the straight
 * edge into curvature, a circular arc of (1 - s) * 90 degrees, and a mirrored
 * cubic back to the next edge. s = 0 is the plain circular corner, s = 1 the
 * fully smoothed one. The corner extent along each edge is p = (1 + s) * r,
 * limited to min(w,h)/2 by reducing s (the radius is kept). */
static void squircle_corner(struct gfx_path *p, int kx, int ky, int e1x, int e1y,
                            int r, int ext, int a, int b, int c, int d,
                            int arc_sec, int m, int th0)
{
    /* local (l1, l2) -> global: K + l1*e1 + l2*e2, with e2 = (-e1y, e1x) */
#define GX(l1, l2) (kx + (l1) * e1x - (l2) * e1y)
#define GY(l1, l2) (ky + (l1) * e1y + (l2) * e1x)
    int s1 = -ext;
    gfx_line_to(p, GX(s1, 0), GY(s1, 0));
    gfx_cubic_to(p, GX(s1 + a, 0), GY(s1 + a, 0),
                    GX(s1 + a + b, 0), GY(s1 + a + b, 0),
                    GX(s1 + a + b + c, d), GY(s1 + a + b + c, d));
    int ex = s1 + a + b + c, ey = d;                    /* arc start, local */
    if (m > 0) {
        int ccx = ex - (int)(((long long)r * gfx_cos(th0)) >> 16);
        int ccy = ey - (int)(((long long)r * gfx_sin(th0)) >> 16);
        arc_core(p, GX(ccx, ccy), GY(ccx, ccy), r, r, e1x * 65536, e1y * 65536,
                 th0, m, 2, 0);
    }
    int fx = ex + arc_sec, fy = ey + arc_sec;           /* arc end, local */
    gfx_cubic_to(p, GX(fx + d, fy + c), GY(fx + d, fy + c),
                    GX(fx + d, fy + b + c), GY(fx + d, fy + b + c),
                    GX(0, ext), GY(0, ext));
#undef GX
#undef GY
}

void gfx_path_squircle(struct gfx_path *p, int x, int y, int w, int h, int r,
                       int smoothing_q8)
{
    if (w <= 0 || h <= 0) return;
    int q = smoothing_q8 < 0 ? 0 : (smoothing_q8 > 256 ? 256 : smoothing_q8);
    int budget = (w < h ? w : h) / 2;
    if (r > budget) r = budget;
    if (q == 0 || r <= 0) { gfx_path_rrect4(p, x, y, w, h, r, r, r, r); return; }

    long long ext = (long long)r * (256 + q) / 256;
    if (ext > budget) {                                 /* spend less smoothing */
        ext = budget;
        q = (int)((long long)budget * 256 / r) - 256;
        if (q <= 0) { gfx_path_rrect4(p, x, y, w, h, r, r, r, r); return; }
    }
    int m = 90 * (256 - q);                             /* arc measure, 24.8 deg */
    int alpha = 45 * q;                                 /* (90 - m)/2            */
    int beta = alpha;                                   /* 45 * s                */
    int arc_sec = (int)((((long long)r * gfx_sin(m / 2)) * SQRT2_Q16) >> 32);
    int ha = gfx_sin(alpha / 2), ca = gfx_cos(alpha / 2);
    int p3p4 = ca ? (int)((long long)r * ha / ca) : 0;  /* r * tan(alpha/2) */
    int c = (int)(((long long)p3p4 * gfx_cos(beta)) >> 16);
    int sb = gfx_sin(beta), cb = gfx_cos(beta);
    int d = cb ? (int)((long long)c * sb / cb) : 0;
    int b = (int)((ext - arc_sec - c - d) / 3);
    int a = 2 * b;
    int th0 = -DEG(45) - m / 2;                         /* arc start angle, local frame */
    int e = (int)ext;

    gfx_move_to(p, x + e, y);                           /* end of the top-left corner */
    squircle_corner(p, x + w, y,     1, 0, r, e, a, b, c, d, arc_sec, m, th0);
    squircle_corner(p, x + w, y + h, 0, 1, r, e, a, b, c, d, arc_sec, m, th0);
    squircle_corner(p, x,     y + h, -1, 0, r, e, a, b, c, d, arc_sec, m, th0);
    squircle_corner(p, x,     y,     0, -1, r, e, a, b, c, d, arc_sec, m, th0);
    gfx_close(p);
}
