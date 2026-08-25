/* c/lib/video/mpeg4_hdr.c -- the header layer: VOS / VO / VOL / GOV / VOP for
 * MPEG-4 Part 2, and the picture header for H.263 (which is also MPEG-4's
 * "short video header" mode).
 *
 * This is where every refusal in the decoder lives, and each one is BY NAME.
 * The rule the file is written to: a feature we do not implement must be
 * detected here, at the bit that announces it, and reported -- never
 * discovered three hundred macroblocks later as a corrupt bitstream, and never
 * concealed by decoding something else that happens not to crash. GMC is the
 * sharp case: a GMC stream parses perfectly all the way to the macroblock
 * layer and then silently predicts from the wrong samples, so it is refused
 * at vol_sprite_usage.
 *
 * The parts that are easy to get subtly wrong, recorded because they are:
 *
 *  - time_increment_bits comes from the VOL as ceil(log2(vop_time_increment_
 *    resolution - 1)), and EVERY VOP header's length depends on it. A stream
 *    whose VOL was lost decodes into noise rather than failing, so the marker
 *    bit that must follow vop_time_increment is checked and, if it is not
 *    there, the width is re-derived by search -- the same recovery the
 *    reference decoder does, and the reason it exists is that broken muxers
 *    dropping the VOL are common.
 *
 *  - THE GOP HEADER'S TIME CODE IS NOT DECORATIVE. It sets time_base, which
 *    feeds s->time, which feeds pp_time/pb_time, which are the DIVISORS in
 *    B-VOP direct mode. Skipping the GOP header as "just a timestamp" changes
 *    every direct-mode vector in the following group.
 *
 *  - vop_coded == 0 is a legal, empty picture (the N-VOP that packed-bitstream
 *    encoders emit). It is not an error and it is not a frame; the previous
 *    picture stays on screen.
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "mpeg4_int.h"

#define VOS_STARTCODE        0x1B0
#define USER_DATA_STARTCODE  0x1B2
#define GOP_STARTCODE        0x1B3
#define VISUAL_OBJ_STARTCODE 0x1B5
#define VOP_STARTCODE        0x1B6

#define SIMPLE_VO_TYPE        1
#define ADV_SIMPLE_VO_TYPE   17
#define CORE_STUDIO_VO_TYPE  14
#define SIMPLE_STUDIO_VO_TYPE 15

void m4_fail(m4ctx *s, const char *what)
{
    if (!s->err) s->err = what;
}

void m4_set_qscale(m4ctx *s, int qscale)
{
    if (qscale < 1) qscale = 1;
    else if (qscale > 31) qscale = 31;
    s->qscale        = qscale;
    s->chroma_qscale = s->chroma_qscale_table[qscale];
    /* A NULL scale table is H.263 without Advanced Intra Coding, where the
     * intra DC step is the constant 8 (H.263 5.4.1) rather than a function of
     * QUANT. The reference decoder spells that as a 32-entry array of eights;
     * a constant is what it is, and a generated array of one repeated value
     * would read as data that could be wrong. */
    if (s->y_dc_scale_table) {
        s->y_dc_scale = s->y_dc_scale_table[qscale];
        s->c_dc_scale = s->c_dc_scale_table[s->chroma_qscale];
    } else {
        s->y_dc_scale = s->c_dc_scale = 8;
    }
}

int m4_packet_prefix_len(const m4ctx *s)
{
    switch (s->pict_type) {
    case MPEG4_PICT_I: return 16;
    case MPEG4_PICT_P:
    case MPEG4_PICT_S: return s->f_code + 15;
    case MPEG4_PICT_B: {
        int m = s->f_code > s->b_code ? s->f_code : s->b_code;
        if (m < 2) m = 2;
        return m + 15;
    }
    default: return -1;
    }
}

/* --------------------------------------------------------------- geometry -- */
static void free_pic(m4pic *p)
{
    int i;
    free(p->buf);        p->buf = NULL;
    free(p->mb_type);    p->mb_type = NULL;
    free(p->qscale_table); p->qscale_table = NULL;
    free(p->mbskip_table); p->mbskip_table = NULL;
    for (i = 0; i < 2; i++) {
        free(p->motion_val_base[i]); p->motion_val_base[i] = NULL;
        p->motion_val[i] = NULL;
        free(p->ref_index[i]);       p->ref_index[i] = NULL;
    }
}

