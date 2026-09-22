/* /bin/audiocheck -- decode an audio file on LogitOS and print a CRC32 of the
 * PCM, in exactly the format the host driver prints:
 *
 *     AUDIO-CRC 3f2a91c7 22050 2 44100 mp3
 *
 * (crc, frames, channels, rate, format.)
 *
 * That is the whole point of this program, and it is the same argument
 * /bin/vidcheck makes for the video decoder. The host gate proves the decoders
 * correct against ffmpeg -- but the host build is glibc on x86-64 Linux, and
 * the target build is clang -ffreestanding against mini-libc, with a different
 * malloc, a 24 MiB arena instead of an OS heap, SSE enabled by boot code
 * rather than by the ABI, doubles that survive preemption only because
 * isr_common does FXSAVE, and file I/O through virtio-blk and LogitFS. Any of
 * those can break a decoder that is exact on the host. Printing the same
 * number from inside the OS is what makes "the audio decoders work on LogitOS"
 * a comparison instead of a claim.
 *
 * FLAC gets a second, stronger line. Its STREAMINFO carries the MD5 of the
 * original PCM, so the guest can prove bit-exact lossless reconstruction with
 * no reference decoder present at all:
 *
 *     FLAC-MD5 ok
 *
 * The CRC is taken over the int16 output rather than the decoders' internal
 * floats on purpose. MP3 is a floating-point format and the standard defines
 * conformance as a tolerance, not a bit pattern, so a CRC over float output
 * would be asserting something the format does not have. The int16 output is
 * what a sound card is handed, and if the host and the guest agree on every
 * one of those samples then the arithmetic agreed all the way down.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "flac.h"

static unsigned crc_table[256];

static void crc_init(void)
{
    for (unsigned i = 0; i < 256; i++) {
        unsigned c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
        crc_table[i] = c;
    }
}

static unsigned crc_feed(unsigned crc, const unsigned char *p, unsigned long n)
{
    while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: audiocheck <file.wav|file.mp3|file.flac>\n");
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("AUDIO-ERR cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { printf("AUDIO-ERR empty %s\n", argv[1]); fclose(f); return 1; }
    unsigned char *buf = malloc((unsigned long)len);
    if (!buf) { printf("AUDIO-ERR out of memory (%ld bytes)\n", len); fclose(f); return 1; }
    if ((long)fread(buf, 1, (unsigned long)len, f) != len) {
        printf("AUDIO-ERR short read\n");
        free(buf); fclose(f); return 1;
    }
    fclose(f);

    audio_format fmt = audio_sniff(buf, len);

    /* Decode through the pull interface in modest chunks -- the same path a
     * player takes, and the one that has to work with a real allocator rather
     * than one big convenient buffer. */
    int err = 0;
    adec *d = adec_open(buf, len, &err);
    if (!d) { printf("AUDIO-ERR open %d\n", err); free(buf); return 1; }

    int rate = 0, ch = 0;
    adec_info(d, &rate, &ch);
    if (ch < 1 || ch > AUDIO_MAX_CHANNELS) {
        printf("AUDIO-ERR channels %d\n", ch);
        adec_close(d); free(buf); return 1;
    }

    enum { CHUNK = 1024 };
    short *pcm = malloc((unsigned long)CHUNK * (unsigned long)ch * sizeof(short));
    if (!pcm) { printf("AUDIO-ERR out of memory\n"); adec_close(d); free(buf); return 1; }

    crc_init();
    unsigned crc = 0xFFFFFFFFu;
    long frames = 0;
    for (;;) {
        long n = adec_read(d, pcm, CHUNK);
        if (n < 0) { printf("AUDIO-ERR decode %ld\n", n); adec_close(d); free(buf); return 1; }
        if (n == 0) break;
        /* Hash the samples as little-endian bytes explicitly rather than
         * memcpy-ing the buffer: the CRC must mean the same thing whatever the
         * compiler decided about padding or endianness. */
        for (long i = 0; i < n * ch; i++) {
            unsigned char b[2];
            unsigned short v = (unsigned short)pcm[i];
            b[0] = (unsigned char)(v & 0xFF);
            b[1] = (unsigned char)(v >> 8);
            crc = crc_feed(crc, b, 2);
        }
        frames += n;
    }
    crc ^= 0xFFFFFFFFu;

    printf("AUDIO-CRC %08x %ld %d %d %s\n", crc, frames, ch, rate,
           audio_format_name(fmt));

    adec_close(d);
    free(pcm);

    /* The format's own conformance check, run on the guest. */
    if (fmt == AUDIO_FMT_FLAC) {
        int e2 = 0;
        flacdec *fl = flac_open(buf, len, &e2);
        if (!fl) {
            printf("FLAC-MD5 open-failed %d\n", e2);
        } else {
            int ok = flac_md5_ok(fl);
            printf("FLAC-MD5 %s\n", ok == 1 ? "ok" :
                   ok == 0 ? "MISMATCH" : "unavailable");
            flac_close(fl);
        }
    }

    free(buf);
    return 0;
}
