/* c/lib/video/h265_nal.c -- Annex B, VPS/SPS/PPS and the slice segment header.
 *
 * HEVC's parameter-set model is richer than H.264's in three ways that all
 * show up here. There is a third level (the VPS) which single-layer Main
 * carries but which contributes nothing to decoding, so it is parsed for
 * validity and otherwise ignored. Reference-picture management moved out of
 * the per-picture MMCO commands into declarative *reference picture sets* --
 * a list of POC deltas, either taken from one of the SPS's candidates or
 * written inline, and optionally coded as a delta against another set
 * (st_ref_pic_set's inter-RPS prediction, 7.4.8). And the slice header can be
 * a *dependent* segment that inherits everything from the previous
 * independent one, which is why the header struct is copied rather than
 * re-parsed for those.
 *
 * Every value here is untrusted input: bounds come through bs_t's sticky
 * error, and each semantic limit is checked before the value is stored.
 */
#include <stdlib.h>
#include <string.h>
#include "h265.h"
#include "h265_int.h"

/* Strip emulation-prevention bytes (00 00 03 -> 00 00) into a fresh RBSP
 * buffer. `len` includes the two NAL header bytes, which are dropped. */
uint8_t *h265_nal_to_rbsp(const uint8_t *nal, int len, int *rbsp_len,
                          int *epb_pos, int max_epb, int *n_epb)
{
    if (len < 2) return 0;
    uint8_t *out = (uint8_t *)malloc((size_t)len);
    if (!out) return 0;
    int o = 0, zeros = 0, ne = 0;
    for (int i = 2; i < len; i++) {
        uint8_t b = nal[i];
        if (zeros == 2 && b == 3) {
            zeros = 0;
            /* Record the RBSP index the removed byte sat at. WPP and tile
             * entry point offsets are counted in NAL bytes (emulation
             * prevention included) but index into the RBSP once we strip
             * them, so a substream that happens to contain a 00 00 03 would
             * start one byte late without this. */
            if (epb_pos && ne < max_epb) epb_pos[ne] = o;
            ne++;
            continue;
        }
        zeros = (b == 0) ? zeros + 1 : 0;
        out[o++] = b;
    }
    *rbsp_len = o;
    if (n_epb) *n_epb = ne < max_epb ? ne : max_epb;
    return out;
}

static int ceil_log2(int v)
{
    int n = 0;
    while ((1 << n) < v) n++;
    return n;
}

/* ------------------------------------------------- profile_tier_level ---- */
static void parse_ptl(bs_t *bs, int profile_present, int max_sub_layers_minus1)
{
    if (profile_present) {
        bs_u(bs, 32); bs_u(bs, 32); bs_u(bs, 24);   /* 88 bits of profile info */
    }
    bs_u(bs, 8);                                    /* general_level_idc */
    int spf[8], slf[8];
    for (int i = 0; i < max_sub_layers_minus1; i++) {
        spf[i] = (int)bs_u1(bs);
        slf[i] = (int)bs_u1(bs);
    }
    if (max_sub_layers_minus1 > 0)
        for (int i = max_sub_layers_minus1; i < 8; i++) bs_u(bs, 2);
    for (int i = 0; i < max_sub_layers_minus1; i++) {
        if (spf[i]) { bs_u(bs, 32); bs_u(bs, 32); bs_u(bs, 24); }
        if (slf[i]) bs_u(bs, 8);
    }
}

/* ------------------------------------------------------- scaling lists --- */
static const uint8_t default_sl_intra[64] = {
    16,16,16,16,17,18,21,24, 16,16,16,16,17,19,22,25,
    16,16,17,18,20,22,25,29, 16,16,18,21,24,27,31,36,
    17,17,20,24,30,35,41,47, 18,19,22,27,35,44,54,65,
    21,22,25,31,41,54,70,88, 24,25,29,36,47,65,88,115
};
static const uint8_t default_sl_inter[64] = {
    16,16,16,16,17,18,20,24, 16,16,16,17,18,20,24,25,
    16,16,17,18,20,24,25,28, 16,17,18,20,24,25,28,33,
    17,18,20,24,25,28,33,41, 18,20,24,25,28,33,41,54,
    20,24,25,28,33,41,54,71, 24,25,28,33,41,54,71,91
};

static void default_scaling(uint8_t sl[4][6][64], uint8_t dc[2][6])
{
    for (int m = 0; m < 6; m++) {
        for (int i = 0; i < 16; i++) sl[0][m][i] = 16;
        const uint8_t *def = (m < 3) ? default_sl_intra : default_sl_inter;
        for (int s = 1; s < 4; s++) memcpy(sl[s][m], def, 64);
        dc[0][m] = dc[1][m] = 16;
    }
}