int m4_alloc_geometry(m4ctx *s)
{
    int i, y_size, c_size, yc_size, mb_array, b8_array;
    size_t plane;

    if (s->width <= 0 || s->height <= 0 ||
        s->width > 16384 || s->height > 16384) {
        m4_fail(s, "picture size out of range");
        return MPEG4_ERR_CORRUPT;
    }
    if (s->alloc_w == s->width && s->alloc_h == s->height)
        return MPEG4_OK;

    for (i = 0; i < 3; i++) free_pic(&s->pool[i]);
    free(s->dc_val_base);   s->dc_val_base = NULL;
    free(s->ac_val_base);   s->ac_val_base = NULL;
    free(s->mbintra_table); s->mbintra_table = NULL;
    free(s->cbp_table);     s->cbp_table = NULL;
    free(s->pred_dir_table); s->pred_dir_table = NULL;
    free(s->p_field_mv_table[0]); s->p_field_mv_table[0] = NULL;
    free(s->p_field_mv_table[1]); s->p_field_mv_table[1] = NULL;
    free(s->emu);           s->emu = NULL;
    s->cur = s->last = s->next = NULL;

    s->mb_width  = (s->width  + 15) / 16;
    s->mb_height = (s->height + 15) / 16;
    s->mb_num    = s->mb_width * s->mb_height;
    s->mb_stride = s->mb_width + 1;
    s->b8_stride = s->mb_width * 2 + 1;
    s->h_edge_pos = s->mb_width  * 16;
    s->v_edge_pos = s->mb_height * 16;
    /* The scratch buffer for out-of-picture reads is written at the picture's
     * own stride (the field paths depend on that), and one of those writes is
     * 17 bytes wide, so a one-macroblock-wide picture needs the padding. */
    s->linesize   = s->mb_width * 16 + 32;
    s->uvlinesize = s->mb_width * 8  + 16;

    y_size   = s->b8_stride * (2 * s->mb_height + 1);
    c_size   = s->mb_stride * (s->mb_height + 1);
    yc_size  = y_size + 2 * c_size;
    mb_array = s->mb_stride * (s->mb_height + 1);
    b8_array = s->b8_stride * (2 * s->mb_height + 2);

    plane = (size_t)s->linesize * (s->mb_height * 16 + 16)
          + (size_t)s->uvlinesize * (s->mb_height * 8 + 8) * 2;

    for (i = 0; i < 3; i++) {
        m4pic *p = &s->pool[i];
        int k;
        p->buf = calloc(1, plane);
        p->mb_type = calloc((size_t)mb_array, sizeof(int32_t));
        p->qscale_table = calloc((size_t)mb_array, 1);
        p->mbskip_table = calloc((size_t)mb_array, 1);
        if (!p->buf || !p->mb_type || !p->qscale_table || !p->mbskip_table)
            return MPEG4_ERR_OOM;
        p->data[0] = p->buf;
        p->data[1] = p->data[0] + (size_t)s->linesize * (s->mb_height * 16 + 16);
        p->data[2] = p->data[1] + (size_t)s->uvlinesize * (s->mb_height * 8 + 8);
        for (k = 0; k < 2; k++) {
            p->motion_val_base[k] = calloc((size_t)b8_array, sizeof(int16_t) * 2);
            p->ref_index[k] = calloc((size_t)mb_array * 4, 1);
            if (!p->motion_val_base[k] || !p->ref_index[k])
                return MPEG4_ERR_OOM;
            p->motion_val[k] = p->motion_val_base[k] + s->b8_stride + 1;
        }
        p->in_use = 0;
    }

    s->dc_val_base = calloc((size_t)yc_size, sizeof(int16_t));
    s->ac_val_base = calloc((size_t)yc_size, sizeof(int16_t) * 16);
    s->mbintra_table = calloc((size_t)mb_array, 1);
    s->cbp_table = calloc((size_t)mb_array, 1);
    s->pred_dir_table = calloc((size_t)mb_array, 1);
    s->p_field_mv_table[0] = calloc((size_t)mb_array, sizeof(int16_t) * 2);
    s->p_field_mv_table[1] = calloc((size_t)mb_array, sizeof(int16_t) * 2);
    s->emu_size = s->linesize * 24 + s->uvlinesize * 24 + 128;
    s->emu = calloc(1, (size_t)s->emu_size);
    if (!s->dc_val_base || !s->ac_val_base || !s->mbintra_table ||
        !s->cbp_table || !s->pred_dir_table || !s->emu ||
        !s->p_field_mv_table[0] || !s->p_field_mv_table[1])
        return MPEG4_ERR_OOM;

    for (i = 0; i < yc_size; i++) s->dc_val_base[i] = 1024;
    memset(s->mbintra_table, 1, (size_t)mb_array);

    s->dc_val = s->dc_val_base + s->b8_stride + 1;
    s->ac_val = s->ac_val_base + s->b8_stride + 1;

    s->block_wrap[0] = s->block_wrap[1] =
    s->block_wrap[2] = s->block_wrap[3] = s->b8_stride;
    s->block_wrap[4] = s->block_wrap[5] = s->mb_stride;

    s->alloc_w = s->width;
    s->alloc_h = s->height;
    return MPEG4_OK;
}

/* --------------------------------------------------------------- VOL ------ */
static int marker(m4ctx *s, m4bits *gb)
{
    if (!m4b_u1(gb)) { m4_fail(s, "missing marker bit in a header"); return 0; }
    return 1;
}

static int ilog2(unsigned v) { int n = 0; while (v >>= 1) n++; return n; }

