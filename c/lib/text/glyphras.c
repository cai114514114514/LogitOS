/* Glyphs onto Open Logit. This file is the bridge that let c/kernel/gui/raster.c
 * be deleted, and it is deliberately small: a converter plus one call.
 *
 * WHAT WAS DELETED. raster.c was 285 lines and a complete second scanline
 * coverage rasterizer -- its own 8.8 fixed point, its own MAXEDGE 8192 edge
 * table, its own heapsort per sub-scanline, its own 16-sub-scanline sweep. It
 * drew every glyph of every label, menu, title and CJK page in this machine, so
 * it was the largest live contradiction of the claim Open Logit is built on:
 * ONE rasterizer for every pixel. The engine's caps were raised to raster.c's
 * OWN production numbers for this moment (GFX_MAX_EDGES 8192 == raster.c's
 * MAXEDGE, GFX_MAX_ACTIVE 1024 == its MAXCROSS) and gfx_fill_mask_subs() takes
 * the sub-scanline count per call, so a glyph keeps its 16 while a button keeps
 * its 4.
 *
 * ONE THING DID HAVE TO CHANGE IN THE ENGINE, and only this migration could
 * have found it: gfx_raster.c accumulated coverage straight into the finished
 * 0..255 byte row, converting each sub-scanline's contribution with an integer
 * divide on the spot -- so the truncation was paid once per sub-scanline and the
 * error GREW with the sub-scanline count. At the shape default of 4 it cost up
 * to 3/255 and nobody noticed in three years; at 16 it cost up to 15/255 on
 * every antialiased pixel of every glyph, and the first measurement of this
 * bridge showed the entire typeface coming out lighter. See g_acc's comment in
 * gfx_raster.c for the numbers and the fix.
 *
 * .BSS ARITHMETIC, because this compiles into the kernel and every byte here is
 * a byte of every boot. Gone with raster.c: edges[8192] 131,072 + path_cmds
 * 114,688 + ol_scratch 262,144 + acc/cross/nedges 7,172 = 515,076 B. Added
 * here: gr_cmds 114,688 + gr_scratch 262,144 + gr_pt[8192*2] 65,536 + gr_sub
 * 2,048 = 444,416 B. Two more places moved with the port: icons.c now owns its
 * own path storage (33,112 B, it used to borrow raster.c's edge table) and
 * gfx_raster.c grew the exact row accumulator this migration forced it to have
 * (8,192 B, see its own comment). Net across all four: -29,356 B, measured with
 * nm on the objects rather than counted by hand.
 *
 * The two big buffers (the fp_cmd list and the glyf point scratch) simply
 * MOVED -- they belong to ttf_glyph_path's contract, not to any rasterizer --
 * and what actually went away is the second edge table.
 *
 * THE ARITHMETIC IS RASTER.C'S, ON PURPOSE. Scaling is per point at full
 * precision, (v * px * 256) / upem, exactly as raster.c's FX/BX/BY did, and the
 * gfx CTM is left at identity. The tempting alternative -- build a 16.16 scale
 * matrix once and let gfx_m_apply do it -- loses about 0.03 px at CJK
 * upem/px combinations, because px*65536/upem is not exact for upem 1000 or
 * 2048 and that rounding is then multiplied by coordinates up to a full em.
 * Per-point exact division has no such error and costs one 64-bit divide per
 * point, on a path that runs once per (glyph, size) and is then cached forever.
 *
 * WHAT IS NOT RASTER.C'S: curve flattening. raster.c used a FIXED segment count
 * per curve (px/8 for quads, px/6 for cubics, clamped) -- so a long sweeping
 * stroke and a two-pixel serif curve both got the same number of segments, one
 * over-tessellated and the other visibly polygonal. gfx flattens adaptively
 * against a device-space tolerance, and that is most of why the replacement
 * beats the thing it replaced against the true geometry: 0.310 mean error
 * versus raster.c's 0.490, worst pixel 16 versus 61 (make test-glyph-agree).
 * Read GR_TOL's comment before trusting the FreeType differential on this
 * particular question -- FreeType quantises control points to 1/64 px, so it is
 * the wrong ruler for sub-pixel curve fidelity and says the opposite.
 *
 * NO ALLOCATOR AND NO LIBC, same rule as the engine: every buffer here is a
 * bounded static, and a glyph that does not fit is REFUSED (-1, the caller
 * draws nothing) rather than truncated into a plausible wrong letter. The
 * budget is not a guess -- see glyphras_peak_points() and the numbers the
 * agreement test prints for it. */
#include "glyphras.h"
#include "fontpath.h"
#include "gfx.h"

