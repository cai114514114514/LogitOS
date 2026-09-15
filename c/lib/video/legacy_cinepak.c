/* c/lib/video/legacy_cinepak.c -- Cinepak (CVID) decoder.
 *
 * Ported from ffmpeg's libavcodec/cinepak.c (LGPL 2.1+, Dr. Tim Ferguson's
 * public format writeup cross-checked the chunk-ID table), adapted to a
 * pull-model API over caller-owned buffers instead of AVFrame/AVPacket. The
 * per-chunk switch, the codebook layout, the strip geometry rules (including
 * the "y1==0 means relative to the previous strip's y2" rule) and the exact
 * integer YUV->RGB conversion are copied algorithm-for-algorithm; nothing
 * here is a guess at what the format does.
 *
 * NOT IMPLEMENTED, BY NAME (features the real cinepak.c has that this file
 * does not, because no test fixture in this tree's gate needs them and
 * every one is a namable simplification, not an unnoticed gap):
 *   - Sega FILM/CPK's 2- or 6-byte header skip quirk (that container is not
 *     one this tree reads at all; sega_film_skip_bytes is always 0 here).
 *   - The "damaged frame" discard heuristic (a demuxer-level judgment call,
 *     not part of decoding a chunk you were already handed).
 * Both are pure ffmpeg robustness hacks against a specific badly-formed
 * source, not part of the CVID format itself.
 *
 * INTEGRATION LINES (not written here; see the phase report for context):
 *   demux.c sniff:  fourcc 'cvid' inside an AVI 'strf' -> legacy_cinepak
 *   media.h ops:    { "cvid", legacy_cinepak_open, legacy_cinepak_decode }
 *   canPlayType:    video/x-cinepak, video/avi;codecs=cvid
 */
#include <string.h>
#include <stdlib.h>
#include "legacy.h"

static int clip_u8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static uint32_t rb16(const uint8_t *p) { return (uint32_t)(p[0] << 8) | p[1]; }
static uint32_t rb24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }

int legacy_cinepak_open(legacy_cinepak_ctx *c, int width, int height, int gray)
{
    if (width < 4 || height < 4 || width > 8192 || height > 8192)
        return LEGACY_ERR_UNSUPPORTED;
    memset(c, 0, sizeof(*c));
    c->width = width;
    c->height = height;
    c->rw = (width + 3) & ~3;
    c->rh = (height + 3) & ~3;
    c->gray = gray ? 1 : 0;
    c->bpp = c->gray ? 1 : 3;
    c->frame = (uint8_t *)calloc((size_t)c->rw * c->bpp, (size_t)c->rh);
    if (!c->frame) return LEGACY_ERR_OOM;
    return LEGACY_OK;
}

void legacy_cinepak_close(legacy_cinepak_ctx *c)
{
    free(c->frame);
    c->frame = NULL;
}

/* chunk_id bit 0x04: 1 => 4-byte (grayscale) entries, 0 => 6-byte (color).
 * chunk_id bit 0x01: 1 => "sparse" -- a flag dword gates every 32 entries. */
static void decode_codebook(cvid_codebook *cb, int chunk_id, int size, const uint8_t *data)
{
    const uint8_t *eod = data + size;
    uint32_t flag = 0, mask = 0;
    int n = (chunk_id & 0x04) ? 4 : 6;

    for (int i = 0; i < 256; i++) {
        if ((chunk_id & 0x01) && !(mask >>= 1)) {
            if (data + 4 > eod) break;
            flag = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                   ((uint32_t)data[2] << 8) | data[3];
            data += 4;
            mask = 0x80000000u;
        }

        if (!(chunk_id & 0x01) || (flag & mask)) {
            if (data + n > eod) break;
            uint8_t *p = cb->v[i];
            for (int k = 0; k < 4; k++) {
                int y = data[k];
                p[k * 3 + 0] = p[k * 3 + 1] = p[k * 3 + 2] = (uint8_t)y;
            }
            data += 4;
            if (n == 6) {
                int u = (int8_t)data[0];
                int v = (int8_t)data[1];
                data += 2;
                for (int k = 0; k < 4; k++) {
                    int y = p[k * 3 + 0]; /* the Y byte just stored */
                    int r = y + 2 * v;
                    int g = y - (u / 2) - v;
                    int b = y + 2 * u;
                    p[k * 3 + 0] = (uint8_t)clip_u8(r);
                    p[k * 3 + 1] = (uint8_t)clip_u8(g);
                    p[k * 3 + 2] = (uint8_t)clip_u8(b);
                }
            }
        }
        /* else: entry i keeps whatever it already held (sparse skip). */
    }
}