static int parse_scaling_list_data(bs_t *bs, uint8_t sl[4][6][64], uint8_t dc[2][6])
{
    default_scaling(sl, dc);
    for (int size = 0; size < 4; size++) {
        for (int mat = 0; mat < 6; mat += (size == 3) ? 3 : 1) {
            if (!bs_u1(bs)) {
                uint32_t delta = bs_ue(bs);
                if (bs_error(bs)) return H265_ERR_CORRUPT;
                if (delta == 0) continue;            /* keep the default */
                int step = (size == 3) ? 3 : 1;
                int ref = mat - (int)delta * step;
                if (ref < 0) return H265_ERR_CORRUPT;
                memcpy(sl[size][mat], sl[size][ref], 64);
                if (size > 1) dc[size - 2][mat] = dc[size - 2][ref];
            } else {
                int next = 8;
                int n = (size == 0) ? 16 : 64;
                if (size > 1) {
                    int v = bs_se(bs);
                    if (v < -7 || v > 247) return H265_ERR_CORRUPT;
                    next = v + 8;
                    dc[size - 2][mat] = (uint8_t)next;
                }
                for (int i = 0; i < n; i++) {
                    int v = bs_se(bs);
                    next = (next + v + 256) % 256;
                    sl[size][mat][i] = (uint8_t)next;
                }
                if (bs_error(bs)) return H265_ERR_CORRUPT;
            }
        }
    }
    /* sizeId 3 only codes matrixId 0 and 3; 1,2 mirror 0 and 4,5 mirror 3. */
    for (int m = 1; m < 3; m++) {
        memcpy(sl[3][m], sl[3][0], 64);
        dc[1][m] = dc[1][0];
    }
    for (int m = 4; m < 6; m++) {
        memcpy(sl[3][m], sl[3][3], 64);
        dc[1][m] = dc[1][3];
    }
    return H265_OK;
}

/* -------------------------------------------------------- st_ref_pic_set -- */
/* Fills `out`; `sets` are the already-parsed SPS candidates, `idx` this set's
 * index (== nsets when it is written inline in a slice header). */
static int parse_strps(bs_t *bs, strps_t *out, const strps_t *sets, int idx, int nsets)
{
    memset(out, 0, sizeof *out);
    int inter_pred = 0;
    if (idx != 0) inter_pred = (int)bs_u1(bs);

    if (inter_pred) {
        int delta_idx = 1;
        if (idx == nsets) delta_idx = (int)bs_ue(bs) + 1;
        int sign = (int)bs_u1(bs);
        int abs_delta = (int)bs_ue(bs) + 1;
        if (bs_error(bs)) return H265_ERR_CORRUPT;
        int ridx = idx - delta_idx;
        if (ridx < 0 || ridx >= nsets) return H265_ERR_CORRUPT;
        int delta_rps = (1 - 2 * sign) * abs_delta;
        const strps_t *r = &sets[ridx];
        int nd = r->num_negative + r->num_positive;
        uint8_t used[H265_MAX_REFS + 1], use_delta[H265_MAX_REFS + 1];
        if (nd + 1 > H265_MAX_REFS + 1) return H265_ERR_CORRUPT;
        for (int j = 0; j <= nd; j++) {
            used[j] = (uint8_t)bs_u1(bs);
            use_delta[j] = 1;
            if (!used[j]) use_delta[j] = (uint8_t)bs_u1(bs);
        }
        if (bs_error(bs)) return H265_ERR_CORRUPT;

        int i = 0;
        for (int j = r->num_positive - 1; j >= 0; j--) {
            int dpoc = r->delta_poc[r->num_negative + j] + delta_rps;
            if (dpoc < 0 && use_delta[r->num_negative + j]) {
                out->delta_poc[i] = dpoc;
                out->used[i++] = used[r->num_negative + j];
            }
        }
        if (delta_rps < 0 && use_delta[nd]) {
            out->delta_poc[i] = delta_rps;
            out->used[i++] = used[nd];
        }
        for (int j = 0; j < r->num_negative; j++) {
            int dpoc = r->delta_poc[j] + delta_rps;
            if (dpoc < 0 && use_delta[j]) {
                out->delta_poc[i] = dpoc;
                out->used[i++] = used[j];
            }
        }
        out->num_negative = i;

        int k = i;
        for (int j = r->num_negative - 1; j >= 0; j--) {
            int dpoc = r->delta_poc[j] + delta_rps;
            if (dpoc > 0 && use_delta[j]) {
                out->delta_poc[k] = dpoc;
                out->used[k++] = used[j];
            }
        }
        if (delta_rps > 0 && use_delta[nd]) {
            out->delta_poc[k] = delta_rps;
            out->used[k++] = used[nd];
        }
        for (int j = 0; j < r->num_positive; j++) {
            int dpoc = r->delta_poc[r->num_negative + j] + delta_rps;
            if (dpoc > 0 && use_delta[r->num_negative + j]) {
                out->delta_poc[k] = dpoc;
                out->used[k++] = used[r->num_negative + j];
            }
        }
        out->num_positive = k - i;
        if (k > H265_MAX_REFS) return H265_ERR_CORRUPT;
        return H265_OK;
    }

    int nneg = (int)bs_ue(bs);
    int npos = (int)bs_ue(bs);
    if (bs_error(bs) || nneg < 0 || npos < 0 || nneg + npos > H265_MAX_REFS)
        return H265_ERR_CORRUPT;
    out->num_negative = nneg;
    out->num_positive = npos;
    int prev = 0;
    for (int i = 0; i < nneg; i++) {
        int d = (int)bs_ue(bs) + 1;
        prev -= d;
        out->delta_poc[i] = prev;
        out->used[i] = (uint8_t)bs_u1(bs);
    }
    prev = 0;
    for (int i = 0; i < npos; i++) {
        int d = (int)bs_ue(bs) + 1;
        prev += d;
        out->delta_poc[nneg + i] = prev;
        out->used[nneg + i] = (uint8_t)bs_u1(bs);
    }
    return bs_error(bs) ? H265_ERR_CORRUPT : H265_OK;
}

