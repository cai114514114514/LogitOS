#include "scrypt.h"
#include "crypto.h"
#include <stdint.h>
#include <stddef.h>

/* scrypt (RFC 7914): Salsa20/8 Core, scryptBlockMix, scryptROMix, and the
 * PBKDF2-HMAC-SHA256 wrapper around them, straight off the spec text -- see
 * scrypt.h for the API and the constant-time argument, and pbkdf2.c for why
 * THIS tree's login record is PBKDF2 rather than this file (short version:
 * this machine has 512 MiB total and a young reclaim path, and a KDF an
 * unauthenticated caller can turn into an OOM is a worse trade here than
 * elsewhere -- read pbkdf2.c, it argues the number).
 *
 * SALSA20/8 IS NOT CHACHA20/8, AND THIS TREE ALREADY HAS A CHACHA CORE
 * ----------------------------------------------------------------------
 * c/crypto/aead/chacha20poly1305.c 20 lines away has a quarter-round that
 * LOOKS like this one and is not: ChaCha's quarter-round is
 * (a+=b;d^=a;d<<<=16; c+=d;b^=c;b<<<=12; a+=b;d^=a;d<<<=8; c+=d;b^=c;b<<<=7)
 * applied to the same four state words twice with two different orderings
 * (columns, then diagonals). Salsa20/8's round below applies FOUR INDEPENDENT
 * two-operand steps (x[b] ^= rotl(x[a]+x[c], n)) to four DIFFERENT triples,
 * and the "row" half indexes a transposed set from the "column" half rather
 * than reusing diagonals. Transcribing from the neighbouring file, or from
 * memory of "the ARX cipher with 8 in the name", produces code that mixes
 * bits just as thoroughly and agrees with nothing -- there is no vector in
 * this gate a self-consistent-but-wrong core would fail except the section 8
 * standalone one, which is exactly why it is first in the test file and not
 * folded into the full scrypt vectors.
 *
 * The array indices and rotate amounts below are transcribed verbatim from
 * RFC 7914 s3's C reference (which the RFC itself calls out as "included ...
 * as a stable reference"), not re-derived from the Salsa20 paper's row/column
 * description -- two people re-deriving "row round then column round" from
 * prose have historically disagreed with each other and with this RFC. */

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static uint32_t load_le32(const uint8_t p[4])
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store_le32(uint8_t p[4], uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

void salsa20_8_core(uint8_t out[64], const uint8_t in[64])
{
    uint32_t w[16], x[16];
    for (int i = 0; i < 16; i++) w[i] = load_le32(in + 4 * i);
    for (int i = 0; i < 16; i++) x[i] = w[i];

    for (int i = 8; i > 0; i -= 2) {
        x[ 4] ^= ROTL32(x[ 0]+x[12], 7);  x[ 8] ^= ROTL32(x[ 4]+x[ 0], 9);
        x[12] ^= ROTL32(x[ 8]+x[ 4],13);  x[ 0] ^= ROTL32(x[12]+x[ 8],18);
        x[ 9] ^= ROTL32(x[ 5]+x[ 1], 7);  x[13] ^= ROTL32(x[ 9]+x[ 5], 9);
        x[ 1] ^= ROTL32(x[13]+x[ 9],13);  x[ 5] ^= ROTL32(x[ 1]+x[13],18);
        x[14] ^= ROTL32(x[10]+x[ 6], 7);  x[ 2] ^= ROTL32(x[14]+x[10], 9);
        x[ 6] ^= ROTL32(x[ 2]+x[14],13);  x[10] ^= ROTL32(x[ 6]+x[ 2],18);
        x[ 3] ^= ROTL32(x[15]+x[11], 7);  x[ 7] ^= ROTL32(x[ 3]+x[15], 9);
        x[11] ^= ROTL32(x[ 7]+x[ 3],13);  x[15] ^= ROTL32(x[11]+x[ 7],18);
        x[ 1] ^= ROTL32(x[ 0]+x[ 3], 7);  x[ 2] ^= ROTL32(x[ 1]+x[ 0], 9);
        x[ 3] ^= ROTL32(x[ 2]+x[ 1],13);  x[ 0] ^= ROTL32(x[ 3]+x[ 2],18);
        x[ 6] ^= ROTL32(x[ 5]+x[ 4], 7);  x[ 7] ^= ROTL32(x[ 6]+x[ 5], 9);
        x[ 4] ^= ROTL32(x[ 7]+x[ 6],13);  x[ 5] ^= ROTL32(x[ 4]+x[ 7],18);
        x[11] ^= ROTL32(x[10]+x[ 9], 7);  x[ 8] ^= ROTL32(x[11]+x[10], 9);
        x[ 9] ^= ROTL32(x[ 8]+x[11],13);  x[10] ^= ROTL32(x[ 9]+x[ 8],18);
        x[12] ^= ROTL32(x[15]+x[14], 7);  x[13] ^= ROTL32(x[12]+x[15], 9);
        x[14] ^= ROTL32(x[13]+x[12],13);  x[15] ^= ROTL32(x[14]+x[13],18);
    }
    /* out[i] = x[i] + in[i] -- reading w[], the copy taken before the rounds
     * touched x[], is what lets `out` alias `in`: by the time anything is
     * written, every value the sum still needs has already been read out of
     * `in` and into w[]. */
    for (int i = 0; i < 16; i++) store_le32(out + 4 * i, x[i] + w[i]);
}

/* scryptBlockMix (RFC 7914 s4). `in`/`out` are 2*r 64-octet blocks each; see
 * scrypt.h for why they must be distinct buffers.
 *
 * THE TRAP THIS FUNCTION EXISTS TO NAME: step 3 of the spec is
 *   B' = (Y[0], Y[2], ..., Y[2r-2], Y[1], Y[3], ..., Y[2r-1])
 * -- INTERLEAVED, not (Y[0], Y[1], ..., Y[2r-1]) in the order they were
 * produced. Writing Y[i] to out[i] (natural/produced order) instead of the
 * even-then-odd order below builds a self-consistent KDF: it round-trips
 * against itself, it just does not compute scrypt. And it is invisible at
 * r=1 -- with 2r=2 blocks, interleaved order (Y[0],Y[1]) and natural order
 * (Y[0],Y[1]) are THE SAME SEQUENCE, so RFC 7914 s9's own BlockMix vector and
 * s12's first scrypt vector (both r=1) cannot catch this bug; only an r=8
 * vector can (this file's negative control, SCRYPT_BREAK_INTERLEAVE, is
 * built exactly to demonstrate that -- see scrypt_test.c). */
void scrypt_blockmix(uint32_t r, const uint8_t *in, uint8_t *out)
{
    uint8_t X[64], T[64];

    for (int j = 0; j < 64; j++) X[j] = in[(size_t)(2 * r - 1) * 64 + (size_t)j];

    for (uint32_t i = 0; i < 2 * r; i++) {
        for (int j = 0; j < 64; j++) T[j] = X[j] ^ in[(size_t)i * 64 + (size_t)j];
        salsa20_8_core(X, T);

#ifdef SCRYPT_BREAK_INTERLEAVE
        uint32_t dst = i;                              /* WRONG: produced order */
#else
        uint32_t dst = (i % 2 == 0) ? (i / 2) : (r + i / 2);  /* RFC 7914 s4 step 3 */
#endif
        for (int j = 0; j < 64; j++) out[(size_t)dst * 64 + (size_t)j] = X[j];
    }
    crypto_wipe(T, sizeof T);
}

uint64_t scrypt_romix_scratch_len(uint32_t r, uint64_t N)
{
    return (N + 2) * 128ULL * (uint64_t)r;
}

/* Integerify (RFC 7914 s5 step 3): interpret the LAST 64-octet block of the
 * 2r-block value as a little-endian integer, then mod N. N is bounded by what
 * fits in scratch on any machine that will ever call this (2^32 blocks of
 * 128*r octets is already terabytes), so only the low 8 octets of that block
 * -- a little-endian uint64_t -- are read; every octet above that is provably
 * zero for any N this file could plausibly be asked to support and reading it
 * would only cost cycles. Reading the FIRST block instead of the last, or
 * reading big-endian, both produce a plausible-looking pseudo-random access
 * pattern into V[] that happens to visit the wrong blocks -- a bug with no
 * crash and no self-inconsistency, caught only by the ROMix KAT (RFC 7914
 * s10), which is why that vector is in the gate on its own rather than folded
 * into the full scrypt vectors. */
static uint64_t integerify(const uint8_t *X, uint32_t r)
{
    const uint8_t *last = X + (size_t)(2 * r - 1) * 64;
    uint64_t v = 0;
    for (int j = 7; j >= 0; j--) v = (v << 8) | last[j];
    return v;
}

int scrypt_romix(uint32_t r, uint64_t N, uint8_t *B,
                  uint8_t *scratch, uint64_t scratch_len)
{
    if (r == 0 || N <= 1 || (N & (N - 1)) != 0) return -1;
    if (scratch_len < scrypt_romix_scratch_len(r, N)) return -1;

    uint64_t blk = 128ULL * r;
    uint8_t *V = scratch;
    uint8_t *X = scratch + N * blk;
    uint8_t *T = X + blk;

    for (uint64_t k = 0; k < blk; k++) X[k] = B[k];

    /* Step 2: fill V[], walking X forward through scryptBlockMix. blockmix
     * needs in != out, so this ping-pongs through T rather than writing X in
     * place, then copies back -- an extra 128*r-byte copy per iteration that
     * a consumer chasing throughput would want to remove by swapping pointers
     * instead; nothing here has that consumer yet (see scrypt.h). */
    for (uint64_t i = 0; i < N; i++) {
        for (uint64_t k = 0; k < blk; k++) V[i * blk + k] = X[k];
        scrypt_blockmix(r, X, T);
        for (uint64_t k = 0; k < blk; k++) X[k] = T[k];
    }

    /* Step 3: NOT constant time by construction -- see scrypt.h. */
    for (uint64_t i = 0; i < N; i++) {
        uint64_t j = integerify(X, r) % N;
        const uint8_t *Vj = V + j * blk;
        for (uint64_t k = 0; k < blk; k++) T[k] = X[k] ^ Vj[k];
        scrypt_blockmix(r, T, X);
    }

    for (uint64_t k = 0; k < blk; k++) B[k] = X[k];
    crypto_wipe(T, (size_t)blk);
    return 0;
}

/* ---- PBKDF2-HMAC-SHA256 with iteration count fixed at 1, streamed ----
 *
 * ONE JAR, TWO DOORS, AVOIDED ON PURPOSE: the tree already has pbkdf2() in
 * this same directory (crypto.h:303), and it is NOT reused here. Its salt
 * buffer is `uint8_t block[256 + 4]` -- sized for pwhash_check's login
 * record, where the salt is a handful of random bytes. scrypt's SECOND
 * PBKDF2-HMAC-SHA256 call (RFC 7914 s6 step 3) salts on the whole B[] block
 * array, which is p*128*r octets -- 16,384 for RFC 7914 s12 test vector 2
 * alone, already 64x past that cap. Raising a limit that was sized for a
 * different caller, to serve this one, is how the two callers end up
 * fighting over one constant (CLAUDE.md's "one jar, two doors"). This
 * instead streams the salt straight into HMAC's inner hash via sha256_update
 * in one call, with no length cap and no concatenation buffer, and needs
 * only the c=1 special case (F() collapses to a single HMAC per block, RFC
 * 2898 s5.2 with c=1: T_i = U_1). */
static void hmac_sha256_2(const uint8_t *key, int keylen,
                           const uint8_t *m1, uint64_t m1len,
                           const uint8_t ctr[4],
                           uint8_t out[32])
{
    uint8_t kpad[64], ipad[64], opad[64];
    if (keylen > 64) { sha256(key, (size_t)keylen, kpad); for (int j = 32; j < 64; j++) kpad[j] = 0; }
    else { int j = 0; for (; j < keylen; j++) kpad[j] = key[j]; for (; j < 64; j++) kpad[j] = 0; }
    for (int j = 0; j < 64; j++) { ipad[j] = (uint8_t)(kpad[j] ^ 0x36); opad[j] = (uint8_t)(kpad[j] ^ 0x5c); }

    struct sha256 c;
    uint8_t ih[32];
    sha256_init(&c);
    sha256_update(&c, ipad, 64);
    sha256_update(&c, m1, (size_t)m1len);
    sha256_update(&c, ctr, 4);
    sha256_final(&c, ih);

    sha256_init(&c);
    sha256_update(&c, opad, 64);
    sha256_update(&c, ih, 32);
    sha256_final(&c, out);

    crypto_wipe(kpad, sizeof kpad);
    crypto_wipe(ipad, sizeof ipad);
    crypto_wipe(opad, sizeof opad);
    crypto_wipe(ih, sizeof ih);
}

static void pbkdf2_sha256_c1(const uint8_t *pw, int pwlen,
                              const uint8_t *salt, uint64_t saltlen,
                              uint8_t *dk, uint64_t dklen)
{
    uint64_t done = 0;
    uint32_t i = 1;
    while (done < dklen) {
        uint8_t ctr[4] = { (uint8_t)(i >> 24), (uint8_t)(i >> 16), (uint8_t)(i >> 8), (uint8_t)i };
        uint8_t u[32];
        hmac_sha256_2(pw, pwlen, salt, saltlen, ctr, u);
        uint64_t n = dklen - done; if (n > 32) n = 32;
        for (uint64_t j = 0; j < n; j++) dk[done + j] = u[j];
        done += n; i++;
        crypto_wipe(u, sizeof u);
    }
}

uint64_t scrypt_scratch_len(uint64_t N, uint32_t r, uint32_t p)
{
    return (uint64_t)p * 128ULL * (uint64_t)r + scrypt_romix_scratch_len(r, N);
}

int scrypt(const uint8_t *pw, int pwlen, const uint8_t *salt, int saltlen,
           uint64_t N, uint32_t r, uint32_t p,
           uint8_t *dk, uint64_t dklen,
           uint8_t *scratch, uint64_t scratch_len)
{
    if (r == 0 || p == 0 || N <= 1 || (N & (N - 1)) != 0 || dklen == 0) return -1;
    if (pwlen < 0 || saltlen < 0) return -1;

    uint64_t blk = 128ULL * r;
    uint64_t bblen = (uint64_t)p * blk;
    uint64_t romix_len = scrypt_romix_scratch_len(r, N);
    if (scratch_len < bblen + romix_len) return -1;

    uint8_t *Bbuf = scratch;
    uint8_t *romix_scratch = scratch + bblen;

    /* Step 1 (RFC 7914 s6): B[0..p-1] = PBKDF2-HMAC-SHA256(P, S, 1, p*128*r) */
    pbkdf2_sha256_c1(pw, pwlen, salt, (uint64_t)saltlen, Bbuf, bblen);

    /* Step 2: each of the p blocks through ROMix independently. p blocks in
     * one password imply p*N*128*r bytes of TOTAL memory traffic but only ONE
     * romix_scratch region -- the p passes are sequential, never concurrent,
     * so reusing the region is exact, not an approximation. */
    for (uint32_t i = 0; i < p; i++) {
        if (scrypt_romix(r, N, Bbuf + (uint64_t)i * blk, romix_scratch, romix_len) != 0) {
            crypto_wipe(scratch, (size_t)scratch_len);
            return -1;
        }
    }

    /* Step 3: DK = PBKDF2-HMAC-SHA256(P, B, 1, dkLen) */
    pbkdf2_sha256_c1(pw, pwlen, Bbuf, bblen, dk, dklen);

    crypto_wipe(scratch, (size_t)scratch_len);
    return 0;
}
