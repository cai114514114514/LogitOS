/* c/lib/video/legacy_qtrle.c -- QuickTime Animation ("rle ") decoder.
 *
 * Ported from ffmpeg's libavcodec/qtrle.c (LGPL 2.1+), algorithm-for-
 * algorithm across all seven bit depths it defines (1,2,4,8,16,24,32bpp):
 * the skip-code / RLE-run / literal-run opcode dispatch, the per-depth pixel
 * group sizes, 1bpp's own row-pointer-and-line-count-off-by-one convention
 * (see the comment inline; it is copied from ffmpeg's own comment and the
 * Trac ticket it cites, not reconstructed), and 24/32bpp's "copy raw bytes
 * two pixels at a time when possible" loop shape.
 *
 * bits_per_coded_sample follows QuickTime's own convention: 33/34/36/40 are
 * 1/2/4/8 with the "greyscale" bit (0x20) added, and they decode through the
 * exact same routine as their un-greyscaled counterpart -- QuickTime's own
 * qtrle_decode_init groups them that way, and so does legacy_qtrle_open
 * (`depth & 0x1f` alone selects the routine). What the greyscale bit changes
 * is only how a PALETTE gets built from indices, which is the caller's job
 * here (see legacy.h's output-convention note), not this file's.
 *
 * ENCODER COVERAGE, measured (`ffmpeg -h encoder=qtrle`): rgb24, rgb555be,
 * argb, gray. That reaches 8 (via gray, depth 40), 16, 24 and 32bpp for the
 * byte-exact gate. It reaches NEITHER an arbitrary (non-grayscale) 8bpp
 * palette NOR 1/2/4bpp at all -- ffmpeg's own qtrle encoder never emits
 * them. Those four cases in tests/legacy.mk are hand-authored bitstreams
 * (tools/genlegacy.sh writes the skip/RLE/literal opcode bytes directly)
 * wrapped in a hand-built MOV 'stsd' entry carrying an explicit in-place
 * color table (ff_get_qtpalette's "color table ID is 0" branch) so there is
 * no dependency on ffmpeg's default-palette guesswork -- and then decoded
 * by ffmpeg's own (real, independent) DECODER as the oracle. The report
 * says plainly that the INPUT bytes are hand-authored for those four; the
 * verification is still a real differential, not self-consistency.
 *
 * NOT bug-for-bug with ffmpeg on truncated/malformed input: ffmpeg's
 * CHECK_PIXEL_PTR silently `return`s a partially-painted frame. This file
 * returns LEGACY_ERR_CORRUPT at the same point instead, for the same reason
 * given in legacy_msvideo1.c -- untrusted input, and a clean error beats a
 * silent partial paint. Never fires on a well-formed frame.
 *
 * INTEGRATION LINES (not written here; see the phase report):
 *   demux.c sniff:  fourcc 'rle ' inside a MOV 'stsd' video sample entry
 *   media.h ops:    { "qtrle", legacy_qtrle_open, legacy_qtrle_decode }
 *   canPlayType:    video/quicktime;codecs=rle
 */
#include <string.h>
#include <stdlib.h>
#include "legacy.h"

/* QuickTime's "depth" convention only ADDS the 0x20 greyscale flag to the
 * four PALETTE depths (1,2,4,8 -> 33,34,36,40); 16/24/32 are plain values
 * that must NOT be masked the same way (32 & 0x1f == 0, which is why this
 * is its own function instead of a bare `depth & 0x1f` at both call sites). */
static int qtrle_route(int depth)
{
    if (depth == 16 || depth == 24 || depth == 32) return depth;
    return depth & 0x1f;
}

int legacy_qtrle_open(legacy_qtrle_ctx *c, int width, int height, int depth)
{
    int d = qtrle_route(depth);
    int bpp_out;
    switch (d) {
    case 1: case 2: case 4: case 8: bpp_out = 1; break;
    case 16: bpp_out = 2; break;
    case 24: bpp_out = 3; break;
    case 32: bpp_out = 4; break;
    default: return LEGACY_ERR_UNSUPPORTED;
    }
    if (width < 4 || height < 4) return LEGACY_ERR_UNSUPPORTED;
    memset(c, 0, sizeof(*c));
    c->width = width;
    c->height = height;
    c->depth = depth;
    c->bpp_out = bpp_out;
    c->frame = (uint8_t *)calloc((size_t)width * bpp_out, (size_t)height);
    if (!c->frame) return LEGACY_ERR_OOM;
    return LEGACY_OK;
}

