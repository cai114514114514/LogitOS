/* c/lib/video/legacy_rpza.c -- Apple RPZA ("QuickTime video") decoder.
 *
 * Ported from ffmpeg's libavcodec/rpza.c (LGPL 2.1+, Roberto Togni),
 * algorithm-for-algorithm: the opcode dispatch (skip / fill-1-color /
 * fill-4-color / fill-16-color), the two-tap 11/21 chroma-style blend that
 * derives the two intermediate colors of the 4-color mode, and the
 * fall-through "MSbit clear on the opcode byte" 1-color special case are
 * all copied, not reconstructed from a description.
 *
 * RPZA is fixed at 15-bit RGB555 -- there is no bit-depth axis to this
 * format the way there is for MS Video 1 or QTRLE, so the gate in
 * tests/legacy.mk covers it at several sizes and frame counts instead.
 *
 * INTEGRATION LINES (not written here; see the phase report):
 *   demux.c sniff:  fourcc 'rpza' inside a MOV 'stsd' video sample entry
 *   media.h ops:    { "rpza", legacy_rpza_open, legacy_rpza_decode }
 *   canPlayType:    video/quicktime;codecs=rpza
 */
#include <string.h>
#include <stdlib.h>
#include "legacy.h"

int legacy_rpza_open(legacy_rpza_ctx *c, int width, int height)
{
    if (width < 4 || height < 4)
        return LEGACY_ERR_UNSUPPORTED;
    memset(c, 0, sizeof(*c));
    c->width = width;
    c->height = height;
    c->frame = (uint16_t *)calloc((size_t)width * sizeof(uint16_t), (size_t)height);
    if (!c->frame) return LEGACY_ERR_OOM;
    return LEGACY_OK;
}

void legacy_rpza_close(legacy_rpza_ctx *c)
{
    free(c->frame);
    c->frame = NULL;
}