static void load_default_matrices(m4ctx *s)
{
    int i;
    for (i = 0; i < 64; i++) {
        s->intra_matrix[i] = m4_default_intra_matrix[i];
        s->inter_matrix[i] = m4_default_inter_matrix[i];
    }
}

static int decode_vol(m4ctx *s, m4bits *gb)
{
    int vo_ver_id, aspect, width, height, i, v;

    m4b_skip(gb, 1);                 /* random_accessible_vol */
    s->vo_type = (int)m4b_u(gb, 8);

    if (s->vo_type == CORE_STUDIO_VO_TYPE || s->vo_type == SIMPLE_STUDIO_VO_TYPE) {
        m4_fail(s, "MPEG-4 Simple Studio profile (10/12-bit 4:2:2)");
        return MPEG4_ERR_UNSUPPORTED;
    }

    if (m4b_u1(gb)) {                /* is_object_layer_identifier */
        vo_ver_id = (int)m4b_u(gb, 4);
        m4b_skip(gb, 3);             /* video_object_layer_priority */
    } else {
        vo_ver_id = 1;
    }

    aspect = (int)m4b_u(gb, 4);
    if (aspect == 15) m4b_skip(gb, 16);   /* extended par */

    s->vol_control_parameters = (int)m4b_u1(gb);
    if (s->vol_control_parameters) {
        int chroma_format = (int)m4b_u(gb, 2);
        if (chroma_format != 1) {
            m4_fail(s, "chroma_format other than 4:2:0");
            return MPEG4_ERR_UNSUPPORTED;
        }
        s->low_delay = (int)m4b_u1(gb);
        if (m4b_u1(gb)) {            /* vbv_parameters */
            m4b_skip(gb, 15); if (!marker(s, gb)) return MPEG4_ERR_CORRUPT;
            m4b_skip(gb, 15); if (!marker(s, gb)) return MPEG4_ERR_CORRUPT;
            m4b_skip(gb, 15); if (!marker(s, gb)) return MPEG4_ERR_CORRUPT;
            m4b_skip(gb, 3);
            m4b_skip(gb, 11); if (!marker(s, gb)) return MPEG4_ERR_CORRUPT;
            m4b_skip(gb, 15); if (!marker(s, gb)) return MPEG4_ERR_CORRUPT;
        }
    } else if (s->picture_number == 0) {
        s->low_delay = (s->vo_type == SIMPLE_VO_TYPE ||
                        s->vo_type == ADV_SIMPLE_VO_TYPE);
    }

    s->shape = (int)m4b_u(gb, 2);
    if (s->shape != M4_RECT_SHAPE) {
        m4_fail(s, "non-rectangular / binary / grey video object shape");
        return MPEG4_ERR_UNSUPPORTED;
    }

    if (!marker(s, gb)) return MPEG4_ERR_CORRUPT;
    s->framerate_num = (int)m4b_u(gb, 16);   /* vop_time_increment_resolution */
    if (!s->framerate_num) { m4_fail(s, "vop_time_increment_resolution == 0");
                             return MPEG4_ERR_CORRUPT; }
    s->time_increment_bits = ilog2((unsigned)(s->framerate_num - 1)) + 1;
    if (s->time_increment_bits < 1) s->time_increment_bits = 1;

    if (!marker(s, gb)) return MPEG4_ERR_CORRUPT;
    if (m4b_u1(gb))                   /* fixed_vop_rate */
        s->framerate_den = (int)m4b_u(gb, s->time_increment_bits);
    else
        s->framerate_den = 1;
    s->t_frame = 0;

    if (!marker(s, gb)) return MPEG4_ERR_CORRUPT;
    width  = (int)m4b_u(gb, 13);
    if (!marker(s, gb)) return MPEG4_ERR_CORRUPT;
    height = (int)m4b_u(gb, 13);
    if (!marker(s, gb)) return MPEG4_ERR_CORRUPT;
    if (width && height) { s->width = width; s->height = height; }

    s->progressive_sequence = (int)(m4b_u1(gb) ^ 1);
    s->progressive_frame    = s->progressive_sequence;
    s->interlaced_dct       = 0;
    if (!m4b_u1(gb)) {                /* obmc_disable */
        m4_fail(s, "MPEG-4 OBMC");
        return MPEG4_ERR_UNSUPPORTED;
    }

    s->vol_sprite_usage = (vo_ver_id == 1) ? (int)m4b_u1(gb) : (int)m4b_u(gb, 2);
    if (s->vol_sprite_usage) {
        m4_fail(s, "sprite / GMC warping (vol_sprite_usage != 0)");
        return MPEG4_ERR_GMC;
    }

    if (m4b_u1(gb)) {                 /* not_8_bit */
        m4_fail(s, "N-bit video (bits_per_pixel != 8)");
        return MPEG4_ERR_UNSUPPORTED;
    }
    s->quant_precision = 5;

    s->mpeg_quant = (int)m4b_u1(gb);  /* quant_type */
    if (s->mpeg_quant) {
        load_default_matrices(s);
        if (m4b_u1(gb)) {             /* load_intra_quant_mat */
            int last = 0;
            for (i = 0; i < 64; i++) {
                if (m4b_left(gb) < 8) { m4_fail(s, "truncated intra quant matrix");
                                        return MPEG4_ERR_CORRUPT; }
                v = (int)m4b_u(gb, 8);
                if (v == 0) break;
                last = v;
                s->intra_matrix[m4_zigzag[i]] = (uint16_t)last;
            }
            for (; i < 64; i++) s->intra_matrix[m4_zigzag[i]] = (uint16_t)last;
        }
        if (m4b_u1(gb)) {             /* load_nonintra_quant_mat */
            int last = 0;
            for (i = 0; i < 64; i++) {
                if (m4b_left(gb) < 8) { m4_fail(s, "truncated inter quant matrix");
                                        return MPEG4_ERR_CORRUPT; }
                v = (int)m4b_u(gb, 8);
                if (v == 0) break;
                last = v;
                s->inter_matrix[m4_zigzag[i]] = (uint16_t)v;
            }
            for (; i < 64; i++) s->inter_matrix[m4_zigzag[i]] = (uint16_t)last;
        }
    }

    s->quarter_sample = (vo_ver_id != 1) ? (int)m4b_u1(gb) : 0;

    if (m4b_left(gb) < 4) { m4_fail(s, "VOL header truncated"); return MPEG4_ERR_CORRUPT; }

    s->cplx_i = s->cplx_p = s->cplx_b = 0;
    if (!m4b_u1(gb)) {                /* complexity_estimation_disable */
        int method = (int)m4b_u(gb, 2);
        if (method < 2) {
            if (!m4b_u1(gb)) {
                s->cplx_i += 8 * (int)m4b_u1(gb);
                s->cplx_i += 8 * (int)m4b_u1(gb);
                s->cplx_i += 8 * (int)m4b_u1(gb);
                s->cplx_i += 8 * (int)m4b_u1(gb);
                s->cplx_i += 8 * (int)m4b_u1(gb);
                s->cplx_i += 8 * (int)m4b_u1(gb);
            }
            if (!m4b_u1(gb)) {
                s->cplx_i += 8 * (int)m4b_u1(gb);
                s->cplx_p += 8 * (int)m4b_u1(gb);
                s->cplx_p += 8 * (int)m4b_u1(gb);
                s->cplx_i += 8 * (int)m4b_u1(gb);
            }
            if (!marker(s, gb)) return MPEG4_ERR_CORRUPT;
            if (!m4b_u1(gb)) {
                s->cplx_i += 8 * (int)m4b_u1(gb);
                s->cplx_i += 8 * (int)m4b_u1(gb);
                s->cplx_i += 8 * (int)m4b_u1(gb);
                s->cplx_i += 4 * (int)m4b_u1(gb);
            }
            if (!m4b_u1(gb)) {
                s->cplx_p += 8 * (int)m4b_u1(gb);
                s->cplx_p += 8 * (int)m4b_u1(gb);
                s->cplx_b += 8 * (int)m4b_u1(gb);
                s->cplx_p += 8 * (int)m4b_u1(gb);
                s->cplx_p += 8 * (int)m4b_u1(gb);
                s->cplx_p += 8 * (int)m4b_u1(gb);
            }
            if (!marker(s, gb)) return MPEG4_ERR_CORRUPT;
            if (method == 1) {
                s->cplx_i += 8 * (int)m4b_u1(gb);
                s->cplx_p += 8 * (int)m4b_u1(gb);
            }
        } else {
            m4_fail(s, "complexity estimation method > 1");
            return MPEG4_ERR_UNSUPPORTED;
        }
    }

    s->resync_marker     = (int)(m4b_u1(gb) ^ 1);   /* resync_marker_disable */
    s->data_partitioning = (int)m4b_u1(gb);
    s->rvlc = s->data_partitioning ? (int)m4b_u1(gb) : 0;

    if (vo_ver_id != 1) {
        s->new_pred = (int)m4b_u1(gb);
        if (s->new_pred) { m4_fail(s, "newpred"); return MPEG4_ERR_UNSUPPORTED; }
        if (m4b_u1(gb)) { m4_fail(s, "reduced-resolution VOP");
                          return MPEG4_ERR_UNSUPPORTED; }
    } else {
        s->new_pred = 0;
    }

    s->scalability = (int)m4b_u1(gb);
    if (s->scalability) {
        m4_fail(s, "scalable video object layer");
        return MPEG4_ERR_UNSUPPORTED;
    }

    s->have_vol = 1;
    s->short_header = 0;
    return m4_alloc_geometry(s);
}

