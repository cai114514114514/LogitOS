/* c/lib/video/mpeg12.c -- MPEG-1 / MPEG-2 video: start codes, headers,
 * picture management and display reordering.
 *
 *
 * THE IDCT QUESTION, ANSWERED FIRST BECAUSE IT DECIDES WHAT THE GATE CAN BE
 * ------------------------------------------------------------------------
 * H.264 and H.265 in this tree are gated on bit-exactness because their
 * reconstruction is exactly specified integer arithmetic: there is one right
 * answer per sample and any difference from ffmpeg is our bug. MPEG-1/2 is
 * NOT like that. ISO/IEC 13818-2 Annex A specifies the inverse DCT only by an
 * ACCURACY REQUIREMENT -- the IEEE 1180 statistical test -- and explicitly
 * does not name an algorithm. Two decoders that both pass Annex A may differ
 * by +-1 in a sample, and because P and B pictures predict from the
 * reconstruction, that +-1 is fed back and accumulates for the rest of the
 * GOP. "Within a tolerance, drifting" is not a gate: it cannot distinguish a
 * one-bit rounding difference from a wrong motion vector on a flat area.
 *
 * So the transform is PINNED to the one the oracle uses. FFmpeg's `-idct
 * simple` selects a specific 14-bit-constant integer transform, and this
 * decoder implements that transform (mpeg12_idct.c derives every butterfly
 * coefficient from cos((2n+1)k*pi/16) and pins the three places where the
 * implementation makes a CHOICE rather than following the algebra). With the
 * transform pinned, every remaining difference is a real decoding difference,
 * and the gate is bit-exactness over whole streams -- the same bar as H.264.
 *
 * THE FLAG DOES SOMETHING, MEASURED. Decoding one 352x288 MPEG-2 stream (10
 * frames, IBBP, qscale 3) twice, changing nothing but the IDCT:
 *
 *     ffmpeg -idct simple  vs  -idct int   ->  differ, first at byte 76
 *     1,520,640 bytes compared, 274,969 differ (18.1%), max |delta| 3
 *
 * That is the control for this whole approach: if the two had matched, the
 * flag would not be selecting anything and "bit-exact against -idct simple"
 * would be a claim about nothing. 18% of samples differing, by up to 3, over
 * ten frames is also the size of the drift that a tolerance-based gate would
 * have had to accept -- and would therefore have been blind to.
 *
 * (The reference YUV in tests/mpeg12.mk is generated with -idct simple for
 * exactly this reason. Using ffmpeg's default there would compare against
 * whichever SIMD transform the host CPU happened to select.)
 *
 *
 * WHAT IS REFUSED, BY NAME
 * ------------------------
 * 4:2:2 and 4:4:4 chroma (Main profile is 4:2:0 and that is what this decodes;
 * the block count, the chroma vector derivation and the coded_block_pattern
 * extension all change), D-pictures (MPEG-1 picture_coding_type 4 -- a
 * DC-only picture type no encoder has emitted in thirty years), the scalable
 * extensions and data partitioning, and vertical_size > 2800 (which adds
 * slice_vertical_position_extension to every slice header). Each is reported
 * as MPEG12_ERR_UNSUPPORTED at the header that declares it, never decoded
 * half-way and never concealed.
 */
#include <stdlib.h>
#include <string.h>
#include "mpeg12_int.h"

#define SC_PICTURE   0x00
#define SC_SLICE_MIN 0x01
#define SC_SLICE_MAX 0xAF
#define SC_USER      0xB2
#define SC_SEQUENCE  0xB3
#define SC_EXTENSION 0xB5
#define SC_SEQ_END   0xB7
#define SC_GOP       0xB8

#define EXT_SEQUENCE        1
#define EXT_SEQ_DISPLAY     2
#define EXT_QUANT_MATRIX    3
#define EXT_COPYRIGHT       4
#define EXT_SEQ_SCALABLE    5
#define EXT_PIC_DISPLAY     7
#define EXT_PIC_CODING      8
#define EXT_PIC_SPATIAL     9
#define EXT_PIC_TEMPORAL   10

#define NPOOL 6

/* ------------------------------------------------------------- frames -- */
static void pic_free(m12pic *p)
{
    free(p->mem);
    p->mem = p->y = p->u = p->v = 0;
    p->aw = p->ah = 0;
}

