/* c/lib/video/mpeg4.h -- public API of the from-scratch MPEG-4 Part 2
 * (Simple + Advanced Simple profile) and ITU-T H.263 video decoder.
 *
 * ONE DECODER, TWO SYNTAXES.  MPEG-4 Part 2's "short video header" mode IS
 * H.263 baseline: same macroblock layer, same VLCs, same half-pel motion
 * compensation, a different picture header.  Splitting them into two decoders
 * would duplicate every table and every macroblock path and then let the two
 * copies drift; here the picture header is the only fork.
 *
 * What it decodes:
 *   MPEG-4 Part 2  VOS/VO/VOL/GOV/VOP headers; I-, P- and B-VOPs; the four-MV
 *                  mode; quarter-pel motion compensation (ASP) with the 8-tap
 *                  filter and both roundings; AC/DC prediction with the
 *                  scan-order switch; both quantiser types (H.263 and MPEG);
 *                  interlaced VOPs (field DCT and 16x8 field MC) in ASP;
 *                  resync markers and video packet headers; data partitioning
 *                  with and without RVLC; B-VOP direct mode with the TRB/TRD
 *                  arithmetic; custom intra/inter quantiser matrices.
 *   H.263          picture header (source formats sub-QCIF..16CIF and H.263+
 *                  custom picture format), I/P pictures, four-MV, half-pel MC,
 *                  Advanced Intra Coding, alternative inter VLC, modified
 *                  quantisation, unrestricted motion vectors, the deblocking
 *                  loop filter, and long vectors.
 *
 * What it REFUSES BY NAME, never silently:
 *   GMC / sprite warping (vol_sprite_usage != 0, S-VOPs) -- MPEG4_ERR_GMC
 *   Simple Studio profile (vo_type 0x0E/0x0F, 10/12-bit 4:2:2)
 *   scalability (any of the scalable extensions), new_pred, reduced-resolution
 *   VOPs, non-rectangular and binary/grey shape, N-bit (bits_per_pixel != 8),
 *   H.263 Annex G/M PB-frames, H.263 slice-structured mode, H.263 Annex D SAC,
 *   OBMC (H.263 Advanced Prediction), H.263 B-pictures (Annex O),
 *   Intel/I263 and the DivX packed-bitstream container hack.
 * Everything above returns MPEG4_ERR_UNSUPPORTED (or MPEG4_ERR_GMC) and names
 * itself through mpeg4_last_error(); nothing is concealed and nothing is
 * guessed.  Every input byte is UNTRUSTED.
 *
 * Usage (pull model, a video elementary stream in):
 *   mpeg4dec *d = mpeg4_open(MPEG4_FLAVOR_AUTO);
 *   while (consumed < len) {
 *       mpeg4frame f; int got = 0;
 *       int n = mpeg4_decode(d, buf + consumed, len - consumed, &f, &got);
 *       if (n < 0) -> fatal decode error
 *       consumed += n;
 *       if (got) -> present f.y/f.u/f.v (valid until the next call)
 *   }
 *   while (mpeg4_flush(d, &f) == 1) -> the picture held for reordering
 *
 * A call consumes at most one coded picture.  The buffer handed in must
 * contain whole pictures: the decoder treats the end of the buffer as the end
 * of the last picture in it, which is right when the caller feeds a whole
 * elementary stream and wrong if it feeds arbitrary chunks.
 */
#ifndef LOGIT_MPEG4_H
#define LOGIT_MPEG4_H

#include <stdint.h>

#define MPEG4_OK               0
#define MPEG4_ERR_CORRUPT     -1   /* bitstream violates the standard */
#define MPEG4_ERR_UNSUPPORTED -2   /* legal, but a feature we do not do */
#define MPEG4_ERR_OOM         -3
#define MPEG4_ERR_GMC         -4   /* sprite warping, refused by name */

/* Which syntax the elementary stream is in. AUTO sniffs the first bytes:
 * 00 00 01 -> MPEG-4 Part 2 start code, otherwise the 22-bit H.263 PSC. */
#define MPEG4_FLAVOR_AUTO  0
#define MPEG4_FLAVOR_MPEG4 1
#define MPEG4_FLAVOR_H263  2

/* Picture coding types as coded in the VOP header. */
#define MPEG4_PICT_I 1
#define MPEG4_PICT_P 2
#define MPEG4_PICT_B 3
#define MPEG4_PICT_S 4   /* sprite (GMC) -- refused */

#define MPEG4_NOPTS ((int64_t)(-0x7fffffffffffffffLL - 1))

typedef struct mpeg4dec mpeg4dec;

typedef struct {
    int width, height;      /* display size, cropped from the coded MB grid */
    int stride_y, stride_c;
    const uint8_t *y, *u, *v;
    int64_t pts;            /* what the caller handed in, carried through */
    int coding_type;        /* MPEG4_PICT_* of the coded picture */
    int interlaced;         /* the VOP was coded as an interlaced frame */
    int top_field_first;
} mpeg4frame;

mpeg4dec *mpeg4_open(int flavor);
void      mpeg4_close(mpeg4dec *d);

/* Decode. Returns bytes consumed (>= 0) and sets *got_frame when a picture
 * came out in DISPLAY order. Negative: MPEG4_ERR_*; the decoder state is
 * undefined afterwards, close it. */
int mpeg4_decode(mpeg4dec *d, const uint8_t *data, int len,
                 mpeg4frame *out, int *got_frame);

/* As mpeg4_decode, but attaches `pts` to the picture that starts in this call
 * and hands it back on that picture however far out of decode order it comes
 * -- which for a stream with B-VOPs is most of them. */
int mpeg4_decode_pts(mpeg4dec *d, const uint8_t *data, int len, int64_t pts,
                     mpeg4frame *out, int *got_frame);

/* Drain the reordering delay. 1 = a frame came out, 0 = done. */
int mpeg4_flush(mpeg4dec *d, mpeg4frame *out);

/* A human-readable name for the last refusal or error. Never NULL. */
const char *mpeg4_last_error(const mpeg4dec *d);

#endif /* LOGIT_MPEG4_H */
