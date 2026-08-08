/* imgcheck -- decode image files and print a one-line digest per file.
 *
 * ONE source, TWO builds. The host build runs against glibc; the target build
 * is /bin/imgcheck, a ring-3 LogitOS process linked against mini-libc and the
 * same decoders. `make test-imgcheck` runs both over the identical fixture
 * bytes and requires the output to be identical line for line.
 *
 * That comparison is the point. The host tests prove the decoders are
 * byte-exact against PIL/libwebp/ffmpeg, but they prove it about a glibc build
 * on Linux. On the guest the allocator is mini-libc's 24 MiB arena rather than
 * glibc's malloc, the compiler flags are -ffreestanding -mno-red-zone -msse2,
 * the files come off LogitFS instead of a filesystem the host trusts, and the
 * Rust staticlib is the x86_64-unknown-none build rather than the host one.
 * A decoder can be right in the first environment and wrong in the second --
 * an alignment assumption, a size_t width, a stack that is 32 KiB rather than
 * 8 MiB -- and the digest below is what would say so.
 *
 * The digest covers every frame's pixels plus the geometry and the per-frame
 * delay, so a difference in disposal, timing or orientation moves it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "img.h"

void *kmalloc(unsigned long n) { return malloc((size_t)n); }
void  kfree(void *p) { free(p); }

static unsigned long crc_table[256];
static int crc_ready;

static void crc_init(void)
{
    for (unsigned long i = 0; i < 256; i++) {
        unsigned long c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_ready = 1;
}

static unsigned long crc32_up(unsigned long crc, const unsigned char *b, long n)
{
    if (!crc_ready) crc_init();
    for (long i = 0; i < n; i++) crc = crc_table[(crc ^ b[i]) & 0xff] ^ (crc >> 8);
    return crc;
}

static const char *base(const char *p)
{
    const char *s = p;
    for (const char *q = p; *q; q++) if (*q == '/' || *q == '\\') s = q + 1;
    return s;
}

int main(int argc, char **argv)
{
    int bad = 0;
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) { printf("IMG %s OPENFAIL\n", base(argv[a])); bad++; continue; }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *buf = n > 0 ? malloc(n) : 0;
        if (!buf || fread(buf, 1, n, f) != (size_t)n) {
            printf("IMG %s READFAIL\n", base(argv[a])); bad++;
            fclose(f); free(buf); continue;
        }
        fclose(f);

        struct img_anim an;
        if (img_decode_anim(buf, (int)n, &an) != 0) {
            printf("IMG %s DECODEFAIL\n", base(argv[a])); bad++; free(buf); continue;
        }
        unsigned long crc = 0xFFFFFFFFUL;
        long delaysum = 0;
        for (int k = 0; k < an.nframes; k++) {
            crc = crc32_up(crc, an.frames[k].rgba, (long)an.w * an.h * 4);
            delaysum += an.frames[k].delay_ms;
        }
        crc ^= 0xFFFFFFFFUL;
        printf("IMG %s %dx%d frames=%d loops=%d delaysum=%ld crc=%08lx\n",
               base(argv[a]), an.w, an.h, an.nframes, an.loops, delaysum,
               crc & 0xFFFFFFFFUL);
        img_anim_free(&an);
        free(buf);
    }
    printf("IMGCHECK DONE %d\n", bad);
    return bad ? 1 : 0;
}
