/* Open Logit -- stroking: a path in, the OUTLINE of its stroke out.
 *
 * WHAT THIS IS NOT. It is not a second rasterizer and it never touches a
 * pixel. gfx_stroke_path() turns a path into a path; the caller fills the
 * result with the same gfx_fill/gfx_fill_mask it would use for any other
 * shape, NONZERO. That is the whole reason stroking could be a new file and
 * not a second coverage loop -- a fourth rasterizer in this tree is exactly
 * what this engine exists to stop (gfx.h, "WHY IT EXISTS").
 *
 * WHY OFFSETTING THE POLYLINE IS SOUND HERE. Offset curves have no closed
 * form: the offset of a cubic is not a cubic. A stroker therefore either
 * approximates offset curves or flattens first and offsets the polyline.
 * gfx_path.c ALREADY flattens at record time, in DEVICE space, against a
 * device-space tolerance -- so by the time a path reaches this file there is
 * no curve left to offset, and the flattening error is charged where it was
 * chosen (the path's tolerance) rather than to the stroker.
 *
 * THE OUTLINE, per source subpath:
 *   closed -> TWO subpaths wound OPPOSITELY: the offset contour on each side.
 *             NONZERO fills the ring between them and leaves the middle empty.
 *   open   -> ONE subpath: up one side, the end cap, back down the other
 *             side, the start cap. Its two halves are wound oppositely for
 *             the same reason; they are simply joined by the caps.
 * The far side is not a second body of code: the far side of a polyline is
 * the NEAR side of the reversed polyline, so walk_side() runs twice over an
 * index mapping and the opposite winding falls out by construction.
 *
 * WHICH SUBPATHS ARE CLOSED -- READ THIS BEFORE USING IT. `struct gfx_path`
 * does not record closure. gfx_close() emits no point (a fill closes every
 * subpath implicitly, so recording one would add a zero-length edge the
 * winding walk then has to ignore) and it sets no flag, so nothing in the
 * stored path distinguishes the closed triangle A,B,C,close() from the open
 * polyline A,B,C. A FILL does not care -- it closes both. A STROKE cannot not
 * care: one has a join at A, the other has two caps there. So this file uses
 * the only evidence that survives into the storage: a subpath whose LAST
 * POINT EQUALS ITS FIRST is stroked closed, and the duplicate is dropped.
 * That is the ordinary convention for a flattened contour and it costs
 * nothing, but it has one consequence worth knowing before you trust it:
 *
 *   gfx_path_rrect / rrect4 / ellipse / circle all END their last curve
 *   exactly on their first point, so they stroke closed, correctly.
 *   gfx_path_rect does NOT -- it stops at the fourth corner and leaves the
 *   fourth side to gfx_close() -- so a plain rect strokes as an OPEN
 *   four-point polyline, with two caps at its top-left corner instead of a
 *   join. A caller that needs it today writes the closing gfx_line_to itself
 *   (move_to + four line_to's back to the start).
 *
 * The real fix is one line in gfx_path.c or a closure bit in struct gfx_path;
 * both are outside this milestone's files, so it is REPORTED rather than
 * guessed at here. Guessing -- "a 4-point subpath is probably a rect" -- is
 * how a stroker starts drawing plausible wrong pictures.
 *
 * NO LIBC, NO MALLOC, NO STATICS. Everything is caller storage or stack. The
 * dash walker in particular does NOT copy each on-run into a scratch
 * polyline: struct ply below is a VIEW onto the source subpath with its two
 * ends moved, because the buffer that copy would need is sized by
 * GFX_MAX_EDGES and this file compiles into the kernel, where every static is
 * .bss on every boot. The only bytes this file adds are 40 of .rodata (the
 * four quadrant directions used to build a dot).
 *
 * COORDINATES BYPASS out's CTM, deliberately. `src` is already flattened in
 * DEVICE space, so its points are device points; running them through
 * gfx_move_to/gfx_line_to would apply out's matrix a second time. Points are
 * therefore appended raw. A caller that wants a transformed stroke sets the
 * matrix on SRC before building it -- which is also the only order that
 * flattens at the right scale.
 */
#include "gfx.h"

