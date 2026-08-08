/* c/lib/video/h264_int.h -- internal contracts between decoder modules.
 *
 * The orchestrator (h264.c) parses slices/macroblock headers and drives the
 * leaf modules, each of which is a spec-pure function set:
 *
 *   h264_cavlc.c  coefficient entropy decoding (CAVLC)        [bs -> coeffs]
 *   h264_cabac.c  the arithmetic decoder + residual_block_cabac
 *   h264_pred.c   intra prediction modes + transforms/dequant [pixels]
 *   h264_mc.c     inter motion compensation interpolation     [ref -> pred]
 *   h264_deblock.c post-pass in-loop deblocking               [frame filter]
 *
 * All modules are pure C, no allocation at decode time (buffers are owned by
 * h264dec and handed in), no global mutable state, and treat every input as
 * untrusted: return < 0 rather than reading/writing out of bounds.
 *
 * Coverage is Baseline + Main + High, 4:2:0, 8-bit, progressive: CAVLC and
 * CABAC, I/P/B slices, the 8x8 transform and its intra modes, scaling
 * matrices. What is refused in the parser rather than half-implemented:
 * fields/MBAFF, 4:0:0/4:2:2/4:4:4, bit depths above 8, data partitioning,
 * FMO/ASO, SP/SI slices.
 */
#ifndef LOGIT_H264_INT_H
#define LOGIT_H264_INT_H

#include <stdint.h>
#include "bs.h"
#include "h264.h"        /* h264frame, the H264_ERR_* codes */

/* ------------------------------------------------------------------ SPS -- */
typedef struct {
    int present;
    int profile_idc, level_idc, sps_id;
    int chroma_format_idc;            /* 1 = 4:2:0; anything else is refused */
    int bit_depth_luma, bit_depth_chroma;             /* 8 only */
    int qpprime_y_zero_transform_bypass;
    int log2_max_frame_num;           /* 4..16 bits, minus4 stored raw + 4 */
    int poc_type;                     /* 0,1,2 */
    int log2_max_poc_lsb;             /* poc_type 0 */
    int delta_pic_order_always_zero;  /* poc_type 1 */
    int offset_for_non_ref_pic;
    int offset_for_top_to_bottom;
    int num_ref_frames_in_poc_cycle;  /* poc_type 1 */
    int max_num_ref_frames;
    int gaps_in_frame_num_allowed;
    int mb_width, mb_height;          /* map units; frame_mbs_only required */
    int frame_mbs_only;
    int direct_8x8_inference;
    int crop[4];                      /* left,right,top,bottom in crop units */
    int crop_flag;
    /* Scaling matrices (7.3.2.1.1.1). Stored in RASTER order, already through
     * the fall-back rules of Table 7-2, so a consumer never has to know
     * whether a list was coded, inherited or defaulted. */
    int scaling_present;
    uint8_t scaling4[6][16];          /* Intra Y/Cb/Cr, Inter Y/Cb/Cr */
    uint8_t scaling8[2][64];          /* 4:2:0 codes only Intra Y and Inter Y */
    /* VUI (only what we use) */
    int vui_timing;
    uint32_t num_units_in_tick, time_scale;
    int vui_fixed_rate;
    int has_bitstream_restriction;
    int num_reorder_frames;           /* VUI max_num_reorder_frames */
    int max_dec_frame_buffering;
} sps_t;

/* ------------------------------------------------------------------ PPS -- */
typedef struct {
    int present;
    int pps_id, sps_id;
    int entropy_cabac;                /* entropy_coding_mode_flag */
    int bottom_field_poc_in_frame;
    int num_slice_groups;             /* must be 1 (FMO unsupported) */
    int num_ref_idx_l0_default, num_ref_idx_l1_default;
    int weighted_pred, weighted_bipred_idc;
    int pic_init_qp, pic_init_qs;
    int chroma_qp_index_offset;
    int deblock_control_present;
    int constrained_intra_pred;
    int redundant_pic_cnt_present;    /* must be 0 */
    int transform_8x8;
    int second_chroma_qp_offset, has_second_chroma_offset;
    /* Effective scaling matrices for this PPS: the SPS lists with the PPS's
     * own overrides and Table 7-2's fall-back rule B already applied. */
    uint8_t scaling4[6][16];
    uint8_t scaling8[2][64];
    /* LevelScale (8.5.9), precomputed once per PPS because it depends only on
     * the scaling lists: ls4[list][qP%6][raster], ls8[list][qP%6][raster].
     * With the flat default lists ls4 == 16 * v, which is where the older
     * "coefficient * v << qP/6" arithmetic came from. */
    int ls4[6][6][16];
    int ls8[2][6][64];
} pps_t;

