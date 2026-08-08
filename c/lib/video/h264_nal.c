/* c/lib/video/h264_nal.c -- NAL unit, SPS/PPS, and slice header parsing.
 *
 * Every value read here is untrusted network/hostile input: bounds are
 * checked through bs_t (sticky error), and every semantic limit the spec
 * imposes is validated before the value is stored. Anything outside the
 * supported subset is H264_ERR_UNSUPPORTED, anything malformed is
 * H264_ERR_CORRUPT.
 *
 * Supported: Baseline (66), Main (77), Extended (88), High (100) and the
 * High-profile aliases that stay 8-bit 4:2:0 in practice, progressive frames
 * only. High is the one that matters -- essentially every video on the open
 * web is profile_idc 100 -- and what it adds over the older two is parsed
 * here: chroma_format_idc, the bit depths, the 8x8 transform flag and the
 * scaling matrices, including Table 7-2's fall-back rules, which are the part
 * that is easy to get subtly wrong because a list that is not coded is not
 * necessarily the default one.
 */
#include <stdlib.h>
#include <string.h>
#include "h264.h"
#include "h264_int.h"

/* Strip emulation-prevention bytes (00 00 03 -> 00 00) from a NAL payload
 * into a fresh RBSP buffer. `len` includes the NAL header byte. Returns
 * malloc'd RBSP and its length, or NULL on allocation failure. */
uint8_t *h264_nal_to_rbsp(const uint8_t *nal, int len, int *rbsp_len)
{
    uint8_t *out = (uint8_t *)malloc((size_t)len);
    if (!out) return NULL;
    int o = 0, zeros = 0;
    for (int i = 1; i < len; i++) {          /* skip the NAL header byte */
        uint8_t b = nal[i];
        if (zeros == 2 && b == 3) { zeros = 0; continue; }   /* emulation byte */
        zeros = (b == 0) ? zeros + 1 : 0;
        out[o++] = b;
    }
    *rbsp_len = o;
    return out;
}

/* --------------------------------------------------------- scaling lists -- */
/* zigzag scan position -> raster index, 4x4 (8.5.6) and 8x8 (8.5.7). The
 * coded scaling list is in scan order; weightScale is raster. */
