#include "img.h"

void *kmalloc(unsigned long);
void  kfree(void *);

/* EXIF orientation.
 *
 * A camera does not rotate its sensor. A phone held in portrait writes the
 * pixels in the sensor's landscape frame and records a tag saying which way up
 * the result goes; a decoder that ignores the tag renders every portrait photo
 * on its side. It is one 16-bit value, and it is the difference between "the
 * photo is wrong" and "the photo is right" on the majority of photos a phone
 * has ever taken -- which is why it is here rather than filed as metadata.
 *
 * The tag lives in a TIFF IFD, which reaches us through four different
 * containers, so the finder below handles all four: a bare TIFF, a JPEG APP1
 * segment (prefixed "Exif\0\0"), a PNG `eXIf` chunk, and a WebP `EXIF` chunk.
 *
 * SECURITY: this parses attacker-controlled bytes and follows an offset stored
 * IN those bytes into the same buffer. Every read goes through rd16/rd32, which
 * take the buffer length and return a sentinel rather than reading out of
 * range, and the IFD entry count is capped. Failure is always "orientation 1",
 * i.e. leave the image alone -- an unparseable tag must never be a crash and
 * must never be a rotation. */

#define ORIENT_TAG 0x0112

static long rd16(const uint8_t *p, long n, long off, int be)
{
    if (off < 0 || off + 2 > n) return -1;
    return be ? ((long)p[off] << 8 | p[off + 1]) : ((long)p[off + 1] << 8 | p[off]);
}

static long rd32(const uint8_t *p, long n, long off, int be)
{
    if (off < 0 || off + 4 > n) return -1;
    if (be) return ((long)p[off] << 24) | ((long)p[off+1] << 16) | ((long)p[off+2] << 8) | p[off+3];
    return ((long)p[off+3] << 24) | ((long)p[off+2] << 16) | ((long)p[off+1] << 8) | p[off];
}

/* Locate the TIFF block inside whatever container `p` is. Returns its offset,
 * or -1. `*tlen` gets the bytes available from there to the end of the file. */
static long find_tiff(const uint8_t *p, long n, long *tlen)
{
    if (n < 8) return -1;

    /* A bare TIFF (also how a .tif would arrive). */
    if ((p[0] == 'I' && p[1] == 'I' && p[2] == 42 && p[3] == 0) ||
        (p[0] == 'M' && p[1] == 'M' && p[2] == 0 && p[3] == 42)) {
        *tlen = n; return 0;
    }

    /* JPEG: walk the marker segments for APP1/Exif. */
    if (p[0] == 0xFF && p[1] == 0xD8) {
        long i = 2;
        while (i + 4 <= n) {
            if (p[i] != 0xFF) { i++; continue; }        /* resync over fill bytes */
            int m = p[i + 1];
            if (m == 0xFF) { i++; continue; }
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
            if (m == 0xDA || m == 0xD9) break;          /* scan data / end: no more headers */
            long slen = rd16(p, n, i + 2, 1);
            if (slen < 2 || i + 2 + slen > n) break;
            if (m == 0xE1 && slen >= 8 &&
                p[i+4]=='E' && p[i+5]=='x' && p[i+6]=='i' && p[i+7]=='f' &&
                p[i+8]==0 && p[i+9]==0) {
                *tlen = i + 2 + slen - (i + 10);
                return i + 10;
            }
            i += 2 + slen;
        }
        return -1;
    }

    /* PNG: an `eXIf` chunk holds the TIFF block directly. */
    if (n >= 8 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') {
        long i = 8;
        while (i + 8 <= n) {
            long clen = rd32(p, n, i, 1);
            if (clen < 0 || clen > 0x7fffffff || i + 12 + clen > n) break;
            if (p[i+4]=='e' && p[i+5]=='X' && p[i+6]=='I' && p[i+7]=='f') {
                *tlen = clen; return i + 8;
            }
            if (p[i+4]=='I' && p[i+5]=='E' && p[i+6]=='N' && p[i+7]=='D') break;
            i += 12 + clen;
        }
        return -1;
    }

    /* WebP: an `EXIF` chunk, optionally with the JPEG-style "Exif\0\0" prefix. */
    if (n >= 16 && p[0]=='R' && p[1]=='I' && p[2]=='F' && p[3]=='F' &&
        p[8]=='W' && p[9]=='E' && p[10]=='B' && p[11]=='P') {
        long i = 12;
        while (i + 8 <= n) {
            long clen = rd32(p, n, i + 4, 0);
            if (clen < 0 || i + 8 + clen > n) break;
            if (p[i]=='E' && p[i+1]=='X' && p[i+2]=='I' && p[i+3]=='F') {
                long o = i + 8, l = clen;
                if (l >= 6 && p[o]=='E' && p[o+1]=='x' && p[o+2]=='i' && p[o+3]=='f' &&
                    p[o+4]==0 && p[o+5]==0) { o += 6; l -= 6; }
                *tlen = l; return o;
            }
            i += 8 + clen + (clen & 1);
        }
        return -1;
    }
    return -1;
}