/* --------------------------------------------------------------- slice -- */
#define SLICE_P 0
#define SLICE_B 1
#define SLICE_I 2

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
    int direct_spatial_mv_pred;       /* B slices */
    int num_ref_idx_active[2];        /* after slice-header override */
    int reorder_cmds[2][32][2];       /* (idc, arg) raw, applied by h264.c */
    int n_reorder[2];
    int no_output_of_prior_pics, long_term_reference_flag;   /* IDR */
    int adaptive_marking;             /* dec_ref_pic_marking for non-IDR */
    int mmco[32][2];                  /* (mmco, arg1) ; arg2 folded into arg1 hi */
    int n_mmco;
    int cabac_init_idc;
    int slice_qp_delta;
    int disable_deblocking_filter_idc;/* 0=on 1=off 2=on, no slice-edge */
    int slice_alpha_c0_offset, slice_beta_offset;
    /* Explicit weighted prediction (pred_weight_table, 7.3.3.2): used for P
     * when pps->weighted_pred, and for B when weighted_bipred_idc == 1.
     * Defaults are weight = 1 << denom, offset = 0. */
    int luma_log2_weight_denom, chroma_log2_weight_denom;
    int wp_luma_w[2][32], wp_luma_o[2][32];
    int wp_chroma_w[2][32][2], wp_chroma_o[2][32][2];
} slice_t;

/* ------------------------------------------------------- macroblock info -- */
/* Kept for every MB of the current frame; the deblock post-pass reads it, and
 * so do the CAVLC nC / CABAC ctxIdxInc / motion-vector-prediction neighbour
 * derivations. */
#define MB_I4x4    0
#define MB_I16x16  1
#define MB_I_PCM   2
#define MB_INTER   3        /* P or B, any partition layout */
#define MB_SKIP    4        /* P_Skip or B_Skip */

typedef struct {
    uint8_t  type;          /* MB_* */
    uint8_t  cbp;           /* coded block pattern (post-entropy) */
    int8_t   qp;            /* luma qp (0..51) */
    uint8_t  intra16_mode;  /* I16x16: prediction mode 0..3 */
    uint8_t  nz_i16dc;      /* I16x16: nonzero count of the luma DC block */
    uint8_t  nz[24];        /* nonzero counts per 4x4: 16 luma, 4 Cb, 4 Cr.
                               For I16x16 the luma entries count AC only.
                               An 8x8-transform block writes its count into all
                               four of its 4x4 entries -- both deblocking and
                               the CABAC coded_block_flag context ask the
                               question per 4x4. */
    uint8_t  nz_cdc[2];     /* chroma DC nonzero count per component. CAVLC
                               never asks (chroma DC uses nC = -1); CABAC's
                               coded_block_flag context does. */
    uint8_t  transform8x8;  /* transform_size_8x8_flag */
    uint8_t  chroma_mode;   /* intra_chroma_pred_mode (CABAC context) */
    uint8_t  skip;          /* mb_skip_flag (CABAC context) */
    uint8_t  direct8x8;     /* bit per 8x8: derived by a direct mode */
    uint8_t  bdirect16;     /* mb_type was B_Skip or B_Direct_16x16 exactly.
                               Not the same question as "all four 8x8s are
                               direct": 9.3.3.1.1.3 names the two mb_types, and
                               a B_8x8 whose sub_mb_types all happen to be
                               B_Direct_8x8 is neither of them. */
    int8_t   ref_idx[2][4]; /* per 8x8 quadrant, per list; -1 = list unused */
    /* Per 8x8 quadrant and list, the DPB slot of the picture that index
     * resolves to (-1 = none). Deblocking asks "are the two sides predicted
     * from the same reference PICTURE" (8.7.2.1, and its note is explicit that
     * the index position in the list must not enter into it), which is not the
     * same question as "are the two ref_idx equal": weighted prediction puts
     * one picture at several indices on purpose, so that each index can carry
     * its own weights. Motion vector prediction, by contrast, really does
     * compare indices -- so both are kept. */
    int8_t   ref_pic[2][4];
    uint8_t  i4mode[16];    /* I4x4/I8x8: per-block intra pred mode. An I8x8
                               macroblock writes each 8x8 mode into all four of
                               its 4x4 slots, which is exactly what the
                               mode-prediction neighbour derivation wants. */
    int16_t  mv[2][16][2];  /* per 4x4 block motion vector, quarter-pel units */
    int16_t  mvd[2][16][2]; /* the coded difference; CABAC context only */
} mbinfo_t;