/* Sub-scanlines per pixel row FOR TYPE. Not the engine's default of 4, and the
 * difference is measured on this set, not inherited: sweeping it against the
 * oracle in tests/unit/glyph_agree_test.c gives mean error / worst pixel
 *
 *      1  3.521 / 188     4  0.670 / 48     16  0.310 / 16
 *      2  1.464 /  95     8  0.405 / 32     32  0.236 / 10
 *
 * The WORST PIXEL is the column that matters and it is what 16 buys: a thin
 * horizontal bar such as an underscore, whose top and bottom edges both land
 * inside one sub-scanline band, is off by 48/255 at 4 and 16/255 at 16 -- and
 * an underscore is a thing this UI draws. 32 would halve it again for another
 * doubling of vertical passes; 16 is where the worst pixel stops being visible.
 *
 * Text can afford that where a button cannot: the coverage bitmap is cached by
 * c/kernel/gui/text.c per (glyph, size) and reused forever, while a rounded rect
 * is re-rasterized on every frame of a drag. 16 is also exactly what raster.c
 * used (SUB_SHIFT 4, i.e. 16 bands), so the migration changes no sampling. */
#define GR_SUBS     16

/* Flattening tolerance, 24.8 device pixels: the engine's own default, stated
 * rather than inherited so it cannot drift under type without a measurement.
 *
 * raster.c had no tolerance at all -- it used a FIXED segment count per curve
 * (px/8 for quads, px/6 for cubics, clamped), so a long sweeping stroke and a
 * two-pixel serif got the same number of segments. Against the supersampled
 * oracle in tests/unit/glyph_agree_test.c, sweeping this constant over the
 * whole representative set gives mean error and worst-case flattened points:
 *
 *      1/4 px  1.423   446 pts        1/16 px  0.458    795 pts
 *      1/8 px  0.799   589 pts        1/32 px  0.310   1079 pts
 *                                     1/64 px  0.269   1503 pts
 *
 * -- monotonic, as it must be, and raster.c scores 0.490 on the same set. 1/32
 * is where the curve flattens out; 1/64 buys 0.041 for 40% more points. The
 * point column is the worst single glyph in any shipped font at the largest
 * size the glyph cache can serve, which is what the budget below has to clear.
 *
 * A TRAP WORTH RECORDING, because it points the other way. Against FREETYPE
 * (make test-font) the CFF fonts get WORSE as this gets finer: SourceSans3
 * scores 0.74 at 1/8 and 1.49 at 1/32. That is not our error growing. FreeType
 * scales control points onto a 26.6 grid (1/64 px) before it flattens; this
 * path quantises to 1/256. So converging harder on the TRUE curve moves us away
 * from FreeType's slightly different one, most visibly on the all-cubic fonts.
 * FreeType is the third-party check on the whole pipeline and stays; it is not
 * the ground truth for sub-pixel curve fidelity, and the oracle above is. */
#define GR_TOL      (GFX_ONE / 32)

#define GR_MAXCMD   4096              /* drawing commands in one outline      */
#define GR_SCRATCH  (1 << 18)         /* glyf point arrays, while ttf.c converts */
#define GR_MAXSUB   512               /* contours in one outline              */
#define GR_MAXW     768               /* widest glyph bitmap, px              */

/* Flattened device points. Each point is exactly one edge (gfx closes every
 * subpath implicitly), so this cap and GFX_MAX_EDGES are the same number by
 * necessity, not by taste: a path that fits here always fits the engine's edge
 * table, and one that does not fit here would have been refused there anyway --
 * one refusal, at the earlier and cheaper place. */
#define GR_MAXPT    GFX_MAX_EDGES

static struct fp_cmd gr_cmds[GR_MAXCMD];
static uint8_t       gr_scratch[GR_SCRATCH];
static int           gr_pt[GR_MAXPT * 2];
static int           gr_sub[GR_MAXSUB];
static struct gfx_path gr_path;
static int gr_peak, gr_refused;

int glyphras_peak_points(void) { return gr_peak; }
int glyphras_refused(void)     { return gr_refused; }

/* Per-point scaling, font units (y up, origin at the glyph origin) to 24.8
 * device pixels in the BITMAP's own frame (y down, origin at its top-left).
 * `long long` and not `long` because this file is host-compiled by the font
 * suites too, and a 32-bit long would overflow at upem 2048 * px 200 * 256. */
#define FX(v)   ((int)(((long long)(v) * px * 256) / upem))
#define BX(v)   (FX(v) - ox_i * 256)
#define BY(v)   (top_i * 256 - FX(v))

/* Convert one outline into gr_path, flattened in device space by the engine.
 * Returns 0, or -1 if the point budget ran out (gfx latches ->overflow and
 * would refuse the fill anyway -- this just names the reason earlier). */