/* Recursion cap for an arc: 2^6 = 64 chords, which covers a quarter turn of a
 * radius-1000 circle at the default tolerance. Past that the chord error
 * exceeds the tolerance rather than the recursion running away. */
#define ARC_DEPTH 6

/* A miter ratio beyond this is clamped before the tip is computed. At 256 the
 * two segments meet at 0.45 degrees; nothing real asks for more, and the
 * clamp is what keeps the tip coordinate inside an int for any plausible
 * width. It is a clamp on a limit, not on geometry: a corner sharper than
 * this still falls back to bevel, which is what the limit means. */
#define MITER_MAX (256 << 16)

/* SVG's default, used when miter_limit is unset. A zero-initialized struct
 * gfx_stroke carries 0, and a caller who wrote GFX_JOIN_MITER and silently
 * got nothing but bevels would be debugging the wrong file. */
#define MITER_DEFAULT (4 << 16)

/* ------------------------------------------------------------- fixed point -- */

/* 24.8 * 16.16 -> 24.8, rounded. Used for every offset point: half-width
 * (24.8) times a unit normal (16.16). */
static int fx(int a, int b)
{
    long long v = (long long)a * b;
    return (int)((v + (1 << 15)) >> 16);
}

/* Length of a 24.8 vector, 24.8, truncated. */
static int veclen(int dx, int dy)
{
    return (int)gfx_isqrt((unsigned long long)((long long)dx * dx + (long long)dy * dy));
}

/* Unit vector in 16.16; 0 if the input is the zero vector.
 *
 * The extra 8 bits of length are not decoration. gfx_isqrt truncates, so a
 * segment of one 24.8 unit in each axis would come back with length 1 instead
 * of 1.414 and the "unit" vector would be 41% too long -- an error in the
 * OFFSET DISTANCE, i.e. in pixels, not in rounding. Squaring the length into
 * 16 extra bits first costs one shift and removes it. */
static int unit(int dx, int dy, int *ux, int *uy)
{
    long long q = (long long)dx * dx + (long long)dy * dy;
    unsigned long long L;
    if (!q) return 0;
    if (q > (1LL << 44)) L = (unsigned long long)gfx_isqrt((unsigned long long)q) << 8;
    else                 L = gfx_isqrt((unsigned long long)(q << 16));
    if (!L) return 0;
    *ux = (int)(((long long)dx << 24) / (long long)L);
    *uy = (int)(((long long)dy << 24) / (long long)L);
    return 1;
}

/* --------------------------------------------------------------- emission -- */
/* Raw appenders. These are gfx_path.c's push_pt/start_sub without the CTM and
 * without the consecutive-duplicate check: our points are already device
 * space (see the file comment), and a duplicate point is a zero-length edge
 * that add_edge() discards anyway -- whereas dropping one here would make the
 * emitted point count depend on the geometry, and the capacity contract
 * ("2x the source points plus join/cap geometry") is what a caller sizes its
 * out-path from. Overflow latches exactly as it does for a built path, so an
 * out-path too small to hold the outline is REFUSED by every consumer instead
 * of being filled as a truncated shape. */

static void emit_pt(struct gfx_path *o, int x, int y)
{
    if (o->npt >= o->ptcap) { o->overflow = 1; return; }
    o->pt[o->npt * 2] = x;
    o->pt[o->npt * 2 + 1] = y;
    o->npt++;
    o->cx = x; o->cy = y;
}

static void emit_sub(struct gfx_path *o, int x, int y)
{
    if (o->nsub >= o->subcap) { o->overflow = 1; return; }
    o->sub[o->nsub++] = o->npt;
    o->sx = x; o->sy = y;
    o->open = 1;
    emit_pt(o, x, y);
}

static void put(struct gfx_path *o, int x, int y, int startsub)
{
    if (startsub) emit_sub(o, x, y);
    else          emit_pt(o, x, y);
}

/* ------------------------------------------------------------------- arcs -- */
/* The INTERIOR points of the arc from unit direction u0 to u1 around (cx,cy)
 * at radius r -- the caller emits the two endpoints, so a join is
 * put(A0); arc_mid(n0,n1); put(A1) with no duplicated point anywhere.
 *
 * Recursive bisection with an EXACT error test and no trigonometry: the
 * midpoint direction of an arc is the normalized sum of its endpoints, and
 * the sagitta -- the deviation of the chord from the arc, which IS the
 * flattening error -- is r * (1 - |u0+u1|/2). So the subdivision test is the
 * path's own tolerance, computed from a length that has to be computed
 * anyway. No angle is ever formed, which is what keeps this integer-only:
 * gfx_sin/gfx_cos exist, but their inverse does not.
 *
 * u0 == -u1 (a half turn) has no bisector and returns immediately; every
 * caller that can produce one splits it first -- see cap_at(). */
