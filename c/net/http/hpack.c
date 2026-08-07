/* hpack.c -- HPACK (RFC 7541) for the HTTP/2 client. See hpack.h for the
 * eviction discipline and why this module is the one with the security bugs.
 *
 * Three structural choices are worth stating here rather than in the header:
 *
 * THE HUFFMAN DECODER IS CANONICAL, NOT A TRIE.  RFC 7541 Appendix B is a
 * complete canonical code -- sorted by code value the lengths are
 * non-decreasing and codes of equal length are consecutive (both checked by
 * the unit test, along with Kraft equality summing to exactly 1).  That lets
 * the decoder carry one 257-entry symbol order plus a per-length first-code
 * table instead of a 513-node pointer trie: no allocation, no pointer chasing,
 * and the table is verifiable by construction from the same array the encoder
 * uses, so the two halves cannot drift apart.
 *
 * NOTHING TRUSTS A LENGTH.  Every string length is checked against the bytes
 * actually present before a byte is copied, and the Huffman decoder grows its
 * output as it emits rather than reserving from the encoded size -- a Huffman
 * string expands, so "allocate the wire length" is a 60% heap overflow on
 * chosen input.  Decoded strings and the whole header list are capped
 * (HPACK_STR_MAX / HPACK_LIST_MAX), because a peer can otherwise ask us to
 * materialise gigabytes from a few kilobytes of frames -- that is the HPACK
 * bomb, and the cap is the only defence against it.
 *
 * THE TABLE IS A FIXED RING.  Capacity is bounded (HPACK_CAP_MAX) and each
 * entry costs at least 32 bytes by the RFC's own metric, so the maximum entry
 * count is a compile-time constant and the spine never allocates.  Only the
 * strings do.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "hpack.h"

const char *hpack_strerror(int err)
{
    switch (err) {
    case HPACK_OK:       return "ok";
    case HPACK_E_TRUNC:  return "truncated header block";
    case HPACK_E_INDEX:  return "bad table index";
    case HPACK_E_HUFF:   return "bad huffman coding";
    case HPACK_E_INT:    return "integer overflow";
    case HPACK_E_SIZE:   return "header too large";
    case HPACK_E_NOMEM:  return "out of memory";
    case HPACK_E_UPDATE: return "bad dynamic table size update";
    case HPACK_E_ARG:    return "bad argument";
    case HPACK_E_NUL:    return "embedded NUL in a header name or value";
    default:             return "hpack error";
    }
}

/* ------------------------------------------------------------ static table */

/* RFC 7541 Appendix A, indices 1..61. Entry 0 is a placeholder: index 0 is
 * not a valid HPACK index and decoding one is an error, not an off-by-one. */
static const struct hpack_entry g_static[HPACK_STATIC_N + 1] = {
    { NULL, NULL, 0, 0 },
    { (char *)":authority",                  (char *)"",              10,  0 },
    { (char *)":method",                     (char *)"GET",            7,  3 },
    { (char *)":method",                     (char *)"POST",           7,  4 },
    { (char *)":path",                       (char *)"/",              5,  1 },
    { (char *)":path",                       (char *)"/index.html",    5, 11 },
    { (char *)":scheme",                     (char *)"http",           7,  4 },
    { (char *)":scheme",                     (char *)"https",          7,  5 },
    { (char *)":status",                     (char *)"200",            7,  3 },
    { (char *)":status",                     (char *)"204",            7,  3 },
    { (char *)":status",                     (char *)"206",            7,  3 },
    { (char *)":status",                     (char *)"304",            7,  3 },
    { (char *)":status",                     (char *)"400",            7,  3 },
    { (char *)":status",                     (char *)"404",            7,  3 },
    { (char *)":status",                     (char *)"500",            7,  3 },
    { (char *)"accept-charset",              (char *)"",              14,  0 },
    { (char *)"accept-encoding",             (char *)"gzip, deflate", 15, 13 },
    { (char *)"accept-language",             (char *)"",              15,  0 },
    { (char *)"accept-ranges",               (char *)"",              13,  0 },
    { (char *)"accept",                      (char *)"",               6,  0 },
    { (char *)"access-control-allow-origin", (char *)"",              27,  0 },
    { (char *)"age",                         (char *)"",               3,  0 },
    { (char *)"allow",                       (char *)"",               5,  0 },
    { (char *)"authorization",               (char *)"",              13,  0 },
    { (char *)"cache-control",               (char *)"",              13,  0 },
    { (char *)"content-disposition",         (char *)"",              19,  0 },
    { (char *)"content-encoding",            (char *)"",              16,  0 },
    { (char *)"content-language",            (char *)"",              16,  0 },
    { (char *)"content-length",              (char *)"",              14,  0 },
    { (char *)"content-location",            (char *)"",              16,  0 },
    { (char *)"content-range",               (char *)"",              13,  0 },
    { (char *)"content-type",                (char *)"",              12,  0 },
    { (char *)"cookie",                      (char *)"",               6,  0 },
    { (char *)"date",                        (char *)"",               4,  0 },
    { (char *)"etag",                        (char *)"",               4,  0 },
    { (char *)"expect",                      (char *)"",               6,  0 },
    { (char *)"expires",                     (char *)"",               7,  0 },
    { (char *)"from",                        (char *)"",               4,  0 },
    { (char *)"host",                        (char *)"",               4,  0 },
    { (char *)"if-match",                    (char *)"",               8,  0 },
    { (char *)"if-modified-since",           (char *)"",              17,  0 },
    { (char *)"if-none-match",               (char *)"",              13,  0 },
    { (char *)"if-range",                    (char *)"",               8,  0 },
    { (char *)"if-unmodified-since",         (char *)"",              19,  0 },
    { (char *)"last-modified",               (char *)"",              13,  0 },
    { (char *)"link",                        (char *)"",               4,  0 },
    { (char *)"location",                    (char *)"",               8,  0 },
    { (char *)"max-forwards",                (char *)"",              12,  0 },
    { (char *)"proxy-authenticate",          (char *)"",              18,  0 },
    { (char *)"proxy-authorization",         (char *)"",              19,  0 },
    { (char *)"range",                       (char *)"",               5,  0 },
    { (char *)"referer",                     (char *)"",               7,  0 },
    { (char *)"refresh",                     (char *)"",               7,  0 },
    { (char *)"retry-after",                 (char *)"",              11,  0 },
    { (char *)"server",                      (char *)"",               6,  0 },
    { (char *)"set-cookie",                  (char *)"",              10,  0 },
    { (char *)"strict-transport-security",   (char *)"",              25,  0 },
    { (char *)"transfer-encoding",           (char *)"",              17,  0 },
    { (char *)"user-agent",                  (char *)"",              10,  0 },
    { (char *)"vary",                        (char *)"",               4,  0 },
    { (char *)"via",                         (char *)"",               3,  0 },
    { (char *)"www-authenticate",            (char *)"",              16,  0 }
};