/* ------------------------------------------------------------- VUI ------- */
static void parse_hrd(bs_t *bs, int common_inf, int max_sub_layers_minus1)
{
    int nal_hrd = 0, vcl_hrd = 0, sub_pic = 0;
    if (common_inf) {
        nal_hrd = (int)bs_u1(bs);
        vcl_hrd = (int)bs_u1(bs);
        if (nal_hrd || vcl_hrd) {
            sub_pic = (int)bs_u1(bs);
            if (sub_pic) { bs_u(bs, 8); bs_u(bs, 5); bs_u1(bs); bs_u(bs, 5); }
            bs_u(bs, 4); bs_u(bs, 4);
            if (sub_pic) bs_u(bs, 4);
            bs_u(bs, 5); bs_u(bs, 5); bs_u(bs, 5);
        }
    }
    for (int i = 0; i <= max_sub_layers_minus1; i++) {
        int fixed_pic_rate_general = (int)bs_u1(bs);
        int fixed_pic_rate_within = fixed_pic_rate_general;
        int low_delay = 0;
        uint32_t cpb_cnt = 1;
        if (!fixed_pic_rate_general) fixed_pic_rate_within = (int)bs_u1(bs);
        if (fixed_pic_rate_within) bs_ue(bs);
        else low_delay = (int)bs_u1(bs);
        if (!low_delay) cpb_cnt = bs_ue(bs) + 1;
        if (bs_error(bs) || cpb_cnt > 32) { bs->error = 1; return; }
        for (int nl = 0; nl < 2; nl++) {
            if ((nl == 0 && !nal_hrd) || (nl == 1 && !vcl_hrd)) continue;
            for (uint32_t j = 0; j < cpb_cnt; j++) {
                bs_ue(bs); bs_ue(bs);
                if (sub_pic) { bs_ue(bs); bs_ue(bs); }
                bs_u1(bs);
            }
        }
    }
}

static void parse_vui(sps_t *s, bs_t *bs)
{
    if (bs_u1(bs)) {                          /* aspect_ratio_info_present */
        uint32_t idc = bs_u(bs, 8);
        if (idc == 255) bs_u(bs, 32);
    }
    if (bs_u1(bs)) bs_u1(bs);                 /* overscan */
    if (bs_u1(bs)) {                          /* video_signal_type */
        bs_u(bs, 3); bs_u1(bs);
        if (bs_u1(bs)) bs_u(bs, 24);
    }
    if (bs_u1(bs)) { bs_ue(bs); bs_ue(bs); }  /* chroma_loc_info */
    bs_u1(bs);                                /* neutral_chroma_indication */
    bs_u1(bs);                                /* field_seq_flag */
    bs_u1(bs);                                /* frame_field_info_present */
    if (bs_u1(bs)) {                          /* default_display_window */
        bs_ue(bs); bs_ue(bs); bs_ue(bs); bs_ue(bs);
    }
    if (bs_u1(bs)) {                          /* vui_timing_info_present */
        s->num_units_in_tick = bs_u(bs, 32);
        s->time_scale = bs_u(bs, 32);
        s->vui_timing = s->num_units_in_tick && s->time_scale;
        if (bs_u1(bs)) bs_ue(bs);             /* num_ticks_poc_diff_one */
        if (bs_u1(bs)) parse_hrd(bs, 1, s->max_sub_layers - 1);
    }
    if (bs_u1(bs)) {                          /* bitstream_restriction */
        bs_u1(bs); bs_u1(bs); bs_u1(bs);
        bs_ue(bs); bs_ue(bs); bs_ue(bs); bs_ue(bs); bs_ue(bs);
    }
}

/* ------------------------------------------------------------- VPS ------- */
int h265_parse_vps(h265dec *d, bs_t *bs)
{
    int id = (int)bs_u(bs, 4);
    bs_u(bs, 2);                              /* base_layer_internal/available */
    bs_u(bs, 6);                              /* vps_max_layers_minus1 */
    int max_sub = (int)bs_u(bs, 3) + 1;
    bs_u1(bs);                                /* temporal_id_nesting */
    bs_u(bs, 16);                             /* reserved 0xFFFF */
    if (bs_error(bs) || id > 15) return H265_ERR_CORRUPT;
    parse_ptl(bs, 1, max_sub - 1);
    if (bs_error(bs)) return H265_ERR_CORRUPT;
    /* The rest (layer sets, timing) never affects a single-layer decode. */
    d->vps[id].present = 1;
    d->vps[id].vps_id = id;
    d->vps[id].max_sub_layers = max_sub;
    return H265_OK;
}

