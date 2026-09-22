/* /bin/vidcheck -- decode an H.264 elementary stream on LogitOS and print the
 * CRC32 of the visible YUV, in exactly the format the host test prints:
 *
 *     H264-CRC b02a15b9 60 frames
 *
 * That is the whole point of this program. The decoder is proved bit-exact
 * against ffmpeg on the host (make test-h264), but the host build is glibc on
 * x86-64 Linux; the target build is clang -ffreestanding against mini-libc,
 * with a different malloc, no OS memory to fall back on, and SSE enabled at
 * boot rather than by the ABI. Printing the same 32-bit number from inside the
 * OS is what makes "the decoder works on LogitOS" a checkable claim instead of
 * an assertion -- tests/boot/run-video-test.sh boots this and compares the
 * number with tests/fixtures/video/sample.crc32.
 *
 * Reads the whole stream into memory: these are test fixtures, not films, and
 * a streaming reader would only add a code path the host build never takes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "h264.h"
/* c/lib/image allocates through the kernel heap's names, and c/lib/video's
 * mjpeg.c reaches for it too (it decodes each frame through img_decode). In
 * ring 3 those names are mini-libc's -- the same two-line shim preview.c:64
 * and browser_rt.c:44 carry. See the Makefile note on this file's link line:
 * the dependency arrived with MJPEG and the link line did not follow it. */
void *kmalloc(unsigned long n) { return malloc((size_t)n); }
void  kfree(void *p) { free(p); }


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
        printf("usage: vidcheck <stream.h264>\n");
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("vidcheck: cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { printf("vidcheck: empty %s\n", argv[1]); fclose(f); return 1; }
    unsigned char *buf = malloc((unsigned long)len);
    if (!buf) { printf("vidcheck: out of memory (%ld bytes)\n", len); fclose(f); return 1; }
    if ((long)fread(buf, 1, (unsigned long)len, f) != len) {
        printf("vidcheck: short read\n"); free(buf); fclose(f); return 1;
    }
    fclose(f);

    crc_init();
    h264dec *d = h264_open();
    if (!d) { printf("vidcheck: decoder init failed\n"); free(buf); return 1; }

    unsigned int crc = 0xFFFFFFFFu;
    long off = 0;
    int frames = 0;
    for (;;) {
        h264frame fr;
        int got = 0, used;
        if (off < len) {
            used = h264_decode(d, buf + off, (int)(len - off), &fr, &got);
            if (used < 0) {
                printf("H264-ERR %d after %d frames\n", used, frames);
                h264_close(d); free(buf);
                return 1;
            }
            off += used;
            if (!got) { if (off < len) continue; }
        }
        if (!got) { if (!h264_flush(d, &fr)) break; }
        crc = crc_plane(crc, fr.y, fr.stride_y, fr.width, fr.height);
        crc = crc_plane(crc, fr.u, fr.stride_c, fr.width / 2, fr.height / 2);
        crc = crc_plane(crc, fr.v, fr.stride_c, fr.width / 2, fr.height / 2);
        frames++;
    }
    crc ^= 0xFFFFFFFFu;

    printf("H264-CRC %08x %d frames\n", crc, frames);
    h264_close(d);
    free(buf);
    return 0;
}
