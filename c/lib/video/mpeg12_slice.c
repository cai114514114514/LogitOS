/* c/lib/video/mpeg12_slice.c -- slice(), macroblock(), block() and the
 * reconstruction, for both MPEG-1 (ISO/IEC 11172-2) and MPEG-2 (13818-2).
 *
 * The two standards share this syntax almost exactly, which is why they share
 * a file; every place they differ is marked MPEG-1 in a comment, and there are
 * only six of them:
 *   - the dequantiser oddifies the level ((v-1)|1) and there is no mismatch
 *     control on coefficient 63;
 *   - the escape carries an 8-bit level with a second escape for |level| > 127,
 *     where MPEG-2 carries 12 bits;
 *   - intra_vlc_format does not exist, so Table B-14 decodes intra blocks too;
 *   - motion vectors may be full-pel (a picture-header flag doubles them);
 *   - there is no picture_coding_extension, so every picture is a progressive
 *     frame picture with 8-bit DC and the zigzag scan;
 *   - D-pictures exist, and are refused by name in mpeg12.c.
 *
 * Nothing here conceals an error. A code that is not in the table, a
 * macroblock address past the end of the picture, a coefficient index past 63:
 * each returns MPEG12_ERR_CORRUPT and the picture stops. A decoder that
 * guesses produces a picture that looks decoded and is not, which is worse
 * than no picture at all for anything that has to trust the output.
 */
#include <string.h>
#include "mpeg12_int.h"

/* ------------------------------------------------------------------ VLC -- */
#define vlc_get m12_vlc_get            /* the reader lives in mpeg12_int.h */

#define MV_ERR 0x40000000

/* 7.6.3.1. `pred` is the predictor already scaled into this vector's units;
 * the caller owns the field-in-frame halving. The final modulo is the
 * standard's "if (vector < low) vector += range" written as a sign extension
 * to 5 + r_size bits, which is the same map. */
static int decode_motion(m12br *b, int fcode, int pred)
{
    int code = vlc_get(b, m12_vlc_motion);
    int sign, val, shift, bits;

    if (code < 0) return MV_ERR;
    if (code == 0) return pred;
    if (fcode < 1 || fcode > 9) return MV_ERR;

    sign  = (int)m12_u1(b);
    shift = fcode - 1;
    val   = code;
    if (shift) {
        val  = (val - 1) << shift;
        val |= (int)m12_u(b, shift);
        val++;
    }
    if (sign) val = -val;
    val += pred;
    bits = 5 + shift;
    return (int)((uint32_t)val << (32 - bits)) >> (32 - bits);
}

/* Table B-11: '0' -> 0, '10' -> +1, '11' -> -1. */
static int decode_dmvector(m12br *b)
{
    if (!m12_u1(b)) return 0;
    return 1 - 2 * (int)m12_u1(b);
}

#define DC_ERR 0x40000000

static int decode_dc_diff(m12br *b, int chroma)
{
    int size = vlc_get(b, chroma ? m12_vlc_dc_chroma : m12_vlc_dc_lum);
    int v;
    if (size < 0) return DC_ERR;
    if (size == 0) return 0;
    v = (int)m12_u(b, size);
    if (!(v & (1 << (size - 1)))) v = v - (1 << size) + 1;
    return v;
}

/* ---------------------------------------------------------------- block -- */
/* 7.4.2.3 saturation. On a conforming stream this never fires -- an encoder
 * that produced a coefficient outside [-2048, 2047] would be producing a
 * stream no decoder reconstructs the same way -- so the counter is the useful
 * output: "we clipped" and "we agreed with the oracle" cannot both be
 * uninteresting. */
static inline int16_t sat(mpeg12dec *d, int v)
{
    if (v > 2047)  { d->cen.coeff_saturated++; return 2047; }
    if (v < -2048) { d->cen.coeff_saturated++; return -2048; }
    return (int16_t)v;
}

