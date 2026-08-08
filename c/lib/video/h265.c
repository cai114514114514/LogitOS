/* c/lib/video/h265.c -- the orchestrator: Annex B scanning, NAL dispatch,
 * reference picture sets, the DPB and its output order, tile/wavefront
 * substreams, and the CTU quadtree with everything hanging off it.
 *
 * Where the H.264 decoder next door has a macroblock raster, HEVC has a
 * coding tree: a CTB up to 64x64 splits recursively into coding units, each
 * of which splits again into prediction units for motion and, separately,
 * into a transform tree for residual. Three overlapping subdivisions of the
 * same square, each with its own neighbour rules. Most of the length here is
 * that structure rather than arithmetic; the arithmetic lives in the leaf
 * modules, which have their own tests.
 *
 * Design notes:
 *  - OUTPUT ORDER IS NOT DECODE ORDER. B slices mean a picture may be decoded
 *    several pictures before it is shown, so h265_decode() returns frames out
 *    of a real DPB governed by sps_max_num_reorder_pics, and the picture it
 *    returns is usually not the one it just decoded. This is the one place
 *    the API had to differ from h264.h.
 *  - Reference management is declarative. Instead of H.264's MMCO commands
 *    mutating the DPB, every slice header carries the complete set of
 *    pictures that must be held (8.3.2); anything not named is dropped. Wrong
 *    is easy to spot: the DPB fills or a reference vanishes mid-GOP.
 *  - Neighbour availability goes through the z-scan order table (6.5.2) plus
 *    slice and tile identity, kept per 4x4 block. "Available" excludes
 *    anything not yet decoded, which for prediction units includes other
 *    partitions of the CU being decoded -- the same trap the H.264 decoder
 *    documents for its C neighbour, and HEVC has more shapes for it.
 *  - Reference planes carry H265_PAD replicated border samples so the
 *    interpolation module can read without clamping.
 */
#include <stdlib.h>
#include <string.h>
#include "h265.h"
#include "h265_int.h"

/* A CU/PU/slice trace, compiled in only under -DH265_TRACE and therefore never
 * present in the ring-3 build (mini-libc has no stderr worth writing to, and a
 * decoder should not carry a printf into a process's address space).
 *
 * This exists because it is the only thing that finds a CABAC desynchronisation
 * in reasonable time. A desync does not look like a decode error: the rest of
 * the slice parses into plausible syntax and paints a recognisable, wrong
 * picture. What identifies it is the CABAC BYTE POSITION at each CU compared
 * against a known-good decoder's -- they agree exactly up to the offending
 * element and then part company. tests/h265.mk's `test-h265-trace` prints ours
 * in the format tools/h265trace.sh diffs against a traced ffmpeg. */
#ifdef H265_TRACE
#include <stdio.h>
#define TRACE(...) fprintf(stderr, __VA_ARGS__)
#else
#define TRACE(...) ((void)0)
#endif

/* ============================== small helpers =========================== */
static int diff_poc(int a, int b) { return a - b; }

/* ============================ picture storage =========================== */
static void release_pic(h265dec *d, int i)
{
    pic_t *p = &d->pics[i];
    if (!p->used) return;
    free(p->base);
    free(p->col);
    memset(p, 0, sizeof *p);
}

static int alloc_picture(h265dec *d)
{
    const sps_t *sps = d->cur_sps;
    int slot = -1;
    for (int i = 0; i < H265_MAX_DPB; i++)
        if (!d->pics[i].used) { slot = i; break; }
    if (slot < 0) return H265_ERR_OOM;

    int ly = d->stride_y * (sps->height + 2 * H265_PAD);
    int lc = d->stride_c * (sps->height / 2 + 2 * H265_PAD);
    /* Samples are uint16_t at EVERY bit depth, so one set of code paths
     * serves 8 and 10 bits and the 8-bit gate exercises the same arithmetic
     * the 10-bit streams take. The price is exactly one byte per sample of
     * reference-frame memory; see the note on H265_PAD above. */
    uint16_t *base = (uint16_t *)malloc((size_t)(ly + 2 * lc) * sizeof(uint16_t));
    if (!base) return H265_ERR_OOM;
    memset(base, 0, (size_t)(ly + 2 * lc) * sizeof(uint16_t));

    pic_t *p = &d->pics[slot];
    p->base = base;
    p->y = base + (long)H265_PAD * d->stride_y + H265_PAD;
    p->u = base + ly + (long)H265_PAD * d->stride_c + H265_PAD;
    p->v = base + ly + lc + (long)H265_PAD * d->stride_c + H265_PAD;
    p->stride_y = d->stride_y;
    p->stride_c = d->stride_c;
    p->used = 1;

    /* The collocated motion field, at the 16x16 granularity 8.5.3.2.8 reads
     * it back at. Storing it per 4x4 would be four times the memory for
     * information the spec discards anyway. */
    p->col_w = (sps->width + 15) / 16;
    p->col_h = (sps->height + 15) / 16;
    p->col = (colmv_t *)malloc((size_t)p->col_w * p->col_h * sizeof(colmv_t));
    if (!p->col) { release_pic(d, slot); return H265_ERR_OOM; }
    memset(p->col, 0, (size_t)p->col_w * p->col_h * sizeof(colmv_t));

    d->cur = p;
    return H265_OK;
}

static void border_pad_plane(uint16_t *vis, int stride, int w, int h)
{
    for (int r = 0; r < h; r++) {
        uint16_t *row = vis + (long)r * stride;
        for (int i = 1; i <= H265_PAD; i++) { row[-i] = row[0]; row[w - 1 + i] = row[w - 1]; }
    }
    for (int i = 1; i <= H265_PAD; i++) {
        memcpy(vis - (long)i * stride - H265_PAD, vis - H265_PAD,
               (size_t)(w + 2 * H265_PAD) * sizeof(uint16_t));
        memcpy(vis + (long)(h - 1 + i) * stride - H265_PAD,
               vis + (long)(h - 1) * stride - H265_PAD,
               (size_t)(w + 2 * H265_PAD) * sizeof(uint16_t));
    }
}

/* ========================= geometry: tiles and z-scan ==================== */
static void free_geom(h265dec *d)
{
    free(d->ctb_ts_to_rs); free(d->ctb_rs_to_ts); free(d->ctb_tile_id);
    free(d->zscan);
    d->ctb_ts_to_rs = d->ctb_rs_to_ts = d->ctb_tile_id = 0;
    d->zscan = 0;
    d->geom_valid = 0;
}

static int build_geom(h265dec *d)
{
    const sps_t *sps = d->cur_sps;
    const pps_t *pps = d->cur_pps;
    int cw = sps->ctb_width, ch = sps->ctb_height, n = sps->ctb_count;

    free_geom(d);
    d->ctb_ts_to_rs = (int *)malloc((size_t)n * sizeof(int));
    d->ctb_rs_to_ts = (int *)malloc((size_t)n * sizeof(int));
    d->ctb_tile_id = (int *)malloc((size_t)n * sizeof(int));
    if (!d->ctb_ts_to_rs || !d->ctb_rs_to_ts || !d->ctb_tile_id) return H265_ERR_OOM;

    /* 6.5.1: column and row boundaries. */
    int ncols = pps->num_tile_columns, nrows = pps->num_tile_rows;
    if (pps->uniform_spacing) {
        for (int i = 0; i <= ncols; i++) d->col_bd[i] = i * cw / ncols;
        for (int j = 0; j <= nrows; j++) d->row_bd[j] = j * ch / nrows;
    } else {
        d->col_bd[0] = 0;
        for (int i = 0; i < ncols; i++) d->col_bd[i + 1] = d->col_bd[i] + pps->column_width[i];
        d->row_bd[0] = 0;
        for (int j = 0; j < nrows; j++) d->row_bd[j + 1] = d->row_bd[j] + pps->row_height[j];
    }

    /* CtbAddrRsToTs (6.5.1) */
    for (int rs = 0; rs < n; rs++) {
        int tbx = rs % cw, tby = rs / cw;
        int tx = 0, ty = 0;
        for (int i = 0; i < ncols; i++) if (tbx >= d->col_bd[i]) tx = i;
        for (int j = 0; j < nrows; j++) if (tby >= d->row_bd[j]) ty = j;
        int ts = 0;
        for (int i = 0; i < tx; i++) ts += (d->row_bd[ty + 1] - d->row_bd[ty]) *
                                           (d->col_bd[i + 1] - d->col_bd[i]);
        for (int j = 0; j < ty; j++) ts += cw * (d->row_bd[j + 1] - d->row_bd[j]);
        ts += (tby - d->row_bd[ty]) * (d->col_bd[tx + 1] - d->col_bd[tx]) +
              (tbx - d->col_bd[tx]);
        d->ctb_rs_to_ts[rs] = ts;
        d->ctb_ts_to_rs[ts] = rs;
        d->ctb_tile_id[ts] = ty * ncols + tx;
    }

    /* MinTbAddrZs (6.5.2), always at 4x4 granularity: the z-order of 4x4
     * blocks refines the z-order of any larger minimum transform block, so
     * comparisons come out the same and the table needs no size parameter. */
    int sh = sps->log2_ctb - 2;
    d->zw = cw << sh;
    d->zh = ch << sh;
    d->zscan = (int *)malloc((size_t)d->zw * d->zh * sizeof(int));
    if (!d->zscan) return H265_ERR_OOM;
    for (int y = 0; y < d->zh; y++)
        for (int x = 0; x < d->zw; x++) {
            int rs = (y >> sh) * cw + (x >> sh);
            int v = d->ctb_rs_to_ts[rs] << (sh * 2);
            int p = 0;
            for (int i = 0; i < sh; i++) {
                int m = 1 << i;
                p += ((m & x) ? m * m : 0) + ((m & y) ? 2 * m * m : 0);
            }
            d->zscan[y * d->zw + x] = v + p;
        }
    d->geom_valid = 1;
    return H265_OK;
}

/* ========================== availability (6.4.1) ======================== */
static int zorder(const h265dec *d, int x, int y)
{
    return d->zscan[(y >> 2) * d->zw + (x >> 2)];
}

/* Is (xn,yn) available as a neighbour of the block at (xc,yc)? Both in luma
 * samples. Decoded-yet, same slice, same tile. */
static int avail_z(h265dec *d, int xc, int yc, int xn, int yn)
{
    if (xn < 0 || yn < 0 || xn >= d->cur_sps->width || yn >= d->cur_sps->height)
        return 0;
    if (zorder(d, xn, yn) > zorder(d, xc, yc)) return 0;
    const bi_t *bn = h265_bi(d, xn, yn);
    if (bn->slice_idx != (uint16_t)d->slice_idx) return 0;
    if (bn->tile_idx != (uint16_t)d->cur_tile) return 0;
    return 1;
}

/* 6.4.2, prediction block availability: the extra rule is that the second
 * partition of an NxN split may not look into a partition of the same CU that
 * is decoded later. */
static int avail_pb(h265dec *d, int xcb, int ycb, int ncbs, int xpb, int ypb,
                    int npbw, int npbh, int part_idx, int xn, int yn)
{
    int same_cb = (xcb <= xn && xn < xcb + ncbs && ycb <= yn && yn < ycb + ncbs);
    int a;
    if (!same_cb) {
        a = avail_z(d, xpb, ypb, xn, yn);
    } else if ((npbw << 1) == ncbs && (npbh << 1) == ncbs && part_idx == 1 &&
               (ycb + npbh) <= yn && (xcb + npbw) > xn) {
        a = 0;
    } else {
        a = 1;
    }
    if (a && h265_bi(d, xn, yn)->pred_mode == MODE_INTRA) a = 0;
    return a;
}

/* ================================ POC / RPS ============================= */
static int compute_poc(h265dec *d, const slice_t *sl, int nal_type)
{
    int maxlsb = d->max_poc_lsb;
    if (IS_IDR(nal_type)) return 0;
    int no_rasl_output = IS_IRAP(nal_type) && (IS_IDR(nal_type) || IS_BLA(nal_type) ||
                                               d->first_picture);
    int msb;
    if (no_rasl_output) {
        msb = 0;
    } else {
        int lsb = sl->poc_lsb;
        if (lsb < d->prev_poc_lsb && (d->prev_poc_lsb - lsb) >= maxlsb / 2)
            msb = d->prev_poc_msb + maxlsb;
        else if (lsb > d->prev_poc_lsb && (lsb - d->prev_poc_lsb) > maxlsb / 2)
            msb = d->prev_poc_msb - maxlsb;
        else
            msb = d->prev_poc_msb;
    }
    return msb + sl->poc_lsb;
}

static pic_t *find_poc(h265dec *d, int poc, int lsb_only)
{
    for (int i = 0; i < H265_MAX_DPB; i++) {
        pic_t *p = &d->pics[i];
        if (!p->used || p == d->cur || !p->reference) continue;
        if (lsb_only) {
            if ((p->poc & (d->max_poc_lsb - 1)) == poc) return p;
        } else if (p->poc == poc) {
            return p;
        }
    }
    return 0;
}

/* 8.3.2: apply the slice's reference picture set. Every reference not named
 * becomes unused; the named ones are (re)marked short- or long-term. */
