/* SYS_GUI_PATH (GUI ABI v2): validated fill or stroke of an app path with a
 * solid, linear or radial paint.
 *
 *   wm2_path(e, &logit_path2, &logit_paint2, &logit_stroke2 or 0)
 *
 * PIPELINE. Everything the app passes is copied into display_scratch(e->d,
 * OL_DS_USER) (a per-display slot, so per context; no statics here) through
 * e->copyin, validated, and only then turned into a gfx_path and drawn
 * through ol_display_fill_path / ol_display_stroke_path on e->d. A malformed
 * call returns its status before the first pixel is written. The display
 * scissor (the surface clip, including fb-clip-stack pushes) is honoured by
 * those functions.
 *
 * SCALE. The device matrix is the app matrix (or identity) times S(pct),
 * applied with gfx_path_matrix BEFORE any point is recorded: the path is
 * flattened in device space, so the flattening tolerance follows the window
 * scale. Gradient geometry goes through the same matrix. With
 * LOGIT_RECT_SNAP in paint.flags every path point (after the app matrix) is
 * converted with wm2_edge24(..., snap = 1), so an integer-point rect covers
 * exactly the wm2_rect24 SNAP edges. Stroke width and dashes are window
 * points scaled by pct only; the app matrix does not scale them. A radial
 * gradient's radius follows the matrix's mean scale (gfx_m_scale_of).
 *
 * STATUS. OL_ARGUMENT: wrong size, copy fault, malformed verbs/points/stops/
 * stroke, undefined flag bits. OL_LIMIT: more than LOGIT_PATH2_MAX_VERBS /
 * _MAX_XY, a matrix outside canvas/state.c's bounds, or the path/stroker ran
 * out of its fixed storage. OL_UNSUPPORTED: conic, op != 0, spread != 0,
 * radial r0 != 0 and 9..16 stops (lifted by later items). A path with no
 * verbs draws nothing and returns OL_OK.
 *
 * -DWM_DRAW2_NO_VALIDATE (the negative control) skips the count limits and the
 * verb-sequence / point-count checks; the builder stays memory-safe on its
 * own, so the unvalidated handler draws rather than crashes. */
#include "wm_draw2.h"
#include "wm2_scale.h"
#include "openlogit_display_path.h"
#include "display_pixels.h"

const unsigned wm2_path_info = LOGIT_GUI2_F_PATH;

#define WP_PT_CAP      8192     /* flattened points of the fill path */
#define WP_SUB_CAP     4096
#define WP_OUT_PT_CAP  16384    /* stroke outline */
#define WP_OUT_SUB_CAP 8192
#define WP_MAX_STOPS   8
#define WP_COORD_MAX   (32768 * 256)       /* +-32768 pt, 24.8 */
#define WP_DEV_MAX     (1 << 30)

#define ALIGN8(n) (((n) + 7ul) & ~7ul)
enum {
    OFF_VERBS = 0,
    OFF_XY    = OFF_VERBS + (LOGIT_PATH2_MAX_VERBS + 7) / 8 * 8,
    OFF_PT    = OFF_XY + LOGIT_PATH2_MAX_XY * 4,
    OFF_SUB   = OFF_PT + WP_PT_CAP * 8,
    OFF_OPT   = OFF_SUB + WP_SUB_CAP * 4,
    OFF_OSUB  = OFF_OPT + WP_OUT_PT_CAP * 8,
    SCRATCH_FILL   = OFF_OPT,
    SCRATCH_STROKE = OFF_OSUB + WP_OUT_SUB_CAP * 4
};

static int matrix_ok(const int *m)
{
    for (int i = 0; i < 4; i++)
        if (m[i] < -64 * GFX_MONE || m[i] > 64 * GFX_MONE) return 0;
    for (int i = 4; i < 6; i++)
        if (m[i] < -WP_COORD_MAX || m[i] > WP_COORD_MAX) return 0;
    return 1;
}

static int scale16(int v, int pct)      /* round(v * pct / 100) */
{
    long long n = (long long)v * pct;
    return wm2_sat(n >= 0 ? (n + 50) / 100 : -((-n + 50) / 100));
}