static void arc_mid(struct gfx_path *o, int cx, int cy, int r,
                    int u0x, int u0y, int u1x, int u1y, int tol, int depth)
{
    int mx = u0x + u1x, my = u0y + u1y;
    int len = (int)gfx_isqrt((unsigned long long)((long long)mx * mx + (long long)my * my));
    int sag, nx, ny;
    if (len == 0) return;
    sag = r - (int)(((long long)r * len) >> 17);
    if (sag <= tol || depth >= ARC_DEPTH) return;
    /* `* 65536`, not `<< 16`: mx/my are signed direction components and are
     * negative for half the arc. See gfx_raster.c's add_edge for the class. */
    nx = (int)(((long long)mx * 65536) / len);
    ny = (int)(((long long)my * 65536) / len);
    arc_mid(o, cx, cy, r, u0x, u0y, nx, ny, tol, depth + 1);
    emit_pt(o, cx + fx(r, nx), cy + fx(r, ny));
    arc_mid(o, cx, cy, r, nx, ny, u1x, u1y, tol, depth + 1);
}

/* ------------------------------------------------------------ the polyline --
 * A run the stroker walks. It is a VIEW, not a copy: `pt` is the source
 * subpath and the run is the index range [first, first+m) with its two END
 * points replaced by (hx,hy) and (tx,ty). Dashing needs exactly that -- an
 * on-run starts and ends part-way along a segment and is otherwise the source
 * -- and expressing it this way is what lets dashing cost no buffer. `wrap`
 * lets a run started near the end of a CLOSED source carry on through index
 * 0. */
struct ply {
    const int *pt;
    int nsrc;
    int wrap;
    int first;
    int m;
    int closed;              /* walk it as a closed loop (never true when dashed) */
    int hx, hy, tx, ty;
};

static void ply_pt(const struct ply *q, int i, int *x, int *y)
{
    int k;
    if (i <= 0)          { *x = q->hx; *y = q->hy; return; }
    if (i >= q->m - 1)   { *x = q->tx; *y = q->ty; return; }
    k = q->first + i;
    if (q->wrap && k >= q->nsrc) k -= q->nsrc;
    *x = q->pt[k * 2]; *y = q->pt[k * 2 + 1];
}

/* Point i of the walk. `rev` reverses the traversal, which is how the far
 * side of the stroke is produced by the near-side code. */
static void wpt(const struct ply *q, int rev, int i, int *x, int *y)
{
    ply_pt(q, rev ? q->m - 1 - i : i, x, y);
}

/* Unit direction of walk-segment i (point i -> point i+1, wrapping when the
 * run is closed). 0 if the segment has no length -- a dash clip can put the
 * head of a run exactly on a source vertex, and a zero-length segment has no
 * direction to offset along. Such a vertex is SKIPPED rather than joined:
 * there is no corner there. */
static int walk_dir(const struct ply *q, int rev, int i, int *ux, int *uy)
{
    int j = i + 1, ax, ay, bx, by;
    if (j >= q->m) j -= q->m;
    wpt(q, rev, i, &ax, &ay);
    wpt(q, rev, j, &bx, &by);
    return unit(bx - ax, by - ay, ux, uy);
}