/* ------------------------------------------------------------- SPS ------- */
int h265_parse_sps(h265dec *d, bs_t *bs)
{
    sps_t s;
    memset(&s, 0, sizeof s);
    s.vps_id = (int)bs_u(bs, 4);
    s.max_sub_layers = (int)bs_u(bs, 3) + 1;
    bs_u1(bs);                                /* temporal_id_nesting */
    parse_ptl(bs, 1, s.max_sub_layers - 1);
    s.sps_id = (int)bs_ue(bs);
    if (bs_error(bs) || s.sps_id >= H265_MAX_SPS) return H265_ERR_CORRUPT;

    s.chroma_format_idc = (int)bs_ue(bs);
    if (s.chroma_format_idc == 3) s.separate_colour_plane = (int)bs_u1(bs);
    if (s.chroma_format_idc != 1) return H265_ERR_UNSUPPORTED;   /* 4:2:0 only */

    s.width = (int)bs_ue(bs);
    s.height = (int)bs_ue(bs);
    if (bs_error(bs) || s.width <= 0 || s.height <= 0 ||
        s.width > 16384 || s.height > 16384) return H265_ERR_CORRUPT;
    if (bs_u1(bs))
        for (int i = 0; i < 4; i++) s.conf_win[i] = (int)bs_ue(bs);

    s.bit_depth_luma = (int)bs_ue(bs) + 8;
    s.bit_depth_chroma = (int)bs_ue(bs) + 8;
    if (s.bit_depth_luma != 8 || s.bit_depth_chroma != 8)
        return H265_ERR_UNSUPPORTED;          /* Main 8-bit only */

    s.log2_max_poc_lsb = (int)bs_ue(bs) + 4;
    if (s.log2_max_poc_lsb > 16) return H265_ERR_CORRUPT;

    int sub_layer_ordering = (int)bs_u1(bs);
    for (int i = sub_layer_ordering ? 0 : s.max_sub_layers - 1;
         i < s.max_sub_layers; i++) {
        s.max_dec_pic_buffering[i] = (int)bs_ue(bs) + 1;
        s.max_num_reorder[i] = (int)bs_ue(bs);
        uint32_t lat = bs_ue(bs);
        s.max_latency[i] = lat ? (int)(lat - 1) : 0;
        if (s.max_dec_pic_buffering[i] > 16) return H265_ERR_CORRUPT;
    }
    if (!sub_layer_ordering)
        for (int i = 0; i < s.max_sub_layers - 1; i++) {
            s.max_dec_pic_buffering[i] = s.max_dec_pic_buffering[s.max_sub_layers - 1];
            s.max_num_reorder[i] = s.max_num_reorder[s.max_sub_layers - 1];
            s.max_latency[i] = s.max_latency[s.max_sub_layers - 1];
        }

    s.log2_min_cb = (int)bs_ue(bs) + 3;
    s.log2_ctb = s.log2_min_cb + (int)bs_ue(bs);
    s.log2_min_tb = (int)bs_ue(bs) + 2;
    s.log2_max_tb = s.log2_min_tb + (int)bs_ue(bs);
    s.max_transform_hierarchy_depth_inter = (int)bs_ue(bs);
    s.max_transform_hierarchy_depth_intra = (int)bs_ue(bs);
    if (bs_error(bs)) return H265_ERR_CORRUPT;
    if (s.log2_min_cb < 3 || s.log2_ctb > 6 || s.log2_ctb < s.log2_min_cb ||
        s.log2_min_tb < 2 || s.log2_max_tb > 5 || s.log2_max_tb < s.log2_min_tb ||
        s.log2_max_tb > s.log2_ctb)
        return H265_ERR_CORRUPT;
    if (s.width % (1 << s.log2_min_cb) || s.height % (1 << s.log2_min_cb))
        return H265_ERR_CORRUPT;

    s.scaling_list_enabled = (int)bs_u1(bs);
    if (s.scaling_list_enabled) {
        default_scaling(s.sl, s.sl_dc);
        s.sl_present = 1;
        s.sps_scaling_list_data_present = (int)bs_u1(bs);
        if (s.sps_scaling_list_data_present) {
            int rc = parse_scaling_list_data(bs, s.sl, s.sl_dc);
            if (rc) return rc;
        }
    }
    s.amp_enabled = (int)bs_u1(bs);
    s.sao_enabled = (int)bs_u1(bs);
    s.pcm_enabled = (int)bs_u1(bs);
    if (s.pcm_enabled) {
        /* I_PCM is parsed but not decoded. pcm_flag is a terminating bin, and
         * restarting the arithmetic decoder at the right byte after it is the
         * one place in HEVC where the spec's engine model and a bit-at-a-time
         * implementation disagree about "the current position". Getting that
         * subtly wrong desynchronises the rest of the slice, so rather than
         * ship a path that cannot be exercised -- x265 has no PCM encoder and
         * none of the test matrix can produce one -- this is an honest
         * refusal. */
        return H265_ERR_UNSUPPORTED;
    }

    s.num_strps = (int)bs_ue(bs);
    if (bs_error(bs) || s.num_strps > H265_MAX_RPS) return H265_ERR_CORRUPT;
    for (int i = 0; i < s.num_strps; i++) {
        int rc = parse_strps(bs, &s.strps[i], s.strps, i, s.num_strps);
        if (rc) return rc;
    }

    s.long_term_ref_pics_present = (int)bs_u1(bs);
    if (s.long_term_ref_pics_present) {
        s.num_long_term_sps = (int)bs_ue(bs);
        if (bs_error(bs) || s.num_long_term_sps > 32) return H265_ERR_CORRUPT;
        for (int i = 0; i < s.num_long_term_sps; i++) {
            s.lt_ref_poc_lsb_sps[i] = (int)bs_u(bs, s.log2_max_poc_lsb);
            s.lt_used_by_curr_sps[i] = (int)bs_u1(bs);
        }
    }
    s.temporal_mvp_enabled = (int)bs_u1(bs);
    s.strong_intra_smoothing = (int)bs_u1(bs);
    if (bs_u1(bs)) parse_vui(&s, bs);
    if (bs_error(bs)) return H265_ERR_CORRUPT;

    /* derived */
    s.ctb_size = 1 << s.log2_ctb;
    s.ctb_width = (s.width + s.ctb_size - 1) >> s.log2_ctb;
    s.ctb_height = (s.height + s.ctb_size - 1) >> s.log2_ctb;
    s.ctb_count = s.ctb_width * s.ctb_height;
    s.min_cb_width = s.width >> s.log2_min_cb;
    s.min_cb_height = s.height >> s.log2_min_cb;
    s.min_tb_width = s.width >> 2;
    s.min_tb_height = s.height >> 2;
    s.pic_width_c = s.width / 2;
    s.pic_height_c = s.height / 2;
    s.present = 1;
    d->sps[s.sps_id] = s;
    return H265_OK;
}

