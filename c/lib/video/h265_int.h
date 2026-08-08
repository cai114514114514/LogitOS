/* c/lib/video/h265_int.h -- internal contracts between the HEVC modules.
 *
 * Same split as the H.264 decoder next door: an orchestrator (h265.c) that
 * parses the CTU quadtree and drives spec-pure leaf modules.
 *
 *   h265_cabac.c    the arithmetic decoder + every context-coded syntax
 *                   element                                  [bits -> values]
 *   h265_pred.c     intra prediction, dequant, inverse transforms  [pixels]
 *   h265_mc.c       inter interpolation + weighted prediction   [ref -> pred]
 *   h265_deblock.c  the in-loop deblocking filter and SAO      [frame filter]
 *   h265_nal.c      Annex B / VPS / SPS / PPS / slice headers   [bits -> sets]
 *
 * All leaf modules are pure C, allocate nothing at decode time, keep no global
 * mutable state and treat every input as untrusted.
 */
#ifndef LOGIT_H265_INT_H
#define LOGIT_H265_INT_H

#include <stdint.h>
#include "bs.h"

/* --------------------------------------------------------------- limits -- */
#define H265_PAD        80     /* reference-plane border, >= 64 + filter tap */
#define H265_MAX_DPB    17     /* 16 references + the picture being decoded */
#define H265_MAX_REFS   16
#define H265_MAX_SPS    16
#define H265_MAX_PPS    64
#define H265_MAX_RPS    64
#define H265_MAX_TILES  22     /* level 6.2 caps columns/rows at 20 */
#define H265_MAX_SUBSTREAMS 600

/* NAL unit types we care about (Table 7-1). */
#define NAL_TRAIL_N   0
#define NAL_TRAIL_R   1
#define NAL_TSA_N     2
#define NAL_TSA_R     3
#define NAL_STSA_N    4
#define NAL_STSA_R    5
#define NAL_RADL_N    6
#define NAL_RADL_R    7
#define NAL_RASL_N    8
#define NAL_RASL_R    9
#define NAL_BLA_W_LP  16
#define NAL_BLA_W_RADL 17
#define NAL_BLA_N_LP  18
#define NAL_IDR_W_RADL 19
#define NAL_IDR_N_LP  20
#define NAL_CRA       21
#define NAL_VPS       32
#define NAL_SPS       33
#define NAL_PPS       34
#define NAL_AUD       35
#define NAL_EOS       36
#define NAL_EOB       37
#define NAL_FD        38
#define NAL_SEI_PREFIX 39
#define NAL_SEI_SUFFIX 40

#define IS_IRAP(t)     ((t) >= 16 && (t) <= 23)
#define IS_IDR(t)      ((t) == NAL_IDR_W_RADL || (t) == NAL_IDR_N_LP)
#define IS_BLA(t)      ((t) >= NAL_BLA_W_LP && (t) <= NAL_BLA_N_LP)
#define IS_RASL(t)     ((t) == NAL_RASL_N || (t) == NAL_RASL_R)
#define IS_VCL(t)      ((t) <= 31)
/* sub-layer non-reference pictures: even types below 16 */
#define IS_SUBLAYER_NONREF(t) ((t) < 16 && ((t) & 1) == 0)

/* slice types */
#define SLICE_B 0
#define SLICE_P 1
#define SLICE_I 2

/* CU prediction modes */
#define MODE_INTER 0
#define MODE_INTRA 1
#define MODE_SKIP  2

/* PU partition modes (Table 7-10) */
#define PART_2Nx2N 0
#define PART_2NxN  1
#define PART_Nx2N  2
#define PART_NxN   3
#define PART_2NxnU 4
#define PART_2NxnD 5
#define PART_nLx2N 6
#define PART_nRx2N 7

/* SAO */
#define SAO_NOT_APPLIED 0
#define SAO_BAND        1
#define SAO_EDGE        2

/* ---------------------------------------------------- short-term RPS ----- */
typedef struct {
    int num_negative, num_positive;
    int delta_poc[H265_MAX_REFS];      /* negatives first, then positives */
    uint8_t used[H265_MAX_REFS];
} strps_t;

/* -------------------------------------------------------------- VPS ------ */
typedef struct {
    int present;
    int vps_id;
    int max_sub_layers;
} vps_t;