const struct hpack_entry *hpack_static(int idx)
{
    if (idx < 1 || idx > HPACK_STATIC_N) return NULL;
    return &g_static[idx];
}

/* ---------------------------------------------------------------- huffman */

/* RFC 7541 Appendix B, transcribed from the RFC text and validated three ways
 * by the unit test: every code fits its declared length, the Kraft sum is
 * exactly 1 (the code is complete), and the Appendix C strings encode to the
 * published bytes. Index 256 is EOS, which is never emitted and is a decode
 * error if it appears in a stream. */
static const struct { uint32_t code; uint8_t bits; } g_huff[257] = {
    {0x00001ff8,13}, {0x007fffd8,23}, {0x0fffffe2,28}, {0x0fffffe3,28},
    {0x0fffffe4,28}, {0x0fffffe5,28}, {0x0fffffe6,28}, {0x0fffffe7,28},
    {0x0fffffe8,28}, {0x00ffffea,24}, {0x3ffffffc,30}, {0x0fffffe9,28},
    {0x0fffffea,28}, {0x3ffffffd,30}, {0x0fffffeb,28}, {0x0fffffec,28},
    {0x0fffffed,28}, {0x0fffffee,28}, {0x0fffffef,28}, {0x0ffffff0,28},
    {0x0ffffff1,28}, {0x0ffffff2,28}, {0x3ffffffe,30}, {0x0ffffff3,28},
    {0x0ffffff4,28}, {0x0ffffff5,28}, {0x0ffffff6,28}, {0x0ffffff7,28},
    {0x0ffffff8,28}, {0x0ffffff9,28}, {0x0ffffffa,28}, {0x0ffffffb,28},
    {0x00000014, 6}, {0x000003f8,10}, {0x000003f9,10}, {0x00000ffa,12},
    {0x00001ff9,13}, {0x00000015, 6}, {0x000000f8, 8}, {0x000007fa,11},
    {0x000003fa,10}, {0x000003fb,10}, {0x000000f9, 8}, {0x000007fb,11},
    {0x000000fa, 8}, {0x00000016, 6}, {0x00000017, 6}, {0x00000018, 6},
    {0x00000000, 5}, {0x00000001, 5}, {0x00000002, 5}, {0x00000019, 6},
    {0x0000001a, 6}, {0x0000001b, 6}, {0x0000001c, 6}, {0x0000001d, 6},
    {0x0000001e, 6}, {0x0000001f, 6}, {0x0000005c, 7}, {0x000000fb, 8},
    {0x00007ffc,15}, {0x00000020, 6}, {0x00000ffb,12}, {0x000003fc,10},
    {0x00001ffa,13}, {0x00000021, 6}, {0x0000005d, 7}, {0x0000005e, 7},
    {0x0000005f, 7}, {0x00000060, 7}, {0x00000061, 7}, {0x00000062, 7},
    {0x00000063, 7}, {0x00000064, 7}, {0x00000065, 7}, {0x00000066, 7},
    {0x00000067, 7}, {0x00000068, 7}, {0x00000069, 7}, {0x0000006a, 7},
    {0x0000006b, 7}, {0x0000006c, 7}, {0x0000006d, 7}, {0x0000006e, 7},
    {0x0000006f, 7}, {0x00000070, 7}, {0x00000071, 7}, {0x00000072, 7},
    {0x000000fc, 8}, {0x00000073, 7}, {0x000000fd, 8}, {0x00001ffb,13},
    {0x0007fff0,19}, {0x00001ffc,13}, {0x00003ffc,14}, {0x00000022, 6},
    {0x00007ffd,15}, {0x00000003, 5}, {0x00000023, 6}, {0x00000004, 5},
    {0x00000024, 6}, {0x00000005, 5}, {0x00000025, 6}, {0x00000026, 6},
    {0x00000027, 6}, {0x00000006, 5}, {0x00000074, 7}, {0x00000075, 7},
    {0x00000028, 6}, {0x00000029, 6}, {0x0000002a, 6}, {0x00000007, 5},
    {0x0000002b, 6}, {0x00000076, 7}, {0x0000002c, 6}, {0x00000008, 5},
    {0x00000009, 5}, {0x0000002d, 6}, {0x00000077, 7}, {0x00000078, 7},
    {0x00000079, 7}, {0x0000007a, 7}, {0x0000007b, 7}, {0x00007ffe,15},
    {0x000007fc,11}, {0x00003ffd,14}, {0x00001ffd,13}, {0x0ffffffc,28},
    {0x000fffe6,20}, {0x003fffd2,22}, {0x000fffe7,20}, {0x000fffe8,20},
    {0x003fffd3,22}, {0x003fffd4,22}, {0x003fffd5,22}, {0x007fffd9,23},
    {0x003fffd6,22}, {0x007fffda,23}, {0x007fffdb,23}, {0x007fffdc,23},
    {0x007fffdd,23}, {0x007fffde,23}, {0x00ffffeb,24}, {0x007fffdf,23},
    {0x00ffffec,24}, {0x00ffffed,24}, {0x003fffd7,22}, {0x007fffe0,23},
    {0x00ffffee,24}, {0x007fffe1,23}, {0x007fffe2,23}, {0x007fffe3,23},
    {0x007fffe4,23}, {0x001fffdc,21}, {0x003fffd8,22}, {0x007fffe5,23},
    {0x003fffd9,22}, {0x007fffe6,23}, {0x007fffe7,23}, {0x00ffffef,24},
    {0x003fffda,22}, {0x001fffdd,21}, {0x000fffe9,20}, {0x003fffdb,22},
    {0x003fffdc,22}, {0x007fffe8,23}, {0x007fffe9,23}, {0x001fffde,21},
    {0x007fffea,23}, {0x003fffdd,22}, {0x003fffde,22}, {0x00fffff0,24},
    {0x001fffdf,21}, {0x003fffdf,22}, {0x007fffeb,23}, {0x007fffec,23},
    {0x001fffe0,21}, {0x001fffe1,21}, {0x003fffe0,22}, {0x001fffe2,21},
    {0x007fffed,23}, {0x003fffe1,22}, {0x007fffee,23}, {0x007fffef,23},
    {0x000fffea,20}, {0x003fffe2,22}, {0x003fffe3,22}, {0x003fffe4,22},
    {0x007ffff0,23}, {0x003fffe5,22}, {0x003fffe6,22}, {0x007ffff1,23},
    {0x03ffffe0,26}, {0x03ffffe1,26}, {0x000fffeb,20}, {0x0007fff1,19},
    {0x003fffe7,22}, {0x007ffff2,23}, {0x003fffe8,22}, {0x01ffffec,25},
    {0x03ffffe2,26}, {0x03ffffe3,26}, {0x03ffffe4,26}, {0x07ffffde,27},
    {0x07ffffdf,27}, {0x03ffffe5,26}, {0x00fffff1,24}, {0x01ffffed,25},
    {0x0007fff2,19}, {0x001fffe3,21}, {0x03ffffe6,26}, {0x07ffffe0,27},
    {0x07ffffe1,27}, {0x03ffffe7,26}, {0x07ffffe2,27}, {0x00fffff2,24},
    {0x001fffe4,21}, {0x001fffe5,21}, {0x03ffffe8,26}, {0x03ffffe9,26},
    {0x0ffffffd,28}, {0x07ffffe3,27}, {0x07ffffe4,27}, {0x07ffffe5,27},
    {0x000fffec,20}, {0x00fffff3,24}, {0x000fffed,20}, {0x001fffe6,21},
    {0x003fffe9,22}, {0x001fffe7,21}, {0x001fffe8,21}, {0x007ffff3,23},
    {0x003fffea,22}, {0x003fffeb,22}, {0x01ffffee,25}, {0x01ffffef,25},
    {0x00fffff4,24}, {0x00fffff5,24}, {0x03ffffea,26}, {0x007ffff4,23},
    {0x03ffffeb,26}, {0x07ffffe6,27}, {0x03ffffec,26}, {0x03ffffed,26},
    {0x07ffffe7,27}, {0x07ffffe8,27}, {0x07ffffe9,27}, {0x07ffffea,27},
    {0x07ffffeb,27}, {0x0ffffffe,28}, {0x07ffffec,27}, {0x07ffffed,27},
    {0x07ffffee,27}, {0x07ffffef,27}, {0x07fffff0,27}, {0x03ffffee,26},
    {0x3fffffff,30}
};

