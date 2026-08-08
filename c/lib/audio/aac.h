/* c/lib/audio/aac.h -- from-scratch MPEG-2/4 AAC Low Complexity decoder.
 *
 * WHY AAC IS FIRST OF THE THREE.  It is what essentially every MP4 carries,
 * and a container line landing MP4/MKV demuxing without it delivers video with
 * no sound. Vorbis and Opus only matter once you can also read Ogg/WebM.
 *
 * WHAT "CORRECT" MEANS HERE, AND WHAT IT DOES NOT.  AAC is the same shape as
 * MP3 and not the same shape as FLAC: reconstruction runs through an IMDCT and
 * a floating-point windowed overlap-add, so ISO/IEC 14496-3 does not define a
 * bit pattern for the output and no honest decoder claims one. 14496-4 (and
 * 13818-4 before it) define conformance as a bound on the difference from a
 * reference decoder, with the same "full accuracy" limits MPEG-1 Layer III
 * uses: RMS of the difference below 2^-15/sqrt(12) and maximum absolute
 * difference at or below 2^-15, on a full scale of 1.0. tests/unit/aac_test.c
 * measures against those and reports the whole error distribution.
 *
 * DO NOT READ THAT AS BIT-EXACTNESS. It is not, and it cannot be. The official
 * ISO conformance bitstreams are not redistributable and are not present here;
 * what the test runs is a DIFFERENTIAL against ffmpeg's float AAC decoder,
 * scored against the published bounds, and it says so in its own output.
 *
 * IMPLEMENTED
 *   AAC-LC (object type 2) in ADTS and in raw form with an
 *   AudioSpecificConfig, which is the pair a demuxer hands over. 1024-sample
 *   frames at all twelve sampling frequencies. Long, start, eight-short and
 *   stop window sequences with both window shapes (sine and KBD), window
 *   grouping, all eleven spectral codebooks including the codebook 11 escape,
 *   both scalefactor coding paths, pulse data, TNS (both coefficient
 *   resolutions and both compressions, forward and backward), M/S stereo,
 *   intensity stereo, and PNS. SCE, CPE, LFE, DSE, PCE and FIL elements; up to
 *   eight channels.
 *
 * NOT IMPLEMENTED (each a clean error or a documented degradation, never a
 * crash or silent garbage)
 *   SBR and PS -- that is, HE-AAC v1 and v2. An AudioSpecificConfig that
 *   explicitly signals object type 5 or 29 is refused with
 *   AUDIO_ERR_UNSUPPORTED rather than silently decoded as its core, because
 *   the core alone is the right samples at half the intended rate and that is
 *   a wrong answer that sounds nearly right. SBR arriving IMPLICITLY inside a
 *   FIL element is skipped and the core is decoded, and aac_had_sbr() reports
 *   that it happened so a caller can say so.
 *   Main profile prediction, LTP, SSR, scalable and ER syntax, LATM/LOAS
 *   framing, and the 960-sample frame length.
 *
 * Output is double internally and float out, deliberately and for the reason
 * mp3.h gives: the conformance criterion is defined on the decoder's
 * floating-point output before it is quantised to an integer sample format.
 */
#ifndef LOGIT_AAC_H
#define LOGIT_AAC_H

#include <stdint.h>
#include "audio.h"

#define AAC_MAX_CHANNELS  8
#define AAC_FRAME_LEN     1024

typedef struct aacdec aacdec;

typedef struct {
    int rate;
    int channels;
    int nsamples;              /* per channel; always 1024 for LC */
    const float *pcm;          /* interleaved, nsamples*channels, valid until
                                * the next aac_decode* call */
} aacframe;

/* An ADTS decoder: the geometry comes from each frame's own header. */
aacdec *aac_open(void);

/* A raw decoder configured from an MP4/MKV AudioSpecificConfig. This is the
 * entry point a demuxer uses: it hands over the ASC once and then a sequence
 * of raw_data_block payloads with no ADTS headers on them. */
aacdec *aac_open_asc(const uint8_t *asc, long len, int *err);

void aac_close(aacdec *d);

/* Decode one ADTS frame starting at data[0] (leading garbage is skipped up to
 * the next syncword). Returns the number of bytes consumed, 0 if there is not
 * a whole frame in the buffer, or a negative AUDIO_ERR_*. *got is set to 1
 * when out was filled. */
int aac_decode(aacdec *d, const uint8_t *data, long len, aacframe *out, int *got);

/* Decode one raw_data_block of exactly `len` bytes (the demuxer's frame). */
int aac_decode_raw(aacdec *d, const uint8_t *data, long len, aacframe *out, int *got);

/* Geometry, valid once a frame has been decoded (or immediately after
 * aac_open_asc). */
int aac_info(const aacdec *d, int *rate, int *channels);

/* 1 if a FIL element carrying an SBR payload has been seen and skipped, so the
 * output is the core at half the intended sampling rate. */
int aac_had_sbr(const aacdec *d);

/* 1 if any band in the stream so far used perceptual noise substitution. A
 * PNS band carries an ENERGY, not samples: the decoder synthesises its own
 * noise to fill it, so two conformant decoders differ there by design and a
 * sample-for-sample comparison of such a stream measures nothing. Callers that
 * score against a reference must ask. */
int aac_had_pns(const aacdec *d);

/* Length of the ADTS frame whose header starts at data[0], or 0 if that is not
 * a plausible ADTS header. Used for sniffing and for framing. */
long aac_adts_frame_len(const uint8_t *data, long len);

#endif /* LOGIT_AAC_H */