static int pic_alloc(m12pic *p, int w, int h)
{
    long ysz, csz;
    if (p->mem && p->aw == w && p->ah == h) return 0;
    pic_free(p);
    ysz = (long)w * h;
    csz = (long)(w / 2) * (h / 2);
    p->mem = calloc(1, (size_t)(ysz + 2 * csz));
    if (!p->mem) return MPEG12_ERR_OOM;
    p->y = p->mem;
    p->u = p->mem + ysz;
    p->v = p->mem + ysz + csz;
    p->stride_y = w;
    p->stride_c = w / 2;
    p->aw = w; p->ah = h;
    return 0;
}

static m12pic *pic_get(mpeg12dec *d)
{
    int i;
    for (i = 0; i < NPOOL; i++) {
        m12pic *p = &d->pool[i];
        if (p == d->cur || p == d->ref_old || p == d->ref_new ||
            p == d->delayed || p == d->last_out) continue;
        if (pic_alloc(p, d->coded_w, d->coded_h) < 0) return 0;
        return p;
    }
    return 0;
}

static void emit(mpeg12dec *d, m12pic *p, mpeg12frame *out, int *got)
{
    out->width = d->width;
    out->height = d->height;
    out->stride_y = p->stride_y;
    out->stride_c = p->stride_c;
    out->y = p->y; out->u = p->u; out->v = p->v;
    out->pts = p->pts;
    out->coding_type = p->coding_type;
    out->temporal_reference = p->temporal_reference;
    d->last_out = p;
    *got = 1;
}

/* --------------------------------------------------------- quant load -- */
static void load_matrix(m12br *b, uint8_t *dst)
{
    int i;
    for (i = 0; i < 64; i++)
        dst[m12_scan_zigzag[i]] = (uint8_t)m12_u(b, 8);
}

/* -------------------------------------------------------- geometry -- */
static void set_geometry(mpeg12dec *d)
{
    d->mb_width = (d->width + 15) / 16;
    /* A non-progressive sequence may code any picture as two fields, and a
     * field is half the frame's lines, so the frame must be a whole number of
     * macroblocks IN EACH FIELD -- 32 lines, not 16. A 240-line interlaced
     * picture is 8 macroblock rows per field and therefore 256 coded lines. */
    if (d->progressive_sequence) d->mb_height = (d->height + 15) / 16;
    else                         d->mb_height = ((d->height + 31) / 32) * 2;
    d->coded_w = d->mb_width * 16;
    d->coded_h = d->mb_height * 16;
}

/* --------------------------------------------------------- headers -- */
static int parse_sequence(mpeg12dec *d, const uint8_t *p, int len)
{
    m12br b;
    int i;

    m12_br_init(&b, p, len);
    d->width  = (int)m12_u(&b, 12);
    d->height = (int)m12_u(&b, 12);
    if (d->width < 1 || d->height < 1) return MPEG12_ERR_CORRUPT;
    m12_u(&b, 4);                                  /* aspect_ratio */
    d->frame_rate_code = (int)m12_u(&b, 4);
    m12_u(&b, 18);                                 /* bit_rate_value */
    if (!m12_u1(&b)) return MPEG12_ERR_CORRUPT;    /* marker_bit */
    m12_u(&b, 10);                                 /* vbv_buffer_size */
    m12_u(&b, 1);                                  /* constrained_parameters */

    if (m12_u1(&b)) load_matrix(&b, d->qm_intra);
    else for (i = 0; i < 64; i++) d->qm_intra[i] = m12_default_intra_matrix[i];
    if (m12_u1(&b)) load_matrix(&b, d->qm_inter);
    else for (i = 0; i < 64; i++) d->qm_inter[i] = m12_default_non_intra_matrix[i];
    memcpy(d->qm_cintra, d->qm_intra, 64);
    memcpy(d->qm_cinter, d->qm_inter, 64);

    if (b.over) return MPEG12_ERR_CORRUPT;

    /* A bare sequence header is MPEG-1 until a sequence extension says
     * otherwise; the extension, if it comes, follows immediately. */
    d->is_mpeg2 = 0;
    d->progressive_sequence = 1;
    d->chroma_format = 1;
    d->fr_n = 1; d->fr_d = 1;
    d->have_seq = 1;
    set_geometry(d);
    return 0;
}