void legacy_qtrle_close(legacy_qtrle_ctx *c)
{
    free(c->frame);
    c->frame = NULL;
}

/* ---- a tiny bounds-checked byte reader over the chunk payload ---------- */
typedef struct { const uint8_t *p; int size, pos; } rd;
static int rd_left(rd *g) { return g->size - g->pos; }
static int rd_u8(rd *g) { return g->p[g->pos++]; }
static int rd_s8(rd *g) { return (int8_t)g->p[g->pos++]; }
static uint16_t rd_be16(rd *g) { uint16_t v = (uint16_t)((g->p[g->pos] << 8) | g->p[g->pos + 1]); g->pos += 2; return v; }
static uint32_t rd_be32(rd *g) { uint32_t v = ((uint32_t)g->p[g->pos] << 24) | ((uint32_t)g->p[g->pos + 1] << 16) | ((uint32_t)g->p[g->pos + 2] << 8) | g->p[g->pos + 3]; g->pos += 4; return v; }

#define NEED(g, n) do { if (rd_left(g) < (n)) return LEGACY_ERR_CORRUPT; } while (0)
/* CHECK_PIXEL_PTR: pixel_ptr must land within [0, pixel_limit]. */
#define CKPTR(extra) do { if ((pixel_ptr) + (extra) < 0 || (pixel_ptr) + (extra) > pixel_limit) return LEGACY_ERR_CORRUPT; } while (0)

static int dec_1bpp(legacy_qtrle_ctx *c, rd *g, int row_ptr, int lines)
{
    int row_inc = c->width;
    int pixel_limit = c->width * c->height;
    uint8_t *out = c->frame;

    /* ffmpeg's own comment, copied: skip&0x80 means "go to next line" during
     * decode but "go to first line" at the very start; always treating it as
     * "next line" needs row_ptr pre-decremented by one stride and the line
     * count bumped by one so the first line is not lost (ffmpeg cites
     * https://trac.ffmpeg.org/ticket/226). The negative control undoes
     * exactly this: the real, named, historical bug the ticket was filed
     * against, not a mutilation -- a decoder written from the format
     * description without this adjustment loses the first coded line of
     * every 1bpp frame and misplaces every line after it by one row. */
#ifdef QTRLE_CONTROL_TICKET226
    int pixel_ptr = row_ptr;
#else
    row_ptr -= row_inc;
    int pixel_ptr = row_ptr;
    lines += 1;
#endif

    while (lines) {
        NEED(g, 2);
        int skip = rd_u8(g);
        int rle = rd_s8(g);
        if (rle == 0) break;
        if (skip & 0x80) {
            lines--;
            row_ptr += row_inc;
            pixel_ptr = row_ptr + 2 * 8 * (skip & 0x7f);
        } else {
            pixel_ptr += 2 * 8 * skip;
        }
        CKPTR(0);

        if (rle == -1) continue;

        if (rle < 0) {
            rle = -rle;
            NEED(g, 2);
            uint8_t p0 = (uint8_t)rd_u8(g), p1 = (uint8_t)rd_u8(g);
            CKPTR(rle * 2 * 8);
            while (rle--) {
                for (int b = 7; b >= 0; b--) out[pixel_ptr++] = (p0 >> b) & 1;
                for (int b = 7; b >= 0; b--) out[pixel_ptr++] = (p1 >> b) & 1;
            }
        } else {
            rle *= 2;
            CKPTR(rle * 8);
            while (rle--) {
                NEED(g, 1);
                int x = rd_u8(g);
                for (int b = 7; b >= 0; b--) out[pixel_ptr++] = (uint8_t)((x >> b) & 1);
            }
        }
    }
    return LEGACY_OK;
}

