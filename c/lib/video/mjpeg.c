/* c/lib/video/mjpeg.c -- Motion JPEG, built entirely on top of
 * c/lib/image/jpeg.c's img_decode(). See mjpeg.h for the two things this
 * file actually owns (framing, and the default-Huffman-tables splice) and
 * the three things it explicitly does not do.
 *
 * SECURITY: every input byte is UNTRUSTED, same rule as jpeg.c and every
 * other decoder in this tree. The marker walk below never reads past `len`
 * (every advance is bounds-checked before it is taken, subtraction form, no
 * signed overflow) and never trusts a declared segment length past the end
 * of the buffer it was handed. jpeg.c re-validates everything about the
 * frame it is actually asked to decode -- structure, geometry, dimensions --
 * from scratch; nothing walked here is taken on faith by the decode step. */
#include "mjpeg.h"
#include "img.h"

void *kmalloc(unsigned long);
void  kfree(void *);
void *memcpy(void *, const void *, unsigned long);

#include "mjpeg_deftables.inc"

struct mjpegdec {
    unsigned char *rgba;   /* w*h*4, kmalloc'd once at the first decoded frame */
    int w, h;
    int have_size;
};

mjpegdec *mjpeg_open(void)
{
    mjpegdec *d = kmalloc(sizeof *d);
    if (!d) return 0;
    d->rgba = 0; d->w = d->h = 0; d->have_size = 0;
    return d;
}

void mjpeg_close(mjpegdec *d)
{
    if (!d) return;
    if (d->rgba) kfree(d->rgba);
    kfree(d);
}

const char *mjpeg_strerror(int err)
{
    switch (err) {
    case MJPEG_OK:              return "ok";
    case MJPEG_ERR_CORRUPT:     return "not a JPEG frame jpeg.c can decode";
    case MJPEG_ERR_OOM:         return "out of memory";
    case MJPEG_ERR_SIZE_CHANGE: return "SOF geometry changed mid-stream";
    case MJPEG_ERR_TWO_FIELD:   return "AVI two-field interlaced MJPEG (two JPEGs "
                                        "per chunk) is not supported";
    case MJPEG_ERR_NO_DHT:      return "frame has no DHT and default tables are "
                                        "disabled (MJPEG_NO_DEFAULT_DHT)";
    default:                    return "unknown mjpeg error";
    }
}

/* --- marker walk: framing only, no entropy decode -------------------------
 * Walks data[0..n) starting AT an SOI (caller-guaranteed) looking for the
 * matching EOI, and reports along the way whether a DHT segment was seen.
 * Every marker with a length field is skipped BY that length (subtraction
 * form: `seglen < 2 || 2 + seglen > n - i` before the jump is taken, never
 * after), which is what makes this safe against a crafted APPn/COM payload
 * that happens to contain the byte pair FF C4 -- a raw substring search for
 * "is there a DHT" would be fooled by that; skipping every segment by its
 * own declared length, the same way jpeg.c's outer marker loop does, is not.
 *
 * Inside entropy-coded data (after SOS), a literal 0xFF a compliant encoder
 * writes is ALWAYS followed by a stuffed 0x00 (or is an RSTn marker); both
 * are skipped so the scan does not mistake either for the frame's end, and
 * the first UNSTUFFED marker reached there is trusted as real -- the same
 * rule c/lib/image/jpeg.c's br_bit()/next_marker() apply, independently
 * re-derived here rather than shared, because this file does not reach into
 * jpeg.c's internals (they are `static`, on purpose -- see mjpeg.h).
 *
 * Returns 0 with out_len/out_has_dht filled on a clean SOI..EOI walk that
 * never ran off the end; -1 (truncated/corrupt) otherwise. */
static int walk_frame(const unsigned char *d, int n, int *out_len, int *out_has_dht)
{
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return -1;   /* not SOI */
    int has_dht = 0;
    int i = 2;
    while (i + 1 < n) {
        if (d[i] != 0xFF) return -1;          /* every marker starts with 0xFF */
        int m = d[i + 1];
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }  /* TEM/RSTn */
        if (m == 0xD9) {                      /* EOI: frame ends here */
            *out_len = i + 2;
            *out_has_dht = has_dht;
            return 0;
        }
        if (i + 3 >= n) return -1;            /* no room for the length field */
        int seglen = (d[i + 2] << 8) | d[i + 3];
        if (seglen < 2 || seglen > n - i - 2) return -1;   /* subtraction form */
        if (m == 0xC4) has_dht = 1;
        if (m == 0xDA) {                      /* SOS: header, then entropy data */
            i += 2 + seglen;
            while (i + 1 < n) {
                if (d[i] == 0xFF) {
                    int mm = d[i + 1];
                    if (mm == 0x00) { i += 2; continue; }               /* stuffed */
                    if (mm >= 0xD0 && mm <= 0xD7) { i += 2; continue; } /* RSTn */
                    break;   /* real marker: let the outer loop read it */
                }
                i++;
            }
            continue;
        }
        i += 2 + seglen;
    }
    return -1;   /* ran off the end without an EOI: truncated */
}