#define HUFF_MAXBITS 30

/* Canonical decoding tables, built once from g_huff. `order` lists the symbols
 * sorted by code value; first_code[l] / first_idx[l] / cnt[l] describe the run
 * of length-l codes, which is contiguous because the code is canonical. */
static uint16_t huff_order[257];
static uint32_t huff_first_code[HUFF_MAXBITS + 1];
static uint16_t huff_first_idx[HUFF_MAXBITS + 1];
static uint16_t huff_cnt[HUFF_MAXBITS + 1];
static int      huff_ready;

static void huff_init(void)
{
    if (huff_ready) return;
    for (int l = 0; l <= HUFF_MAXBITS; l++) { huff_cnt[l] = 0; huff_first_code[l] = 0; huff_first_idx[l] = 0; }
    for (int i = 0; i < 257; i++) huff_cnt[g_huff[i].bits]++;
    /* Canonical: the first code of length l is (first(l-1) + cnt(l-1)) << 1. */
    uint32_t code = 0;
    int idx = 0;
    for (int l = 1; l <= HUFF_MAXBITS; l++) {
        huff_first_code[l] = code;
        huff_first_idx[l]  = (uint16_t)idx;
        idx  += huff_cnt[l];
        code  = (code + huff_cnt[l]) << 1;
    }
    /* Place each symbol at its offset within its length's run. That the offset
     * is unique and in range is the canonical property, and the unit test
     * checks it directly rather than trusting it. */
    for (int i = 0; i < 257; i++) {
        int l = g_huff[i].bits;
        uint32_t off = g_huff[i].code - huff_first_code[l];
        huff_order[huff_first_idx[l] + off] = (uint16_t)i;
    }
    huff_ready = 1;
}

