/* c/lib/audio/audio.h -- public API of the from-scratch audio decoders.
 *
 * Formats: WAV/PCM (exact), FLAC (lossless, bit-exact), MP3 (MPEG-1/2/2.5
 * Layer III), AAC-LC (ADTS here; a demuxer hands raw blocks straight to
 * aac_open_asc), and Vorbis I in Ogg. Every input byte is UNTRUSTED: a malformed file is an error
 * return, never a crash, never a read outside the buffer the caller handed in.
 *
 * WHY THIS IS A RING-3 LIBRARY, NOT A KERNEL ONE
 * c/lib/audio is filtered out of the kernel's C_SRC for the same reason
 * c/lib/video is (see the comment above C_SRC in the Makefile). An image is
 * decoded once, so the image codecs can sit behind SYS_IMG_DECODE; audio is
 * decoded continuously for the whole length of a track, holds hundreds of
 * kilobytes of live state per stream, and allocates with malloc/free. In the
 * kernel that would hold the big lock for the duration of playback and put a
 * parser of attacker-chosen bytes in ring 0. Consumers link AUD_OBJ plus
 * mini-libc, exactly as /bin/vidcheck and Preview link VID_OBJ.
 *
 * There are two ways in:
 *
 *   whole file    audio_decode(data, len, &pcm) -> one apcm, then
 *                 audio_pcm_free(&pcm). Simple, and what the tests use.
 *
 *   pull/stream   adec_open() then adec_read() into a caller buffer until it
 *                 returns 0. This is the shape a sound driver wants: the
 *                 player owns the ring buffer and asks for exactly as many
 *                 frames as it can queue. It decodes lazily, so a long track
 *                 costs one frame of work at a time rather than a whole
 *                 file's worth of PCM in memory.
 *
 * Sample format out is signed 16-bit host-endian, interleaved by channel --
 * the format a PCM layer and mixer take. Decoders that work internally in
 * floating point (MP3) can also hand back their pre-quantisation float output,
 * because that, not the rounded integers, is what the ISO conformance
 * criterion is defined on. See mp3.h.
 */
#ifndef LOGIT_AUDIO_H
#define LOGIT_AUDIO_H

#include <stdint.h>

#define AUDIO_OK               0
#define AUDIO_ERR_CORRUPT     -1   /* the bytes violate the format */
#define AUDIO_ERR_UNSUPPORTED -2   /* a valid file using something we do not do */
#define AUDIO_ERR_OOM         -3
#define AUDIO_ERR_RANGE       -4   /* self-consistent but absurd (guards allocation) */

/* Ceilings. A header field is a claim by the file, not a fact, and every one
 * of these is multiplied into an allocation size somewhere below. */
#define AUDIO_MAX_CHANNELS     8
#define AUDIO_MIN_RATE         1000
#define AUDIO_MAX_RATE         768000

typedef enum {
    AUDIO_FMT_UNKNOWN = 0,
    AUDIO_FMT_WAV,
    AUDIO_FMT_FLAC,
    AUDIO_FMT_MP3,
    AUDIO_FMT_AAC,         /* raw ADTS; MP4-carried AAC comes via aac_open_asc */
    AUDIO_FMT_VORBIS       /* in Ogg; Matroska-carried comes via
                            * vorbis_open_headers */
} audio_format;

/* A decoded buffer. `s16` is always present; `f32` only when the decoder is
 * natively floating point and the caller asked for it. `frames` counts sample
 * frames, so the number of int16 in s16 is frames*channels. */
typedef struct {
    int   rate;
    int   channels;
    int   bits;          /* source bit depth; 0 for a lossy source */
    long  frames;
    int16_t *s16;
    float   *f32;        /* may be NULL */
} apcm;

/* Bit flags for audio_decode(). */
#define AUDIO_WANT_FLOAT  1   /* also produce f32 (lossy sources only) */

/* Identify by content, never by file name. Returns AUDIO_FMT_UNKNOWN if the
 * bytes are not a container this library reads. */
audio_format audio_sniff(const uint8_t *data, long len);
const char  *audio_format_name(audio_format f);

/* Decode a whole file held in memory. Returns AUDIO_OK or AUDIO_ERR_*.
 * On success the caller owns *out and must audio_pcm_free() it. */
int  audio_decode(const uint8_t *data, long len, unsigned flags, apcm *out);
void audio_pcm_free(apcm *p);

/* Clamp-and-round a float sample buffer to int16. Rounds half away from zero
 * and saturates, which is what every decoder's output stage does; separated
 * out so the conformance tests can measure the float before it is applied. */
void audio_f32_to_s16(const float *in, int16_t *out, long n);

/* --- pull interface ---------------------------------------------------- */

typedef struct adec adec;

/* `data` must stay valid and unmodified for the life of the decoder: the
 * decoders read from it in place rather than copying the file. */
adec *adec_open(const uint8_t *data, long len, int *err);
void  adec_close(adec *d);

int   adec_info(const adec *d, int *rate, int *channels);
audio_format adec_format(const adec *d);

/* Decode up to `maxframes` frames into `out` (maxframes*channels int16).
 * Returns the number of frames written, 0 at end of stream, or AUDIO_ERR_*
 * (negative). A short return that is not 0 is normal -- ask again. */
long  adec_read(adec *d, int16_t *out, long maxframes);

/* Total frames if the container states it (WAV, FLAC STREAMINFO), else -1.
 * MP3 has no reliable count without scanning, so it reports -1. */
long  adec_duration_frames(const adec *d);

#endif /* LOGIT_AUDIO_H */