/* --------------------------------------------------------------- VOP ------ */
static void init_direct_mv(m4ctx *s)
{
    const int tab_size = (int)(sizeof(s->direct_scale_mv[0]) / sizeof(int));
    const int tab_bias = tab_size / 2;
    int i;
    for (i = 0; i < tab_size; i++) {
        s->direct_scale_mv[0][i] = (i - tab_bias) * s->pb_time / s->pp_time;
        s->direct_scale_mv[1][i] = (i - tab_bias) * (s->pb_time - s->pp_time) /
                                   s->pp_time;
    }
}

static int rdiv(int64_t a, int64_t b)
{
    if (b == 0) return 0;
    return (int)((a > 0 ? a + b / 2 : a - b / 2) / b);
}

static int decode_vop(m4ctx *s, m4bits *gb, int *skipped)
{
    int time_incr = 0, time_increment;

    *skipped = 0;
    s->mcsel = 0;
    s->pict_type = (int)m4b_u(gb, 2) + 1;      /* I=1 P=2 B=3 S=4 */
    if (s->pict_type == MPEG4_PICT_S) {
        m4_fail(s, "S-VOP (sprite / GMC prediction)");
        return MPEG4_ERR_GMC;
    }
    if (s->pict_type == MPEG4_PICT_B && s->low_delay &&
        s->vol_control_parameters == 0)
        s->low_delay = 0;

    s->partitioned_frame = s->data_partitioning && s->pict_type != MPEG4_PICT_B;

    while (m4b_u1(gb)) {
        time_incr++;
        if (time_incr > 1 << 16) { m4_fail(s, "runaway modulo_time_base");
                                   return MPEG4_ERR_CORRUPT; }
    }
    if (!marker(s, gb)) return MPEG4_ERR_CORRUPT;

    if (s->time_increment_bits == 0 ||
        !(m4b_show(gb, s->time_increment_bits + 1) & 1)) {
        /* The marker that must follow vop_time_increment is not there, so the
         * width we were told is wrong -- almost always a lost VOL header.
         * Re-derive it from the bits that follow instead of decoding noise. */
        for (s->time_increment_bits = 1; s->time_increment_bits < 16;
             s->time_increment_bits++) {
            if (s->pict_type == MPEG4_PICT_P) {
                if ((m4b_show(gb, s->time_increment_bits + 6) & 0x37) == 0x30) break;
            } else if ((m4b_show(gb, s->time_increment_bits + 5) & 0x1F) == 0x18) break;
        }
    }
    time_increment = (int)m4b_u(gb, s->time_increment_bits);

    if (s->pict_type != MPEG4_PICT_B) {
        s->last_time_base = s->time_base;
        s->time_base     += time_incr;
        s->time = s->time_base * (int64_t)s->framerate_num + time_increment;
        s->pp_time         = (int)(s->time - s->last_non_b_time);
        s->last_non_b_time = s->time;
    } else {
        s->time = (s->last_time_base + time_incr) * (int64_t)s->framerate_num
                + time_increment;
        s->pb_time = s->pp_time - (int)(s->last_non_b_time - s->time);
        if (s->pp_time <= s->pb_time || s->pp_time <= s->pp_time - s->pb_time ||
            s->pp_time <= 0) {
            *skipped = 1;              /* messed-up order: drop this B-VOP */
            return MPEG4_OK;
        }
        init_direct_mv(s);
        if (s->t_frame == 0) s->t_frame = s->pb_time;
        if (s->t_frame == 0) s->t_frame = 1;
        s->pp_field_time = (rdiv(s->last_non_b_time, s->t_frame) -
                            rdiv(s->last_non_b_time - s->pp_time, s->t_frame)) * 2;
        s->pb_field_time = (rdiv(s->time, s->t_frame) -
                            rdiv(s->last_non_b_time - s->pp_time, s->t_frame)) * 2;
        if (s->pp_field_time <= s->pb_field_time || s->pb_field_time <= 1) {
            s->pb_field_time = 2;
            s->pp_field_time = 4;
            if (!s->progressive_sequence) { *skipped = 1; return MPEG4_OK; }
        }
    }

    if (!marker(s, gb)) return MPEG4_ERR_CORRUPT;

    if (!m4b_u1(gb)) {                  /* vop_coded */
        *skipped = 1;                   /* a legal, empty picture */
        return MPEG4_OK;
    }

    s->no_rounding = (s->pict_type == MPEG4_PICT_P) ? (int)m4b_u1(gb) : 0;

    m4b_skip(gb, s->cplx_i);
    if (s->pict_type != MPEG4_PICT_I) m4b_skip(gb, s->cplx_p);
    if (s->pict_type == MPEG4_PICT_B)  m4b_skip(gb, s->cplx_b);

    if (m4b_left(gb) < 3) { m4_fail(s, "VOP header truncated"); return MPEG4_ERR_CORRUPT; }
    s->intra_dc_threshold = m4_dc_threshold[m4b_u(gb, 3)];
    if (!s->progressive_sequence) {
        s->top_field_first = (int)m4b_u1(gb);
        s->alternate_scan  = (int)m4b_u1(gb);
    } else {
        s->alternate_scan = 0;
    }

    s->scan   = s->alternate_scan ? m4_scan_alt_v : m4_zigzag;
    s->scan_h = s->alternate_scan ? m4_scan_alt_v : m4_scan_alt_h;
    s->scan_v = m4_scan_alt_v;

    s->chroma_qscale = s->qscale = (int)m4b_u(gb, s->quant_precision);
    if (s->qscale == 0) { m4_fail(s, "vop_quant == 0"); return MPEG4_ERR_CORRUPT; }

    if (s->pict_type != MPEG4_PICT_I) {
        s->f_code = (int)m4b_u(gb, 3);
        if (s->f_code == 0) { m4_fail(s, "vop_fcode_forward == 0");
                              return MPEG4_ERR_CORRUPT; }
    } else {
        s->f_code = 1;
    }
    if (s->pict_type == MPEG4_PICT_B) {
        s->b_code = (int)m4b_u(gb, 3);
        if (s->b_code == 0) { m4_fail(s, "vop_fcode_backward == 0");
                              return MPEG4_ERR_CORRUPT; }
    } else {
        s->b_code = 1;
    }

    s->picture_number++;
    s->interlaced_dct   = 0;
    s->chroma_qscale_table = m4_identity_qscale;
    s->y_dc_scale_table = m4_y_dc_scale;
    s->c_dc_scale_table = m4_c_dc_scale;
    s->progressive_frame = s->progressive_sequence;
    return MPEG4_OK;
}