int hpack_huff_len(const char *s, int n)
{
    int bits = 0;
    for (int i = 0; i < n; i++) bits += g_huff[(unsigned char)s[i]].bits;
    return (bits + 7) / 8;
}

int hpack_huff_encode(uint8_t *out, int max, const char *s, int n)
{
    uint64_t acc = 0;
    int nbits = 0, o = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        acc = (acc << g_huff[c].bits) | g_huff[c].code;
        nbits += g_huff[c].bits;
        while (nbits >= 8) {
            if (o >= max) return -1;
            out[o++] = (uint8_t)((acc >> (nbits - 8)) & 0xFF);
            nbits -= 8;
        }
    }
    if (nbits > 0) {
        /* Pad with the EOS prefix, i.e. ones (RFC 7541 5.2). */
        if (o >= max) return -1;
        out[o++] = (uint8_t)(((acc << (8 - nbits)) | ((1u << (8 - nbits)) - 1)) & 0xFF);
    }
    return o;
}

/* Decode `len` encoded bytes into a fresh NUL-terminated buffer. `cap` bounds
 * the DECODED size: the expansion ratio is 8/5, so the encoded length is not a
 * safe allocation size and is never used as one. */
int hpack_huff_decode(const uint8_t *in, int len, char **out, int *outlen, int cap)
{
    huff_init();
    if (!out || !outlen || len < 0) return HPACK_E_ARG;
    *out = NULL; *outlen = 0;

    if (cap < 1) return HPACK_E_ARG;
    int ocap = len + 16;                    /* a starting guess, grown as needed */
    if (ocap > cap) ocap = cap;
    char *buf = (char *)malloc((size_t)ocap + 1);
    if (!buf) return HPACK_E_NOMEM;
    int olen = 0;

    uint32_t code = 0;
    int nbits = 0;
    for (int i = 0; i < len; i++) {
        for (int b = 7; b >= 0; b--) {
            code = (code << 1) | ((in[i] >> b) & 1);
            nbits++;
            if (nbits > HUFF_MAXBITS) { free(buf); return HPACK_E_HUFF; }
            if (!huff_cnt[nbits]) continue;
            uint32_t first = huff_first_code[nbits];
            if (code < first || code - first >= huff_cnt[nbits]) continue;
            int sym = huff_order[huff_first_idx[nbits] + (code - first)];
            /* EOS inside the stream is a decoding error (RFC 7541 5.2): it is
             * only ever a padding prefix, never a symbol. */
            if (sym == 256) { free(buf); return HPACK_E_HUFF; }
            if (olen >= cap) { free(buf); return HPACK_E_SIZE; }
            if (olen + 1 > ocap) {
                int ncap = ocap * 2;
                if (ncap > cap) ncap = cap;
                if (ncap < olen + 1) ncap = olen + 1;
                char *nb = (char *)realloc(buf, (size_t)ncap + 1);
                if (!nb) { free(buf); return HPACK_E_NOMEM; }
                buf = nb; ocap = ncap;
            }
            buf[olen++] = (char)sym;
            code = 0; nbits = 0;
        }
    }
    /* The tail must be a strict prefix of EOS, at most 7 bits, all ones. A
     * longer or non-all-ones remainder means the encoder lied or the string
     * was truncated -- both are decoding errors, and accepting them is how a
     * decoder silently drops the last character of a header value. */
    if (nbits >= 8) { free(buf); return HPACK_E_HUFF; }
    if (nbits > 0 && code != ((1u << nbits) - 1)) { free(buf); return HPACK_E_HUFF; }

    buf[olen] = 0;
    *out = buf; *outlen = olen;
    return HPACK_OK;
}

/* --------------------------------------------------------------- integers */