/* -------------------------------------------------------------- SPS ------ */
typedef struct {
    int present;
    int sps_id, vps_id;
    int max_sub_layers;
    int chroma_format_idc;
    int separate_colour_plane;
    int width, height;                 /* pic_width/height_in_luma_samples */
    int conf_win[4];                   /* left, right, top, bottom (chroma units) */
    int bit_depth_luma, bit_depth_chroma;
    int log2_max_poc_lsb;
    int max_dec_pic_buffering[8];      /* per sub-layer, +1 already applied */
    int max_num_reorder[8];
    int max_latency[8];                /* 0 = unbounded */
    int log2_min_cb, log2_ctb;         /* luma coding block sizes */
    int log2_min_tb, log2_max_tb;
    int max_transform_hierarchy_depth_inter, max_transform_hierarchy_depth_intra;
    int scaling_list_enabled, sps_scaling_list_data_present;
    int amp_enabled;
    int sao_enabled;
    int pcm_enabled, pcm_bit_depth_luma, pcm_bit_depth_chroma;
    int log2_min_pcm_cb, log2_max_pcm_cb, pcm_loop_filter_disabled;
    int num_strps;
    strps_t strps[H265_MAX_RPS];
    int long_term_ref_pics_present;
    int num_long_term_sps;
    int lt_ref_poc_lsb_sps[32], lt_used_by_curr_sps[32];
    int temporal_mvp_enabled;
    int strong_intra_smoothing;
    /* derived */
    int ctb_size, ctb_width, ctb_height, ctb_count;
    int min_cb_width, min_cb_height;   /* in min-CB units */
    int min_tb_width, min_tb_height;   /* in min-TB (4x4) units */
    int pic_width_c, pic_height_c;
    /* VUI (only what we use) */
    int vui_timing;
    uint32_t num_units_in_tick, time_scale;
    /* scaling lists: [sizeId][matrixId][coef] plus the DC term for 16/32 */
    uint8_t  sl[4][6][64];
    uint8_t  sl_dc[2][6];
    int      sl_present;
} sps_t;

/* -------------------------------------------------------------- PPS ------ */
typedef struct {
    int present;
    int pps_id, sps_id;
    int dependent_slice_segments_enabled;
    int output_flag_present;
    int num_extra_slice_header_bits;
    int sign_data_hiding;
    int cabac_init_present;
    int num_ref_idx_l0_default, num_ref_idx_l1_default;
    int init_qp;
    int constrained_intra_pred;
    int transform_skip_enabled;
    int cu_qp_delta_enabled, diff_cu_qp_delta_depth;
    int cb_qp_offset, cr_qp_offset;
    int slice_chroma_qp_offsets_present;
    int weighted_pred, weighted_bipred;
    int transquant_bypass_enabled;
    int tiles_enabled, entropy_coding_sync_enabled;
    int num_tile_columns, num_tile_rows, uniform_spacing;
    int column_width[H265_MAX_TILES], row_height[H265_MAX_TILES];
    int loop_filter_across_tiles;
    int loop_filter_across_slices;
    int deblocking_filter_control_present;
    int deblocking_filter_override_enabled;
    int pps_deblocking_filter_disabled;
    int beta_offset, tc_offset;        /* already doubled */
    int pps_scaling_list_data_present;
    int lists_modification_present;
    int log2_parallel_merge_level;
    int slice_segment_header_extension_present;
    uint8_t sl[4][6][64];
    uint8_t sl_dc[2][6];
    int sl_present;
} pps_t;

