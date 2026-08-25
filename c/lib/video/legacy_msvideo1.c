/* c/lib/video/legacy_msvideo1.c -- Microsoft Video 1 ("MSVC"/"CRAM") decoder.
 *
 * Ported from ffmpeg's libavcodec/msvideo1.c (LGPL 2.1+, Mike Melanson),
 * algorithm-for-algorithm: the two bit-depth decode loops, the skip-run
 * code (0x84-0x87 in byte_b), the 1/2/8-color block encodings and the
 * bottom-up block-row iteration order are copied, not reconstructed from a
 * description.
 *
 * Two independent block encodings share one 4x4-block raster, and BOTH are
 * delta codecs: a run of skipped blocks means "leave those blocks exactly as
 * the previous frame left them", so (like the other three files in this
 * directory) a context's frame buffer must persist across calls.
 *
 * The 8-bit (palette) mode has NO ffmpeg ENCODER at all -- confirmed by
 * `ffmpeg -h encoder=msvideo1`, which lists only rgb555le -- so every 8-bit
 * fixture tests/legacy.mk gates is a HAND-AUTHORED bitstream (tools/
 * genlegacy.sh writes the 1/2/8-color block bytes directly, following this
 * same algorithm in reverse). It is still cross-checked against ffmpeg's
 * DECODER, which does support both modes; that decoder is a real,
 * independent oracle even though no real encoder produced the input bytes,
 * and the report says so plainly rather than calling it "byte-exact against
 * ffmpeg" without qualification.
 *
 * NOT bug-for-bug with ffmpeg on truncated input: ffmpeg's CHECK_STREAM_PTR
 * silently `return`s a partially-painted frame (no error) when a stream runs
 * out mid-block. This file returns LEGACY_ERR_CORRUPT at the same point
 * instead -- every byte of an old media file is untrusted input like any
 * other here, and "decoded some pixels, then silently stopped" is a worse
 * contract for a caller than a clean error. This never changes the result
 * on a well-formed (complete) frame, which is all the byte-exact gate feeds
 * it; it only changes what the negative-control fixture gets back.
 *
 * INTEGRATION LINES (not written here; see the phase report):
 *   demux.c sniff:  fourcc 'MSVC'/'CRAM'/'WHAM' inside an AVI 'strf'
 *   media.h ops:    { "msvideo1", legacy_msvideo1_open, legacy_msvideo1_decode }
 *   canPlayType:    video/avi;codecs=msvc
 */
#include <string.h>
#include <stdlib.h>
#include "legacy.h"

/* The negative control: msvideo1.c's own flag-bit convention is the
 * INVERSE of the flags value (bit clear selects colors[1], bit set selects
 * colors[0] -- confirmed against ffmpeg's msvideo1.c, which XORs the same
 * way at all four sites this macro replaces). Memorising the bit sense
 * backwards -- using the flag bit directly instead of its complement -- is
 * exactly the kind of transcription slip this file's own header warns a
 * port invites, and it flips every 2-color and 8-color block's checkerboard
 * without touching skip runs, solid-color blocks, or the 8-bit/16-bit
 * dispatch, so only those two block types redden. */
#ifdef MSVIDEO1_CONTROL_NOFLIP
#define M1_SEL(flags) ((flags) & 1)
#else
#define M1_SEL(flags) (((flags) & 1) ^ 1)
#endif

int legacy_msvideo1_open(legacy_msvideo1_ctx *c, int width, int height, int mode_8bit)
{
    if (width < 4 || height < 4 || (width & 3) || (height & 3))
        return LEGACY_ERR_UNSUPPORTED;
    memset(c, 0, sizeof(*c));
    c->width = width;
    c->height = height;
    c->mode_8bit = mode_8bit ? 1 : 0;
    size_t bpp = c->mode_8bit ? 1 : 2;
    c->frame = (uint8_t *)calloc((size_t)width * bpp, (size_t)height);
    if (!c->frame) return LEGACY_ERR_OOM;
    return LEGACY_OK;
}

void legacy_msvideo1_close(legacy_msvideo1_ctx *c)
{
    free(c->frame);
    c->frame = NULL;
}

#define CHECK(n) do { if (sp + (n) > size) return LEGACY_ERR_CORRUPT; } while (0)