int hpack_int_decode(const uint8_t *in, int len, int *off, int prefix, uint32_t *out)
{
    if (!in || !off || !out || prefix < 1 || prefix > 8) return HPACK_E_ARG;
    int i = *off;
    if (i < 0 || i >= len) return HPACK_E_TRUNC;
    uint32_t mask = (uint32_t)((1u << prefix) - 1);
    uint32_t v = in[i++] & mask;
    if (v < mask) { *off = i; *out = v; return HPACK_OK; }

    /* Continuation octets. The bound is not decorative: 5 octets of 7 bits is
     * already 35 bits, so anything longer cannot fit a uint32 and a decoder
     * without the check either loops on a padded encoding or wraps silently
     * and produces an attacker-chosen small length. */
    uint64_t acc = mask;
    int shift = 0;
    for (int k = 0; k < 6; k++) {
        if (i >= len) return HPACK_E_TRUNC;
        uint8_t b = in[i++];
        acc += (uint64_t)(b & 0x7F) << shift;
        if (acc > 0x7FFFFFFFULL) return HPACK_E_INT;
        if (!(b & 0x80)) { *off = i; *out = (uint32_t)acc; return HPACK_OK; }
        shift += 7;
    }
    return HPACK_E_INT;
}

int hpack_int_encode(uint8_t *out, int max, int prefix, uint8_t flags, uint32_t val)
{
    if (!out || prefix < 1 || prefix > 8) return HPACK_E_ARG;
    uint32_t mask = (uint32_t)((1u << prefix) - 1);
    int o = 0;
    if (val < mask) {
        if (o >= max) return HPACK_E_SIZE;
        out[o++] = (uint8_t)(flags | val);
        return o;
    }
    if (o >= max) return HPACK_E_SIZE;
    out[o++] = (uint8_t)(flags | mask);
    val -= mask;
    while (val >= 128) {
        if (o >= max) return HPACK_E_SIZE;
        out[o++] = (uint8_t)((val & 0x7F) | 0x80);
        val >>= 7;
    }
    if (o >= max) return HPACK_E_SIZE;
    out[o++] = (uint8_t)val;
    return o;
}

/* ------------------------------------------------------------ header list */

static char *dupn(const char *s, int n)
{
    char *p = (char *)malloc((size_t)n + 1);
    if (!p) return NULL;
    if (n > 0) memcpy(p, s, (size_t)n);
    p[n] = 0;
    return p;
}

void hpack_list_init(struct hpack_list *l)
{
    if (!l) return;
    l->v = NULL; l->n = 0; l->cap = 0; l->bytes = 0;
}

void hpack_list_free(struct hpack_list *l)
{
    if (!l) return;
    for (int i = 0; i < l->n; i++) { free(l->v[i].name); free(l->v[i].value); }
    free(l->v);
    hpack_list_init(l);
}

int hpack_list_add(struct hpack_list *l, const char *name, int nlen,
                   const char *value, int vlen, int sensitive)
{
    if (!l || !name || !value) return HPACK_E_ARG;
    if (nlen < 0) nlen = (int)strlen(name);
    if (vlen < 0) vlen = (int)strlen(value);
    if (nlen < 0 || vlen < 0) return HPACK_E_ARG;
    if (l->n >= HPACK_MAX_FIELDS) return HPACK_E_SIZE;
    if (l->n == l->cap) {
        int ncap = l->cap ? l->cap * 2 : 16;
        struct hpack_hdr *nv = (struct hpack_hdr *)realloc(l->v, (size_t)ncap * sizeof *nv);
        if (!nv) return HPACK_E_NOMEM;
        l->v = nv; l->cap = ncap;
    }
    char *n = dupn(name, nlen);
    char *v = dupn(value, vlen);
    if (!n || !v) { free(n); free(v); return HPACK_E_NOMEM; }
    l->v[l->n].name = n; l->v[l->n].nlen = nlen;
    l->v[l->n].value = v; l->v[l->n].vlen = vlen;
    l->v[l->n].sensitive = sensitive;
    l->n++;
    l->bytes += nlen + vlen + 32;
    return HPACK_OK;
}

const char *hpack_list_get(const struct hpack_list *l, const char *name)
{
    return hpack_list_nth(l, name, 0);
}

int hpack_list_count(const struct hpack_list *l, const char *name)
{
    int c = 0;
    if (!l || !name) return 0;
    for (int i = 0; i < l->n; i++) if (!strcmp(l->v[i].name, name)) c++;
    return c;
}

const char *hpack_list_nth(const struct hpack_list *l, const char *name, int idx)
{
    if (!l || !name) return NULL;
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->v[i].name, name) && idx-- == 0) return l->v[i].value;
    return NULL;
}

/* ---------------------------------------------------------- dynamic table */

void hpack_table_init(struct hpack_table *t, int cap_max)
{
    if (!t) return;
    memset(t, 0, sizeof *t);
    if (cap_max < 0) cap_max = 0;
    if (cap_max > HPACK_CAP_MAX) cap_max = HPACK_CAP_MAX;
    t->cap_max = cap_max;
    t->cap = cap_max < HPACK_DEFAULT_CAP ? cap_max : HPACK_DEFAULT_CAP;
}

void hpack_table_free(struct hpack_table *t)
{
    if (!t) return;
    for (int i = 0; i < t->count; i++) {
        int k = (t->head + i) % HPACK_MAX_ENTRIES;
        free(t->v[k].name); free(t->v[k].value);
        t->v[k].name = t->v[k].value = NULL;
    }
    t->count = 0; t->size = 0; t->head = 0;
}

int hpack_table_count(const struct hpack_table *t) { return t ? t->count : 0; }
int hpack_table_size(const struct hpack_table *t)  { return t ? t->size  : 0; }

static int entry_size(int nlen, int vlen) { return nlen + vlen + 32; }

