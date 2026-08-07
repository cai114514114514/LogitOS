/* c/lib/audio/flac.h -- from-scratch FLAC decoder.
 *
 * FLAC is lossless, so the bar is bit-exactness and nothing else. Better
 * still, the format carries its own conformance criterion: STREAMINFO holds
 * the MD5 of the unencoded interleaved little-endian PCM, so a decoder can
 * prove it reconstructed the original samples exactly WITHOUT any external
 * reference decoder. flac_md5_ok() is that check, and it is the strongest
 * statement in this library: it holds on the guest, where there is no ffmpeg
 * to compare against.
 *
 * Implemented: STREAMINFO, all four subframe kinds (constant, verbatim, fixed
 * orders 0-4, LPC orders 1-32), both residual coding methods, partitioned Rice
 * with escaped partitions, wasted bits, all stereo decorrelations, both
 * blocking strategies, 4-32 bit samples, and the CRC-8 header / CRC-16 frame
 * checks (which are what turns a corrupted download into an error return
 * rather than noise). Not implemented: Ogg-encapsulated FLAC (there is no
 * demuxer line yet) and 33-bit-per-sample streams.
 */
#ifndef LOGIT_FLAC_H
#define LOGIT_FLAC_H

#include <stdint.h>
#include "audio.h"

#define FLAC_MAX_LPC_ORDER 32

typedef struct flacdec flacdec;

/* `data` must outlive the decoder: frames are decoded out of it in place. */
flacdec *flac_open(const uint8_t *data, long len, int *err);
void     flac_close(flacdec *d);

int  flac_info(const flacdec *d, int *rate, int *channels, int *bits, long *total);

/* Decode the next frame. On success returns the frame count (>0) and points
 * planes[c] at the decoder's channel buffers, valid until the next call.
 * Returns 0 at end of stream, or a negative AUDIO_ERR_*. Samples are at the
 * stream's native bit depth, sign-extended into int32. */
long flac_decode_frame(flacdec *d, const int32_t *planes[]);

/* Rewind to the first audio frame and reset the running MD5. */
void flac_rewind(flacdec *d);

/* The format's own conformance check. Decodes the whole stream from the
 * current position, feeds every sample to MD5 in the exact byte order the
 * spec defines, and compares with STREAMINFO. Returns 1 on match, 0 on
 * mismatch, or a negative AUDIO_ERR_* if decoding failed. Returns
 * AUDIO_ERR_UNSUPPORTED when the file's MD5 field is all zero, which the spec
 * permits and which means the file makes no claim to check. */
int flac_md5_ok(flacdec *d);

#endif /* LOGIT_FLAC_H */