static int decode_block(mpeg12dec *d, m12br *b, int n, int intra)
{
    int16_t *blk = d->block[n];
    const uint8_t *scan = d->scan;
    const uint8_t *qm;
    const m12_vlc_e *tab;
    int i, j, sym, run, level, v, mismatch, qs = d->qscale;
    int m2 = d->is_mpeg2;

    memset(blk, 0, sizeof d->block[0]);

    if (intra) {
        int comp = (n < 4) ? 0 : ((n & 1) + 1);
        int diff = decode_dc_diff(b, comp != 0);
        if (diff == DC_ERR) return MPEG12_ERR_CORRUPT;
        qm = (n < 4) ? d->qm_intra : d->qm_cintra;
        d->last_dc[comp] += diff;
        blk[0] = (int16_t)(d->last_dc[comp] * (1 << (3 - d->intra_dc_precision)));
        mismatch = blk[0] ^ 1;
        i = 0;
        tab = (m2 && d->intra_vlc_format) ? m12_vlc_coef_b15 : m12_vlc_coef_b14;
        if (tab == m12_vlc_coef_b15) d->cen.blocks_intra_vlc++;
    } else {
        qm = (n < 4) ? d->qm_inter : d->qm_cinter;
        mismatch = 1;
        i = -1;
        tab = m12_vlc_coef_b14;
        /* THE FIRST COEFFICIENT OF A NON-INTRA BLOCK IS NOT IN THE TABLE.
         * '10' is End of Block everywhere else, and an empty non-intra block
         * cannot be coded (its coded_block_pattern bit would be 0), so at
         * position 0 the leading '1' means run 0, level 1 and the next bit is
         * the sign. Decoding it from the table instead yields End of Block for
         * half of these blocks and a silently empty residual. */
        if (m12_peek(b, 1)) {
            v = ((2 * 1 + 1) * qs * qm[0]) >> 5;
            if (!m2) v = (v - 1) | 1;
            m12_skip(b, 1);
            if (m12_u1(b)) v = -v;
            blk[0] = sat(d, v);
            mismatch ^= v;
            i = 0;
        }
    }

    for (;;) {
        sym = vlc_get(b, tab);
        if (sym < 0) return MPEG12_ERR_CORRUPT;
        if (sym == M12_COEF_EOB) break;

        if (sym == M12_COEF_ESCAPE) {
            d->cen.escapes++;
            run = (int)m12_u(b, 6) + 1;
            if (m2) {
                level = m12_s(b, 12);
            } else {
                /* MPEG-1's two-stage escape (11172-2 2.4.3.7): eight bits, and
                 * the two values a level may never take -- 0 and -128 -- are
                 * spent as the flag for eight more. Counted separately,
                 * because a corpus that never reaches the second stage leaves
                 * half of this branch unmeasured and looks from the outside
                 * exactly like one that does. */
                level = m12_s(b, 8);
                if (level == -128) {
                    level = (int)m12_u(b, 8) - 256;
                    d->cen.escapes_mpeg1_second++;
                } else if (level == 0) {
                    level = (int)m12_u(b, 8);
                    d->cen.escapes_mpeg1_second++;
                }
            }
        } else {
            run   = m12_rl_run[sym] + 1;
            level = m12_rl_level[sym];
            if (m12_u1(b)) level = -level;
        }

        i += run;
        if (i > 63) return MPEG12_ERR_CORRUPT;
        j = scan[i];

        {
            int mag = level < 0 ? -level : level;
            if (intra) v = (mag * qs * qm[j]) >> 4;
            else       v = ((mag * 2 + 1) * qs * qm[j]) >> 5;
            if (!m2) v = (v - 1) | 1;      /* MPEG-1 oddification, 2.4.4.1 */
            if (level < 0) v = -v;
        }

        mismatch ^= v;
        blk[j] = sat(d, v);
        if (b->over) return MPEG12_ERR_CORRUPT;
    }

    /* 7.4.4 mismatch control: MPEG-2 only. XOR-ing every reconstructed level
     * into `mismatch` computes the parity of their sum, which is what the
     * standard's "if the sum is even" asks for. */
    if (m2) blk[63] ^= (int16_t)(mismatch & 1);
    if (b->over) return MPEG12_ERR_CORRUPT;
    return 0;
}

