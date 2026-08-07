/* Total mismatching bytes over a whole HEVC stream, per plane. A single number
 * to compare variants against -- "the first mismatch moved" says nothing about
 * whether a change helped, and on a decoder with in-loop filters a fix in one
 * module routinely moves the first mismatch backwards while removing 90% of the
 * wrong bytes.
 *
 * Same shape as h264_diff.c next door. Also prints the per-picture profile of
 * the first few bad pictures, because "picture 0 is wrong" and "picture 0 is
 * right and picture 1 is wrong" are different bugs (intra/transform vs inter).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "h265.h"

static uint8_t *slurp(const char *p, long *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)*n);
    if (!b || fread(b, 1, (size_t)*n, f) != (size_t)*n) { fprintf(stderr, "short\n"); exit(1); }
    fclose(f); return b;
}

static long cmp_plane(const uint8_t *got, int stride, const uint8_t *want,
                      int w, int h, long *first, int *maxdelta)
{
    long bad = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int g = got[(long)y * stride + x], r = want[(long)y * w + x];
            if (g != r) {
                if (!bad && first) *first = (long)y * w + x;
                int dd = g > r ? g - r : r - g;
                if (maxdelta && dd > *maxdelta) *maxdelta = dd;
                bad++;
            }
        }
    return bad;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <h265> <ref.yuv>\n", argv[0]); return 1; }
    long slen, rlen;
    uint8_t *s = slurp(argv[1], &slen), *ref = slurp(argv[2], &rlen);

    h265dec *d = h265_open();
    long off = 0, roff = 0;
    long bad_y = 0, bad_u = 0, bad_v = 0;
    int frames = 0, badframes = 0, maxdelta = 0;

    for (;;) {
        h265frame f; int got = 0, n;
        if (off < slen) {
            n = h265_decode(d, s + off, (int)(slen - off), &f, &got);
            if (n < 0) { printf("decode error %d after %d pictures\n", n, frames); break; }
            if (n == 0 && !got) break;
            off += n;
            if (!got) { if (off < slen) continue; }
        }
        if (!got) { if (h265_flush(d, &f) != 1) break; }

        int W = f.width, H = f.height, CW = (W + 1) / 2, CH = (H + 1) / 2;
        long need = (long)W * H + 2L * CW * CH;
        if (roff + need > rlen) { printf("  ref exhausted at picture %d\n", frames); break; }
        long by = cmp_plane(f.y, f.stride_y, ref + roff, W, H, 0, &maxdelta);
        long bu = cmp_plane(f.u, f.stride_c, ref + roff + (long)W * H, CW, CH, 0, &maxdelta);
        long bv = cmp_plane(f.v, f.stride_c, ref + roff + (long)W * H + (long)CW * CH,
                            CW, CH, 0, &maxdelta);
        if (by | bu | bv) {
            badframes++;
            if (badframes <= 3)
                printf("  bad picture %d (poc %d): Y %ld  U %ld  V %ld\n",
                       frames, (int)f.poc, by, bu, bv);
        }
        bad_y += by; bad_u += bu; bad_v += bv;
        roff += need;
        frames++;
    }
    printf("  pictures %d, bad %d, bytes wrong: Y %ld  U %ld  V %ld  total %ld  maxdelta %d\n",
           frames, badframes, bad_y, bad_u, bad_v, bad_y + bad_u + bad_v, maxdelta);
    h265_close(d);
    free(s);
    free(ref);
    return (bad_y + bad_u + bad_v) ? 1 : 0;
}
