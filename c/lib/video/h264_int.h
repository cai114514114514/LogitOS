/* c/lib/video/h264_int.h -- internal contracts between decoder modules.
 *
 * The orchestrator (h264.c) parses slices/macroblock headers and drives the
 * leaf modules, each of which is a spec-pure function set:
 *
 *   h264_cavlc.c  coefficient entropy decoding (CAVLC)        [bs -> coeffs]
 *   h264_pred.c   intra prediction modes + transforms/dequant [pixels]
 *   h264_mc.c     inter motion compensation interpolation     [ref -> pred]
 *   h264_deblock.c post-pass in-loop deblocking               [frame filter]
 *
 * All modules are pure C, no allocation at decode time (buffers are owned by
 * h264dec and handed in), no global mutable state, and treat every input as
 * untrusted: return < 0 rather than reading/writing out of bounds.
 */
#ifndef LOGIT_H264_INT_H
#define LOGIT_H264_INT_H

#include <stdint.h>
#include "bs.h"

/* ------------------------------------------------------------------ SPS -- */
typedef struct {
    int present;
    int profile_idc, level_idc, sps_id;
    int log2_max_frame_num;           /* 4..16 bits, minus4 stored raw + 4 */
    int poc_type;                     /* 0,1,2 (baseline uses 0 or 2) */
    int log2_max_poc_lsb;             /* poc_type 0 */
    int delta_pic_order_always_zero;  /* poc_type 1 */
    int offset_for_non_ref_pic;
    int offset_for_top_to_bottom;
    int num_ref_frames_in_poc_cycle;  /* poc_type 1 (baseline: expect 0) */
    int max_num_ref_frames;
    int gaps_in_frame_num_allowed;
    int mb_width, mb_height;          /* map units; frame_mbs_only required */
    int frame_mbs_only;
    int direct_8x8_inference;
    int crop[4];                      /* left,right,top,bottom in crop units */
    int crop_flag;
    /* VUI (only what we use) */
    int vui_timing;
    uint32_t num_units_in_tick, time_scale;
    int vui_fixed_rate;
} sps_t;

/* ------------------------------------------------------------------ PPS -- */
typedef struct {
    int present;
    int pps_id, sps_id;
    int entropy_cabac;                /* must be 0 for us */
    int bottom_field_poc_in_frame;
    int num_slice_groups;             /* must be 1 (FMO unsupported) */
    int num_ref_idx_l0_default, num_ref_idx_l1_default;
    int weighted_pred, weighted_bipred_idc;
    int pic_init_qp, pic_init_qs;
    int chroma_qp_index_offset;
    int deblock_control_present;
    int constrained_intra_pred;
    int redundant_pic_cnt_present;    /* must be 0 */
    int transform_8x8;                /* must be 0 */
    int second_chroma_qp_offset, has_second_chroma_offset;
} pps_t;

/* --------------------------------------------------------------- slice -- */
typedef struct {
    int first_mb_in_slice;
    int slice_type;                   /* canonical: 0=P 1=B 2=I (5/6/7 folded) */
    int pps_id;
    int frame_num;
    int idr_pic_id;                   /* IDR only */
    int poc_lsb;                      /* poc_type 0 */
    int delta_poc_bottom;             /* poc_type 0 && bottom_field flag */
    int delta_poc[2];                 /* poc_type 1 */
    int redundant_pic_cnt;
    int num_ref_idx_l0_active;        /* after slice-header override */
    int ref_reorder_present;          /* we apply reordering to L0 */
    int reorder_cmds[32][2];          /* (idc, arg) raw, applied by h264.c */
    int n_reorder;
    int no_output_of_prior_pics, long_term_reference_flag;   /* IDR MMCO=5-ish */
    int adaptive_marking;             /* dec_ref_pic_marking for non-IDR */
    int mmco[32][2];                  /* (mmco, arg1) ; arg2 folded into arg1 hi */
    int n_mmco;
    int cabac_init_idc;
    int slice_qp_delta;
    int disable_deblocking_filter_idc;/* 0=on 1=off 2=on, no slice-edge */
    int slice_alpha_c0_offset, slice_beta_offset;
    /* weighted P prediction (pps->weighted_pred): defaults weight=1<<denom */
    int luma_log2_weight_denom, chroma_log2_weight_denom;
    int wp_luma_w[16], wp_luma_o[16];
    int wp_chroma_w[16][2], wp_chroma_o[16][2];
} slice_t;