/* --------------------------------------------------- colocated motion ---- */
/* What a B slice's direct modes need from an already-decoded picture (8.4.1.2).
 * Kept per reference picture and resolved AT WRITE TIME: 8.4.1.2.1 says to use
 * the colocated block's L0 motion unless predFlagL0 is 0, in which case its L1
 * motion -- a question about the colocated picture, not about the current
 * slice, so answering it once when that picture was decoded costs one mv and
 * one POC per 4x4 block instead of the full two-list state.
 *
 * ref_poc holds the POC of the picture the colocated block referenced, not a
 * DPB slot: slots get recycled, and the temporal-direct mapping has to find
 * "the lowest index in the current RefPicList0 that references that picture",
 * which is a question about picture identity. COL_NOREF marks intra or
 * otherwise absent motion. */
#define COL_NOREF ((int32_t)0x80000000)
typedef struct {
    int16_t mv[16][2];
    int32_t ref_poc[16];
    uint16_t ref_lt;        /* bit per 4x4: the referenced picture is long-term */
    uint16_t ref_zero;      /* bit per 4x4: refIdxCol was 0. Spatial direct's
                               colZeroFlag asks for the INDEX being zero, not
                               for the picture, so the answer cannot be
                               recovered from ref_poc later. */
    uint8_t  intra;         /* the colocated macroblock is intra coded */
} colmb_t;

/* ----------------------------------------------------------- public pics -- */
typedef struct {
    uint8_t *y, *u, *v;
    int stride_y, stride_c;
    int used;                 /* slot occupied */
    int reference;            /* 0=none 1=short-term 2=long-term */
    int frame_num;            /* short-term id */
    int lt_idx;               /* long-term frame idx */
    int poc;                  /* for output ordering */
    int64_t pts;              /* the caller value that came with this picture */
    int needed_for_output;
    int output_seq;           /* decode-order tiebreak for equal POC */
    colmb_t *col;             /* colocated motion field (NULL if not kept) */
    int mb_field;             /* unused (frames only), kept for clarity */
} pic_t;

/* ------------------------------------------------------------- decoder -- */
/* Slots, not buffers: a slot costs a struct, the picture memory behind it is
 * allocated on demand and freed as soon as the picture is neither a reference
 * nor waiting to be output. The bound that matters is how many pictures are
 * alive at once, which the bumping process holds at max_num_ref_frames +
 * max_num_reorder_frames + 1. Sizing the array for the spec's worst case
 * (16 references, 16 held back for reordering, one being decoded) just means a
 * conforming stream can never run out of slots. */
#define MAX_DPB 34

/* CABAC decoding engine state (9.3.3.2 / 9.3.4.3). */
typedef struct {
    const uint8_t *buf;
    int len;
    int bytepos, bitpos;
    uint32_t range, offset;
    int overrun;              /* reads past the end of the slice data */
    int error;
    uint8_t state[460];       /* (pStateIdx << 1) | valMPS per context */
} h264cabac;

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
    /* Which 4x4 blocks of the macroblock being decoded already have their
     * motion vector, in raster bit order (brow*4 + bcol). A partition of the
     * CURRENT macroblock that is not in here is "not yet decoded" and so is
     * not an available neighbour (6.4.11.7) -- the case that bites is the C
     * neighbour of an 8x4 sub-partition, which points into the 8x8 region to
     * its right and that region is coded later. */
    uint16_t mb_mv_done;
    int width, height;        /* cropped, visible */
    int stride_y, stride_c;

    int prev_frame_num;       /* for gap detection */
    int frame_num_offset;     /* after gaps */
    int idr_seen;
    int max_long_term_idx;    /* DPB bookkeeping, -1 = none */
    int next_lt_idx;

    int eos;
    int err;

    /* ---- orchestrator state (h264.c) ---- */
    slice_t last_slice;           /* slice header of the latest slice (deblock) */
    int slice_first_mb[64];       /* raster-ordered slice starts, this frame */
    int n_slices;
    int prev_poc_msb, prev_poc_lsb;
    int cur_idr;                  /* current picture is IDR */
    int cur_ref;                  /* current picture nal_ref_idc != 0 */
    int num_reorder;              /* DPB output delay, in frames */
    int output_seq;               /* decode-order counter for output ordering */
    int pending_free;             /* slot to release at the next entry, -1 none */
    int64_t next_pts;             /* attaches to the next picture that starts */
    /* Reference lists of the slice being decoded, and the weight tables that
     * go with them (implicit bipred derives weights from POC, so they are not
     * simply the slice header's). */
    pic_t *rl[2][34];             /* 32 active entries + 8.2.4.3.1 scratch */
    int n_rl[2];
    int impl_w[32][32][2];        /* implicit bipred: [refL0][refL1][0..1] */
    int use_weight;               /* 0 none, 1 explicit, 2 implicit */

    h264cabac cab;
    int cabac;                    /* the current slice uses CABAC */
    int last_qp_delta;            /* CABAC mb_qp_delta context */
    int drain;                    /* emit everything pending before decoding on */

    /* Direct-mode state, derived once per macroblock (8.4.1.2). Spatial
     * direct picks its reference indices from the MACROBLOCK's neighbours, so
     * they are the same for all sixteen 4x4 blocks and only the vectors vary;
     * temporal direct varies both. Holding the whole 16-block answer lets a
     * B_8x8 apply it to just the sub-macroblocks that asked for it. */
    int16_t dir_mv[2][16][2];
    int8_t  dir_ref[2][16];

    uint8_t emu_luma[24 * 24];    /* edge-emulation scratch for far-out MVs */
    uint8_t emu_chroma[10 * 10];
    /* Bi-prediction scratch: the two lists are motion compensated into these
     * and then combined. 16x16 luma plus 8x8 of each chroma component. */
    uint8_t bip_y[2][16 * 16];
    uint8_t bip_u[2][8 * 8];
    uint8_t bip_v[2][8 * 8];
} h264dec;