static int dec_2n4bpp(legacy_qtrle_ctx *c, rd *g, int row_ptr, int lines, int bpp)
{
    int row_inc = c->width;
    int pixel_limit = c->width * c->height;
    uint8_t *out = c->frame;
    int num_pixels = (bpp == 4) ? 8 : 16;

    while (lines--) {
        NEED(g, 1);
        int pixel_ptr = row_ptr + num_pixels * (rd_u8(g) - 1);
        CKPTR(0);

        int rle;
        for (;;) {
            NEED(g, 1);
            rle = rd_s8(g);
            if (rle == -1) break;
            if (rle == 0) {
                NEED(g, 1);
                pixel_ptr += num_pixels * (rd_u8(g) - 1);
                CKPTR(0);
            } else if (rle < 0) {
                rle = -rle;
                NEED(g, 4);
                uint8_t pi[16];
                /* Faithful translation of qtrle_decode_2n4bpp's own bit
                 * extraction (peek current byte, shift by (i*bpp)&7, mask,
                 * advance the read cursor by one byte every (num_pixels/4)
                 * samples produced) -- copied rather than re-derived. */
                for (int i = num_pixels - 1; i >= 0; i--) {
                    pi[num_pixels - 1 - i] = (uint8_t)((g->p[g->pos] >> ((i * bpp) & 0x07)) & ((1 << bpp) - 1));
                    if ((i & ((num_pixels >> 2) - 1)) == 0) g->pos++;
                }
                CKPTR(rle * num_pixels);
                while (rle--) {
                    memcpy(out + pixel_ptr, pi, (size_t)num_pixels);
                    pixel_ptr += num_pixels;
                }
            } else {
                rle *= 4;
                CKPTR(rle * (num_pixels >> 2));
                while (rle--) {
                    NEED(g, 1);
                    int x = rd_u8(g);
                    if (bpp == 4) {
                        out[pixel_ptr++] = (uint8_t)((x >> 4) & 0x0f);
                        out[pixel_ptr++] = (uint8_t)(x & 0x0f);
                    } else {
                        out[pixel_ptr++] = (uint8_t)((x >> 6) & 0x03);
                        out[pixel_ptr++] = (uint8_t)((x >> 4) & 0x03);
                        out[pixel_ptr++] = (uint8_t)((x >> 2) & 0x03);
                        out[pixel_ptr++] = (uint8_t)(x & 0x03);
                    }
                }
            }
        }
        row_ptr += row_inc;
    }
    return LEGACY_OK;
}

static int dec_8bpp(legacy_qtrle_ctx *c, rd *g, int row_ptr, int lines)
{
    int row_inc = c->width;
    int pixel_limit = c->width * c->height;
    uint8_t *out = c->frame;

    while (lines--) {
        NEED(g, 1);
        int pixel_ptr = row_ptr + 4 * (rd_u8(g) - 1);
        CKPTR(0);
        int rle;
        for (;;) {
            NEED(g, 1);
            rle = rd_s8(g);
            if (rle == -1) break;
            if (rle == 0) {
                NEED(g, 1);
                pixel_ptr += 4 * (rd_u8(g) - 1);
                CKPTR(0);
            } else if (rle < 0) {
                rle = -rle;
                NEED(g, 4);
                uint8_t p1 = (uint8_t)rd_u8(g), p2 = (uint8_t)rd_u8(g),
                        p3 = (uint8_t)rd_u8(g), p4 = (uint8_t)rd_u8(g);
                CKPTR(rle * 4);
                while (rle--) {
                    out[pixel_ptr++] = p1; out[pixel_ptr++] = p2;
                    out[pixel_ptr++] = p3; out[pixel_ptr++] = p4;
                }
            } else {
                rle *= 4;
                CKPTR(rle);
                NEED(g, rle);
                memcpy(out + pixel_ptr, g->p + g->pos, (size_t)rle);
                g->pos += rle;
                pixel_ptr += rle;
            }
        }
        row_ptr += row_inc;
    }
    return LEGACY_OK;
}

/* 16bpp is NOT a raw byte passthrough: ffmpeg's qtrle_decode_16bpp reads
 * each source pixel with an explicit big-endian `bytestream2_get_be16` and
 * stores it through a native uint16_t*. On this project's little-endian
 * target that is a byte SWAP relative to the file, unlike 24/32bpp (which
 * use `get_ne16`/`get_ne32` -- read-native then write-native, a pure
 * passthrough on any host). Two bugs would hide behind a memcpy here: one
 * where every 16bpp pixel comes out byte-reversed, the other where nothing
 * in this file's own test corpus would show it, because RPZA and MS Video 1
 * already do their own explicit big-endian reads elsewhere in this
 * directory -- this is the one place that memcpy would have been wrong. */