/* Walk start codes until a VOP header has been parsed. */
int m4_decode_vop(m4ctx *s, m4bits *gb, int *skipped)
{
    uint32_t startcode = 0xff;

    m4b_align(gb);
    *skipped = 0;

    for (;;) {
        int ret;
        if (m4b_count(gb) >= gb->nbits) { m4_fail(s, "no VOP start code in the unit");
                                          return MPEG4_ERR_CORRUPT; }
        startcode = ((startcode << 8) | m4b_u(gb, 8)) & 0xffffffffu;
        if ((startcode & 0xFFFFFF00u) != 0x100u) continue;

        if (startcode >= 0x120 && startcode <= 0x12F) {
            if ((ret = decode_vol(s, gb)) < 0) return ret;
        } else if (startcode == GOP_STARTCODE) {
            if (m4b_show(gb, 23)) {
                int hours   = (int)m4b_u(gb, 5);
                int minutes = (int)m4b_u(gb, 6);
                marker(s, gb);
                {
                    int seconds = (int)m4b_u(gb, 6);
                    s->time_base = seconds + 60 * (minutes + 60 * hours);
                }
                m4b_skip(gb, 2);        /* closed_gov, broken_link */
            }
        } else if (startcode == VOS_STARTCODE) {
            int profile = (int)m4b_u(gb, 4);
            int level   = (int)m4b_u(gb, 4);
            (void)level;
            if (profile == 4) {         /* Simple Studio */
                m4_fail(s, "MPEG-4 Simple Studio profile");
                return MPEG4_ERR_UNSUPPORTED;
            }
        } else if (startcode == VISUAL_OBJ_STARTCODE) {
            int vot;
            if (m4b_u1(gb)) m4b_skip(gb, 4 + 3);
            vot = (int)m4b_u(gb, 4);
            if (vot == 1 || vot == 2) {
                if (m4b_u1(gb)) {       /* video_signal_type */
                    m4b_skip(gb, 3 + 1);
                    if (m4b_u1(gb)) m4b_skip(gb, 24);
                }
            }
        } else if (startcode == USER_DATA_STARTCODE) {
            /* Deliberately not parsed. The reference decoder reads the encoder
             * ident out of here to enable bug workarounds for DivX 4/5 and old
             * Xvid builds; this decoder implements none of those workarounds,
             * so reading the ident could only make it pretend to. */
        } else if (startcode == VOP_STARTCODE) {
            break;
        }
        m4b_align(gb);
        startcode = 0xff;
    }

    if (!s->have_vol) { m4_fail(s, "VOP with no VOL header"); return MPEG4_ERR_CORRUPT; }
    return decode_vop(s, gb, skipped);
}