/* ----------------------------------------------------- module contracts -- */
/* h264_nal.c: parse parameter sets + slice header from an RBSP buffer. */
int h264_parse_sps(h264dec *d, bs_t *bs);            /* fills d->sps[id] */
int h264_parse_pps(h264dec *d, bs_t *bs);            /* fills d->pps[id] */
int h264_parse_slice_header(h264dec *d, bs_t *bs, int nal_ref_idc,
                            int nal_type, slice_t *sl);

/* h264_cavlc.c: decode one block of coefficients into coef[] in SCAN order.
 * nC: neighbor context (-1 for chroma DC). max: 16 (I4x4 luma incl. DC),
 * 15 (AC-only blocks: I16x16 luma AC, chroma AC) or 4 (chroma DC).
 * Returns total coeff count, or < 0 on corrupt input. */
int cavlc_decode(bs_t *bs, int nC, int max, int coef[16]);

/* h264_cabac.c -- the arithmetic decoder.
 *
 * ctxIdx values are the spec's own (Table 9-11); h264.c passes them in, so
 * this module never needs to know what a macroblock is. Every read past the
 * end of the slice returns zero bits and counts an overrun; 64 of those set
 * the sticky error, which h264.c checks at macroblock boundaries. */
void h264_cabac_init(h264cabac *c, const uint8_t *buf, int len,
                     int slice_qp, int slice_type, int cabac_init_idc);
void h264_cabac_restart(h264cabac *c, int bytepos);   /* I_PCM: engine only */
int  h264_cabac_decision(h264cabac *c, int ctx_idx);
int  h264_cabac_bypass(h264cabac *c);
int  h264_cabac_terminate(h264cabac *c);
/* Unary / UEGk helpers shared by several syntax elements. */
int  h264_cabac_ueg(h264cabac *c, int ctx, int ctx_inc_max, int k,
                    int u_coff, int sign);
/* residual_block_cabac (9.3.2.3 + 7.3.5.3.3). `cat` is ctxBlockCat (0..5),
 * max_coeff the block's coefficient count (4, 15, 16 or 64), cbf_inc the
 * ctxIdxInc for coded_block_flag or -1 when the block carries none (a 4:2:0
 * 8x8 luma block, where 7.3.5.3.3 omits it). Fills coef[] in SCAN order and
 * returns the number of nonzero coefficients (0 = block not coded), or < 0
 * on corrupt input. */
int  h264_cabac_residual(h264cabac *c, int cat, int max_coeff, int cbf_inc,
                         int coef[64]);

/* h264_pred.c: transforms + intra prediction. All clip internally.
 * `ls` is the LevelScale row for this qP%6 (16 or 64 entries, raster order)
 * from pps_t; passing it in is what makes scaling matrices work without every
 * transform needing to know about parameter sets. */
void h264_dequant_idct_add(int coef[16], int qp, const int *ls,
                           uint8_t *dst, int stride);
void h264_dequant_idct8_add(int coef[64], int qp, const int *ls8,
                            uint8_t *dst, int stride);
void h264_dc16_transform(int dc[16]);              /* inv hadamard 4x4 (I16) */
void h264_dcdcm_transform(int dc[4]);              /* inv hadamard 2x2 (chr) */
void h264_intra4x4(uint8_t *dst, int stride, int mode,
                   const uint8_t *top, const uint8_t *left, int topleft,
                   int avail_left, int avail_top, int avail_topright,
                   int avail_topleft);
