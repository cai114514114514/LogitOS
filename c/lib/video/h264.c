/* c/lib/video/h264.c -- orchestrator: Annex B scanning, NAL dispatch, DPB
 * management, slice/macroblock decode loops, POC.  Drives the leaf modules
 * (CAVLC / intra pred+IDCT / MC / deblock) per the contracts in h264_int.h.
 *
 * Design notes:
 *  - Pull model: h264_decode() consumes NAL units until a picture COMPLETES
 *    (signalled by the first slice of the NEXT picture) or the input runs
 *    out.  Output is in decode order, which equals presentation order for
 *    every stream this decoder accepts (B slices are rejected, so no
 *    reordering is possible); h264_flush() finishes the trailing picture.
 *  - Reference frames are kept in d->pics[] with H264_PAD-pixel replicated
 *    borders on all four sides (border_pad).  MC reads inside the pad
 *    directly; MVs that reach beyond the pad go through a clamped
 *    edge-emulation scratch (mc_plane), so every input is in bounds.
 *  - Neighbour pixels for intra prediction are read straight out of the
 *    frame under construction (reconstruction is raster-ordered and the
 *    deblock post-pass runs only after the frame is complete).
 *  - Per-frame mbinfo_t array feeds the deblock post-pass and the CAVLC/MV
 *    neighbour contexts.  Cross-slice neighbours are "unavailable".
 *  - Limitation: deblock alpha/beta offsets and the filter-disable flag are
 *    taken from the LAST slice of the frame (per-slice offsets are legal but
 *    essentially never produced by baseline encoders).
 */ 
#include <stdlib.h>
#include <string.h>
#include "h264.h"
#include "h264_int.h"

#ifdef H264_TRACE
#ifndef H264_PREDDBG_MB
#define H264_PREDDBG_MB (-1)
#define H264_PREDDBG_BLK (-1)
#endif
#include <stdio.h>
#define TRACE(...) fprintf(stderr, __VA_ARGS__)
#else
#define TRACE(...)
#endif

uint8_t *h264_nal_to_rbsp(const uint8_t *nal, int len, int *rbsp_len);

/* ---------------------------------------------------------------- tables -- */
/* zigzag scan position -> raster index (identical to h264_pred.c's zz4) */
static const uint8_t zz4[16] = {
    0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
};
/* luma4x4BlkIdx <-> 4x4 grid position (spec 6.4.3): the 16 luma blocks of
 * a MB are NOT in raster order; they zigzag at both the 8x8 and the 4x4
 * level: 0,1,4,5 / 2,3,6,7 / 8,9,12,13 / 10,11,14,15.  i4x/i4y map a
 * block index to its (x,y) in 4x4 units; i4_inv maps (by*4+bx) back to
 * the block index. */
static const uint8_t i4x[16] = { 0,1,0,1, 2,3,2,3, 0,1,0,1, 2,3,2,3 };
static const uint8_t i4y[16] = { 0,0,1,1, 0,0,1,1, 2,2,3,3, 2,2,3,3 };
static const uint8_t i4_inv[16] = { 0,1,4,5, 2,3,6,7, 8,9,12,13, 10,11,14,15 };
/* LevelScale v-table (spec Table 8-16), identical to h264_pred.c's vq */
static const int16_t vq[6][3] = {
    { 10, 16, 13 }, { 11, 18, 14 }, { 13, 20, 16 },
    { 14, 23, 18 }, { 16, 25, 20 }, { 18, 29, 23 }
};
/* codeNum -> coded_block_pattern (spec Table 9-4) */
static const uint8_t cbp_intra_tab[48] = {
    47, 31, 15,  0, 23, 27, 29, 30,  7, 11, 13, 14, 39, 43, 45, 46,
    16,  3,  5, 10, 12, 19, 21, 26, 28, 35, 37, 42, 44,  1,  2,  4,
     8, 17, 18, 20, 24,  6,  9, 22, 25, 32, 33, 34, 36, 40, 38, 41
};
static const uint8_t cbp_inter_tab[48] = {
     0, 16,  1,  2,  4,  8, 32,  3,  5, 10, 12, 15, 47,  7, 11, 13,
    14,  6,  9, 31, 35, 37, 42, 44, 33, 34, 36, 40, 39, 43, 45, 46,
    17, 18, 20, 24, 19, 21, 26, 28, 23, 27, 29, 30, 22, 25, 38, 41
};
/* qP 30..51 -> qP_C (spec Table 8-15) */
static const int8_t chroma_qp_map[22] = {
    29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36, 36,
    37, 37, 37, 38, 38, 38, 39, 39, 39, 39
};

/* --------------------------------------------------------------- helpers -- */
static int clip3(int lo, int hi, int v) { return v < lo ? lo : (v > hi ? hi : v); }
static int median3(int a, int b, int c)
{
    if (a > b) { int t = a; a = b; b = t; }
    if (b > c) { int t = b; b = c; c = t; }
    if (a > b) { int t = a; a = b; b = t; }
    return b;
}
static int vclass(int i, int j)
{
    if ((i & 1) == 0 && (j & 1) == 0) return 0;
    if ((i & 1) && (j & 1)) return 1;
    return 2;
}
static int chroma_qp(int qpy, int offset)
{
    int qpi = clip3(0, 51, qpy + offset);
    return qpi < 30 ? qpi : chroma_qp_map[qpi - 30];
}
/* floor(v / 2). Written out because >> of a negative value is
 * implementation-defined, and because rounding direction matters here. */
static int half_floor(int v) { return v >= 0 ? v >> 1 : -((-v + 1) >> 1); }
static int is_intra_type(int t) { return t == MB_I4x4 || t == MB_I16x16 || t == MB_I_PCM; }

/* slice index of a macroblock address (slice_first_mb is raster-ordered) */
static int slice_of(const h264dec *d, int addr)
{
    int s = 0;
    for (int i = 1; i < d->n_slices; i++) {
        if (addr >= d->slice_first_mb[i]) s = i;
        else break;
    }
    return s;
}

/* more_rbsp_data(): shared inline in bs.h (bs_more_rbsp_data). */
static int more_rbsp_data(const bs_t *bs)
{
    return bs_more_rbsp_data(bs);
}

/* ------------------------------------------------------- picture storage -- */
static void release_pic(h264dec *d, int i)
{
    pic_t *p = &d->pics[i];
    if (!p->used) return;
    if (p->y) free(p->y - (long)H264_PAD * p->stride_y - H264_PAD);
    memset(p, 0, sizeof *p);
}

static void clear_dpb(h264dec *d)
{
    for (int i = 0; i < MAX_DPB; i++)
        if (d->pics[i].used && &d->pics[i] != d->cur) release_pic(d, i);
}

static int alloc_picture(h264dec *d)
{
    int slot = -1;
    for (int i = 0; i < MAX_DPB; i++)
        if (!d->pics[i].used) { slot = i; break; }
    if (slot < 0) return H264_ERR_OOM;   /* marking keeps <= 16 refs + cur */

    int ly = d->stride_y * (d->mbh * 16 + 2 * H264_PAD);
    int lc = d->stride_c * (d->mbh * 8 + 2 * H264_PAD);
    uint8_t *base = (uint8_t *)malloc((size_t)(ly + 2 * lc));
    if (!base) return H264_ERR_OOM;
    memset(base, 0, (size_t)(ly + 2 * lc));    /* deterministic for partial frames */

    pic_t *p = &d->pics[slot];
    p->y = base + (long)H264_PAD * d->stride_y + H264_PAD;
    p->u = base + ly + (long)H264_PAD * d->stride_c + H264_PAD;
    p->v = base + ly + lc + (long)H264_PAD * d->stride_c + H264_PAD;
    p->stride_y = d->stride_y;
    p->stride_c = d->stride_c;
    p->used = 1;
    p->reference = 0;
    d->cur = p;

    /* The per-picture mbinfo array must start ZEROED. A macroblock only writes
     * the fields it owns -- nz[] of uncoded blocks and mv[]/ref_idx[] of intra
     * macroblocks are left alone -- and its neighbours read those back when
     * deriving nC for CAVLC and the deblocking boundary strength. This was
     * malloc'd, and since finish_picture frees a block of exactly this size
     * every picture, the allocator handed the SAME chunk back on the next one:
     * each frame silently inherited the previous frame's macroblock state. */
    size_t mbbytes = (size_t)d->mbw * d->mbh * sizeof(mbinfo_t);
    d->mb = (mbinfo_t *)malloc(mbbytes);
    if (!d->mb) { release_pic(d, slot); d->cur = 0; return H264_ERR_OOM; }
    memset(d->mb, 0, mbbytes);   /* malloc+memset, not calloc: mini-libc has
                                  * no calloc and this file is headed there */
    return H264_OK;
}

/* Replicate the visible edges into the H264_PAD border (MC pad). */
static void border_pad_plane(uint8_t *vis, int stride, int w, int h)
{
    for (int i = 1; i <= H264_PAD; i++) {
        memcpy(vis - (long)i * stride, vis, (size_t)w);
        memcpy(vis + (long)(h - 1 + i) * stride, vis + (long)(h - 1) * stride, (size_t)w);
    }
    for (int r = -H264_PAD; r < h + H264_PAD; r++) {
        uint8_t *row = vis + (long)r * stride;
        for (int i = 1; i <= H264_PAD; i++) {
            row[-i] = row[0];
            row[w - 1 + i] = row[w - 1];
        }
    }
}

static void border_pad(h264dec *d, pic_t *p)
{
    border_pad_plane(p->y, p->stride_y, d->mbw * 16, d->mbh * 16);
    border_pad_plane(p->u, p->stride_c, d->mbw * 8, d->mbh * 8);
    border_pad_plane(p->v, p->stride_c, d->mbw * 8, d->mbh * 8);
}