int legacy_rpza_decode(legacy_rpza_ctx *c, const uint8_t *data, int size)
{
    int width = c->width;
    int sp = 0;
    int row_ptr = 0, pixel_ptr = 0;
    int total_blocks = ((width + 3) / 4) * ((c->height + 3) / 4);
    int row_inc = width - 4;
    uint16_t colorA = 0;

#define LEFT() (size - sp)
#define NEED(n) do { if (LEFT() < (n)) return LEGACY_ERR_CORRUPT; } while (0)
#define GET8() (data[sp++])
#define PEEK8() (sp < size ? data[sp] : 0)
#define GET16BE() (sp += 2, (uint16_t)((data[sp - 2] << 8) | data[sp - 1]))

    NEED(4);
    /* First byte is conventionally 0xe1; ffmpeg only warns, never refuses,
     * so this decoder does not refuse either -- a fixture with a different
     * leading byte is still valid CVID-adjacent RPZA in the wild. */
    sp += 1;
    uint32_t chunk_size = ((uint32_t)data[sp] << 16) | ((uint32_t)data[sp + 1] << 8) | data[sp + 2];
    sp += 3;
    (void)chunk_size; /* container-level cross-check only; not load-bearing here */

    if (total_blocks / 32 > LEFT())
        return LEGACY_ERR_CORRUPT;

    while (LEFT() > 0) {
        NEED(1);
        uint8_t opcode = GET8();
        int n_blocks = (opcode & 0x1f) + 1;

        if (!(opcode & 0x80)) {
            NEED(1);
            colorA = (uint16_t)((opcode << 8) | GET8());
            opcode = 0;
            if (LEFT() > 0 && (PEEK8() & 0x80) != 0) {
                opcode = 0x20;
                n_blocks = 1;
            }
        }

        if (n_blocks > total_blocks) n_blocks = total_blocks;

        switch (opcode & 0xe0) {
        case 0x80: /* skip blocks */
            while (n_blocks--) {
                if (total_blocks < 1) return LEGACY_ERR_CORRUPT;
                pixel_ptr += 4;
                if (pixel_ptr >= width) { pixel_ptr = 0; row_ptr += width * 4; }
                total_blocks--;
            }
            break;

        case 0xa0: { /* fill with one color */
            NEED(2);
            colorA = GET16BE();
            while (n_blocks--) {
                if (total_blocks < 1) return LEGACY_ERR_CORRUPT;
                int bp = row_ptr + pixel_ptr;
                for (int y = 0; y < 4; y++) {
                    for (int x = 0; x < 4; x++) c->frame[bp++] = colorA;
                    bp += row_inc;
                }
                pixel_ptr += 4;
                if (pixel_ptr >= width) { pixel_ptr = 0; row_ptr += width * 4; }
                total_blocks--;
            }
            break;
        }

        case 0xc0: {
            NEED(2);
            colorA = GET16BE();
            /* fallthrough into the 4-color body below */
        }
        /* fallthrough */
        case 0x20: {
            NEED(2);
            uint16_t colorB = GET16BE();
            uint16_t color4[4];
            color4[0] = colorB;
            color4[3] = colorA;
            int ta, tb;
            /* The negative control: swap which tap gets weight 11 and which
             * gets 21 between the two intermediate colors. ffmpeg's own
             * rpza.c uses exactly (11*ta+21*tb) for c1 and (21*ta+11*tb) for
             * c2 -- two-tap blends with the weights swapped between outputs
             * is a real, easy-to-make transcription error in this exact
             * shape of code (four nearly-identical lines differing only in
             * which constant comes first), and it is wrong on every 4-color
             * block's two derived colors while leaving colorA/colorB (index
             * 0 and 3) untouched. */
#ifdef RPZA_CONTROL_SWAPPED_BLEND
            ta = (colorA >> 10) & 0x1f; tb = (colorB >> 10) & 0x1f;
            uint16_t c1 = (uint16_t)(((21 * ta + 11 * tb) >> 5) << 10);
            uint16_t c2 = (uint16_t)(((11 * ta + 21 * tb) >> 5) << 10);
            ta = (colorA >> 5) & 0x1f; tb = (colorB >> 5) & 0x1f;
            c1 = (uint16_t)(c1 | (((21 * ta + 11 * tb) >> 5) << 5));
            c2 = (uint16_t)(c2 | (((11 * ta + 21 * tb) >> 5) << 5));
            ta = colorA & 0x1f; tb = colorB & 0x1f;
            c1 = (uint16_t)(c1 | ((21 * ta + 11 * tb) >> 5));
            c2 = (uint16_t)(c2 | ((11 * ta + 21 * tb) >> 5));
#else
            ta = (colorA >> 10) & 0x1f; tb = (colorB >> 10) & 0x1f;
            uint16_t c1 = (uint16_t)(((11 * ta + 21 * tb) >> 5) << 10);
            uint16_t c2 = (uint16_t)(((21 * ta + 11 * tb) >> 5) << 10);
            ta = (colorA >> 5) & 0x1f; tb = (colorB >> 5) & 0x1f;
            c1 = (uint16_t)(c1 | (((11 * ta + 21 * tb) >> 5) << 5));
            c2 = (uint16_t)(c2 | (((21 * ta + 11 * tb) >> 5) << 5));
            ta = colorA & 0x1f; tb = colorB & 0x1f;
            c1 = (uint16_t)(c1 | ((11 * ta + 21 * tb) >> 5));
            c2 = (uint16_t)(c2 | ((21 * ta + 11 * tb) >> 5));
#endif
            color4[1] = c1; color4[2] = c2;

            if (LEFT() < n_blocks * 4) return LEGACY_ERR_CORRUPT;
            while (n_blocks--) {
                if (total_blocks < 1) return LEGACY_ERR_CORRUPT;
                int bp = row_ptr + pixel_ptr;
                for (int y = 0; y < 4; y++) {
                    uint8_t idxb = GET8();
                    for (int x = 0; x < 4; x++) {
                        int idx = (idxb >> (2 * (3 - x))) & 0x03;
                        c->frame[bp++] = color4[idx];
                    }
                    bp += row_inc;
                }
                pixel_ptr += 4;
                if (pixel_ptr >= width) { pixel_ptr = 0; row_ptr += width * 4; }
                total_blocks--;
            }
            break;
        }

        case 0x00: { /* 16 colors, one per pixel */
            NEED(30);
            if (total_blocks < 1) return LEGACY_ERR_CORRUPT;
            int bp = row_ptr + pixel_ptr;
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    if (y != 0 || x != 0) colorA = GET16BE();
                    c->frame[bp++] = colorA;
                }
                bp += row_inc;
            }
            pixel_ptr += 4;
            if (pixel_ptr >= width) { pixel_ptr = 0; row_ptr += width * 4; }
            total_blocks--;
            break;
        }

        default:
            return LEGACY_ERR_CORRUPT; /* unknown opcode */
        }
    }

#undef NEED
#undef LEFT
#undef GET8
#undef PEEK8
#undef GET16BE
    return LEGACY_OK;
}