/* ------------------------------------------------------------- slice ----- */
typedef struct {
    int first_slice_in_pic;
    int no_output_of_prior_pics;
    int pps_id;
    int dependent_slice_segment;
    int segment_address;
    int type;                          /* SLICE_B / SLICE_P / SLICE_I */
    int pic_output_flag;
    int poc_lsb;
    int short_term_ref_pic_set_sps_flag;
    strps_t rps;                       /* the short-term set in force */
    int num_long_term;                 /* total lt entries */
    int lt_poc[32];                    /* PocLsbLt of each lt entry */
    int lt_msb_cycle[32];              /* DeltaPocMsbCycleLt */
    int lt_used[32];
    int lt_msb_present[32];
    int temporal_mvp_enabled;
    int sao_luma, sao_chroma;
    int num_ref_idx[2];
    int ref_list_modification_flag[2];
    int list_entry[2][H265_MAX_REFS];
    int mvd_l1_zero;
    int cabac_init_flag;
    int collocated_from_l0, collocated_ref_idx;
    int five_minus_max_num_merge_cand;
    int qp_delta;
    int cb_qp_offset, cr_qp_offset;
    int deblocking_filter_disabled;
    int beta_offset, tc_offset;
    int loop_filter_across_slices;
    int num_entry_points;
    uint32_t entry_point_offset[H265_MAX_SUBSTREAMS];
    /* weighted prediction */
    int luma_log2_weight_denom, chroma_log2_weight_denom;
    int luma_weight[2][H265_MAX_REFS], luma_offset[2][H265_MAX_REFS];
    int chroma_weight[2][H265_MAX_REFS][2], chroma_offset[2][H265_MAX_REFS][2];
    /* derived */
    int qp;                            /* SliceQpY */
    int max_merge_cand;
    int header_bits;                   /* size of the header, for entry points */
} slice_t;

/* ------------------------------------------------------- decoded picture -- */
/* The collocated motion field, stored at the 16x16 granularity the spec's
 * motion-vector compression prescribes (8.5.3.2.8 fetches the candidate at
 * ((x >> 4) << 4)), and holding the referenced picture's POC rather than its
 * ref_idx -- the collocated picture's reference lists are gone by the time we
 * read it, and DiffPicOrderCnt(colPic, refPicListCol[refIdxCol]) is what the
 * scaling actually needs. */
typedef struct {
    int16_t mv[2][2];      /* [list][x/y] */
    int8_t  refpoc_valid[2];
    int32_t refpoc[2];
    uint8_t islt[2];       /* referenced picture was long-term */
    uint8_t intra;
} colmv_t;

typedef struct {
    uint16_t *y, *u, *v;
    uint16_t *base;
    int stride_y, stride_c;        /* in SAMPLES */
    int used;
    int reference;         /* 0 none, 1 short-term, 2 long-term */
    int output;            /* still needs to be output */
    int poc;
    int output_flag;
    colmv_t *col;          /* (ctb-aligned width/16) x (height/16) */
    int col_w, col_h;
} pic_t;

/* ------------------------------------------------ per-4x4 decode state ---- */
/* Everything the neighbour derivations and the loop filters read back, at the
 * 4x4 minimum-transform-block granularity, for the picture being decoded. */
typedef struct {
    uint8_t  pred_mode;    /* MODE_INTRA / MODE_INTER (skip folded into INTER) */
    uint8_t  skip;         /* cu_skip_flag, for its own context derivation */
    uint8_t  intra_mode;   /* IntraPredModeY, 0..34 */
    uint8_t  ct_depth;     /* CtDepth, for split_cu_flag's context */
    uint8_t  pred_flag;    /* bit0 = L0 used, bit1 = L1 used */
    uint8_t  bypass;       /* cu_transquant_bypass_flag */
    uint8_t  pcm;          /* pcm_flag */
    uint8_t  cbf_luma;     /* the covering transform block has luma coeffs */
    uint8_t  bnd;          /* bit0: left edge is a TU or PU boundary, bit1: top
                            * edge is; bit2/bit3: that edge is a TRANSFORM
                            * block edge specifically, which is the only case
                            * where a non-zero cbf raises bS to 1 */
    int8_t   qp;           /* QpY of the covering quantisation group */
    int8_t   ref_idx[2];
    int16_t  mv[2][2];
    int32_t  refpoc[2];    /* POC of the referenced PICTURE (deblock bS asks
                            * about pictures, not list positions) */
    uint8_t  islt[2];
    uint16_t slice_idx;    /* slice this block belongs to (for availability) */
    uint16_t tile_idx;
} bi_t;

/* Per-CTB SAO parameters (7.3.8.3). */
typedef struct {
    uint8_t type[3];       /* SAO_NOT_APPLIED / SAO_BAND / SAO_EDGE */
    uint8_t clsx[3];       /* edge class 0..3, or band position 0..31 */
    int8_t  off[3][4];
} sao_t;

