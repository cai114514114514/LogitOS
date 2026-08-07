/* Total mismatching bytes over a whole stream, per plane. A single number to
 * compare variants against -- "first mismatch moved" says nothing about whether
 * a change helped. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "h264.h"

static uint8_t *slurp(const char *p, long *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)*n);
    if (fread(b, 1, (size_t)*n, f) != (size_t)*n) { fprintf(stderr, "short\n"); exit(1); }
    fclose(f); return b;
}

static long cmp_plane(const uint8_t *got, int stride, const uint8_t *want,
                      int w, int h, long *first)
{
    long bad = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (got[(long)y * stride + x] != want[(long)y * w + x]) {
                if (!bad && first) *first = (long)y * w + x;
                bad++;
            }
    return bad;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <h264> <ref.yuv>\n", argv[0]); return 1; }
    long slen, rlen;
    uint8_t *s = slurp(argv[1], &slen), *ref = slurp(argv[2], &rlen);

    h264dec *d = h264_open();
    long off = 0, roff = 0;
    long bad_y = 0, bad_u = 0, bad_v = 0;
    int frames = 0, badframes = 0;

    for (;;) {
        h264frame f; int got = 0, n;
        if (off < slen) {
            n = h264_decode(d, s + off, (int)(slen - off), &f, &got);
            if (n < 0) { printf("decode error %d after %d frames\n", n, frames); break; }
            off += n;
            if (!got) { if (off < slen) continue; }
        }
        if (!got) { if (!h264_flush(d, &f)) break; }

        int W = f.width, H = f.height, CW = W / 2, CH = H / 2;
        long need = (long)W * H + 2L * CW * CH;
        if (roff + need > rlen) { printf("ref exhausted at frame %d\n", frames); break; }
        long by = cmp_plane(f.y, f.stride_y, ref + roff, W, H, 0);
        long bu = cmp_plane(f.u, f.stride_c, ref + roff + (long)W * H, CW, CH, 0);
        long bv = cmp_plane(f.v, f.stride_c, ref + roff + (long)W * H + (long)CW * CH, CW, CH, 0);
        if (by | bu | bv) {
            badframes++;
            if (badframes == 1)
                printf("  first bad frame %d: Y %ld  U %ld  V %ld\n", frames, by, bu, bv);
        }
        bad_y += by; bad_u += bu; bad_v += bv;
        roff += need;
        frames++;
    }
    printf("  frames %d, bad frames %d, bytes wrong: Y %ld  U %ld  V %ld  total %ld\n",
           frames, badframes, bad_y, bad_u, bad_v, bad_y + bad_u + bad_v);
    /* Release everything so this harness can be run under ASan and any leak it
     * reports belongs to the decoder rather than to the test. */
    h264_close(d);
    free(s);
    free(ref);
    return (bad_y + bad_u + bad_v) ? 1 : 0;
}