/* ------------------------------------------------------ reconstruction -- */
/* Which reference frame a prediction of `parity` comes from. Frame pictures
 * and B pictures: the anchors. The one interesting case is the SECOND field
 * of a P (or I-with-concealment) field picture asking for the opposite
 * parity: that field is the FIRST field of the picture being decoded, not a
 * field of the previous anchor -- "the two most recently decoded reference
 * fields" includes the one three macroblock rows above the cursor. */
static m12pic *ref_for(mpeg12dec *d, int dir, int parity)
{
    if (d->coding_type == MPEG12_PICT_B)
        return dir ? d->ref_new : d->ref_old;
    if (d->picture_structure != M12_FRAME && d->second_field &&
        parity != (d->picture_structure - 1))
        return d->cur;
    return d->ref_new;
}

static void mc_one(mpeg12dec *d, m12pic *ref, int parity, int field,
                   uint8_t *dy, uint8_t *du, uint8_t *dv,
                   int row_y, int mvx, int mvy, int h, int avg)
{
    int sy, sc, ph, phc, sx, syy, mx, my, cx, cy;
    const uint8_t *py, *pu, *pv;

    if (!ref) return;                     /* missing reference: leave as is */

    sy  = ref->stride_y << field;
    sc  = ref->stride_c << field;
    py  = ref->y + (parity ? ref->stride_y : 0);
    pu  = ref->u + (parity ? ref->stride_c : 0);
    pv  = ref->v + (parity ? ref->stride_c : 0);
    ph  = d->coded_h >> field;
    phc = (d->coded_h >> 1) >> field;

    sx  = d->mb_x * 16 + (mvx >> 1);
    syy = row_y + (mvy >> 1);

    /* 7.6.3.7: for 4:2:0 the chroma vector is the luma vector divided by two,
     * truncated TOWARD ZERO -- so -3 becomes -1, i.e. minus half a chroma
     * sample, and the split into an integer offset (>>1, which floors) and a
     * half-pel flag (&1) reproduces exactly that. Using >>1 for the division
     * as well moves every odd negative vector a whole chroma sample. */
    mx = mvx / 2;
    my = mvy / 2;
    cx = d->mb_x * 8 + (mx >> 1);
    cy = (row_y >> 1) + (my >> 1);

    d->cen.mv_clamped += m12_pred_edge(dy, py, sy, sx, syy, d->coded_w, ph,
                                  16, h, mvx & 1, mvy & 1, avg);
    d->cen.mv_clamped += m12_pred_edge(du, pu, sc, cx, cy, d->coded_w >> 1, phc,
                                  8, h >> 1, mx & 1, my & 1, avg);
    d->cen.mv_clamped += m12_pred_edge(dv, pv, sc, cx, cy, d->coded_w >> 1, phc,
                                  8, h >> 1, mx & 1, my & 1, avg);
}