/* ------------------------------------------------------------- PPS ------- */
int h265_parse_pps(h265dec *d, bs_t *bs)
{
    pps_t p;
    memset(&p, 0, sizeof p);
    p.pps_id = (int)bs_ue(bs);
    p.sps_id = (int)bs_ue(bs);
    if (bs_error(bs) || p.pps_id >= H265_MAX_PPS || p.sps_id >= H265_MAX_SPS)
        return H265_ERR_CORRUPT;
    if (!d->sps[p.sps_id].present) return H265_ERR_CORRUPT;
    const sps_t *sps = &d->sps[p.sps_id];

    p.dependent_slice_segments_enabled = (int)bs_u1(bs);
    p.output_flag_present = (int)bs_u1(bs);
    p.num_extra_slice_header_bits = (int)bs_u(bs, 3);
    p.sign_data_hiding = (int)bs_u1(bs);
    p.cabac_init_present = (int)bs_u1(bs);
    p.num_ref_idx_l0_default = (int)bs_ue(bs) + 1;
    p.num_ref_idx_l1_default = (int)bs_ue(bs) + 1;
    if (p.num_ref_idx_l0_default > 16 || p.num_ref_idx_l1_default > 16)
        return H265_ERR_CORRUPT;
    p.init_qp = (int)bs_se(bs) + 26;
    p.constrained_intra_pred = (int)bs_u1(bs);
    p.transform_skip_enabled = (int)bs_u1(bs);
    p.cu_qp_delta_enabled = (int)bs_u1(bs);
    if (p.cu_qp_delta_enabled) p.diff_cu_qp_delta_depth = (int)bs_ue(bs);
    p.cb_qp_offset = (int)bs_se(bs);
    p.cr_qp_offset = (int)bs_se(bs);
    p.slice_chroma_qp_offsets_present = (int)bs_u1(bs);
    p.weighted_pred = (int)bs_u1(bs);
    p.weighted_bipred = (int)bs_u1(bs);
    p.transquant_bypass_enabled = (int)bs_u1(bs);
    p.tiles_enabled = (int)bs_u1(bs);
    p.entropy_coding_sync_enabled = (int)bs_u1(bs);
    if (bs_error(bs)) return H265_ERR_CORRUPT;
    if (p.init_qp < 0 || p.init_qp > 51) return H265_ERR_CORRUPT;
    if (p.diff_cu_qp_delta_depth > 3) return H265_ERR_CORRUPT;

    p.num_tile_columns = p.num_tile_rows = 1;
    p.uniform_spacing = 1;
    p.loop_filter_across_tiles = 1;
    if (p.tiles_enabled) {
        p.num_tile_columns = (int)bs_ue(bs) + 1;
        p.num_tile_rows = (int)bs_ue(bs) + 1;
        p.uniform_spacing = (int)bs_u1(bs);
        if (bs_error(bs) ||
            p.num_tile_columns < 1 || p.num_tile_columns > H265_MAX_TILES ||
            p.num_tile_rows < 1 || p.num_tile_rows > H265_MAX_TILES ||
            p.num_tile_columns > sps->ctb_width || p.num_tile_rows > sps->ctb_height)
            return H265_ERR_CORRUPT;
        if (!p.uniform_spacing) {
            int sum = 0;
            for (int i = 0; i < p.num_tile_columns - 1; i++) {
                p.column_width[i] = (int)bs_ue(bs) + 1;
                sum += p.column_width[i];
            }
            if (sum >= sps->ctb_width) return H265_ERR_CORRUPT;
            p.column_width[p.num_tile_columns - 1] = sps->ctb_width - sum;
            sum = 0;
            for (int i = 0; i < p.num_tile_rows - 1; i++) {
                p.row_height[i] = (int)bs_ue(bs) + 1;
                sum += p.row_height[i];
            }
            if (sum >= sps->ctb_height) return H265_ERR_CORRUPT;
            p.row_height[p.num_tile_rows - 1] = sps->ctb_height - sum;
        }
        p.loop_filter_across_tiles = (int)bs_u1(bs);
    }
    p.loop_filter_across_slices = (int)bs_u1(bs);
    p.deblocking_filter_control_present = (int)bs_u1(bs);
    if (p.deblocking_filter_control_present) {
        p.deblocking_filter_override_enabled = (int)bs_u1(bs);
        p.pps_deblocking_filter_disabled = (int)bs_u1(bs);
        if (!p.pps_deblocking_filter_disabled) {
            p.beta_offset = (int)bs_se(bs) * 2;
            p.tc_offset = (int)bs_se(bs) * 2;
        }
    }
    p.pps_scaling_list_data_present = (int)bs_u1(bs);
    if (p.pps_scaling_list_data_present) {
        int rc = parse_scaling_list_data(bs, p.sl, p.sl_dc);
        if (rc) return rc;
        p.sl_present = 1;
    }
    p.lists_modification_present = (int)bs_u1(bs);
    p.log2_parallel_merge_level = (int)bs_ue(bs) + 2;
    p.slice_segment_header_extension_present = (int)bs_u1(bs);
    if (bs_error(bs)) return H265_ERR_CORRUPT;
    if (p.log2_parallel_merge_level > sps->log2_ctb) return H265_ERR_CORRUPT;

    p.present = 1;
    d->pps[p.pps_id] = p;
    return H265_OK;
}