#ifndef WM_DRAW2_NO_VALIDATE
/* Points needed by a verb, or -1 for an unknown one. */
static int verb_pts(unsigned char v)
{
    switch (v) {
    case LOGIT_VERB_M: case LOGIT_VERB_L: return 1;
    case LOGIT_VERB_Q: return 2;
    case LOGIT_VERB_C: return 3;
    case LOGIT_VERB_Z: return 0;
    }
    return -1;
}

/* M first (and after every Z), known verbs, and exactly nxy/2 points. */
static long check_sequence(const unsigned char *verbs, unsigned nverb, unsigned nxy)
{
    unsigned long pts = 0;
    int need_m = 1;
    for (unsigned i = 0; i < nverb; i++) {
        int n = verb_pts(verbs[i]);
        if (n < 0) return OL_ARGUMENT;
        if (need_m && verbs[i] != LOGIT_VERB_M) return OL_ARGUMENT;
        need_m = verbs[i] == LOGIT_VERB_Z;
        pts += (unsigned)n;
    }
    return pts * 2 == nxy ? OL_OK : OL_ARGUMENT;
}
#endif

struct wp_xf {
    struct gfx_matrix app;   /* the app matrix (identity without the flag) */
    struct gfx_matrix dev;   /* app x S(pct) */
    int pct, snap;
};

static void xf_point(const struct wp_xf *t, int x, int y, int *ox, int *oy)
{
    if (!t->snap) { *ox = x; *oy = y; return; }
    gfx_m_apply(&t->app, x, y, &x, &y);
    *ox = wm2_edge24(x, t->pct, 1);
    *oy = wm2_edge24(y, t->pct, 1);
}

static int take(const int *xy, unsigned nxy, unsigned *xi, const struct wp_xf *t, int *x, int *y)
{
    if (*xi + 2 > nxy) return 0;
    xf_point(t, xy[*xi], xy[*xi + 1], x, y);
    *xi += 2;
    return 1;
}

/* Memory-safe whatever the sequence: a verb whose points ran out stops the build. */
static void build_path(struct gfx_path *gp, const unsigned char *verbs, unsigned nverb,
                       const int *xy, unsigned nxy, const struct wp_xf *t)
{
    unsigned xi = 0;
    for (unsigned i = 0; i < nverb; i++) {
        int x, y, x1, y1, x2, y2;
        switch (verbs[i]) {
        case LOGIT_VERB_M:
            if (!take(xy, nxy, &xi, t, &x, &y)) return;
            gfx_move_to(gp, x, y); break;
        case LOGIT_VERB_L:
            if (!take(xy, nxy, &xi, t, &x, &y)) return;
            gfx_line_to(gp, x, y); break;
        case LOGIT_VERB_Q:
            if (!take(xy, nxy, &xi, t, &x1, &y1) || !take(xy, nxy, &xi, t, &x, &y)) return;
            gfx_quad_to(gp, x1, y1, x, y); break;
        case LOGIT_VERB_C:
            if (!take(xy, nxy, &xi, t, &x1, &y1) || !take(xy, nxy, &xi, t, &x2, &y2) ||
                !take(xy, nxy, &xi, t, &x, &y)) return;
            gfx_cubic_to(gp, x1, y1, x2, y2, x, y); break;
        case LOGIT_VERB_Z:
            gfx_close(gp); break;
        default:
            return;
        }
    }
}

static long check_stroke(const struct logit_stroke2 *s)
{
    if (s->width <= 0 || s->width > WP_COORD_MAX) return OL_ARGUMENT;
    if (s->cap > LOGIT_STROKE2_CAP_SQUARE || s->join > LOGIT_STROKE2_JOIN_BEVEL || s->pad)
        return OL_ARGUMENT;
    if (s->miter < 0 || s->miter > 1000 * GFX_MONE) return OL_ARGUMENT;
    if (s->ndash > 8 || (s->ndash & 1)) return OL_ARGUMENT;
    long long sum = 0;
    for (unsigned i = 0; i < s->ndash; i++) {
        if (s->dash[i] < 0 || s->dash[i] > WP_COORD_MAX) return OL_ARGUMENT;
        sum += s->dash[i];
    }
    if (s->ndash && sum <= 0) return OL_ARGUMENT;
    if (s->dash_phase < -WP_COORD_MAX || s->dash_phase > WP_COORD_MAX) return OL_ARGUMENT;
    return OL_OK;
}

