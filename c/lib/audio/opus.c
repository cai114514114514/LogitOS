/* c/lib/audio/opus.c -- Opus packet layer and top-level decoder, RFC 6716.
 *
 * ============================================================================
 * THE BAR FOR THIS CODEC IS THE SPEC'S OWN CONFORMANCE TEST, NOT BYTE EQUALITY
 * ============================================================================
 *
 * Every other codec in this tree is held to bit-exactness, and that is right
 * for every other codec in this tree: H.264, H.265, VP8 and FLAC are defined
 * as exactly specified integer arithmetic, so a single differing byte against
 * ffmpeg is OUR bug and the tests say so. Opus is not like them, and the
 * difference is in the standard rather than in anybody's convenience.
 *
 * RFC 6716 does not define the decoder's output as a function. It publishes a
 * REFERENCE DECODER (Appendix A) and defines conformance, in section 6, as
 * agreement with that decoder's output under `opus_compare`'s quality metric.
 * That indirection is not laziness. The reference has a float build and a
 * fixed-point build, they produce DIFFERENT samples for the same packet, and
 * the RFC ships both and calls both conformant. So "the correct output" for
 * an Opus packet is not a bit pattern; it is a set of bit patterns, and the
 * membership test is opus_compare. There is no byte to be exact to.
 *
 * The practical consequence, and the reason this paragraph is at the top of
 * the file rather than in a test: A DIFF AGAINST ffmpeg's DECODE PROVES
 * NOTHING HERE, in either direction. It will not be zero for a correct
 * decoder, and a small non-zero difference does not distinguish a correct
 * decoder from a subtly wrong one. `make test-opus-ffmpeg` therefore scores
 * ffmpeg's output with the SAME metric rather than subtracting it, and
 * `make test-opus` is the real gate: the official vectors, through
 * opus_compare, at the threshold the RFC names.
 *
 * WHAT IS STILL EXACT, AND IS TESTED AS EXACT.  The entropy layer.  The range
 * decoder and everything that decides a symbol is integer arithmetic here and
 * agrees with the reference bit for bit -- see the arithmetic note in
 * opus_celt.c for the line between the two halves. That is not a consolation
 * prize; it is the sharper instrument of the two. Each test vector carries
 * the ENCODER's range-coder state at the end of every packet, so the gate can
 * compare a 32-bit checksum per packet and report a COUNT of packets whose
 * entropy decode diverged. "4 packets of 2147 have the wrong rng" is a
 * bisectable fact. A quality score of 0.9 is not.
 *
 * ============================================================================
 *
 * SCOPE: CELT ONLY, AND SILK IS REFUSED BY NAME.  An Opus packet's TOC byte
 * says which of three modes each frame uses. This decoder implements CELT
 * (RFC 6716 4.3) and refuses SILK-only and hybrid frames with a distinct
 * error each. It does not decode them as silence, and the distinction is the
 * point: a player handed silence cannot tell "this file is quiet" from "this
 * build cannot do speech", and neither can a bug report. See the header of
 * opus_celt.h for why CELT was worth landing first -- in one line, because
 * WebM/Opus music tracks are CELT and a SILK-first decoder would have played
 * telephone speech and left every music track silent.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "opus.h"
#include "opus_range.h"
#include "opus_celt.h"

struct opus_dec {
    int channels;
    opus_celt_dec *celt;
    uint32_t final_range;
    int prev_mode;            /* 0 = nothing decoded yet */
    int prev_frame_samples;   /* what a lost packet should conceal for */
    /* Per instance, not static: opus_decode() converts through this and two
     * decoders running in one process must not share it. */
    double pcmbuf[OPUS_MAX_SAMPLES * 2];
};

/* ------------------------------------------------------------ TOC parsing */

/* RFC 6716 3.1: config = toc>>3 indexes a 32-entry table of
 * (mode, bandwidth, frame size). It is written as arithmetic on the config
 * number rather than as a 32-row table because the RFC defines it that way --
 * three contiguous ranges with a regular stride -- and a table would be 32
 * lines a reader has to check against the same three rules. */