/* --------------------------------------------------------------------- POC */
static int compute_poc(h264dec *d, const slice_t *sl, int nal_ref_idc, int nal_type)
{
    const sps_t *sps = d->cur_sps;
    if (sps->poc_type == 0) {
        int maxlsb = 1 << sps->log2_max_poc_lsb;
        if (nal_type == 5) { d->prev_poc_msb = 0; d->prev_poc_lsb = 0; }
        int lsb = sl->poc_lsb, msb;
        if (lsb < d->prev_poc_lsb && d->prev_poc_lsb - lsb >= maxlsb / 2)
            msb = d->prev_poc_msb + maxlsb;
        else if (lsb > d->prev_poc_lsb && lsb - d->prev_poc_lsb > maxlsb / 2)
            msb = d->prev_poc_msb - maxlsb;
        else
            msb = d->prev_poc_msb;
        if (nal_ref_idc) { d->prev_poc_msb = msb; d->prev_poc_lsb = lsb; }
        int poc = msb + lsb;
        if (d->cur_pps->bottom_field_poc_in_frame) poc += sl->delta_poc_bottom;
        return poc;
    }
    /* poc_type 1 (zero cycle, optionally delta) and 2: decode-order based */
    int fn = d->frame_num_offset + sl->frame_num;
    int poc = 2 * fn - (nal_ref_idc ? 0 : 1);
    if (sps->poc_type == 1 && !sps->delta_pic_order_always_zero)
        poc += sl->delta_poc[0];
    return poc;
}

/* --------------------------------------------------- reference marking --- */
static int find_short(const h264dec *d, int frame_num)
{
    for (int i = 0; i < MAX_DPB; i++)
        if (d->pics[i].used && d->pics[i].reference == 1 &&
            d->pics[i].frame_num == frame_num && &d->pics[i] != d->cur)
            return i;
    return -1;
}

static void mark_refs(h264dec *d)
{
    const slice_t *sl = &d->last_slice;
    pic_t *cur = d->cur;
    int maxfn = 1 << d->cur_sps->log2_max_frame_num;

    if (!d->cur_ref) return;

    if (d->cur_idr) {
        clear_dpb(d);
        if (sl->long_term_reference_flag) {
            cur->reference = 2; cur->lt_idx = 0;
            d->max_long_term_idx = 0;
        } else {
            cur->reference = 1;
        }
        cur->frame_num = sl->frame_num;
        return;
    }

    if (sl->adaptive_marking) {
        for (int c = 0; c < sl->n_mmco; c++) {
            int mmco = sl->mmco[c][0], arg = sl->mmco[c][1];
            if (mmco == 1) {                       /* short-term -> unused */
                int picnum = ((sl->frame_num - (arg + 1)) % maxfn + maxfn) % maxfn;
                int i = find_short(d, picnum);
                if (i >= 0) release_pic(d, i);
            } else if (mmco == 2) {                /* long-term -> unused */
                for (int i = 0; i < MAX_DPB; i++)
                    if (d->pics[i].used && d->pics[i].reference == 2 &&
                        d->pics[i].lt_idx == arg) release_pic(d, i);
            } else if (mmco == 3) {                /* short -> long assign */
                int picnum = ((sl->frame_num - ((arg & 0xffff) + 1)) % maxfn + maxfn) % maxfn;
                int i = find_short(d, picnum);
                if (i >= 0) { d->pics[i].reference = 2; d->pics[i].lt_idx = arg >> 16; }
            } else if (mmco == 4) {                /* max_long_term_frame_idx */
                int maxlt = arg - 1;
                for (int i = 0; i < MAX_DPB; i++)
                    if (d->pics[i].used && d->pics[i].reference == 2 &&
                        d->pics[i].lt_idx > maxlt) release_pic(d, i);
                d->max_long_term_idx = maxlt;
            } else if (mmco == 5) {                /* clear all */
                clear_dpb(d);
                d->prev_poc_msb = 0; d->prev_poc_lsb = 0;
                d->max_long_term_idx = -1;
            } else if (mmco == 6) {                /* current -> long-term */
                cur->reference = 2; cur->lt_idx = arg >> 16;
                d->max_long_term_idx = arg >> 16;
            }
        }
    }
    if (cur->reference == 0) {
        cur->reference = 1;                        /* sliding window marks short */
        cur->frame_num = sl->frame_num;
    }

    /* sliding window: enforce max_num_ref_frames */
    int maxrefs = clip3(1, 16, d->cur_sps->max_num_ref_frames);
    for (;;) {
        int count = 0;
        for (int i = 0; i < MAX_DPB; i++)
            if (d->pics[i].used && d->pics[i].reference) count++;
        if (count <= maxrefs) break;
        int oldest = -1, odist = -1;
        for (int i = 0; i < MAX_DPB; i++) {
            if (!d->pics[i].used || d->pics[i].reference != 1 ||
                &d->pics[i] == d->cur) continue;
            int dist = ((sl->frame_num - d->pics[i].frame_num) % maxfn + maxfn) % maxfn;
            if (dist > odist) { odist = dist; oldest = i; }
        }
        if (oldest < 0) break;                     /* all long-term: encoder's problem */
        release_pic(d, oldest);
    }
}

/* ------------------------------------------------------- new/finish frame */
static int new_picture(h264dec *d, const slice_t *sl, int nal_ref_idc, int nal_type)
{
    const sps_t *sps = d->cur_sps;
    if (d->mbw != sps->mb_width || d->mbh != sps->mb_height) {
        for (int i = 0; i < MAX_DPB; i++) release_pic(d, i);
        d->mbw = sps->mb_width;
        d->mbh = sps->mb_height;
        d->stride_y = d->mbw * 16 + 2 * H264_PAD;
        d->stride_c = d->mbw * 8 + 2 * H264_PAD;
        int cw = sps->crop_flag ? (sps->crop[0] + sps->crop[1]) * 2 : 0;
        int ch = sps->crop_flag ? (sps->crop[2] + sps->crop[3]) * 2 : 0;
        d->width = d->mbw * 16 - cw;
        d->height = d->mbh * 16 - ch;
        if (d->width <= 0 || d->height <= 0) return H264_ERR_CORRUPT;
    }
    int rc = alloc_picture(d);
    if (rc) return rc;
    d->cur->poc = compute_poc(d, sl, nal_ref_idc, nal_type);
    d->cur->frame_num = sl->frame_num;
    d->n_slices = 0;
    d->cur_idr = (nal_type == 5);
    d->cur_ref = (nal_ref_idc != 0);
    return H264_OK;
}

static void finish_picture(h264dec *d, h264frame *out)
{
    pic_t *cur = d->cur;
    if (!cur) return;
    if (d->n_slices > 0 && d->last_slice.disable_deblocking_filter_idc != 1)
        h264_deblock_frame(cur->y, cur->u, cur->v, d->stride_y, d->stride_c,
                           d->mbw, d->mbh, d->mb, &d->last_slice,
                           d->cur_sps, d->cur_pps,
                           d->slice_first_mb, d->n_slices);
    border_pad(d, cur);
    mark_refs(d);

    out->width = d->width;
    out->height = d->height;
    out->stride_y = d->stride_y;
    out->stride_c = d->stride_c;
    out->y = cur->y;
    out->u = cur->u;
    out->v = cur->v;
    out->poc = cur->poc;

    free(d->mb); d->mb = 0;
    d->cur = 0;
    if (cur->reference == 0) {
        /* non-reference: the slot can be recycled by the next picture; the
         * output pointers stay valid until the next h264_decode() call. */
        for (int i = 0; i < MAX_DPB; i++)
            if (&d->pics[i] == cur) { release_pic(d, i); break; }
    }
    d->n_slices = 0;
}

/* ============================================================ neighbours == */
typedef struct { int avail; int ref; int mvx, mvy; } nb_t;

/* Inter-prediction neighbour of the 4x4 block at global grid (gx, gy).
 *
 * `avail` answers "is mbAddrN available" in the spec's sense -- inside the
 * picture, same slice, already decoded -- and NOT "does it have a motion
 * vector". An INTRA neighbour is available; 6.4.11.7 just gives it refIdx -1
 * and mv (0, 0). The two are not interchangeable, because two rules test
 * availability itself rather than refIdx:
 *   - P_Skip (8.4.1.1) forces mv (0,0) when A or B is UNAVAILABLE. Treating an
 *     intra neighbour as unavailable pins skipped macroblocks next to intra
 *     ones at zero motion instead of letting them predict.
 *   - the median (8.4.1.3.1) substitutes A for B and C only when B and C are
 *     both unavailable.
 * Everywhere else the two cases coincide, since an unavailable neighbour also
 * contributes refIdx -1 and mv (0, 0). */
static nb_t get_nb(h264dec *d, int cur_addr, int gx, int gy)
{
    nb_t r = { 0, -1, 0, 0 };
    if (gx < 0 || gy < 0 || gx >= d->mbw * 4 || gy >= d->mbh * 4) return r;
    int addr = (gy >> 2) * d->mbw + (gx >> 2);
    /* A macroblock that comes LATER in decoding order is not a neighbour
     * (spec 6.4.9): it has not been decoded, so it has no motion vector to
     * predict from. This matters for mvp's C neighbour, which sits at
     * (px + pw, py - 1) and therefore lands in the macroblock to the RIGHT
     * whenever the partition touches the right edge of its own macroblock --
     * the fourth 8x8 of a P_8x8, the lower half of a 16x8, every sub-partition
     * in the right-hand column. Those must fall back to the D substitution.
     * Without this test C came back "available" holding the zeroed state of a
     * macroblock not yet decoded, i.e. a plausible-looking ref 0 / mv (0,0),
     * which quietly dragged the median prediction toward zero. Intra streams
     * never notice -- they do no motion prediction at all. */
    if (addr > cur_addr) return r;
    if (addr == cur_addr &&
        !(d->mb_mv_done & (uint16_t)(1u << ((gy & 3) * 4 + (gx & 3)))))
        return r;                              /* same MB, not decoded yet */
    if (slice_of(d, addr) != slice_of(d, cur_addr)) return r;
    const mbinfo_t *m = &d->mb[addr];
    r.avail = 1;
    if (is_intra_type(m->type)) return r;      /* available, but refIdx -1 */
    r.ref = m->ref_idx[((gy & 3) >> 1) * 2 + ((gx & 3) >> 1)];
    r.mvx = m->mv[(gy & 3) * 4 + (gx & 3)][0];
    r.mvy = m->mv[(gy & 3) * 4 + (gx & 3)][1];
    return r;
}