static int build_path(const struct fp_path *path, int px, int upem,
                      int ox_i, int top_i)
{
    gfx_path_init(&gr_path, gr_pt, GR_MAXPT, gr_sub, GR_MAXSUB);
    gfx_path_tolerance(&gr_path, GR_TOL);        /* CTM stays identity: see the
                                                  * file comment on FX/BX/BY */
    for (int i = 0; i < path->n; i++) {
        const struct fp_cmd *c = &path->cmd[i];
        switch (c->op) {
        case FP_MOVE:
            gfx_move_to(&gr_path, BX(c->x[0]), BY(c->y[0]));
            break;
        case FP_LINE:
            gfx_line_to(&gr_path, BX(c->x[0]), BY(c->y[0]));
            break;
        case FP_QUAD:
            gfx_quad_to(&gr_path, BX(c->x[0]), BY(c->y[0]),
                                  BX(c->x[1]), BY(c->y[1]));
            break;
        case FP_CUBIC:
            gfx_cubic_to(&gr_path, BX(c->x[0]), BY(c->y[0]),
                                   BX(c->x[1]), BY(c->y[1]),
                                   BX(c->x[2]), BY(c->y[2]));
            break;
        case FP_CLOSE:
            gfx_close(&gr_path);
            break;
        }
    }
    if (gr_path.npt > gr_peak) gr_peak = gr_path.npt;
    if (gr_path.overflow) { gr_refused++; return -1; }
    return 0;
}

/* Bitmap placement for a path at `px`: the origin column/top row, plus the
 * width and height that hold it. Returns 0, or -1 for an empty path.
 *
 * Byte-for-byte raster.c's, including the one-pixel margin on each side, and
 * that is not laziness: this box is what the glyph cache stores as (ox, oy) and
 * therefore where every character in the UI lands. Changing it by a pixel would
 * move every label on the screen and every recorded extent in
 * test-desktop-look, for no rendering gain at all. */
static int path_extent(const struct fp_path *path, int px, int upem,
                       int *ox_i, int *top_i, int *w, int *h)
{
    if (path->n == 0 || path->xmax < path->xmin) return -1;
    int fxmin = FX(path->xmin), fxmax = FX(path->xmax);
    int fymin = FX(path->ymin), fymax = FX(path->ymax);
    *ox_i  = (fxmin >> 8) - 1;
    *top_i = (fymax >> 8) + 1;                          /* pixels above baseline */
    *w = ((fxmax + 255) >> 8) - *ox_i + 1;
    *h = *top_i - (fymin >> 8) + 1;
    return 0;
}

int text_raster(const struct ttf_font *f, int gid, int px,
                uint8_t *cov, int covcap, int *wout, int *hout, int *ox, int *oy)
{
    struct fp_path path;
    fp_init(&path, gr_cmds, (int)sizeof gr_cmds);
    if (ttf_glyph_path(f, gid, &path, gr_scratch, sizeof gr_scratch)) return -1;

    int upem = f->units_per_em;
    if (upem <= 0) return -1;
    int ox_i, top_i, w, h;
    if (path_extent(&path, px, upem, &ox_i, &top_i, &w, &h)) {
        *wout = 0; *hout = 0; *ox = 0; *oy = 0; return 0;   /* blank glyph */
    }
    if (w <= 0 || h <= 0) { *wout = 0; *hout = 0; *ox = 0; *oy = 0; return 0; }
    if (w > GR_MAXW || w * h > covcap) return -1;

    if (build_path(&path, px, upem, ox_i, top_i)) return -1;
    if (!gfx_fill_mask_subs(&gr_path, GFX_NONZERO, cov, w, h, 0, 0, GR_SUBS))
        return -1;

    *wout = w; *hout = h; *ox = ox_i; *oy = top_i;
    return 0;
}

int text_raster_at(const struct ttf_font *f, int gid, int px, int ox_i, int top_i,
                   uint8_t *cov, int w, int h)
{
    struct fp_path path;
    fp_init(&path, gr_cmds, (int)sizeof gr_cmds);
    if (ttf_glyph_path(f, gid, &path, gr_scratch, sizeof gr_scratch)) return -1;
    if (path.n == 0) return -1;
    int upem = f->units_per_em;
    if (upem <= 0 || w <= 0 || h <= 0 || w > GR_MAXW) return -1;
    if (build_path(&path, px, upem, ox_i, top_i)) return -1;
    return gfx_fill_mask_subs(&gr_path, GFX_NONZERO, cov, w, h, 0, 0, GR_SUBS)
           ? 0 : -1;
}

int text_raster_extent(const struct ttf_font *f, int gid, int px,
                       int *ox_i, int *top_i, int *w, int *h)
{
    struct fp_path path;
    fp_init(&path, gr_cmds, (int)sizeof gr_cmds);
    if (ttf_glyph_path(f, gid, &path, gr_scratch, sizeof gr_scratch)) return -1;
    int upem = f->units_per_em;
    if (upem <= 0) return -1;
    return path_extent(&path, px, upem, ox_i, top_i, w, h);
}