static int parse_sequence_ext(mpeg12dec *d, m12br *b)
{
    int hext, vext;

    m12_u(b, 8);                                   /* profile_and_level */
    d->progressive_sequence = (int)m12_u1(b);
    d->chroma_format = (int)m12_u(b, 2);
    hext = (int)m12_u(b, 2);
    vext = (int)m12_u(b, 2);
    m12_u(b, 12);                                  /* bit_rate_extension */
    if (!m12_u1(b)) return MPEG12_ERR_CORRUPT;     /* marker_bit */
    m12_u(b, 8);                                   /* vbv_buffer_size_ext */
    m12_u(b, 1);                                   /* low_delay */
    d->fr_n = (int)m12_u(b, 2) + 1;
    d->fr_d = (int)m12_u(b, 5) + 1;

    d->is_mpeg2 = 1;
    d->width  |= hext << 12;
    d->height |= vext << 12;
    if (d->chroma_format != 1) return MPEG12_ERR_UNSUPPORTED;   /* 4:2:2 / 4:4:4 */
    if (d->height > 2800) return MPEG12_ERR_UNSUPPORTED;        /* svp extension */
    set_geometry(d);
    return 0;
}

static int parse_picture(mpeg12dec *d, const uint8_t *p, int len)
{
    m12br b;

    if (!d->have_seq) return MPEG12_ERR_CORRUPT;
    m12_br_init(&b, p, len);
    d->temporal_reference = (int)m12_u(&b, 10);
    d->coding_type = (int)m12_u(&b, 3);
    m12_u(&b, 16);                                 /* vbv_delay */

    d->full_pel[0] = d->full_pel[1] = 0;
    d->f_code[0][0] = d->f_code[0][1] = 15;
    d->f_code[1][0] = d->f_code[1][1] = 15;

    if (d->coding_type == MPEG12_PICT_P || d->coding_type == MPEG12_PICT_B) {
        d->full_pel[0] = (int)m12_u1(&b);
        d->f_code[0][0] = d->f_code[0][1] = (int)m12_u(&b, 3);
    }
    if (d->coding_type == MPEG12_PICT_B) {
        d->full_pel[1] = (int)m12_u1(&b);
        d->f_code[1][0] = d->f_code[1][1] = (int)m12_u(&b, 3);
    }
    while (m12_u1(&b)) m12_skip(&b, 8);            /* extra_information_picture */
    if (b.over) return MPEG12_ERR_CORRUPT;

    if (d->coding_type == MPEG12_PICT_D) return MPEG12_ERR_UNSUPPORTED;
    if (d->coding_type < MPEG12_PICT_I || d->coding_type > MPEG12_PICT_D)
        return MPEG12_ERR_CORRUPT;

    /* MPEG-1 has no picture_coding_extension, so these ARE the defaults it
     * defines; for MPEG-2 they are overwritten a few bytes later and a
     * picture that never gets its extension is refused at the first slice. */
    d->picture_structure = M12_FRAME;
    d->frame_pred_frame_dct = 1;
    d->intra_dc_precision = 0;
    d->concealment_mv = 0;
    d->q_scale_type = 0;
    d->intra_vlc_format = 0;
    d->alternate_scan = 0;
    d->top_field_first = 1;
    d->repeat_first_field = 0;
    d->progressive_frame = 1;
    if (d->is_mpeg2) d->frame_pred_frame_dct = -1;  /* "not seen yet" */

    d->pic_open = 1;
    return 0;
}

static int parse_picture_ext(mpeg12dec *d, m12br *b)
{
    d->f_code[0][0] = (int)m12_u(b, 4);
    d->f_code[0][1] = (int)m12_u(b, 4);
    d->f_code[1][0] = (int)m12_u(b, 4);
    d->f_code[1][1] = (int)m12_u(b, 4);
    d->intra_dc_precision = (int)m12_u(b, 2);
    d->picture_structure = (int)m12_u(b, 2);
    d->top_field_first = (int)m12_u1(b);
    d->frame_pred_frame_dct = (int)m12_u1(b);
    d->concealment_mv = (int)m12_u1(b);
    d->q_scale_type = (int)m12_u1(b);
    d->intra_vlc_format = (int)m12_u1(b);
    d->alternate_scan = (int)m12_u1(b);
    d->repeat_first_field = (int)m12_u1(b);
    m12_u1(b);                                     /* chroma_420_type */
    d->progressive_frame = (int)m12_u1(b);
    if (m12_u1(b)) m12_skip(b, 20);                /* composite_display_flag */
    if (b->over) return MPEG12_ERR_CORRUPT;
    if (d->picture_structure < 1 || d->picture_structure > 3)
        return MPEG12_ERR_CORRUPT;
    return 0;
}