/* 8x8 luma intra prediction (8.3.2). `top` holds 16 samples (the block's own
 * row plus the above-right row), `left` 8. The reference-sample filtering of
 * 8.3.2.2.1 happens inside. */
void h264_intra8x8(uint8_t *dst, int stride, int mode,
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
 * {2,4,8,16}) from a reference plane. mvx/mvy in quarter-pel units (chroma
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
/* Explicit weighted prediction of a single-list block (8.4.2.3.2): rescales
 * the block in place after mc. */
void h264_mc_weight(uint8_t *dst, int stride, int w, int h,
                    int log2w, int weight, int offset);
/* Bi-prediction combination (8.4.2.3). The default is the rounded average;
 * the weighted form covers both explicit and implicit weights, which differ
 * only in where w0/w1/o0/o1 came from. */
void h264_mc_bi_avg(uint8_t *dst, int dst_stride,
                    const uint8_t *a, int as, const uint8_t *b, int bs,
                    int w, int h);
void h264_mc_bi_weight(uint8_t *dst, int dst_stride,
                       const uint8_t *a, int as, const uint8_t *b, int bs,
                       int w, int h, int log2w,
                       int w0, int w1, int o0, int o1);

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

/* ------------------------------------------- h264_dpb.c (picture store) -- */
/* Everything about pictures as objects: allocation, the decoded picture
 * buffer, picture order counts, reference marking, the two reference lists,
 * implicit bi-prediction weights, the colocated motion field and the output
 * (bumping) process. Split from the macroblock decoder because none of it
 * looks at a pixel. */
void h264_release_pic(h264dec *d, int i);
int  h264_alloc_picture(h264dec *d);
void h264_border_pad(h264dec *d, pic_t *p);
int  h264_compute_poc(h264dec *d, const slice_t *sl, int nal_ref_idc, int nal_type);
void h264_mark_refs(h264dec *d);
void h264_store_colocated(h264dec *d);
int  h264_build_lists(h264dec *d, slice_t *sl);
void h264_calc_implicit_weights(h264dec *d);
int  h264_set_reorder_depth(h264dec *d);
int  h264_pending_output(const h264dec *d);
int  h264_bump_one(h264dec *d, h264frame *out);

/* --------------------------------------------- h264_mb.c (macroblock) --- */
/* One macroblock, entropy coder and all. `addr` is the raster address, qpyp
 * the running QPY the slice threads through its macroblocks. Returns H264_OK
 * or an H264_ERR_*. */
int  h264_decode_mb(h264dec *d, bs_t *bs, slice_t *sl, int addr, int *qpyp);
/* P_Skip / B_Skip: no syntax elements of its own beyond the skip flag. */
int  h264_decode_skip_mb(h264dec *d, slice_t *sl, int addr, int qpy);

/* ------------------------------------------ h264.c (shared by the above) - */
int  h264_slice_of(const h264dec *d, int addr);
int  h264_intra_avail(h264dec *d, int cur_addr, int mbx, int mby);
int  h264_is_intra_type(int t);
int  h264_chroma_qp(int qpy, int offset);
/* Motion vector prediction (8.4.1.3) for one partition of the macroblock at
 * (mbx, mby); (px, py) and (pw, ph) are in 4x4 units. dir_kind selects the
 * directional shortcuts: 0 none, 1 = 16x8 halves, 2 = 8x16 halves. */
void h264_mv_pred(h264dec *d, int cur, int mbx, int mby,
                  int px, int py, int pw, int ph, int list, int ref,
                  int dir_kind, int *outx, int *outy);
void h264_mv_pred_skip(h264dec *d, int cur, int mbx, int mby,
                       int *outx, int *outy);
/* Fills d->dir_mv / d->dir_ref for the macroblock at `addr` (8.4.1.2). */
void h264_direct_motion(h264dec *d, slice_t *sl, int addr);
/* Motion compensation of one partition, including weighting and
 * bi-prediction. `pred` is 0 = L0, 1 = L1, 2 = Bi. */
void h264_inter_pred(h264dec *d, slice_t *sl, int addr,
                     int px, int py, int pw4, int ph4, int pred);
/* CABAC neighbour helper shared by the macroblock layer: the |mvd| a
 * neighbouring 4x4 block contributes to the mvd context (0 when absent). */
int  h264_mvd_ctx(h264dec *d, int addr, int gx, int gy, int list, int comp);

#endif /* LOGIT_H264_INT_H */