static void toc_config(uint8_t toc, int *mode, int *bw, int *fs_num, int *fs_den)
{
    static const int silk_bw[3]  = { OPUS_BW_NARROW, OPUS_BW_MEDIUM, OPUS_BW_WIDE };
    static const int silk_ms[4]  = { 10, 20, 40, 60 };
    static const int celt_bw[4]  = { OPUS_BW_NARROW, OPUS_BW_WIDE,
                                     OPUS_BW_SUPERWIDE, OPUS_BW_FULL };
    static const int celt_hms[4] = { 5, 10, 20, 40 };   /* halves of a ms */
    int cfg = toc >> 3;
    if (cfg < 12) {
        /* SILK: NB/MB/WB x 10/20/40/60 ms */
        *mode = OPUS_MODE_SILK;
        *bw = silk_bw[cfg / 4];
        *fs_num = silk_ms[cfg % 4];
        *fs_den = 1;
    } else if (cfg < 16) {
        /* Hybrid: SWB/FB x 10/20 ms */
        *mode = OPUS_MODE_HYBRID;
        *bw = (cfg - 12) / 2 ? OPUS_BW_FULL : OPUS_BW_SUPERWIDE;
        *fs_num = (cfg - 12) % 2 ? 20 : 10;
        *fs_den = 1;
    } else {
        /* CELT: NB/WB/SWB/FB x 2.5/5/10/20 ms. The 2.5 ms case is why this
         * is a fraction and not an integer count of milliseconds. */
        *mode = OPUS_MODE_CELT;
        *bw = celt_bw[(cfg - 16) / 4];
        *fs_num = celt_hms[(cfg - 16) % 4];
        *fs_den = 2;
    }
}

int opus_packet_frame_samples(uint8_t toc)
{
    int mode, bw, num, den;
    toc_config(toc, &mode, &bw, &num, &den);
    /* 48 samples per ms at 48 kHz. Every (num, den) pair here divides
     * exactly: 5/2 * 48 = 120. */
    return 48 * num / den;
}

/* RFC 6716 3.2.1's one- or two-byte frame length. Returns bytes consumed, and
 * writes -1 into *size if the encoding runs off the end. */
static int parse_size(const uint8_t *data, int len, int *size)
{
    if (len < 1) { *size = -1; return -1; }
    if (data[0] < 252) { *size = data[0]; return 1; }
    if (len < 2) { *size = -1; return -1; }
    *size = 4 * data[1] + data[0];
    return 2;
}

int opus_packet_parse(const uint8_t *data, int len, opus_packet *p)
{
    int i, bytes, count, cbr, last_size, framesize;
    uint8_t ch, toc;
    int mode, bw, num, den;

    if (!data || !p || len < 1) return OPUS_E_BADARG;

    memset(p, 0, sizeof *p);
    toc = data[0];
    toc_config(toc, &mode, &bw, &num, &den);
    framesize = 48 * num / den;

    p->toc = toc;
    p->mode = mode;
    p->bandwidth = bw;
    p->stereo = (toc >> 2) & 1;
    p->frame_samples = framesize;
    p->code = toc & 3;

    data++;
    len--;
    last_size = len;
    cbr = 0;

    switch (toc & 3) {
    case 0:
        count = 1;
        break;
    case 1:
        count = 2;
        cbr = 1;
        if (len & 1) return OPUS_E_PACKET;
        p->size[0] = last_size = len / 2;
        break;
    case 2:
        count = 2;
        bytes = parse_size(data, len, &p->size[0]);
        if (bytes < 0) return OPUS_E_PACKET;
        len -= bytes;
        if (p->size[0] < 0 || p->size[0] > len) return OPUS_E_PACKET;
        data += bytes;
        last_size = len - p->size[0];
        break;
    default: /* case 3 */
        if (len < 1) return OPUS_E_PACKET;
        ch = *data++;
        count = ch & 0x3F;
        /* Two bounds, and both are checked. The duration one is the RFC's
         * (5760 = 120 ms at 48 kHz); the count one is this struct's array
         * size. The field is six bits, so the wire can say 63 -- the
         * duration check happens to exclude that because the shortest frame
         * is 120 samples, but frame[] is sized 48 and must not depend on
         * that arithmetic holding somewhere else in the file. */
        if (count <= 0 || count > OPUS_MAX_FRAMES) return OPUS_E_PACKET;
        if (framesize * count > OPUS_MAX_SAMPLES) return OPUS_E_PACKET;
        len--;
        if (ch & 0x40) {
            /* Padding, counted in 254-byte units with 255 as a continuation.
             * The 255 case adds 254 and not 255 because the 255 byte itself
             * is one of the padding bytes -- an off-by-one here shifts every
             * subsequent frame and is invisible until a frame fails to
             * decode somewhere later. */
            int padding = 0, pv;
            do {
                if (len <= 0) return OPUS_E_PACKET;
                pv = *data++;
                len--;
                padding += pv == 255 ? 254 : pv;
            } while (pv == 255);
            len -= padding;
            p->padding = padding;
        }
        if (len < 0) return OPUS_E_PACKET;
        cbr = !(ch & 0x80);
        if (!cbr) {
            last_size = len;
            for (i = 0; i < count - 1; i++) {
                bytes = parse_size(data, len, &p->size[i]);
                if (bytes < 0) return OPUS_E_PACKET;
                len -= bytes;
                if (p->size[i] < 0 || p->size[i] > len) return OPUS_E_PACKET;
                data += bytes;
                last_size -= bytes + p->size[i];
            }
            if (last_size < 0) return OPUS_E_PACKET;
        } else {
            last_size = len / count;
            if (last_size * count != len) return OPUS_E_PACKET;
            for (i = 0; i < count - 1; i++) p->size[i] = last_size;
        }
        break;
    }

    /* The last frame's length is implied rather than coded, so nothing has
     * bounded it yet -- and it is a FRAME, so 1275 is the right number. */
    if (last_size > OPUS_MAX_FRAME_BYTES) return OPUS_E_PACKET;
    p->size[count - 1] = last_size;

    for (i = 0; i < count; i++) {
        p->frame[i] = data;
        data += p->size[i];
    }
    p->nframes = count;
    return OPUS_OK;
}