static void put16_native(uint8_t *dst, uint16_t v) { memcpy(dst, &v, 2); }
static uint16_t get16_be(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static int dec_nbpp_literal(legacy_qtrle_ctx *c, rd *g, int row_ptr, int lines, int bytes_per_px)
{
    /* Shared shape of 16/24/32bpp: one pixel group == one pixel, skip codes
     * count pixels (not groups-of-4 like 2/4/8bpp). */
    int row_inc = c->width;
    int pixel_limit = c->width * c->height;
    uint8_t *out = c->frame;

    while (lines--) {
        NEED(g, 1);
        int pixel_ptr = row_ptr + (rd_u8(g) - 1);
        CKPTR(0);
        int rle;
        for (;;) {
            NEED(g, 1);
            rle = rd_s8(g);
            if (rle == -1) break;
            if (rle == 0) {
                NEED(g, 1);
                pixel_ptr += (rd_u8(g) - 1);
                CKPTR(0);
            } else if (rle < 0) {
                rle = -rle;
                NEED(g, bytes_per_px);
                CKPTR(rle);
                if (bytes_per_px == 2) {
                    uint16_t v = get16_be(g->p + g->pos);
                    g->pos += 2;
                    for (int k = 0; k < rle; k++)
                        put16_native(out + (size_t)(pixel_ptr + k) * 2, v);
                } else {
                    for (int k = 0; k < rle; k++)
                        memcpy(out + (size_t)(pixel_ptr + k) * bytes_per_px, g->p + g->pos, (size_t)bytes_per_px);
                    g->pos += bytes_per_px;
                }
                pixel_ptr += rle;
            } else {
                CKPTR(rle);
                NEED(g, rle * bytes_per_px);
                if (bytes_per_px == 2) {
                    for (int k = 0; k < rle; k++) {
                        put16_native(out + (size_t)(pixel_ptr + k) * 2, get16_be(g->p + g->pos));
                        g->pos += 2;
                    }
                } else {
                    memcpy(out + (size_t)pixel_ptr * bytes_per_px, g->p + g->pos, (size_t)rle * bytes_per_px);
                    g->pos += rle * bytes_per_px;
                }
                pixel_ptr += rle;
            }
        }
        row_ptr += row_inc;
    }
    return LEGACY_OK;
}

int legacy_qtrle_decode(legacy_qtrle_ctx *c, const uint8_t *data, int size)
{
    if (size < 8) return LEGACY_OK; /* NOP frame: previous picture unchanged */

    rd g0 = { data, size, 0 }, *g = &g0;
    uint32_t chunk_size = rd_be32(g) & 0x3FFFFFFFu;
    (void)chunk_size; /* container already told us the exact sample length */

    uint16_t header = rd_be16(g);
    int start_line, lines;
    if (header & 0x0008) {
        if (size < 14) return LEGACY_OK; /* malformed extended header: NOP */
        start_line = rd_be16(g);
        g->pos += 2;
        lines = rd_be16(g);
        g->pos += 2;
        if (lines > c->height - start_line) return LEGACY_OK;
    } else {
        start_line = 0;
        lines = c->height;
    }

    int row_ptr = start_line * c->width; /* in PIXELS; each dec_* scales by bpp itself where relevant */
    int d = qtrle_route(c->depth);
    switch (d) {
    case 1:  return dec_1bpp(c, g, row_ptr, lines);
    case 2:  return dec_2n4bpp(c, g, row_ptr, lines, 2);
    case 4:  return dec_2n4bpp(c, g, row_ptr, lines, 4);
    case 8:  return dec_8bpp(c, g, row_ptr, lines);
    case 16: return dec_nbpp_literal(c, g, row_ptr, lines, 2);
    case 24: return dec_nbpp_literal(c, g, row_ptr, lines, 3);
    case 32: return dec_nbpp_literal(c, g, row_ptr, lines, 4);
    default: return LEGACY_ERR_UNSUPPORTED;
    }
}
