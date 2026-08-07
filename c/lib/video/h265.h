/* c/lib/video/h265.h -- public API of the hand-written H.265/HEVC decoder.
 *
 * Main profile, 4:2:0, 8-bit: CABAC entropy, I/P/B slices, the full CTU
 * quadtree (CU 64..8, PU incl. AMP, TU quadtree), 35 intra modes, merge/AMVP
 * with temporal MV prediction, 8-tap luma / 4-tap chroma interpolation,
 * bi-prediction, explicit weighted prediction, transform skip, sign data
 * hiding, scaling lists, PCM, deblocking and SAO, tiles, wavefront entropy
 * sync and dependent slice segments.
 *
 * Everything outside that (Main10 and the rest of >8-bit, 4:2:2/4:4:4, the
 * range extensions, SCC, fields, multi-layer) is a clean H265_ERR_UNSUPPORTED,
 * never a crash: every input byte is UNTRUSTED.
 *
 * Deliberately the same shape as h264.h -- one decoder object, a pull model
 * over an Annex B byte stream -- with ONE difference that HEVC forces: B
 * slices mean decode order is not display order, so h265_decode() returns
 * pictures in OUTPUT order out of a real DPB (sps_max_num_reorder_pics), and
 * a picture may only surface several calls after the one that decoded it.
 *
 * Usage:
 *   h265dec *d = h265_open();
 *   while (consumed < len) {
 *       h265frame f; int got = 0;
 *       int n = h265_decode(d, buf + consumed, len - consumed, &f, &got);
 *       if (n < 0) -> fatal decode error
 *       consumed += n;
 *       if (got) -> present f.y/f.u/f.v (valid until the next call)
 *   }
 *   while (h265_flush(d, &f) == 1) -> present the pictures still in the DPB
 */
#ifndef LOGIT_H265_H
#define LOGIT_H265_H

#include <stdint.h>

#define H265_OK               0
#define H265_ERR_CORRUPT     -1   /* bitstream violates the spec */
#define H265_ERR_UNSUPPORTED -2   /* valid HEVC, but a feature we do not do */
#define H265_ERR_OOM         -3

typedef struct h265dec h265dec;

/* One decoded picture, YUV 4:2:0 planar, 8 bits. Rows are `stride*` bytes;
 * the visible area is width x height with the SPS conformance window already
 * applied, so the caller displays from (0,0). */
typedef struct {
    int width, height;            /* visible (cropped) size */
    int stride_y, stride_c;
    const uint8_t *y, *u, *v;
    int32_t poc;                  /* presentation order */
} h265frame;

h265dec *h265_open(void);
void     h265_close(h265dec *d);

/* Decode from an Annex B stream. Returns the number of bytes consumed (>= 0)
 * and sets *got_frame = 1 when a picture became ready for OUTPUT (and filled
 * `out`). A single call stops as soon as it can emit a picture; call again
 * with the remainder. Negative: H265_ERR_* and the decoder state is undefined
 * -- close it. */
int h265_decode(h265dec *d, const uint8_t *data, int len,
                h265frame *out, int *got_frame);

/* End of stream: drain the DPB in output (POC) order. Returns 1 and fills
 * `out` per remaining picture (call until it returns 0). Negative on error. */
int h265_flush(h265dec *d, h265frame *out);

/* Stream geometry once an SPS has been seen (rc 0), else H265_ERR_CORRUPT.
 * fps is 0 when the stream carries no VUI timing info. */
int h265_stream_info(h265dec *d, int *w, int *h, double *fps);

#endif /* LOGIT_H265_H */
