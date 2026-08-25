/* tests/unit/mpeg12_test.c -- host driver for the MPEG-1/2 video decoder.
 *
 *   mpeg12_test <stream.m2v>              decode all frames, print the CRC32
 *                                         of the concatenated visible YUV
 *   mpeg12_test <stream.m2v> <ref.yuv>    also compare every frame byte for
 *                                         byte against ffmpeg's decode and
 *                                         stop at the first difference,
 *                                         naming frame / plane / pixel
 *   mpeg12_test --diff <stream> <ref.yuv> compare the WHOLE stream and print
 *                                         totals: frames, wrong bytes, worst
 *                                         |delta|, and the first bad pixel
 *
 * --diff exists because "the first mismatch moved" says nothing about whether
 * a change helped. A per-case wrong-byte total does, and it is what makes the
 * decoder bisectable; the bit-exact mode is the gate.
 *
 * Exit 0 on success, 1 on any mismatch, decode error, or frame-count
 * disagreement -- a decoder that emits four of ten frames must not pass by
 * matching the four.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mpeg12.h"

static uint32_t crc_table[256];
static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
        crc_table[i] = c;
    }
}
static uint32_t crc_feed(uint32_t crc, const uint8_t *p, size_t n)
{
    while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

static uint8_t *read_all(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    b = (uint8_t *)malloc((size_t)(*len ? *len : 1));
    if (!b || fread(b, 1, (size_t)*len, f) != (size_t)*len) {
        fprintf(stderr, "cannot read %s\n", path); exit(1);
    }
    fclose(f);
    return b;
}

struct cmp {
    const uint8_t *ref;
    long reflen, off;
    long bad, total;
    int maxdelta;
    int first_frame, first_plane, first_x, first_y, have_first;
};

/* One plane of one frame against the reference. Returns bytes consumed. */
static void cmp_plane(struct cmp *c, int frame, int plane,
                      const uint8_t *p, int stride, int w, int h)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int got, want, dd;
            if (c->off >= c->reflen) { c->off++; continue; }
            got = p[(long)y * stride + x];
            want = c->ref[c->off++];
            c->total++;
            dd = got - want; if (dd < 0) dd = -dd;
            if (dd) {
                c->bad++;
                if (dd > c->maxdelta) c->maxdelta = dd;
                if (!c->have_first) {
                    c->have_first = 1;
                    c->first_frame = frame; c->first_plane = plane;
                    c->first_x = x; c->first_y = y;
                }
            }
        }
    }
}

static void cmp_frame(struct cmp *c, int n, const mpeg12frame *f)
{
    cmp_plane(c, n, 0, f->y, f->stride_y, f->width, f->height);
    cmp_plane(c, n, 1, f->u, f->stride_c, f->width / 2, f->height / 2);
    cmp_plane(c, n, 2, f->v, f->stride_c, f->width / 2, f->height / 2);
}