/* ------------------------------------------------------------------ joins -- */
/* One join at (vx,vy) between incoming unit direction d0 and outgoing d1, on
 * the side whose normal is perp(d) = (-dy, dx). Emits the incoming segment's
 * offset point, the join geometry if THIS side is the outer one, and the
 * outgoing segment's offset point.
 *
 * WHICH SIDE IS OUTER, and why it decides everything. With n = perp(d), a
 * turn with cross(d0,d1) > 0 puts this side on the INSIDE: the two offset
 * bands overlap there and the contour crosses itself, which NONZERO resolves
 * (the doubled part has winding 2, still "inside"). Only the OUTER side has a
 * wedge missing between the two bands, and only the outer side gets join
 * geometry.
 *
 * Emitting a miter tip on the inner side is the classic bowtie: a spike
 * pointing INTO the shape, which reverses the local winding and punches a
 * hole in the stroke exactly where it should be solid. That is why this is a
 * side test and not a "join every vertex" loop.
 *
 * THE INNER SIDE STILL EMITS THE VERTEX, and this was a real bug before it
 * did. Going straight from A0 to A1 cuts the corner off the inside, and the
 * area cut off is normally handed back by the OTHER end of the following
 * segment's offset -- but only if that segment is long enough to reach past
 * the corner. A segment SHORTER THAN THE HALF-WIDTH (which a dash on-run
 * produces constantly: a run that ends one pixel past a corner) does not
 * reach, and a wedge of the previous segment's own band goes missing. Routing
 * the inner contour through V closes it, and it can never over-fill: every
 * point of the triangle (A0, V, A1) is within h of V and, because the inner
 * side is the side where n1.d0 < 0, projects onto the incoming segment at
 * t <= 1 -- so it is inside that segment's band by definition, area the
 * stroke owns anyway. Collinear vertices are skipped: there A0 == A1 and the
 * detour would be a zero-area spike down to the centreline.
 *
 * cross == 0 with dot < 0 is a CUSP -- the polyline doubles back. The
 * bisector is undefined and the miter tip is at infinity, so miter falls back
 * to bevel there (the limit is exceeded by definition), bevel connects the
 * two antipodal offset points with a straight edge -- which is exactly the
 * end of the doubled band, correct -- and round sweeps a half turn, which is
 * what a round join at a reversal means. Both sides take that branch, so a
 * round cusp comes out as the full disc. */
static void join_at(struct gfx_path *o, int vx, int vy,
                    int d0x, int d0y, int d1x, int d1y,
                    int h, int join, int miter_limit, int tol, int startsub)
{
    int n0x = -d0y, n0y = d0x;
    int n1x = -d1y, n1y = d1x;
    int a0x = vx + fx(h, n0x), a0y = vy + fx(h, n0y);
    int a1x = vx + fx(h, n1x), a1y = vy + fx(h, n1y);
    long long cross = (long long)d0x * d1y - (long long)d0y * d1x;
    long long dot   = (long long)d0x * d1x + (long long)d0y * d1y;
    int outer = (cross < 0) || (cross == 0 && dot < 0);

    put(o, a0x, a0y, startsub);

    if (outer) {
        if (join == GFX_JOIN_ROUND) {
            if (cross == 0 && dot < 0) {
                /* Half turn: no bisector. Split at the incoming direction,
                 * which is the way the tip points, so the two quarter turns
                 * bulge PAST the vertex rather than back over the band. */
                arc_mid(o, vx, vy, h, n0x, n0y, d0x, d0y, tol, 0);
                emit_pt(o, vx + fx(h, d0x), vy + fx(h, d0y));
                arc_mid(o, vx, vy, h, d0x, d0y, n1x, n1y, tol, 0);
            } else {
                arc_mid(o, vx, vy, h, n0x, n0y, n1x, n1y, tol, 0);
            }
        } else if (join == GFX_JOIN_MITER) {
            /* c = n0.n1 = cos(turn). The miter ratio (miter length / stroke
             * width, SVG's definition) is |tip-V|/h = 1/cos(turn/2) =
             * sqrt(2/(1+c)) -- so the limit can be tested WITHOUT forming the
             * tip, which matters because the tip is what overflows as the
             * turn approaches a half turn. In 16.16 that is
             * ratio = sqrt(2^49 / (65536+c)), all inside 64 bits. */
            long long c = ((long long)n0x * n1x + (long long)n0y * n1y) >> 16;
            long long den = 65536 + c;
            if (den > 0) {
                long long ratio = (long long)gfx_isqrt((unsigned long long)((1ULL << 49) / (unsigned long long)den));
                if (ratio <= miter_limit) {
                    long long tx = (long long)h * (n0x + n1x) / den;
                    long long ty = (long long)h * (n0y + n1y) / den;
                    /* Belt and braces on top of the clamped limit: a tip that
                     * cannot be expressed as a device coordinate becomes a
                     * bevel rather than a wrapped integer. */
                    if (tx > -(1 << 27) && tx < (1 << 27) &&
                        ty > -(1 << 27) && ty < (1 << 27))
                        emit_pt(o, vx + (int)tx, vy + (int)ty);
                }
            }
            /* Over the limit, or a cusp: fall through to the bevel, which is
             * just the two offset points already being emitted. THAT IS WHAT
             * miter_limit MEANS -- not "clip the spike", but "use the other
             * join". */
        }
        /* GFX_JOIN_BEVEL: the two offset points and the edge between them. */
    } else if (cross != 0) {
        emit_pt(o, vx, vy);
    }

    emit_pt(o, a1x, a1y);
}