/* Is MB (mbx, mby) available as an intra-prediction/mode neighbour of the
 * MB at cur_addr?  constrained_intra_pred hides inter MBs. */
static int intra_avail(h264dec *d, int cur_addr, int mbx, int mby)
{
    if (mbx < 0 || mby < 0 || mbx >= d->mbw || mby >= d->mbh) return 0;
    int addr = mby * d->mbw + mbx;
    if (slice_of(d, addr) != slice_of(d, cur_addr)) return 0;
    if (d->cur_pps->constrained_intra_pred && !is_intra_type(d->mb[addr].type))
        return 0;
    return 1;
}

/* nC combine rule (spec 9.2.1): both neighbours -> rounded mean, one -> that
 * one, none -> 0.  -1 marks an unavailable neighbour. */
static int combine_nc(int na, int nb)
{
    if (na >= 0 && nb >= 0) return (na + nb + 1) >> 1;
    if (na >= 0) return na;
    if (nb >= 0) return nb;
    return 0;
}

/* nz of a neighbour MB's block, with the I_PCM "16" substitution */
static int nb_nz(const mbinfo_t *m, int idx)
{
    if (m->type == MB_I_PCM) return 16;
    return m->nz[idx];
}

/* CAVLC context for luma 4x4 block (bx, by) of MB addr (I4x4 full blocks,
 * I16x16 AC blocks and inter blocks all use this: each neighbour contributes
 * whatever its own nz[] stores for the corresponding block). */
static int luma_nC(h264dec *d, int addr, int bx, int by)
{
    int mbx = addr % d->mbw, mby = addr / d->mbw;
    const mbinfo_t *mi = &d->mb[addr];
    int na = -1, nb = -1;
    if (bx > 0) na = nb_nz(mi, i4_inv[by * 4 + bx - 1]);
    else if (mbx > 0 && slice_of(d, addr - 1) == slice_of(d, addr))
        na = nb_nz(&d->mb[addr - 1], i4_inv[by * 4 + 3]);
    if (by > 0) nb = nb_nz(mi, i4_inv[(by - 1) * 4 + bx]);
    else if (mby > 0 && slice_of(d, addr - d->mbw) == slice_of(d, addr))
        nb = nb_nz(&d->mb[addr - d->mbw], i4_inv[12 + bx]);
    return combine_nc(na, nb);
}

/* nC for the I16x16 luma DC block.  NOT the neighbours' DC TotalCoeff --
 * JM (read_comp_cavlc.c predict_nnz, called with i=j=0) and ffmpeg
 * (pred_non_zero_count on block index 0) both read the ordinary 4x4 luma
 * block nz counts adjacent to position (0,0): left MB's (3,0) block and
 * top MB's (0,3) block.  Getting this wrong desyncs coeff_token whenever
 * a neighbour's DC count and its (3,0)/(0,3) block counts differ. */
static int i16dc_nC(h264dec *d, int addr)
{
    return luma_nC(d, addr, 0, 0);
}

/* nC for chroma AC block (bx, by) [2x2 grid] of component comp (0=Cb,1=Cr) */
static int chroma_nC(h264dec *d, int addr, int comp, int bx, int by)
{
    int mbx = addr % d->mbw, mby = addr / d->mbw;
    const mbinfo_t *mi = &d->mb[addr];
    int base = 16 + comp * 4;
    int na = -1, nb = -1;
    if (bx > 0) na = nb_nz(mi, base + by * 2 + bx - 1);
    else if (mbx > 0 && slice_of(d, addr - 1) == slice_of(d, addr))
        na = nb_nz(&d->mb[addr - 1], base + by * 2 + 1);
    if (by > 0) nb = nb_nz(mi, base + (by - 1) * 2 + bx);
    else if (mby > 0 && slice_of(d, addr - d->mbw) == slice_of(d, addr))
        nb = nb_nz(&d->mb[addr - d->mbw], base + 2 + bx);
    return combine_nc(na, nb);
}

/* ======================================================== MV prediction == */
/* dir_kind: 0 = no directional rule, 1 = 16x8 MB partitions, 2 = 8x16.
 * Partition's top-left 4x4 block at (px, py) within the MB, size pw x ph
 * in 4x4 units. */
static void mv_pred(h264dec *d, int cur, int mbx, int mby,
                    int px, int py, int pw, int ph, int ref, int dir_kind,
                    int *outx, int *outy)
{
    int gx = mbx * 4 + px, gy = mby * 4 + py;
    /* Spec 6.4.11.7 locates the three neighbours from the partition's TOP-LEFT
     * corner (x, y), and only C steps to the right by the partition width:
     *   A = (x - 1, y)   B = (x, y - 1)   C = (x + predPartWidth, y - 1)
     *   D = (x - 1, y - 1), substituted for C when C is unavailable.
     * B was reading (x + pw - 1, y - 1) -- the partition's own right edge --
     * which picks a different 4x4 block whenever the row above is partitioned
     * more finely than this partition is. The bit count is unaffected (mvd is
     * read either way), so a wrong mvp never desynchronises the stream; it just
     * silently shifts the reconstructed block and then feeds the error onward
     * through every neighbour that predicts from it. */
    nb_t A = get_nb(d, cur, gx - 1, gy);
    nb_t B = get_nb(d, cur, gx, gy - 1);
    nb_t C = get_nb(d, cur, gx + pw, gy - 1);
    if (!C.avail) C = get_nb(d, cur, gx - 1, gy - 1);   /* D substitution */

    if (dir_kind == 1) {              /* 16x8: upper -> B, lower -> A */
        if (py == 0 && B.avail && B.ref == ref) { *outx = B.mvx; *outy = B.mvy; return; }
        if (py != 0 && A.avail && A.ref == ref) { *outx = A.mvx; *outy = A.mvy; return; }
    } else if (dir_kind == 2) {       /* 8x16: left -> A, right -> C */
        if (px == 0 && A.avail && A.ref == ref) { *outx = A.mvx; *outy = A.mvy; return; }
        if (px != 0 && C.avail && C.ref == ref) { *outx = C.mvx; *outy = C.mvy; return; }
    }

    if (A.avail && !B.avail && !C.avail) { *outx = A.mvx; *outy = A.mvy; return; }
    int ma = A.avail && A.ref == ref;
    int mb = B.avail && B.ref == ref;
    int mc = C.avail && C.ref == ref;
    if (ma + mb + mc == 1) {
        if (ma)      { *outx = A.mvx; *outy = A.mvy; }
        else if (mb) { *outx = B.mvx; *outy = B.mvy; }
        else         { *outx = C.mvx; *outy = C.mvy; }
        return;
    }
    *outx = median3(A.avail ? A.mvx : 0, B.avail ? B.mvx : 0, C.avail ? C.mvx : 0);
    *outy = median3(A.avail ? A.mvy : 0, B.avail ? B.mvy : 0, C.avail ? C.mvy : 0);
}

/* P_Skip motion vector (spec 8.4.1.1).
 *
 * The zero case is an OR over four conditions, not an AND over two: the vector
 * is zero if EITHER neighbour is missing, or if EITHER of them is itself a
 * zero-motion reference-0 block. Requiring both to hold leaves a skipped
 * macroblock predicting from the median when the spec says it must not move at
 * all -- a small displacement, so the picture stays recognisable and only some
 * blocks are off, which is what makes it survive a casual look.
 *
 * When it is not zero, the answer is the ORDINARY 16x16 prediction with
 * refIdx 0 -- including that derivation's own special cases (a single
 * ref-matching neighbour wins outright; A alone available wins outright). A
 * bare median of the three is not the same function. */
static void mv_pred_skip(h264dec *d, int cur, int mbx, int mby, int *outx, int *outy)
{
    /* A and B here are the 16x16 partition's neighbours from 6.4.11.7, so B is
     * at (x, y - 1) -- the macroblock's own left column, not its right one. */
    nb_t A = get_nb(d, cur, mbx * 4 - 1, mby * 4);
    nb_t B = get_nb(d, cur, mbx * 4, mby * 4 - 1);
    if (!A.avail || !B.avail ||
        (A.ref == 0 && A.mvx == 0 && A.mvy == 0) ||
        (B.ref == 0 && B.mvx == 0 && B.mvy == 0)) {
        *outx = 0; *outy = 0;
        return;
    }
    mv_pred(d, cur, mbx, mby, 0, 0, 4, 4, 0, 0, outx, outy);
}

/* ==================================================================== MC == */
/* Motion-compensate one block from a reference plane into the current
 * frame.  Reads inside the replicated H264_PAD border go straight to the
 * plane; anything beyond is rebuilt through a clamped edge-emulation
 * scratch so no access ever leaves the allocation. */