/* ------------------------------------------------------- macroblock info -- */
/* Kept for every MB of the current frame; the deblock post-pass reads it. */
#define MB_I4x4    0
#define MB_I16x16  1
#define MB_I_PCM   2
#define MB_P_L0    3        /* inter, any partition layout */
#define MB_P_SKIP  4
typedef struct {
    uint8_t  type;          /* MB_* */
    uint8_t  cbp;           /* coded block pattern (post-CAVLC) */
    int8_t   qp;            /* luma qp (0..51) */
    uint8_t  intra16_mode;  /* I16x16: prediction mode 0..3 */
    uint8_t  nz_i16dc;      /* I16x16: nonzero count of the luma DC block */
    uint8_t  nz[24];        /* nonzero counts per 4x4: 16 luma, 4 Cb, 4 Cr.
                               For I16x16 the luma entries count AC only. */
    uint8_t  ref_idx[4];    /* per 8x8 region L0 ref index (0..15; 0xFF intra) */
    uint8_t  i4mode[16];    /* I4x4: per-block intra pred mode (mode-pred ctx) */
    int16_t  mv[16][2];     /* per 4x4 block motion vector, quarter-pel units */
} mbinfo_t;

/* ----------------------------------------------------------- public pics -- */
typedef struct {
    uint8_t *y, *u, *v;
    int stride_y, stride_c;
    int used;                 /* slot occupied */
    int reference;            /* 0=none 1=short-term 2=long-term */
    int frame_num;            /* short-term id */
    int lt_idx;               /* long-term frame idx */
    int poc;                  /* for output ordering */
    int needed_for_output;
    int mb_field;             /* unused (frames only), kept for clarity */
} pic_t;

/* ------------------------------------------------------------- decoder -- */
#define MAX_DPB 17            /* 16 reference + current decode buffer */
typedef struct h264dec {
    sps_t sps[32];
    pps_t pps[32];
    sps_t *cur_sps;
    pps_t *cur_pps;

    /* current frame under construction */
    pic_t  pics[MAX_DPB];     /* DPB + decode buffer slots */
    pic_t *cur;               /* slot being decoded into */
    mbinfo_t *mb;             /* mbw*mbh info array for the current frame */
    int mbw, mbh;             /* in macroblocks */
    int width, height;        /* cropped, visible */
    int stride_y, stride_c;

    /* neighbor scratch for intra pred (top row persists across the MB row) */
    uint8_t *top_y;           /* mbw*16 + 16 */
    uint8_t *top_u, *top_v;   /* mbw*8 + 8 each */

    int prev_frame_num;       /* for gap detection */
    int frame_num_offset;     /* after gaps */
    int idr_seen;
    int max_long_term_idx;    /* DPB bookkeeping, -1 = none */
    int next_lt_idx;

    /* pending output queue: indices into pics[], poc-ordered lazily */
    int eos;
    int err;

    /* ---- orchestrator state (h264.c) ---- */
    slice_t last_slice;           /* slice header of the latest slice (deblock) */
    int slice_first_mb[64];       /* raster-ordered slice starts, this frame */
    int n_slices;
    int prev_poc_msb, prev_poc_lsb;
    int cur_idr;                  /* current picture is IDR */
    int cur_ref;                  /* current picture nal_ref_idc != 0 */
    uint8_t emu_luma[24 * 24];    /* edge-emulation scratch for far-out MVs */
    uint8_t emu_chroma[10 * 10];
} h264dec;