static void predict_dir(mpeg12dec *d, int dir, uint8_t *dy, uint8_t *du,
                        uint8_t *dv, int avg)
{
    int frame_pic = (d->picture_structure == M12_FRAME);
    int f, p;

    switch (d->mv_type) {
    case M12_MV_16X16:
        mc_one(d, ref_for(d, dir, 0), 0, 0, dy, du, dv,
               d->mb_y * 16, d->mv[dir][0][0], d->mv[dir][0][1], 16, avg);
        break;

    case M12_MV_FIELD:
        if (frame_pic) {
            for (f = 0; f < 2; f++) {
                p = d->field_select[dir][f];
                mc_one(d, ref_for(d, dir, p), p, 1,
                       dy + (long)f * d->ps_y, du + (long)f * d->ps_c,
                       dv + (long)f * d->ps_c, d->mb_y * 8,
                       d->mv[dir][f][0], d->mv[dir][f][1], 8, avg);
            }
        } else {
            p = d->field_select[dir][0];
            mc_one(d, ref_for(d, dir, p), p, 1, dy, du, dv,
                   d->mb_y * 16, d->mv[dir][0][0], d->mv[dir][0][1], 16, avg);
        }
        break;

    case M12_MV_16X8:                      /* field pictures only */
        for (f = 0; f < 2; f++) {
            p = d->field_select[dir][f];
            mc_one(d, ref_for(d, dir, p), p, 1,
                   dy + (long)f * 8 * d->ps_y, du + (long)f * 4 * d->ps_c,
                   dv + (long)f * 4 * d->ps_c, d->mb_y * 16 + f * 8,
                   d->mv[dir][f][0], d->mv[dir][f][1], 8, avg);
        }
        break;

    case M12_MV_DMV:
#ifdef MPEG12_NO_DUALPRIME
        /* NEGATIVE CONTROL, and it is the PLAUSIBLE half-done version rather
         * than the absent one: the syntax is still parsed, the coded vector is
         * still used, every macroblock still gets a prediction and the picture
         * still looks like a picture. What is removed is the SECOND
         * prediction -- dual prime averages a same-parity prediction with an
         * opposite-parity one made from the DERIVED vector, and a decoder that
         * forms only the first is what somebody writes on the way to finishing
         * the feature. It must redden exactly the cases whose census reports
         * mv_dualprime > 0. */
        if (frame_pic) {
            for (f = 0; f < 2; f++)
                mc_one(d, ref_for(d, dir, f), f, 1,
                       dy + (long)f * d->ps_y, du + (long)f * d->ps_c,
                       dv + (long)f * d->ps_c, d->mb_y * 8,
                       d->mv[dir][f][0], d->mv[dir][f][1], 8, avg);
        } else {
            p = d->picture_structure - 1;
            mc_one(d, ref_for(d, dir, p), p, 1, dy, du, dv,
                   d->mb_y * 16, d->mv[dir][0][0], d->mv[dir][0][1], 16, avg);
        }
        break;
#else
      { int pass;
        if (frame_pic) {
            /* pass 0 predicts each field from the SAME parity with the coded
             * vector, pass 1 from the OPPOSITE parity with the derived one,
             * and the two are averaged. */
            for (pass = 0; pass < 2; pass++)
                for (f = 0; f < 2; f++) {
                    p = f ^ pass;
                    mc_one(d, ref_for(d, dir, p), p, 1,
                           dy + (long)f * d->ps_y, du + (long)f * d->ps_c,
                           dv + (long)f * d->ps_c, d->mb_y * 8,
                           d->mv[dir][2 * pass + f][0],
                           d->mv[dir][2 * pass + f][1], 8, avg || pass);
                }
        } else {
            int cur_par = d->picture_structure - 1;
            for (pass = 0; pass < 2; pass++) {
                p = pass ? !cur_par : cur_par;
                mc_one(d, ref_for(d, dir, p), p, 1, dy, du, dv,
                       d->mb_y * 16, d->mv[dir][2 * pass][0],
                       d->mv[dir][2 * pass][1], 16, avg || pass);
            }
        }
      }
        break;
#endif
    }
}

static void census(mpeg12dec *d)
{
    d->cen.mb_total++;
    if (d->mb_intra) { d->cen.mb_intra++; }
    else switch (d->mv_type) {
        case M12_MV_16X16: d->cen.mv_frame++; break;
        case M12_MV_FIELD: d->cen.mv_field++; break;
        case M12_MV_16X8:  d->cen.mv_16x8++; break;
        case M12_MV_DMV:   d->cen.mv_dualprime++; break;
    }
    if (d->dct_type) d->cen.mb_field_dct++;
}