/* ------------------------------------------------------------- CABAC ----- */
#define H265_NCTX 155

typedef struct {
    const uint8_t *buf;
    int len;
    int bytepos;
    int bitpos;            /* 0..7 inside buf[bytepos] */
    int overrun;           /* bits consumed past the end of the substream */
    uint32_t range;
    uint32_t offset;
    int error;
    uint8_t state[H265_NCTX];   /* (pStateIdx << 1) | valMps */
} cabac_t;

/* Context base offsets. One flat array; every element's contexts are
 * contiguous so ctxInc is just an add. */
enum {
    CTX_SAO_MERGE = 0,             /* 1 */
    CTX_SAO_TYPE_IDX = 1,          /* 1 */
    CTX_SPLIT_CU = 2,              /* 3 */
    CTX_CU_TRANSQUANT_BYPASS = 5,  /* 1 */
    CTX_CU_SKIP = 6,               /* 3 */
    CTX_PRED_MODE = 9,             /* 1 */
    CTX_PART_MODE = 10,            /* 4 */
    CTX_PREV_INTRA_LUMA = 14,      /* 1 */
    CTX_INTRA_CHROMA = 15,         /* 1 */
    CTX_RQT_ROOT_CBF = 16,         /* 1 */
    CTX_MERGE_FLAG = 17,           /* 1 */
    CTX_MERGE_IDX = 18,            /* 1 */
    CTX_INTER_PRED_IDC = 19,       /* 5 */
    CTX_REF_IDX = 24,              /* 2 */
    CTX_MVP_FLAG = 26,             /* 1 */
    CTX_MVD_GREATER0 = 27,         /* 1 */
    CTX_MVD_GREATER1 = 28,         /* 1 */
    CTX_SPLIT_TRANSFORM = 29,      /* 3 */
    CTX_CBF_LUMA = 32,             /* 2 */
    CTX_CBF_CHROMA = 34,           /* 4 */
    CTX_TRANSFORM_SKIP = 38,       /* 2: luma, chroma */
    CTX_CU_QP_DELTA = 40,          /* 3 */
    CTX_LAST_X = 43,               /* 18 */
    CTX_LAST_Y = 61,               /* 18 */
    CTX_SUBBLOCK = 79,             /* 4 */
    CTX_SIG = 83,                  /* 42 */
    CTX_GREATER1 = 125,            /* 24 */
    CTX_GREATER2 = 149             /* 6 -> 155 total */
};

void  h265_cabac_init_ctx(cabac_t *c, int init_type, int qp);
int   h265_cabac_start(cabac_t *c, const uint8_t *buf, int len, int bytepos);
int   h265_cabac_decision(cabac_t *c, int ctx);
int   h265_cabac_bypass(cabac_t *c);
uint32_t h265_cabac_bypass_n(cabac_t *c, int n);
int   h265_cabac_terminate(cabac_t *c);
/* Byte position just after the terminating bin, per 9.3.2.5 -- where the next
 * substream or the slice's rbsp trailing bits begin. */
int   h265_cabac_pos(const cabac_t *c);

/* ------------------------------------------------------------ h265_pred -- */
/* Inverse transform + dequant of one nTbS x nTbS block, adding into dst.
 * `coeff` is in raster order inside the block. trType 1 selects the 4x4 DST. */
/* `bd` is the component's BitDepth (8 or 10). Every one of these is bit-depth
 * dependent in the spec and takes it explicitly rather than reading a global,
 * so tests/unit/h265_pred_test.c can drive the SAME function at both depths. */
void h265_dequant(int16_t *coeff, int n, int qp, int log2_size,
                  const uint8_t *scaling, int dc_scale, int bd);
void h265_itransform_add(const int16_t *coeff, int log2_size, int tr_type,
                         uint16_t *dst, int stride, int bd);
void h265_transform_skip_add(const int16_t *coeff, int log2_size,
                             uint16_t *dst, int stride, int bd);
void h265_bypass_add(const int16_t *coeff, int log2_size,
                     uint16_t *dst, int stride, int bd);