/* ------------------------------------------------- MPEG-4 video packet ---- */
int m4_video_packet_header(m4ctx *s)
{
    m4bits *gb = &s->gb;
    int mb_num_bits = ilog2((unsigned)(s->mb_num - 1)) + 1;
    int header_extension = 0, mb_num, len;

    if (m4b_count(gb) > gb->nbits - 20) return -1;

    for (len = 0; len < 32; len++)
        if (m4b_u1(gb)) break;
    if (len != m4_packet_prefix_len(s)) return -1;

    mb_num = (int)m4b_u(gb, mb_num_bits);
    if (mb_num >= s->mb_num || !mb_num) return -1;
    s->mb_x = mb_num % s->mb_width;
    s->mb_y = mb_num / s->mb_width;

    {
        int qscale = (int)m4b_u(gb, s->quant_precision);
        if (qscale) { s->chroma_qscale = s->qscale = qscale; }
    }
    header_extension = (int)m4b_u1(gb);

    if (header_extension) {
        while (m4b_u1(gb)) { if (m4b_over(gb)) return -1; }
        if (!m4b_u1(gb)) return -1;              /* marker */
        m4b_skip(gb, s->time_increment_bits);
        if (!m4b_u1(gb)) return -1;              /* marker */
        m4b_skip(gb, 2);                         /* vop_coding_type */
        m4b_skip(gb, 3);                         /* intra_dc_vlc_thr */
        if (s->pict_type != MPEG4_PICT_I) m4b_skip(gb, 3);   /* fcode_forward */
        if (s->pict_type == MPEG4_PICT_B)  m4b_skip(gb, 3);  /* fcode_backward */
    }
    return 0;
}

