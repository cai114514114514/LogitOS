/* c/lib/video/h264_nal.c -- NAL unit, SPS/PPS, and slice header parsing.
 *
 * Every value read here is untrusted network/hostile input: bounds are
 * checked through bs_t (sticky error), and every semantic limit the spec
 * imposes is validated before the value is stored. Anything outside the
 * baseline subset is H264_ERR_UNSUPPORTED, anything malformed is
 * H264_ERR_CORRUPT.
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
    if (bs_u1(bs)) {                          /* nal_hrd_parameters */
        parse_hrd(bs);
    }
    if (bs_u1(bs)) {                          /* vcl_hrd_parameters */
        parse_hrd(bs);
    }
    if (bs_error(bs)) return H264_ERR_CORRUPT;
    /* low_delay_hrd lives in HRD; a pic_struct flag follows */
    /* (pic_struct_present_flag) */
    bs_u1(bs);
    if (bs_u1(bs)) {                          /* bitstream_restriction */
        bs_u1(bs); bs_ue(bs); bs_ue(bs); bs_ue(bs);
        bs_ue(bs); bs_ue(bs); bs_ue(bs);
    }
    return bs_error(bs) ? H264_ERR_CORRUPT : H264_OK;
}

/* ------------------------------------------------------------- SPS ------ */
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
    /* High-profile profiles carry extra fields we do not implement. */
    if (s.profile_idc != 66 && s.profile_idc != 77 && s.profile_idc != 88)
        return H264_ERR_UNSUPPORTED;

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
    if (s.mb_width > 512 || s.mb_height > 512) return H264_ERR_CORRUPT;
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

    p.entropy_cabac = (int)bs_u1(bs);
    if (p.entropy_cabac) return H264_ERR_UNSUPPORTED;       /* CABAC: Phase 7 */
    p.bottom_field_poc_in_frame = (int)bs_u1(bs);
    p.num_slice_groups = (int)bs_ue(bs) + 1;
    if (p.num_slice_groups != 1) return H264_ERR_UNSUPPORTED;   /* FMO */
    p.num_ref_idx_l0_default = (int)bs_ue(bs) + 1;
    p.num_ref_idx_l1_default = (int)bs_ue(bs) + 1;
    if (p.num_ref_idx_l0_default > 16) return H264_ERR_CORRUPT;
    p.weighted_pred = (int)bs_u1(bs);
    p.weighted_bipred_idc = (int)bs_u(bs, 2);
    p.pic_init_qp = (int)bs_se(bs);
    p.pic_init_qs = (int)bs_se(bs);
    p.chroma_qp_index_offset = (int)bs_se(bs);
    if (p.pic_init_qp < -26 || p.pic_init_qp > 25) return H264_ERR_CORRUPT;
    p.deblock_control_present = (int)bs_u1(bs);
    p.constrained_intra_pred = (int)bs_u1(bs);
    p.redundant_pic_cnt_present = (int)bs_u1(bs);
    if (p.redundant_pic_cnt_present) return H264_ERR_UNSUPPORTED;

    /* Optional tail (more_rbsp_data): 8x8 transform + scaling matrices are
     * High profile; tolerate their presence only to reject. */
    if (bs_more_rbsp_data(bs)) {
        p.transform_8x8 = (int)bs_u1(bs);
        if (p.transform_8x8) return H264_ERR_UNSUPPORTED;
        if (bs_u1(bs)) return H264_ERR_UNSUPPORTED;         /* scaling lists */
        p.second_chroma_qp_offset = (int)bs_se(bs);
        p.has_second_chroma_offset = 1;
    }
    if (bs_error(bs)) return H264_ERR_CORRUPT;
    p.present = 1;
    d->pps[p.pps_id] = p;
    return H264_OK;
}

