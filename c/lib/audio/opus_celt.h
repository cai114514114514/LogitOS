/* c/lib/audio/opus_celt.h -- the CELT half of an Opus decoder, RFC 6716 4.3.
 *
 * WHY CELT FIRST AND SILK SECOND.  Not because CELT is easier -- it is not --
 * but because of what the web actually serves. A WebM/Matroska audio track is
 * Opus far more often than it is anything else, and an Opus stream carrying
 * music or any wideband content is coded CELT-only in fullband mode. SILK is
 * the speech half: it is what a voice call uses, and it is what the hybrid
 * modes put underneath CELT below 8 kHz. Landing CELT completely is the half
 * that makes a video's audio track play; landing SILK first would have made
 * telephone speech play and left every music track silent.
 *
 * The corpus says the same thing without being asked. Of the twelve official
 * test vectors, three (01, 07, 11) are pure CELT and cover every CELT
 * configuration there is -- all four frame sizes, all four bandwidths, mono
 * and stereo, transient and not. Six others are SILK or hybrid. See the table
 * in tests/opus.mk, which is generated from the vectors rather than asserted.
 *
 * SCOPE, stated so that nothing here has to be inferred:
 *   IN   CELT-only frames at 48 kHz, mono and stereo, 2.5/5/10/20 ms,
 *        narrowband through fullband, coarse+fine+final band energy, the
 *        full bit allocation including dynalloc/trim/skip/intensity/dual
 *        stereo, PVQ with the exact pulse algebra, spreading, the
 *        time-frequency (tf) resolution changes, anti-collapse, the
 *        post-filter, and the low-overlap MDCT with its overlap-add.
 *   OUT  SILK frames and the SILK half of hybrid frames -- refused BY NAME
 *        by opus.c, never silently rendered as silence.
 *   OUT  Packet loss concealment. No frame in any of the twelve vectors has
 *        a payload of one byte or less (measured: 0 of 5524, 4186 and 1501
 *        frames in vectors 01/07/11), so a PLC here would be code no gate
 *        could ever watch fail. A lost frame yields silence of the correct
 *        duration, which keeps the timeline right and is what a file-based
 *        player needs; it is named in the API rather than dressed up.
 *   OUT  Output rates other than 48 kHz. The Opus CELT mode is DEFINED at
 *        48 kHz internally (the reference builds mode48000_960_120 whatever
 *        the output rate is and resamples afterwards), so this is a missing
 *        resampler, not a missing decoder.
 *
 * ARITHMETIC. Every quantity that feeds the bitstream parse is exact integer
 * here, matching the reference's fixed-point path bit for bit -- the range
 * decoder, the Laplace model, the pulse cache, the whole of the bit
 * allocation, compute_qn, bitexact_cos and bitexact_log2tan, the PVQ index
 * algebra, and the LCG that seeds spectral folding. Reconstruction (band
 * denormalisation, the IMDCT, the window, the post-filter, de-emphasis) is
 * double, which is the reference's FLOAT path widened. That split is not a
 * compromise between the two builds; it is where the two builds actually
 * differ. See the top of opus.c.
 */
#ifndef LOGIT_OPUS_CELT_H
#define LOGIT_OPUS_CELT_H

#include <stdint.h>
#include "opus_range.h"

#define CELT_NBANDS      21
#define CELT_OVERLAP     120
#define CELT_SHORT_MDCT  120
#define CELT_MAXLM       3
#define CELT_MAX_FRAME   (CELT_SHORT_MDCT << CELT_MAXLM)   /* 960 */

typedef struct opus_celt_dec opus_celt_dec;

/* channels is the number of channels the caller wants out (1 or 2); a frame
 * may be coded with fewer, and is up-mixed, exactly as the reference does. */
opus_celt_dec *opus_celt_create(int channels);
void           opus_celt_destroy(opus_celt_dec *st);
void           opus_celt_reset(opus_celt_dec *st);

/* The band range the frame occupies. `start` is 17 for the CELT half of a
 * hybrid frame and 0 otherwise; `end` follows the bandwidth. Set before each
 * decode, as the reference's CELT_SET_START_BAND / CELT_SET_END_BAND do. */
void opus_celt_set_bands(opus_celt_dec *st, int start, int end);
void opus_celt_set_stream_channels(opus_celt_dec *st, int c);

/* Decode one CELT frame out of an ALREADY-INITIALISED range decoder -- the
 * decoder is shared with SILK in a hybrid frame, so it cannot be created
 * here. frame_size is in samples per channel at 48 kHz and must be one of
 * 120/240/480/960. Writes frame_size*channels interleaved doubles, each
 * already scaled so that +-32768 is full scale (the reference's `celt_sig`
 * units), and returns frame_size, or negative on error. */
int  opus_celt_decode(opus_celt_dec *st, orange *dec, const uint8_t *data,
                      int len, double *pcm, int frame_size);

/* A frame that is lost or is a DTX filler. Emits silence and advances the
 * state; see the PLC note above -- it fabricates nothing. */
int  opus_celt_conceal(opus_celt_dec *st, double *pcm, int frame_size);

/* The range coder state after the last decoded frame. This is the exact
 * 32-bit checksum the test vectors carry; see opus_range.h. */
uint32_t opus_celt_final_range(const opus_celt_dec *st);

#endif /* LOGIT_OPUS_CELT_H */
