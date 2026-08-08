/* c/lib/video/h265.h -- public API of the hand-written H.265/HEVC decoder.
 *
 * Main and Main 10, 4:2:0, 8 or 10 bits per sample: CABAC entropy, I/P/B
 * slices, the full CTU quadtree (CU 64..8, PU incl. AMP, TU quadtree), 35
 * intra modes, merge/AMVP with temporal MV prediction, 8-tap luma / 4-tap
 * chroma interpolation, bi-prediction, explicit weighted prediction, transform
 * skip, sign data hiding, deblocking and SAO, tiles, wavefront entropy sync
 * and dependent slice segments.
 *
 * Everything outside that (12-bit and above, monochrome, 4:2:2/4:4:4, unequal
 * luma/chroma bit depths, PCM, the range extensions, SCC, fields, multi-layer)
 * is a clean H265_ERR_UNSUPPORTED, never a crash: every input byte is
 * UNTRUSTED. All of those refusals are verified, not assumed -- x265 will
 * encode monochrome, 4:2:2 and 4:4:4 at both depths, and each one is checked
 * to come back H265_ERR_UNSUPPORTED rather than a wrong picture.
 *
 * TWO THINGS THIS LIST USED TO CLAIM AND SHOULD NOT HAVE:
 *   scaling lists  are parsed, and do not reconstruct bit-exactly at EITHER
 *                  depth. No case in the 8-bit test matrix ever used a
 *                  non-flat list, so the feature was claimed and never
 *                  measured; `make test-h265-scaling` now puts a number on it.
 *   PCM            is refused outright in h265_nal.c, and always was. It is
 *                  not merely theoretical either: the ITU conformance stream
 *                  DBLK_A_MAIN10_VIXS_4 uses it.
 *
 * B slices decode but are NOT bit-exact at either depth; they are gated
 * separately (`make test-h265-b`) rather than counted in `make test-h265`.
 * Since real streaming content is B-heavy, treat "HEVC works" as a claim about
 * I/P material only until that is fixed. The Main 10 conformance stream this
 * tree gates on (WP_A_MAIN10_Toshiba_3) is 1 I + 255 P and does NOT cover it.
 *
 * ===================== MEMORY: READ THIS BEFORE 1080p =====================
 * Samples are 16 bits at every depth, and a reference picture carries a
 * H265_PAD = 80 sample border on all four sides. Per DECODED PICTURE:
 *
 *      416x240   ~0.6 MB        1280x720  ~4.0 MB
 *      832x480   ~1.8 MB        1920x1080 ~7.9 MB
 *
 * and the DPB holds up to sps_max_dec_pic_buffering of them at once (16 + the
 * current picture in the worst case). Measured peak RSS decoding 1080p is
 * 62 MB at BOTH 8 and 10 bits.
 *
 * mini-libc's malloc arena is 24 MiB (c/apps/libc/src/malloc.c, ARENA_SIZE).
 * So 1080p DOES NOT FIT ON THE DEVICE at either bit depth, and did not fit
 * before the 10-bit work either -- widening did not cause this, it only made
 * it about 80% worse (34.5 MB -> 62.2 MB peak for the same 1080p 8-bit
 * stream). Anything on LogitOS that decodes video must either stay at or
 * below roughly 720p with a shallow DPB, or grow the arena first.
 *
 * Cheapest real saving, if someone needs it: H265_PAD is 80 and only exists
 * so that ordinary motion vectors can read past the picture edge without
 * taking the emulate() path. At 1080p that border is 24% of the luma plane.
 * Dropping it to 16 saves about 20% of the whole DPB (7.9 -> 6.3 MB per
 * picture) and costs only SPEED: h265_mc.c already clamps every out-of-range
 * fetch through emulate(), which is bit-exact with the padded read, so
 * correctness does not depend on the border at all -- it would just be taken
 * more often. Halving again would mean storing 8-bit streams in 8 bits, which
 * re-introduces the dual sample type this decoder deliberately does not have.
 * ==========================================================================
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

/* One decoded picture, YUV 4:2:0 planar. The visible area is width x height
 * with the SPS conformance window already applied, so the caller displays from
 * (0,0), and `stride_y`/`stride_c` are counted in SAMPLES -- which is the same
 * number for both views below, so existing 8-bit indexing arithmetic is
 * unchanged.
 *
 * TWO VIEWS OF THE SAME PICTURE:
 *
 *   y16/u16/v16  the decoder's own samples, at the stream's real precision.
 *                ALWAYS non-NULL. `bit_depth` says how many of the 16 bits are
 *                significant (8 or 10); values are 0..(1 << bit_depth) - 1.
 *                THIS is the view to use for anything that must be exact --
 *                a CRC, a comparison against a reference decoder, a re-encode.
 *
 *   y/u/v        an 8-bit DISPLAY view, always non-NULL, always populated.
 *                When bit_depth == 8 it is the samples exactly. When
 *                bit_depth > 8 it is a rounded down-conversion, which is
 *                LOSSY and is a renderer's convenience, never a decode
 *                result: nothing inside the decoder ever reads it back.
 *
 * The split is deliberate. Widening the decoder to 10 bits must not silently
 * change what an existing 8-bit caller sees, and must not silently hand a
 * bit-exactness test a truncated picture and let it pass. So the old fields
 * keep their old type and their old meaning, and exactness moved to a new
 * name that a caller has to opt into by typing it.
 *
 * Both views stay valid until the next h265_decode()/h265_flush() call. */
typedef struct {
    int width, height;            /* visible (cropped) size */
    int stride_y, stride_c;       /* in SAMPLES, valid for both views */
    const uint8_t *y, *u, *v;     /* 8-bit display view (lossy if depth > 8) */
    int32_t poc;                  /* presentation order */
    int bit_depth;                /* 8 or 10: significant bits in y16/u16/v16 */
    const uint16_t *y16, *u16, *v16;   /* the decoder's real samples */
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