static void mc_plane(h264dec *d, uint8_t *dst, int dstride,
                     const uint8_t *plane, int stride, int pw, int ph,
                     int is_luma, int x, int y, int w, int h, int mvx, int mvy)
{
    /* Split each component into a floored integer offset and a fraction.
     * Negative motion vectors are routine, and both `>>` and `<<` on negative
     * ints are implementation-defined or undefined -- UBSan flags the shift on
     * essentially every P frame. Do the floor with division and a correction,
     * matching h264_mc.c's mv_split, which already got this right. */
    int bits = is_luma ? 2 : 3;
    int unit = 1 << bits;
    int qx = mvx / unit, fx = mvx - qx * unit;
    int qy = mvy / unit, fy = mvy - qy * unit;
    if (fx < 0) { fx += unit; qx--; }
    if (fy < 0) { fy += unit; qy--; }
    int m = is_luma ? 2 : 0;                  /* filter taps before the block */
    int t = is_luma ? 3 : 1;                  /* filter taps after the block */
    if (x + qx >= m - H264_PAD && y + qy >= m - H264_PAD &&
        x + qx + w + t <= pw + H264_PAD && y + qy + h + t <= ph + H264_PAD) {
        h264_mc_block(dst, dstride, plane, stride, x, y, w, h, mvx, mvy, is_luma);
        return;
    }
    if (is_luma) {
        int sw = w + 6, sh = h + 6;           /* [-2 .. w+3] both axes */
        uint8_t *s = d->emu_luma;
        for (int r = 0; r < sh; r++) {
            const uint8_t *row = plane + (long)clip3(0, ph - 1, y + qy - 2 + r) * stride;
            for (int c = 0; c < sw; c++)
                s[r * sw + c] = row[clip3(0, pw - 1, x + qx - 2 + c)];
        }
        h264_mc_block(dst, dstride, s, sw, 2, 2, w, h, fx, fy, 1);
    } else {
        int sw = w + 1, sh = h + 1;           /* bilinear: [0 .. w] x [0 .. h] */
        uint8_t *s = d->emu_chroma;
        for (int r = 0; r < sh; r++) {
            const uint8_t *row = plane + (long)clip3(0, ph - 1, y + qy + r) * stride;
            for (int c = 0; c < sw; c++)
                s[r * sw + c] = row[clip3(0, pw - 1, x + qx + c)];
        }
        h264_mc_block(dst, dstride, s, sw, 0, 0, w, h, fx, fy, 0);
    }
}

/* MC one inter partition (px, py, pw4, ph4 in 4x4 units) + optional
 * explicit weighted prediction. */
static void inter_pred(h264dec *d, int mbx, int mby,
                       int px, int py, int pw4, int ph4,
                       const pic_t *ref, int ref_idx, const slice_t *sl)
{
    const mbinfo_t *mi = &d->mb[mby * d->mbw + mbx];
    int mvx = mi->mv[py * 4 + px][0];
    int mvy = mi->mv[py * 4 + px][1];
    int x = mbx * 16 + px * 4, y = mby * 16 + py * 4;
    int w = pw4 * 4, h = ph4 * 4;
    uint8_t *dy = d->cur->y + (long)y * d->stride_y + x;
    uint8_t *du = d->cur->u + (long)(y / 2) * d->stride_c + x / 2;
    uint8_t *dv = d->cur->v + (long)(y / 2) * d->stride_c + x / 2;
    mc_plane(d, dy, d->stride_y, ref->y, ref->stride_y,
             d->mbw * 16, d->mbh * 16, 1, x, y, w, h, mvx, mvy);
    mc_plane(d, du, d->stride_c, ref->u, ref->stride_c,
             d->mbw * 8, d->mbh * 8, 0, x / 2, y / 2, w / 2, h / 2, mvx, mvy);
    mc_plane(d, dv, d->stride_c, ref->v, ref->stride_c,
             d->mbw * 8, d->mbh * 8, 0, x / 2, y / 2, w / 2, h / 2, mvx, mvy);
    if (d->cur_pps->weighted_pred) {
        h264_mc_weight(dy, d->stride_y, w, h, sl->luma_log2_weight_denom,
                       sl->wp_luma_w[ref_idx], sl->wp_luma_o[ref_idx]);
        h264_mc_weight(du, d->stride_c, w / 2, h / 2, sl->chroma_log2_weight_denom,
                       sl->wp_chroma_w[ref_idx][0], sl->wp_chroma_o[ref_idx][0]);
        h264_mc_weight(dv, d->stride_c, w / 2, h / 2, sl->chroma_log2_weight_denom,
                       sl->wp_chroma_w[ref_idx][1], sl->wp_chroma_o[ref_idx][1]);
    }
}

/* ============================================== residual reconstruction == */
/* IDCT + add for blocks whose DC arrives FINAL (I16x16 luma via the scaled
 * Hadamard, chroma via the scaled 2x2 Hadamard): dc_val is placed verbatim
 * at raster position 0, ac[0..14] are the scan positions 1..15 (NULL = no
 * AC coded).  Same integer arithmetic as h264_dequant_idct_add minus the DC
 * dequant, so results are bit-identical to the fused spec pipeline. */
static void idct_add_dc_ac(int dc_val, const int *ac, int qp, uint8_t *dst, int stride)
{
    int d[16], f[16], g[16];
    int m = qp % 6, qbits = qp / 6;
    for (int i = 0; i < 16; i++) d[i] = 0;
    d[0] = dc_val;
    if (ac) {
        for (int i = 0; i < 15; i++) {
            int c = ac[i];
            if (!c) continue;
            int r = zz4[i + 1];
            if (c > 16383) c = 16383; else if (c < -16384) c = -16384;
            d[r] = c * vq[m][vclass(r >> 2, r & 3)] * (1 << qbits);
        }
    }
    for (int j = 0; j < 4; j++) {                 /* columns */
        int d0 = d[j], d1 = d[4 + j], d2 = d[8 + j], d3 = d[12 + j];
        int e0 = d0 + d2, e1 = d0 - d2, e2 = (d1 >> 1) - d3, e3 = d1 + (d3 >> 1);
        f[j] = e0 + e3; f[4 + j] = e1 + e2; f[8 + j] = e1 - e2; f[12 + j] = e0 - e3;
    }
    for (int i = 0; i < 4; i++) {                 /* rows */
        int f0 = f[i * 4], f1 = f[i * 4 + 1], f2 = f[i * 4 + 2], f3 = f[i * 4 + 3];
        int e0 = f0 + f2, e1 = f0 - f2, e2 = (f1 >> 1) - f3, e3 = f1 + (f3 >> 1);
        g[i * 4] = e0 + e3; g[i * 4 + 1] = e1 + e2;
        g[i * 4 + 2] = e1 - e2; g[i * 4 + 3] = e0 - e3;
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            int v = dst[i * stride + j] + ((g[i * 4 + j] + 32) >> 6);
            dst[i * stride + j] = (uint8_t)clip3(0, 255, v);
        }
}

/* Chroma residual for one MB (both components).  cbp_chroma: 0 = none,
 * 1 = DC only, 2 = DC + AC.  Prediction (intra or MC) is already in dst. */