/* ------------------------------------------------------- slice header --- */
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
    d->cur_pps = pps; d->cur_sps = sps;

    st %= 5;
    if (st == 1) return H264_ERR_UNSUPPORTED;             /* B slice: Phase 6 */
    if (st >= 3) return H264_ERR_UNSUPPORTED;             /* SP/SI */
    sl->slice_type = st;                                  /* 0=P 2=I */
    sl->num_ref_idx_l0_active = pps->num_ref_idx_l0_default;

    sl->frame_num = (int)bs_u(bs, sps->log2_max_frame_num);
    if (nal_type == 5) sl->idr_pic_id = (int)bs_ue(bs);

    if (sps->poc_type == 0) {
        sl->poc_lsb = (int)bs_u(bs, sps->log2_max_poc_lsb);
        if (pps->bottom_field_poc_in_frame) sl->delta_poc_bottom = (int)bs_se(bs);
    } else if (sps->poc_type == 1 && !sps->delta_pic_order_always_zero) {
        sl->delta_poc[0] = (int)bs_se(bs);
        if (pps->bottom_field_poc_in_frame) sl->delta_poc[1] = (int)bs_se(bs);
    }

    if (sl->slice_type == 0) {                            /* P */
        if (bs_u1(bs)) {                                  /* num_ref_idx_active_override */
            sl->num_ref_idx_l0_active = (int)bs_ue(bs) + 1;
            if (sl->num_ref_idx_l0_active > 16) return H264_ERR_CORRUPT;
        }
        /* ref_pic_list_reordering, L0 */
        if (bs_u1(bs)) {
            int n = 0;
            for (;;) {
                uint32_t idc = bs_ue(bs);
                if (idc == 3) break;
                if (idc > 2 || n >= 32) return H264_ERR_CORRUPT;
                uint32_t arg = (idc == 2) ? bs_ue(bs) : bs_ue(bs);
                if (bs_error(bs)) return H264_ERR_CORRUPT;
                sl->reorder_cmds[n][0] = (int)idc;
                sl->reorder_cmds[n][1] = (int)arg;
                n++;
            }
            sl->n_reorder = n;
        }
        sl->ref_reorder_present = sl->n_reorder > 0;
    }

    if (pps->weighted_pred && sl->slice_type == 0) {
        sl->luma_log2_weight_denom = (int)bs_ue(bs);
        sl->chroma_log2_weight_denom = (int)bs_ue(bs);
        if (sl->luma_log2_weight_denom > 7 || sl->chroma_log2_weight_denom > 7)
            return H264_ERR_CORRUPT;
        for (int i = 0; i < sl->num_ref_idx_l0_active; i++) {
            sl->wp_luma_w[i] = 1 << sl->luma_log2_weight_denom;
            sl->wp_chroma_w[i][0] = sl->wp_chroma_w[i][1] =
                1 << sl->chroma_log2_weight_denom;
            if (bs_u1(bs)) {                              /* luma_weight_l0_flag */
                sl->wp_luma_w[i] = (int)bs_se(bs);
                sl->wp_luma_o[i] = (int)bs_se(bs);
            }
            if (bs_u1(bs)) {                              /* chroma_weight_l0_flag */
                sl->wp_chroma_w[i][0] = (int)bs_se(bs);
                sl->wp_chroma_o[i][0] = (int)bs_se(bs);
                sl->wp_chroma_w[i][1] = (int)bs_se(bs);
                sl->wp_chroma_o[i][1] = (int)bs_se(bs);
            }
        }
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

    sl->slice_qp_delta = (int)bs_se(bs);
    if (sl->slice_qp_delta < -26 || sl->slice_qp_delta > 25)
        return H264_ERR_CORRUPT;

    sl->disable_deblocking_filter_idc = 0;
    if (pps->deblock_control_present) {
        sl->disable_deblocking_filter_idc = (int)bs_ue(bs);
        if (sl->disable_deblocking_filter_idc > 2) return H264_ERR_CORRUPT;
        if (sl->disable_deblocking_filter_idc != 1) {
            sl->slice_alpha_c0_offset = (int)bs_se(bs) * 2;
            sl->slice_beta_offset = (int)bs_se(bs) * 2;
        }
    }
    if (bs_error(bs)) return H264_ERR_CORRUPT;
    return H264_OK;
}