static int apply_rps(h265dec *d, const slice_t *sl, int nal_type)
{
    pic_t *keep[H265_MAX_DPB * 2];
    int nkeep = 0;

    if (IS_IDR(nal_type) || IS_BLA(nal_type) ||
        (IS_IRAP(nal_type) && d->first_picture)) {
        for (int i = 0; i < H265_MAX_DPB; i++)
            if (d->pics[i].used && &d->pics[i] != d->cur) d->pics[i].reference = 0;
        d->nb_refs[0] = d->nb_refs[1] = 0;
        return H265_OK;
    }

    /* Short-term entries. */
    pic_t *st_before[H265_MAX_REFS], *st_after[H265_MAX_REFS], *lt_curr[32];
    int n_before = 0, n_after = 0, n_lt = 0;
    const strps_t *r = &sl->rps;
    for (int i = 0; i < r->num_negative; i++) {
        pic_t *p = find_poc(d, d->poc + r->delta_poc[i], 0);
        if (p) { p->reference = 1; keep[nkeep++] = p; }
        if (r->used[i]) st_before[n_before++] = p;
    }
    for (int i = 0; i < r->num_positive; i++) {
        int k = r->num_negative + i;
        pic_t *p = find_poc(d, d->poc + r->delta_poc[k], 0);
        if (p) { p->reference = 1; keep[nkeep++] = p; }
        if (r->used[k]) st_after[n_after++] = p;
    }
    /* Long-term entries: matched by full POC when the msb was sent, by LSB
     * otherwise (7.4.7.1 / 8.3.2). */
    for (int i = 0; i < sl->num_long_term; i++) {
        int poc = sl->lt_poc[i];
        pic_t *p;
        if (sl->lt_msb_present[i]) {
            poc = d->poc - sl->lt_msb_cycle[i] * d->max_poc_lsb -
                  (d->poc & (d->max_poc_lsb - 1)) + sl->lt_poc[i];
            p = find_poc(d, poc, 0);
        } else {
            p = find_poc(d, poc & (d->max_poc_lsb - 1), 1);
        }
        if (p) { p->reference = 2; keep[nkeep++] = p; }
        if (sl->lt_used[i]) lt_curr[n_lt++] = p;
    }

    for (int i = 0; i < H265_MAX_DPB; i++) {
        pic_t *p = &d->pics[i];
        if (!p->used || p == d->cur || !p->reference) continue;
        int found = 0;
        for (int k = 0; k < nkeep; k++) if (keep[k] == p) { found = 1; break; }
        if (!found) p->reference = 0;
    }

    /* 8.3.4: build the two reference picture lists. */
    int ntotal = n_before + n_after + n_lt;
    if (ntotal == 0) {
        d->nb_refs[0] = d->nb_refs[1] = 0;
        return (sl->type == SLICE_I) ? H265_OK : H265_ERR_CORRUPT;
    }
    for (int l = 0; l < 2; l++) {
        if (l == 1 && sl->type != SLICE_B) { d->nb_refs[1] = 0; continue; }
        pic_t *tmp[3 * H265_MAX_REFS + 32];
        int nt = 0;
        while (nt < sl->num_ref_idx[l] + ntotal) {
            if (l == 0) {
                for (int i = 0; i < n_before && nt < (int)(sizeof tmp / sizeof *tmp); i++) tmp[nt++] = st_before[i];
                for (int i = 0; i < n_after && nt < (int)(sizeof tmp / sizeof *tmp); i++) tmp[nt++] = st_after[i];
            } else {
                for (int i = 0; i < n_after && nt < (int)(sizeof tmp / sizeof *tmp); i++) tmp[nt++] = st_after[i];
                for (int i = 0; i < n_before && nt < (int)(sizeof tmp / sizeof *tmp); i++) tmp[nt++] = st_before[i];
            }
            for (int i = 0; i < n_lt && nt < (int)(sizeof tmp / sizeof *tmp); i++) tmp[nt++] = lt_curr[i];
            if (nt >= sl->num_ref_idx[l]) break;
        }
        int n = sl->num_ref_idx[l];
        if (n > H265_MAX_REFS) return H265_ERR_CORRUPT;
        for (int i = 0; i < n; i++) {
            int src = sl->ref_list_modification_flag[l] ? sl->list_entry[l][i] : i;
            if (src >= nt) return H265_ERR_CORRUPT;
            pic_t *p = tmp[src];
            if (!p) return H265_ERR_CORRUPT;    /* a named reference is missing */
            d->ref_list[l][i] = p;
            d->ref_poc_list[l][i] = p->poc;
            d->ref_islt[l][i] = (uint8_t)(p->reference == 2);
        }
        d->nb_refs[l] = n;
    }
    return H265_OK;
}

/* ============================== DPB output ============================== */
static void queue_output(h265dec *d)
{
    /* Bump while more pictures are waiting than the SPS allows to be
     * reordered, or the buffer is full. Smallest POC first. */
    const sps_t *sps = d->cur_sps;
    int reorder = sps->max_num_reorder[sps->max_sub_layers - 1];
    int maxdpb = sps->max_dec_pic_buffering[sps->max_sub_layers - 1];
    for (;;) {
        int waiting = 0, used = 0;
        for (int i = 0; i < H265_MAX_DPB; i++) {
            if (!d->pics[i].used) continue;
            used++;
            if (d->pics[i].output) waiting++;
        }
        if (waiting == 0) break;
        if (waiting <= reorder && used < maxdpb + 1) break;
        int best = -1;
        for (int i = 0; i < H265_MAX_DPB; i++)
            if (d->pics[i].used && d->pics[i].output &&
                (best < 0 || d->pics[i].poc < d->pics[best].poc)) best = i;
        if (best < 0) break;
        d->pics[best].output = 0;
        if (d->n_outq < H265_MAX_DPB) d->out_queue[d->n_outq++] = best;
        else break;
    }
    /* Anything neither referenced nor waiting is free. */
    for (int i = 0; i < H265_MAX_DPB; i++) {
        int queued = 0;
        for (int k = 0; k < d->n_outq; k++) if (d->out_queue[k] == i) queued = 1;
        if (d->pics[i].used && !d->pics[i].reference && !d->pics[i].output &&
            !queued && &d->pics[i] != d->cur)
            release_pic(d, i);
    }
}

static void flush_all_output(h265dec *d)
{
    for (;;) {
        int best = -1;
        for (int i = 0; i < H265_MAX_DPB; i++)
            if (d->pics[i].used && d->pics[i].output &&
                (best < 0 || d->pics[i].poc < d->pics[best].poc)) best = i;
        if (best < 0) break;
        d->pics[best].output = 0;
        if (d->n_outq < H265_MAX_DPB) d->out_queue[d->n_outq++] = best;
        else break;
    }
}

/* The 8-bit display view of h265frame. It exists so that widening the decoder
 * to 10 bits does not change the type of a field an existing caller already
 * reads, and so that a caller who wants exactness has to ask for y16 by name
 * rather than get a quietly truncated picture and a passing test.
 *
 * At 8 bits this is a straight copy and is exact. Above 8 bits it is a rounded
 * down-conversion and is LOSSY -- which is a renderer's business, never a
 * decode result: nothing in the decoder ever reads these planes back, and no
 * bit-exactness check in the test suite looks at them.
 *
 * One buffer, reused for every output picture, matching the documented "valid
 * until the next call" lifetime of the 16-bit planes it shadows. */
static int build_display(h265dec *d, h265frame *out)
{
    int sh = out->bit_depth - 8;
    int rnd = sh ? (1 << (sh - 1)) : 0;
    int hc = (out->height + 1) / 2;
    long need = (long)out->stride_y * out->height +
                2L * out->stride_c * hc;
    if (need > d->disp_sz) {
        free(d->disp);
        d->disp = (uint8_t *)malloc((size_t)need);
        if (!d->disp) { d->disp_sz = 0; return H265_ERR_OOM; }
        d->disp_sz = need;
    }
    uint8_t *dy = d->disp;
    uint8_t *du = dy + (long)out->stride_y * out->height;
    uint8_t *dv = du + (long)out->stride_c * hc;

    const uint16_t *sp[3] = { out->y16, out->u16, out->v16 };
    uint8_t *dp[3] = { dy, du, dv };
    int sw[3] = { out->width, (out->width + 1) / 2, (out->width + 1) / 2 };
    int shh[3] = { out->height, hc, hc };
    int st[3] = { out->stride_y, out->stride_c, out->stride_c };
    for (int c = 0; c < 3; c++)
        for (int y = 0; y < shh[c]; y++) {
            const uint16_t *r = sp[c] + (long)y * st[c];
            uint8_t *w = dp[c] + (long)y * st[c];
            if (!sh) { for (int x = 0; x < sw[c]; x++) w[x] = (uint8_t)r[x]; }
            else     { for (int x = 0; x < sw[c]; x++) {
                           int v = (r[x] + rnd) >> sh;
                           w[x] = (uint8_t)(v > 255 ? 255 : v);
                       } }
        }
    out->y = dy; out->u = du; out->v = dv;
    return H265_OK;
}

static int emit(h265dec *d, h265frame *out)
{
    if (d->to_free >= 0) { release_pic(d, d->to_free); d->to_free = -1; }
    if (d->n_outq == 0) return 0;
    int slot = d->out_queue[0];
    for (int i = 1; i < d->n_outq; i++) d->out_queue[i - 1] = d->out_queue[i];
    d->n_outq--;
    pic_t *p = &d->pics[slot];
    const sps_t *sps = d->cur_sps;
    int cl = sps->conf_win[0] * 2, cr = sps->conf_win[1] * 2;
    int ct = sps->conf_win[2] * 2, cb = sps->conf_win[3] * 2;
    out->width = sps->width - cl - cr;
    out->height = sps->height - ct - cb;
    out->stride_y = p->stride_y;
    out->stride_c = p->stride_c;
    out->bit_depth = sps->bit_depth_luma;
    out->y16 = p->y + (long)ct * p->stride_y + cl;
    out->u16 = p->u + (long)(ct / 2) * p->stride_c + cl / 2;
    out->v16 = p->v + (long)(ct / 2) * p->stride_c + cl / 2;
    out->poc = p->poc;
    if (build_display(d, out) != H265_OK) return H265_ERR_OOM;
    /* The slot may now be dead, but the caller is holding pointers into it;
     * the H.264 decoder makes the same promise -- valid until the next call. */
    if (!p->reference) d->to_free = slot;
    return 1;
}

/* ======================= CABAC syntax element helpers =================== */
#define DEC(c) h265_cabac_decision(&d->cabac, (c))
#define BYP()  h265_cabac_bypass(&d->cabac)

static int decode_tr_bypass(h265dec *d, int cmax)   /* TR, all bins bypass */
{
    int v = 0;
    while (v < cmax && BYP()) v++;
    return v;
}

static int decode_eg(h265dec *d, int k)             /* EGk, all bins bypass */
{
    int v = 0;
    while (BYP()) { v += 1 << k; k++; if (k > 30) { d->cabac.error = 1; return 0; } }
    return v + (int)h265_cabac_bypass_n(&d->cabac, k);
}

/* ============================== SAO parsing ============================= */
static void parse_sao(h265dec *d, int rx, int ry)
{
    const sps_t *sps = d->cur_sps;
    const slice_t *sl = &d->sh;
    int ctb = ry * sps->ctb_width + rx;
    sao_t *s = &d->sao[ctb];
    memset(s, 0, sizeof *s);

    int merge_left = 0, merge_up = 0;
    if (rx > 0) {
        int left_in_seg = d->ctb_addr_rs > d->seg_addr_rs;
        int left_in_tile = d->ctb_tile_id[d->ctb_addr_ts] ==
                           d->ctb_tile_id[d->ctb_rs_to_ts[d->ctb_addr_rs - 1]];
        if (left_in_seg && left_in_tile) merge_left = DEC(CTX_SAO_MERGE);
    }
    if (ry > 0 && !merge_left) {
        int up_in_seg = (d->ctb_addr_rs - sps->ctb_width) >= d->seg_addr_rs;
        int up_in_tile = d->ctb_tile_id[d->ctb_addr_ts] ==
                         d->ctb_tile_id[d->ctb_rs_to_ts[d->ctb_addr_rs - sps->ctb_width]];
        if (up_in_seg && up_in_tile) merge_up = DEC(CTX_SAO_MERGE);
    }
    if (merge_left) { *s = d->sao[ctb - 1]; return; }
    if (merge_up)   { *s = d->sao[ctb - sps->ctb_width]; return; }

    for (int c = 0; c < 3; c++) {
        if (c == 0 && !sl->sao_luma) continue;
        if (c > 0 && !sl->sao_chroma) continue;
        if (c == 2) {
            /* Cr shares Cb's type and class; only the offsets are its own. */
            s->type[2] = s->type[1];
            s->clsx[2] = s->clsx[1];
        } else {
            int t = 0;
            if (DEC(CTX_SAO_TYPE_IDX)) t = BYP() ? 2 : 1;
            s->type[c] = (uint8_t)t;
        }
        if (s->type[c] == SAO_NOT_APPLIED) continue;

        /* 9.3.3.2 with Table 9-38: sao_offset_abs is TR-binarised with
         * cMax = (1 << (Min(bitDepth, 10) - 5)) - 1, i.e. 7 at 8 bits and 31
         * at 10. This is the ONLY bit-depth dependency on the PARSE side of
         * the whole decoder, and it is the expensive kind: too small a cMax
         * stops reading bins early, so the arithmetic decoder desynchronises
         * and everything after it in the slice is garbage. It shows up as a
         * corrupt-stream error several pictures later rather than as a wrong
         * pixel, which is why it is worth naming here. */
        int bd = c ? sps->bit_depth_chroma : sps->bit_depth_luma;
        int cmax = (1 << ((bd < 10 ? bd : 10) - 5)) - 1;
        int abs_off[4];
        for (int i = 0; i < 4; i++) abs_off[i] = decode_tr_bypass(d, cmax);
        if (s->type[c] == SAO_BAND) {
            for (int i = 0; i < 4; i++) {
                int sign = 0;
                if (abs_off[i]) sign = BYP();
                s->off[c][i] = (int8_t)(sign ? -abs_off[i] : abs_off[i]);
            }
            s->clsx[c] = (uint8_t)h265_cabac_bypass_n(&d->cabac, 5);
        } else {
            /* Edge offsets: the first two are positive and the last two
             * negative by definition, no sign is coded. */
            for (int i = 0; i < 4; i++)
                s->off[c][i] = (int8_t)(i < 2 ? abs_off[i] : -abs_off[i]);
            if (c < 2) s->clsx[c] = (uint8_t)h265_cabac_bypass_n(&d->cabac, 2);
            if (c == 1) s->clsx[2] = s->clsx[1];
        }
    }
}