static int parse_extension(mpeg12dec *d, const uint8_t *p, int len)
{
    m12br b;
    int id;

    m12_br_init(&b, p, len);
    id = (int)m12_u(&b, 4);
    switch (id) {
    case EXT_SEQUENCE:     return parse_sequence_ext(d, &b);
    case EXT_PIC_CODING:   return parse_picture_ext(d, &b);
    case EXT_QUANT_MATRIX:
        if (m12_u1(&b)) { load_matrix(&b, d->qm_intra);  memcpy(d->qm_cintra, d->qm_intra, 64); }
        if (m12_u1(&b)) { load_matrix(&b, d->qm_inter);  memcpy(d->qm_cinter, d->qm_inter, 64); }
        if (m12_u1(&b)) load_matrix(&b, d->qm_cintra);
        if (m12_u1(&b)) load_matrix(&b, d->qm_cinter);
        return b.over ? MPEG12_ERR_CORRUPT : 0;
    case EXT_SEQ_SCALABLE:
    case EXT_PIC_SPATIAL:
    case EXT_PIC_TEMPORAL:
        return MPEG12_ERR_UNSUPPORTED;             /* scalable coding */
    default:
        return 0;                                  /* display / copyright */
    }
}

/* ------------------------------------------------------ picture flow -- */
static int start_picture(mpeg12dec *d)
{
    int field_pic = (d->picture_structure != M12_FRAME);

    if (d->frame_pred_frame_dct < 0)               /* MPEG-2 with no ext */
        return MPEG12_ERR_CORRUPT;
    if (field_pic && d->progressive_sequence)
        return MPEG12_ERR_CORRUPT;

    if (!field_pic || !d->second_field) {
        m12pic *p;
        d->second_field = 0;
        p = pic_get(d);
        if (!p) return MPEG12_ERR_OOM;
        d->cur = p;
        p->pts = d->pending_pts;
        p->coding_type = d->coding_type;
        p->temporal_reference = d->temporal_reference;
    }
    if (!d->cur) return MPEG12_ERR_CORRUPT;

    if (d->coding_type != MPEG12_PICT_I && !d->ref_new) return MPEG12_ERR_CORRUPT;
    if (d->coding_type == MPEG12_PICT_B && !d->ref_old) return MPEG12_ERR_CORRUPT;

    d->scan = d->alternate_scan ? m12_scan_alternate : m12_scan_zigzag;
    d->ps_y = d->cur->stride_y << (field_pic ? 1 : 0);
    d->ps_c = d->cur->stride_c << (field_pic ? 1 : 0);
    d->pic_y = d->cur->y;
    d->pic_u = d->cur->u;
    d->pic_v = d->cur->v;
    if (d->picture_structure == M12_BOT_FIELD) {
        d->pic_y += d->cur->stride_y;
        d->pic_u += d->cur->stride_c;
        d->pic_v += d->cur->stride_c;
    }
    d->pic_mb_rows = field_pic ? d->mb_height / 2 : d->mb_height;
    d->cen.pictures++;
    if (field_pic) d->cen.field_pictures++;
    return 0;
}

/* Called when the last unit of a picture has gone by. */
static void finish_picture(mpeg12dec *d, mpeg12frame *out, int *got)
{
    m12pic *done;

    if (d->picture_structure != M12_FRAME && !d->second_field) {
        d->second_field = 1;                       /* one more field to come */
        d->pic_open = 0;
        return;
    }
    d->second_field = 0;
    d->pic_open = 0;
    done = d->cur;
    d->cur = 0;
    if (!done) return;

    if (done->coding_type == MPEG12_PICT_B) {
        emit(d, done, out, got);
    } else {
        /* An anchor is held back one anchor: everything that displays before
         * it and decodes after it is a B picture, and all of those arrive
         * before the NEXT anchor does. */
        m12pic *prev = d->delayed;
        d->delayed = done;
        d->ref_old = d->ref_new;
        d->ref_new = done;
        if (prev) emit(d, prev, out, got);
    }
}

