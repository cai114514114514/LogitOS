/* c/lib/audio/mp3.h -- from-scratch MPEG-1/2/2.5 Layer III decoder.
 *
 * WHY MP3 AND NOT AAC, VORBIS OR OPUS.  Three reasons, in order. It needs no
 * container, so it is decodable today with no demuxer line: a bare .mp3 is a
 * sequence of self-delimiting frames, whereas Vorbis and Opus only exist
 * inside Ogg and AAC in practice only inside MP4. It has a conformance
 * criterion that is a number rather than an opinion -- ISO/IEC 11172-4 defines
 * a limited-accuracy and a full-accuracy bound on the difference from the
 * reference decoder, which is exactly the kind of bar the H.264 line set. And
 * it is the format most likely to already be on a user's disk.
 *
 * WHAT "CORRECT" MEANS HERE, AND WHAT IT DOES NOT.  Layer III reconstructs
 * through an IMDCT and a polyphase synthesis filter bank in floating point.
 * The standard therefore does NOT define a bit-exact output, and no honest
 * decoder claims one: 11172-4 defines conformance as a bound on the difference
 * from the reference decoder's output. This decoder is measured against that
 * bound and the score is reported; see tests/unit/mp3_test.c. Do not read
 * "passes conformance" here as "bit-identical to ffmpeg", because that is not
 * a property the format has.
 *
 * IMPLEMENTED
 *   MPEG-1, MPEG-2 (LSF) and MPEG-2.5 Layer III, all sampling rates and
 *   bitrates including free-format-free CBR and VBR; mono, stereo, dual
 *   channel and joint stereo (MS and intensity); long, start, short and stop
 *   blocks including mixed blocks; the bit reservoir; both scalefactor coding
 *   schemes; preflag and scalefac_scale; alias reduction; the full 36- and
 *   12-point IMDCTs; the 512-tap polyphase synthesis window; CRC-protected
 *   frames (the CRC is checked and a failing frame is refused).
 *
 * NOT IMPLEMENTED (each is a clean error, never a crash or silent garbage)
 *   Layer I and Layer II, free-format streams (bitrate_index 0), and MPEG
 *   Multichannel. Gapless playback metadata (Xing/LAME encoder delay and
 *   padding) is parsed only far enough to recognise and skip an info frame;
 *   this decoder does not trim the encoder delay, because trimming would make
 *   its output no longer comparable sample-for-sample with a reference.
 *
 * Output is float, deliberately. The ISO criterion is defined on the decoder's
 * floating-point output before it is quantised to an integer sample format, so
 * quantising inside the decoder would destroy the thing being measured.
 * audio_f32_to_s16() does that last step for the PCM layer.
 */
#ifndef LOGIT_MP3_H
#define LOGIT_MP3_H

#include <stdint.h>
#include "audio.h"

#define MP3_MAX_SAMPLES 1152      /* per channel, MPEG-1 */

typedef struct mp3dec mp3dec;

typedef struct {
    int rate;
    int channels;
    int nsamples;              /* per channel: 1152 (MPEG-1) or 576 (MPEG-2/2.5) */
    int bitrate_kbps;
    const float *pcm;          /* interleaved, nsamples*channels, valid until the
                                * next mp3_decode() call */
} mp3frame;

mp3dec *mp3_open(void);
void    mp3_close(mp3dec *d);

/* Decode from a byte stream. Returns the number of bytes consumed (> 0) and
 * sets *got = 1 when a frame was produced. Returns 0 when `len` does not yet
 * hold a whole frame (feed more). Negative values are AUDIO_ERR_*; only
 * AUDIO_ERR_OOM is fatal, a corrupt frame is reported by resynchronising and
 * consuming the bad bytes, which is what a decoder must do for a file that was
 * cut out of a stream.
 *
 * Frames whose main data is not yet available (the first frames after a seek,
 * because Layer III's bit reservoir lets a frame's data live in its
 * predecessors) consume bytes and return with *got = 0. That is not an error;
 * it is the format. */
int mp3_decode(mp3dec *d, const uint8_t *data, long len, mp3frame *out, int *got);

/* Bytes of the leading ID3v2 tag, if any, so a caller can skip it. Returns 0
 * when there is none. */
long mp3_id3_len(const uint8_t *data, long len);

/* True if the frame beginning at `data` is a Xing/Info/VBRI header frame --
 * a real Layer III frame whose payload is all zeros and which carries stream
 * metadata. Encoders emit one at the head of the file; it decodes to silence
 * and is not part of the audio. */
int mp3_is_info_frame(const uint8_t *data, long len);

#endif /* LOGIT_MP3_H */