int exif_orientation(const uint8_t *p, int n)
{
    if (!p || n <= 0) return 1;
    long tlen = 0;
    long toff = find_tiff(p, n, &tlen);
    if (toff < 0 || tlen < 8) return 1;
    const uint8_t *t = p + toff;

    int be = (t[0] == 'M');
    long ifd = rd32(t, tlen, 4, be);
    if (ifd < 8) return 1;
    long cnt = rd16(t, tlen, ifd, be);
    if (cnt < 0 || cnt > 4096) return 1;
    for (long i = 0; i < cnt; i++) {
        long e = ifd + 2 + i * 12;
        long tag = rd16(t, tlen, e, be);
        if (tag < 0) return 1;
        if (tag != ORIENT_TAG) continue;
        long type = rd16(t, tlen, e + 2, be);
        long num  = rd32(t, tlen, e + 4, be);
        if (type != 3 || num != 1) return 1;             /* SHORT, one value */
        /* A 1-element SHORT is stored inline in the value field. Big-endian
         * files put it in the FIRST two bytes of that 4-byte field, which is
         * exactly what rd16 at e+8 reads either way. */
        long v = rd16(t, tlen, e + 8, be);
        return (v >= 1 && v <= 8) ? (int)v : 1;
    }
    return 1;
}

/* Rewrite `im` so that it is upright, i.e. orientation 1. The eight values are
 * the eight ways to map a rectangle onto itself: four rotations, each with or
 * without a mirror. Values 5-8 transpose, so w and h swap. */
int exif_apply(struct image *im, int orientation)
{
    if (!im || !im->rgba) return -1;
    if (orientation <= 1 || orientation > 8) return 0;

    int w = im->w, h = im->h;
    int swap = (orientation >= 5);
    int ow = swap ? h : w, oh = swap ? w : h;
    if (w <= 0 || h <= 0) return -1;

    uint8_t *out = kmalloc((unsigned long)ow * oh * 4);
    if (!out) return -1;

    for (int y = 0; y < oh; y++) {
        for (int x = 0; x < ow; x++) {
            int sx, sy;
            switch (orientation) {
            case 2: sx = w - 1 - x; sy = y;             break;  /* mirror       */
            case 3: sx = w - 1 - x; sy = h - 1 - y;     break;  /* 180          */
            case 4: sx = x;         sy = h - 1 - y;     break;  /* flip         */
            case 5: sx = y;         sy = x;             break;  /* transpose    */
            case 6: sx = y;         sy = h - 1 - x;     break;  /* 90 CW        */
            case 7: sx = w - 1 - y; sy = h - 1 - x;     break;  /* transverse   */
            default: sx = w - 1 - y; sy = x;            break;  /* 8: 90 CCW    */
            }
            const uint8_t *s = im->rgba + ((long)sy * w + sx) * 4;
            uint8_t *d = out + ((long)y * ow + x) * 4;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }
    kfree(im->rgba);
    im->rgba = out; im->w = ow; im->h = oh;
    return 0;
}