static void reconstruct(mpeg12dec *d)
{
    uint8_t *dy = d->pic_y + (long)d->mb_y * 16 * d->ps_y + d->mb_x * 16;
    uint8_t *du = d->pic_u + (long)d->mb_y * 8 * d->ps_c + d->mb_x * 8;
    uint8_t *dv = d->pic_v + (long)d->mb_y * 8 * d->ps_c + d->mb_x * 8;
    int i, avg = 0, st;
    uint8_t *p;

    census(d);
    if (!d->mb_intra) {
        for (i = 0; i < 2; i++)
            if (d->mv_dir & (1 << i)) { predict_dir(d, i, dy, du, dv, avg); avg = 1; }
    }

    for (i = 0; i < 4; i++) {
        if (!d->mb_intra && !(d->cbp & (1 << (5 - i)))) continue;
        /* 6.1.3: with dct_type == 1 a luma block holds the lines of ONE field
         * of the macroblock, so it is written at twice the stride starting on
         * the field's own line. */
        if (d->dct_type) { p = dy + (long)(i >> 1) * d->ps_y + (i & 1) * 8; st = d->ps_y * 2; }
        else             { p = dy + (long)(i >> 1) * 8 * d->ps_y + (i & 1) * 8; st = d->ps_y; }
        if (d->mb_intra) m12_idct_put(p, st, d->block[i]);
        else             m12_idct_add(p, st, d->block[i]);
    }
    for (i = 4; i < 6; i++) {
        if (!d->mb_intra && !(d->cbp & (1 << (5 - i)))) continue;
        p = (i == 4) ? du : dv;
        if (d->mb_intra) m12_idct_put(p, d->ps_c, d->block[i]);
        else             m12_idct_add(p, d->ps_c, d->block[i]);
    }
}

/* -------------------------------------------------------- macroblock -- */
static int get_qscale(mpeg12dec *d, m12br *b)
{
    int c = (int)m12_u(b, 5);
    if (c == 0) return 0;
    return d->q_scale_type ? m12_non_linear_qscale[c] : (c << 1);
}

static void reset_dc(mpeg12dec *d)
{
    d->last_dc[0] = d->last_dc[1] = d->last_dc[2] = 128 << d->intra_dc_precision;
}

static void setup_skip(mpeg12dec *d)
{
    int par = (d->picture_structure - 1) & 1;

    d->cen.mb_skipped++;
    d->mb_intra = 0;
    d->cbp = 0;
    d->dct_type = 0;
    reset_dc(d);
    d->mv_type = (d->picture_structure == M12_FRAME) ? M12_MV_16X16 : M12_MV_FIELD;

    if (d->coding_type == MPEG12_PICT_P) {
        /* 7.6.6: a skipped macroblock in a P picture has a zero vector, and
         * the predictors are reset -- not carried. */
        d->mv_dir = 1;
        d->mv[0][0][0] = d->mv[0][0][1] = 0;
        memset(d->last_mv, 0, sizeof d->last_mv);
        d->field_select[0][0] = par;
    } else {
        /* ...and in a B picture it reuses the previous macroblock's vectors
         * AND its prediction directions, which is why mv_dir is left alone. */
        d->mv[0][0][0] = d->last_mv[0][0][0];
        d->mv[0][0][1] = d->last_mv[0][0][1];
        d->mv[1][0][0] = d->last_mv[1][0][0];
        d->mv[1][0][1] = d->last_mv[1][0][1];
        d->field_select[0][0] = d->field_select[1][0] = par;
    }
}

