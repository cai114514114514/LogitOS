/* ol-status-unify: status-returning variants of the path overflow latch,
 * ol_raster_fill and the ol_bitmap_* operations.
 *
 * Checks (each prints "ok <name>" or "FAIL <name>"; exit status 1 on any FAIL):
 *   path-ok      a fresh path and a reset path report GFX_OK
 *   distinct     every refusal cause yields its own code: path overflow and
 *                matrix-after-points (gfx_path_status); null target, width 5000,
 *                edges 9000 and an overflowed path (ol_raster_fill_st); an
 *                oversize bitmap and a null target (ol_bitmap_copy_st and
 *                ol_bitmap_reshape_st). Under -DGFX_STATUS_COLLAPSE the limit
 *                refusals report the argument code and the matrix cause reports
 *                the overflow code, so these checks must fail.
 *   first-cause  an overflowed path that later sees a matrix change keeps the
 *                overflow cause
 *   legacy       legacy return values are unchanged: ol_raster_fill returns 0 for
 *                an over-edge or overflowed path and 1 for a wide path clipped to
 *                the 48px surface, ol_bitmap_* keep OL_ARGUMENT for oversize
 *                surfaces, and the overflow flag is still latched
 *   fill-ok      a good fill returns OL_OK, paints, and agrees with the legacy 1
 */
#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "openlogit.h"
#include "openlogit_draw.h"
#include "openlogit_bitmap.h"

static int fails, checks;

/* Not linked with fb.c, so the test defines the context hook itself. */
int (*gfx_ctx_hook)(void);

static void ck(int cond, const char *what, const char *detail)
{
    checks++;
    printf("%s %s%s%s\n", cond ? "ok  " : "FAIL", what, detail ? "  " : "", detail ? detail : "");
    if (!cond) fails++;
}

#define BIG_PTS 9100
static int BPT[BIG_PTS * 2], BSUB[64];
static int SPT[3 * 2], SSUB[4];
static int MPT[16 * 2], MSUB[8];
static unsigned char PX[48 * 48 * 4], PX2[48 * 48 * 4];

/* Coordinates are 24.8 device units: GFX_PX(n) is n whole pixels. */
/* A w x 10 px strip. */
static void path_strip(struct gfx_path *p, int w)
{
    gfx_path_init(p, BPT, BIG_PTS, BSUB, 64);
    gfx_move_to(p, 0, 0);
    gfx_line_to(p, GFX_PX(w), 0);
    gfx_line_to(p, GFX_PX(w), GFX_PX(10));
    gfx_line_to(p, 0, GFX_PX(10));
    gfx_close(p);
}

/* A zig-zag of n segments, every point distinct from its predecessor, 4000 px wide. */
static void path_edges(struct gfx_path *p, int n)
{
    gfx_path_init(p, BPT, BIG_PTS, BSUB, 64);
    gfx_move_to(p, 0, 0);
    for (int i = 1; i <= n; i++) gfx_line_to(p, GFX_PX(i % 4000), (i & 1) * GFX_PX(20));
}

/* Three points of storage: the stored triangle is real, and the fourth point
 * runs out, so the path is truncated and must be refused by the raster. */
static void path_overflow(struct gfx_path *p)
{
    gfx_path_init(p, SPT, 3, SSUB, 4);
    gfx_move_to(p, 0, 0);
    gfx_line_to(p, GFX_PX(10), 0);
    gfx_line_to(p, GFX_PX(10), GFX_PX(10));
    gfx_line_to(p, 0, GFX_PX(10));
    gfx_close(p);
}

/* A matrix set after points exist. */
static void path_matrix_after(struct gfx_path *p)
{
    struct gfx_matrix m;
    gfx_path_init(p, MPT, 16, MSUB, 8);
    gfx_move_to(p, 0, 0);
    gfx_line_to(p, GFX_PX(5), GFX_PX(5));
    gfx_m_identity(&m);
    gfx_path_matrix(p, &m);
}