/* ------------------------------------------------- pred_weight_table ----- */
static int parse_pred_weight_table(bs_t *bs, slice_t *sl)
{
    sl->luma_log2_weight_denom = (int)bs_ue(bs);
    if (bs_error(bs) || sl->luma_log2_weight_denom > 7) return H265_ERR_CORRUPT;
    sl->chroma_log2_weight_denom = sl->luma_log2_weight_denom + (int)bs_se(bs);
    if (sl->chroma_log2_weight_denom < 0 || sl->chroma_log2_weight_denom > 7)
        return H265_ERR_CORRUPT;

    int nlists = (sl->type == SLICE_B) ? 2 : 1;
    for (int l = 0; l < nlists; l++) {
        int n = sl->num_ref_idx[l];
        uint8_t lflag[H265_MAX_REFS], cflag[H265_MAX_REFS];
        /* Single-layer: a reference can never share the current POC, so the
         * spec's "different picture" condition is always true and both flag
         * arrays are always present. */
        for (int i = 0; i < n; i++) lflag[i] = (uint8_t)bs_u1(bs);
        for (int i = 0; i < n; i++) cflag[i] = (uint8_t)bs_u1(bs);
        for (int i = 0; i < n; i++) {
            sl->luma_weight[l][i] = 1 << sl->luma_log2_weight_denom;
            sl->luma_offset[l][i] = 0;
            sl->chroma_weight[l][i][0] = sl->chroma_weight[l][i][1] =
                1 << sl->chroma_log2_weight_denom;
            sl->chroma_offset[l][i][0] = sl->chroma_offset[l][i][1] = 0;
            if (lflag[i]) {
                sl->luma_weight[l][i] =
                    (1 << sl->luma_log2_weight_denom) + (int)bs_se(bs);
                sl->luma_offset[l][i] = (int)bs_se(bs);
                if (sl->luma_offset[l][i] < -128 || sl->luma_offset[l][i] > 127)
                    return H265_ERR_CORRUPT;
            }
            if (cflag[i]) {
                for (int j = 0; j < 2; j++) {
                    int dw = (int)bs_se(bs);
                    int doff = (int)bs_se(bs);
                    int w = (1 << sl->chroma_log2_weight_denom) + dw;
                    sl->chroma_weight[l][i][j] = w;
                    sl->chroma_offset[l][i][j] = h265_clip3(-128, 127,
                        128 + doff - ((128 * w) >> sl->chroma_log2_weight_denom));
                }
            }
        }
    }
    return bs_error(bs) ? H265_ERR_CORRUPT : H265_OK;
}