int main(int argc, char **argv)
{
    const char *stream, *refpath = 0;
    int diffmode = 0, argi = 1;
    long len, reflen = 0;
    uint8_t *data, *ref = 0;
    mpeg12dec *d;
    mpeg12frame f;
    struct cmp c;
    uint32_t crc = 0xFFFFFFFFu;
    int nframes = 0, w = 0, h = 0, ism2 = 0, rc = 0, censusmode = 0;
    double fps = 0;
    long pos = 0;
    mpeg12_census cen;

    memset(&cen, 0, sizeof cen);
    if (argi < argc && !strcmp(argv[argi], "--diff")) { diffmode = 1; argi++; }
    if (argi < argc && !strcmp(argv[argi], "--census")) { censusmode = 1; argi++; }
    if (argi >= argc) {
        fprintf(stderr, "usage: mpeg12_test [--diff|--census] <stream> [ref.yuv]\n");
        return 1;
    }
    stream = argv[argi++];
    if (argi < argc) refpath = argv[argi++];

    crc_init();
    data = read_all(stream, &len);
    if (refpath) ref = read_all(refpath, &reflen);

    memset(&c, 0, sizeof c);
    c.ref = ref; c.reflen = reflen;

    d = mpeg12_open();
    if (!d) { fprintf(stderr, "oom\n"); return 1; }

    for (;;) {
        int got = 0, n;
        if (pos < len) {
            n = mpeg12_decode(d, data + pos, (int)(len - pos), &f, &got);
            if (n < 0) {
                printf("MPEG12-FAIL %s: decode error %d at byte %ld\n",
                       stream, n, pos);
                rc = 1;
                break;
            }
            if (n == 0) { /* no progress: nothing left to parse */
                pos = len;
                continue;
            }
            pos += n;
        } else {
            got = mpeg12_flush(d, &f);
            if (got <= 0) break;
        }
        if (!got) continue;

        if (!nframes) mpeg12_stream_info(d, &w, &h, &fps, &ism2);
        if (ref) cmp_frame(&c, nframes, &f);
        for (int y = 0; y < f.height; y++)
            crc = crc_feed(crc, f.y + (long)y * f.stride_y, (size_t)f.width);
        for (int y = 0; y < f.height / 2; y++)
            crc = crc_feed(crc, f.u + (long)y * f.stride_c, (size_t)f.width / 2);
        for (int y = 0; y < f.height / 2; y++)
            crc = crc_feed(crc, f.v + (long)y * f.stride_c, (size_t)f.width / 2);
        nframes++;
        if (nframes > 100000) break;
    }
    crc ^= 0xFFFFFFFFu;
    mpeg12_get_census(d, &cen);

    if (censusmode) {
        /* One line, machine-readable, so tests/mpeg12.mk can select the cases
         * a negative control is entitled to redden instead of a person
         * asserting which ones those are. */
        printf("MPEG12-CENSUS %s pictures=%ld field_pictures=%ld mb=%ld "
               "intra=%ld skipped=%ld field_dct=%ld mv_frame=%ld mv_field=%ld "
               "mv_16x8=%ld mv_dualprime=%ld escapes=%ld esc2=%ld "
               "intra_vlc_blocks=%ld saturated=%ld clamped=%ld\n",
               stream, cen.pictures, cen.field_pictures, cen.mb_total,
               cen.mb_intra, cen.mb_skipped, cen.mb_field_dct, cen.mv_frame,
               cen.mv_field, cen.mv_16x8, cen.mv_dualprime,
               cen.escapes, cen.escapes_mpeg1_second, cen.blocks_intra_vlc,
               cen.coeff_saturated, cen.mv_clamped);
        mpeg12_close(d);
        free(data); free(ref);
        return rc;
    }

    if (ref) {
        long want_bytes = (long)nframes * (w * h + 2 * (w / 2) * (h / 2));
        if (want_bytes != reflen) {
            printf("MPEG12-FAIL %s: %d frames = %ld bytes, reference has %ld"
                   " (frame count disagrees)\n", stream, nframes, want_bytes, reflen);
            rc = 1;
        }
        if (c.bad) {
            printf("MPEG12-%s %-26s %dx%d %s frames=%d wrong=%ld/%ld "
                   "maxd=%d first=f%d p%d (%d,%d)%s\n",
                   diffmode ? "DIFF" : "FAIL",
                   stream, w, h, ism2 ? "mpeg2" : "mpeg1", nframes,
                   c.bad, c.total, c.maxdelta, c.first_frame, c.first_plane,
                   c.first_x, c.first_y,
                   cen.mv_clamped ? " [MV CLAMPED]" : "");
            rc = 1;
        } else if (!rc) {
            printf("MPEG12-OK   %-26s %dx%d %s frames=%d bytes=%ld exact"
                   "%s%s\n", stream, w, h, ism2 ? "mpeg2" : "mpeg1",
                   nframes, c.total,
                   cen.mv_dualprime ? " dualprime" : "",
                   cen.coeff_saturated ? " [SATURATED]" : "");
        }
        if (diffmode) rc = 0;      /* --diff reports, it does not judge */
    } else {
        printf("MPEG12-CRC  %s %08x frames=%d %dx%d %s\n",
               stream, crc, nframes, w, h, ism2 ? "mpeg2" : "mpeg1");
    }

    mpeg12_close(d);
    free(data);
    free(ref);
    return rc;
}
