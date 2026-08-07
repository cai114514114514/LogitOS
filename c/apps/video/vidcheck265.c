/* /bin/vidcheck265 -- decode an H.265/HEVC elementary stream on LogitOS and
 * print the CRC32 of the visible YUV, in exactly the format the host test
 * prints:
 *
 *     H265-CRC c6b95aad 21 pictures
 *
 * That is the whole point of this program, and it is the same point
 * /bin/vidcheck makes for H.264. The decoder is proved bit-exact against
 * ffmpeg on the host (make test-h265), but the host build is glibc on x86-64
 * Linux; the target build is clang -ffreestanding against mini-libc, with a
 * different malloc, no OS memory to fall back on, and SSE enabled at boot
 * rather than by the ABI. Printing the same 32-bit number from inside the OS
 * is what makes "the H.265 decoder works on LogitOS" a checkable claim instead
 * of an assertion -- tests/boot/run-video265-test.sh boots this and compares
 * the number with tests/fixtures/video265/sample.crc32.
 *
 * HEVC allocates far more than H.264 does for the same picture size: a real
 * DPB of up to 17 pictures with their motion fields, the per-4x4 decode state,
 * the z-scan and tile tables. That is exactly what makes this worth running on
 * the device rather than only on the host.
 *
 * Reads the whole stream into memory: these are test fixtures, not films, and
 * a streaming reader would only add a code path the host build never takes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "h265.h"

static unsigned int crc_table[256];

static void crc_init(void)
{
    for (unsigned int i = 0; i < 256; i++) {
        unsigned int c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
        crc_table[i] = c;
    }
}

static unsigned int crc_feed(unsigned int crc, const unsigned char *p, unsigned long n)
{
    while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

/* CRC over the VISIBLE samples only, row by row: the decoder's planes carry a
 * padding border and a stride, and the host driver hashes the same thing. */
static unsigned int crc_plane(unsigned int crc, const unsigned char *p,
                              int stride, int w, int h)
{
    for (int y = 0; y < h; y++)
        crc = crc_feed(crc, p + (long)y * stride, (unsigned long)w);
    return crc;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: vidcheck265 <stream.h265>\n");
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("vidcheck265: cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { printf("vidcheck265: empty %s\n", argv[1]); fclose(f); return 1; }
    unsigned char *buf = malloc((unsigned long)len);
    if (!buf) { printf("vidcheck265: out of memory (%ld bytes)\n", len); fclose(f); return 1; }
    if ((long)fread(buf, 1, (unsigned long)len, f) != len) {
        printf("vidcheck265: short read\n"); free(buf); fclose(f); return 1;
    }
    fclose(f);

    crc_init();
    h265dec *d = h265_open();
    if (!d) { printf("vidcheck265: decoder init failed\n"); free(buf); return 1; }

    unsigned int crc = 0xFFFFFFFFu;
    long off = 0;
    int frames = 0;
    for (;;) {
        h265frame fr;
        int got = 0, used;
        if (off < len) {
            used = h265_decode(d, buf + off, (int)(len - off), &fr, &got);
            if (used < 0) {
                printf("H265-ERR %d after %d pictures\n", used, frames);
                h265_close(d); free(buf);
                return 1;
            }
            if (used == 0 && !got) break;
            off += used;
            if (!got) { if (off < len) continue; }
        }
        if (!got) { if (h265_flush(d, &fr) != 1) break; }
        crc = crc_plane(crc, fr.y, fr.stride_y, fr.width, fr.height);
        crc = crc_plane(crc, fr.u, fr.stride_c, fr.width / 2, fr.height / 2);
        crc = crc_plane(crc, fr.v, fr.stride_c, fr.width / 2, fr.height / 2);
        frames++;
    }
    crc ^= 0xFFFFFFFFu;

    printf("H265-CRC %08x %d pictures\n", crc, frames);
    h265_close(d);
    free(buf);
    return 0;
}