static int residual_chroma(h264dec *d, bs_t *bs, int addr, int cbp_chroma, int qpy)
{
    if (cbp_chroma <= 0) return H264_OK;
    if (cbp_chroma > 2) return H264_ERR_CORRUPT;
    int mbx = addr % d->mbw, mby = addr / d->mbw;
    mbinfo_t *mi = &d->mb[addr];
    int off_cb = d->cur_pps->chroma_qp_index_offset;
    int off_cr = d->cur_pps->has_second_chroma_offset
               ? d->cur_pps->second_chroma_qp_offset : off_cb;

    /* Bitstream order (spec 7.3.5.3 residual): ALL chroma DC chains first
     * (Cb then Cr), THEN all AC blocks (Cb x4, Cr x4). */
    int dc[2][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
    int qpc[2];
    for (int comp = 0; comp < 2; comp++) {
        qpc[comp] = chroma_qp(qpy, comp ? off_cr : off_cb);
        int t[16];
        int tc = cavlc_decode(bs, -1, 4, t);
        if (tc < 0 || bs_error(bs)) { TRACE("chromaDC comp=%d tc=%d err=%d\n", comp, tc, bs_error(bs)); return H264_ERR_CORRUPT; }
        for (int i = 0; i < 4; i++) dc[comp][i] = t[i];
        h264_dcdcm_transform(dc[comp]);
        int m = qpc[comp] % 6, k = qpc[comp] / 6;
        for (int i = 0; i < 4; i++) {
            int p = dc[comp][i] * vq[m][0];
            /* Spec 8.5.11.2: dcC = ((f * LevelScale4x4(qP%6,0,0)) << (qP/6))
             * >> 5, and with a flat scaling list LevelScale4x4 is 16*v -- so
             * the whole expression is p * 2^(qP/6) / 2. That is exact once
             * qP >= 6; below it the >> 5 is a FLOOR. Rounding half up there
             * instead is off by one whenever p is odd, which needs both a
             * chroma qP under 6 and an odd v (11 or 13). Nothing in the
             * generated matrix quantises that finely; tests/fixtures/video
             * does, in one macroblock of one frame, and the error then rode
             * the reference chain through the rest of the stream. */
            dc[comp][i] = k > 0 ? p * (1 << (k - 1)) : half_floor(p);
        }
    }
    for (int comp = 0; comp < 2; comp++) {
        uint8_t *plane = comp ? d->cur->v : d->cur->u;
        for (int b = 0; b < 4; b++) {
            int bx = b & 1, by = b >> 1;
            int ac[15], *acp = 0;
            if (cbp_chroma >= 2) {
                int t[16];
                int tc = cavlc_decode(bs, chroma_nC(d, addr, comp, bx, by), 15, t);
                if (tc < 0 || bs_error(bs)) { TRACE("chromaAC comp=%d blk=%d tc=%d err=%d\n", comp, b, tc, bs_error(bs)); return H264_ERR_CORRUPT; }
                for (int i = 0; i < 15; i++) ac[i] = t[i];
                acp = ac;
                mi->nz[16 + comp * 4 + b] = (uint8_t)tc;
            }
            uint8_t *dst = plane + (long)(mby * 8 + by * 4) * d->stride_c
                                   + mbx * 8 + bx * 4;
            if (dc[comp][b] || acp)
                idct_add_dc_ac(dc[comp][b], acp, qpc[comp], dst, d->stride_c);
        }
    }
    return H264_OK;
}

/* ============================================================ intra MBs == */
/* Fetch the intra-prediction neighbour pixels of a 4x4 luma block straight
 * from the frame under construction. */
static void i4_neighbours(h264dec *d, int mbx, int mby, int bx, int by,
                          int al, int at, int atl, int atr,
                          uint8_t *topbuf, uint8_t *leftbuf, int *tl_out)
{
    int px = mbx * 16 + bx * 4, py = mby * 16 + by * 4;
    const uint8_t *y = d->cur->y;
    int s = d->stride_y;
    if (at) {
        const uint8_t *t = y + (long)(py - 1) * s + px;
        memcpy(topbuf, t, 4);
        topbuf[4] = topbuf[5] = topbuf[6] = topbuf[7] = t[3];
        if (atr) memcpy(topbuf + 4, t + 4, 4);   /* top-right bottom row */
    }
    if (al)
        for (int i = 0; i < 4; i++) leftbuf[i] = y[(long)(py + i) * s + px - 1];
    *tl_out = atl ? y[(long)(py - 1) * s + px - 1] : 0;
}

/* Per-block availability flags (spec 6.4.11.4 / 8.3.1.1). */
static void i4_avail(h264dec *d, int cur, int mbx, int mby, int bx, int by,
                     int *al, int *at, int *atl, int *atr)
{
    *al = bx > 0 ? 1 : intra_avail(d, cur, mbx - 1, mby);
    *at = by > 0 ? 1 : intra_avail(d, cur, mbx, mby - 1);
    *atl = (bx > 0 && by > 0) ? 1
         : intra_avail(d, cur, mbx - (bx == 0), mby - (by == 0));
    if (by > 0) {
        /* Inside the macroblock the above-right 4x4 block is available only if
         * it has already been DECODED, and the 4x4 blocks are coded in Z order,
         * not raster order:
         *      0  1  4  5
         *      2  3  6  7
         *      8  9 12 13
         *     10 11 14 15
         * So for a block at (bx, by>0) the above-right is (bx+1, by-1):
         *   bx=0 -> (1,by-1): blk 1<2, 3<8, 9<10    -- earlier, available
         *   bx=1 -> (2,by-1): blk 4>3 and 12>11, but 6<9
         *   bx=2 -> (3,by-1): blk 5<6, 7<12, 13<14  -- earlier, available
         *   bx=3 -> belongs to the macroblock on the right, undecoded
         *
         * So bx==1 is NOT uniformly unavailable: it depends on by. Odd by (the
         * lower half of each 8x8 pair) reaches into the next 8x8 quadrant, which
         * Z order visits later; even by reaches within the already-finished
         * quadrant. The blocks that lose their above-right are exactly 3 and 11.
         *
         * Treating an unavailable above-right as available reads samples that do
         * not exist yet; treating an available one as unavailable substitutes
         * replicated ones. Both are wrong only for modes that actually look up
         * and to the right (Diagonal_Down_Left, Vertical_Left), which is why the
         * damage is confined to scattered macroblocks rather than the picture. */
        *atr = (bx != 3) && !(bx == 1 && (by & 1));
    } else {
        *atr = bx < 3 ? intra_avail(d, cur, mbx, mby - 1)
                      : intra_avail(d, cur, mbx + 1, mby - 1);
    }
}

static int decode_i4(h264dec *d, bs_t *bs, slice_t *sl, int addr, int *qpyp)
{
    int mbx = addr % d->mbw, mby = addr / d->mbw;
    mbinfo_t *mi = &d->mb[addr];
    mi->type = MB_I4x4;

    /* --- mb_pred: 16 luma modes + chroma mode --- */
    for (int blk = 0; blk < 16; blk++) {
        int bx = i4x[blk], by = i4y[blk];
        /* spec 8.3.1.1 / ffmpeg pred_intra_mode: an unavailable neighbour is
         * -1, a neighbour not coded I4x4 (I16/I_PCM/inter) counts as DC(2),
         * and if EITHER side is -1 the predicted mode is DC(2) -- NOT
         * min() with the other side substituted. */
        int modeA = -1, modeB = -1;
        if (bx > 0) modeA = mi->i4mode[i4_inv[by * 4 + bx - 1]];
        else if (intra_avail(d, addr, mbx - 1, mby)) {
            const mbinfo_t *l = &d->mb[addr - 1];
            modeA = l->type == MB_I4x4 ? l->i4mode[i4_inv[by * 4 + 3]] : 2;
        }
        if (by > 0) modeB = mi->i4mode[i4_inv[(by - 1) * 4 + bx]];
        else if (intra_avail(d, addr, mbx, mby - 1)) {
            const mbinfo_t *t = &d->mb[addr - d->mbw];
            modeB = t->type == MB_I4x4 ? t->i4mode[i4_inv[12 + bx]] : 2;
        }
        int mn = modeA < modeB ? modeA : modeB;
        int pred = mn < 0 ? 2 : mn;
        int mode;
        if (bs_u1(bs)) mode = pred;
        else {
            int rem = (int)bs_u(bs, 3);
            mode = rem < pred ? rem : rem + 1;
        }
        if (bs_error(bs)) return H264_ERR_CORRUPT;
        mi->i4mode[blk] = (uint8_t)mode;
        TRACE("i4 addr=%d mode blk=%d (%d,%d) A=%d B=%d pred=%d mode=%d bitpos=%d\n", addr, blk, bx, by, modeA, modeB, pred, mode, bs->bitpos);
    }
    uint32_t chroma_mode = bs_ue(bs);
    if (bs_error(bs) || chroma_mode > 3) { TRACE("i4 chroma_mode=%u err=%d\n", chroma_mode, bs_error(bs)); return H264_ERR_CORRUPT; }
    TRACE("i4 addr=%d modes_done bitpos=%d chroma_mode=%u\n", addr, bs->bitpos, chroma_mode);

    /* --- coded_block_pattern + qp --- */
    uint32_t codeNum = bs_ue(bs);
    if (bs_error(bs) || codeNum > 47) { TRACE("i4 codeNum=%u err=%d\n", codeNum, bs_error(bs)); return H264_ERR_CORRUPT; }
    int cbp = cbp_intra_tab[codeNum];
    TRACE("i4 addr=%d codeNum=%u cbp=%d\n", addr, codeNum, cbp);
    mi->cbp = (uint8_t)cbp;
    int cbp_luma = cbp & 15, cbp_chroma = (cbp >> 4) & 3;
    int qpy = *qpyp;
    if (cbp_luma || cbp_chroma) {
        int delta = (int)bs_se(bs);
        if (bs_error(bs)) return H264_ERR_CORRUPT;
        qpy = ((qpy + delta) % 52 + 52) % 52;
        *qpyp = qpy;
    }
    mi->qp = (int8_t)qpy;

    /* --- luma: predict + residual per block, luma4x4BlkIdx order --- */
    uint8_t topbuf[8], leftbuf[4];
    for (int blk = 0; blk < 16; blk++) {
        int bx = i4x[blk], by = i4y[blk];
        int al, at, atl, atr, tl;
        i4_avail(d, addr, mbx, mby, bx, by, &al, &at, &atl, &atr);
        i4_neighbours(d, mbx, mby, bx, by, al, at, atl, atr, topbuf, leftbuf, &tl);
        uint8_t *dst = d->cur->y + (long)(mby * 16 + by * 4) * d->stride_y
                                   + mbx * 16 + bx * 4;
        h264_intra4x4(dst, d->stride_y, mi->i4mode[blk],
                      topbuf, leftbuf, tl, al, at, atr, atl);
#ifdef H264_TRACE
#ifndef H264_PREDDBG_MB
#define H264_PREDDBG_MB (-1)
#define H264_PREDDBG_BLK (-1)
#endif
        if (addr == H264_PREDDBG_MB && blk == H264_PREDDBG_BLK) {
            fprintf(stderr, "PREDDBG mode=%d al=%d at=%d atr=%d atl=%d tl=%d top=%d,%d,%d,%d tr=%d,%d,%d,%d left=%d,%d,%d,%d\n",
                    mi->i4mode[blk], al, at, atr, atl, tl,
                    topbuf[0], topbuf[1], topbuf[2], topbuf[3],
                    topbuf[4], topbuf[5], topbuf[6], topbuf[7],
                    leftbuf[0], leftbuf[1], leftbuf[2], leftbuf[3]);
            fprintf(stderr, "PREDDBG pred=%d,%d,%d,%d / %d,%d,%d,%d\n",
                    dst[0], dst[1], dst[2], dst[3],
                    dst[4*d->stride_y], dst[4*d->stride_y+1], dst[4*d->stride_y+2], dst[4*d->stride_y+3]);
        }
#endif
        if (cbp_luma & (1 << (blk >> 2))) {
            int coef[16];
            int bp0 = bs->bitpos;
            int tc = cavlc_decode(bs, luma_nC(d, addr, bx, by), 16, coef);
            if (tc < 0 || bs_error(bs)) { TRACE("i4 cavlc luma blk=%d tc=%d err=%d\n", blk, tc, bs_error(bs)); return H264_ERR_CORRUPT; }
            TRACE("i4 addr=%d blk=%d nC=%d tc=%d bits=%d..%d\n", addr, blk, luma_nC(d, addr, bx, by), tc, bp0, bs->bitpos);
            h264_dequant_idct_add(coef, qpy, dst, d->stride_y);
            mi->nz[blk] = (uint8_t)tc;
        }
    }

    /* --- chroma: predict both components, then residual --- */
    int al = intra_avail(d, addr, mbx - 1, mby);
    int at = intra_avail(d, addr, mbx, mby - 1);
    int atl = intra_avail(d, addr, mbx - 1, mby - 1);
    for (int comp = 0; comp < 2; comp++) {
        uint8_t *plane = comp ? d->cur->v : d->cur->u;
        int s = d->stride_c;
        int px = mbx * 8, py = mby * 8;
        uint8_t topc[8], leftc[8];
        int tl = atl ? plane[(long)(py - 1) * s + px - 1] : 0;
        if (at) memcpy(topc, plane + (long)(py - 1) * s + px, 8);
        if (al) for (int i = 0; i < 8; i++)
            leftc[i] = plane[(long)(py + i) * s + px - 1];
        h264_intra_chroma(plane + (long)py * s + px, s, (int)chroma_mode,
                          topc, leftc, tl, al, at);
    }
    return residual_chroma(d, bs, addr, cbp_chroma, qpy);
}

static int decode_i16(h264dec *d, bs_t *bs, slice_t *sl, int addr, int *qpyp,
                      int t16)
{
    int mbx = addr % d->mbw, mby = addr / d->mbw;
    mbinfo_t *mi = &d->mb[addr];
    mi->type = MB_I16x16;
    int mode = t16 & 3;
    int cbpcode = t16 >> 2;             /* 0..5: chroma = cbpcode%3, luma15 = cbpcode>=3 */
    int cbp_luma = cbpcode >= 3 ? 15 : 0;
    int cbp_chroma = cbpcode % 3;
    mi->intra16_mode = (uint8_t)mode;
    mi->cbp = (uint8_t)((cbp_chroma << 4) | cbp_luma);

    uint32_t chroma_mode = bs_ue(bs);
    if (bs_error(bs) || chroma_mode > 3) { TRACE("i16 chroma_mode=%u err=%d\n", chroma_mode, bs_error(bs)); return H264_ERR_CORRUPT; }
    int qpy = *qpyp;
    int delta = (int)bs_se(bs);                 /* I16 always codes mb_qp_delta */
    if (bs_error(bs)) { TRACE("i16 qp_delta err\n"); return H264_ERR_CORRUPT; }
    qpy = ((qpy + delta) % 52 + 52) % 52;
    *qpyp = qpy;
    mi->qp = (int8_t)qpy;
    TRACE("i16 addr=%d chroma=%d qp_delta=%d qpy=%d cbp_l=%d cbp_c=%d bitpos=%d\n", addr, chroma_mode, delta, qpy, cbp_luma, cbp_chroma, bs->bitpos);

    /* --- 16x16 luma prediction --- */
    int al = intra_avail(d, addr, mbx - 1, mby);
    int at = intra_avail(d, addr, mbx, mby - 1);
    int atl = intra_avail(d, addr, mbx - 1, mby - 1);
    int s = d->stride_y;
    int px = mbx * 16, py = mby * 16;
    uint8_t top16[16], left16[16];
    int tl = atl ? d->cur->y[(long)(py - 1) * s + px - 1] : 0;
    if (at) memcpy(top16, d->cur->y + (long)(py - 1) * s + px, 16);
    if (al) for (int i = 0; i < 16; i++)
        left16[i] = d->cur->y[(long)(py + i) * s + px - 1];
    h264_intra16x16(d->cur->y + (long)py * s + px, s, mode,
                    top16, left16, tl, al, at);

    /* --- luma DC chain (spec 8.5.11): scale, Hadamard, verbatim DC --- */
    int dc[16];
    {
        int coef[16];
        int tc = cavlc_decode(bs, i16dc_nC(d, addr), 16, coef);
        if (tc < 0 || bs_error(bs)) { TRACE("i16 lumaDC tc=%d err=%d bitpos=%d\n", tc, bs_error(bs), bs->bitpos); return H264_ERR_CORRUPT; }
        mi->nz_i16dc = (uint8_t)tc;
        int m = qpy % 6;
        for (int i = 0; i < 16; i++) {
            /* coef[i] is the coefficient at zigzag scan position i; the DC
             * matrix is in raster order, so unscramble (spec 8.5.11). */
            int c = coef[i] * vq[m][0];
            dc[zz4[i]] = qpy >= 12 ? c * (1 << (qpy / 6 - 2))
                                   : (c + (1 << (1 - qpy / 6))) >> (2 - qpy / 6);
        }
        h264_dc16_transform(dc);
    }
    /* --- AC blocks --- */
    for (int blk = 0; blk < 16; blk++) {
        int bx = i4x[blk], by = i4y[blk];
        int ac[15], *acp = 0;
        if (cbp_luma) {
            int t[16];
            int tc = cavlc_decode(bs, luma_nC(d, addr, bx, by), 15, t);
            if (tc < 0 || bs_error(bs)) { TRACE("i16 lumaAC blk=%d tc=%d err=%d bitpos=%d\n", blk, tc, bs_error(bs), bs->bitpos); return H264_ERR_CORRUPT; }
            for (int i = 0; i < 15; i++) ac[i] = t[i];
            acp = ac;
            mi->nz[blk] = (uint8_t)tc;
        }
        if (dc[by * 4 + bx] || acp) {
            uint8_t *dst = d->cur->y + (long)(py + by * 4) * s + px + bx * 4;
            idct_add_dc_ac(dc[by * 4 + bx], acp, qpy, dst, s);
        }
    }

    /* --- chroma prediction + residual --- */
    for (int comp = 0; comp < 2; comp++) {
        uint8_t *plane = comp ? d->cur->v : d->cur->u;
        int sc = d->stride_c;
        int cx = mbx * 8, cy = mby * 8;
        uint8_t topc[8], leftc[8];
        int tlc = atl ? plane[(long)(cy - 1) * sc + cx - 1] : 0;
        if (at) memcpy(topc, plane + (long)(cy - 1) * sc + cx, 8);
        if (al) for (int i = 0; i < 8; i++)
            leftc[i] = plane[(long)(cy + i) * sc + cx - 1];
        h264_intra_chroma(plane + (long)cy * sc + cx, sc, (int)chroma_mode,
                          topc, leftc, tlc, al, at);
    }
    return residual_chroma(d, bs, addr, cbp_chroma, qpy);
}

static int decode_ipcm(h264dec *d, bs_t *bs, int addr, int *qpyp)
{
    int mbx = addr % d->mbw, mby = addr / d->mbw;
    mbinfo_t *mi = &d->mb[addr];
    mi->type = MB_I_PCM;
    mi->qp = 0;                                 /* spec: PCM blocks use qP 0 */
    bs_align(bs);
    if (bs_error(bs) || bs_bits_left(bs) < (256 + 64 + 64) * 8)
        return H264_ERR_CORRUPT;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            d->cur->y[(long)(mby * 16 + i) * d->stride_y + mbx * 16 + j] =
                (uint8_t)bs_u(bs, 8);
    for (int comp = 0; comp < 2; comp++) {
        uint8_t *plane = comp ? d->cur->v : d->cur->u;
        for (int i = 0; i < 8; i++)
            for (int j = 0; j < 8; j++)
                plane[(long)(mby * 8 + i) * d->stride_c + mbx * 8 + j] =
                    (uint8_t)bs_u(bs, 8);
    }
    for (int i = 0; i < 24; i++) mi->nz[i] = 16;
    mi->nz_i16dc = 16;
    (void)qpyp;                                 /* PCM does not change qPy */
    return H264_OK;
}

/* ============================================================= inter MBs == */
typedef struct { int px, py, pw, ph, kind, region; } part_t;

static int inter_parts(bs_t *bs, int mb_type, part_t *parts)
{
    if (mb_type == 0) { parts[0] = (part_t){ 0, 0, 4, 4, 0, 0 }; return 1; }
    if (mb_type == 1) {
        parts[0] = (part_t){ 0, 0, 4, 2, 1, 0 };
        parts[1] = (part_t){ 0, 2, 4, 2, 1, 1 };
        return 2;
    }
    if (mb_type == 2) {
        parts[0] = (part_t){ 0, 0, 2, 4, 2, 0 };
        parts[1] = (part_t){ 2, 0, 2, 4, 2, 1 };
        return 2;
    }
    /* P_8x8 / P_8x8ref0: four 8x8 regions, each with its own sub_mb_type */
    int n = 0;
    for (int r = 0; r < 4; r++) {
        uint32_t st = bs_ue(bs);
        if (bs_error(bs) || st > 3) return -1;
        int px = (r & 1) * 2, py = (r >> 1) * 2;
        if (st == 0) {
            parts[n++] = (part_t){ px, py, 2, 2, 0, r };
        } else if (st == 1) {                     /* P_L0_8x4 */
            parts[n++] = (part_t){ px, py, 2, 1, 0, r };
            parts[n++] = (part_t){ px, py + 1, 2, 1, 0, r };
        } else if (st == 2) {                     /* P_L0_4x8 */
            parts[n++] = (part_t){ px, py, 1, 2, 0, r };
            parts[n++] = (part_t){ px + 1, py, 1, 2, 0, r };
        } else {                                  /* P_L0_4x4 */
            parts[n++] = (part_t){ px, py, 1, 1, 0, r };
            parts[n++] = (part_t){ px + 1, py, 1, 1, 0, r };
            parts[n++] = (part_t){ px, py + 1, 1, 1, 0, r };
            parts[n++] = (part_t){ px + 1, py + 1, 1, 1, 0, r };
        }
    }
    return n;
}

/* The 8x8 quadrant a partition lives in -- the index mi->ref_idx[] is stored
 * and read back under. Every partition of every P macroblock type starts on an
 * 8x8 boundary or inside one, so its top-left 4x4 block picks the quadrant. */
static int part_quadrant(const part_t *pt)
{
    return (pt->py >> 1) * 2 + (pt->px >> 1);
}

static int decode_inter_mb(h264dec *d, bs_t *bs, slice_t *sl, int addr,
                           int *qpyp, int mb_type, pic_t **l0, int n_l0)
{
    int mbx = addr % d->mbw, mby = addr / d->mbw;
    mbinfo_t *mi = &d->mb[addr];
    mi->type = MB_P_L0;

    part_t parts[16];
    int npart = inter_parts(bs, mb_type, parts);
    if (npart < 0) return H264_ERR_CORRUPT;
    int nregion = mb_type == 0 ? 1 : (mb_type <= 2 ? 2 : 4);

    /* --- ref_idx: one per macroblock PARTITION on the wire, but stored per
     * 8x8 QUADRANT.
     *
     * The bitstream carries nregion of them (1 for 16x16, 2 for 16x8/8x16, 4
     * for 8x8), indexed by partition. Everyone who reads mi->ref_idx[] back --
     * get_nb for mv prediction, the deblocking boundary strength -- indexes it
     * by 8x8 quadrant, because that is the granularity a neighbouring 4x4
     * block maps to. Those two indexings only agree for 16x16 (quadrant 0) and
     * P_8x8; a 16x8 macroblock would leave quadrants 2 and 3 reading 0 and put
     * its lower partition's ref in quadrant 1. It stays invisible while there
     * is only one reference picture, which is every frame up to the second P
     * frame -- and then it quietly corrupts mv prediction from there on. */
    int part_ref[4] = { 0, 0, 0, 0 };
    if (mb_type != 4 && sl->num_ref_idx_l0_active > 1) {
        for (int r = 0; r < nregion; r++) {
            part_ref[r] = (int)bs_te(bs, (uint32_t)sl->num_ref_idx_l0_active - 1);
            if (bs_error(bs)) return H264_ERR_CORRUPT;
        }
    }
    if (n_l0 == 0) return H264_ERR_CORRUPT;
    for (int q = 0; q < 4; q++) {                /* q: 8x8 quadrant */
        int p;
        if (mb_type == 0)      p = 0;            /* 16x16: one partition */
        else if (mb_type == 1) p = q >> 1;       /* 16x8:  upper / lower */
        else if (mb_type == 2) p = q & 1;        /* 8x16:  left / right */
        else                   p = q;            /* 8x8:   quadrant == region */
        int ri = part_ref[p];
        if (ri >= n_l0) ri = n_l0 - 1;           /* corrupt: clamp */
        mi->ref_idx[q] = (uint8_t)ri;
        mi->ref_pic[q] = (uint8_t)(l0[ri] - d->pics);
    }

    /* --- mvd per (sub-)partition, mvp per spec --- */
    d->mb_mv_done = 0;
    for (int p = 0; p < npart; p++) {
        part_t *pt = &parts[p];
        int ref = mi->ref_idx[part_quadrant(pt)];
        int mvx, mvy;
        mv_pred(d, addr, mbx, mby, pt->px, pt->py, pt->pw, pt->ph,
                ref, pt->kind, &mvx, &mvy);
        mvx += (int)bs_se(bs);
        mvy += (int)bs_se(bs);
        if (bs_error(bs)) return H264_ERR_CORRUPT;
        mvx = clip3(-8192, 8191, mvx);
        mvy = clip3(-8192, 8191, mvy);
        for (int by = pt->py; by < pt->py + pt->ph; by++)
            for (int bx = pt->px; bx < pt->px + pt->pw; bx++) {
                mi->mv[by * 4 + bx][0] = (int16_t)mvx;
                mi->mv[by * 4 + bx][1] = (int16_t)mvy;
                d->mb_mv_done |= (uint16_t)(1u << (by * 4 + bx));
            }
    }

    /* --- coded_block_pattern + qp --- */
    uint32_t codeNum = bs_ue(bs);
    if (bs_error(bs) || codeNum > 47) return H264_ERR_CORRUPT;
    int cbp = cbp_inter_tab[codeNum];
    mi->cbp = (uint8_t)cbp;
    int cbp_luma = cbp & 15, cbp_chroma = (cbp >> 4) & 3;
    int qpy = *qpyp;
    if (cbp_luma || cbp_chroma) {
        int delta = (int)bs_se(bs);
        if (bs_error(bs)) return H264_ERR_CORRUPT;
        qpy = ((qpy + delta) % 52 + 52) % 52;
        *qpyp = qpy;
    }
    mi->qp = (int8_t)qpy;

    /* --- MC all partitions (prediction), then residual on top --- */
    for (int p = 0; p < npart; p++) {
        part_t *pt = &parts[p];
        int q = part_quadrant(pt);
        inter_pred(d, mbx, mby, pt->px, pt->py, pt->pw, pt->ph,
                   l0[mi->ref_idx[q]], mi->ref_idx[q], sl);
    }
    for (int blk = 0; blk < 16; blk++) {
        if (!(cbp_luma & (1 << (blk >> 2)))) continue;
        int bx = i4x[blk], by = i4y[blk];
        int coef[16];
        int tc = cavlc_decode(bs, luma_nC(d, addr, bx, by), 16, coef);
        if (tc < 0 || bs_error(bs)) return H264_ERR_CORRUPT;
        uint8_t *dst = d->cur->y + (long)(mby * 16 + by * 4) * d->stride_y
                                   + mbx * 16 + bx * 4;
        h264_dequant_idct_add(coef, qpy, dst, d->stride_y);
        mi->nz[blk] = (uint8_t)tc;
    }
    return residual_chroma(d, bs, addr, cbp_chroma, qpy);
}

static void decode_skip(h264dec *d, slice_t *sl, int addr, int qpy, pic_t **l0)
{
    int mbx = addr % d->mbw, mby = addr / d->mbw;
    mbinfo_t *mi = &d->mb[addr];
    memset(mi, 0, sizeof *mi);
    mi->type = MB_P_SKIP;
    mi->qp = (int8_t)qpy;
    for (int q = 0; q < 4; q++)                  /* P_Skip: refIdxL0 = 0 */
        mi->ref_pic[q] = (uint8_t)(l0[0] - d->pics);
    int mvx, mvy;
    d->mb_mv_done = 0;
    mv_pred_skip(d, addr, mbx, mby, &mvx, &mvy);
    for (int i = 0; i < 16; i++) {
        mi->mv[i][0] = (int16_t)mvx;
        mi->mv[i][1] = (int16_t)mvy;
    }
    inter_pred(d, mbx, mby, 0, 0, 4, 4, l0[0], 0, sl);
}

static int decode_mb(h264dec *d, bs_t *bs, slice_t *sl, int addr, int *qpyp,
                     pic_t **l0, int n_l0)
{
    mbinfo_t *mi = &d->mb[addr];
    memset(mi, 0, sizeof *mi);
    for (int q = 0; q < 4; q++) {                /* "no reference" until set */
        mi->ref_idx[q] = 0xFF;
        mi->ref_pic[q] = 0xFF;
    }

    uint32_t mb_type = bs_ue(bs);
    if (bs_error(bs)) return H264_ERR_CORRUPT;
    TRACE("MB %d type=%u\n", addr, mb_type);
    int is_p = sl->slice_type == 0;
    if (is_p && mb_type <= 4) {
        if (mb_type >= 3 && n_l0 == 0) return H264_ERR_CORRUPT;
        return decode_inter_mb(d, bs, sl, addr, qpyp, (int)mb_type, l0, n_l0);
    }
    int t = (int)mb_type - (is_p ? 5 : 0);
    if (t < 0 || t > 25) return H264_ERR_CORRUPT;
    if (t == 0) return decode_i4(d, bs, sl, addr, qpyp);
    if (t == 25) return decode_ipcm(d, bs, addr, qpyp);
    return decode_i16(d, bs, sl, addr, qpyp, t - 1);
}

/* ============================================================ ref list === */
/* Default L0: short-term refs by PicNum descending, then long-term by
 * lt_idx ascending; then the slice header's reordering commands. */
static int build_l0(h264dec *d, slice_t *sl, pic_t **l0)
{
    int maxfn = 1 << d->cur_sps->log2_max_frame_num;
    int cur_fn = sl->frame_num;
    pic_t *st[16], *lt[16];
    int nst = 0, nlt = 0;
    for (int i = 0; i < MAX_DPB; i++) {
        pic_t *p = &d->pics[i];
        if (!p->used || p == d->cur) continue;
        if (p->reference == 1 && nst < 16) st[nst++] = p;
        else if (p->reference == 2 && nlt < 16) lt[nlt++] = p;
    }
    /* insertion sort: short-term by PicNum desc */
    for (int i = 1; i < nst; i++) {
        pic_t *p = st[i];
        int pn = p->frame_num > cur_fn ? p->frame_num - maxfn : p->frame_num;
        int j = i - 1;
        while (j >= 0) {
            int qn = st[j]->frame_num > cur_fn ? st[j]->frame_num - maxfn
                                               : st[j]->frame_num;
            if (qn >= pn) break;
            st[j + 1] = st[j]; j--;
        }
        st[j + 1] = p;
    }
    for (int i = 1; i < nlt; i++) {              /* long-term by lt_idx asc */
        pic_t *p = lt[i];
        int j = i - 1;
        while (j >= 0 && lt[j]->lt_idx > p->lt_idx) { lt[j + 1] = lt[j]; j--; }
        lt[j + 1] = p;
    }
    /* The list is num_ref_idx_l0_active entries long, which is NOT the same as
     * the number of distinct reference pictures. When there are fewer pictures
     * than that, the tail is undefined until the modification commands fill it
     * -- and a picture is allowed to appear at several indices at once. x264's
     * weighted P does exactly that: it signals more active refs than it holds
     * and reorders the same picture into the spare slots so each slot can
     * carry its own luma/chroma weight. */
    int nactive = sl->num_ref_idx_l0_active;
    if (nactive < 1) nactive = 1;
    if (nactive > 16) nactive = 16;

    int n = 0;
    for (int i = 0; i < nst && n < nactive; i++) l0[n++] = st[i];
    for (int i = 0; i < nlt && n < nactive; i++) l0[n++] = lt[i];
    for (int i = n; i <= nactive; i++) l0[i] = 0;   /* [nactive] is scratch */

    if (sl->n_reorder) {
        /* Spec 8.2.4.3.1, followed literally. The subtle part is the
         * compaction: it drops other copies of the picture just inserted only
         * from the entries AFTER refIdx, never from the ones before it. That
         * asymmetry is what lets one picture occupy several slots -- removing
         * every copy first (which is the obvious reading) collapses the
         * duplicates and silently hands back the wrong reference. */
        int pred = cur_fn;
        int refIdx = 0;
        for (int c = 0; c < sl->n_reorder && refIdx < nactive; c++) {
            int idc = sl->reorder_cmds[c][0], arg = sl->reorder_cmds[c][1];
            pic_t *target = 0;
            int picnum = -1;
            if (idc <= 1) {
                pred += idc == 0 ? -(arg + 1) : (arg + 1);
                pred = ((pred % maxfn) + maxfn) % maxfn;
                picnum = pred;
                for (int i = 0; i < nst; i++)
                    if (st[i]->frame_num == pred) { target = st[i]; break; }
            } else {                               /* idc == 2: long-term */
                for (int i = 0; i < nlt; i++)
                    if (lt[i]->lt_idx == arg) { target = lt[i]; break; }
            }
            if (!target) return -1;

            for (int cIdx = nactive; cIdx > refIdx; cIdx--)
                l0[cIdx] = l0[cIdx - 1];
            l0[refIdx++] = target;
            int nIdx = refIdx;
            for (int cIdx = refIdx; cIdx <= nactive; cIdx++) {
                pic_t *p = l0[cIdx];
                if (!p) continue;
                /* PicNumF(): a short-term entry compares by its picture
                 * number, a long-term one can never match. */
                int keep = (picnum < 0) ? (p != target)
                                        : !(p->reference == 1 &&
                                            p->frame_num == picnum);
                if (keep) l0[nIdx++] = p;
            }
            for (int i = nIdx; i <= nactive; i++) l0[i] = 0;
            if (n < nactive) n = nIdx < nactive ? nIdx : nactive;
        }
    }
    /* A conforming stream defines every active entry; if one is still empty
     * (truncated or corrupt reordering) fall back to the newest reference
     * rather than handing a null picture to motion compensation. */
    for (int i = 0; i < nactive; i++)
        if (!l0[i]) l0[i] = i > 0 ? l0[i - 1] : (n > 0 ? l0[0] : 0);
    if (!l0[0]) return -1;
    return nactive;
}

/* ============================================================= slices ==== */
static int decode_slice(h264dec *d, bs_t *bs, slice_t *sl,
                        int nal_ref_idc, int nal_type)
{
    int total = d->mbw * d->mbh;
    if (sl->first_mb_in_slice == 0) {
        if (d->cur) return H264_ERR_CORRUPT;     /* boundary handled by caller */
        if (!d->cur_sps) return H264_ERR_CORRUPT;
        int rc = new_picture(d, sl, nal_ref_idc, nal_type);
        if (rc) return rc;
        total = d->mbw * d->mbh;
    } else {
        if (!d->cur) return H264_ERR_CORRUPT;
        if (sl->first_mb_in_slice >= total) return H264_ERR_CORRUPT;
        if (d->cur_sps->mb_width != d->mbw || d->cur_sps->mb_height != d->mbh)
            return H264_ERR_CORRUPT;
    }
    if (d->n_slices < 64) d->slice_first_mb[d->n_slices] = sl->first_mb_in_slice;
    d->n_slices++;
    d->last_slice = *sl;
    TRACE("slice enter: hdr_bits=%d total_mb=%d type=%d\n", bs->bitpos, total, sl->slice_type);

    pic_t *l0[18];               /* 16 active entries + the 8.2.4.3.1 scratch */
    int n_l0 = 0;
    if (sl->slice_type == 0) {
        n_l0 = build_l0(d, sl, l0);
        if (n_l0 < 0) return H264_ERR_CORRUPT;
#ifdef H264_TRACE
        /* The list is worth printing whole: a picture may legitimately appear
         * at several indices, and the weights that go with each index are how
         * you tell an ordering bug from a weighting bug. */
        {
            char b[160]; int o = 0;
            for (int i = 0; i < n_l0 && o < 130; i++)
                o += snprintf(b + o, sizeof b - o, " fn%d(w%d,%d)",
                              l0[i]->frame_num, sl->wp_luma_w[i],
                              sl->wp_luma_o[i]);
            b[o] = 0;
            TRACE("L0 cur_fn=%d n=%d wp=%d denom=%d/%d:%s\n", sl->frame_num,
                  n_l0, d->cur_pps->weighted_pred, sl->luma_log2_weight_denom,
                  sl->chroma_log2_weight_denom, b);
        }
#endif
    }

    int qpy = ((26 + d->cur_pps->pic_init_qp + sl->slice_qp_delta) % 52 + 52) % 52;
    int addr = sl->first_mb_in_slice;
    while (addr < total) {
        if (!more_rbsp_data(bs)) break;
        if (sl->slice_type == 0) {
            uint32_t run = bs_ue(bs);
            if (bs_error(bs)) return H264_ERR_CORRUPT;
            if (run > (uint32_t)(total - addr)) return H264_ERR_CORRUPT;
            while (run--) {
                if (n_l0 == 0) return H264_ERR_CORRUPT;
                decode_skip(d, sl, addr, qpy, l0);
                addr++;
            }
            if (addr >= total) break;
            if (!more_rbsp_data(bs)) break;
        }
        int rc = decode_mb(d, bs, sl, addr, &qpy, l0, n_l0);
        if (rc) { TRACE("decode_slice: addr=%d rc=%d bitpos=%d err=%d\n",
                        addr, rc, bs->bitpos, bs->error); return rc; }
        addr++;
    }
    return H264_OK;
}

/* ==================================================== NAL dispatch / API == */
static int find_start_code(const uint8_t *p, int n)
{
    for (int i = 0; i + 2 < n; i++)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) return i;
    return -1;
}