/* ============================== block state ============================= */
static void fill_bi(h265dec *d, int x, int y, int w, int h,
                    const bi_t *tpl, int fields)
{
    /* fields: 1 = prediction/mode, 2 = qp, 4 = cbf_luma */
    for (int j = y; j < y + h && j < d->cur_sps->height; j += 4)
        for (int i = x; i < x + w && i < d->cur_sps->width; i += 4) {
            bi_t *b = h265_bi(d, i, j);
            if (fields & 1) {
                b->pred_mode = tpl->pred_mode;
                b->skip = tpl->skip;
                b->intra_mode = tpl->intra_mode;
                b->ct_depth = tpl->ct_depth;
                b->pred_flag = tpl->pred_flag;
                b->bypass = tpl->bypass;
                b->pcm = tpl->pcm;
                b->ref_idx[0] = tpl->ref_idx[0];
                b->ref_idx[1] = tpl->ref_idx[1];
                b->mv[0][0] = tpl->mv[0][0]; b->mv[0][1] = tpl->mv[0][1];
                b->mv[1][0] = tpl->mv[1][0]; b->mv[1][1] = tpl->mv[1][1];
                b->refpoc[0] = tpl->refpoc[0]; b->refpoc[1] = tpl->refpoc[1];
                b->islt[0] = tpl->islt[0]; b->islt[1] = tpl->islt[1];
                b->slice_idx = (uint16_t)d->slice_idx;
                b->tile_idx = (uint16_t)d->cur_tile;
            }
            if (fields & 2) b->qp = tpl->qp;
            if (fields & 4) b->cbf_luma = tpl->cbf_luma;
        }
}

/* Mark the left/top edges of a block as filter boundaries. `tu` also sets the
 * transform-edge bits, which are what let a non-zero cbf raise bS to 1. */
static void mark_edges(h265dec *d, int x, int y, int w, int h, int tu)
{
    int W = d->cur_sps->width, H = d->cur_sps->height;
    for (int j = y; j < y + h && j < H; j += 4)
        if (x < W) h265_bi(d, x, j)->bnd |= (uint8_t)(1 | (tu ? 4 : 0));
    for (int i = x; i < x + w && i < W; i += 4)
        if (y < H) h265_bi(d, i, y)->bnd |= (uint8_t)(2 | (tu ? 8 : 0));
}

/* ============================ QP derivation ============================= */
int h265_chroma_qp(int qpi);           /* h265_deblock.c, Table 8-10 */

/* 8.6.1. qPY_PREV is the QP of the last CU of the PREVIOUS quantisation
 * group, reset to SliceQpY at the start of a slice, a tile, or (under
 * wavefront sync) a CTB row. qPY_A and qPY_B fall back to it unless the
 * neighbour is available AND inside the same CTB -- the same-CTB condition is
 * easy to miss and shows up only as slightly wrong deblocking strength. */
static void derive_qp(h265dec *d)
{
    const sps_t *sps = d->cur_sps;
    int qg_x = d->qg_x, qg_y = d->qg_y;
    int ctb_mask = ~((1 << sps->log2_ctb) - 1);

    /* The same-CTB test is one comparison per neighbour, not two: qPY_A sits at
     * (xQg-1, yQg), so only its x can leave the CTB, and qPY_B at (xQg, yQg-1)
     * only its y. Writing both halves made the other half a self-comparison,
     * which is what -Wtautological-compare was pointing at. */
    int qp_a = d->qp_y_prev, qp_b = d->qp_y_prev;
    if (qg_x > 0 && avail_z(d, d->cu_x, d->cu_y, qg_x - 1, qg_y) &&
        ((qg_x - 1) & ctb_mask) == (qg_x & ctb_mask))
        qp_a = h265_bi(d, qg_x - 1, qg_y)->qp;
    if (qg_y > 0 && avail_z(d, d->cu_x, d->cu_y, qg_x, qg_y - 1) &&
        ((qg_y - 1) & ctb_mask) == (qg_y & ctb_mask))
        qp_b = h265_bi(d, qg_x, qg_y - 1)->qp;

    int pred = (qp_a + qp_b + 1) >> 1;
    /* 8.6.1 in full:
     *   QpY = ((qPY_PRED + CuQpDeltaVal + 52 + 2*QpBdOffsetY) % (52 + QpBdOffsetY))
     *         - QpBdOffsetY
     * The 8-bit code could write % 52 because QpBdOffsetY is 0 there. At 10
     * bits QpBdOffsetY is 12, the modulus is 64, and QpY legitimately goes
     * NEGATIVE -- which is why bi_t.qp is signed and why the chroma mapping
     * below must not clamp at 0. */
    int qpbd_y = 6 * (d->cur_sps->bit_depth_luma - 8);
    d->qp_y = ((pred + d->cu_qp_delta + 52 + 2 * qpbd_y) % (52 + qpbd_y)) - qpbd_y;
    d->qp_y_last = d->qp_y;
}

/* Qp'C = QpC + QpBdOffsetC, i.e. what dequantisation actually wants. */
static int chroma_qp_for(h265dec *d, int c)
{
    int off = (c == 1 ? d->cur_pps->cb_qp_offset + d->sh.cb_qp_offset
                      : d->cur_pps->cr_qp_offset + d->sh.cr_qp_offset);
    int qpbd_c = 6 * (d->cur_sps->bit_depth_chroma - 8);
    return h265_chroma_qp(h265_clip3(-qpbd_c, 57, d->qp_y + off)) + qpbd_c;
}

/* ======================= intra reference samples ======================== */
/* Gather the 4*nTbS+1 neighbour samples in h265_pred.c's layout, marking each
 * available or not (6.4.1 plus constrained_intra_pred), then run the
 * substitution of 8.4.4.2.2. */
static void gather_nb(h265dec *d, uint16_t *nb, int x0, int y0, int nbs, int c_idx)
{
    const pic_t *p = d->cur;
    int shift = c_idx ? 1 : 0;
    int stride = c_idx ? p->stride_c : p->stride_y;
    const uint16_t *plane = c_idx == 0 ? p->y : c_idx == 1 ? p->u : p->v;
    int cw = c_idx ? d->cur_sps->width / 2 : d->cur_sps->width;
    int chh = c_idx ? d->cur_sps->height / 2 : d->cur_sps->height;
    int xy = x0 << shift, yy = y0 << shift;
    int n = 4 * nbs + 1;
    uint8_t av[4 * 32 + 1];
    int any = 0;

    for (int i = 0; i < n; i++) {
        int cx, cy;
        if (i < 2 * nbs)      { cx = x0 - 1; cy = y0 + (2 * nbs - 1 - i); }
        else if (i == 2 * nbs) { cx = x0 - 1; cy = y0 - 1; }
        else                   { cx = x0 + (i - 2 * nbs - 1); cy = y0 - 1; }
        int ok = 0;
        if (cx >= 0 && cy >= 0 && cx < cw && cy < chh) {
            int lx = cx << shift, ly = cy << shift;
            if (avail_z(d, xy, yy, lx, ly)) {
                ok = 1;
                if (d->cur_pps->constrained_intra_pred &&
                    h265_bi(d, lx, ly)->pred_mode != MODE_INTRA) ok = 0;
            }
        }
        av[i] = (uint8_t)ok;
        nb[i] = ok ? plane[(long)cy * stride + cx] : 0;
        any |= ok;
    }

    if (!any) {
        /* 8.4.4.2.2: with nothing available every reference sample is
         * 1 << (BitDepth - 1) -- 128 at 8 bits, 512 at 10. A memset of 128
         * would put a 10-bit picture's fallback prediction at a quarter
         * brightness, and only on blocks with no neighbours at all. */
        int bd = c_idx ? d->cur_sps->bit_depth_chroma : d->cur_sps->bit_depth_luma;
        uint16_t mid = (uint16_t)(1 << (bd - 1));
        for (int i = 0; i < n; i++) nb[i] = mid;
        return;
    }
    /* The substitution scans from p[-1][2N-1] up the left column, round the
     * corner and along the top -- which is exactly increasing index here. */
    if (!av[0]) {
        for (int i = 1; i < n; i++)
            if (av[i]) { nb[0] = nb[i]; break; }
    }
    for (int i = 1; i < n; i++) if (!av[i]) nb[i] = nb[i - 1];
}

/* ======================== intra reconstruction ========================== */
static void intra_pred_block(h265dec *d, int x0, int y0, int log2sz, int c_idx, int mode)
{
    int nbs = 1 << log2sz;
    uint16_t nb[4 * 32 + 1];
    int shift = c_idx ? 1 : 0;
    int bd = c_idx ? d->cur_sps->bit_depth_chroma : d->cur_sps->bit_depth_luma;
    gather_nb(d, nb, x0 >> shift, y0 >> shift, nbs, c_idx);
    h265_intra_filter(nb, nbs, mode, c_idx, d->cur_sps->strong_intra_smoothing, bd);
    pic_t *p = d->cur;
    int stride = c_idx ? p->stride_c : p->stride_y;
    uint16_t *dst = (c_idx == 0 ? p->y : c_idx == 1 ? p->u : p->v) +
                   (long)(y0 >> shift) * stride + (x0 >> shift);
    h265_intra_pred(dst, stride, nb, nbs, mode, c_idx, bd);
}

/* ============================ transform unit ============================ */
static void add_residual(h265dec *d, int x0, int y0, int log2sz, int c_idx,
                         int mode, int is_intra, int ts, int bypass)
{
    pic_t *p = d->cur;
    int shift = c_idx ? 1 : 0;
    int stride = c_idx ? p->stride_c : p->stride_y;
    int bd = c_idx ? d->cur_sps->bit_depth_chroma : d->cur_sps->bit_depth_luma;
    uint16_t *dst = (c_idx == 0 ? p->y : c_idx == 1 ? p->u : p->v) +
                   (long)(y0 >> shift) * stride + (x0 >> shift);
    if (bypass) { h265_bypass_add(d->coeff, log2sz, dst, stride, bd); return; }

    /* Dequantisation takes Qp' = Qp + QpBdOffset, not Qp. */
    int qp = c_idx ? chroma_qp_for(d, c_idx)
                   : d->qp_y + 6 * (d->cur_sps->bit_depth_luma - 8);
    const uint8_t *list = 0;
    int dc = 16;
    if (d->cur_sps->scaling_list_enabled && !(ts && log2sz > 2)) {
        const pps_t *pp = d->cur_pps;
        const sps_t *sp = d->cur_sps;
        const uint8_t (*sl)[6][64] = pp->sl_present ? pp->sl : sp->sl;
        const uint8_t (*sd)[6] = pp->sl_present ? pp->sl_dc : sp->sl_dc;
        int size_id = log2sz - 2;
        int mat = (is_intra ? 0 : 3) + c_idx;
        if (size_id == 3) mat = is_intra ? 0 : 3;
        list = sl[size_id][mat];
        if (size_id > 1) dc = sd[size_id - 2][(is_intra ? 0 : 3) + c_idx];
    }
    h265_dequant(d->coeff, 1 << log2sz, qp, log2sz, list, dc, bd);

    if (ts) {
        h265_transform_skip_add(d->coeff, log2sz, dst, stride, bd);
    } else {
        int tr = (is_intra && c_idx == 0 && log2sz == 2) ? 1 : 0;
        h265_itransform_add(d->coeff, log2sz, tr, dst, stride, bd);
        (void)mode;
    }
}

