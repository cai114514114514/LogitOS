#ifndef LOGIT_FFMPEG_AACSBR_GET_BITS_H
#define LOGIT_FFMPEG_AACSBR_GET_BITS_H

#include <stdint.h>
#include <stdlib.h>

typedef struct GetBitContext {
    const uint8_t *buffer;
    int size_in_bits;
    int index;
    int error;
} GetBitContext;

static inline int init_get_bits(GetBitContext *gb, const uint8_t *p, int bits)
{
    if (!gb || !p || bits < 0) return -1;
    gb->buffer = p;
    gb->size_in_bits = bits;
    gb->index = 0;
    gb->error = 0;
    return 0;
}

static inline unsigned get_bits1(GetBitContext *gb)
{
    if (gb->index >= gb->size_in_bits) { gb->error = 1; return 0; }
    unsigned v = (gb->buffer[gb->index >> 3] >> (7 - (gb->index & 7))) & 1u;
    gb->index++;
    return v;
}

static inline unsigned get_bits(GetBitContext *gb, int n)
{
    unsigned v = 0;
    if (n < 0 || n > 32) { gb->error = 1; return 0; }
    while (n--) v = (v << 1) | get_bits1(gb);
    return v;
}

static inline unsigned show_bits(GetBitContext *gb, int n)
{
    GetBitContext t = *gb;
    return get_bits(&t, n);
}

static inline void skip_bits_long(GetBitContext *gb, int n)
{
    if (n < 0 || gb->index > gb->size_in_bits - n) {
        gb->error = 1;
        gb->index = gb->size_in_bits;
    } else {
        gb->index += n;
    }
}

#define skip_bits(gb, n) skip_bits_long((gb), (n))
static inline int get_bits_count(const GetBitContext *gb) { return gb->index; }
static inline int get_bits_left(const GetBitContext *gb) { return gb->size_in_bits - gb->index; }

typedef int32_t VLC_TYPE;
typedef struct VLC {
    VLC_TYPE (*table)[2];
} VLC;

/* FFmpeg normally expands these canonical codes into a multi-level lookup
 * table.  SBR consumes only a few dozen symbols per AAC frame, so this port
 * stores the canonical (code,length) pairs directly.  It removes several KiB
 * of table-builder machinery while preserving the exact bit decisions; the
 * decoder can replace this with a first-level table later if profiling says
 * it matters. */
static inline void logit_init_vlc(VLC *v, int nb_codes,
                                  const void *bits_, int bits_wrap,
                                  const void *codes_, int codes_wrap,
                                  int elem_size)
{
    const uint8_t *bits = (const uint8_t *)bits_;
    const uint8_t *codes = (const uint8_t *)codes_;
    v->table = (VLC_TYPE (*)[2])malloc((size_t)(nb_codes + 1) * sizeof(*v->table));
    if (!v->table) return;
    v->table[0][0] = nb_codes;
    v->table[0][1] = 0;
    for (int i = 0; i < nb_codes; i++) {
        uint32_t code = 0;
        const uint8_t *cp = codes + (size_t)i * codes_wrap;
        if (elem_size == 1) {
            code = cp[0];
        } else if (elem_size == 2) {
            code = (uint32_t)cp[0] | ((uint32_t)cp[1] << 8);
        } else {
            code = (uint32_t)cp[0] | ((uint32_t)cp[1] << 8) |
                   ((uint32_t)cp[2] << 16) | ((uint32_t)cp[3] << 24);
        }
        v->table[i + 1][0] = (VLC_TYPE)code;
        v->table[i + 1][1] = bits[(size_t)i * bits_wrap];
    }
}

#define INIT_VLC_STATIC(v, root, nb, bits, bw, bs, codes, cw, cs, size) \
    do { (void)(root); (void)(bs); (void)(size); \
         logit_init_vlc((v), (nb), (bits), (bw), (codes), (cw), (cs)); } while (0)

static inline int get_vlc2(GetBitContext *gb, VLC_TYPE (*table)[2],
                           int root_bits, int max_depth)
{
    (void)root_bits; (void)max_depth;
    if (!table) { gb->error = 1; return 0; }
    int n = table[0][0];
    uint32_t code = 0;
    for (int len = 1; len <= 24; len++) {
        code = (code << 1) | get_bits1(gb);
        for (int i = 0; i < n; i++)
            if (table[i + 1][1] == len && (uint32_t)table[i + 1][0] == code)
                return i;
        if (gb->error) break;
    }
    gb->error = 1;
    return 0;
}

#endif