/* ------------------------------------------------------------------- caps -- */
/* The cap at an open end. (cx,cy) is the end point, (dx,dy) the unit
 * direction pointing OUT of the path there, and the walk has just emitted the
 * offset point at +perp(d); the next thing emitted (by the other side's walk)
 * is the offset point at -perp(d). So a cap emits only what goes BETWEEN
 * them, and butt emits nothing at all -- the polygon edge that closes the two
 * offset points IS the butt cap. */
static void cap_at(struct gfx_path *o, int cx, int cy, int dx, int dy,
                   int h, int cap, int tol)
{
    int nx = -dy, ny = dx;
    if (cap == GFX_CAP_ROUND) {
        /* A half turn has no bisector, so it is split at the outward
         * direction -- the same split join_at() uses for a cusp, and for the
         * same reason. */
        arc_mid(o, cx, cy, h, nx, ny, dx, dy, tol, 0);
        emit_pt(o, cx + fx(h, dx), cy + fx(h, dy));
        arc_mid(o, cx, cy, h, dx, dy, -nx, -ny, tol, 0);
    } else if (cap == GFX_CAP_SQUARE) {
        emit_pt(o, cx + fx(h, nx) + fx(h, dx), cy + fx(h, ny) + fx(h, dy));
        emit_pt(o, cx - fx(h, nx) + fx(h, dx), cy - fx(h, ny) + fx(h, dy));
    }
}

/* -------------------------------------------------------------- the walker -- */
/* Emits the offset contour along one side of `q`. Hands back the direction of
 * the LAST segment actually walked: the cap needs it, and only this loop
 * knows which segments were degenerate and skipped. */
static void walk_side(struct gfx_path *o, const struct ply *q, int rev, int h,
                      const struct gfx_stroke *st, int miter_limit, int tol,
                      int startsub, int *lastdx, int *lastdy)
{
    int nseg = q->closed ? q->m : q->m - 1;
    int d0x = 0, d0y = 0, d1x, d1y, vx, vy, i;
    int first = startsub;

    if (nseg < 1) return;

    if (q->closed) {
        /* The incoming direction at vertex 0 is the wrap segment's. Scan
         * backwards for the last non-degenerate one so a run that ends on a
         * repeated point still joins at the seam. */
        for (i = nseg - 1; i >= 0; i--)
            if (walk_dir(q, rev, i, &d0x, &d0y)) break;
        if (i < 0) return;                       /* every segment degenerate */
        for (i = 0; i < nseg; i++) {
            if (!walk_dir(q, rev, i, &d1x, &d1y)) continue;
            wpt(q, rev, i, &vx, &vy);
            join_at(o, vx, vy, d0x, d0y, d1x, d1y, h, st->join, miter_limit,
                    tol, first);
            first = 0;
            d0x = d1x; d0y = d1y;
        }
    } else {
        for (i = 0; i < nseg; i++)
            if (walk_dir(q, rev, i, &d0x, &d0y)) break;
        if (i >= nseg) return;
        wpt(q, rev, i, &vx, &vy);
        put(o, vx + fx(h, -d0y), vy + fx(h, d0x), first);
        for (i++; i < nseg; i++) {
            if (!walk_dir(q, rev, i, &d1x, &d1y)) continue;
            wpt(q, rev, i, &vx, &vy);
            join_at(o, vx, vy, d0x, d0y, d1x, d1y, h, st->join, miter_limit,
                    tol, 0);
            d0x = d1x; d0y = d1y;
        }
        wpt(q, rev, nseg, &vx, &vy);
        emit_pt(o, vx + fx(h, -d0y), vy + fx(h, d0x));
    }
    if (lastdx) { *lastdx = d0x; *lastdy = d0y; }
}

