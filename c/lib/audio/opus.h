/* c/lib/audio/opus.h -- an Opus decoder (RFC 6716), CELT half.
 *
 * See the top of opus.c for the conformance bar, and opus_celt.h for what is
 * implemented and what is refused. In one line: CELT-only frames decode;
 * SILK and hybrid frames are REFUSED BY NAME and never rendered as silence,
 * because a player that gets silence cannot tell "this codec does not do
 * speech yet" from "this file is quiet".
 */
#ifndef LOGIT_OPUS_H
#define LOGIT_OPUS_H

#include <stdint.h>

/* Opus is always 48 kHz on the wire; the internal sample rate a frame is
 * coded at is a property of the frame, not of the stream. This decoder
 * outputs 48 kHz and only 48 kHz (see opus_celt.h -- lower output rates are
 * a missing resampler, not a missing decoder). */
#define OPUS_RATE 48000

/* 1275 IS THE PER-FRAME LIMIT, NOT THE PER-PACKET LIMIT, and conflating the
 * two is a real bug this corpus caught: with a 1275-byte packet bound, vector
 * 11 stopped after 3 of its 553 packets and vector 10 after 1 -- both are
 * full of code-3 multi-frame packets, and their largest are 1485 and 1496
 * bytes. RFC 6716 3.4 bounds a FRAME at 1275; a packet may carry up to 48 of
 * them (120 ms of audio at the 2.5 ms minimum frame size), so the packet
 * bound is 48*1275 plus framing, and it is derived here rather than guessed:
 *
 *     48 frames x 1275                      61200
 *     TOC + frame-count byte                    2
 *     two length bytes for 47 frames           94
 *     rounded up                            61440
 *
 * The 48 itself follows from the duration cap, and opus_packet_parse checks
 * BOTH -- the duration, as the reference does, and the array bound directly,
 * because an array size that is safe only via an arithmetic identity three
 * lines away is the kind that stops being safe when a line moves. */
#define OPUS_MAX_FRAME_BYTES 1275
#define OPUS_MAX_FRAMES        48
#define OPUS_MAX_PACKET     61440
#define OPUS_MAX_SAMPLES     5760

typedef struct opus_dec opus_dec;

/* Errors. Negative, distinct, and the two refusals are distinct from each
 * other so a caller can report which half of the codec a file needed. */
#define OPUS_OK             0
#define OPUS_E_BADARG      -1
#define OPUS_E_PACKET      -2   /* malformed TOC / frame lengths */
#define OPUS_E_UNSUP_SILK  -3   /* a SILK-only frame */
#define OPUS_E_UNSUP_HYBRID -4  /* a hybrid frame (SILK under CELT) */
#define OPUS_E_NOMEM       -5
#define OPUS_E_INTERNAL    -6

/* Modes a packet's TOC can name. */
#define OPUS_MODE_SILK   0
#define OPUS_MODE_HYBRID 1
#define OPUS_MODE_CELT   2

/* Bandwidths, in the RFC's order. */
#define OPUS_BW_NARROW      0
#define OPUS_BW_MEDIUM      1
#define OPUS_BW_WIDE        2
#define OPUS_BW_SUPERWIDE   3
#define OPUS_BW_FULL        4

/* --- the packet layer, usable without a decoder ------------------------- */
/* RFC 6716 section 3. These read the TOC byte and the frame-length encoding
 * and touch no decoder state, which is what makes them testable on their own
 * and what lets a demuxer decide whether it can play a track before it
 * allocates anything. */

typedef struct {
    uint8_t  toc;
    int      mode;          /* OPUS_MODE_* */
    int      bandwidth;     /* OPUS_BW_* */
    int      stereo;        /* 1 if the TOC's s bit is set */
    int      frame_samples; /* per frame, at 48 kHz */
    int      nframes;       /* 1..48 */
    int      code;          /* the TOC's 2-bit frame-count code */
    int      padding;       /* padding bytes removed, code 3 only */
    const uint8_t *frame[OPUS_MAX_FRAMES];
    int      size[OPUS_MAX_FRAMES];
} opus_packet;

/* Parses one packet. Returns OPUS_OK or a negative error. `data` must stay
 * alive as long as `p` is used: p->frame[] points into it. */
int opus_packet_parse(const uint8_t *data, int len, opus_packet *p);

/* Samples per frame at 48 kHz for a TOC byte; 0 if the byte is impossible. */
int opus_packet_frame_samples(uint8_t toc);

/* Total samples a packet decodes to, per channel; negative on error. Cheap:
 * it reads the TOC and the frame count only. */
int opus_packet_nb_samples(const uint8_t *data, int len);

/* --- the decoder -------------------------------------------------------- */

/* channels is 1 or 2 and is the OUTPUT channel count; a packet coded with a
 * different number is up- or down-mixed, as the reference does. */
opus_dec *opus_decoder_create(int channels, int *error);
void      opus_decoder_destroy(opus_dec *st);
void      opus_decoder_reset(opus_dec *st);

/* Decodes one packet into interleaved int16, clipped. `pcm` must hold at
 * least OPUS_MAX_SAMPLES*channels samples. Returns samples per channel, or a
 * negative OPUS_E_*. A packet whose frames are not all CELT is refused with
 * OPUS_E_UNSUP_SILK / OPUS_E_UNSUP_HYBRID and NOTHING is written.
 *
 * data == NULL (or len <= 0) is a lost packet: it emits silence of
 * `frame_samples` duration and returns that, which keeps a player's timeline
 * right without pretending to have concealed anything. */
int opus_decode(opus_dec *st, const uint8_t *data, int len,
                int16_t *pcm, int max_samples);

/* Same, in doubles, +-32768 full scale, not clipped. This is the form the
 * conformance harness compares, because clipping is a lossy step that would
 * hide a decoder that overshoots. */
int opus_decode_double(opus_dec *st, const uint8_t *data, int len,
                       double *pcm, int max_samples);

/* The range coder's state after the last decoded packet. The test vectors
 * carry the encoder's copy of this per packet, so it is an exact 32-bit
 * checksum of every entropy decision made -- see opus_range.h. */
uint32_t opus_decoder_final_range(const opus_dec *st);

/* A human-readable name for an OPUS_E_* value. */
const char *opus_strerr(int err);

#endif /* LOGIT_OPUS_H */