/* Drop the OLDEST entry. The oldest sits `count-1` past the head. */
static void evict_oldest(struct hpack_table *t)
{
    if (t->count <= 0) return;
    int k = (t->head + t->count - 1) % HPACK_MAX_ENTRIES;
    t->size -= entry_size(t->v[k].nlen, t->v[k].vlen);
    free(t->v[k].name); free(t->v[k].value);
    t->v[k].name = t->v[k].value = NULL;
    t->count--;
    if (t->size < 0) t->size = 0;       /* unreachable; keeps the invariant local */
}

int hpack_table_resize(struct hpack_table *t, int cap)
{
    if (!t) return HPACK_E_ARG;
    if (cap < 0 || cap > t->cap_max) return HPACK_E_UPDATE;
    t->cap = cap;
    while (t->size > t->cap) evict_oldest(t);
    return HPACK_OK;
}

int hpack_table_add(struct hpack_table *t, const char *name, int nlen,
                    const char *value, int vlen)
{
    if (!t || !name || !value) return HPACK_E_ARG;
    if (nlen < 0) nlen = (int)strlen(name);
    if (vlen < 0) vlen = (int)strlen(value);
    int sz = entry_size(nlen, vlen);

    /* COPY FIRST. The strings we are inserting may live in an entry that the
     * eviction below is about to free -- a literal-with-indexing whose name
     * came from index 62 while the table is full does exactly that. Evicting
     * first and copying after is a use-after-free that only fires when the
     * table is near capacity, which is to say on long-lived connections. */
    char *n = dupn(name, nlen);
    char *v = dupn(value, vlen);
    if (!n || !v) { free(n); free(v); return HPACK_E_NOMEM; }

    while (t->count > 0 && t->size + sz > t->cap) evict_oldest(t);

    /* RFC 7541 4.4: an entry larger than the capacity empties the table and is
     * NOT inserted. This is legal, not an error -- the peer's table does the
     * same thing, so the two stay in step. */
    if (sz > t->cap || t->count >= HPACK_MAX_ENTRIES) {
        free(n); free(v);
        return HPACK_OK;
    }

    t->head = (t->head + HPACK_MAX_ENTRIES - 1) % HPACK_MAX_ENTRIES;
    t->v[t->head].name = n; t->v[t->head].nlen = nlen;
    t->v[t->head].value = v; t->v[t->head].vlen = vlen;
    t->count++;
    t->size += sz;
    return HPACK_OK;
}

const struct hpack_entry *hpack_lookup(const struct hpack_table *t, int idx)
{
    if (idx < 1) return NULL;
    if (idx <= HPACK_STATIC_N) return &g_static[idx];
    if (!t) return NULL;
    int d = idx - HPACK_STATIC_N - 1;          /* 0 = newest */
    if (d >= t->count) return NULL;
    return &t->v[(t->head + d) % HPACK_MAX_ENTRIES];
}

/* ------------------------------------------------------------------ decode */

/* Read a string literal (RFC 7541 5.2) at *off into a fresh buffer. */
static int read_string(const uint8_t *in, int len, int *off, char **out, int *outlen)
{
    if (*off >= len) return HPACK_E_TRUNC;
    int huff = (in[*off] & 0x80) != 0;
    uint32_t slen;
    int rc = hpack_int_decode(in, len, off, 7, &slen);
    if (rc != HPACK_OK) return rc;
    /* The length is checked against the bytes ACTUALLY PRESENT before any
     * allocation. This is the single line that separates this decoder from a
     * heap overflow. */
    if (slen > (uint32_t)(len - *off)) return HPACK_E_TRUNC;
    if (slen > (uint32_t)HPACK_STR_MAX) return HPACK_E_SIZE;

    if (huff) {
        rc = hpack_huff_decode(in + *off, (int)slen, out, outlen, HPACK_STR_MAX);
        if (rc != HPACK_OK) return rc;
    } else {
        char *s = dupn((const char *)(in + *off), (int)slen);
        if (!s) return HPACK_E_NOMEM;
        *out = s; *outlen = (int)slen;
    }
    /* An embedded NUL is rejected rather than carried.
     *
     * HPACK strings are octet strings, so a NUL is representable, and the
     * codec above will happily produce one -- symbol 0 has a perfectly good
     * Huffman code. But EVERY consumer of a header, here and in the browser,
     * treats the result as a C string: `strcmp(name, "content-length")` would
     * match a name of "content-length\0evil", and a value of "close\0keep" is
     * two different headers depending on which end reads it. That is a request
     * smuggling primitive, not a formatting quirk.
     *
     * RFC 9113 section 8.2.1 already forbids NUL in a field name or value, so
     * nothing legitimate is lost. The deviation is in the SEVERITY: the RFC
     * makes such a message malformed, which is a stream error, and rejecting
     * it here makes it a connection error instead. That is deliberate -- the
     * alternative is putting the octet string into the shared dynamic table
     * and hoping every later reader remembers it is not a C string. */
    for (int k = 0; k < *outlen; k++) {
        if ((*out)[k] == 0) { free(*out); *out = NULL; *outlen = 0; return HPACK_E_NUL; }
    }
    *off += (int)slen;
    return HPACK_OK;
}