/* --------------------------------------------------------------------- dot -- */
/* A subpath with no length at all. BUTT draws NOTHING -- a butt cap has zero
 * extent along the direction the subpath does not have -- and ROUND draws the
 * full disc of the stroke width, which is the entire reason a dotted line
 * with round caps shows dots. Both are deliberate and both are tested;
 * "nothing" is a result here, not an omission.
 *
 * SQUARE has no defined orientation (there is no direction to align to), so
 * it takes the one stable answer available, axis-aligned, rather than
 * whatever direction happened to be left in a register. */
static const int QUAD[5][2] = {
    { 65536, 0 }, { 0, 65536 }, { -65536, 0 }, { 0, -65536 }, { 65536, 0 }
};

static void emit_dot(struct gfx_path *o, int cx, int cy, int h, int cap, int tol)
{
    int k;
    if (cap == GFX_CAP_ROUND) {
        for (k = 0; k < 4; k++) {
            put(o, cx + fx(h, QUAD[k][0]), cy + fx(h, QUAD[k][1]), k == 0);
            arc_mid(o, cx, cy, h, QUAD[k][0], QUAD[k][1],
                    QUAD[k + 1][0], QUAD[k + 1][1], tol, 0);
        }
    } else if (cap == GFX_CAP_SQUARE) {
        emit_sub(o, cx - h, cy - h);
        emit_pt(o, cx + h, cy - h);
        emit_pt(o, cx + h, cy + h);
        emit_pt(o, cx - h, cy + h);
    }
}

/* --------------------------------------------------------------- one run -- */
/* Stroke one polyline: both sides, plus the caps if it is open. cap0 is the
 * cap at the run's START and cap1 at its END -- separate parameters because a
 * dash on-run wants the requested cap at both ends while the negative control
 * below wants butt at the ends it invents. */
static void stroke_run(struct gfx_path *o, const struct ply *q, int h,
                       const struct gfx_stroke *st, int miter_limit, int tol,
                       int cap0, int cap1)
{
    int hx, hy, tx, ty, ldx = 0, ldy = 0;

    if (o->overflow) return;

    ply_pt(q, 0, &hx, &hy);
    ply_pt(q, q->m - 1, &tx, &ty);

    if (q->m <= 1 || (q->m == 2 && hx == tx && hy == ty)) {
        emit_dot(o, hx, hy, h, cap0, tol);
        return;
    }

#ifdef GFX_NO_JOINS
    /* NEGATIVE CONTROL (-DGFX_NO_JOINS). Every segment is stroked ON ITS OWN,
     * butt-connected: no wedge is filled at a corner, no miter tip is formed,
     * no arc is swept. Every shape still draws and a stroke still looks
     * broadly like a stroke -- which is the point. What breaks is the
     * AGREEMENT WITH THE REFERENCE at every join, and only at joins: caps,
     * dashes and the dot are left working so the failures name the join
     * machinery and nothing else. The target succeeds when the assertions
     * fail. */
    {
        int nseg = q->closed ? q->m : q->m - 1, i;
        if (nseg > 1 || q->closed) {
            for (i = 0; i < nseg; i++) {
                struct ply seg;
                int j = i + 1;
                if (j >= q->m) j -= q->m;
                seg = *q;
                seg.closed = 0;
                seg.wrap = 0;
                seg.m = 2;
                ply_pt(q, i, &seg.hx, &seg.hy);
                ply_pt(q, j, &seg.tx, &seg.ty);
                stroke_run(o, &seg, h, st, miter_limit, tol,
                           (!q->closed && i == 0) ? cap0 : GFX_CAP_BUTT,
                           (!q->closed && i == nseg - 1) ? cap1 : GFX_CAP_BUTT);
            }
            return;
        }
    }
#endif

    if (q->closed) {
        walk_side(o, q, 0, h, st, miter_limit, tol, 1, 0, 0);
        walk_side(o, q, 1, h, st, miter_limit, tol, 1, 0, 0);
        return;
    }

    ldx = ldy = 0;
    walk_side(o, q, 0, h, st, miter_limit, tol, 1, &ldx, &ldy);
    /* ldx==ldy==0 means the walk found no segment with a direction and so
     * emitted no offset point; there is then no pair of points for a cap to
     * go between, and capping anyway would append a lone point to whatever
     * contour happened to be open. */
    if (ldx || ldy) cap_at(o, tx, ty, ldx, ldy, h, cap1, tol);
    ldx = ldy = 0;
    walk_side(o, q, 1, h, st, miter_limit, tol, 0, &ldx, &ldy);
    if (ldx || ldy) cap_at(o, hx, hy, ldx, ldy, h, cap0, tol);
}

