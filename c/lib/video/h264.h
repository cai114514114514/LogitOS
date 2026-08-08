/* c/lib/video/h264.h -- public API of the hand-written H.264 decoder.
 *
 * Baseline profile, 4:2:0, 8-bit: CAVLC entropy, I/P slices, multiple
 * reference frames, weighted P prediction, full deblocking, frame cropping.
 * Everything the decoder cannot do (B slices, CABAC, fields/MBAFF, 8x8
 * transform, >8bit, SVC/MVC) is a clean H264_ERR_UNSUPPORTED, never a crash:
 * every input byte is UNTRUSTED.
 *
 * Usage (pull model, Annex B byte stream in):
 *   h264dec *d = h264_open();
 *   while (consumed < len) {
 *       h264frame f; int got = 0;
 *       int n = h264_decode(d, buf + consumed, len - consumed, &f, &got);
 *       if (n < 0) -> fatal decode error
 *       consumed += n;
 *       if (got) -> present f.y/f.u/f.v (valid until the next call)
 *   }
 *   while (h264_flush(d, &f)) -> present trailing frames from the DPB
 */
#ifndef LOGIT_H264_H
#define LOGIT_H264_H

#include <stdint.h>

#define H264_OK              0
#define H264_ERR_CORRUPT    -1   /* bitstream violates the spec */
#define H264_ERR_UNSUPPORTED -2  /* valid H.264, but a feature we do not do */
#define H264_ERR_OOM        -3

typedef struct h264dec h264dec;

/* "no timestamp": what a frame carries when the caller never supplied one. */
#define H264_NOPTS ((int64_t)(-0x7fffffffffffffffLL - 1))

/* One decoded picture, YUV 4:2:0 planar, 8 bits. Rows are `stride*` bytes;
 * visible area is width x height (SPS crop already accounted for: the planes
 * are macroblock-aligned, the caller displays from (0,0) with w/h). */
typedef struct {
    int width, height;            /* visible (cropped) size */
    int stride_y, stride_c;
    const uint8_t *y, *u, *v;
    int32_t poc;                  /* presentation order (display order) */
    /* The value the caller handed in with this picture's bytes, carried
     * through the decoder untouched. H264_NOPTS if none was given.
     *
     * This exists because B slices make decode order stop being display
     * order. Pairing frames with timestamps through a decode-order FIFO is
     * correct for a stream with no reordering and silently wrong for one with
     * B pictures -- and it fails in the way that is hardest to notice, since
     * every frame is present and only their times are shuffled. */
    int64_t pts;
} h264frame;

h264dec *h264_open(void);
void     h264_close(h264dec *d);

/* Decode from an Annex B stream. Returns the number of bytes consumed
 * (>= 0) and sets *got_frame = 1 when a picture completed (and filled `out`).
 * A single call consumes at most one picture's worth of NAL units; call again
 * with the remainder. Negative: H264_ERR_* and the decoder state is
 * undefined -- close it. */
int h264_decode(h264dec *d, const uint8_t *data, int len,
                h264frame *out, int *got_frame);

/* As h264_decode, but attaches `pts` to the picture whose data starts in this
 * call, and hands it back on that picture's h264frame however far out of
 * decode order it comes. Feed one access unit per call and the pairing is
 * exact; feed a larger run and every picture that starts inside it takes the
 * same value, which is the same contract a packet-based decoder API gives.
 *
 * `pts` is opaque: the decoder only stores and returns it. A caller that needs
 * more than a timestamp can pass an index into its own table.
 *
 * h264_decode is this with pts = H264_NOPTS, so existing callers keep working
 * and pick up the field when they are ready to supply it. */
int h264_decode_pts(h264dec *d, const uint8_t *data, int len, int64_t pts,
                    h264frame *out, int *got_frame);

/* End of stream: output whatever the DPB still holds. Returns 1 and fills
 * `out` per remaining picture (call until it returns 0). Negative on error. */
int h264_flush(h264dec *d, h264frame *out);

/* Stream geometry once an SPS has been seen (rc 0), else H264_ERR_CORRUPT.
 * fps is 0 when the stream carries no VUI timing info. */
int h264_stream_info(h264dec *d, int *w, int *h, double *fps);

#endif /* LOGIT_H264_H */
