/* c/lib/video/mpeg12_int.h -- internals shared by the MPEG-1/2 decoder's
 * translation units. Not a public header; mpeg12.h is. */
#ifndef LOGIT_MPEG12_INT_H
#define LOGIT_MPEG12_INT_H

#include <stdint.h>
#include "mpeg12.h"
#include "mpeg12_bits.h"
#include "mpeg12_tables.h"

/* picture_structure */
#define M12_TOP_FIELD 1
#define M12_BOT_FIELD 2
#define M12_FRAME     3

/* mv_type */
#define M12_MV_16X16  0   /* one vector, frame prediction                    */
#define M12_MV_FIELD  1   /* frame picture: two field halves; field picture: */
                          /* one 16x16 field prediction                      */
#define M12_MV_16X8   2   /* field picture only: two 16x8 halves             */
#define M12_MV_DMV    3   /* dual prime                                      */

typedef struct {
    uint8_t *mem;                 /* the single allocation */
    uint8_t *y, *u, *v;
    int stride_y, stride_c;       /* FRAME strides, always */
    int aw, ah;                   /* allocated coded size */
    int64_t pts;
    int coding_type;
    int temporal_reference;
    int inuse;
} m12pic;

struct mpeg12dec {
    /* ---- sequence ---- */
    int have_seq, is_mpeg2;
    int width, height;            /* display size */
    int coded_w, coded_h;         /* macroblock-aligned frame size */
    int mb_width, mb_height;      /* macroblocks in a FRAME */
    int progressive_sequence;
    int chroma_format;
    int frame_rate_code, fr_n, fr_d;
    uint8_t qm_intra[64], qm_inter[64];
    uint8_t qm_cintra[64], qm_cinter[64];

    /* ---- picture ---- */
    int coding_type, temporal_reference;
    int f_code[2][2];
    int full_pel[2];
    int intra_dc_precision;
    int picture_structure;
    int top_field_first, frame_pred_frame_dct, concealment_mv;
    int q_scale_type, intra_vlc_format, alternate_scan;
    int repeat_first_field, progressive_frame;
    int second_field;             /* this field picture completes a frame */
    int pic_open;                 /* a picture is being decoded */
    int64_t pending_pts;
    const uint8_t *scan;          /* zigzag or alternate */

    /* current picture geometry, in the picture's own coordinate system */
    uint8_t *pic_y, *pic_u, *pic_v;
    int ps_y, ps_c;               /* picture strides (frame stride << field) */
    int pic_mb_rows;              /* macroblock rows in THIS picture */

    /* ---- slice / macroblock ---- */
    int qscale;                   /* already doubled / table-mapped */
    int mb_x, mb_y;
    int mb_intra, mv_dir, mv_type, dct_type;
    int field_select[2][2];
    int mv[2][4][2];
    int last_mv[2][2][2];
    int last_dc[3];
    int cbp;
    int16_t block[6][64];

    /* ---- frames ----
     * Five distinct pictures can be live at once: the two anchors, the one
     * held back for display order, the one being decoded, and the one the
     * caller is still looking at (the API promises the last output stays
     * valid until the next call). Six so that pic_get() always finds one. */
    m12pic pool[6];
    m12pic *cur, *ref_new, *ref_old, *delayed, *last_out;

    mpeg12_census cen;
};

/* One VLC lookup, in the two-level format tools/gen_mpeg12_tables.py emits.
 * Inline in the header rather than static in mpeg12_slice.c so that
 * tests/unit/mpeg12_vlc_test.c walks the SAME code the decoder walks -- a
 * table test that re-implements the lookup is testing its own copy, and the
 * two can agree about a table both read wrongly. */
static inline int m12_vlc_get(m12br *b, const m12_vlc_e *tab)
{
    m12_vlc_e e = tab[m12_peek(b, M12_VLC_BITS)];
    if (e.sub) {
        m12_skip(b, M12_VLC_BITS);
        e = tab[e.sym + m12_peek(b, e.len)];
    }
    if (!e.len) { b->over = 1; return -1; }
    m12_skip(b, e.len);
    return e.sym;
}

/* mpeg12_idct.c */
void m12_idct_put(uint8_t *dst, int stride, int16_t *block);
void m12_idct_add(uint8_t *dst, int stride, int16_t *block);
/* The same transform with no clipping and no destination: the 64 shifted
 * column sums. Exists for the IEEE 1180 accuracy test, which measures the
 * transform and not the store, and shares idct_row/idct_col with the two
 * above so it cannot drift from what the decoder runs. */
void m12_idct_raw(int16_t *block, int *out);

/* mpeg12_mc.c -- one prediction. `stride` is BOTH the destination and the
 * source line step, which is true in every MPEG-1/2 case: a field prediction
 * writes into a field and reads from a field, at the same doubled step. */
void m12_pred(uint8_t *dst, const uint8_t *src, int stride,
              int w, int h, int hx, int hy, int avg);

/* Bounds-checked variant: builds the block through a clamped fetch when it
 * would otherwise read outside [plane, plane + h_pix x v_pix]. Returns 1 if it
 * had to clamp. */
int m12_pred_edge(uint8_t *dst, const uint8_t *plane, int stride,
                  int src_x, int src_y, int pw, int ph,
                  int w, int h, int hx, int hy, int avg);

/* mpeg12_slice.c */
int m12_decode_slice(mpeg12dec *d, int slice_vertical_position,
                     const uint8_t *data, int len);

#endif /* LOGIT_MPEG12_INT_H */