int mjpeg_next_frame(const unsigned char *data, int len, int *frame_start)
{
    if (!data || len < 4 || !frame_start) return 0;
    int i = 0;
    while (i + 1 < len && !(data[i] == 0xFF && data[i + 1] == 0xD8)) i++;
    if (i + 1 >= len) return 0;               /* no SOI at all: nothing complete */

    int flen, has_dht;
    if (walk_frame(data + i, len - i, &flen, &has_dht)) return MJPEG_ERR_CORRUPT;
    *frame_start = i;
    return flen;
}

int mjpeg_is_two_field(const unsigned char *data, int len)
{
    if (!data || len < 4) return 0;
    int flen, has_dht;
    if (walk_frame(data, len, &flen, &has_dht)) return 0;   /* not even one frame */
    int off = flen;
    if (off + 1 >= len) return 0;             /* nothing after the first frame */
    /* At most ONE 0x00 pad byte can separate two elements inside one RIFF
     * chunk (chunks are word-aligned); anything else between the frames is
     * not this convention, so it is not treated as a match. */
    if (off < len && data[off] == 0x00) off++;
    return (off + 1 < len && data[off] == 0xFF && data[off + 1] == 0xD8);
}

/* One DHT marker segment carrying all four ITU-T.81 Annex K.3 standard
 * tables (table ids 0/1 per class -- luma/chroma -- which is what JFIF and
 * the AVI MJPEG convention both assume). `out` may be NULL to query the
 * size without writing. Returns the segment length in bytes, FF C4 marker
 * included. */
static int build_default_dht(unsigned char *out)
{
    struct { int tc, th; const unsigned char *bits; const unsigned char *vals; int nval; }
    t[4] = {
        { 0, 0, mjpeg_std_dc_luma_bits,   mjpeg_std_dc_luma_vals,   12 },
        { 0, 1, mjpeg_std_dc_chroma_bits, mjpeg_std_dc_chroma_vals, 12 },
        { 1, 0, mjpeg_std_ac_luma_bits,   mjpeg_std_ac_luma_vals,  162 },
        { 1, 1, mjpeg_std_ac_chroma_bits, mjpeg_std_ac_chroma_vals,162 },
    };
    int body = 0;
    for (int k = 0; k < 4; k++) body += 1 + 16 + t[k].nval;
    int total = 4 + body;                     /* FF C4 + 2-byte length + body */
    if (!out) return total;

    out[0] = 0xFF; out[1] = 0xC4;
    out[2] = (unsigned char)((body + 2) >> 8);
    out[3] = (unsigned char)((body + 2) & 0xFF);
    int o = 4;
    for (int k = 0; k < 4; k++) {
        out[o++] = (unsigned char)((t[k].tc << 4) | t[k].th);
        for (int b = 1; b <= 16; b++) out[o++] = t[k].bits[b];
        for (int v = 0; v < t[k].nval; v++) out[o++] = t[k].vals[v];
    }
    return total;
}

int mjpeg_decode_frame(mjpegdec *d, const unsigned char *data, int len, mjpegframe *out)
{
    if (!d || !data || !out) return MJPEG_ERR_CORRUPT;

    int flen, has_dht;
    if (walk_frame(data, len, &flen, &has_dht)) return MJPEG_ERR_CORRUPT;
    if (flen != len) return MJPEG_ERR_CORRUPT;   /* caller must pass exactly one frame */

    const unsigned char *jbytes = data;
    int jlen = len;
    unsigned char *scratch = 0;

    if (!has_dht) {
#ifdef MJPEG_NO_DEFAULT_DHT
        /* NEGATIVE CONTROL: refuse instead of supplying Annex K.3. A stream
         * whose frames genuinely have no DHT must fail here, by name, on the
         * FIRST such frame -- see tests/mjpeg.mk. */
        return MJPEG_ERR_NO_DHT;
#else
        int dlen = build_default_dht(0);
        scratch = kmalloc((unsigned long)len + (unsigned long)dlen);
        if (!scratch) return MJPEG_ERR_OOM;
        memcpy(scratch, data, 2);                       /* SOI */
        build_default_dht(scratch + 2);
        memcpy(scratch + 2 + dlen, data + 2, (unsigned long)len - 2);
        jbytes = scratch;
        jlen = len + dlen;
#endif
    }

    struct image im;
    int rc = img_decode(jbytes, jlen, &im);
    if (scratch) kfree(scratch);
    if (rc != 0) return MJPEG_ERR_CORRUPT;    /* jpeg.c refused it (see mjpeg.h) */

    if (d->have_size && (im.w != d->w || im.h != d->h)) {
        img_free(&im);
        return MJPEG_ERR_SIZE_CHANGE;         /* d->rgba/out untouched: previous frame stands */
    }
    if (!d->have_size) {
        d->rgba = kmalloc((unsigned long)im.w * (unsigned long)im.h * 4);
        if (!d->rgba) { img_free(&im); return MJPEG_ERR_OOM; }
        d->w = im.w; d->h = im.h; d->have_size = 1;
    }
    memcpy(d->rgba, im.rgba, (unsigned long)im.w * (unsigned long)im.h * 4);
    img_free(&im);

    out->width = d->w; out->height = d->h; out->rgba = d->rgba;
    return MJPEG_OK;
}