/* Intra prediction into dst. `nb` holds the 4*nTbS+1 reference samples in the
 * spec's layout: nb[0] = p[-1][2*nTbS-1] .. nb[2*nTbS-1] = p[-1][0],
 * nb[2*nTbS] = p[-1][-1], nb[2*nTbS+1] = p[0][-1] .. nb[4*nTbS] =
 * p[2*nTbS-1][-1]. h265_intra_filter() applies 8.4.4.2.3 in place. */
void h265_intra_filter(uint16_t *nb, int nbs, int mode, int c_idx,
                       int strong_smoothing, int bd);
void h265_intra_pred(uint16_t *dst, int stride, const uint16_t *nb,
                     int nbs, int mode, int c_idx, int bd);

/* -------------------------------------------------------------- h265_mc -- */
/* One WxH block of inter prediction at 14-bit intermediate precision. `ref`
 * points at the visible (0,0) sample of the reference plane, which carries
 * H265_PAD replicated border samples on every side. mv is in quarter-pel
 * (luma) units; chroma derives its eighth-pel phase from the same vector. */
void h265_mc_luma(int16_t *dst, int dst_stride,
                  const uint16_t *ref, int ref_stride, int pw, int ph,
                  int x, int y, int w, int h, int mvx, int mvy, uint16_t *emu,
                  int bd);
void h265_mc_chroma(int16_t *dst, int dst_stride,
                    const uint16_t *ref, int ref_stride, int pw, int ph,
                    int x, int y, int w, int h, int mvx, int mvy, uint16_t *emu,
                    int bd);
/* 8.5.3.3.4 weighted sample prediction. The `offset` arguments are the values
 * as CODED in the slice header; scaling them by WpOffsetBdShift is these
 * functions' job, not the caller's. */
void h265_pred_uni(uint16_t *dst, int dst_stride, const int16_t *src, int src_stride,
                   int w, int h, int bd);
void h265_pred_bi(uint16_t *dst, int dst_stride, const int16_t *s0, int s0_stride,
                  const int16_t *s1, int s1_stride, int w, int h, int bd);
void h265_pred_uni_w(uint16_t *dst, int dst_stride, const int16_t *src, int src_stride,
                     int w, int h, int denom, int weight, int offset, int bd);
void h265_pred_bi_w(uint16_t *dst, int dst_stride, const int16_t *s0, int s0_stride,
                    const int16_t *s1, int s1_stride, int w, int h,
                    int denom, int w0, int o0, int w1, int o1, int bd);

/* --------------------------------------------------------- h265_deblock -- */
struct h265dec;
void h265_deblock_pic(struct h265dec *d);
void h265_sao_pic(struct h265dec *d);

/* ------------------------------------------------------------- h265_nal -- */
uint8_t *h265_nal_to_rbsp(const uint8_t *nal, int len, int *rbsp_len,
                          int *epb_pos, int max_epb, int *n_epb);
int h265_residual_coding(struct h265dec *d, int log2_size, int c_idx,
                         int pred_mode_intra, int is_intra, int bypass,
                         int16_t *out, int *ts);
int h265_parse_vps(struct h265dec *d, bs_t *bs);
int h265_parse_sps(struct h265dec *d, bs_t *bs);
int h265_parse_pps(struct h265dec *d, bs_t *bs);
int h265_parse_slice_header(struct h265dec *d, bs_t *bs, int nal_type,
                            int nuh_temporal_id, slice_t *sl);