int opus_packet_nb_samples(const uint8_t *data, int len)
{
    opus_packet p;
    int r = opus_packet_parse(data, len, &p);
    if (r < 0) return r;
    return p.nframes * p.frame_samples;
}

/* -------------------------------------------------------------- decoder -- */

opus_dec *opus_decoder_create(int channels, int *error)
{
    opus_dec *st;
    if (channels < 1 || channels > 2) {
        if (error) *error = OPUS_E_BADARG;
        return NULL;
    }
    st = (opus_dec *)calloc(1, sizeof *st);
    if (!st) { if (error) *error = OPUS_E_NOMEM; return NULL; }
    st->channels = channels;
    st->celt = opus_celt_create(channels);
    if (!st->celt) {
        free(st);
        if (error) *error = OPUS_E_NOMEM;
        return NULL;
    }
    if (error) *error = OPUS_OK;
    return st;
}

void opus_decoder_destroy(opus_dec *st)
{
    if (!st) return;
    opus_celt_destroy(st->celt);
    free(st);
}

void opus_decoder_reset(opus_dec *st)
{
    if (!st) return;
    opus_celt_reset(st->celt);
    st->final_range = 0;
    st->prev_mode = 0;
}

uint32_t opus_decoder_final_range(const opus_dec *st)
{
    return st ? st->final_range : 0;
}

const char *opus_strerr(int err)
{
    switch (err) {
    case OPUS_OK:              return "ok";
    case OPUS_E_BADARG:        return "bad argument";
    case OPUS_E_PACKET:        return "malformed packet";
    case OPUS_E_UNSUP_SILK:    return "SILK-only frame (not implemented)";
    case OPUS_E_UNSUP_HYBRID:  return "hybrid frame (SILK layer not implemented)";
    case OPUS_E_NOMEM:         return "out of memory";
    default:                   return "internal error";
    }
}

/* The end band each bandwidth occupies, RFC 6716 4.3. NB stops at band 13
 * and not at 21 with the rest zeroed: the bands above are not CODED, so the
 * allocation never sees them and the bitstream would differ. */
static int endband_for(int bw)
{
    switch (bw) {
    case OPUS_BW_NARROW:    return 13;
    case OPUS_BW_MEDIUM:
    case OPUS_BW_WIDE:      return 17;
    case OPUS_BW_SUPERWIDE: return 19;
    default:                return 21;
    }
}