/* -------------------------------------------------------------- dashing -- */
/* A dash pattern is validated up front and a malformed one is REFUSED, not
 * quietly stroked solid: a caller that asked for dashes and got a continuous
 * line has a plausible wrong picture, and that is the failure mode this
 * engine is loud about everywhere else. An all-zero pattern additionally
 * cannot be tolerated even if we wanted to -- the walker would never advance
 * along the polyline. */
static int dash_total(const struct gfx_stroke *st, int *total)
{
    long long t = 0;
    int i;
    if (!st->dash) return 1;                          /* NULL = solid */
    if (st->ndash <= 0 || (st->ndash & 1)) return 0;  /* gfx.h: must be even */
    for (i = 0; i < st->ndash; i++) {
        if (st->dash[i] < 0) return 0;
        t += st->dash[i];
    }
    if (t <= 0 || t > 0x7FFFFFFF) return 0;
    *total = (int)t;
    return 1;
}

/* Walk the polyline by arc length against dash[], stroking each on-run as its
 * own OPEN subpath with its own caps. Nothing is copied: a run is handed to
 * stroke_run() as an index range with two moved ends (struct ply). */
static void dash_walk(struct gfx_path *o, const int *pt, int n, int closed,
                      int h, const struct gfx_stroke *st, int miter_limit,
                      int tol, int total)
{
    int nseg = closed ? n : n - 1;
    int idx = 0, on = 1, remain, s;
    int run_first = 0, hx = pt[0], hy = pt[1];
    long long ph;

    /* Advance the pattern by the phase. A negative phase is a legal offset
     * backwards through the pattern, so it is brought into range rather than
     * clamped to zero. */
    ph = (long long)st->dash_phase % total;
    if (ph < 0) ph += total;
    remain = st->dash[0];
    while (ph > 0) {
        if (ph >= remain) {
            ph -= remain;
            idx = (idx + 1) % st->ndash;
            on = !on;
            remain = st->dash[idx];
        } else {
            remain -= (int)ph;
            ph = 0;
        }
    }

    for (s = 0; s < nseg; s++) {
        int a = s, b = s + 1, ax, ay, bx, by, dx, dy, L, pos = 0;
        if (b >= n) b -= n;
        ax = pt[a * 2]; ay = pt[a * 2 + 1];
        bx = pt[b * 2]; by = pt[b * 2 + 1];
        dx = bx - ax; dy = by - ay;
        L = veclen(dx, dy);
        if (L <= 0) continue;
        while (L - pos > remain) {
            int px, py;
            pos += remain;
            px = ax + (int)((long long)dx * pos / L);
            py = ay + (int)((long long)dy * pos / L);
            if (on) {
                struct ply q;
                q.pt = pt; q.nsrc = n; q.wrap = closed;
                q.closed = 0;
                q.first = run_first;
                q.hx = hx; q.hy = hy;
                /* A toggle at pos == 0 ends the run at the START of this
                 * segment, so the tail replaces point `s`, not point `s+1`.
                 * Getting that wrong appends a zero-length final segment,
                 * which is harmless to a fill and puts the end cap on the
                 * wrong point of the outline. */
                if (pos == 0) { q.m = s - run_first + 1; q.tx = ax; q.ty = ay; }
                else          { q.m = s - run_first + 2; q.tx = px; q.ty = py; }
                stroke_run(o, &q, h, st, miter_limit, tol, st->cap, st->cap);
                if (o->overflow) return;
            } else {
                run_first = s; hx = px; hy = py;
            }
            on = !on;
            idx = (idx + 1) % st->ndash;
            remain = st->dash[idx];
        }
        remain -= L - pos;
    }

    if (on) {
        struct ply q;
        q.pt = pt; q.nsrc = n; q.wrap = closed;
        q.closed = 0;
        q.first = run_first;
        q.hx = hx; q.hy = hy;
        q.m = nseg - run_first + 1;
        q.tx = pt[(closed ? 0 : n - 1) * 2];
        q.ty = pt[(closed ? 0 : n - 1) * 2 + 1];
        stroke_run(o, &q, h, st, miter_limit, tol, st->cap, st->cap);
    }
    /* A dashed CLOSED subpath whose pattern is "on" across the seam gets a cap
     * at the start point rather than a join between the last run and the
     * first. Merging them means carrying the first run's geometry until the
     * walk comes back round; it is a known, visible simplification (a dashed
     * circle shows one seam) and it is written down rather than left to be
     * discovered. */
}