static int transform_unit(h265dec *d, int x0, int y0, int xb, int yb,
                          int log2sz, int depth, int blk_idx, int cbf_luma)
{
    int is_intra = (d->cu_pred_mode == MODE_INTRA);
    int log2c = log2sz > 2 ? log2sz - 1 : 2;
    int cbf_c = (log2sz == 2)
              ? (d->cbf_cb[depth - 1] | d->cbf_cr[depth - 1])
              : (d->cbf_cb[depth] | d->cbf_cr[depth]);

    /* Intra prediction happens per transform block, in tree order, because
     * each block predicts from the reconstruction of the ones before it. */
    if (is_intra)
        intra_pred_block(d, x0, y0, log2sz, 0, h265_bi(d, x0, y0)->intra_mode);

    if ((cbf_luma || cbf_c) && d->cur_pps->cu_qp_delta_enabled &&
        !d->cu_qp_delta_coded) {
        int pre = 0;
        while (pre < 5 && DEC(CTX_CU_QP_DELTA + (pre > 0))) pre++;
        int v = pre;
        if (pre == 5) v += decode_eg(d, 0);
        if (v && BYP()) v = -v;
        d->cu_qp_delta = v;
        d->cu_qp_delta_coded = 1;
        derive_qp(d);
        bi_t t; memset(&t, 0, sizeof t); t.qp = (int8_t)d->qp_y;
        /* The whole quantisation group takes the new QP, including the parts
         * already decoded: the deblocking filter reads it back per 4x4. */
        int qsz = 1 << (d->cur_sps->log2_ctb - d->cur_pps->diff_cu_qp_delta_depth);
        fill_bi(d, d->qg_x, d->qg_y, qsz, qsz, &t, 2);
    }

    if (cbf_luma) {
        int ts = 0;
        int mode = is_intra ? h265_bi(d, x0, y0)->intra_mode : 0;
        int rc = h265_residual_coding(d, log2sz, 0, mode, is_intra,
                                      d->cu_bypass, d->coeff, &ts);
        if (rc < 0) return rc;
        add_residual(d, x0, y0, log2sz, 0, mode, is_intra, ts, d->cu_bypass);
    }

    /* Chroma. At log2sz 2 the four luma blocks share one chroma block, which
     * is handled once, at blkIdx 3, over the parent's area. */
    if (log2sz > 2) {
        if (is_intra) {
            intra_pred_block(d, x0, y0, log2c, 1, d->intra_mode_c);
            intra_pred_block(d, x0, y0, log2c, 2, d->intra_mode_c);
        }
        for (int c = 1; c <= 2; c++) {
            int cbf = c == 1 ? d->cbf_cb[depth] : d->cbf_cr[depth];
            if (!cbf) continue;
            int ts = 0;
            int rc = h265_residual_coding(d, log2c, c, d->intra_mode_c, is_intra,
                                          d->cu_bypass, d->coeff, &ts);
            if (rc < 0) return rc;
            add_residual(d, x0, y0, log2c, c, d->intra_mode_c, is_intra, ts,
                         d->cu_bypass);
        }
    } else if (blk_idx == 3) {
        if (is_intra) {
            intra_pred_block(d, xb, yb, 2, 1, d->intra_mode_c);
            intra_pred_block(d, xb, yb, 2, 2, d->intra_mode_c);
        }
        for (int c = 1; c <= 2; c++) {
            int cbf = c == 1 ? d->cbf_cb[depth - 1] : d->cbf_cr[depth - 1];
            if (!cbf) continue;
            int ts = 0;
            int rc = h265_residual_coding(d, 2, c, d->intra_mode_c, is_intra,
                                          d->cu_bypass, d->coeff, &ts);
            if (rc < 0) return rc;
            add_residual(d, xb, yb, 2, c, d->intra_mode_c, is_intra, ts, d->cu_bypass);
        }
    }
    return H265_OK;
}

static int transform_tree(h265dec *d, int x0, int y0, int xb, int yb,
                          int log2sz, int depth, int blk_idx)
{
    const sps_t *sps = d->cur_sps;
    int W = sps->width, H = sps->height;
    int split;

    int inter_split = (sps->max_transform_hierarchy_depth_inter == 0 &&
                       d->cu_pred_mode == MODE_INTER &&
                       d->cu_part_mode != PART_2Nx2N && depth == 0);
    if (log2sz <= sps->log2_max_tb && log2sz > sps->log2_min_tb &&
        depth < d->max_trafo_depth && !(d->intra_split && depth == 0) &&
        !inter_split) {
        split = DEC(CTX_SPLIT_TRANSFORM + 5 - log2sz);
    } else {
        split = (log2sz > sps->log2_max_tb) ||
                (d->intra_split && depth == 0) || inter_split;
    }

    d->cbf_cb[depth] = 0;
    d->cbf_cr[depth] = 0;
    if (log2sz > 2) {
        if (depth == 0 || d->cbf_cb[depth - 1])
            d->cbf_cb[depth] = (uint8_t)DEC(CTX_CBF_CHROMA + depth);
        if (depth == 0 || d->cbf_cr[depth - 1])
            d->cbf_cr[depth] = (uint8_t)DEC(CTX_CBF_CHROMA + depth);
    } else if (depth > 0) {
        d->cbf_cb[depth] = d->cbf_cb[depth - 1];
        d->cbf_cr[depth] = d->cbf_cr[depth - 1];
    }

    if (split) {
        int half = 1 << (log2sz - 1);
        int x1 = x0 + half, y1 = y0 + half;
        int rc;
        if ((rc = transform_tree(d, x0, y0, x0, y0, log2sz - 1, depth + 1, 0))) return rc;
        if (x1 < W && (rc = transform_tree(d, x1, y0, x0, y0, log2sz - 1, depth + 1, 1))) return rc;
        if (y1 < H && (rc = transform_tree(d, x0, y1, x0, y0, log2sz - 1, depth + 1, 2))) return rc;
        if (x1 < W && y1 < H &&
            (rc = transform_tree(d, x1, y1, x0, y0, log2sz - 1, depth + 1, 3))) return rc;
        return H265_OK;
    }

    int cbf_luma = 1;
    if (d->cu_pred_mode == MODE_INTRA || depth != 0 ||
        d->cbf_cb[depth] || d->cbf_cr[depth])
        cbf_luma = DEC(CTX_CBF_LUMA + (depth == 0));

    mark_edges(d, x0, y0, 1 << log2sz, 1 << log2sz, 1);
    if (cbf_luma) {
        bi_t t; memset(&t, 0, sizeof t); t.cbf_luma = 1;
        fill_bi(d, x0, y0, 1 << log2sz, 1 << log2sz, &t, 4);
    }
    return transform_unit(d, x0, y0, xb, yb, log2sz, depth, blk_idx, cbf_luma);
}

/* ========================= motion vector derivation ===================== */
typedef struct {
    int pred_flag;
    int ref_idx[2];
    int mv[2][2];
} mvc_t;

static void bi_to_cand(const bi_t *b, mvc_t *c)
{
    c->pred_flag = b->pred_flag;
    for (int l = 0; l < 2; l++) {
        c->ref_idx[l] = b->ref_idx[l];
        c->mv[l][0] = b->mv[l][0];
        c->mv[l][1] = b->mv[l][1];
    }
}

static int same_motion(const mvc_t *a, const mvc_t *b)
{
    if (a->pred_flag != b->pred_flag) return 0;
    for (int l = 0; l < 2; l++)
        if (a->pred_flag & (1 << l)) {
            if (a->ref_idx[l] != b->ref_idx[l]) return 0;
            if (a->mv[l][0] != b->mv[l][0] || a->mv[l][1] != b->mv[l][1]) return 0;
        }
    return 1;
}

static int scale_mv(int mv, int td, int tb)
{
    td = h265_clip3(-128, 127, td);
    tb = h265_clip3(-128, 127, tb);
    if (td == 0) return mv;
    int tx = (16384 + (abs(td) >> 1)) / td;
    int dsf = h265_clip3(-4096, 4095, (tb * tx + 32) >> 6);
    int v = dsf * mv;
    int s = v < 0 ? -1 : 1;
    return h265_clip3(-32768, 32767, s * ((abs(v) + 127) >> 8));
}

/* 8.5.3.2.8: fetch the collocated motion vector for list X and refIdx. */
static int col_mv(h265dec *d, int xcol, int ycol, int list, int ref_idx, int mv[2])
{
    pic_t *cp = d->col_pic;
    if (!cp || !cp->col) return 0;
    int cx = xcol >> 4, cy = ycol >> 4;
    if (cx < 0 || cy < 0 || cx >= cp->col_w || cy >= cp->col_h) return 0;
    const colmv_t *c = &cp->col[cy * cp->col_w + cx];
    if (c->intra) return 0;

    int lc;
    if (!c->refpoc_valid[0]) lc = 1;
    else if (!c->refpoc_valid[1]) lc = 0;
    else {
        /* Both lists present. If every reference of the current slice is at
         * or before the current picture, take the list being derived;
         * otherwise take the one the collocated flag names. */
        int all_before = 1;
        for (int l = 0; l < 2; l++)
            for (int i = 0; i < d->nb_refs[l]; i++)
                if (diff_poc(d->ref_poc_list[l][i], d->poc) > 0) all_before = 0;
        lc = all_before ? list : (d->sh.collocated_from_l0 ? 1 : 0);
    }
    if (!c->refpoc_valid[lc]) return 0;

    int col_is_lt = c->islt[lc];
    int cur_is_lt = d->ref_islt[list][ref_idx];
    if (col_is_lt != cur_is_lt) return 0;

    int col_diff = diff_poc(cp->poc, c->refpoc[lc]);
    int cur_diff = diff_poc(d->poc, d->ref_poc_list[list][ref_idx]);
    if (cur_is_lt || col_diff == cur_diff) {
        mv[0] = c->mv[lc][0];
        mv[1] = c->mv[lc][1];
    } else {
        mv[0] = scale_mv(c->mv[lc][0], col_diff, cur_diff);
        mv[1] = scale_mv(c->mv[lc][1], col_diff, cur_diff);
    }
    return 1;
}

/* 8.5.3.2.7: bottom-right first, then the centre. */
static int temporal_mv(h265dec *d, int xpb, int ypb, int npbw, int npbh,
                       int list, int ref_idx, int mv[2])
{
    if (!d->sh.temporal_mvp_enabled) return 0;
    const sps_t *sps = d->cur_sps;
    int xbr = xpb + npbw, ybr = ypb + npbh;
    if ((d->cu_y >> sps->log2_ctb) == (ybr >> sps->log2_ctb) &&
        ybr < sps->height && xbr < sps->width) {
        if (col_mv(d, (xbr >> 4) << 4, (ybr >> 4) << 4, list, ref_idx, mv)) return 1;
    }
    int xc = xpb + (npbw >> 1), yc = ypb + (npbh >> 1);
    return col_mv(d, (xc >> 4) << 4, (yc >> 4) << 4, list, ref_idx, mv);
}

