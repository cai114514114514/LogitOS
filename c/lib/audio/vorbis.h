/* c/lib/audio/vorbis.h -- from-scratch Ogg Vorbis I decoder.
 *
 * WHAT "CORRECT" MEANS HERE, AND WHY IT IS WEAKER THAN FOR THE OTHER FORMATS.
 * Read this before quoting any number this decoder's tests print.
 *
 * WAV and FLAC are exactly specified, so bit-exactness is the honest bar and
 * the tests assert it. MP3 and AAC are not, but their standards DEFINE a
 * conformance tolerance -- ISO/IEC 11172-4 and 14496-4 each give a numeric
 * bound on the difference from a reference decoder -- so those tests measure
 * against a published number.
 *
 * Vorbis has NEITHER. The Vorbis I specification defines the format by its
 * decoding procedure, in floating point, and states no numeric bound on how
 * far two conformant decoders may be apart. Xiph publishes no conformance
 * bitstream suite. There is no official criterion to pass and no official
 * reference output to pass it against.
 *
 * So the only honest thing to report is a DIFFERENTIAL: how far this decoder's
 * output is from another implementation's on the same bytes, stated as a
 * measurement rather than as conformance. tests/unit/vorbis_test.c does
 * exactly that against ffmpeg, prints the whole error distribution, and does
 * NOT quote a tolerance, because inventing one would be inventing a property
 * of the format. What it CAN assert, and does, are the things that are
 * genuinely defined: the packet and page structure, the codebook decode, the
 * floor curve, and that the output is deterministic and identical between the
 * host build and LogitOS.
 *
 * IMPLEMENTED
 *   Vorbis I in Ogg. All three header packets; codebooks with ordered, sparse
 *   and unordered length lists and VQ lookup types 0, 1 and 2; floor type 1
 *   (the one every encoder emits) and floor type 0 (LSP); residue types 0, 1
 *   and 2; channel coupling; mapping type 0 with submaps; both block sizes
 *   with all four lapping cases; up to eight channels.
 *
 * NOT IMPLEMENTED (a clean error, never a crash or silent garbage)
 *   Chained and multiplexed streams beyond following the first logical stream;
 *   Vorbis outside Ogg (Matroska carries it with the headers in
 *   CodecPrivate -- see vorbis_open_headers(), which is the entry point for
 *   that); and the granule-position trimming of the first and last block,
 *   which is deliberate and documented below.
 *
 * ON TRIMMING. A Vorbis stream's final page carries a granule position that
 * says how many samples of the last block are real. This decoder does NOT
 * trim, for the same reason mp3.c does not trim the encoder delay: trimming
 * makes the output no longer comparable sample-for-sample with a reference
 * decode, which is the entire content of the test. vorbis_granule_end()
 * exposes the value so a player can trim.
 */
#ifndef LOGIT_VORBIS_H
#define LOGIT_VORBIS_H

#include <stdint.h>
#include "audio.h"

#define VORBIS_MAX_CHANNELS 8

typedef struct vorbisdec vorbisdec;

typedef struct {
    int rate;
    int channels;
    int nsamples;              /* per channel; varies with the block size */
    const float *pcm;          /* interleaved, valid until the next call */
} vorbisframe;

/* Open an Ogg Vorbis file held in memory. `data` must stay valid. */
vorbisdec *vorbis_open(const uint8_t *data, long len, int *err);

/* Open from three raw header packets, which is how Matroska and WebM carry
 * Vorbis (CodecPrivate holds the identification, comment and setup packets).
 * The caller then feeds audio packets to vorbis_packet(). */
vorbisdec *vorbis_open_headers(const uint8_t *id, long idlen,
                               const uint8_t *comment, long clen,
                               const uint8_t *setup, long slen, int *err);

void vorbis_close(vorbisdec *v);

int  vorbis_info(const vorbisdec *v, int *rate, int *channels);

/* Decode the next audio packet from the Ogg stream. Returns 1 and fills *out,
 * 0 at end of stream, or a negative AUDIO_ERR_*. A packet that produces no
 * samples (the first one after a seek, which only primes the overlap) returns
 * 1 with out->nsamples == 0. */
int  vorbis_decode(vorbisdec *v, vorbisframe *out);

/* Decode one raw audio packet (the vorbis_open_headers path). */
int  vorbis_packet(vorbisdec *v, const uint8_t *pkt, long len, vorbisframe *out);

/* The granule position of the last page seen, i.e. the total sample count the
 * stream claims. -1 if unknown. A player trims the final block to this; the
 * tests deliberately do not. */
int64_t vorbis_granule_end(const vorbisdec *v);

#endif /* LOGIT_VORBIS_H */