/* ----------------------------------------------------- module contracts -- */
/* h264_nal.c: parse parameter sets + slice header from an RBSP buffer. */
int h264_parse_sps(h264dec *d, bs_t *bs);            /* fills d->sps[id] */
int h264_parse_pps(h264dec *d, bs_t *bs);            /* fills d->pps[id] */
int h264_parse_slice_header(h264dec *d, bs_t *bs, int nal_ref_idc,
                            int nal_type, slice_t *sl);

/* h264_cavlc.c: decode one block of coefficients (level values, zigzag
 * order positions filled by caller? NO -- fills coef[16] in zigzag order).
 * nC: neighbor context (-1 for chroma DC). max: 16 (I4x4 luma incl. DC),
 * 15 (AC-only blocks: I16x16 luma AC, chroma AC) or 4 (chroma DC).
 * Returns total coeff count, or < 0 on corrupt input. */
int cavlc_decode(bs_t *bs, int nC, int max, int coef[16]);

/* h264_pred.c: transforms + intra prediction. All clip internally. */
void h264_dequant_idct_add(int coef[16], int qp, uint8_t *dst, int stride);
void h264_dc16_transform(int dc[16]);              /* inv hadamard 4x4 (I16) */
void h264_dcdcm_transform(int dc[4]);              /* inv hadamard 2x2 (chr) */
void h264_intra4x4(uint8_t *dst, int stride, int mode,
                   const uint8_t *top, const uint8_t *left, int topleft,
                   int avail_left, int avail_top, int avail_topright,
                   int avail_topleft);
void h264_intra16x16(uint8_t *dst, int stride, int mode,
                     const uint8_t *top, const uint8_t *left, int topleft,
                     int avail_left, int avail_top);
void h264_intra_chroma(uint8_t *dst, int stride, int mode,
                       const uint8_t *top, const uint8_t *left, int topleft,
                       int avail_left, int avail_top);

/* h264_mc.c: quarter-pel motion compensation of one WxH block (W,H in
 * {4,8,16}) from a reference plane. mvx/mvy in quarter-pel units (chroma
 * derives its own eighth-pel position from the same mv), block origin (x,y)
 * in visible-frame pixel coordinates.
 *
 * `ref` points at the VISIBLE (0,0) pixel of the reference plane. The plane
 * is bordered by H264_PAD replicated edge pixels on all four sides (h264.c
 * maintains them after deblocking), so any access an H.264 MV can produce
 * stays in bounds -- mc reads without clamping. Luma: 6-tap (1,-5,20,20,-5,1)
 * half-pel then bilinear quarter-pel (spec 8.4.2.2.1). Chroma: 1/8-pel
 * bilinear (spec 8.4.2.2.2). */
#define H264_PAD 32
void h264_mc_block(uint8_t *dst, int dst_stride,
                   const uint8_t *ref, int ref_stride,
                   int x, int y, int w, int h, int mvx, int mvy, int is_luma);
/* Explicit weighted prediction (P slices, pps->weighted_pred): rescales the
 * block in place after mc. */
void h264_mc_weight(uint8_t *dst, int stride, int w, int h,
                    int log2w, int weight, int offset);

/* h264_deblock.c: post-pass over a finished frame (spec 8.7). Called only
 * when the slice did not fully disable deblocking. slice_first_mb[] lists
 * the first_mb_in_slice of every slice that built the frame (raster order);
 * when sl->disable_deblocking_filter_idc == 2, edges BETWEEN those slices
 * are skipped. Chroma qp derives from luma qp + pps->chroma_qp_index_offset
 * per spec 8.5.8. */
void h264_deblock_frame(uint8_t *y, uint8_t *u, uint8_t *v,
                        int stride_y, int stride_c, int mbw, int mbh,
                        const mbinfo_t *mb, const slice_t *sl,
                        const sps_t *sps, const pps_t *pps,
                        const int *slice_first_mb, int n_slices);

/* h264_tables.h: CAVLC VLC tables, zigzag scan, dequant scale table. */

#endif /* LOGIT_H264_INT_H */