/* 8.5.3.2.2 / .3: the merge candidate list. */
static int merge_candidates(h265dec *d, int xcb, int ycb, int ncbs,
                            int xpb, int ypb, int npbw, int npbh, int part_idx,
                            mvc_t *list)
{
    const slice_t *sl = &d->sh;
    int par = d->cur_pps->log2_parallel_merge_level;

    /* The parallel merge level can collapse an 8x8 CU's partitions into one
     * merge estimation region (8.5.3.2.2). */
    if (par > 2 && ncbs == 8) {
        xpb = xcb; ypb = ycb; npbw = ncbs; npbh = ncbs; part_idx = 0;
    }

    /* Two different things, and conflating them is a real bug (it was one
     * here): `avail[k]` is availableN -- whether the neighbour EXISTS as a
     * prediction block -- while a candidate makes the list only if it also
     * survives pruning. 8.5.3.2.3 phrases every pruning test against
     * available**N**, not availableFlag**N**: B0 is dropped when "availableB1
     * is TRUE and B1 and B0 have the same motion", which still holds when B1
     * itself was pruned away for matching A1. Testing "did B1 make the list"
     * instead lets B0 in as a duplicate, which shifts every later merge_idx
     * and silently mis-predicts a scattering of blocks per picture. */
    int avail[5] = { 0, 0, 0, 0, 0 };
    mvc_t mv[5];
    int nx[5], ny[5];
    memset(mv, 0, sizeof mv);
    nx[0] = xpb - 1;        ny[0] = ypb + npbh - 1;    /* A1 */
    nx[1] = xpb + npbw - 1; ny[1] = ypb - 1;           /* B1 */
    nx[2] = xpb + npbw;     ny[2] = ypb - 1;           /* B0 */
    nx[3] = xpb - 1;        ny[3] = ypb + npbh;        /* A0 */
    nx[4] = xpb - 1;        ny[4] = ypb - 1;           /* B2 */

    for (int k = 0; k < 5; k++) {
        /* Same merge estimation region: not a candidate. */
        if ((nx[k] >> par) == (xpb >> par) && (ny[k] >> par) == (ypb >> par))
            continue;
        if (k == 0 && (d->cu_part_mode == PART_Nx2N ||
                       d->cu_part_mode == PART_nLx2N ||
                       d->cu_part_mode == PART_nRx2N) && part_idx == 1) continue;
        if (k == 1 && (d->cu_part_mode == PART_2NxN ||
                       d->cu_part_mode == PART_2NxnU ||
                       d->cu_part_mode == PART_2NxnD) && part_idx == 1) continue;
        if (!avail_pb(d, xcb, ycb, ncbs, xpb, ypb, npbw, npbh, part_idx,
                      nx[k], ny[k])) continue;
        avail[k] = 1;
        bi_to_cand(h265_bi(d, nx[k], ny[k]), &mv[k]);
    }

    /* Pruning, exactly the pairs 8.5.3.2.3 names -- not an all-pairs
     * comparison, which would drop candidates the spec keeps. */
    int n = 0;
    int added[5] = { 0, 0, 0, 0, 0 };
    if (avail[0]) added[0] = 1;                                   /* A1 */
    if (avail[1] && !(avail[0] && same_motion(&mv[1], &mv[0])))
        added[1] = 1;                                             /* B1 */
    if (avail[2] && !(avail[1] && same_motion(&mv[2], &mv[1])))
        added[2] = 1;                                             /* B0 */
    if (avail[3] && !(avail[0] && same_motion(&mv[3], &mv[0])))
        added[3] = 1;                                             /* A0 */
    if (avail[4] && !(avail[0] && same_motion(&mv[4], &mv[0])) &&
                    !(avail[1] && same_motion(&mv[4], &mv[1])) &&
        (added[0] + added[1] + added[2] + added[3]) != 4)
        added[4] = 1;                                             /* B2 */

    for (int i = 0; i < 5 && n < sl->max_merge_cand; i++)
        if (added[i]) list[n++] = mv[i];

    /* Temporal candidate, always with refIdx 0. */
    if (n < sl->max_merge_cand && sl->temporal_mvp_enabled) {
        mvc_t c;
        memset(&c, 0, sizeof c);
        int mv[2];
        if (d->nb_refs[0] > 0 && temporal_mv(d, xpb, ypb, npbw, npbh, 0, 0, mv)) {
            c.pred_flag |= 1; c.mv[0][0] = mv[0]; c.mv[0][1] = mv[1]; c.ref_idx[0] = 0;
        }
        if (sl->type == SLICE_B && d->nb_refs[1] > 0 &&
            temporal_mv(d, xpb, ypb, npbw, npbh, 1, 0, mv)) {
            c.pred_flag |= 2; c.mv[1][0] = mv[0]; c.mv[1][1] = mv[1]; c.ref_idx[1] = 0;
        }
        if (c.pred_flag) list[n++] = c;
    }

    /* Combined bi-predictive candidates (8.5.3.2.4, Table 8-6). */
    if (sl->type == SLICE_B && n > 1) {
        static const int l0i[12] = { 0, 1, 0, 2, 1, 2, 0, 3, 1, 3, 2, 3 };
        static const int l1i[12] = { 1, 0, 2, 0, 2, 1, 3, 0, 3, 1, 3, 2 };
        int orig = n;
        for (int ci = 0; ci < orig * (orig - 1) && n < sl->max_merge_cand; ci++) {
            if (ci >= 12) break;
            const mvc_t *a = &list[l0i[ci]], *b = &list[l1i[ci]];
            if (!(a->pred_flag & 1) || !(b->pred_flag & 2)) continue;
            int pa = d->ref_poc_list[0][a->ref_idx[0]];
            int pb = d->ref_poc_list[1][b->ref_idx[1]];
            if (pa == pb && a->mv[0][0] == b->mv[1][0] && a->mv[0][1] == b->mv[1][1])
                continue;
            mvc_t c;
            memset(&c, 0, sizeof c);
            c.pred_flag = 3;
            c.ref_idx[0] = a->ref_idx[0];
            c.mv[0][0] = a->mv[0][0]; c.mv[0][1] = a->mv[0][1];
            c.ref_idx[1] = b->ref_idx[1];
            c.mv[1][0] = b->mv[1][0]; c.mv[1][1] = b->mv[1][1];
            list[n++] = c;
        }
    }

    /* Zero candidates (8.5.3.2.5). */
    int numref = (sl->type == SLICE_P) ? d->nb_refs[0]
               : (d->nb_refs[0] < d->nb_refs[1] ? d->nb_refs[0] : d->nb_refs[1]);
    int zi = 0;
    while (n < sl->max_merge_cand) {
        mvc_t c;
        memset(&c, 0, sizeof c);
        c.pred_flag = (sl->type == SLICE_B) ? 3 : 1;
        c.ref_idx[0] = (zi < numref) ? zi : 0;
        c.ref_idx[1] = (sl->type == SLICE_B) ? ((zi < numref) ? zi : 0) : 0;
        list[n++] = c;
        zi++;
        if (zi > 32) break;
    }
    return n;
}

/* 8.5.3.2.6: the two AMVP candidates for one list. */
static void amvp(h265dec *d, int xcb, int ycb, int ncbs, int xpb, int ypb,
                 int npbw, int npbh, int part_idx, int list, int ref_idx,
                 int mvp_flag, int out[2])
{
    int cand[2][2], ncand = 0;
    int cur_poc = d->ref_poc_list[list][ref_idx];
    int cur_lt = d->ref_islt[list][ref_idx];
    int other = 1 - list;

    int ax[2], ay[2], bx[3], by[3];
    ax[0] = xpb - 1;        ay[0] = ypb + npbh;        /* A0 */
    ax[1] = xpb - 1;        ay[1] = ypb + npbh - 1;    /* A1 */
    bx[0] = xpb + npbw;     by[0] = ypb - 1;           /* B0 */
    bx[1] = xpb + npbw - 1; by[1] = ypb - 1;           /* B1 */
    bx[2] = xpb - 1;        by[2] = ypb - 1;           /* B2 */

    int a_avail[2], b_avail[3];
    for (int k = 0; k < 2; k++)
        a_avail[k] = avail_pb(d, xcb, ycb, ncbs, xpb, ypb, npbw, npbh, part_idx,
                              ax[k], ay[k]);
    for (int k = 0; k < 3; k++)
        b_avail[k] = avail_pb(d, xcb, ycb, ncbs, xpb, ypb, npbw, npbh, part_idx,
                              bx[k], by[k]);
    int is_scaled = a_avail[0] || a_avail[1];

    int have_a = 0, mv_a[2] = { 0, 0 };
    /* First pass: an exact picture match needs no scaling. */
    for (int k = 0; k < 2 && !have_a; k++) {
        if (!a_avail[k]) continue;
        const bi_t *b = h265_bi(d, ax[k], ay[k]);
        for (int t = 0; t < 2 && !have_a; t++) {
            int l = t == 0 ? list : other;
            if (!(b->pred_flag & (1 << l))) continue;
            if (b->refpoc[l] != cur_poc) continue;
            mv_a[0] = b->mv[l][0]; mv_a[1] = b->mv[l][1];
            have_a = 1;
        }
    }
    /* Second pass: any reference of the same long-term-ness, then scale. */
    for (int k = 0; k < 2 && !have_a; k++) {
        if (!a_avail[k]) continue;
        const bi_t *b = h265_bi(d, ax[k], ay[k]);
        for (int t = 0; t < 2 && !have_a; t++) {
            int l = t == 0 ? list : other;
            if (!(b->pred_flag & (1 << l))) continue;
            if (b->islt[l] != cur_lt) continue;
            mv_a[0] = b->mv[l][0]; mv_a[1] = b->mv[l][1];
            have_a = 1;
            if (!cur_lt && b->refpoc[l] != cur_poc) {
                mv_a[0] = scale_mv(mv_a[0], diff_poc(d->poc, b->refpoc[l]),
                                   diff_poc(d->poc, cur_poc));
                mv_a[1] = scale_mv(mv_a[1], diff_poc(d->poc, b->refpoc[l]),
                                   diff_poc(d->poc, cur_poc));
            }
        }
    }

    int have_b = 0, mv_b[2] = { 0, 0 };
    for (int k = 0; k < 3 && !have_b; k++) {
        if (!b_avail[k]) continue;
        const bi_t *b = h265_bi(d, bx[k], by[k]);
        for (int t = 0; t < 2 && !have_b; t++) {
            int l = t == 0 ? list : other;
            if (!(b->pred_flag & (1 << l))) continue;
            if (b->refpoc[l] != cur_poc) continue;
            mv_b[0] = b->mv[l][0]; mv_b[1] = b->mv[l][1];
            have_b = 1;
        }
    }
    if (!is_scaled) {
        /* When neither A position exists, B's value moves into A and B is
         * re-derived with scaling allowed. */
        have_a = have_b;
        mv_a[0] = mv_b[0]; mv_a[1] = mv_b[1];
        have_b = 0;
        for (int k = 0; k < 3 && !have_b; k++) {
            if (!b_avail[k]) continue;
            const bi_t *b = h265_bi(d, bx[k], by[k]);
            for (int t = 0; t < 2 && !have_b; t++) {
                int l = t == 0 ? list : other;
                if (!(b->pred_flag & (1 << l))) continue;
                if (b->islt[l] != cur_lt) continue;
                mv_b[0] = b->mv[l][0]; mv_b[1] = b->mv[l][1];
                have_b = 1;
                if (!cur_lt && b->refpoc[l] != cur_poc) {
                    mv_b[0] = scale_mv(mv_b[0], diff_poc(d->poc, b->refpoc[l]),
                                       diff_poc(d->poc, cur_poc));
                    mv_b[1] = scale_mv(mv_b[1], diff_poc(d->poc, b->refpoc[l]),
                                       diff_poc(d->poc, cur_poc));
                }
            }
        }
    }

    if (have_a) { cand[ncand][0] = mv_a[0]; cand[ncand][1] = mv_a[1]; ncand++; }
    if (have_b && !(have_a && mv_a[0] == mv_b[0] && mv_a[1] == mv_b[1])) {
        cand[ncand][0] = mv_b[0]; cand[ncand][1] = mv_b[1]; ncand++;
    }
    if (ncand < 2 && d->sh.temporal_mvp_enabled) {
        int mv[2];
        if (temporal_mv(d, xpb, ypb, npbw, npbh, list, ref_idx, mv)) {
            cand[ncand][0] = mv[0]; cand[ncand][1] = mv[1]; ncand++;
        }
    }
    while (ncand < 2) { cand[ncand][0] = 0; cand[ncand][1] = 0; ncand++; }

    out[0] = cand[mvp_flag][0];
    out[1] = cand[mvp_flag][1];
}

/* ========================= inter reconstruction ========================= */
static void mc_pu(h265dec *d, int x, int y, int w, int h, const mvc_t *c)
{
    const sps_t *sps = d->cur_sps;
    const slice_t *sl = &d->sh;
    pic_t *dst = d->cur;
    int wp = (sl->type == SLICE_P) ? d->cur_pps->weighted_pred
                                   : d->cur_pps->weighted_bipred;
    int bi = (c->pred_flag == 3);

    for (int comp = 0; comp < 3; comp++) {
        int shift = comp ? 1 : 0;
        int cw = comp ? sps->width / 2 : sps->width;
        int ch = comp ? sps->height / 2 : sps->height;
        int bw = w >> shift, bh = h >> shift;
        int bx = x >> shift, by = y >> shift;
        int stride = comp ? dst->stride_c : dst->stride_y;
        int bd = comp ? sps->bit_depth_chroma : sps->bit_depth_luma;
        uint16_t *out = (comp == 0 ? dst->y : comp == 1 ? dst->u : dst->v) +
                       (long)by * stride + bx;
        int16_t *buf[2] = { d->tmp0, d->tmp1 };

        for (int l = 0; l < 2; l++) {
            if (!(c->pred_flag & (1 << l))) continue;
            pic_t *ref = d->ref_list[l][c->ref_idx[l]];
            const uint16_t *rp = comp == 0 ? ref->y : comp == 1 ? ref->u : ref->v;
            int rs = comp ? ref->stride_c : ref->stride_y;
            if (comp == 0)
                h265_mc_luma(buf[l], 64, rp, rs, cw, ch, bx, by, bw, bh,
                             c->mv[l][0], c->mv[l][1], d->emu, bd);
            else
                h265_mc_chroma(buf[l], 64, rp, rs, cw, ch, bx, by, bw, bh,
                               c->mv[l][0], c->mv[l][1], d->emu, bd);
        }

        int l0 = (c->pred_flag & 1) ? 0 : 1;
        if (!wp) {
            if (bi) h265_pred_bi(out, stride, buf[0], 64, buf[1], 64, bw, bh, bd);
            else    h265_pred_uni(out, stride, buf[l0], 64, bw, bh, bd);
        } else {
            int denom = comp ? sl->chroma_log2_weight_denom : sl->luma_log2_weight_denom;
            if (bi) {
                int w0 = comp ? sl->chroma_weight[0][c->ref_idx[0]][comp - 1]
                              : sl->luma_weight[0][c->ref_idx[0]];
                int o0 = comp ? sl->chroma_offset[0][c->ref_idx[0]][comp - 1]
                              : sl->luma_offset[0][c->ref_idx[0]];
                int w1 = comp ? sl->chroma_weight[1][c->ref_idx[1]][comp - 1]
                              : sl->luma_weight[1][c->ref_idx[1]];
                int o1 = comp ? sl->chroma_offset[1][c->ref_idx[1]][comp - 1]
                              : sl->luma_offset[1][c->ref_idx[1]];
                h265_pred_bi_w(out, stride, buf[0], 64, buf[1], 64, bw, bh,
                               denom, w0, o0, w1, o1, bd);
            } else {
                int wt = comp ? sl->chroma_weight[l0][c->ref_idx[l0]][comp - 1]
                              : sl->luma_weight[l0][c->ref_idx[l0]];
                int of = comp ? sl->chroma_offset[l0][c->ref_idx[l0]][comp - 1]
                              : sl->luma_offset[l0][c->ref_idx[l0]];
                h265_pred_uni_w(out, stride, buf[l0], 64, bw, bh, denom, wt, of, bd);
            }
        }
    }
}

