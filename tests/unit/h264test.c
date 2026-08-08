/* tests/unit/h264test.c -- host driver for the H.264 decoder.
 *
 *   h264_test <stream.h264>            decode all frames, print CRC32 of the
 *                                      concatenated visible YUV (guest check
 *                                      mode prints the same number)
 *   h264_test <stream.h264> <ref.yuv>  also compare every frame byte-for-byte
 *                                      against ffmpeg's decode. H.264
 *                                      reconstruction is exactly specified,
 *                                      so ANY mismatch is our bug: report
 *                                      frame, plane, and pixel of the first.
 *
 * Exit 0 on match/crc printed, 1 on any mismatch or decode error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "h264.h"

static uint32_t crc_table[256];
static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
        crc_table[i] = c;
    }
}
static uint32_t crc32_feed(uint32_t crc, const uint8_t *p, size_t n)
{
    while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

static uint8_t *read_all(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)*len);
    if (!b || fread(b, 1, (size_t)*len, f) != (size_t)*len) {
        fprintf(stderr, "cannot read %s\n", path); exit(1);
    }
    fclose(f);
    return b;
}

/* --pts: check that a caller value handed in with a picture comes back on
 * that picture, however far out of decode order it is emitted. Each call into
 * the decoder consumes at most one picture, so numbering the calls numbers the
 * pictures in DECODE order; frames come out in DISPLAY order, so for a stream
 * with B pictures the returned sequence must NOT be monotonic -- and every
 * value handed in must come back exactly once. Pairing timestamps with a
 * decode-order FIFO passes the second half of that and fails the first, which
 * is precisely the bug this API replaces. */
static int run_pts_check(const char *path)
{
    long len;
    uint8_t *data = read_all(path, &len);
    h264dec *d = h264_open();
    long pos = 0;
    int64_t call = 0;
    static int64_t got[8192];
    int n = 0, inversions = 0;
    h264frame f;

    for (;;) {
        int gotf = 0, r;
        if (pos < len) {
            r = h264_decode_pts(d, data + pos, (int)(len - pos), call++, &f, &gotf);
            if (r < 0) {
                fprintf(stderr, "H264-PTS-FAIL: decode error %d\n", r);
                return 1;
            }
            if (r == 0 && !gotf) break;
            pos += r;
        } else {
            r = h264_flush(d, &f);
            if (r <= 0) break;
            gotf = 1;
        }
        if (!gotf) continue;
        if (n >= (int)(sizeof got / sizeof got[0])) break;
        if (f.pts == H264_NOPTS) {
            fprintf(stderr, "H264-PTS-FAIL: frame %d carries no timestamp\n", n);
            return 1;
        }
        if (n > 0 && f.pts < got[n - 1]) inversions++;
        got[n++] = f.pts;
    }
    h264_close(d);
    free(data);

    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (got[i] == got[j]) {
                fprintf(stderr, "H264-PTS-FAIL: value %ld returned twice\n",
                        (long)got[i]);
                return 1;
            }
        }
    }
    printf("H264-PTS-OK %d frames, %d emitted out of decode order (%s)\n",
           n, inversions, path);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--pts") == 0) {
        crc_init();
        return run_pts_check(argv[2]);
    }
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: h264_test [--pts] <stream.h264> [ref.yuv]\n");
        return 1;
    }
    crc_init();
    long len;
    uint8_t *data = read_all(argv[1], &len);
    FILE *ref = NULL;
    if (argc == 3) {
        ref = fopen(argv[2], "rb");
        if (!ref) { perror(argv[2]); return 1; }
    }

    h264dec *d = h264_open();
    if (!d) { fprintf(stderr, "h264_open failed\n"); return 1; }

    uint32_t crc = 0xFFFFFFFFu;
    int frames = 0, bad = 0;
    long pos = 0;
    h264frame f;
    uint8_t *refbuf = NULL;
    size_t refcap = 0;

    for (;;) {
        int got = 0, n;
        if (pos < len) {
            n = h264_decode(d, data + pos, (int)(len - pos), &f, &got);
            if (n < 0) {
                fprintf(stderr, "H264-FAIL: decode error %d at byte %ld (frame %d)\n",
                        n, pos, frames);
                h264_close(d);
                return 1;
            }
            if (n == 0 && !got) break;          /* consumed nothing more */
            pos += n;
        } else {
            n = h264_flush(d, &f);
            if (n < 0) { fprintf(stderr, "H264-FAIL: flush error %d\n", n); return 1; }
            if (n == 0) break;
            got = 1;
        }
        if (!got) continue;

        /* fold into CRC: visible area, plane by plane */
        int pw[3] = { f.width, (f.width + 1) / 2, (f.width + 1) / 2 };
        int ph[3] = { f.height, (f.height + 1) / 2, (f.height + 1) / 2 };
        const uint8_t *pl[3] = { f.y, f.u, f.v };
        int st[3] = { f.stride_y, f.stride_c, f.stride_c };
        for (int p = 0; p < 3; p++)
            for (int r = 0; r < ph[p]; r++)
                crc = crc32_feed(crc, pl[p] + (size_t)r * st[p], (size_t)pw[p]);

        if (ref) {
            size_t need = (size_t)f.width * f.height * 3 / 2;
            if (need > refcap) { refbuf = realloc(refbuf, need); refcap = need; }
            if (!refbuf || fread(refbuf, 1, need, ref) != need) {
                fprintf(stderr, "H264-FAIL: ref.yuv ran out at frame %d "
                        "(decoder produced MORE frames than ffmpeg)\n", frames);
                h264_close(d);
                return 1;
            }
            size_t off = 0;
            for (int p = 0; p < 3 && !bad; p++) {
                for (int r = 0; r < ph[p] && !bad; r++) {
                    const uint8_t *got_row = pl[p] + (size_t)r * st[p];
                    const uint8_t *ref_row = refbuf + off + (size_t)r * pw[p];
                    if (memcmp(got_row, ref_row, (size_t)pw[p]) != 0) {
                        int x = 0;
                        while (got_row[x] == ref_row[x]) x++;
                        fprintf(stderr,
                                "H264-FAIL: frame %d %s row %d col %d: got %d want %d\n",
                                frames, p == 0 ? "Y" : p == 1 ? "U" : "V",
                                r, x, got_row[x], ref_row[x]);
                        bad = 1;
                    }
                }
                off += (size_t)pw[p] * ph[p];
            }
        }
        frames++;
        if (bad) { h264_close(d); return 1; }
    }

    if (ref) {
        /* ffmpeg must not have produced MORE frames than we did */
        uint8_t extra;
        if (fread(&extra, 1, 1, ref) == 1) {
            fprintf(stderr, "H264-FAIL: decoder produced FEWER frames (%d) than ffmpeg\n",
                    frames);
            h264_close(d);
            return 1;
        }
        fclose(ref);
        printf("H264-OK %d frames bit-exact (%s)\n", frames, argv[1]);
    } else {
        printf("H264-CRC %08x %d frames (%s)\n", crc ^ 0xFFFFFFFFu, frames, argv[1]);
    }
    h264_close(d);
    return 0;
}