h264dec *h264_open(void)
{
    h264dec *d = (h264dec *)malloc(sizeof *d);
    if (!d) return 0;
    memset(d, 0, sizeof *d);
    d->max_long_term_idx = -1;
    return d;
}

void h264_close(h264dec *d)
{
    if (!d) return;
    for (int i = 0; i < MAX_DPB; i++) release_pic(d, i);
    free(d->mb);
    free(d);
}

int h264_decode(h264dec *d, const uint8_t *data, int len,
                h264frame *out, int *got_frame)
{
    if (!d || !data || !got_frame || len < 0) return H264_ERR_CORRUPT;
    *got_frame = 0;
    int pos = 0;
    while (pos < len) {
        int sc = find_start_code(data + pos, len - pos);
        if (sc < 0) return len;                  /* trailing garbage: eat it */
        int nal = pos + sc + 3;
        if (nal >= len) return len;
        int next = find_start_code(data + nal, len - nal);
        int nend = next < 0 ? len : nal + next;
        /* Annex B: zero bytes immediately before a start code prefix are
         * leading_zero_8bits of the NEXT NAL, not trailing content of this
         * one -- leave them out or more_rbsp_data() logic sees phantom bits.
         * (An RBSP always ends with the stop bit, so its last byte != 0.) */
        while (nend > nal + 1 && data[nend - 1] == 0) nend--;

        uint8_t hdr = data[nal];
        if (hdr & 0x80) return H264_ERR_CORRUPT; /* forbidden_zero_bit */
        int nri = (hdr >> 5) & 3, ntype = hdr & 31;

        if (ntype == 7 || ntype == 8 || ntype == 1 || ntype == 5) {
            int rbsp_len = 0;
            uint8_t *rbsp = h264_nal_to_rbsp(data + nal, nend - nal, &rbsp_len);
            if (!rbsp) return H264_ERR_OOM;
            bs_t bs;
            bs_init(&bs, rbsp, rbsp_len);
            int rc = H264_OK;
            if (ntype == 7) {
                rc = h264_parse_sps(d, &bs);
            } else if (ntype == 8) {
                rc = h264_parse_pps(d, &bs);
            } else {
                slice_t sl;
                rc = h264_parse_slice_header(d, &bs, nri, ntype, &sl);
                if (rc == H264_OK) {
                    if (sl.first_mb_in_slice == 0 && d->cur) {
                        /* first slice of the NEXT picture: the current one is
                         * complete -- finish it and leave this NAL unconsumed */
                        finish_picture(d, out);
                        free(rbsp);
                        *got_frame = 1;
                        return pos + sc;
                    }
                    rc = decode_slice(d, &bs, &sl, nri, ntype);
                }
            }
            free(rbsp);
            if (rc) return rc;
        } else if (ntype >= 2 && ntype <= 4) {
            return H264_ERR_UNSUPPORTED;         /* data partitioning */
        }
        /* 6 (SEI), 9 (AUD), everything else: skipped */
        pos = nend;
    }
    return len;
}

int h264_flush(h264dec *d, h264frame *out)
{
    if (!d || !out) return H264_ERR_CORRUPT;
    if (!d->cur) return 0;
    finish_picture(d, out);
    return 1;
}

int h264_stream_info(h264dec *d, int *w, int *h, double *fps)
{
    if (!d) return H264_ERR_CORRUPT;
    for (int i = 0; i < 32; i++) {
        if (!d->sps[i].present) continue;
        const sps_t *s = &d->sps[i];
        int cw = s->crop_flag ? (s->crop[0] + s->crop[1]) * 2 : 0;
        int ch = s->crop_flag ? (s->crop[2] + s->crop[3]) * 2 : 0;
        if (w) *w = s->mb_width * 16 - cw;
        if (h) *h = s->mb_height * 16 - ch;
        if (fps)
            *fps = s->vui_timing && s->num_units_in_tick
                 ? (double)s->time_scale / (2.0 * s->num_units_in_tick) : 0.0;
        return H264_OK;
    }
    return H264_ERR_CORRUPT;
}