static void store_pu(h265dec *d, int x, int y, int w, int h, const mvc_t *c)
{
    bi_t t;
    memset(&t, 0, sizeof t);
    t.pred_mode = MODE_INTER;
    t.skip = (uint8_t)d->cu_skip;
    t.bypass = (uint8_t)d->cu_bypass;
    t.ct_depth = (uint8_t)(d->cur_sps->log2_ctb - d->cu_log2);
    t.pred_flag = (uint8_t)c->pred_flag;
    for (int l = 0; l < 2; l++) {
        t.ref_idx[l] = (int8_t)c->ref_idx[l];
        t.mv[l][0] = (int16_t)c->mv[l][0];
        t.mv[l][1] = (int16_t)c->mv[l][1];
        if (c->pred_flag & (1 << l)) {
            t.refpoc[l] = d->ref_poc_list[l][c->ref_idx[l]];
            t.islt[l] = d->ref_islt[l][c->ref_idx[l]];
        }
    }
    fill_bi(d, x, y, w, h, &t, 1);
    mark_edges(d, x, y, w, h, 0);
}

static int prediction_unit(h265dec *d, int xcb, int ycb, int ncbs,
                           int x, int y, int w, int h, int part_idx)
{
    const slice_t *sl = &d->sh;
    mvc_t c;
    memset(&c, 0, sizeof c);

    int merge = d->cu_skip;
    if (!d->cu_skip) merge = DEC(CTX_MERGE_FLAG);
    if (part_idx == 0) d->cu_merge_2nx2n = merge;

    if (merge) {
        int idx = 0;
        if (sl->max_merge_cand > 1) {
            if (DEC(CTX_MERGE_IDX)) {
                idx = 1;
                while (idx < sl->max_merge_cand - 1 && BYP()) idx++;
            }
        }
        mvc_t list[8];
        int n = merge_candidates(d, xcb, ycb, ncbs, x, y, w, h, part_idx, list);
        if (idx >= n) idx = n - 1;
        if (idx < 0) return H265_ERR_CORRUPT;
        c = list[idx];
        /* 8.5.3.2.1: a bi-predictive merge candidate on an 8x4 or 4x8 block
         * is forced back to uni-L0. */
        if (c.pred_flag == 3 && (w + h) == 12) c.pred_flag = 1;
    } else {
        int idc = 0;                        /* 0 = L0, 1 = L1, 2 = BI */
        if (sl->type == SLICE_B) {
            int ct = d->cur_sps->log2_ctb - d->cu_log2;
            if ((w + h) != 12) {
                if (DEC(CTX_INTER_PRED_IDC + ct)) idc = 2;
                else idc = DEC(CTX_INTER_PRED_IDC + 4);
            } else {
                idc = DEC(CTX_INTER_PRED_IDC + 4);
            }
        }
        c.pred_flag = (idc == 0) ? 1 : (idc == 1) ? 2 : 3;
        for (int l = 0; l < 2; l++) {
            if (!(c.pred_flag & (1 << l))) continue;
            int ref = 0;
            if (d->nb_refs[l] > 1) {
                if (DEC(CTX_REF_IDX)) {
                    ref = 1;
                    if (d->nb_refs[l] > 2 && DEC(CTX_REF_IDX + 1)) {
                        ref = 2;
                        while (ref < d->nb_refs[l] - 1 && BYP()) ref++;
                    }
                }
            }
            if (ref >= d->nb_refs[l]) return H265_ERR_CORRUPT;
            c.ref_idx[l] = ref;

            int mvd[2] = { 0, 0 };
            if (l == 1 && sl->mvd_l1_zero && c.pred_flag == 3) {
                mvd[0] = mvd[1] = 0;
            } else {
                int g0[2], g1[2] = { 0, 0 };
                g0[0] = DEC(CTX_MVD_GREATER0);
                g0[1] = DEC(CTX_MVD_GREATER0);
                if (g0[0]) g1[0] = DEC(CTX_MVD_GREATER1);
                if (g0[1]) g1[1] = DEC(CTX_MVD_GREATER1);
                for (int k = 0; k < 2; k++) {
                    if (!g0[k]) continue;
                    int v = 1;
                    if (g1[k]) v = decode_eg(d, 1) + 2;
                    if (BYP()) v = -v;
                    mvd[k] = v;
                }
            }
            int mvp_flag = DEC(CTX_MVP_FLAG);
            int pred[2];
            amvp(d, xcb, ycb, ncbs, x, y, w, h, part_idx, l, ref, mvp_flag, pred);
            /* 8.5.3.2.10: the sum wraps in 16-bit two's complement. */
            int u0 = (pred[0] + mvd[0] + 65536) & 0xFFFF;
            int u1 = (pred[1] + mvd[1] + 65536) & 0xFFFF;
            c.mv[l][0] = (u0 >= 32768) ? u0 - 65536 : u0;
            c.mv[l][1] = (u1 >= 32768) ? u1 - 65536 : u1;
        }
    }
    if (d->cabac.error) return H265_ERR_CORRUPT;
    for (int l = 0; l < 2; l++)
        if ((c.pred_flag & (1 << l)) &&
            (c.ref_idx[l] < 0 || c.ref_idx[l] >= d->nb_refs[l]))
            return H265_ERR_CORRUPT;

    TRACE("  PU (%d,%d) %dx%d merge%d pf%d ref%d,%d mv0(%d,%d) mv1(%d,%d)\n",
          x, y, w, h, merge, c.pred_flag, c.ref_idx[0], c.ref_idx[1],
          c.mv[0][0], c.mv[0][1], c.mv[1][0], c.mv[1][1]);
    store_pu(d, x, y, w, h, &c);
    mc_pu(d, x, y, w, h, &c);
    return H265_OK;
}

/* ============================== coding unit ============================= */
static int decode_part_mode(h265dec *d, int log2cb, int is_intra)
{
    int min_log2 = d->cur_sps->log2_min_cb;
    if (is_intra) return DEC(CTX_PART_MODE) ? PART_2Nx2N : PART_NxN;
    if (DEC(CTX_PART_MODE)) return PART_2Nx2N;
    if (log2cb > min_log2) {
        if (!d->cur_sps->amp_enabled)
            return DEC(CTX_PART_MODE + 1) ? PART_2NxN : PART_Nx2N;
        if (DEC(CTX_PART_MODE + 1)) {
            if (DEC(CTX_PART_MODE + 3)) return PART_2NxN;
            return BYP() ? PART_2NxnD : PART_2NxnU;
        }
        if (DEC(CTX_PART_MODE + 3)) return PART_Nx2N;
        return BYP() ? PART_nRx2N : PART_nLx2N;
    }
    if (DEC(CTX_PART_MODE + 1)) return PART_2NxN;
    if (log2cb == 3) return PART_Nx2N;
    return DEC(CTX_PART_MODE + 2) ? PART_Nx2N : PART_NxN;
}

/* 8.4.2: IntraPredModeY from the three most probable modes. */
static int derive_intra_mode(h265dec *d, int x, int y, int prev, int mpm, int rem)
{
    const sps_t *sps = d->cur_sps;
    int ca, cb;
    if (!avail_z(d, x, y, x - 1, y) || h265_bi(d, x - 1, y)->pred_mode != MODE_INTRA
        || h265_bi(d, x - 1, y)->pcm)
        ca = 1;
    else ca = h265_bi(d, x - 1, y)->intra_mode;
    if (!avail_z(d, x, y, x, y - 1) || h265_bi(d, x, y - 1)->pred_mode != MODE_INTRA
        || h265_bi(d, x, y - 1)->pcm)
        cb = 1;
    else if ((y - 1) < ((y >> sps->log2_ctb) << sps->log2_ctb))
        cb = 1;                     /* above the CTB row: not remembered */
    else cb = h265_bi(d, x, y - 1)->intra_mode;

    int cand[3];
    if (ca == cb) {
        if (ca < 2) { cand[0] = 0; cand[1] = 1; cand[2] = 26; }
        else {
            cand[0] = ca;
            cand[1] = 2 + ((ca + 29) % 32);
            cand[2] = 2 + ((ca - 2 + 1) % 32);
        }
    } else {
        cand[0] = ca; cand[1] = cb;
        if (ca != 0 && cb != 0) cand[2] = 0;
        else if (ca != 1 && cb != 1) cand[2] = 1;
        else cand[2] = 26;
    }
    if (prev) return cand[mpm];
    for (int i = 0; i < 2; i++)
        for (int j = i + 1; j < 3; j++)
            if (cand[i] > cand[j]) { int t = cand[i]; cand[i] = cand[j]; cand[j] = t; }
    int m = rem;
    for (int i = 0; i < 3; i++) if (m >= cand[i]) m++;
    return m;
}