static int decode_mb(mpeg12dec *d, m12br *b)
{
    int frame_pic = (d->picture_structure == M12_FRAME);
    int mb_type, motion_type, i, j, k, val, r;

    switch (d->coding_type) {
    case MPEG12_PICT_I:
        if (m12_u1(b))      mb_type = M12_MB_INTRA;
        else if (m12_u1(b)) mb_type = M12_MB_QUANT | M12_MB_INTRA;
        else                return MPEG12_ERR_CORRUPT;
        break;
    case MPEG12_PICT_P: mb_type = vlc_get(b, m12_vlc_mb_ptype); break;
    case MPEG12_PICT_B: mb_type = vlc_get(b, m12_vlc_mb_btype); break;
    default: return MPEG12_ERR_UNSUPPORTED;
    }
    if (mb_type < 0) return MPEG12_ERR_CORRUPT;

    d->dct_type = 0;

    if (mb_type & M12_MB_INTRA) {
        d->mb_intra = 1;
        d->cbp = 0x3F;
        if (frame_pic && !d->frame_pred_frame_dct) d->dct_type = (int)m12_u1(b);
        if (mb_type & M12_MB_QUANT) {
            d->qscale = get_qscale(d, b);
            if (!d->qscale) return MPEG12_ERR_CORRUPT;
        }
        if (d->concealment_mv) {
            /* Parsed, and only parsed: concealment vectors describe how to
             * hide a LOST macroblock, and this decoder loses none -- it
             * reports the stream instead. The predictors they update are
             * real, though, so the next macroblock's vectors depend on them. */
            if (!frame_pic) m12_skip(b, 1);
            val = decode_motion(b, d->f_code[0][0], d->last_mv[0][0][0]);
            if (val == MV_ERR) return MPEG12_ERR_CORRUPT;
            d->last_mv[0][0][0] = d->last_mv[0][1][0] = val;
            val = decode_motion(b, d->f_code[0][1], d->last_mv[0][0][1]);
            if (val == MV_ERR) return MPEG12_ERR_CORRUPT;
            d->last_mv[0][0][1] = d->last_mv[0][1][1] = val;
            if (!m12_u1(b)) return MPEG12_ERR_CORRUPT;   /* marker_bit */
        } else {
            memset(d->last_mv, 0, sizeof d->last_mv);
        }
        for (i = 0; i < 6; i++) {
            r = decode_block(d, b, i, 1);
            if (r < 0) return r;
        }
        reconstruct(d);
        return 0;
    }

    d->mb_intra = 0;
    reset_dc(d);
    d->mv_dir = ((mb_type & M12_MB_FORWARD) ? 1 : 0) |
                ((mb_type & M12_MB_BACKWARD) ? 2 : 0);

    if (!d->mv_dir) {
        /* Table B-3's "pattern only": a P macroblock with a coded residual and
         * NO vector, which means a zero vector -- and resets the predictors. */
        d->mv_dir = 1;
        if (frame_pic) {
            if (!d->frame_pred_frame_dct) d->dct_type = (int)m12_u1(b);
            d->mv_type = M12_MV_16X16;
        } else {
            d->mv_type = M12_MV_FIELD;
            d->field_select[0][0] = d->picture_structure - 1;
        }
        if (mb_type & M12_MB_QUANT) {
            d->qscale = get_qscale(d, b);
            if (!d->qscale) return MPEG12_ERR_CORRUPT;
        }
        memset(d->last_mv, 0, sizeof d->last_mv);
        d->mv[0][0][0] = d->mv[0][0][1] = 0;
    } else {
        if (frame_pic && d->frame_pred_frame_dct) {
            motion_type = 2;               /* frame-based, not coded */
        } else {
            motion_type = (int)m12_u(b, 2);
            if (frame_pic && (mb_type & M12_MB_PATTERN))
                d->dct_type = (int)m12_u1(b);
        }
        if (mb_type & M12_MB_QUANT) {
            d->qscale = get_qscale(d, b);
            if (!d->qscale) return MPEG12_ERR_CORRUPT;
        }

        switch (motion_type) {
        case 2:                            /* frame-based, or 16x8 in a field */
            if (frame_pic) {
                d->mv_type = M12_MV_16X16;
                for (i = 0; i < 2; i++) {
                    if (!(d->mv_dir & (1 << i))) continue;
                    for (k = 0; k < 2; k++) {
                        val = decode_motion(b, d->f_code[i][k], d->last_mv[i][0][k]);
                        if (val == MV_ERR) return MPEG12_ERR_CORRUPT;
                        d->last_mv[i][0][k] = d->last_mv[i][1][k] = val;
                        d->mv[i][0][k] = val;
                    }
                    /* MPEG-1 only: the vectors were coded in full samples. */
                    if (d->full_pel[i]) {
                        d->mv[i][0][0] *= 2;
                        d->mv[i][0][1] *= 2;
                    }
                }
            } else {
                d->mv_type = M12_MV_16X8;
                for (i = 0; i < 2; i++) {
                    if (!(d->mv_dir & (1 << i))) continue;
                    for (j = 0; j < 2; j++) {
                        d->field_select[i][j] = (int)m12_u1(b);
                        for (k = 0; k < 2; k++) {
                            val = decode_motion(b, d->f_code[i][k], d->last_mv[i][j][k]);
                            if (val == MV_ERR) return MPEG12_ERR_CORRUPT;
                            d->last_mv[i][j][k] = d->mv[i][j][k] = val;
                        }
                    }
                }
            }
            break;

        case 1:                            /* field-based */
            d->mv_type = M12_MV_FIELD;
            if (frame_pic) {
                for (i = 0; i < 2; i++) {
                    if (!(d->mv_dir & (1 << i))) continue;
                    for (j = 0; j < 2; j++) {
                        d->field_select[i][j] = (int)m12_u1(b);
                        val = decode_motion(b, d->f_code[i][0], d->last_mv[i][j][0]);
                        if (val == MV_ERR) return MPEG12_ERR_CORRUPT;
                        d->last_mv[i][j][0] = d->mv[i][j][0] = val;
                        /* 7.6.3.1: a field vector inside a frame picture
                         * measures FIELD lines, so the predictor -- which is
                         * kept in frame lines -- is halved going in and
                         * doubled coming out. */
                        val = decode_motion(b, d->f_code[i][1], d->last_mv[i][j][1] >> 1);
                        if (val == MV_ERR) return MPEG12_ERR_CORRUPT;
                        d->last_mv[i][j][1] = 2 * val;
                        d->mv[i][j][1] = val;
                    }
                }
            } else {
                for (i = 0; i < 2; i++) {
                    if (!(d->mv_dir & (1 << i))) continue;
                    d->field_select[i][0] = (int)m12_u1(b);
                    for (k = 0; k < 2; k++) {
                        val = decode_motion(b, d->f_code[i][k], d->last_mv[i][0][k]);
                        if (val == MV_ERR) return MPEG12_ERR_CORRUPT;
                        d->last_mv[i][0][k] = d->last_mv[i][1][k] = val;
                        d->mv[i][0][k] = val;
                    }
                }
            }
            break;

        case 3: {                          /* dual prime, 7.6.3.6 */
            int dmx, dmy, mx, my, m, my_shift = frame_pic ? 1 : 0;
            /* 13818-2's "//" is division rounding half AWAY from zero (4.1),
             * which for a halving is (v + (v > 0)) >> 1. A plain >>1 rounds
             * toward minus infinity and is off by one for every negative odd
             * vector -- the single most likely defect in this formula, and
             * invisible on content that never moves left or up.
             * -DMPEG12_DMV_TRUNC is that mistake on a switch. */
#ifdef MPEG12_DMV_TRUNC
#define DMV_HALF(v, mul) (((v) * (mul)) >> 1)
#else
#define DMV_HALF(v, mul) (((v) * (mul) + ((v) > 0)) >> 1)
#endif
            if (d->progressive_sequence) return MPEG12_ERR_CORRUPT;
            d->mv_type = M12_MV_DMV;
            for (i = 0; i < 2; i++) {
                if (!(d->mv_dir & (1 << i))) continue;
                mx = decode_motion(b, d->f_code[i][0], d->last_mv[i][0][0]);
                if (mx == MV_ERR) return MPEG12_ERR_CORRUPT;
                d->last_mv[i][0][0] = d->last_mv[i][1][0] = mx;
                dmx = decode_dmvector(b);
                my = decode_motion(b, d->f_code[i][1], d->last_mv[i][0][1] >> my_shift);
                if (my == MV_ERR) return MPEG12_ERR_CORRUPT;
                dmy = decode_dmvector(b);
                d->last_mv[i][0][1] = d->last_mv[i][1][1] = my * (1 << my_shift);

                d->mv[i][0][0] = d->mv[i][1][0] = mx;
                d->mv[i][0][1] = d->mv[i][1][1] = my;

                if (frame_pic) {
                    /* The derived vector scales the coded one by 1 or 3 --
                     * one field spacing or three -- and the +-1 is the half
                     * frame line between the two fields. `(v + (v > 0)) >> 1`
                     * is the standard's "//", division rounding half AWAY
                     * from zero; >>1 alone rounds toward -inf and moves every
                     * negative odd vector by a whole sample. */
                    m = d->top_field_first ? 1 : 3;
                    d->mv[i][2][0] = DMV_HALF(mx, m) + dmx;
                    d->mv[i][2][1] = DMV_HALF(my, m) + dmy - 1;
                    m = 4 - m;
                    d->mv[i][3][0] = DMV_HALF(mx, m) + dmx;
                    d->mv[i][3][1] = DMV_HALF(my, m) + dmy + 1;
                } else {
                    d->mv[i][2][0] = DMV_HALF(mx, 1) + dmx;
                    d->mv[i][2][1] = DMV_HALF(my, 1) + dmy;
                    if (d->picture_structure == M12_TOP_FIELD) d->mv[i][2][1]--;
                    else                                       d->mv[i][2][1]++;
                }
            }
            break;
        }

        default:
            return MPEG12_ERR_CORRUPT;     /* motion_type '00' is reserved */
        }
    }

    d->cbp = 0;
    if (mb_type & M12_MB_PATTERN) {
        int cbp = vlc_get(b, m12_vlc_cbp);
        if (cbp <= 0) return MPEG12_ERR_CORRUPT;   /* cbp 0 is 4:2:2/4:4:4 only */
        d->cbp = cbp;
        for (i = 0; i < 6; i++) {
            if (!(cbp & (1 << (5 - i)))) continue;
            r = decode_block(d, b, i, 0);
            if (r < 0) return r;
        }
    }
    reconstruct(d);
    return 0;
}

