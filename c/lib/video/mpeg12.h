/* c/lib/video/mpeg12.h -- public API of the from-scratch MPEG-1/MPEG-2 video
 * decoder (ISO/IEC 11172-2 and 13818-2, 4:2:0, 8 bit).
 *
 * What it decodes: sequence header + sequence/quant-matrix/picture-coding
 * extensions, GOP headers, I/P/B pictures, FRAME and FIELD pictures, frame and
 * field DCT, frame/field/16x8/dual-prime motion compensation, half-pel MC,
 * both quantiser matrices and their mid-stream updates, intra VLC tables 0 and
 * 1, alternate scan, non-linear quantiser scale, concealment motion vectors,
 * and the MPEG-1 differences (full-pel vectors, the oddification of the
 * dequantised level, the single-escape level code).
 *
 * What it refuses BY NAME, never silently: chroma formats other than 4:2:0
 * (the 4:2:2 and 4:4:4 profiles), D-pictures (MPEG-1 picture_coding_type 4),
 * the scalable extensions (sequence_scalable_extension,
 * picture_spatial_scalable_extension, picture_temporal_scalable_extension),
 * data partitioning, and vertical_size > 2800 (slice_vertical_position_
 * extension). There is NO error concealment: a stream that violates the
 * standard is reported as MPEG12_ERR_CORRUPT, not painted over with a guess.
 * Every input byte is UNTRUSTED.
 *
 * Usage (pull model, an MPEG video elementary stream in):
 *   mpeg12dec *d = mpeg12_open();
 *   while (consumed < len) {
 *       mpeg12frame f; int got = 0;
 *       int n = mpeg12_decode(d, buf + consumed, len - consumed, &f, &got);
 *       if (n < 0) -> fatal decode error
 *       consumed += n;
 *       if (got) -> present f.y/f.u/f.v (valid until the next call)
 *   }
 *   while (mpeg12_flush(d, &f) == 1) -> the picture still held for reordering
 *
 * A call consumes at most one picture. The buffer handed in must contain whole
 * start-code-delimited units: the decoder treats the end of the buffer as the
 * end of the last unit in it, which is right when the caller feeds a whole
 * elementary stream or whole pictures and wrong if it feeds arbitrary chunks.
 */
#ifndef LOGIT_MPEG12_H
#define LOGIT_MPEG12_H

#include <stdint.h>

#define MPEG12_OK               0
#define MPEG12_ERR_CORRUPT     -1   /* bitstream violates the standard */
#define MPEG12_ERR_UNSUPPORTED -2   /* legal MPEG, but a feature we do not do */
#define MPEG12_ERR_OOM         -3

typedef struct mpeg12dec mpeg12dec;

#define MPEG12_NOPTS ((int64_t)(-0x7fffffffffffffffLL - 1))

/* Picture coding types, as coded in the picture header. */
#define MPEG12_PICT_I 1
#define MPEG12_PICT_P 2
#define MPEG12_PICT_B 3
#define MPEG12_PICT_D 4

typedef struct {
    int width, height;        /* display size (horizontal_size x vertical_size) */
    int stride_y, stride_c;
    const uint8_t *y, *u, *v;
    int64_t pts;              /* what the caller handed in, carried through */
    int coding_type;          /* MPEG12_PICT_* of the coded picture */
    int temporal_reference;
} mpeg12frame;

mpeg12dec *mpeg12_open(void);
void       mpeg12_close(mpeg12dec *d);

/* Decode. Returns bytes consumed (>= 0) and sets *got_frame when a picture
 * came out in DISPLAY order. Negative: MPEG12_ERR_*; the decoder state is
 * undefined afterwards, close it. */
int mpeg12_decode(mpeg12dec *d, const uint8_t *data, int len,
                  mpeg12frame *out, int *got_frame);

/* As mpeg12_decode, but attaches `pts` to the picture that starts in this
 * call and hands it back on that picture however far out of decode order it
 * comes -- which for a stream with B pictures is most of them. */
int mpeg12_decode_pts(mpeg12dec *d, const uint8_t *data, int len, int64_t pts,
                      mpeg12frame *out, int *got_frame);

/* End of stream: hand back the anchor picture still held for reordering.
 * Returns 1 and fills `out`, 0 when there is nothing left, negative on error. */
int mpeg12_flush(mpeg12dec *d, mpeg12frame *out);

/* Stream geometry once a sequence header has been seen (rc 0), else
 * MPEG12_ERR_CORRUPT. *is_mpeg2 is 1 when a sequence extension was present. */
int mpeg12_stream_info(mpeg12dec *d, int *w, int *h, double *fps, int *is_mpeg2);

/* A macroblock-type census, for the gate rather than for a player.
 *
 * It exists because "this case exercises dual prime" is a claim, and a
 * negative control aimed at dual prime is only meaningful against a count:
 * a control that reddens a case using none of the feature is measuring
 * something else, and a case that uses it zero times cannot redden at all.
 * The same argument covers field pictures, 16x8 and field DCT. */
typedef struct {
    long pictures;                /* coded pictures (a field picture is one) */
    long field_pictures;
    long mb_total, mb_intra, mb_skipped, mb_field_dct;
    long mv_frame, mv_field, mv_16x8, mv_dualprime;
    long coeff_saturated;         /* 7.4.2.3 clip fired: never on valid input */
    long mv_clamped;              /* MC read outside the coded picture */
    long escapes;                 /* Table B-14/B-15 escape codes decoded */
    long escapes_mpeg1_second;    /* MPEG-1's SECOND escape, |level| > 127 */
    long blocks_intra_vlc;        /* intra blocks decoded with Table B-15 */
} mpeg12_census;

void mpeg12_get_census(mpeg12dec *d, mpeg12_census *out);

#endif /* LOGIT_MPEG12_H */
