/* c/lib/video/mpeg4_int.h -- internal state of the MPEG-4 Part 2 / H.263
 * decoder. Not a public header; nothing outside c/lib/video/mpeg4*.c uses it.
 */
#ifndef LOGIT_MPEG4_INT_H
#define LOGIT_MPEG4_INT_H

#include <stdint.h>
#include "mpeg4.h"
#include "mpeg4_bits.h"

/* ---- macroblock type bits -------------------------------------------------
 * A private set, not the format's: the two syntaxes describe the same twelve
 * facts about a macroblock with different codewords, and the reconstruction
 * path wants the facts. */
#define M4_MB_INTRA      0x0001
#define M4_MB_16x16      0x0002
#define M4_MB_16x8       0x0004
#define M4_MB_8x8        0x0008
#define M4_MB_INTERLACED 0x0010
#define M4_MB_DIRECT2    0x0020
#define M4_MB_ACPRED     0x0040
#define M4_MB_GMC        0x0080
#define M4_MB_SKIP       0x0100
#define M4_MB_FWD        0x0200
#define M4_MB_BWD        0x0400
#define M4_MB_BIDIR      (M4_MB_FWD | M4_MB_BWD)
#define M4_MB_CBP        0x0800
#define M4_MB_QUANT      0x1000

#define M4_MV_DIR_FWD 1
#define M4_MV_DIR_BWD 2
#define M4_MV_DIRECT  4

#define M4_MV_16X16 0
#define M4_MV_8X8   1
#define M4_MV_FIELD 2

/* decode_mb return codes, ffmpeg's SLICE_* by another name */
#define M4_SLICE_OK     0
#define M4_SLICE_END   -1
#define M4_SLICE_NOEND -2
#define M4_SLICE_ERROR -3

#define M4_RECT_SHAPE 0
#define M4_GMC_SPRITE 2

typedef struct {
    uint8_t *buf;                 /* one allocation for all three planes */
    uint8_t *data[3];
    int32_t *mb_type;             /* mb_stride * mb_height */
    int8_t  *qscale_table;
    int16_t (*motion_val_base[2])[2];
    int16_t (*motion_val[2])[2];  /* + b8_stride + 1 */
    int8_t  *ref_index[2];        /* 4 per macroblock */
    uint8_t *mbskip_table;
    int      pict_type;
    int      reference;
    int      in_use;
    int64_t  pts;
    int      interlaced, tff;
} m4pic;

struct mpeg4dec {
    int flavor;
    const char *err;
    char errbuf[160];

    /* ---- VOL ---- */
    int have_vol, vo_type, shape, quant_precision, time_increment_bits;
    int vol_control_parameters, low_delay, mpeg_quant, quarter_sample;
    int resync_marker, data_partitioning, rvlc, new_pred, scalability;
    int vol_sprite_usage, sprite_warping_points;
    int progressive_sequence;
    int cplx_i, cplx_p, cplx_b;
    int framerate_num, framerate_den;
    uint16_t intra_matrix[64], inter_matrix[64];
    int short_header;             /* H.263 picture syntax (plain or MPEG-4 SVH) */

    /* ---- geometry ---- */
    int width, height, mb_width, mb_height, mb_num, mb_stride, b8_stride;
    int linesize, uvlinesize, h_edge_pos, v_edge_pos;
    int alloc_w, alloc_h;

    /* ---- picture ---- */
    int pict_type, qscale, chroma_qscale, f_code, b_code;
    int no_rounding, top_field_first, alternate_scan, interlaced_dct;
    int intra_dc_threshold, partitioned_frame, progressive_frame;
    int64_t time, last_non_b_time, time_base, last_time_base;
    int pp_time, pb_time, pp_field_time, pb_field_time, t_frame;
    int direct_scale_mv[2][512];
    int picture_number;

    /* ---- macroblock ---- */
    int mb_x, mb_y, resync_mb_x, resync_mb_y, first_slice_line, mb_num_left;
    int mb_intra, mb_skipped, ac_pred, mv_dir, mv_type, mcsel;
    int16_t mv[2][4][2];
    int16_t last_mv[2][2][2];
    int field_select[2][2];
    int y_dc_scale, c_dc_scale;
    const uint8_t *y_dc_scale_table, *c_dc_scale_table, *chroma_qscale_table;
    int block_index[6], block_wrap[6];
    int block_last_index[6];
    int16_t block[6][64];
    const uint8_t *scan, *scan_h, *scan_v;

    /* ---- prediction storage ---- */
    int16_t *dc_val;              /* + b8_stride + 1 */
    int16_t *dc_val_base;
    int16_t (*ac_val)[16];        /* + b8_stride + 1 */
    int16_t (*ac_val_base)[16];
    uint8_t *mbintra_table;
    uint8_t *cbp_table;
    uint8_t *pred_dir_table;
    int16_t (*p_field_mv_table[2])[2];

    /* ---- H.263 ---- */
    int h263_plus, h263_long_vectors, h263_aic, h263_aic_dir, obmc, umvplus;
    int alt_inter_vlc, modified_quant, loop_filter, custom_pcf;
    int slice_structured, pb_frame, gob_index;

    /* ---- pictures ---- */
    m4pic pool[3];
    m4pic *cur, *last, *next;
    int64_t next_pts;

    /* ---- scratch ---- */
    uint8_t *emu;                 /* edge emulation buffer, stride == linesize */
    int emu_size;
    uint8_t *dest[3];

    m4bits gb;
};

typedef struct mpeg4dec m4ctx;

/* mpeg4_hdr.c */
int  m4_decode_vop(m4ctx *s, m4bits *gb, int *skipped);
int  m4_decode_h263_picture(m4ctx *s, m4bits *gb);
int  m4_video_packet_header(m4ctx *s);
int  m4_gob_header(m4ctx *s);
int  m4_packet_prefix_len(const m4ctx *s);
void m4_set_qscale(m4ctx *s, int qscale);
int  m4_alloc_geometry(m4ctx *s);
void m4_fail(m4ctx *s, const char *what);

/* mpeg4_mb.c */
int  m4_decode_mb(m4ctx *s);
int  m4_decode_mb_h263(m4ctx *s);
int  m4_decode_mb_partitioned(m4ctx *s);
int  m4_decode_partitions(m4ctx *s);
int  m4_is_resync(m4ctx *s);
void m4_clean_buffers(m4ctx *s);
void m4_clean_intra_entries(m4ctx *s);
void m4_update_motion_val(m4ctx *s);
int  m4_pred_motion(m4ctx *s, int block, int dir, int *px, int *py,
                    int16_t **mot_val);
void m4_dequant(m4ctx *s, int16_t *block, int n, int qscale, int intra);

/* mpeg4_mc.c */
void m4_mc(m4ctx *s, uint8_t *dy, uint8_t *dcb, uint8_t *dcr, int dir,
           uint8_t *const *ref, int avg);
void m4_loop_filter(m4ctx *s);

/* mpeg4_idct.c */
void m4_idct_put(uint8_t *dest, int line_size, int16_t *block);
void m4_idct_add(uint8_t *dest, int line_size, int16_t *block);

#endif /* LOGIT_MPEG4_INT_H */