void hpack_dec_init(struct hpack_dec *d, int cap_max)
{
    if (!d) return;
    memset(d, 0, sizeof *d);
    hpack_table_init(&d->tab, cap_max > 0 ? cap_max : HPACK_DEFAULT_CAP);
    d->list_max = HPACK_LIST_MAX;
}

void hpack_dec_free(struct hpack_dec *d)
{
    if (!d) return;
    hpack_table_free(&d->tab);
}

int hpack_decode(struct hpack_dec *d, const uint8_t *in, int len,
                 struct hpack_list *out)
{
    if (!d || !out || (!in && len)) return HPACK_E_ARG;
    if (len < 0) return HPACK_E_ARG;

    int off = 0;
    /* RFC 7541 4.2: dynamic table size updates MUST appear at the START of a
     * header block. A peer that slips one in after a field is either buggy or
     * probing, and accepting it there is how two implementations end up with
     * different tables while both think they agree. */
    int at_start = 1;
    int updates = 0;

    while (off < len) {
        uint8_t b = in[off];

        if (b & 0x80) {                              /* 1xxxxxxx indexed */
            uint32_t idx;
            int rc = hpack_int_decode(in, len, &off, 7, &idx);
            if (rc != HPACK_OK) return rc;
            if (idx == 0) return HPACK_E_INDEX;      /* index 0 is never valid */
            const struct hpack_entry *e = hpack_lookup(&d->tab, (int)idx);
            if (!e) return HPACK_E_INDEX;
            if (out->bytes + e->nlen + e->vlen + 32 > d->list_max) return HPACK_E_SIZE;
            rc = hpack_list_add(out, e->name, e->nlen, e->value, e->vlen, 0);
            if (rc != HPACK_OK) return rc;
            at_start = 0;
            continue;
        }

        if ((b & 0xE0) == 0x20) {                    /* 001xxxxx size update */
            uint32_t cap;
            int rc = hpack_int_decode(in, len, &off, 5, &cap);
            if (rc != HPACK_OK) return rc;
            if (!at_start) return HPACK_E_UPDATE;
            /* Two is the most that can be meaningful (shrink then grow, the
             * pattern RFC 7541 4.2 describes for a settings change). */
            if (++updates > 2) return HPACK_E_UPDATE;
            if (cap > (uint32_t)d->tab.cap_max) return HPACK_E_UPDATE;
            rc = hpack_table_resize(&d->tab, (int)cap);
            if (rc != HPACK_OK) return rc;
            continue;
        }

        /* The three literal forms differ only in the prefix width and what
         * they do to the table. */
        int prefix, index_it = 0, sensitive = 0;
        if ((b & 0xC0) == 0x40)      { prefix = 6; index_it = 1; }   /* 01 incremental */
        else if ((b & 0xF0) == 0x10) { prefix = 4; sensitive = 1; }  /* 0001 never indexed */
        else                         { prefix = 4; }                 /* 0000 without indexing */

        uint32_t nidx;
        int rc = hpack_int_decode(in, len, &off, prefix, &nidx);
        if (rc != HPACK_OK) return rc;

        char *name = NULL, *value = NULL;
        int nlen = 0, vlen = 0;
        if (nidx == 0) {
            rc = read_string(in, len, &off, &name, &nlen);
            if (rc != HPACK_OK) return rc;
        } else {
            const struct hpack_entry *e = hpack_lookup(&d->tab, (int)nidx);
            if (!e) return HPACK_E_INDEX;
            /* Copy: `e` points into the dynamic table, and inserting this
             * field can evict the very entry it names. */
            name = dupn(e->name, e->nlen);
            if (!name) return HPACK_E_NOMEM;
            nlen = e->nlen;
        }
        rc = read_string(in, len, &off, &value, &vlen);
        if (rc != HPACK_OK) { free(name); return rc; }

        if (out->bytes + nlen + vlen + 32 > d->list_max) {
            free(name); free(value);
            return HPACK_E_SIZE;
        }
        rc = hpack_list_add(out, name, nlen, value, vlen, sensitive);
        if (rc == HPACK_OK && index_it) rc = hpack_table_add(&d->tab, name, nlen, value, vlen);
        free(name); free(value);
        if (rc != HPACK_OK) return rc;
        at_start = 0;
    }

    d->blocks++;
    return HPACK_OK;
}

/* ------------------------------------------------------------------ encode */

void hpack_enc_init(struct hpack_enc *e, int cap_max)
{
    if (!e) return;
    memset(e, 0, sizeof *e);
    hpack_table_init(&e->tab, cap_max > 0 ? cap_max : HPACK_DEFAULT_CAP);
    e->pending_cap = -1;
    e->huffman = 1;
}

void hpack_enc_free(struct hpack_enc *e)
{
    if (!e) return;
    hpack_table_free(&e->tab);
}

void hpack_enc_set_capacity(struct hpack_enc *e, int cap)
{
    if (!e) return;
    if (cap < 0) cap = 0;
    if (cap > e->tab.cap_max) cap = e->tab.cap_max;
    e->pending_cap = cap;
}

/* Header names that must never enter a compression context: an attacker who
 * can inject a request and observe compressed sizes recovers a secret one
 * character at a time (CRIME). `cookie` is on the list too -- browsers do
 * index it, trading that attack against a real bandwidth win on a page with
 * dozens of subresources; here the client is not the thing under attack often
 * enough for that trade to be worth the exposure. */
