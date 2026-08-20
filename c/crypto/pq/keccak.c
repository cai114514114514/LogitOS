#include "keccak.h"

/* Keccak-f[1600] + FIPS 202 SHA3-256/512 and SHAKE128/256.
 *
 * Every table below was DERIVED by build/tlsx/gen_keccak.py from the spec's own
 * definitions rather than copied from another implementation, for the reason
 * the VP8 tables carry in CLAUDE.md: a wrong constant here does not shade an
 * output, it makes the permutation a different function, and one wrong bit in
 * twenty-four 64-bit words is not findable by looking. The derivations:
 *
 *  RC[]  FIPS 202 Alg.5. rc(t) is the LFSR x^8+x^6+x^5+x^4+1 (0x71); round i's
 *        constant sets bit 2^j-1 from rc(j + 7i), j = 0..6. XOR of all 24 is
 *        0x800000000000800a -- a checksum to diff against, not a magic number.
 *  RHO[] FIPS 202 Alg.2. Walk (x,y) <- (y, 2x+3y mod 5) from (1,0); the t-th
 *        lane visited rotates by (t+1)(t+2)/2 mod 64. Sum over all lanes = 680.
 *  PI[]  FIPS 202 Alg.3, A'[x,y] = A[x+3y, x], flattened at lane index x+5y.
 *        Verified to be a permutation of 0..24 (it is; a typo here would make
 *        it not one, which is why that check is worth printing).
 *
 * No libc: the byte loops are written out rather than calling memcpy/memset,
 * because this file compiles into the kernel and into ring-3 test binaries that
 * link neither. Loop-per-byte rather than word tricks -- the permutation
 * dominates by two orders of magnitude, so the copy is not worth a cast that
 * would also be an alignment assumption.
 */

static const uint64_t RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL,
    0x800000000000808aULL, 0x8000000080008000ULL,
    0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008aULL, 0x0000000000000088ULL,
    0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL,
    0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL,
    0x0000000080000001ULL, 0x8000000080008008ULL,
};

static const int RHO[25] = {
     0,  1, 62, 28, 27, 36, 44,  6, 55, 20,  3, 10, 43,
    25, 39, 41, 45, 15, 21,  8, 18,  2, 61, 56, 14
};

static const int PI[25] = {
     0,  6, 12, 18, 24,  3,  9, 10, 16, 22,  1,  7, 13,
    19, 20,  4,  5, 11, 17, 23,  2,  8, 14, 15, 21
};

static uint64_t rol64(uint64_t x, int n)
{
    /* n == 0 must not become a 64-bit shift (undefined). RHO[0] is 0, so this
     * is reached on every single permutation, not an edge case. */
    return n ? ((x << n) | (x >> (64 - n))) : x;
}

void keccakf1600(uint64_t st[25])
{
    for (int rnd = 0; rnd < 24; rnd++) {
        uint64_t c[5], d[5], b[25];

        /* theta */
        for (int x = 0; x < 5; x++)
            c[x] = st[x] ^ st[x+5] ^ st[x+10] ^ st[x+15] ^ st[x+20];
        for (int x = 0; x < 5; x++)
            d[x] = c[(x+4)%5] ^ rol64(c[(x+1)%5], 1);
        for (int y = 0; y < 5; y++)
            for (int x = 0; x < 5; x++)
                st[x+5*y] ^= d[x];

        /* rho + pi, fused: b[dest] = rot(st[src]) with src = PI[dest]. Fusing
         * them needs the scratch b[] anyway, and pi is a permutation so an
         * in-place version would need it too. */
        for (int i = 0; i < 25; i++)
            b[i] = rol64(st[PI[i]], RHO[PI[i]]);

        /* chi */
        for (int y = 0; y < 5; y++)
            for (int x = 0; x < 5; x++)
                st[x+5*y] = b[x+5*y] ^ ((~b[(x+1)%5+5*y]) & b[(x+2)%5+5*y]);

        /* iota */
        st[0] ^= RC[rnd];
    }
}

/* Little-endian lane access. Keccak's state is defined over bits and FIPS 202
 * fixes the byte order as little-endian; doing it byte by byte rather than
 * casting to uint64_t* keeps this correct on a big-endian target and free of
 * an alignment assumption about the caller's buffer. */
static uint64_t lane_get(const uint64_t st[25], int i) { return st[i]; }

static void st_xor_byte(uint64_t st[25], int off, uint8_t v)
{
    st[off >> 3] ^= (uint64_t)v << (8 * (off & 7));
}
static uint8_t st_get_byte(const uint64_t st[25], int off)
{
    return (uint8_t)(lane_get(st, off >> 3) >> (8 * (off & 7)));
}

static void shake_init(struct shake *s, int rate)
{
    for (int i = 0; i < 25; i++) s->st[i] = 0;
    s->rate = rate;
    s->pos = 0;
    s->squeezing = 0;
}
void shake128_init(struct shake *s) { shake_init(s, 168); }
void shake256_init(struct shake *s) { shake_init(s, 136); }

void shake_absorb(struct shake *s, const uint8_t *in, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        st_xor_byte(s->st, s->pos, in[i]);
        if (++s->pos == s->rate) { keccakf1600(s->st); s->pos = 0; }
    }
}

/* pad10*1 with the SHAKE domain separator 0x1F (FIPS 202 6.2: the 1111 suffix
 * for XOFs, then the 1 of pad10*1, little-endian = 0x1F). SHA-3's is 0x06.
 * Getting this byte wrong yields a well-formed hash of the wrong function --
 * which is exactly why the gate below is openssl and not a self-comparison. */
static void pad_and_switch(struct shake *s, uint8_t dsbyte)
{
    st_xor_byte(s->st, s->pos, dsbyte);
    st_xor_byte(s->st, s->rate - 1, 0x80);
    keccakf1600(s->st);
    s->pos = 0;
    s->squeezing = 1;
}

void shake_finalize(struct shake *s) { pad_and_switch(s, 0x1F); }

void shake_squeeze(struct shake *s, uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s->pos == s->rate) { keccakf1600(s->st); s->pos = 0; }
        out[i] = st_get_byte(s->st, s->pos);
        s->pos++;
    }
}

static void xof(uint8_t *out, size_t outlen, const uint8_t *in, size_t inlen, int rate)
{
    struct shake s;
    shake_init(&s, rate);
    shake_absorb(&s, in, inlen);
    shake_finalize(&s);
    shake_squeeze(&s, out, outlen);
}
void shake128(uint8_t *out, size_t outlen, const uint8_t *in, size_t inlen)
{ xof(out, outlen, in, inlen, 168); }
void shake256(uint8_t *out, size_t outlen, const uint8_t *in, size_t inlen)
{ xof(out, outlen, in, inlen, 136); }

/* SHA3-d: rate = 200 - 2*d/8, domain separator 0x06. */
static void sha3(uint8_t *out, int outlen, const uint8_t *in, size_t inlen)
{
    struct shake s;
    shake_init(&s, 200 - 2 * outlen);
    shake_absorb(&s, in, inlen);
    pad_and_switch(&s, 0x06);
    shake_squeeze(&s, out, (size_t)outlen);
}
void sha3_256(uint8_t out[32], const uint8_t *in, size_t inlen) { sha3(out, 32, in, inlen); }
void sha3_512(uint8_t out[64], const uint8_t *in, size_t inlen) { sha3(out, 64, in, inlen); }