/* V4 quadrant: codebook entry p's OWN four samples (its internal TL,TR,BL,BR
 * at byte offsets 0,1,2,3) become a 2x2 pixel block at physical (x,y). */
static void put_quad(legacy_cinepak_ctx *c, const uint8_t *p, int x, int y)
{
    int stride = c->rw * c->bpp;
    uint8_t *row0 = c->frame + y * stride + x * c->bpp;
    uint8_t *row1 = row0 + stride;
    if (c->gray) {
        row0[0] = p[0 * 3]; row0[1] = p[1 * 3];
        row1[0] = p[2 * 3]; row1[1] = p[3 * 3];
    } else {
        memcpy(row0 + 0, p + 0 * 3, 3);
        memcpy(row0 + 3, p + 1 * 3, 3);
        memcpy(row1 + 0, p + 2 * 3, 3);
        memcpy(row1 + 3, p + 3 * 3, 3);
    }
}

/* V1 quadrant: ONE sample (a single Y byte or RGB triple) is flat-filled
 * across a 2x2 pixel block at physical (x,y). */
static void put_solid2x2(legacy_cinepak_ctx *c, const uint8_t *sample, int x, int y)
{
    int stride = c->rw * c->bpp;
    uint8_t *row0 = c->frame + y * stride + x * c->bpp;
    uint8_t *row1 = row0 + stride;
    if (c->gray) {
        row0[0] = row0[1] = row1[0] = row1[1] = sample[0];
    } else {
        memcpy(row0 + 0, sample, 3); memcpy(row0 + 3, sample, 3);
        memcpy(row1 + 0, sample, 3); memcpy(row1 + 3, sample, 3);
    }
}

/* Refill the shared 32-bit flag/mask bit reader from the next big-endian
 * dword, exactly mirroring cinepak_decode_vectors's `!(mask >>= 1)` refill
 * idiom: mask starts at 0, so the first `>>=1` on any path is always 0,
 * triggering a refill before the first bit of a new flag word is read. */
static int refill(uint32_t *flag, uint32_t *mask, const uint8_t **data, const uint8_t *eod)
{
    if (*data + 4 > eod) return LEGACY_ERR_CORRUPT;
    *flag = ((uint32_t)(*data)[0] << 24) | ((uint32_t)(*data)[1] << 16) |
            ((uint32_t)(*data)[2] << 8) | (*data)[3];
    *data += 4;
    *mask = 0x80000000u;
    return LEGACY_OK;
}

static int decode_vectors(legacy_cinepak_ctx *c, cvid_strip_geom *strip,
                           cvid_codebook *v1, cvid_codebook *v4,
                           int chunk_id, int size, const uint8_t *data)
{
    const uint8_t *eod = data + size;
    uint32_t flag = 0, mask = 0;

    for (int y = strip->y1; y < strip->y2; y += 4) {
        for (int x = strip->x1; x < strip->x2; x += 4) {
            /* Bit 1: is this block coded at all (else: unchanged / delta
             * skip)? Only chunks with the 0x01 "sparse" bit carry this. */
            if ((chunk_id & 0x01) && !(mask >>= 1)) {
                int r = refill(&flag, &mask, &data, eod);
                if (r != LEGACY_OK) return r;
            }

            if (!(chunk_id & 0x01) || (flag & mask)) {
                /* Bit 2 (same continuous bitstream): V1 (0) or V4 (1) --
                 * only when the chunk doesn't already force one (bit 0x02). */
                if (!(chunk_id & 0x02) && !(mask >>= 1)) {
                    int r = refill(&flag, &mask, &data, eod);
                    if (r != LEGACY_OK) return r;
                }

                if ((chunk_id & 0x02) || !(flag & mask)) {
                    /* V1: one codebook entry, its 4 samples flat-fill the
                     * block's four 2x2 quadrants (TL,TR,BL,BR). */
                    if (data >= eod) return LEGACY_ERR_CORRUPT;
                    const uint8_t *p = v1->v[*data++];
                    put_solid2x2(c, p + 0 * 3, x,     y);
                    put_solid2x2(c, p + 1 * 3, x + 2, y);
                    put_solid2x2(c, p + 2 * 3, x,     y + 2);
                    put_solid2x2(c, p + 3 * 3, x + 2, y + 2);
                } else {
                    /* V4: four codebook entries, one per quadrant, each
                     * entry's own internal 2x2 pattern. */
                    if (data + 4 > eod) return LEGACY_ERR_CORRUPT;
                    const uint8_t *q0 = v4->v[data[0]];
                    const uint8_t *q1 = v4->v[data[1]];
                    const uint8_t *q2 = v4->v[data[2]];
                    const uint8_t *q3 = v4->v[data[3]];
                    data += 4;
                    put_quad(c, q0, x,     y);
                    put_quad(c, q1, x + 2, y);
                    put_quad(c, q2, x,     y + 2);
                    put_quad(c, q3, x + 2, y + 2);
                }
            }
            /* else: block unchanged (delta skip). */
        }
    }
    return LEGACY_OK;
}