/* ------------------------------------------------------------- decoder --- */
typedef struct h265dec {
    vps_t vps[16];
    sps_t sps[H265_MAX_SPS];
    pps_t pps[H265_MAX_PPS];
    sps_t *cur_sps;
    pps_t *cur_pps;

    pic_t pics[H265_MAX_DPB];
    pic_t *cur;
    int width, height;               /* cropped, visible */
    int stride_y, stride_c;
    int pic_w, pic_h;                /* CTB-aligned coded size */

    bi_t *bi;                        /* per-4x4 decode state, current picture */
    int   bw4, bh4;                  /* its dimensions */
    int  *zscan;                     /* MinTbAddrZs (6.5.2), per 4x4 */
    int   zw, zh;                    /* the zscan grid, CTB-aligned */
    int   out_queue[H265_MAX_DPB];   /* slots decided for output, POC order */
    int   n_outq;
    int   to_free;                   /* slot released at the next entry point */
    int   cur_nal_type;
    int   max_poc_lsb;
    sao_t *sao;                      /* per CTB */
    uint8_t *ctb_deblock_disabled;   /* per CTB: slice_deblocking_filter_disabled */
    int8_t  *ctb_beta, *ctb_tc;      /* per CTB slice deblock offsets */
    uint8_t *ctb_filt_across_slice;  /* per CTB: loop_filter_across_slices */

    /* tile geometry (derived from the active PPS) */
    int col_bd[H265_MAX_TILES + 1], row_bd[H265_MAX_TILES + 1];
    int *ctb_ts_to_rs, *ctb_rs_to_ts, *ctb_tile_id;
    int geom_valid;

    /* current slice */
    slice_t sh;
    int slice_idx;                   /* index of the current slice in the pic */
    pic_t *ref_list[2][H265_MAX_REFS];
    int    ref_poc_list[2][H265_MAX_REFS];
    uint8_t ref_islt[2][H265_MAX_REFS];
    int    nb_refs[2];
    pic_t *col_pic;

    /* reference picture set for the current picture */
    int poc;
    int poc_tid0;
    int prev_poc_lsb, prev_poc_msb;
    int first_picture;               /* nothing decoded yet / after EOS */
    int max_lt_poc_lsb_present;

    cabac_t cabac;
    cabac_t cabac_wpp_save;          /* contexts saved after CTU 1 of a row */
    int wpp_saved;

    /* CTU decode cursor */
    int ctb_addr_ts, ctb_addr_rs;
    int slice_addr_rs;               /* first CTB (raster) of the current SLICE */
    int seg_addr_rs;                 /* ... of the current slice SEGMENT */
    int seg_addr_ts;                 /* ... in tile scan order */
    int cu_merge_2nx2n;              /* the CU is one 2Nx2N merge partition,
                                      * which is exactly when rqt_root_cbf is
                                      * absent (7.3.8.5) */
    int qp_y, qp_y_prev, qp_y_last;
    int qg_x, qg_y;                  /* current quantisation group origin */
    int cu_qp_delta_coded, cu_qp_delta;
    int ctu_x, ctu_y;                /* current CTB origin in luma samples */
    int cur_tile;

    /* per-CU scratch */
    int cu_x, cu_y, cu_log2;
    int cu_pred_mode, cu_part_mode, cu_bypass, cu_skip;
    int intra_split;
    int intra_mode_y[4];
    int intra_mode_c;
    int max_trafo_depth;
    int pcm_flag;
    uint8_t cbf_cb[8], cbf_cr[8];    /* per trafoDepth */
    bs_t pcm_bs;                     /* PCM samples are read straight from RBSP */

    int eos;
    int err;
    /* Edge-emulation scratch: a 64x64 PU plus the 8-tap filter's support. */
    uint16_t emu[(64 + 8) * (64 + 8)];
    int16_t tmp0[64 * 64], tmp1[64 * 64];
    int16_t coeff[32 * 32];
    uint16_t *sao_src;               /* deblocked copy SAO reads from */
    /* The 8-bit display shadow handed to callers through h265frame.y/u/v.
     * One buffer, rebuilt per output picture; see build_display(). */
    uint8_t *disp;
    long     disp_sz;
} h265dec;

/* Shared small helpers. */
static inline int h265_clip3(int lo, int hi, int v)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline int h265_clip_u8(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}
/* Clip1Y / Clip1C of 8.7 and friends: the upper bound is (1 << BitDepth) - 1,
 * NOT 255. Every reconstruction path funnels through this one function on
 * purpose -- a clamp that is right at 8 bits and left at 255 for 10 is
 * invisible on ordinary content and wrong on bright content, which is exactly
 * the failure this decoder is not allowed to have. */
static inline int h265_clip_pix(int v, int bd)
{
    int hi = (1 << bd) - 1;
    return v < 0 ? 0 : (v > hi ? hi : v);
}

/* Per-4x4 state accessor. Callers guarantee the coordinates are inside the
 * CTB-aligned picture. */
static inline bi_t *h265_bi(h265dec *d, int x, int y)
{
    return &d->bi[(y >> 2) * d->bw4 + (x >> 2)];
}

#endif /* LOGIT_H265_INT_H */