static int coding_unit(h265dec *d, int x0, int y0, int log2cb)
{
    const sps_t *sps = d->cur_sps;
    const pps_t *pps = d->cur_pps;
    const slice_t *sl = &d->sh;
    int ncbs = 1 << log2cb;
    int rc;

    TRACE("CU (%d,%d) sz%d poc%d pos%d\n", x0, y0, ncbs, d->poc,
          h265_cabac_pos(&d->cabac));
    d->cu_x = x0; d->cu_y = y0; d->cu_log2 = log2cb;
    d->cu_bypass = 0;
    d->cu_skip = 0;
    d->intra_split = 0;
    d->cu_part_mode = PART_2Nx2N;

    if (pps->transquant_bypass_enabled)
        d->cu_bypass = DEC(CTX_CU_TRANSQUANT_BYPASS);
    if (sl->type != SLICE_I) {
        int inc = 0;
        if (avail_z(d, x0, y0, x0 - 1, y0) && h265_bi(d, x0 - 1, y0)->skip) inc++;
        if (avail_z(d, x0, y0, x0, y0 - 1) && h265_bi(d, x0, y0 - 1)->skip) inc++;
        d->cu_skip = DEC(CTX_CU_SKIP + inc);
    }

    if (d->cu_skip) {
        d->cu_pred_mode = MODE_INTER;
        derive_qp(d);
        bi_t t; memset(&t, 0, sizeof t); t.qp = (int8_t)d->qp_y;
        fill_bi(d, x0, y0, ncbs, ncbs, &t, 2 | 4);
        if ((rc = prediction_unit(d, x0, y0, ncbs, x0, y0, ncbs, ncbs, 0))) return rc;
        /* A skipped CU has no transform tree, but its boundary is still a
         * TRANSFORM block edge -- see the note at the rqt_root_cbf == 0 case
         * below. prediction_unit only marked it as a prediction edge. */
        mark_edges(d, x0, y0, ncbs, ncbs, 1);
        return H265_OK;
    }

    d->cu_pred_mode = MODE_INTRA;
    if (sl->type != SLICE_I)
        d->cu_pred_mode = DEC(CTX_PRED_MODE) ? MODE_INTRA : MODE_INTER;
    if (d->cu_pred_mode != MODE_INTRA || log2cb == sps->log2_min_cb)
        d->cu_part_mode = decode_part_mode(d, log2cb, d->cu_pred_mode == MODE_INTRA);
    d->intra_split = (d->cu_pred_mode == MODE_INTRA && d->cu_part_mode == PART_NxN);

    derive_qp(d);
    {
        bi_t t; memset(&t, 0, sizeof t); t.qp = (int8_t)d->qp_y;
        fill_bi(d, x0, y0, ncbs, ncbs, &t, 2);
    }

    if (d->cu_pred_mode == MODE_INTRA) {
        if (d->cu_part_mode == PART_2Nx2N && sps->pcm_enabled)
            return H265_ERR_UNSUPPORTED;            /* see h265_nal.c */
        int nparts = d->intra_split ? 4 : 1;
        int pboff = d->intra_split ? (ncbs / 2) : ncbs;
        int prev[4], mpm[4] = { 0, 0, 0, 0 }, rem[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < nparts; i++) prev[i] = DEC(CTX_PREV_INTRA_LUMA);
        for (int i = 0; i < nparts; i++) {
            if (prev[i]) {
                mpm[i] = 0;
                if (BYP()) mpm[i] = BYP() ? 2 : 1;
            } else {
                rem[i] = (int)h265_cabac_bypass_n(&d->cabac, 5);
            }
        }
        for (int i = 0; i < nparts; i++) {
            int px = x0 + (i & 1) * pboff, py = y0 + (i >> 1) * pboff;
            d->intra_mode_y[i] = derive_intra_mode(d, px, py, prev[i], mpm[i], rem[i]);
            bi_t t;
            memset(&t, 0, sizeof t);
            t.pred_mode = MODE_INTRA;
            t.intra_mode = (uint8_t)d->intra_mode_y[i];
            t.ct_depth = (uint8_t)(sps->log2_ctb - log2cb);
            t.bypass = (uint8_t)d->cu_bypass;
            t.ref_idx[0] = t.ref_idx[1] = -1;
            fill_bi(d, px, py, pboff, pboff, &t, 1);
        }
        /* 8.4.3 / Table 8-3: chroma takes its mode from block 0's luma mode. */
        int cm = 4;
        if (DEC(CTX_INTRA_CHROMA)) cm = (int)h265_cabac_bypass_n(&d->cabac, 2);
        static const int base[4] = { 0, 26, 10, 1 };
        int ym = d->intra_mode_y[0];
        d->intra_mode_c = (cm == 4) ? ym : (base[cm] == ym ? 34 : base[cm]);
        mark_edges(d, x0, y0, ncbs, ncbs, 0);
        if (d->intra_split) {
            mark_edges(d, x0 + ncbs / 2, y0, ncbs / 2, ncbs, 0);
            mark_edges(d, x0, y0 + ncbs / 2, ncbs, ncbs / 2, 0);
        }
    } else {
        int half = ncbs / 2, q = ncbs / 4;
        switch (d->cu_part_mode) {
        case PART_2Nx2N:
            rc = prediction_unit(d, x0, y0, ncbs, x0, y0, ncbs, ncbs, 0); break;
        case PART_2NxN:
            if ((rc = prediction_unit(d, x0, y0, ncbs, x0, y0, ncbs, half, 0))) break;
            rc = prediction_unit(d, x0, y0, ncbs, x0, y0 + half, ncbs, half, 1); break;
        case PART_Nx2N:
            if ((rc = prediction_unit(d, x0, y0, ncbs, x0, y0, half, ncbs, 0))) break;
            rc = prediction_unit(d, x0, y0, ncbs, x0 + half, y0, half, ncbs, 1); break;
        case PART_2NxnU:
            if ((rc = prediction_unit(d, x0, y0, ncbs, x0, y0, ncbs, q, 0))) break;
            rc = prediction_unit(d, x0, y0, ncbs, x0, y0 + q, ncbs, ncbs - q, 1); break;
        case PART_2NxnD:
            if ((rc = prediction_unit(d, x0, y0, ncbs, x0, y0, ncbs, ncbs - q, 0))) break;
            rc = prediction_unit(d, x0, y0, ncbs, x0, y0 + ncbs - q, ncbs, q, 1); break;
        case PART_nLx2N:
            if ((rc = prediction_unit(d, x0, y0, ncbs, x0, y0, q, ncbs, 0))) break;
            rc = prediction_unit(d, x0, y0, ncbs, x0 + q, y0, ncbs - q, ncbs, 1); break;
        case PART_nRx2N:
            if ((rc = prediction_unit(d, x0, y0, ncbs, x0, y0, ncbs - q, ncbs, 0))) break;
            rc = prediction_unit(d, x0, y0, ncbs, x0 + ncbs - q, y0, q, ncbs, 1); break;
        default:            /* PART_NxN */
            if ((rc = prediction_unit(d, x0, y0, ncbs, x0, y0, half, half, 0))) break;
            if ((rc = prediction_unit(d, x0, y0, ncbs, x0 + half, y0, half, half, 1))) break;
            if ((rc = prediction_unit(d, x0, y0, ncbs, x0, y0 + half, half, half, 2))) break;
            rc = prediction_unit(d, x0, y0, ncbs, x0 + half, y0 + half, half, half, 3);
            break;
        }
        if (rc) return rc;
    }

    /* rqt_root_cbf (7.3.8.5) is absent for an intra CU and for an inter CU that
     * is a single 2Nx2N merge partition. In BOTH cases it is inferred 1
     * (7.4.9.5): the only inference to 0 is cu_skip_flag == 1, and a skipped CU
     * has already returned above. Inferring 0 here instead reads none of a
     * merged CU's residual, which is a CABAC desynchronisation, not a wrong
     * block -- the rest of the slice decodes as garbage that still looks like
     * syntax. It cost every P and B picture in the matrix. */
    int rqt_root_cbf = 1;
    if (d->cu_pred_mode != MODE_INTRA &&
        !(d->cu_part_mode == PART_2Nx2N && d->cu_merge_2nx2n))
        rqt_root_cbf = DEC(CTX_RQT_ROOT_CBF);
    if (rqt_root_cbf) {
        d->max_trafo_depth = (d->cu_pred_mode == MODE_INTRA)
            ? sps->max_transform_hierarchy_depth_intra + d->intra_split
            : sps->max_transform_hierarchy_depth_inter;
        rc = transform_tree(d, x0, y0, x0, y0, log2cb, 0, 0);
        if (rc) return rc;
    } else {
        /* No transform tree, so nothing marked this CU's boundary as a
         * TRANSFORM block edge -- but it is one. 8.7.2.4's second rule raises
         * bS to 1 when "the edge is a transform block edge and p0 or q0 is in a
         * transform block containing a non-zero coefficient", and the two sides
         * of a CU boundary are always different transform blocks. Marking only
         * the prediction edge here loses every bS=1 edge where the residual is
         * entirely on the OTHER side of a merged or residual-free CU: the
         * picture is right except for a thin, low-amplitude error along those
         * edges, which is exactly the kind of thing a PSNR threshold passes. */
        mark_edges(d, x0, y0, ncbs, ncbs, 1);
    }
    return H265_OK;
}

static int coding_quadtree(h265dec *d, int x0, int y0, int log2cb, int depth)
{
    const sps_t *sps = d->cur_sps;
    const pps_t *pps = d->cur_pps;
    int W = sps->width, H = sps->height;
    int size = 1 << log2cb;
    int split;

    if (x0 + size <= W && y0 + size <= H && log2cb > sps->log2_min_cb) {
        int inc = 0;
        if (avail_z(d, x0, y0, x0 - 1, y0) && h265_bi(d, x0 - 1, y0)->ct_depth > depth) inc++;
        if (avail_z(d, x0, y0, x0, y0 - 1) && h265_bi(d, x0, y0 - 1)->ct_depth > depth) inc++;
        split = DEC(CTX_SPLIT_CU + inc);
    } else {
        split = log2cb > sps->log2_min_cb;
    }

    if (pps->cu_qp_delta_enabled &&
        log2cb >= sps->log2_ctb - pps->diff_cu_qp_delta_depth) {
        d->cu_qp_delta_coded = 0;
        d->cu_qp_delta = 0;
        d->qg_x = x0; d->qg_y = y0;
        d->qp_y_prev = d->qp_y_last;
    }

    if (split) {
        int half = size / 2;
        int rc;
        if ((rc = coding_quadtree(d, x0, y0, log2cb - 1, depth + 1))) return rc;
        if (x0 + half < W && (rc = coding_quadtree(d, x0 + half, y0, log2cb - 1, depth + 1))) return rc;
        if (y0 + half < H && (rc = coding_quadtree(d, x0, y0 + half, log2cb - 1, depth + 1))) return rc;
        if (x0 + half < W && y0 + half < H &&
            (rc = coding_quadtree(d, x0 + half, y0 + half, log2cb - 1, depth + 1))) return rc;
        return H265_OK;
    }
    return coding_unit(d, x0, y0, log2cb);
}

/* ============================== slice data ============================== */
static int init_pic_state(h265dec *d)
{
    const sps_t *sps = d->cur_sps;
    d->bw4 = sps->width / 4;
    d->bh4 = sps->height / 4;
    free(d->bi);
    d->bi = (bi_t *)malloc((size_t)d->bw4 * d->bh4 * sizeof(bi_t));
    if (!d->bi) return H265_ERR_OOM;
    memset(d->bi, 0, (size_t)d->bw4 * d->bh4 * sizeof(bi_t));
    for (int i = 0; i < d->bw4 * d->bh4; i++) {
        d->bi[i].slice_idx = 0xFFFF;
        d->bi[i].tile_idx = 0xFFFF;
        d->bi[i].qp = (int8_t)d->sh.qp;
    }
    free(d->sao);
    d->sao = (sao_t *)malloc((size_t)sps->ctb_count * sizeof(sao_t));
    free(d->ctb_deblock_disabled);
    d->ctb_deblock_disabled = (uint8_t *)malloc((size_t)sps->ctb_count);
    free(d->ctb_beta); d->ctb_beta = (int8_t *)malloc((size_t)sps->ctb_count);
    free(d->ctb_tc);   d->ctb_tc = (int8_t *)malloc((size_t)sps->ctb_count);
    free(d->ctb_filt_across_slice);
    d->ctb_filt_across_slice = (uint8_t *)malloc((size_t)sps->ctb_count);
    if (!d->sao || !d->ctb_deblock_disabled || !d->ctb_beta || !d->ctb_tc ||
        !d->ctb_filt_across_slice) return H265_ERR_OOM;
    memset(d->sao, 0, (size_t)sps->ctb_count * sizeof(sao_t));
    memset(d->ctb_deblock_disabled, 0, (size_t)sps->ctb_count);
    memset(d->ctb_beta, 0, (size_t)sps->ctb_count);
    memset(d->ctb_tc, 0, (size_t)sps->ctb_count);
    memset(d->ctb_filt_across_slice, 1, (size_t)sps->ctb_count);

    if (sps->sao_enabled) {
        int need = d->stride_y * (sps->height + 2 * H265_PAD) +
                   2 * d->stride_c * (sps->height / 2 + 2 * H265_PAD);
        free(d->sao_src);
        d->sao_src = (uint16_t *)malloc((size_t)need * sizeof(uint16_t));
        if (!d->sao_src) return H265_ERR_OOM;
    }
    return H265_OK;
}

/* Decode the CTUs of one slice segment. `rbsp` is the whole slice NAL's RBSP;
 * substream boundaries come from the entry point offsets, corrected for the
 * emulation prevention bytes that were stripped out of them. */
static int decode_slice_data(h265dec *d, const uint8_t *rbsp, int rbsp_len,
                             const int *epb, int n_epb)
{
    const sps_t *sps = d->cur_sps;
    const pps_t *pps = d->cur_pps;
    slice_t *sl = &d->sh;
    int init_type;
    if (sl->type == SLICE_I) init_type = 0;
    else if (sl->type == SLICE_P) init_type = sl->cabac_init_flag ? 2 : 1;
    else init_type = sl->cabac_init_flag ? 1 : 2;

    /* Substream table. */
    int sub_off[H265_MAX_SUBSTREAMS + 1];
    int nsub = sl->num_entry_points + 1;
    sub_off[0] = (sl->header_bits + 7) / 8;
    for (int i = 0; i < sl->num_entry_points; i++) {
        int cmpt = 0, end = sub_off[i] + (int)sl->entry_point_offset[i];
        for (int j = 0; j < n_epb; j++)
            if (epb[j] >= sub_off[i] && epb[j] < end) { end--; cmpt++; }
        sub_off[i + 1] = sub_off[i] + (int)sl->entry_point_offset[i] - cmpt;
        if (sub_off[i + 1] > rbsp_len) return H265_ERR_CORRUPT;
    }

    int sub = 0;
    int rc = h265_cabac_start(&d->cabac, rbsp, rbsp_len, sub_off[0]);
    if (rc) return rc;
    h265_cabac_init_ctx(&d->cabac, init_type, sl->qp);
    d->wpp_saved = 0;
    d->qp_y_last = sl->qp;
    d->qp_y = sl->qp;

    int ts = d->ctb_addr_ts;
    for (;;) {
        int rs = d->ctb_ts_to_rs[ts];
        d->ctb_addr_ts = ts;
        d->ctb_addr_rs = rs;
        d->cur_tile = d->ctb_tile_id[ts];
        int rx = rs % sps->ctb_width, ry = rs / sps->ctb_width;
        d->ctu_x = rx << sps->log2_ctb;
        d->ctu_y = ry << sps->log2_ctb;

        /* A new tile, or a new CTB row under wavefront sync, starts its own
         * substream: re-init the arithmetic decoder at the entry point, and
         * either reset the contexts (tile) or restore the ones saved after
         * the second CTB of the row above (WPP, 9.3.1). */
        int new_tile = (ts > 0 && d->ctb_tile_id[ts] != d->ctb_tile_id[ts - 1]);
        int new_row = pps->entropy_coding_sync_enabled && rx == 0 &&
                      ts != d->seg_addr_ts;
        if ((new_tile || new_row) && ts != d->seg_addr_ts) {
            if (++sub >= nsub) return H265_ERR_CORRUPT;
            rc = h265_cabac_start(&d->cabac, rbsp, rbsp_len, sub_off[sub]);
            if (rc) return rc;
            if (new_tile) {
                h265_cabac_init_ctx(&d->cabac, init_type, sl->qp);
            } else if (d->wpp_saved) {
                memcpy(d->cabac.state, d->cabac_wpp_save.state, H265_NCTX);
            } else {
                h265_cabac_init_ctx(&d->cabac, init_type, sl->qp);
            }
            d->qp_y_last = sl->qp;
        }

        d->ctb_deblock_disabled[rs] = (uint8_t)sl->deblocking_filter_disabled;
        d->ctb_beta[rs] = (int8_t)sl->beta_offset;
        d->ctb_tc[rs] = (int8_t)sl->tc_offset;
        d->ctb_filt_across_slice[rs] = (uint8_t)sl->loop_filter_across_slices;

        if (sl->sao_luma || sl->sao_chroma) parse_sao(d, rx, ry);

        d->cu_qp_delta_coded = 0;
        d->cu_qp_delta = 0;
        d->qg_x = d->ctu_x; d->qg_y = d->ctu_y;
        d->qp_y_prev = d->qp_y_last;

        rc = coding_quadtree(d, d->ctu_x, d->ctu_y, sps->log2_ctb, 0);
        if (rc) return rc;
        if (d->cabac.error) return H265_ERR_CORRUPT;

        /* WPP saves its contexts after the SECOND CTB of a row. */
        if (pps->entropy_coding_sync_enabled && rx == 1) {
            memcpy(d->cabac_wpp_save.state, d->cabac.state, H265_NCTX);
            d->wpp_saved = 1;
        }

        int end_of_slice = h265_cabac_terminate(&d->cabac);
        ts++;
        if (end_of_slice) break;
        if (ts >= sps->ctb_count) break;
        /* end_of_sub_stream_one_bit precedes a tile or WPP row change. */
        int nrs = d->ctb_ts_to_rs[ts];
        int nrx = nrs % sps->ctb_width;
        if ((pps->tiles_enabled && d->ctb_tile_id[ts] != d->ctb_tile_id[ts - 1]) ||
            (pps->entropy_coding_sync_enabled && nrx == 0))
            h265_cabac_terminate(&d->cabac);
    }
    d->ctb_addr_ts = ts;
    TRACE("SLICE poc%d type%d qp%d ctus %d/%d cabac %d/%d\n", d->poc, sl->type,
          sl->qp, ts, sps->ctb_count, h265_cabac_pos(&d->cabac), rbsp_len);
    return H265_OK;
}