static int decode_strip(legacy_cinepak_ctx *c, cvid_strip_geom *strip,
                         cvid_codebook *v1, cvid_codebook *v4,
                         const uint8_t *data, int size)
{
    const uint8_t *eod = data + size;

    if (strip->x2 > c->rw || strip->y2 > c->rh ||
        strip->x1 >= strip->x2 || strip->y1 >= strip->y2)
        return LEGACY_ERR_CORRUPT;

    while (data + 4 <= eod) {
        int chunk_id = data[0];
        int chunk_size = (int)rb24(data + 1) - 4;
        if (chunk_size < 0) return LEGACY_ERR_CORRUPT;
        data += 4;
        if (data + chunk_size > eod) chunk_size = (int)(eod - data);

        switch (chunk_id) {
        case 0x20: case 0x21: case 0x24: case 0x25:
            decode_codebook(v4, chunk_id, chunk_size, data);
            break;
        case 0x22: case 0x23: case 0x26: case 0x27:
            decode_codebook(v1, chunk_id, chunk_size, data);
            break;
        case 0x30: case 0x31: case 0x32:
            return decode_vectors(c, strip, v1, v4, chunk_id, chunk_size, data);
        default:
            break; /* unknown chunk type: skip, as ffmpeg does */
        }
        data += chunk_size;
    }
    return LEGACY_ERR_CORRUPT; /* strip never reached a vector chunk */
}

int legacy_cinepak_decode(legacy_cinepak_ctx *c, const uint8_t *data, int size)
{
    if (size < 10) return LEGACY_ERR_CORRUPT;
    const uint8_t *eod = data + size;

    int frame_flags = data[0];
    int num_strips = (int)rb16(data + 8);
    if (num_strips > CVID_MAX_STRIPS) num_strips = CVID_MAX_STRIPS;
    data += 10;

    int y0 = 0;
    for (int i = 0; i < num_strips; i++) {
        if (data + 12 > eod) return LEGACY_ERR_CORRUPT;

        cvid_strip_geom *st = &c->strips[i];
        st->id = data[0];
        uint32_t y1f = rb16(data + 4);
#ifdef CINEPAK_CONTROL_ABS_Y1
        /* The negative control: drop the "zero y1 means relative to the
         * previous strip" rule (ffmpeg's own cinepak.c carries the identical
         * comment at the identical field -- this is not a mutilation, it is
         * the naive reading of the format that a decoder written without
         * that one-line comment would produce) and always treat y1 as an
         * absolute row. Every real encoder emits y1=0 for every strip after
         * the first, so this is wrong on ordinary multi-strip content, not
         * an edge case. */
        st->y1 = (int)y1f;
        st->y2 = (int)rb16(data + 8);
#else
        if (y1f == 0) {
            st->y1 = y0;
            st->y2 = y0 + (int)rb16(data + 8);
        } else {
            st->y1 = (int)y1f;
            st->y2 = (int)rb16(data + 8);
        }
#endif
        st->x1 = (int)rb16(data + 6);
        st->x2 = (int)rb16(data + 10);

        int strip_size = (int)rb24(data + 1) - 12;
        if (strip_size < 0) return LEGACY_ERR_CORRUPT;
        data += 12;
        if (data + strip_size > eod) strip_size = (int)(eod - data);

        if (i > 0 && !(frame_flags & 0x01)) {
            c->v4_codebook[i] = c->v4_codebook[i - 1];
            c->v1_codebook[i] = c->v1_codebook[i - 1];
        }

        int r = decode_strip(c, st, &c->v1_codebook[i], &c->v4_codebook[i], data, strip_size);
        if (r != LEGACY_OK) return r;

        data += strip_size;
        y0 = st->y2;
    }
    c->nstrips = num_strips;
    return LEGACY_OK;
}
