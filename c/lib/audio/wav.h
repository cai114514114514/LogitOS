/* c/lib/audio/wav.h -- RIFF/WAVE reader and header writer.
 *
 * WAV is the instrument the rest of this library is checked with: it is the
 * one format whose "decoding" is exactly specified down to the last bit, so a
 * disagreement anywhere is unambiguously ours, and writing one is how decoded
 * output gets out of the OS and in front of a comparison tool.
 *
 * Reading is deliberately chunk-driven rather than "assume the 44-byte
 * canonical header": real files carry LIST/INFO/fact/cue chunks before `data`,
 * odd-sized chunks with a pad byte, and WAVE_FORMAT_EXTENSIBLE. Sizes in the
 * file are claims -- a chunk that says it is larger than the buffer, or an
 * fmt chunk whose block alignment disagrees with its own bit depth, is an
 * error, not something to trust and index with.
 */
#ifndef LOGIT_WAV_H
#define LOGIT_WAV_H

#include <stdint.h>
#include "audio.h"

typedef struct {
    int  rate;
    int  channels;
    int  bits;           /* 8, 16, 24 or 32 for PCM; 32 or 64 for float */
    int  is_float;       /* sample data is IEEE 754 rather than integer */
    int  frame_bytes;    /* bytes per sample frame = channels * bits/8 */
    long frames;         /* frames actually present in the data chunk */
    const uint8_t *data; /* points into the caller's buffer */
    long data_len;
} wavinfo;

/* Parse the header chain. Does not copy the samples. AUDIO_OK or AUDIO_ERR_*. */
int wav_parse(const uint8_t *buf, long len, wavinfo *w);

/* Convert frames [at, at+n) to interleaved int16, saturating. Returns the
 * number of frames converted (may be short at end of data), or AUDIO_ERR_*. */
long wav_read_s16(const wavinfo *w, long at, long n, int16_t *out);

/* Same, but to int32 at the source bit depth with no scaling at all -- 16-bit
 * samples come back in [-32768, 32767]. This is the exact path: it is how the
 * FLAC tests compare against the encoder's own input without a conversion
 * standing between them. Float sources are scaled by 2^(bits-1) and rounded.
 */
long wav_read_s32(const wavinfo *w, long at, long n, int32_t *out);

/* Build a 44-byte canonical PCM header for `frames` frames of 16-bit audio.
 * Returns 44, or AUDIO_ERR_RANGE if the parameters are out of range. */
int wav_header_s16(uint8_t out[44], int rate, int channels, long frames);

#endif /* LOGIT_WAV_H */