int opus_decode_double(opus_dec *st, const uint8_t *data, int len,
                       double *pcm, int max_samples)
{
    opus_packet p;
    int r, f, produced = 0;

    if (!st || !pcm) return OPUS_E_BADARG;

    if (!data || len <= 0) {
        /* A lost packet. It conceals for as long as the LAST packet ran,
         * which is the only duration anything here knows; with no previous
         * packet nothing is emitted, rather than a guess at a frame size. */
        int n = st->prev_frame_samples;
        if (!st->prev_mode || n <= 0) return 0;
        if (n > max_samples) return OPUS_E_BADARG;
        return opus_celt_conceal(st->celt, pcm, n);
    }

    r = opus_packet_parse(data, len, &p);
    if (r < 0) return r;

    /* REFUSE FIRST, DECODE SECOND.  The check runs over every frame before
     * any of them is decoded, so a packet that mixes CELT and SILK frames
     * (vectors 08 and 09 contain exactly that, 5 packets each) produces one
     * clean refusal instead of half a packet of audio followed by an error
     * the caller has to unpick. */
    if (p.mode == OPUS_MODE_SILK) {
        st->prev_mode = p.mode + 1;
        st->prev_frame_samples = p.frame_samples;
        return OPUS_E_UNSUP_SILK;
    }
    if (p.mode == OPUS_MODE_HYBRID) {
        st->prev_mode = p.mode + 1;
        st->prev_frame_samples = p.frame_samples;
        return OPUS_E_UNSUP_HYBRID;
    }

    if (p.nframes * p.frame_samples > max_samples) return OPUS_E_BADARG;

    /* A MODE CHANGE RESETS THE CELT STATE, and it has to be done here even
     * though the other mode is refused. The reference does exactly this
     * (opus_decode_frame: "Make sure to discard any previous CELT state"),
     * and the reason it matters to a CELT-only build is not obvious: the
     * frames this build refuses still ADVANCE a conformant decoder's CELT
     * state, so carrying our stale overlap and energy history across the gap
     * would make the first CELT frame after a refusal differ from a
     * conformant decoder's for a reason unrelated to the missing mode.
     *
     * MEASURED, on the three vectors that mix modes, and recorded because
     * the result is not the one that was expected:
     *
     *     vector   mode changes   weighted error  without -> with
     *       08           1           0.456754  ->  0.456754   (unchanged)
     *       09           1           0.433883  ->  0.433883   (unchanged)
     *       10          16           4.901231  ->  3.544598
     *
     * 08 and 09 do not move at all, and the reason is NOT that the reset is
     * ineffective. Those two vectors put their five SILK packets at indices
     * 0..4 -- FIRST. So each file contains exactly one mode change, it
     * happens at packet 5, and what it resets is a CELT state that is still
     * the freshly created one. Nothing to discard, nothing to gain. Vector
     * 10 interleaves its hybrid packets throughout, changes mode 16 times,
     * and is where the rule earns its place: 28% off the error.
     *
     * "The reset must be broken" was the first theory, and it was wrong; the
     * question that settled it was "where ARE the SILK packets", not another
     * look at this code. None of the three passes and none was going to: a
     * refused frame is silence, and silence where speech belongs is what
     * those numbers measure. The reset is here because it is what a decoder
     * does at a mode boundary, not as a repair. */
    if (st->prev_mode && st->prev_mode != p.mode + 1)
        opus_celt_reset(st->celt);

    opus_celt_set_bands(st->celt, 0, endband_for(p.bandwidth));
    opus_celt_set_stream_channels(st->celt, p.stereo ? 2 : 1);

    for (f = 0; f < p.nframes; f++) {
        orange dec;
        int got;
        if (p.size[f] <= 1) {
            /* A frame of one byte or less is DTX/lost. No vector in the
             * corpus contains one (measured: 0 of 5524/4186/1501 frames in
             * vectors 01/07/11), so this path has never been exercised by a
             * gate and says so rather than pretending otherwise. */
            got = opus_celt_conceal(st->celt, pcm + produced * st->channels,
                                    p.frame_samples);
        } else {
            orange_init(&dec, p.frame[f], (uint32_t)p.size[f]);
            got = opus_celt_decode(st->celt, &dec, p.frame[f], p.size[f],
                                   pcm + produced * st->channels,
                                   p.frame_samples);
            if (got > 0) st->final_range = opus_celt_final_range(st->celt);
        }
        if (got < 0) return OPUS_E_INTERNAL;
        produced += got;
    }

    st->prev_mode = p.mode + 1;
    st->prev_frame_samples = p.frame_samples;
    return produced;
}

/* Rounds and clips, which is exactly the reference's FLOAT2INT16 once the
 * 1/32768 it applies first is folded away (see the deemphasis note in
 * opus_celt.c: the samples arrive here already in +-32768 units). */
static int16_t to_int16(double x)
{
    long v;
    if (x > 32767.0) return 32767;
    if (x < -32768.0) return -32768;
    v = lrint(x);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

int opus_decode(opus_dec *st, const uint8_t *data, int len,
                int16_t *pcm, int max_samples)
{
    int n, i, total;
    if (!st || !pcm) return OPUS_E_BADARG;
    if (max_samples > OPUS_MAX_SAMPLES) max_samples = OPUS_MAX_SAMPLES;
    n = opus_decode_double(st, data, len, st->pcmbuf, max_samples);
    if (n < 0) return n;
    total = n * st->channels;
    for (i = 0; i < total; i++) pcm[i] = to_int16(st->pcmbuf[i]);
    return n;
}