/* ---------------------------------------------------------------- entry -- */

int gfx_stroke_path(struct gfx_path *out, const struct gfx_path *src,
                    const struct gfx_stroke *stroke)
{
    int h, tol, total = 0, s, miter_limit;

    if (!out || !src || !stroke) return 0;
    if (out->overflow || src->overflow) return 0;
    if (stroke->width <= 0) return 0;
    /* An out-of-range cap or join is a caller bug, and the two ways to absorb
     * one -- draw butt/bevel, or draw nothing -- are both a picture that does
     * not match what was asked for. Refuse instead. */
    if (stroke->cap < GFX_CAP_BUTT || stroke->cap > GFX_CAP_SQUARE) return 0;
    if (stroke->join < GFX_JOIN_MITER || stroke->join > GFX_JOIN_BEVEL) return 0;
    if (!dash_total(stroke, &total)) return 0;

    /* Round the half-width UP. Rounding down lets a width of one 24.8 unit
     * collapse to h = 0, i.e. an outline with no area, i.e. a stroke that
     * VANISHES -- and a sub-pixel stroke is supposed to survive as reduced
     * coverage, not disappear. The cost is at most 1/512 px of extra width on
     * an odd width. */
    h = (stroke->width + 1) >> 1;

    /* The join arcs are flattened against the tolerance that produced the
     * polyline being offset, so a stroke is exactly as smooth as the path it
     * follows. */
    tol = src->tol > 0 ? src->tol : GFX_ONE / 32;

    miter_limit = stroke->miter_limit > 0 ? stroke->miter_limit : MITER_DEFAULT;
    if (miter_limit > MITER_MAX) miter_limit = MITER_MAX;

    for (s = 0; s < src->nsub; s++) {
        int a = src->sub[s];
        int b = (s + 1 < src->nsub) ? src->sub[s + 1] : src->npt;
        int n = b - a, closed = 0;
        const int *pt = &src->pt[a * 2];
        struct ply q;

        if (n <= 0) continue;
        /* The closure convention -- see the file comment. Three points are
         * required so that a two-point subpath whose ends coincide reads as
         * the degenerate dot it is, not as a closed contour of one point. */
        if (n >= 3 && pt[0] == pt[(n - 1) * 2] && pt[1] == pt[(n - 1) * 2 + 1]) {
            closed = 1;
            n--;
        }

        if (n == 1) {
            /* A dash pattern has no length here to divide into runs, so the
             * dot is emitted by the cap rule regardless of it. */
            emit_dot(out, pt[0], pt[1], h, stroke->cap, tol);
            if (out->overflow) return 0;
            continue;
        }

        if (stroke->dash) {
            dash_walk(out, pt, n, closed, h, stroke, miter_limit, tol, total);
        } else {
            q.pt = pt; q.nsrc = n; q.wrap = closed; q.first = 0; q.m = n;
            q.closed = closed;
            q.hx = pt[0]; q.hy = pt[1];
            q.tx = pt[(n - 1) * 2]; q.ty = pt[(n - 1) * 2 + 1];
            stroke_run(out, &q, h, stroke, miter_limit, tol,
                       stroke->cap, stroke->cap);
        }
        if (out->overflow) return 0;
    }

    /* Every contour this file emits is closed; leave the pen in a state where
     * a caller can carry on building on the same path. */
    out->open = 0;
    return out->overflow ? 0 : 1;
}