/* --------------------------------------------------------- top level -- */
mpeg12dec *mpeg12_open(void)
{
    mpeg12dec *d = (mpeg12dec *)calloc(1, sizeof *d);
    if (!d) return 0;
    d->pending_pts = MPEG12_NOPTS;
    return d;
}

void mpeg12_close(mpeg12dec *d)
{
    int i;
    if (!d) return;
    for (i = 0; i < NPOOL; i++) pic_free(&d->pool[i]);
    free(d);
}

/* Offset of the next 00 00 01 prefix at or after `from`, or -1. */
static int find_start(const uint8_t *p, int len, int from, int *code)
{
    int i;
    for (i = from; i + 3 < len; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            *code = p[i + 3];
            return i;
        }
    }
    return -1;
}

int mpeg12_decode_pts(mpeg12dec *d, const uint8_t *data, int len, int64_t pts,
                      mpeg12frame *out, int *got_frame)
{
    int pos = 0, code, next, next_code, end, r;

    *got_frame = 0;
    if (!d || !data || len < 0) return MPEG12_ERR_CORRUPT;
    if (pts != MPEG12_NOPTS) d->pending_pts = pts;

    for (;;) {
        int sc = find_start(data, len, pos, &code);
        if (sc < 0) {
            if (d->pic_open) finish_picture(d, out, got_frame);
            return len;
        }

        /* A picture, sequence, GOP or end code closes the picture in flight,
         * and the caller gets it before anything of the next one is touched. */
        if (d->pic_open &&
            (code == SC_PICTURE || code == SC_SEQUENCE ||
             code == SC_GOP || code == SC_SEQ_END)) {
            finish_picture(d, out, got_frame);
            return sc;
        }

        next = find_start(data, len, sc + 4, &next_code);
        end = (next < 0) ? len : next;

        if (code == SC_PICTURE) {
            r = parse_picture(d, data + sc + 4, end - sc - 4);
            if (r < 0) return r;
        } else if (code == SC_SEQUENCE) {
            r = parse_sequence(d, data + sc + 4, end - sc - 4);
            if (r < 0) return r;
        } else if (code == SC_EXTENSION) {
            r = parse_extension(d, data + sc + 4, end - sc - 4);
            if (r < 0) return r;
        } else if (code >= SC_SLICE_MIN && code <= SC_SLICE_MAX) {
            if (!d->pic_open) { pos = end; continue; }   /* slice with no picture */
            if (!d->pic_y) {
                r = start_picture(d);
                if (r < 0) return r;
            }
            r = m12_decode_slice(d, code, data + sc + 4, end - sc - 4);
            if (r < 0) return r;
        }
        /* SC_GOP, SC_USER, SC_SEQ_END and the reserved codes: nothing to do */

        if (code == SC_PICTURE) d->pic_y = 0;        /* allocate at first slice */
        pos = end;
        if (next < 0) {
            if (d->pic_open) finish_picture(d, out, got_frame);
            return len;
        }
    }
}

int mpeg12_decode(mpeg12dec *d, const uint8_t *data, int len,
                  mpeg12frame *out, int *got_frame)
{
    return mpeg12_decode_pts(d, data, len, MPEG12_NOPTS, out, got_frame);
}

int mpeg12_flush(mpeg12dec *d, mpeg12frame *out)
{
    int got = 0;
    if (!d) return MPEG12_ERR_CORRUPT;
    if (d->delayed) {
        m12pic *p = d->delayed;
        d->delayed = 0;
        emit(d, p, out, &got);
    }
    return got;
}

int mpeg12_stream_info(mpeg12dec *d, int *w, int *h, double *fps, int *is_mpeg2)
{
    static const double base[16] = {
        0, 24000.0 / 1001, 24, 25, 30000.0 / 1001, 30, 50, 60000.0 / 1001, 60,
        0, 0, 0, 0, 0, 0, 0
    };
    if (!d || !d->have_seq) return MPEG12_ERR_CORRUPT;
    if (w) *w = d->width;
    if (h) *h = d->height;
    if (fps) *fps = base[d->frame_rate_code & 15] * d->fr_n / d->fr_d;
    if (is_mpeg2) *is_mpeg2 = d->is_mpeg2;
    return 0;
}

void mpeg12_get_census(mpeg12dec *d, mpeg12_census *out)
{
    if (!d || !out) return;
    *out = d->cen;
}