static int decode_8bit(legacy_msvideo1_ctx *c, const uint8_t *buf, int size)
{
    int blocks_wide = c->width / 4, blocks_high = c->height / 4;
    int total_blocks = blocks_wide * blocks_high;
    int stride = c->width;
    int row_dec = stride + 4;
    int sp = 0, skip_blocks = 0;

    for (int by = blocks_high; by > 0; by--) {
        int block_ptr = ((by * 4) - 1) * stride;
        for (int bx = blocks_wide; bx > 0; bx--) {
            if (skip_blocks) {
                block_ptr += 4;
                skip_blocks--;
                total_blocks--;
                continue;
            }
            int pp = block_ptr;
            CHECK(2);
            uint8_t a = buf[sp++], b = buf[sp++];

            if (a == 0 && b == 0 && total_blocks == 0) {
                return LEGACY_OK;
            } else if ((b & 0xFC) == 0x84) {
                skip_blocks = ((b - 0x84) << 8) + a - 1;
            } else if (b < 0x80) {
                uint16_t flags = (uint16_t)((b << 8) | a);
                CHECK(2);
                uint8_t colors[2] = { buf[sp], buf[sp + 1] };
                sp += 2;
                for (int y = 0; y < 4; y++) {
                    for (int x = 0; x < 4; x++, flags >>= 1)
                        c->frame[pp++] = colors[M1_SEL(flags)];
                    pp -= row_dec;
                }
            } else if (b >= 0x90) {
                uint16_t flags = (uint16_t)((b << 8) | a);
                CHECK(8);
                uint8_t colors[8];
                memcpy(colors, buf + sp, 8);
                sp += 8;
                for (int y = 0; y < 4; y++) {
                    for (int x = 0; x < 4; x++, flags >>= 1)
                        c->frame[pp++] = colors[((y & 2) << 1) + (x & 2) + (M1_SEL(flags))];
                    pp -= row_dec;
                }
            } else {
                uint8_t color = a;
                for (int y = 0; y < 4; y++) {
                    for (int x = 0; x < 4; x++) c->frame[pp++] = color;
                    pp -= row_dec;
                }
            }
            block_ptr += 4;
            total_blocks--;
        }
    }
    return LEGACY_OK;
}

static int decode_16bit(legacy_msvideo1_ctx *c, const uint8_t *buf, int size)
{
    int blocks_wide = c->width / 4, blocks_high = c->height / 4;
    int total_blocks = blocks_wide * blocks_high;
    int stride = c->width; /* in pixels; frame is uint16_t-addressed below */
    uint16_t *pixels = (uint16_t *)c->frame;
    int row_dec = stride + 4;
    int sp = 0, skip_blocks = 0;

    for (int by = blocks_high; by > 0; by--) {
        int block_ptr = ((by * 4) - 1) * stride;
        for (int bx = blocks_wide; bx > 0; bx--) {
            if (skip_blocks) {
                block_ptr += 4;
                skip_blocks--;
                total_blocks--;
                continue;
            }
            int pp = block_ptr;
            CHECK(2);
            uint8_t a = buf[sp++], b = buf[sp++];

            if (a == 0 && b == 0 && total_blocks == 0) {
                return LEGACY_OK;
            } else if ((b & 0xFC) == 0x84) {
                skip_blocks = ((b - 0x84) << 8) + a - 1;
            } else if (b < 0x80) {
                uint16_t flags = (uint16_t)((b << 8) | a);
                CHECK(4);
                uint16_t c0 = (uint16_t)(buf[sp] | (buf[sp + 1] << 8)); sp += 2;
                uint16_t c1 = (uint16_t)(buf[sp] | (buf[sp + 1] << 8)); sp += 2;
                if (c0 & 0x8000) {
                    uint16_t colors[8]; colors[0] = c0; colors[1] = c1;
                    CHECK(12);
                    for (int k = 2; k < 8; k++) {
                        colors[k] = (uint16_t)(buf[sp] | (buf[sp + 1] << 8));
                        sp += 2;
                    }
                    for (int y = 0; y < 4; y++) {
                        for (int x = 0; x < 4; x++, flags >>= 1)
                            pixels[pp++] = colors[((y & 2) << 1) + (x & 2) + (M1_SEL(flags))];
                        pp -= row_dec;
                    }
                } else {
                    uint16_t colors[2] = { c0, c1 };
                    for (int y = 0; y < 4; y++) {
                        for (int x = 0; x < 4; x++, flags >>= 1)
                            pixels[pp++] = colors[M1_SEL(flags)];
                        pp -= row_dec;
                    }
                }
            } else {
                uint16_t color = (uint16_t)((b << 8) | a);
                for (int y = 0; y < 4; y++) {
                    for (int x = 0; x < 4; x++) pixels[pp++] = color;
                    pp -= row_dec;
                }
            }
            block_ptr += 4;
            total_blocks--;
        }
    }
    return LEGACY_OK;
}

int legacy_msvideo1_decode(legacy_msvideo1_ctx *c, const uint8_t *data, int size)
{
    if (size < 0) return LEGACY_ERR_CORRUPT;
    return c->mode_8bit ? decode_8bit(c, data, size) : decode_16bit(c, data, size);
}
#undef CHECK