long wm2_path(struct wm2_env *e, long a, long b, long c)
{
    struct logit_path2 path;
    struct logit_paint2 paint;
    struct logit_stroke2 st;
    long rc = wm2_copy_struct(e, &path, a, sizeof path);
    if (rc == OL_OK) rc = wm2_copy_struct(e, &paint, b, sizeof paint);
    if (rc == OL_OK && c) rc = wm2_copy_struct(e, &st, c, sizeof st);
    if (rc != OL_OK) return rc;
    if (!e->d) return OL_STATE;

    /* Flags and enums first: they are cheap and need no memory. */
    if (path.flags & ~LOGIT_PATH_MATRIX) return OL_ARGUMENT;
    if (paint.flags & ~LOGIT_RECT_SNAP) return OL_ARGUMENT;
    if (path.rule != LOGIT_PATH2_NONZERO && path.rule != LOGIT_PATH2_EVENODD) return OL_ARGUMENT;
    if (paint.kind > LOGIT_PAINT2_CONIC || paint.nstop > LOGIT_PAINT2_MAX_STOPS) return OL_ARGUMENT;
    /* Not yet: conic, blend ops, spread modes, 9..16 stops, an inner radius. */
    if (paint.kind == LOGIT_PAINT2_CONIC || paint.op || paint.spread ||
        paint.nstop > WP_MAX_STOPS || (paint.kind == LOGIT_PAINT2_RADIAL && paint.r0))
        return OL_UNSUPPORTED;
    if (c && (rc = check_stroke(&st)) != OL_OK) return rc;

    unsigned nverb = path.nverb, nxy = path.nxy;
#ifndef WM_DRAW2_NO_VALIDATE
    if (nverb > LOGIT_PATH2_MAX_VERBS || nxy > LOGIT_PATH2_MAX_XY) return OL_LIMIT;
#else
    if (nverb > LOGIT_PATH2_MAX_VERBS) nverb = LOGIT_PATH2_MAX_VERBS;
    if (nxy > LOGIT_PATH2_MAX_XY) nxy = LOGIT_PATH2_MAX_XY;
#endif
    if ((nverb && !path.verbs) || (nxy && !path.xy)) return OL_ARGUMENT;

    /* Matrix: canvas/state.c's bounds. */
    struct wp_xf t;
    t.pct = e->scale_pct;
    t.snap = (paint.flags & LOGIT_RECT_SNAP) != 0;
    gfx_m_identity(&t.app);
    if (path.flags & LOGIT_PATH_MATRIX) {
        if (!matrix_ok(path.m)) return OL_LIMIT;
        gfx_m_set(&t.app, path.m[0], path.m[1], path.m[2], path.m[3], path.m[4], path.m[5]);
    }
    t.dev.a = scale16(t.app.a, t.pct); t.dev.b = scale16(t.app.b, t.pct);
    t.dev.c = scale16(t.app.c, t.pct); t.dev.d = scale16(t.app.d, t.pct);
    t.dev.e = wm2_pt24(t.app.e, t.pct); t.dev.f = wm2_pt24(t.app.f, t.pct);
    if (t.pct <= 0) return OL_STATE;

    /* Paint stops. */
    unsigned stop_pos[WP_MAX_STOPS], stop_argb[WP_MAX_STOPS];
    if (paint.kind != LOGIT_PAINT2_SOLID) {
        if (paint.nstop == 0 || !paint.stop_pos || !paint.stop_argb) return OL_ARGUMENT;
        if (e->copyin(stop_pos, paint.stop_pos, paint.nstop * 4ul) < 0 ||
            e->copyin(stop_argb, paint.stop_argb, paint.nstop * 4ul) < 0)
            return OL_ARGUMENT;
        for (unsigned i = 0; i < paint.nstop; i++)
            if (stop_pos[i] > 65536 || (i && stop_pos[i] < stop_pos[i - 1])) return OL_ARGUMENT;
        const int geo[4] = { paint.x0, paint.y0, paint.x1, paint.y1 };
        int bad = 0;
        for (int v = 0; v < 4; v++)     /* geometry within range */
            if (geo[v] < -WP_COORD_MAX || geo[v] > WP_COORD_MAX) bad = 1;
        if (paint.kind == LOGIT_PAINT2_RADIAL &&
            (paint.r1 <= 0 || paint.r1 > WP_COORD_MAX || paint.r0 < 0)) bad = 1;
        if (bad) return OL_ARGUMENT;
    }

    /* Copy-in. */
    unsigned char *base = display_scratch(e->d, OL_DS_USER, c ? SCRATCH_STROKE : SCRATCH_FILL);
    if (!base) return OL_LIMIT;
    unsigned char *verbs = base + OFF_VERBS;
    int *xy = (int *)(void *)(base + OFF_XY);
    if (nverb && e->copyin(verbs, path.verbs, nverb) < 0) return OL_ARGUMENT;
    if (nxy && e->copyin(xy, path.xy, nxy * 4ul) < 0) return OL_ARGUMENT;
#ifndef WM_DRAW2_NO_VALIDATE
    if ((rc = check_sequence(verbs, nverb, nxy)) != OL_OK) return rc;
#endif
    for (unsigned i = 0; i < nxy; i++)
        if (xy[i] < -WP_COORD_MAX || xy[i] > WP_COORD_MAX) return OL_ARGUMENT;
    if (nverb == 0) return OL_OK;

    /* Build the path in device space. */
    struct gfx_path gp;
    gfx_path_init(&gp, (int *)(void *)(base + OFF_PT), WP_PT_CAP,
                  (int *)(void *)(base + OFF_SUB), WP_SUB_CAP);
    if (!t.snap) gfx_path_matrix(&gp, &t.dev);
    build_path(&gp, verbs, nverb, xy, nxy, &t);
    if (gp.overflow) return OL_LIMIT;
    for (int i = 0; i < gp.npt * 2; i++)
        if (gp.pt[i] < -WP_DEV_MAX || gp.pt[i] > WP_DEV_MAX) return OL_ARGUMENT;
    if (gp.npt == 0) return OL_OK;

    /* Paint, in device space. */
    struct gfx_paint pa;
    int kind = paint.kind;
    if (kind != LOGIT_PAINT2_SOLID && paint.nstop == 1) kind = LOGIT_PAINT2_SOLID;
    if (kind == LOGIT_PAINT2_SOLID) {
        unsigned argb = paint.nstop == 1 && paint.kind != LOGIT_PAINT2_SOLID ? stop_argb[0] : paint.argb;
        gfx_paint_solid(&pa, argb & 0xFFFFFFu, (int)(argb >> 24));   /* no marker byte */
    } else {
        int x0, y0, x1, y1;
        gfx_m_apply(&t.dev, paint.x0, paint.y0, &x0, &y0);
        if (kind == LOGIT_PAINT2_LINEAR) {
            gfx_m_apply(&t.dev, paint.x1, paint.y1, &x1, &y1);
            gfx_paint_linear(&pa, x0, y0, x1, y1);
        } else {
            long long s = gfx_m_scale_of(&t.dev);
            long long r = ((long long)paint.r1 * s + GFX_MONE / 2) / GFX_MONE;
            if (r < 1) r = 1;
            gfx_paint_radial(&pa, x0, y0, wm2_sat(r));
        }
        for (unsigned i = 0; i < paint.nstop; i++)
            gfx_paint_stop(&pa, (int)stop_pos[i], stop_argb[i] & 0xFFFFFFu, (int)(stop_argb[i] >> 24));
    }

    if (!c)
        return ol_display_fill_path(e->d, &gp, path.rule == LOGIT_PATH2_EVENODD ? GFX_EVENODD : GFX_NONZERO,
                                    &pa, 255, 0);
    struct gfx_stroke gs;
    int dash[8];
    gs.width = wm2_pt24(st.width, t.pct);
    if (gs.width < 1) gs.width = 1;
    gs.cap = st.cap; gs.join = st.join; gs.miter_limit = st.miter;
    gs.ndash = st.ndash;
    for (unsigned i = 0; i < st.ndash; i++) dash[i] = wm2_pt24(st.dash[i], t.pct);
    gs.dash = st.ndash ? dash : 0;
    gs.dash_phase = wm2_pt24(st.dash_phase, t.pct);
    struct gfx_path outline;
    gfx_path_init(&outline, (int *)(void *)(base + OFF_OPT), WP_OUT_PT_CAP,
                  (int *)(void *)(base + OFF_OSUB), WP_OUT_SUB_CAP);
    return ol_display_stroke_path(e->d, &gp, &gs, &outline, &pa, 255);
}