static const uint8_t zz4[16] = {
    0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
};
static const uint8_t zz8[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

/* Table 7-3 / 7-4, already in RASTER order (both matrices are symmetric, so
 * the distinction is invisible here -- it is not for a coded list). */
static const uint8_t default_4x4[2][16] = {
    {  6, 13, 20, 28, 13, 20, 28, 32, 20, 28, 32, 37, 28, 32, 37, 42 },
    { 10, 14, 20, 24, 14, 20, 24, 27, 20, 24, 27, 30, 24, 27, 30, 34 }
};
static const uint8_t default_8x8[2][64] = {
    {  6, 10, 13, 16, 18, 23, 25, 27, 10, 11, 16, 18, 23, 25, 27, 29,
      13, 16, 18, 23, 25, 27, 29, 31, 16, 18, 23, 25, 27, 29, 31, 33,
      18, 23, 25, 27, 29, 31, 33, 36, 23, 25, 27, 29, 31, 33, 36, 38,
      25, 27, 29, 31, 33, 36, 38, 40, 27, 29, 31, 33, 36, 38, 40, 42 },
    {  9, 13, 15, 17, 19, 21, 22, 24, 13, 13, 17, 19, 21, 22, 24, 25,
      15, 17, 19, 21, 22, 24, 25, 27, 17, 19, 21, 22, 24, 25, 27, 28,
      19, 21, 22, 24, 25, 27, 28, 30, 21, 22, 24, 25, 27, 28, 30, 32,
      22, 24, 25, 27, 28, 30, 32, 33, 24, 25, 27, 28, 30, 32, 33, 35 }
};
static const uint8_t flat_16[64] = {
    16,16,16,16,16,16,16,16, 16,16,16,16,16,16,16,16,
    16,16,16,16,16,16,16,16, 16,16,16,16,16,16,16,16,
    16,16,16,16,16,16,16,16, 16,16,16,16,16,16,16,16,
    16,16,16,16,16,16,16,16, 16,16,16,16,16,16,16,16
};

/* One scaling_list( ) (7.3.2.1.1.1). Writes `out` in raster order.
 *
 * useDefault is signalled OUT OF BAND, by delta_scale driving nextScale to 0
 * at the very first coefficient -- and when that happens the REST of the list
 * is not coded at all, so the loop must stop, not merely remember a flag.
 * Returns 1 if the list was "use default", 0 if it was coded. */
static int parse_scaling_list(bs_t *bs, uint8_t *out, int size,
                              const uint8_t *dflt, const uint8_t *scan)
{
    int last = 8, next = 8, use_default = 0;
    for (int j = 0; j < size; j++) {
        if (next != 0) {
            int delta = (int)bs_se(bs);
            next = (last + delta + 256) % 256;
            use_default = (j == 0 && next == 0);
        }
        if (use_default) break;
        last = (next == 0) ? last : next;
        out[scan[j]] = (uint8_t)last;
    }
    if (use_default) memcpy(out, dflt, (size_t)size);
    return use_default;
}

/* The 6 (or 8) lists of a scaling matrix, with Table 7-2's fall-back rules.
 *
 * `fb4`/`fb8` are the fall-back for list 0 and list 6/7: NULL selects rule A
 * (the spec defaults), non-NULL selects rule B (the SPS's own matrices, used
 * when a PPS overrides an SPS that already carried one). Every OTHER list
 * falls back to the list decoded just before it, which is why this cannot be
 * done list-by-list in isolation. */
static void parse_scaling_matrix(bs_t *bs, uint8_t s4[6][16], uint8_t s8[2][64],
                                 const uint8_t (*fb4)[16], const uint8_t (*fb8)[64],
                                 int n8)
{
    for (int i = 0; i < 6; i++) {
        if (bs_u1(bs)) {
            parse_scaling_list(bs, s4[i], 16, default_4x4[i < 3 ? 0 : 1], zz4);
        } else if (i == 0 || i == 3) {
            if (fb4) memcpy(s4[i], fb4[i], 16);                 /* rule B */
            else     memcpy(s4[i], default_4x4[i == 0 ? 0 : 1], 16);  /* rule A */
        } else {
            memcpy(s4[i], s4[i - 1], 16);
        }
    }
    for (int i = 0; i < n8; i++) {
        if (bs_u1(bs)) {
            parse_scaling_list(bs, s8[i], 64, default_8x8[i & 1], zz8);
        } else if (i < 2) {
            if (fb8) memcpy(s8[i], fb8[i], 64);
            else     memcpy(s8[i], default_8x8[i], 64);
        } else {
            memcpy(s8[i], s8[i - 2], 64);
        }
    }
}

/* ---------------------------------------------------------- LevelScale --- */
/* normAdjust4x4 (Table 8-15) and normAdjust8x8 (Table 8-14), by qP%6 and the
 * position class. */
static const int16_t v4[6][3] = {
    { 10, 16, 13 }, { 11, 18, 14 }, { 13, 20, 16 },
    { 14, 23, 18 }, { 16, 25, 20 }, { 18, 29, 23 }
};
static const int16_t v8[6][6] = {
    { 20, 18, 32, 19, 25, 24 }, { 22, 19, 35, 21, 28, 26 },
    { 26, 23, 42, 24, 33, 31 }, { 28, 25, 45, 26, 35, 33 },
    { 32, 28, 51, 30, 40, 38 }, { 36, 32, 58, 34, 46, 43 }
};

static int class4(int i, int j)
{
    if ((i & 1) == 0 && (j & 1) == 0) return 0;
    if ((i & 1) && (j & 1)) return 1;
    return 2;
}

static int class8(int i, int j)
{
    if (i % 4 == 0 && j % 4 == 0) return 0;
    if (i % 2 == 1 && j % 2 == 1) return 1;
    if (i % 4 == 2 && j % 4 == 2) return 2;
    if ((i % 4 == 0 && j % 2 == 1) || (i % 2 == 1 && j % 4 == 0)) return 3;
    if ((i % 4 == 0 && j % 4 == 2) || (i % 4 == 2 && j % 4 == 0)) return 4;
    return 5;
}

/* LevelScale4x4/8x8 (8.5.9) = weightScale * normAdjust, folded once per PPS.
 * Every transform in the decoder then takes a plain integer row and knows
 * nothing about scaling matrices; with the flat default lists these come out
 * as 16 * v, which is exactly the arithmetic the CAVLC-only decoder used to
 * hardcode. */
static void build_levelscale(pps_t *p)
{
    for (int l = 0; l < 6; l++)
        for (int m = 0; m < 6; m++)
            for (int r = 0; r < 16; r++)
                p->ls4[l][m][r] = p->scaling4[l][r] * v4[m][class4(r >> 2, r & 3)];
    for (int l = 0; l < 2; l++)
        for (int m = 0; m < 6; m++)
            for (int r = 0; r < 64; r++)
                p->ls8[l][m][r] = p->scaling8[l][r] * v8[m][class8(r >> 3, r & 7)];
}

/* ------------------------------------------------------------- VUI ------ */
static void parse_hrd(bs_t *bs)
{
    uint32_t cpb_cnt = bs_ue(bs) + 1;
    if (bs_error(bs) || cpb_cnt > 32) { bs->error = 1; return; }
    bs_u(bs, 8);                              /* bit_rate_scale, cpb_size_scale */
    for (uint32_t i = 0; i < cpb_cnt; i++) {
        bs_ue(bs); bs_ue(bs); bs_u1(bs);      /* bit_rate, cpb_size, cbr */
    }
    bs_u(bs, 5 + 5 + 5 + 5);                  /* cpb/dpb delays, time, bitrate */
}

static int parse_vui(sps_t *s, bs_t *bs)
{
    int nal_hrd = 0, vcl_hrd = 0;
    if (bs_u1(bs)) {                          /* aspect_ratio_info_present */
        uint32_t ar_idc = bs_u(bs, 8);
        if (ar_idc == 255) bs_u(bs, 32);      /* sar_width, sar_height */
    }
    if (bs_u1(bs)) bs_u1(bs);                 /* overscan */
    if (bs_u1(bs)) {                          /* video_signal_type */
        bs_u(bs, 3 + 1);
        if (bs_u1(bs)) bs_u(bs, 24);          /* colour_description */
    }
    if (bs_u1(bs)) { bs_ue(bs); bs_ue(bs); }  /* chroma_loc_info */
    if (bs_u1(bs)) {                          /* timing_info_present */
        s->num_units_in_tick = bs_u(bs, 32);
        s->time_scale = bs_u(bs, 32);
        s->vui_fixed_rate = (int)bs_u1(bs);
        s->vui_timing = s->num_units_in_tick && s->time_scale;
    }
    if ((nal_hrd = (int)bs_u1(bs))) parse_hrd(bs);
    if ((vcl_hrd = (int)bs_u1(bs))) parse_hrd(bs);
    if (nal_hrd || vcl_hrd) bs_u1(bs);        /* low_delay_hrd_flag */
    if (bs_error(bs)) return H264_ERR_CORRUPT;
    bs_u1(bs);                                /* pic_struct_present_flag */
    if (bs_u1(bs)) {                          /* bitstream_restriction */
        bs_u1(bs);                            /* motion_vectors_over_pic_bdry */
        bs_ue(bs); bs_ue(bs); bs_ue(bs); bs_ue(bs);
        /* max_num_reorder_frames is how many pictures the DPB may hold back
         * before output. It is the whole of the B-frame output-order question:
         * get it wrong and every frame is still decoded correctly and every
         * frame comes out in the wrong place. */
        s->num_reorder_frames = (int)bs_ue(bs);
        s->max_dec_frame_buffering = (int)bs_ue(bs);
        if (s->num_reorder_frames > 16 || s->max_dec_frame_buffering > 16)
            return H264_ERR_CORRUPT;
        s->has_bitstream_restriction = 1;
    }
    return bs_error(bs) ? H264_ERR_CORRUPT : H264_OK;
}

/* ------------------------------------------------------------- SPS ------ */
/* Which profiles carry the High-profile extension block (chroma_format_idc
 * onwards, 7.3.2.1.1). The list is normative and longer than the three
 * profiles anybody encodes for: a stream that says 122 and is 8-bit 4:2:0 is
 * rejected below on chroma_format_idc / bit depth, not here. */
static int has_high_extension(int p)
{
    return p == 100 || p == 110 || p == 122 || p == 244 || p == 44 ||
           p == 83 || p == 86 || p == 118 || p == 128 || p == 138 ||
           p == 139 || p == 134 || p == 135;
}

int h264_parse_sps(h264dec *d, bs_t *bs)
{
    sps_t s;
    memset(&s, 0, sizeof s);
    s.profile_idc = (int)bs_u(bs, 8);
    bs_u(bs, 8);                              /* constraint flags */
    s.level_idc = (int)bs_u(bs, 8);
    s.sps_id = (int)bs_ue(bs);
    if (bs_error(bs)) return H264_ERR_CORRUPT;
    if (s.sps_id > 31) return H264_ERR_CORRUPT;

    /* Defaults for the profiles that do not carry the extension block, and
     * the starting point for the ones that do: no scaling matrix means flat
     * 16s, which is the identity for LevelScale. */
    s.chroma_format_idc = 1;
    s.bit_depth_luma = s.bit_depth_chroma = 8;
    for (int i = 0; i < 6; i++) memcpy(s.scaling4[i], flat_16, 16);
    for (int i = 0; i < 2; i++) memcpy(s.scaling8[i], flat_16, 64);

    if (has_high_extension(s.profile_idc)) {
        s.chroma_format_idc = (int)bs_ue(bs);
        if (s.chroma_format_idc == 3) bs_u1(bs);   /* separate_colour_plane */
        s.bit_depth_luma = (int)bs_ue(bs) + 8;
        s.bit_depth_chroma = (int)bs_ue(bs) + 8;
        s.qpprime_y_zero_transform_bypass = (int)bs_u1(bs);
        if (bs_error(bs)) return H264_ERR_CORRUPT;
        /* 4:0:0 monochrome and 4:2:2/4:4:4 need a different residual layout,
         * a different chroma MC and a different deblocker; >8 bits changes
         * every clip and every transform bound. None of that is here, and
         * none of it appears in web video -- refuse rather than approximate. */
        if (s.chroma_format_idc != 1) return H264_ERR_UNSUPPORTED;
        if (s.bit_depth_luma != 8 || s.bit_depth_chroma != 8)
            return H264_ERR_UNSUPPORTED;
        /* Lossless macroblocks bypass the transform entirely (8.5.15). x264
         * only emits this at --qp 0. */
        if (s.qpprime_y_zero_transform_bypass) return H264_ERR_UNSUPPORTED;
        if (bs_u1(bs)) {                       /* seq_scaling_matrix_present */
            s.scaling_present = 1;
            parse_scaling_matrix(bs, s.scaling4, s.scaling8, NULL, NULL, 2);
            if (bs_error(bs)) return H264_ERR_CORRUPT;
        }
    } else if (s.profile_idc != 66 && s.profile_idc != 77 &&
               s.profile_idc != 88) {
        return H264_ERR_UNSUPPORTED;
    }

    s.log2_max_frame_num = (int)bs_ue(bs) + 4;
    s.poc_type = (int)bs_ue(bs);
    if (s.log2_max_frame_num > 16) return H264_ERR_CORRUPT;
    if (s.poc_type == 0) {
        s.log2_max_poc_lsb = (int)bs_ue(bs) + 4;
        if (s.log2_max_poc_lsb > 16) return H264_ERR_CORRUPT;
    } else if (s.poc_type == 1) {
        s.delta_pic_order_always_zero = (int)bs_u1(bs);
        s.offset_for_non_ref_pic = (int)bs_se(bs);
        s.offset_for_top_to_bottom = (int)bs_se(bs);
        s.num_ref_frames_in_poc_cycle = (int)bs_ue(bs);
        if (s.num_ref_frames_in_poc_cycle != 0)
            return H264_ERR_UNSUPPORTED;      /* cyclic offsets: never seen */
    } else if (s.poc_type != 2) {
        return H264_ERR_CORRUPT;
    }

    s.max_num_ref_frames = (int)bs_ue(bs);
    if (s.max_num_ref_frames > 16) return H264_ERR_CORRUPT;
    s.gaps_in_frame_num_allowed = (int)bs_u1(bs);
    s.mb_width = (int)bs_ue(bs) + 1;
    s.mb_height = (int)bs_ue(bs) + 1;
    if (s.mb_width > 1024 || s.mb_height > 1024) return H264_ERR_CORRUPT;
    s.frame_mbs_only = (int)bs_u1(bs);
    if (!s.frame_mbs_only) return H264_ERR_UNSUPPORTED;   /* fields/MBAFF */
    s.direct_8x8_inference = (int)bs_u1(bs);
    s.crop_flag = (int)bs_u1(bs);
    if (s.crop_flag) {
        for (int i = 0; i < 4; i++) s.crop[i] = (int)bs_ue(bs);
    }
    if (bs_u1(bs)) {                          /* vui_parameters_present */
        int rc = parse_vui(&s, bs);
        if (rc) return rc;
    }
    if (bs_error(bs)) return H264_ERR_CORRUPT;
    s.present = 1;
    d->sps[s.sps_id] = s;
    return H264_OK;
}

/* ------------------------------------------------------------- PPS ------ */
int h264_parse_pps(h264dec *d, bs_t *bs)
{
    pps_t p;
    memset(&p, 0, sizeof p);
    p.pps_id = (int)bs_ue(bs);
    p.sps_id = (int)bs_ue(bs);
    if (bs_error(bs) || p.pps_id > 31 || p.sps_id > 31) return H264_ERR_CORRUPT;
    if (!d->sps[p.sps_id].present) return H264_ERR_CORRUPT;
    const sps_t *sps = &d->sps[p.sps_id];

    p.entropy_cabac = (int)bs_u1(bs);
    p.bottom_field_poc_in_frame = (int)bs_u1(bs);
    p.num_slice_groups = (int)bs_ue(bs) + 1;
    if (p.num_slice_groups != 1) return H264_ERR_UNSUPPORTED;   /* FMO */
    p.num_ref_idx_l0_default = (int)bs_ue(bs) + 1;
    p.num_ref_idx_l1_default = (int)bs_ue(bs) + 1;
    if (p.num_ref_idx_l0_default > 16 || p.num_ref_idx_l1_default > 16)
        return H264_ERR_CORRUPT;
    p.weighted_pred = (int)bs_u1(bs);
    p.weighted_bipred_idc = (int)bs_u(bs, 2);
    if (p.weighted_bipred_idc == 3) return H264_ERR_CORRUPT;
    p.pic_init_qp = (int)bs_se(bs);
    p.pic_init_qs = (int)bs_se(bs);
    p.chroma_qp_index_offset = (int)bs_se(bs);
    if (p.pic_init_qp < -26 || p.pic_init_qp > 25) return H264_ERR_CORRUPT;
    if (p.chroma_qp_index_offset < -12 || p.chroma_qp_index_offset > 12)
        return H264_ERR_CORRUPT;
    p.deblock_control_present = (int)bs_u1(bs);
    p.constrained_intra_pred = (int)bs_u1(bs);
    p.redundant_pic_cnt_present = (int)bs_u1(bs);
    if (p.redundant_pic_cnt_present) return H264_ERR_UNSUPPORTED;

    /* The PPS starts from the SPS's matrices; the optional tail may then
     * replace some of them. */
    memcpy(p.scaling4, sps->scaling4, sizeof p.scaling4);
    memcpy(p.scaling8, sps->scaling8, sizeof p.scaling8);

    /* Optional tail (more_rbsp_data): the High-profile fields. */
    if (bs_more_rbsp_data(bs)) {
        p.transform_8x8 = (int)bs_u1(bs);
        if (bs_u1(bs)) {                       /* pic_scaling_matrix_present */
            /* Fall-back rule B when the SPS carried a matrix, rule A when it
             * did not (Table 7-2). Rule A defaults to the spec's tables; rule
             * B defaults to whatever the SPS decided -- getting these two the
             * wrong way round changes the dequantiser for every coefficient of
             * every macroblock, silently. */
            parse_scaling_matrix(bs, p.scaling4, p.scaling8,
                                 sps->scaling_present ? sps->scaling4 : NULL,
                                 sps->scaling_present ? sps->scaling8 : NULL,
                                 p.transform_8x8 ? 2 : 0);
            if (bs_error(bs)) return H264_ERR_CORRUPT;
        }
        p.second_chroma_qp_offset = (int)bs_se(bs);
        if (p.second_chroma_qp_offset < -12 || p.second_chroma_qp_offset > 12)
            return H264_ERR_CORRUPT;
        p.has_second_chroma_offset = 1;
    }
    if (bs_error(bs)) return H264_ERR_CORRUPT;
    build_levelscale(&p);
    p.present = 1;
    d->pps[p.pps_id] = p;
    return H264_OK;
}

/* ------------------------------------------------------- slice header --- */
/* ref_pic_list_modification (7.3.3.1) for one list. */
static int parse_reorder(bs_t *bs, slice_t *sl, int list)
{
    if (!bs_u1(bs)) return H264_OK;
    int n = 0;
    for (;;) {
        uint32_t idc = bs_ue(bs);
        if (idc == 3) break;
        if (idc > 2 || n >= 32) return H264_ERR_CORRUPT;
        uint32_t arg = bs_ue(bs);
        if (bs_error(bs)) return H264_ERR_CORRUPT;
        sl->reorder_cmds[list][n][0] = (int)idc;
        sl->reorder_cmds[list][n][1] = (int)arg;
        n++;
    }
    sl->n_reorder[list] = n;
    return H264_OK;
}

/* pred_weight_table (7.3.3.2). `nlist` is 1 for P, 2 for B. */
static int parse_pred_weights(bs_t *bs, slice_t *sl, int nlist)
{
    sl->luma_log2_weight_denom = (int)bs_ue(bs);
    sl->chroma_log2_weight_denom = (int)bs_ue(bs);
    if (sl->luma_log2_weight_denom > 7 || sl->chroma_log2_weight_denom > 7)
        return H264_ERR_CORRUPT;
    for (int l = 0; l < nlist; l++) {
        for (int i = 0; i < sl->num_ref_idx_active[l]; i++) {
            sl->wp_luma_w[l][i] = 1 << sl->luma_log2_weight_denom;
            sl->wp_chroma_w[l][i][0] = sl->wp_chroma_w[l][i][1] =
                1 << sl->chroma_log2_weight_denom;
            if (bs_u1(bs)) {                          /* luma_weight_lX_flag */
                sl->wp_luma_w[l][i] = (int)bs_se(bs);
                sl->wp_luma_o[l][i] = (int)bs_se(bs);
            }
            if (bs_u1(bs)) {                          /* chroma_weight_lX_flag */
                sl->wp_chroma_w[l][i][0] = (int)bs_se(bs);
                sl->wp_chroma_o[l][i][0] = (int)bs_se(bs);
                sl->wp_chroma_w[l][i][1] = (int)bs_se(bs);
                sl->wp_chroma_o[l][i][1] = (int)bs_se(bs);
            }
            if (bs_error(bs)) return H264_ERR_CORRUPT;
        }
    }
    return H264_OK;
}

int h264_parse_slice_header(h264dec *d, bs_t *bs, int nal_ref_idc,
                            int nal_type, slice_t *sl)
{
    memset(sl, 0, sizeof *sl);
    sl->first_mb_in_slice = (int)bs_ue(bs);
    int st = (int)bs_ue(bs);
    sl->pps_id = (int)bs_ue(bs);
    if (bs_error(bs) || sl->pps_id > 31 || st > 9) return H264_ERR_CORRUPT;
    if (!d->pps[sl->pps_id].present) return H264_ERR_CORRUPT;

    pps_t *pps = &d->pps[sl->pps_id];
    sps_t *sps = &d->sps[pps->sps_id];
    if (!sps->present) return H264_ERR_CORRUPT;
    d->cur_pps = pps; d->cur_sps = sps;

    st %= 5;
    if (st >= 3) return H264_ERR_UNSUPPORTED;             /* SP/SI */
    sl->slice_type = st;                                  /* 0=P 1=B 2=I */
    sl->num_ref_idx_active[0] = pps->num_ref_idx_l0_default;
    sl->num_ref_idx_active[1] = pps->num_ref_idx_l1_default;

    sl->frame_num = (int)bs_u(bs, sps->log2_max_frame_num);
    if (nal_type == 5) sl->idr_pic_id = (int)bs_ue(bs);

    if (sps->poc_type == 0) {
        sl->poc_lsb = (int)bs_u(bs, sps->log2_max_poc_lsb);
        if (pps->bottom_field_poc_in_frame) sl->delta_poc_bottom = (int)bs_se(bs);
    } else if (sps->poc_type == 1 && !sps->delta_pic_order_always_zero) {
        sl->delta_poc[0] = (int)bs_se(bs);
        if (pps->bottom_field_poc_in_frame) sl->delta_poc[1] = (int)bs_se(bs);
    }

    if (sl->slice_type == SLICE_B)
        sl->direct_spatial_mv_pred = (int)bs_u1(bs);

    if (sl->slice_type != SLICE_I) {
        if (bs_u1(bs)) {                          /* num_ref_idx_active_override */
            sl->num_ref_idx_active[0] = (int)bs_ue(bs) + 1;
            if (sl->slice_type == SLICE_B)
                sl->num_ref_idx_active[1] = (int)bs_ue(bs) + 1;
        }
        if (sl->num_ref_idx_active[0] > 16 || sl->num_ref_idx_active[1] > 16)
            return H264_ERR_CORRUPT;
    }
    if (sl->slice_type != SLICE_I) {
        int rc = parse_reorder(bs, sl, 0);
        if (rc) return rc;
    }
    if (sl->slice_type == SLICE_B) {
        int rc = parse_reorder(bs, sl, 1);
        if (rc) return rc;
    }
    if (bs_error(bs)) return H264_ERR_CORRUPT;

    if ((pps->weighted_pred && sl->slice_type == SLICE_P) ||
        (pps->weighted_bipred_idc == 1 && sl->slice_type == SLICE_B)) {
        int rc = parse_pred_weights(bs, sl, sl->slice_type == SLICE_B ? 2 : 1);
        if (rc) return rc;
    }

    if (nal_ref_idc != 0) {
        if (nal_type == 5) {                              /* IDR */
            sl->no_output_of_prior_pics = (int)bs_u1(bs);
            sl->long_term_reference_flag = (int)bs_u1(bs);
        } else {
            sl->adaptive_marking = (int)bs_u1(bs);
            if (sl->adaptive_marking) {
                int n = 0;
                for (;;) {
                    uint32_t mmco = bs_ue(bs);
                    if (mmco == 0) break;
                    if (mmco > 6 || n >= 32) return H264_ERR_CORRUPT;
                    int arg = 0;
                    if (mmco == 1 || mmco == 3) arg = (int)bs_ue(bs);       /* diff_of_pic_nums */
                    if (mmco == 2) arg = (int)bs_ue(bs);                    /* long_term_pic_num */
                    if (mmco == 3 || mmco == 6) arg |= ((int)bs_ue(bs)) << 16; /* lt_frame_idx */
                    if (mmco == 4) arg = (int)bs_ue(bs);                    /* max_lt_idx_plus1 */
                    if (bs_error(bs)) return H264_ERR_CORRUPT;
                    sl->mmco[n][0] = (int)mmco;
                    sl->mmco[n][1] = arg;
                    n++;
                }
                sl->n_mmco = n;
            }
        }
    }

    if (pps->entropy_cabac && sl->slice_type != SLICE_I) {
        sl->cabac_init_idc = (int)bs_ue(bs);
        if (sl->cabac_init_idc > 2) return H264_ERR_CORRUPT;
    }

    sl->slice_qp_delta = (int)bs_se(bs);
    if (sl->slice_qp_delta < -87 || sl->slice_qp_delta > 77)
        return H264_ERR_CORRUPT;

    sl->disable_deblocking_filter_idc = 0;
    if (pps->deblock_control_present) {
        sl->disable_deblocking_filter_idc = (int)bs_ue(bs);
        if (sl->disable_deblocking_filter_idc > 2) return H264_ERR_CORRUPT;
        if (sl->disable_deblocking_filter_idc != 1) {
            sl->slice_alpha_c0_offset = (int)bs_se(bs) * 2;
            sl->slice_beta_offset = (int)bs_se(bs) * 2;
            if (sl->slice_alpha_c0_offset < -12 || sl->slice_alpha_c0_offset > 12 ||
                sl->slice_beta_offset < -12 || sl->slice_beta_offset > 12)
                return H264_ERR_CORRUPT;
        }
    }
    if (bs_error(bs)) return H264_ERR_CORRUPT;
    return H264_OK;
}