int main(void)
{
    struct gfx_surface s = { PX, 48, 48, 48 * 4 };
    struct gfx_surface big = { PX, 5000, 2, 5000 * 4 };
    struct gfx_paint paint;
    struct gfx_path p;
    struct gfx_matrix m;

    gfx_paint_solid(&paint, 0x336699, 255);
    gfx_m_identity(&m);
    memset(PX, 0, sizeof PX);

    /* path-ok */
    gfx_path_init(&p, SPT, 3, SSUB, 4);
    ck(gfx_path_status(&p) == GFX_OK, "path-ok", "fresh path");
    path_overflow(&p);
    gfx_path_reset(&p);
    ck(gfx_path_status(&p) == GFX_OK, "path-ok", "after reset");

    /* distinct: path causes */
    path_overflow(&p);
    ck(gfx_path_status(&p) == GFX_E_OVERFLOW, "distinct", "path-overflow");
    path_matrix_after(&p);
    ck(gfx_path_status(&p) == GFX_E_MATRIX_AFTER_POINTS, "distinct", "path-matrix-after-points");

    /* first-cause: the storage cause survives a later matrix change */
    path_overflow(&p);
    gfx_path_matrix(&p, &m);
    ck(gfx_path_status(&p) == GFX_E_OVERFLOW, "first-cause", "overflow then matrix");

    /* distinct: raster causes */
    path_strip(&p, 5000);
    ck(ol_raster_fill_st(&s, &p, GFX_NONZERO, &paint, NULL) == OL_LIMIT, "distinct", "width-5000");
    path_edges(&p, 9000);
    ck(ol_raster_fill_st(&s, &p, GFX_NONZERO, &paint, NULL) == OL_LIMIT, "distinct", "edges-9000");
    path_strip(&p, 20);
    ck(ol_raster_fill_st(NULL, &p, GFX_NONZERO, &paint, NULL) == OL_ARGUMENT, "distinct", "null-target");
    path_overflow(&p);
    ck(ol_raster_fill_st(&s, &p, GFX_NONZERO, &paint, NULL) == OL_ARGUMENT, "distinct", "raster-path-overflow");

    /* distinct: bitmap causes */
    ck(ol_bitmap_copy_st(&s, 0, 0, &big, 0, 0, 4, 1) == OL_LIMIT, "distinct", "bitmap-oversize-copy");
    ck(ol_bitmap_copy_st(NULL, 0, 0, &s, 0, 0, 4, 1) == OL_ARGUMENT, "distinct", "bitmap-null-copy");
    ck(ol_bitmap_reshape_st((uint32_t *)PX, 5000, 1, (const uint32_t *)PX2, 2, 2, 0) == OL_LIMIT,
       "distinct", "bitmap-oversize-reshape");

    /* legacy: return values unchanged */
    path_strip(&p, 5000);
    ck(ol_raster_fill(&s, &p, GFX_NONZERO, &paint, NULL) == 1, "legacy",
       "width-5000 still draws its part inside the 48px surface (returns 1)");
    path_edges(&p, 9000);
    ck(ol_raster_fill(&s, &p, GFX_NONZERO, &paint, NULL) == 0, "legacy", "edges-9000 refused with 0");
    path_overflow(&p);
    ck(ol_raster_fill(&s, &p, GFX_NONZERO, &paint, NULL) == 0 && p.overflow == 1, "legacy",
       "overflowed path refused and still latched");
    ck(ol_bitmap_copy(&s, 0, 0, &big, 0, 0, 4, 1) == OL_ARGUMENT, "legacy", "copy oversize is OL_ARGUMENT");
    ck(ol_bitmap_reshape((uint32_t *)PX, 5000, 1, (const uint32_t *)PX2, 2, 2, 0) == OL_ARGUMENT, "legacy",
       "reshape oversize is OL_ARGUMENT");

    /* fill-ok: a good fill paints and the status and legacy calls agree */
    gfx_surface_clear(&s);
    path_strip(&p, 20);
    ck(ol_raster_fill_st(&s, &p, GFX_NONZERO, &paint, NULL) == OL_OK, "fill-ok", "status");
    ck(s.px[(5 * 48 + 5) * 4 + 3] != 0, "fill-ok", "painted inside the strip");
    gfx_surface_clear(&s);
    ck(ol_raster_fill(&s, &p, GFX_NONZERO, &paint, NULL) == 1, "fill-ok", "legacy returns 1");
    ck(ol_bitmap_copy_st(&s, 0, 0, &s, 0, 0, 4, 1) == OL_OK, "fill-ok", "copy");

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
