/* tests/unit/opus_vec.c -- run one official Opus test vector through our
 * decoder and report, per vector, the two numbers that matter.
 *
 * IT PRODUCES TWO INDEPENDENT VERDICTS AND THEY MEASURE DIFFERENT HALVES.
 *
 *   rng mismatches   an EXACT count. A `.bit` file stores, per packet, the
 *                    ENCODER's range-coder state at the end of that packet
 *                    (opus_demo writes it: 4 bytes length, 4 bytes range,
 *                    then payload). Our decoder computes the same 32-bit
 *                    number from its own decisions, so a disagreement is a
 *                    bitstream parse that diverged, localised to a packet
 *                    index. This is the H.264 "per-case wrong-byte total"
 *                    of this codec -- something to BISECT with. "The audio
 *                    sounds wrong" is not.
 *
 *   the .sw output   scored afterwards by the reference's own opus_compare
 *                    against the vector's `.dec`. That is the conformance
 *                    bar RFC 6716 section 6 actually sets, and it is the one
 *                    that can pass while the rng count is nonzero (a broken
 *                    reconstruction on a correct parse) or fail while the
 *                    rng count is zero (the reverse).
 *
 * Frames this build refuses (SILK, hybrid) are COUNTED, and silence of the
 * correct duration is written in their place so the output stays aligned
 * with the reference -- an unaligned .sw scores near zero and would read as
 * a catastrophic decoder rather than as a missing feature. A vector with any
 * refusal is therefore not scoreable and is excluded BY NAME in tests/opus.mk
 * rather than silently allowed to report a bad number.
 *
 * usage: opus_vec <in.bit> <out.sw> <channels>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opus.h"

static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8  | (uint32_t)p[3];
}

int main(int argc, char **argv)
{
    FILE *fin, *fout;
    opus_dec *dec;
    int channels, err = 0;
    unsigned char hdr[8];
    unsigned char payload[OPUS_MAX_PACKET + 8];
    int16_t pcm[OPUS_MAX_SAMPLES * 2];
    long packets = 0, frames = 0, samples = 0;
    long rng_bad = 0, first_bad = -1;
    long refused_silk = 0, refused_hybrid = 0, hard_err = 0;
    long first_hard_err_pkt = -1;
    int first_hard_err = 0;

    if (argc != 4) {
        fprintf(stderr, "usage: %s <in.bit> <out.sw> <channels>\n", argv[0]);
        return 2;
    }
    channels = atoi(argv[3]);
    fin = fopen(argv[1], "rb");
    if (!fin) { perror(argv[1]); return 2; }
    fout = fopen(argv[2], "wb");
    if (!fout) { perror(argv[2]); fclose(fin); return 2; }

    dec = opus_decoder_create(channels, &err);
    if (!dec) {
        fprintf(stderr, "decoder create failed: %s\n", opus_strerr(err));
        fclose(fin); fclose(fout);
        return 2;
    }

    while (fread(hdr, 1, 8, fin) == 8) {
        uint32_t len = be32(hdr);
        uint32_t want_rng = be32(hdr + 4);
        int n, i, nsamp;
        opus_packet p;

        if (len == 0 || len > OPUS_MAX_PACKET) {
            fprintf(stderr, "packet %ld: implausible length %u\n", packets, len);
            break;
        }
        if (fread(payload, 1, len, fin) != len) {
            fprintf(stderr, "packet %ld: short read\n", packets);
            break;
        }
        packets++;

        /* Parsed separately from the decode so the frame count and the
         * silence length are known even when the decode is refused. */
        if (opus_packet_parse(payload, (int)len, &p) == OPUS_OK) {
            frames += p.nframes;
            nsamp = p.nframes * p.frame_samples;
        } else {
            nsamp = 0;
        }

        n = opus_decode(dec, payload, (int)len, pcm, OPUS_MAX_SAMPLES);
        if (n < 0) {
            if (n == OPUS_E_UNSUP_SILK)        refused_silk++;
            else if (n == OPUS_E_UNSUP_HYBRID) refused_hybrid++;
            else {
                hard_err++;
                if (first_hard_err_pkt < 0) {
                    first_hard_err_pkt = packets - 1;
                    first_hard_err = n;
                }
            }
            /* Keep the timeline: write silence of the packet's own length. */
            if (nsamp > 0) {
                memset(pcm, 0, (size_t)nsamp * channels * sizeof pcm[0]);
                n = nsamp;
            } else {
                continue;
            }
        } else {
            uint32_t got_rng = opus_decoder_final_range(dec);
            if (got_rng != want_rng) {
                rng_bad++;
                if (first_bad < 0) first_bad = packets - 1;
            }
        }

        for (i = 0; i < n * channels; i++) {
            unsigned char b[2];
            b[0] = (unsigned char)(pcm[i] & 0xFF);
            b[1] = (unsigned char)((pcm[i] >> 8) & 0xFF);
            if (fwrite(b, 1, 2, fout) != 2) { perror("write"); goto done; }
        }
        samples += n;
    }

done:
    opus_decoder_destroy(dec);
    fclose(fin);
    fclose(fout);

    printf("packets=%ld frames=%ld samples=%ld rng_mismatch=%ld",
           packets, frames, samples, rng_bad);
    if (first_bad >= 0) printf(" first_bad_packet=%ld", first_bad);
    printf(" refused_silk=%ld refused_hybrid=%ld hard_err=%ld",
           refused_silk, refused_hybrid, hard_err);
    if (hard_err) printf(" first_hard_err=%d(%s)@%ld",
                         first_hard_err, opus_strerr(first_hard_err),
                         first_hard_err_pkt);
    printf("\n");

    if (hard_err) return 3;
    if (refused_silk || refused_hybrid) return 4;   /* not scoreable */
    if (rng_bad) return 1;
    return 0;
}