static int is_sensitive(const char *name)
{
    return !strcmp(name, "authorization") || !strcmp(name, "proxy-authorization") ||
           !strcmp(name, "cookie") || !strcmp(name, "set-cookie");
}

struct ebuf { uint8_t *p; int n, cap; int err; };

static void eb_need(struct ebuf *b, int extra)
{
    if (b->err) return;
    if (b->n + extra <= b->cap) return;
    int ncap = b->cap ? b->cap * 2 : 256;
    while (ncap < b->n + extra) ncap *= 2;
    uint8_t *np = (uint8_t *)realloc(b->p, (size_t)ncap);
    if (!np) { b->err = HPACK_E_NOMEM; return; }
    b->p = np; b->cap = ncap;
}

static void eb_int(struct ebuf *b, int prefix, uint8_t flags, uint32_t v)
{
    eb_need(b, 8);
    if (b->err) return;
    int k = hpack_int_encode(b->p + b->n, b->cap - b->n, prefix, flags, v);
    if (k < 0) { b->err = k; return; }
    b->n += k;
}

static void eb_str(struct ebuf *b, const char *s, int n, int huffman)
{
    int hl = huffman ? hpack_huff_len(s, n) : n + 1;
    if (huffman && hl < n) {
        eb_int(b, 7, 0x80, (uint32_t)hl);
        eb_need(b, hl);
        if (b->err) return;
        int k = hpack_huff_encode(b->p + b->n, b->cap - b->n, s, n);
        if (k < 0) { b->err = HPACK_E_SIZE; return; }
        b->n += k;
    } else {
        eb_int(b, 7, 0x00, (uint32_t)n);
        eb_need(b, n);
        if (b->err) return;
        if (n) memcpy(b->p + b->n, s, (size_t)n);
        b->n += n;
    }
}

/* Find an exact name+value match, else a name-only match. Returns the absolute
 * index; *exact says which. Dynamic entries are searched newest-first so the
 * index is the smallest (and therefore the shortest to encode). */
static int enc_find(const struct hpack_enc *e, const char *name, int nlen,
                    const char *value, int vlen, int *exact)
{
    int name_only = 0;
    *exact = 0;
    for (int i = 1; i <= HPACK_STATIC_N; i++) {
        const struct hpack_entry *s = &g_static[i];
        if (s->nlen != nlen || memcmp(s->name, name, (size_t)nlen)) continue;
        if (s->vlen == vlen && !memcmp(s->value, value, (size_t)vlen)) { *exact = 1; return i; }
        if (!name_only) name_only = i;
    }
    for (int d = 0; d < e->tab.count; d++) {
        const struct hpack_entry *s = &e->tab.v[(e->tab.head + d) % HPACK_MAX_ENTRIES];
        if (s->nlen != nlen || memcmp(s->name, name, (size_t)nlen)) continue;
        int abs = HPACK_STATIC_N + 1 + d;
        if (s->vlen == vlen && !memcmp(s->value, value, (size_t)vlen)) { *exact = 1; return abs; }
        if (!name_only) name_only = abs;
    }
    return name_only;
}

int hpack_encode(struct hpack_enc *e, const struct hpack_list *in,
                 uint8_t **out, int *outlen)
{
    if (!e || !in || !out || !outlen) return HPACK_E_ARG;
    struct ebuf b = { NULL, 0, 0, 0 };

    if (e->pending_cap >= 0) {
        /* The size update goes FIRST, before any field -- the only position
         * RFC 7541 allows, and the position our own decoder enforces. */
        hpack_table_resize(&e->tab, e->pending_cap);
        eb_int(&b, 5, 0x20, (uint32_t)e->pending_cap);
        e->pending_cap = -1;
    }

    for (int i = 0; i < in->n && !b.err; i++) {
        const struct hpack_hdr *h = &in->v[i];
        int sens = h->sensitive || is_sensitive(h->name);
        int exact = 0;
        int idx = enc_find(e, h->name, h->nlen, h->value, h->vlen, &exact);

        if (exact && !sens) {                       /* 1xxxxxxx */
            eb_int(&b, 7, 0x80, (uint32_t)idx);
            continue;
        }
        if (sens) {                                  /* 0001xxxx never indexed */
            /* `idx` names a table entry whose NAME matches, whether or not the
             * value did; the name index is usable either way, and the value is
             * always spelled out so it never enters a table. */
            eb_int(&b, 4, 0x10, (uint32_t)idx);
            if (!idx) eb_str(&b, h->name, h->nlen, e->huffman);
            eb_str(&b, h->value, h->vlen, e->huffman);
            continue;
        }
        /* 01xxxxxx literal with incremental indexing: the field goes into our
         * table, and the peer's decoder puts it into its own at the same
         * moment. The two tables only stay equal if both run the eviction in
         * hpack_table_add -- which is why the encoder uses the same code. */
        eb_int(&b, 6, 0x40, (uint32_t)idx);
        if (!idx) eb_str(&b, h->name, h->nlen, e->huffman);
        eb_str(&b, h->value, h->vlen, e->huffman);
        if (!b.err) {
            int rc = hpack_table_add(&e->tab, h->name, h->nlen, h->value, h->vlen);
            if (rc != HPACK_OK) b.err = rc;
        }
    }

    if (b.err) { free(b.p); return b.err; }
    *out = b.p; *outlen = b.n;
    return HPACK_OK;
}
