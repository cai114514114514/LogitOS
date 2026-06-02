#include "inflate.h"

/* From-scratch RFC 1951 DEFLATE inflate, integer-only. LSB-first bit reader;
 * canonical Huffman decode via per-length first-code tables. */

struct bitr { const uint8_t *p; int len, pos; uint32_t bits; int nbits; };

static int getbit(struct bitr *b)
{
    if (b->nbits == 0) {
        if (b->pos >= b->len) return -1;
        b->bits = b->p[b->pos++];
        b->nbits = 8;
    }
    int v = b->bits & 1; b->bits >>= 1; b->nbits--;
    return v;
}
static int getbits(struct bitr *b, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) { int bit = getbit(b); if (bit < 0) return -1; v |= bit << i; }
    return v;
}

/* Canonical Huffman: built from code lengths. Decode bit-by-bit (MSB-first per
 * the DEFLATE convention) using first-code/first-symbol per length. */
#define MAXLEN 16
#define MAXSYM 288
struct huff { int count[MAXLEN]; int sym[MAXSYM]; int n; };

static void huff_build(struct huff *h, const uint8_t *lengths, int n)
{
    for (int i = 0; i < MAXLEN; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;
    int offs[MAXLEN]; offs[1] = 0;
    for (int l = 1; l < MAXLEN - 1; l++) offs[l + 1] = offs[l] + h->count[l];
    for (int i = 0; i < n; i++) if (lengths[i]) h->sym[offs[lengths[i]]++] = i;
    h->n = n;
}

static int huff_decode(struct bitr *b, const struct huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < MAXLEN; len++) {
        int bit = getbit(b); if (bit < 0) return -1;
        code |= bit;
        int cnt = h->count[len];
        if (code - first < cnt) return h->sym[index + (code - first)];
        index += cnt; first += cnt; first <<= 1; code <<= 1;
    }
    return -1;
}

static const int len_base[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const int len_extra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const int dist_base[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const int dist_extra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int inflate_block_body(struct bitr *b, const struct huff *lit, const struct huff *dist,
                              uint8_t *out, int outcap, int *op)
{
    for (;;) {
        int s = huff_decode(b, lit);
        if (s < 0) return -1;
        if (s == 256) return 0;                          /* end of block */
        if (s < 256) { if (*op >= outcap) return -1; out[(*op)++] = (uint8_t)s; continue; }
        s -= 257; if (s >= 29) return -1;
        int e = getbits(b, len_extra[s]); if (e < 0) return -1;
        int length = len_base[s] + e;
        int ds = huff_decode(b, dist); if (ds < 0 || ds >= 30) return -1;
        int de = getbits(b, dist_extra[ds]); if (de < 0) return -1;
        int distance = dist_base[ds] + de;
        if (distance > *op || *op + length > outcap) return -1;
        for (int i = 0; i < length; i++) { out[*op] = out[*op - distance]; (*op)++; }
    }
}

static const uint8_t clc_order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

int inflate_raw(const uint8_t *in, int inlen, uint8_t *out, int outcap, int *outlen)
{
    struct bitr b = { in, inlen, 0, 0, 0 };
    int op = 0;
    for (;;) {
        int final = getbit(&b); if (final < 0) return -1;
        int type = getbits(&b, 2); if (type < 0) return -1;
        if (type == 0) {                                 /* stored */
            b.nbits = 0;                                 /* align to byte */
            if (b.pos + 4 > b.len) return -1;
            int len = b.p[b.pos] | (b.p[b.pos+1] << 8); b.pos += 4;  /* skip LEN + NLEN */
            if (b.pos + len > b.len || op + len > outcap) return -1;
            for (int i = 0; i < len; i++) out[op++] = b.p[b.pos++];
        } else if (type == 1 || type == 2) {
            struct huff lit, dist;
            if (type == 1) {                             /* fixed Huffman */
                uint8_t ll[288], dl[30];
                for (int i = 0; i < 144; i++) ll[i] = 8;
                for (int i = 144; i < 256; i++) ll[i] = 9;
                for (int i = 256; i < 280; i++) ll[i] = 7;
                for (int i = 280; i < 288; i++) ll[i] = 8;
                for (int i = 0; i < 30; i++) dl[i] = 5;
                huff_build(&lit, ll, 288); huff_build(&dist, dl, 30);
            } else {                                     /* dynamic Huffman */
                int hlit = getbits(&b, 5) + 257;
                int hdist = getbits(&b, 5) + 1;
                int hclen = getbits(&b, 4) + 4;
                if (hlit > 286 || hdist > 30) return -1;
                uint8_t cll[19]; for (int i = 0; i < 19; i++) cll[i] = 0;
                for (int i = 0; i < hclen; i++) { int v = getbits(&b, 3); if (v < 0) return -1; cll[clc_order[i]] = (uint8_t)v; }
                struct huff cl; huff_build(&cl, cll, 19);
                uint8_t lens[288 + 30]; int n = 0, total = hlit + hdist;
                while (n < total) {
                    int s = huff_decode(&b, &cl); if (s < 0) return -1;
                    if (s < 16) lens[n++] = (uint8_t)s;
                    else if (s == 16) { int r = getbits(&b, 2) + 3; if (n == 0) return -1; while (r-- && n < total) { lens[n] = lens[n-1]; n++; } }
                    else if (s == 17) { int r = getbits(&b, 3) + 3;  while (r-- && n < total) lens[n++] = 0; }
                    else { int r = getbits(&b, 7) + 11; while (r-- && n < total) lens[n++] = 0; }
                }
                huff_build(&lit, lens, hlit);
                huff_build(&dist, lens + hlit, hdist);
            }
            if (inflate_block_body(&b, &lit, &dist, out, outcap, &op)) return -1;
        } else return -1;                                /* reserved BTYPE */
        if (final) break;
    }
    *outlen = op;
    return 0;
}

int zlib_decompress(const uint8_t *in, int inlen, uint8_t *out, int outcap, int *outlen)
{
    if (inlen < 2) return -1;
    int cmf = in[0], flg = in[1];
    if ((cmf & 0x0f) != 8) return -1;                    /* not DEFLATE */
    int off = 2;
    if (flg & 0x20) off += 4;                            /* preset dictionary id */
    if (off >= inlen) return -1;
    return inflate_raw(in + off, inlen - off, out, outcap, outlen);
}