/* ------------------------------------------------ slice segment header --- */
int h265_parse_slice_header(h265dec *d, bs_t *bs, int nal_type,
                            int nuh_temporal_id, slice_t *sl)
{
    (void)nuh_temporal_id;
    memset(sl, 0, sizeof *sl);
    sl->first_slice_in_pic = (int)bs_u1(bs);
    if (IS_IRAP(nal_type)) sl->no_output_of_prior_pics = (int)bs_u1(bs);
    sl->pps_id = (int)bs_ue(bs);
    if (bs_error(bs) || sl->pps_id >= H265_MAX_PPS) return H265_ERR_CORRUPT;
    if (!d->pps[sl->pps_id].present) return H265_ERR_CORRUPT;
    pps_t *pps = &d->pps[sl->pps_id];
    sps_t *sps = &d->sps[pps->sps_id];
    if (!sps->present) return H265_ERR_CORRUPT;
    d->cur_pps = pps;
    d->cur_sps = sps;

    if (!sl->first_slice_in_pic) {
        if (pps->dependent_slice_segments_enabled)
            sl->dependent_slice_segment = (int)bs_u1(bs);
        int nbits = ceil_log2(sps->ctb_count);
        sl->segment_address = (int)bs_u(bs, nbits);
        if (sl->segment_address >= sps->ctb_count) return H265_ERR_CORRUPT;
    }

    if (sl->dependent_slice_segment) {
        /* Everything below is inherited from the last independent segment;
         * the caller copies it in and only the entry points are re-read. */
        goto entry_points;
    }

    for (int i = 0; i < pps->num_extra_slice_header_bits; i++) bs_u1(bs);
    sl->type = (int)bs_ue(bs);
    if (bs_error(bs) || sl->type > 2) return H265_ERR_CORRUPT;
    sl->pic_output_flag = 1;
    if (pps->output_flag_present) sl->pic_output_flag = (int)bs_u1(bs);
    if (sps->separate_colour_plane) bs_u(bs, 2);

    sl->num_ref_idx[0] = sl->num_ref_idx[1] = 0;
    if (!IS_IDR(nal_type)) {
        sl->poc_lsb = (int)bs_u(bs, sps->log2_max_poc_lsb);
        sl->short_term_ref_pic_set_sps_flag = (int)bs_u1(bs);
        if (!sl->short_term_ref_pic_set_sps_flag) {
            int rc = parse_strps(bs, &sl->rps, sps->strps, sps->num_strps,
                                 sps->num_strps);
            if (rc) return rc;
        } else {
            int idx = 0;
            if (sps->num_strps > 1)
                idx = (int)bs_u(bs, ceil_log2(sps->num_strps));
            if (idx >= sps->num_strps) return H265_ERR_CORRUPT;
            sl->rps = sps->strps[idx];
        }
        if (sps->long_term_ref_pics_present) {
            int nsps = 0, npics = 0;
            if (sps->num_long_term_sps > 0) nsps = (int)bs_ue(bs);
            npics = (int)bs_ue(bs);
            if (bs_error(bs) || nsps > sps->num_long_term_sps ||
                nsps + npics > 32) return H265_ERR_CORRUPT;
            sl->num_long_term = nsps + npics;
            for (int i = 0; i < nsps + npics; i++) {
                int poc_lsb, used;
                if (i < nsps) {
                    int k = 0;
                    if (sps->num_long_term_sps > 1)
                        k = (int)bs_u(bs, ceil_log2(sps->num_long_term_sps));
                    if (k >= sps->num_long_term_sps) return H265_ERR_CORRUPT;
                    poc_lsb = sps->lt_ref_poc_lsb_sps[k];
                    used = sps->lt_used_by_curr_sps[k];
                } else {
                    poc_lsb = (int)bs_u(bs, sps->log2_max_poc_lsb);
                    used = (int)bs_u1(bs);
                }
                sl->lt_poc[i] = poc_lsb;          /* PocLsbLt */
                sl->lt_used[i] = used;
                sl->lt_msb_present[i] = (int)bs_u1(bs);
                /* 7.4.7.1: the msb cycle accumulates within each of the two
                 * runs (SPS-derived entries, then slice-coded ones). */
                int raw = sl->lt_msb_present[i] ? (int)bs_ue(bs) : 0;
                sl->lt_msb_cycle[i] = (i == 0 || i == nsps)
                                    ? raw : raw + sl->lt_msb_cycle[i - 1];
            }
        }
        if (sps->temporal_mvp_enabled) sl->temporal_mvp_enabled = (int)bs_u1(bs);
    }

    if (sps->sao_enabled) {
        sl->sao_luma = (int)bs_u1(bs);
        sl->sao_chroma = (int)bs_u1(bs);
    }

    sl->max_merge_cand = 5;
    sl->collocated_from_l0 = 1;
    if (sl->type == SLICE_P || sl->type == SLICE_B) {
        sl->num_ref_idx[0] = pps->num_ref_idx_l0_default;
        sl->num_ref_idx[1] = (sl->type == SLICE_B) ? pps->num_ref_idx_l1_default : 0;
        if (bs_u1(bs)) {                      /* num_ref_idx_active_override */
            sl->num_ref_idx[0] = (int)bs_ue(bs) + 1;
            if (sl->type == SLICE_B) sl->num_ref_idx[1] = (int)bs_ue(bs) + 1;
        }
        if (sl->num_ref_idx[0] > 16 || sl->num_ref_idx[1] > 16)
            return H265_ERR_CORRUPT;

        int total_curr = 0;
        for (int i = 0; i < sl->rps.num_negative + sl->rps.num_positive; i++)
            if (sl->rps.used[i]) total_curr++;
        for (int i = 0; i < sl->num_long_term; i++)
            if (sl->lt_used[i]) total_curr++;

        if (pps->lists_modification_present && total_curr > 1) {
            int nb = ceil_log2(total_curr);
            int nlists = (sl->type == SLICE_B) ? 2 : 1;
            for (int l = 0; l < nlists; l++) {
                sl->ref_list_modification_flag[l] = (int)bs_u1(bs);
                if (sl->ref_list_modification_flag[l])
                    for (int i = 0; i < sl->num_ref_idx[l]; i++)
                        sl->list_entry[l][i] = (int)bs_u(bs, nb);
            }
        }
        if (sl->type == SLICE_B) sl->mvd_l1_zero = (int)bs_u1(bs);
        if (pps->cabac_init_present) sl->cabac_init_flag = (int)bs_u1(bs);
        if (sl->temporal_mvp_enabled) {
            if (sl->type == SLICE_B) sl->collocated_from_l0 = (int)bs_u1(bs);
            if ((sl->collocated_from_l0 && sl->num_ref_idx[0] > 1) ||
                (!sl->collocated_from_l0 && sl->num_ref_idx[1] > 1))
                sl->collocated_ref_idx = (int)bs_ue(bs);
            if (sl->collocated_ref_idx >= 16) return H265_ERR_CORRUPT;
        }
        if ((pps->weighted_pred && sl->type == SLICE_P) ||
            (pps->weighted_bipred && sl->type == SLICE_B)) {
            int rc = parse_pred_weight_table(bs, sl);
            if (rc) return rc;
        }
        sl->five_minus_max_num_merge_cand = (int)bs_ue(bs);
        if (sl->five_minus_max_num_merge_cand > 4) return H265_ERR_CORRUPT;
        sl->max_merge_cand = 5 - sl->five_minus_max_num_merge_cand;
    }

    sl->qp_delta = (int)bs_se(bs);
    sl->qp = pps->init_qp + sl->qp_delta;
    if (sl->qp < 0 || sl->qp > 51) return H265_ERR_CORRUPT;
    if (pps->slice_chroma_qp_offsets_present) {
        sl->cb_qp_offset = (int)bs_se(bs);
        sl->cr_qp_offset = (int)bs_se(bs);
    }
    sl->deblocking_filter_disabled = pps->pps_deblocking_filter_disabled;
    sl->beta_offset = pps->beta_offset;
    sl->tc_offset = pps->tc_offset;
    int override = 0;
    if (pps->deblocking_filter_override_enabled) override = (int)bs_u1(bs);
    if (override) {
        sl->deblocking_filter_disabled = (int)bs_u1(bs);
        if (!sl->deblocking_filter_disabled) {
            sl->beta_offset = (int)bs_se(bs) * 2;
            sl->tc_offset = (int)bs_se(bs) * 2;
        }
    }
    sl->loop_filter_across_slices = pps->loop_filter_across_slices;
    if (pps->loop_filter_across_slices &&
        (sl->sao_luma || sl->sao_chroma || !sl->deblocking_filter_disabled))
        sl->loop_filter_across_slices = (int)bs_u1(bs);

entry_points:
    if (pps->tiles_enabled || pps->entropy_coding_sync_enabled) {
        sl->num_entry_points = (int)bs_ue(bs);
        if (bs_error(bs) || sl->num_entry_points > H265_MAX_SUBSTREAMS)
            return H265_ERR_CORRUPT;
        if (sl->num_entry_points > 0) {
            int len = (int)bs_ue(bs) + 1;
            if (len < 1 || len > 32) return H265_ERR_CORRUPT;
            for (int i = 0; i < sl->num_entry_points; i++)
                sl->entry_point_offset[i] = bs_u(bs, len) + 1;
        }
    }
    if (pps->slice_segment_header_extension_present) {
        uint32_t n = bs_ue(bs);
        if (n > 256) return H265_ERR_CORRUPT;
        for (uint32_t i = 0; i < n; i++) bs_u(bs, 8);
    }
    /* byte_alignment(): a '1' bit then zeros to the byte boundary. */
    if (bs_u1(bs) != 1) return H265_ERR_CORRUPT;
    bs_align(bs);
    if (bs_error(bs)) return H265_ERR_CORRUPT;
    sl->header_bits = bs->bitpos;
    return H265_OK;
}