/* --------------------------------------------------------- H.263 header --- */
int m4_gob_header(m4ctx *s)
{
    m4bits *gb = &s->gb;
    unsigned gob_number;
    int left;

    if (m4b_show(gb, 16)) return -1;
    m4b_skip(gb, 16);
    left = m4b_left(gb);
    if (left > 32) left = 32;
    for (; left > 13; left--)
        if (m4b_u1(gb)) break;
    if (left <= 13) return -1;

    if (s->slice_structured) return -1;      /* refused in the picture header */

    gob_number = m4b_u(gb, 5);
    s->mb_x = 0;
    s->mb_y = s->gob_index * (int)gob_number;
    m4b_skip(gb, 2);                          /* GFID */
    s->qscale = (int)m4b_u(gb, 5);

    if (s->mb_y >= s->mb_height) return -1;
    if (s->qscale == 0) return -1;
    return 0;
}

int m4_decode_h263_picture(m4ctx *s, m4bits *gb)
{
    int format, width, height, i, ret;
    uint32_t startcode;

    m4b_align(gb);
    startcode = m4b_u(gb, 22 - 8);
    for (i = m4b_left(gb); i > 24; i -= 8) {
        startcode = ((startcode << 8) | m4b_u(gb, 8)) & 0x003FFFFF;
        if (startcode == 0x20) break;
    }
    if (startcode != 0x20) { m4_fail(s, "no H.263 picture start code");
                             return MPEG4_ERR_CORRUPT; }

    i = (int)m4b_u(gb, 8);                    /* temporal reference */
    i -= (i - (s->picture_number & 0xFF) + 128) & ~0xFF;
    s->picture_number = (s->picture_number & ~0xFF) + i;

    if (!marker(s, gb)) return MPEG4_ERR_CORRUPT;
    if (m4b_u1(gb)) { m4_fail(s, "bad H.263 id bit"); return MPEG4_ERR_CORRUPT; }
    m4b_skip(gb, 3);                          /* split screen / camera / freeze */

    format = (int)m4b_u(gb, 3);
    s->umvplus = s->h263_aic = s->obmc = s->alt_inter_vlc = 0;
    s->modified_quant = s->loop_filter = s->custom_pcf = s->slice_structured = 0;
    s->h263_long_vectors = s->pb_frame = 0;
    s->chroma_qscale_table = m4_h263_chroma_qscale;

    if (format != 7 && format != 6) {
        s->h263_plus = 0;
        width  = m4_h263_format[format * 2 + 0];
        height = m4_h263_format[format * 2 + 1];
        if (!width) { m4_fail(s, "reserved H.263 source format");
                      return MPEG4_ERR_CORRUPT; }

        s->pict_type = MPEG4_PICT_I + (int)m4b_u1(gb);
        s->h263_long_vectors = (int)m4b_u1(gb);
        if (m4b_u1(gb)) { m4_fail(s, "H.263 Annex D syntax-based arithmetic coding");
                          return MPEG4_ERR_UNSUPPORTED; }
        s->obmc = (int)m4b_u1(gb);
        if (s->obmc) { m4_fail(s, "H.263 Annex F advanced prediction (OBMC)");
                       return MPEG4_ERR_UNSUPPORTED; }
        s->pb_frame = (int)m4b_u1(gb);
        if (s->pb_frame) { m4_fail(s, "H.263 Annex G PB-frames");
                           return MPEG4_ERR_UNSUPPORTED; }
        s->chroma_qscale = s->qscale = (int)m4b_u(gb, 5);
        m4b_skip(gb, 1);                      /* CPM */
        s->width = width; s->height = height;
        s->no_rounding = 0;
    } else {
        int ufep;
        s->h263_plus = 1;
        ufep = (int)m4b_u(gb, 3);
        if (ufep == 1) {
            format = (int)m4b_u(gb, 3);
            s->custom_pcf = (int)m4b_u1(gb);
            s->umvplus    = (int)m4b_u1(gb);
            if (m4b_u1(gb)) { m4_fail(s, "H.263 Annex D syntax-based arithmetic coding");
                              return MPEG4_ERR_UNSUPPORTED; }
            s->obmc = (int)m4b_u1(gb);
            if (s->obmc) { m4_fail(s, "H.263 Annex F advanced prediction (OBMC)");
                           return MPEG4_ERR_UNSUPPORTED; }
            s->h263_aic = (int)m4b_u1(gb);
            s->loop_filter = (int)m4b_u1(gb);
            s->slice_structured = (int)m4b_u1(gb);
            if (s->slice_structured) { m4_fail(s, "H.263 Annex K slice-structured mode");
                                       return MPEG4_ERR_UNSUPPORTED; }
            if (m4b_u1(gb)) { m4_fail(s, "H.263 Annex N reference picture selection");
                              return MPEG4_ERR_UNSUPPORTED; }
            if (m4b_u1(gb)) { m4_fail(s, "H.263 Annex R independent segment decoding");
                              return MPEG4_ERR_UNSUPPORTED; }
            s->alt_inter_vlc  = (int)m4b_u1(gb);
            s->modified_quant = (int)m4b_u1(gb);
            m4b_skip(gb, 1);                  /* prevent start code emulation */
            m4b_skip(gb, 3);                  /* reserved */
        } else if (ufep != 0) {
            m4_fail(s, "reserved H.263 UFEP value"); return MPEG4_ERR_CORRUPT;
        }

        switch (m4b_u(gb, 3)) {               /* MPPTYPE picture type */
        case 0: s->pict_type = MPEG4_PICT_I; break;
        case 1: s->pict_type = MPEG4_PICT_P; break;
        case 2: m4_fail(s, "H.263 Annex M improved PB-frames");
                return MPEG4_ERR_UNSUPPORTED;
        case 3: m4_fail(s, "H.263 Annex O B-pictures");
                return MPEG4_ERR_UNSUPPORTED;
        case 7: s->pict_type = MPEG4_PICT_I; break;
        default: m4_fail(s, "reserved H.263 picture type"); return MPEG4_ERR_CORRUPT;
        }
        m4b_skip(gb, 2);
        s->no_rounding = (int)m4b_u1(gb);
        m4b_skip(gb, 4);

        if (ufep) {
            if (format == 6) {                /* custom picture format */
                int par = (int)m4b_u(gb, 4);  /* pixel aspect ratio code */
                width = ((int)m4b_u(gb, 9) + 1) * 4;
                if (!marker(s, gb)) return MPEG4_ERR_CORRUPT;
                height = (int)m4b_u(gb, 9) * 4;
                if (par == 15) m4b_skip(gb, 16);   /* extended PAR */
            } else {
                width  = m4_h263_format[format * 2 + 0];
                height = m4_h263_format[format * 2 + 1];
            }
            if (!width || !height) { m4_fail(s, "H.263 picture size 0");
                                     return MPEG4_ERR_CORRUPT; }
            s->width = width; s->height = height;
            if (s->custom_pcf) m4b_skip(gb, 1 + 7);
        }
        if (s->custom_pcf) m4b_skip(gb, 2);   /* extended temporal reference */

        if (ufep) {
            if (s->umvplus) { if (m4b_u1(gb) == 0) m4b_skip(gb, 1); }
        }
        s->qscale = s->chroma_qscale = (int)m4b_u(gb, 5);
    }

    if ((ret = m4_alloc_geometry(s)) < 0) return ret;
    s->gob_index = s->height <= 400 ? 1 : (s->height <= 800 ? 2 : 4);

    if (m4b_left(gb) < 0) { m4_fail(s, "H.263 header truncated"); return MPEG4_ERR_CORRUPT; }

    if (s->pict_type != MPEG4_PICT_B) {
        s->time = s->picture_number;
        s->pp_time = (int)(s->time - s->last_non_b_time);
        s->last_non_b_time = s->time;
    }

    /* PEI: a stop bit followed by 8 data bits, repeated */
    for (;;) {
        if (m4b_left(gb) < 1) { m4_fail(s, "H.263 header truncated in PEI");
                                return MPEG4_ERR_CORRUPT; }
        if (!m4b_u1(gb)) break;
        m4b_skip(gb, 8);
    }

    s->f_code = 1;
    s->b_code = 1;
    s->short_header = 1;
    s->quarter_sample = 0;
    s->mpeg_quant = 0;
    s->data_partitioning = 0;
    s->partitioned_frame = 0;
    s->rvlc = 0;
    s->resync_marker = 1;
    s->progressive_sequence = 1;
    s->progressive_frame = 1;
    s->interlaced_dct = 0;
    s->alternate_scan = 0;
    s->low_delay = 1;
    s->intra_dc_threshold = 99;
    s->scan   = m4_zigzag;
    s->scan_h = m4_scan_alt_h;
    s->scan_v = m4_scan_alt_v;
    if (s->h263_aic) {
        s->y_dc_scale_table = s->c_dc_scale_table = m4_aic_dc_scale;
    } else {
        s->y_dc_scale_table = s->c_dc_scale_table = NULL;   /* the constant 8 */
    }
    if (!s->modified_quant) s->chroma_qscale_table = m4_identity_qscale;
    return MPEG4_OK;
}