/* ------------------------------------------------------------- slice -- */
static int read_increment(m12br *b)
{
    int inc = 0;
    for (;;) {
        int code = vlc_get(b, m12_vlc_mbincr);
        if (code < 0) return -1;
        if (code == M12_MBINCR_ESCAPE)   inc += 33;
        else if (code == M12_MBINCR_STUFF) continue;   /* macroblock_stuffing */
        else return inc + code;
        if (b->over) return -1;
    }
}

int m12_decode_slice(mpeg12dec *d, int svp, const uint8_t *data, int len)
{
    m12br b;
    int inc, r;

    m12_br_init(&b, data, len);

    d->mb_y = svp - 1;
    if (d->mb_y < 0 || d->mb_y >= d->pic_mb_rows) return MPEG12_ERR_CORRUPT;

    d->qscale = get_qscale(d, &b);
    if (!d->qscale) return MPEG12_ERR_CORRUPT;

    /* intra_slice_flag / extra_information_slice, which collapse to exactly
     * "while the next bit is 1, drop the eight after it". */
    while (m12_u1(&b)) {
        m12_skip(&b, 8);
        if (b.over) return MPEG12_ERR_CORRUPT;
    }

    inc = read_increment(&b);
    if (inc < 1) return MPEG12_ERR_CORRUPT;
    d->mb_x = inc - 1;
    if (d->mb_x >= d->mb_width) return MPEG12_ERR_CORRUPT;

    reset_dc(d);
    memset(d->last_mv, 0, sizeof d->last_mv);
    d->mv_dir = 1;
    d->dct_type = 0;

    for (;;) {
        r = decode_mb(d, &b);
        if (r < 0) return r;
        if (b.over) return MPEG12_ERR_CORRUPT;

        if (++d->mb_x >= d->mb_width) {
            d->mb_x = 0;
            if (++d->mb_y >= d->pic_mb_rows) break;
        }

        /* 6.2.4: the slice ends when the next 23 bits are zero, which is the
         * head of the next start code. */
        if (m12_left(&b) <= 0 || m12_peek(&b, 23) == 0) break;

        inc = read_increment(&b);
        if (inc < 1) return MPEG12_ERR_CORRUPT;
        while (--inc > 0) {
            if (d->coding_type == MPEG12_PICT_I) return MPEG12_ERR_CORRUPT;
            setup_skip(d);
            reconstruct(d);
            if (++d->mb_x >= d->mb_width) {
                d->mb_x = 0;
                if (++d->mb_y >= d->pic_mb_rows) return MPEG12_ERR_CORRUPT;
            }
        }
    }
    return 0;
}