/* ============================ picture assembly ========================== */
static void compress_motion(h265dec *d)
{
    pic_t *p = d->cur;
    const sps_t *sps = d->cur_sps;
    for (int cy = 0; cy < p->col_h; cy++)
        for (int cx = 0; cx < p->col_w; cx++) {
            int x = cx * 16, y = cy * 16;
            colmv_t *c = &p->col[cy * p->col_w + cx];
            memset(c, 0, sizeof *c);
            if (x >= sps->width || y >= sps->height) { c->intra = 1; continue; }
            const bi_t *b = h265_bi(d, x, y);
            if (b->pred_mode == MODE_INTRA || b->pred_flag == 0) { c->intra = 1; continue; }
            for (int l = 0; l < 2; l++)
                if (b->pred_flag & (1 << l)) {
                    c->refpoc_valid[l] = 1;
                    c->refpoc[l] = b->refpoc[l];
                    c->islt[l] = b->islt[l];
                    c->mv[l][0] = b->mv[l][0];
                    c->mv[l][1] = b->mv[l][1];
                }
        }
}

static void finish_picture(h265dec *d)
{
    const sps_t *sps = d->cur_sps;
    pic_t *cur = d->cur;
    if (!cur) return;

    h265_deblock_pic(d);
    if (sps->sao_enabled) h265_sao_pic(d);

    border_pad_plane(cur->y, cur->stride_y, sps->width, sps->height);
    border_pad_plane(cur->u, cur->stride_c, sps->width / 2, sps->height / 2);
    border_pad_plane(cur->v, cur->stride_c, sps->width / 2, sps->height / 2);
    compress_motion(d);

    cur->reference = 1;
    cur->output = cur->output_flag;
    d->cur = 0;
    queue_output(d);
}

/* Start a new picture from the first slice segment of it. */
static int start_picture(h265dec *d, slice_t *sl, int nal_type)
{
    const sps_t *sps = d->cur_sps;
    if (d->pic_w != sps->width || d->pic_h != sps->height ||
        d->stride_y != sps->width + 2 * H265_PAD) {
        for (int i = 0; i < H265_MAX_DPB; i++) release_pic(d, i);
        d->n_outq = 0;
        d->to_free = -1;
        d->pic_w = sps->width;
        d->pic_h = sps->height;
        d->stride_y = sps->width + 2 * H265_PAD;
        d->stride_c = sps->width / 2 + 2 * H265_PAD;
    }
    d->max_poc_lsb = 1 << sps->log2_max_poc_lsb;
    d->poc = compute_poc(d, sl, nal_type);

    if (IS_IRAP(nal_type) && (IS_IDR(nal_type) || IS_BLA(nal_type) || d->first_picture)) {
        if (!sl->no_output_of_prior_pics || IS_IDR(nal_type))
            flush_all_output(d);
        else
            for (int i = 0; i < H265_MAX_DPB; i++) d->pics[i].output = 0;
    }

    int rc = build_geom(d);
    if (rc) return rc;
    rc = alloc_picture(d);
    if (rc) return rc;
    d->cur->poc = d->poc;
    d->cur->output_flag = sl->pic_output_flag && !(IS_RASL(nal_type) && d->first_picture);
    d->cur->reference = 1;
    d->slice_idx = 0;
    d->ctb_addr_ts = 0;

    rc = init_pic_state(d);
    if (rc) return rc;

    /* prevTid0Pic bookkeeping for the next picture's POC (8.3.1). */
    if (!IS_RASL(nal_type) && !IS_SUBLAYER_NONREF(nal_type)) {
        d->prev_poc_lsb = d->poc & (d->max_poc_lsb - 1);
        d->prev_poc_msb = d->poc - d->prev_poc_lsb;
    }
    d->first_picture = 0;
    d->cur_nal_type = nal_type;
    return H265_OK;
}

/* ============================== NAL dispatch ============================ */
static int find_start_code(const uint8_t *p, int n)
{
    for (int i = 0; i + 2 < n; i++)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) return i;
    return -1;
}

h265dec *h265_open(void)
{
    h265dec *d = (h265dec *)malloc(sizeof *d);
    if (!d) return 0;
    memset(d, 0, sizeof *d);
    d->first_picture = 1;
    d->to_free = -1;
    return d;
}

void h265_close(h265dec *d)
{
    if (!d) return;
    for (int i = 0; i < H265_MAX_DPB; i++) release_pic(d, i);
    free_geom(d);
    free(d->bi);
    free(d->sao);
    free(d->ctb_deblock_disabled);
    free(d->ctb_beta);
    free(d->ctb_tc);
    free(d->ctb_filt_across_slice);
    free(d->sao_src);
    free(d->disp);
    free(d);
}

static int handle_slice(h265dec *d, const uint8_t *rbsp, int rbsp_len,
                        const int *epb, int n_epb, int nal_type, int tid,
                        int *completed)
{
    bs_t bs;
    bs_init(&bs, rbsp, rbsp_len);
    slice_t sl;
    int rc = h265_parse_slice_header(d, &bs, nal_type, tid, &sl);
    if (rc) return rc;

    if (sl.first_slice_in_pic && d->cur) {
        /* Should not happen: the caller finishes the picture first. */
        return H265_ERR_CORRUPT;
    }
    if (sl.dependent_slice_segment) {
        if (!d->cur) return H265_ERR_CORRUPT;
        int seg = sl.segment_address;
        int nep = sl.num_entry_points;
        uint32_t saved[H265_MAX_SUBSTREAMS];
        memcpy(saved, sl.entry_point_offset, sizeof(uint32_t) * (size_t)nep);
        int hb = sl.header_bits;
        sl = d->sh;                       /* inherit the independent header */
        sl.dependent_slice_segment = 1;
        sl.segment_address = seg;
        sl.num_entry_points = nep;
        memcpy(sl.entry_point_offset, saved, sizeof(uint32_t) * (size_t)nep);
        sl.header_bits = hb;
        d->sh = sl;
        d->seg_addr_rs = seg;
        d->seg_addr_ts = d->ctb_rs_to_ts[seg];
        d->ctb_addr_ts = d->seg_addr_ts;
    } else {
        if (sl.first_slice_in_pic) {
            rc = start_picture(d, &sl, nal_type);
            if (rc) return rc;
            d->sh = sl;
            d->seg_addr_rs = 0;
            d->seg_addr_ts = 0;
            d->slice_idx = 0;
            rc = apply_rps(d, &sl, nal_type);
            if (rc) return rc;
        } else {
            if (!d->cur) return H265_ERR_CORRUPT;
            d->sh = sl;
            d->slice_idx++;
            d->seg_addr_rs = sl.segment_address;
            d->seg_addr_ts = d->ctb_rs_to_ts[sl.segment_address];
            d->ctb_addr_ts = d->seg_addr_ts;
            rc = apply_rps(d, &sl, nal_type);
            if (rc) return rc;
        }
        d->slice_addr_rs = d->seg_addr_rs;
    }

    /* The collocated picture for temporal MV prediction. */
    d->col_pic = 0;
    if (d->sh.temporal_mvp_enabled && d->sh.type != SLICE_I) {
        int l = d->sh.collocated_from_l0 ? 0 : 1;
        if (d->sh.collocated_ref_idx < d->nb_refs[l])
            d->col_pic = d->ref_list[l][d->sh.collocated_ref_idx];
    }

    rc = decode_slice_data(d, rbsp, rbsp_len, epb, n_epb);
    if (rc) return rc;
    if (d->ctb_addr_ts >= d->cur_sps->ctb_count) {
        finish_picture(d);
        *completed = 1;
    }
    return H265_OK;
}

int h265_decode(h265dec *d, const uint8_t *data, int len,
                h265frame *out, int *got_frame)
{
    if (!d || !data || !got_frame || !out || len < 0) return H265_ERR_CORRUPT;
    *got_frame = 0;
    if (emit(d, out)) { *got_frame = 1; return 0; }

    int pos = 0;
    while (pos < len) {
        int sc = find_start_code(data + pos, len - pos);
        if (sc < 0) return len;
        int nal = pos + sc + 3;
        if (nal + 1 >= len) return len;
        int next = find_start_code(data + nal, len - nal);
        int nend = next < 0 ? len : nal + next;
        while (nend > nal + 2 && data[nend - 1] == 0) nend--;

        int type = (data[nal] >> 1) & 63;
        int tid = (data[nal + 1] & 7) - 1;
        if (data[nal] & 0x80) return H265_ERR_CORRUPT;   /* forbidden_zero_bit */

        int rbsp_len = 0, n_epb = 0;
        static int epb[4096];
        uint8_t *rbsp = 0;
        int rc = H265_OK;

        if (type == NAL_VPS || type == NAL_SPS || type == NAL_PPS || IS_VCL(type)) {
            rbsp = h265_nal_to_rbsp(data + nal, nend - nal, &rbsp_len,
                                    epb, 4096, &n_epb);
            if (!rbsp) return H265_ERR_OOM;
        }

        if (type == NAL_VPS || type == NAL_SPS || type == NAL_PPS) {
            bs_t bs;
            bs_init(&bs, rbsp, rbsp_len);
            if (type == NAL_VPS)      rc = h265_parse_vps(d, &bs);
            else if (type == NAL_SPS) rc = h265_parse_sps(d, &bs);
            else                      rc = h265_parse_pps(d, &bs);
        } else if (IS_VCL(type)) {
            /* Does this NAL start a new picture? first_slice_segment_in_pic
             * is the very first bit of the slice header. */
            int first = (rbsp_len > 0) && (rbsp[0] & 0x80);
            if (first && d->cur) {
                finish_picture(d);
                free(rbsp);
                if (emit(d, out)) { *got_frame = 1; return pos + sc; }
                pos = pos + sc;
                continue;
            }
            int completed = 0;
            rc = handle_slice(d, rbsp, rbsp_len, epb, n_epb, type, tid, &completed);
            if (rc == H265_OK && completed) {
                free(rbsp);
                pos = nend;
                if (emit(d, out)) { *got_frame = 1; return pos; }
                continue;
            }
        } else if (type == NAL_EOS || type == NAL_EOB) {
            if (d->cur) finish_picture(d);
            flush_all_output(d);
            d->first_picture = 1;
        }
        free(rbsp);
        if (rc) return rc;
        pos = nend;
    }
    return len;
}

int h265_flush(h265dec *d, h265frame *out)
{
    if (!d || !out) return H265_ERR_CORRUPT;
    if (d->cur) finish_picture(d);
    if (d->n_outq == 0) flush_all_output(d);
    return emit(d, out) ? 1 : 0;
}

int h265_stream_info(h265dec *d, int *w, int *h, double *fps)
{
    if (!d) return H265_ERR_CORRUPT;
    for (int i = 0; i < H265_MAX_SPS; i++) {
        if (!d->sps[i].present) continue;
        const sps_t *s = &d->sps[i];
        if (w) *w = s->width - (s->conf_win[0] + s->conf_win[1]) * 2;
        if (h) *h = s->height - (s->conf_win[2] + s->conf_win[3]) * 2;
        if (fps)
            *fps = s->vui_timing && s->num_units_in_tick
                 ? (double)s->time_scale / (double)s->num_units_in_tick : 0.0;
        return H265_OK;
    }
    return H265_ERR_CORRUPT;
}
