#include "argon2.h"
#include "crypto.h"
#include <stdint.h>
#include <stddef.h>

/* Argon2 (RFC 9106, version 0x13). See argon2.h for the API and the
 * no-consumer / constant-time notes; this file is the RFC's own construction,
 * transcribed section by section, with the traps the RFC does not spell out
 * as loudly as it should.
 *
 * NO LIBC: byte loops instead of memcpy/memset, matching c/crypto/pq's
 * keccak.c and mlkem.c -- this file compiles into the kernel and into a host
 * test, and a bare memcpy would resolve to a different implementation (or
 * none) in each.
 *
 * THE TRAP THIS FILE IS NAMED AFTER: THE INDEXING MODE SWITCH
 * --------------------------------------------------------------------------
 * Argon2 has THREE reference-index generators, not one:
 *
 *   Argon2d  every block's reference index comes from the PREVIOUS block's
 *            own first 8 bytes (data-DEPENDENT addressing) -- maximum
 *            resistance to a time-memory trade-off, but the access pattern
 *            is a function of the password and therefore leaks through
 *            cache/DRAM timing to anything else running on the machine.
 *   Argon2i  every block's reference index comes from a SEPARATE stream,
 *            seeded only from the public parameters (pass, lane, slice, m',
 *            t, type) and a running counter -- never from a block's
 *            contents (data-INDEPENDENT addressing). Immune to that side
 *            channel; weaker against the trade-off attack it does buy.
 *   Argon2id runs Argon2i's data-independent generator for the first TWO
 *            slices of PASS 0 ONLY, and Argon2d's data-dependent generator
 *            for every other (pass, slice) -- slices 2 and 3 of pass 0, and
 *            the whole of every later pass.
 *
 * Getting the id switch point wrong is invisible in exactly the way a wrong
 * password KDF should not be: the wrong version is still deterministic
 * (same input always produces the same output), still memory-hard (it still
 * touches every block, still costs the claimed time and space), and still
 * passes a round-trip against ITSELF (hash, then re-verify with the same
 * code -- of course it matches, both calls made the same mistake). None of
 * that catches a switch at the wrong boundary, or a switch that never
 * happens, or one that happens at every slice instead of the first two of
 * pass 0 only. The ONLY thing that catches it is an independently-computed
 * answer -- which is why this file's test is three official RFC 9106
 * vectors run through the SAME code with only the `type` argument changed
 * (5.1 Argon2d, 5.2 Argon2i, 5.3 Argon2id, identical password/salt/secret/
 * ad), plus per-pass intermediate block state from the reference
 * implementation's own KAT file, plus an OpenSSL differential across
 * randomised parameters. See argon2_test.c and this file's negative control
 * (ARGON2_NEGCTL_ALWAYS_DATA_DEP below) for how that switch is watched
 * actually mattering rather than assumed to.
 *
 * THE VERSION BYTE. RFC 9106 defines TWO wire versions: 0x10 (the pre-2016
 * "Argon2 v1.2.1", which OVERWRITES each new block outright) and 0x13
 * (the current one, which XORs the new block INTO the block already there on
 * every pass after the first). Both produce a deterministic, memory-hard,
 * self-consistent answer for a DIFFERENT function. This file implements ONLY
 * 0x13 -- it is not a parameter, it is what argon2() computes -- because
 * shipping the older wire format as a silent option is exactly the kind of
 * thing that gets selected by accident and produces answers that agree with
 * nothing. The RFC 5.3 vector runs 3 passes, so it exercises the
 * overwrite-vs-XOR rule twice (pass 0 overwrites everywhere -- there is no
 * old content yet -- passes 1 and 2 must XOR), and this file's intermediate-
 * block checks are taken after EACH pass specifically so a version-byte
 * mistake cannot hide behind a tag that only gets checked at the very end.
 */

/* ---------------------------------------------------------------- BLAKE2b --
 * A private, minimal BLAKE2b (RFC 7693): init/update/final for outlen in
 * 1..64, unkeyed, sequential (no tree hashing -- Argon2 never needs it).
 * This is NOT a general-purpose hash export: it lives here because Argon2's
 * H and H' (RFC 9106 3.3) are both built on it and nothing else in this tree
 * currently needs BLAKE2b at all. If a second consumer shows up, promoting
 * this to c/crypto/hash/blake2b.c with its own header is the right move --
 * doing it now, for a hypothetical second caller that does not exist, would
 * just be a second place for these constants to be wrong. */

#define B2_IV0 0x6a09e667f3bcc908ULL
#define B2_IV1 0xbb67ae8584caa73bULL
#define B2_IV2 0x3c6ef372fe94f82bULL
#define B2_IV3 0xa54ff53a5f1d36f1ULL
#define B2_IV4 0x510e527fade682d1ULL
#define B2_IV5 0x9b05688c2b3e6c1fULL
#define B2_IV6 0x1f83d9abfb41bd6bULL
#define B2_IV7 0x5be0cd19137e2179ULL

static const uint64_t b2_iv[8] = {
    B2_IV0, B2_IV1, B2_IV2, B2_IV3, B2_IV4, B2_IV5, B2_IV6, B2_IV7,
};

/* RFC 7693 section 2.7's message schedule, all 12 rounds (rounds 10 and 11
 * repeat rounds 0 and 1 -- that repetition is in the spec, not a copy/paste
 * error here). */
static const uint8_t b2_sigma[12][16] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15},
    {14,10, 4, 8, 9,15,13, 6, 1,12, 0, 2,11, 7, 5, 3},
    {11, 8,12, 0, 5, 2,15,13,10,14, 3, 6, 7, 1, 9, 4},
    { 7, 9, 3, 1,13,12,11,14, 2, 6, 5,10, 4, 0,15, 8},
    { 9, 0, 5, 7, 2, 4,10,15,14, 1,11,12, 6, 8, 3,13},
    { 2,12, 6,10, 0,11, 8, 3, 4,13, 7, 5,15,14, 1, 9},
    {12, 5, 1,15,14,13, 4,10, 0, 7, 6, 3, 9, 2, 8,11},
    {13,11, 7,14,12, 1, 3, 9, 5, 0,15, 4, 8, 6, 2,10},
    { 6,15,14, 9,11, 3, 0, 8,12, 2,13, 7, 1, 4,10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5,15,11, 9,14, 3,12,13, 0},
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15},
    {14,10, 4, 8, 9,15,13, 6, 1,12, 0, 2,11, 7, 5, 3},
};

static uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static uint64_t load64_le(const uint8_t p[8])
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void store64_le(uint8_t p[8], uint64_t v)
{
    for (int i = 0; i < 8; i++) { p[i] = (uint8_t)v; v >>= 8; }
}

static void store32_le(uint8_t p[4], uint32_t v)
{
    for (int i = 0; i < 4; i++) { p[i] = (uint8_t)v; v >>= 8; }
}

struct b2 {
    uint64_t h[8];
    uint64_t t0, t1;      /* byte counter, low/high -- NOT a secret, just a length */
    uint8_t  buf[128];
    uint32_t buflen;
};

/* Unkeyed BLAKE2b init (RFC 7693 3.2): XOR the parameter block into the IV.
 * For unkeyed, unsalted, non-tree BLAKE2b every parameter-block byte is 0
 * except digest_length (byte 0) and fanout=depth=1 (bytes 2,3), so the XOR
 * reduces to touching only h[0], with the well-known constant 0x01010000. */
static void b2_init(struct b2 *s, uint32_t outlen)
{
    for (int i = 0; i < 8; i++) s->h[i] = b2_iv[i];
    s->h[0] ^= (0x01010000ULL | outlen);
    s->t0 = 0; s->t1 = 0; s->buflen = 0;
}

#define B2_G(a, b, c, d, x, y) do {          \
    a = a + b + (x);                         \
    d = rotr64(d ^ a, 32);                   \
    c = c + d;                               \
    b = rotr64(b ^ c, 24);                   \
    a = a + b + (y);                         \
    d = rotr64(d ^ a, 16);                   \
    c = c + d;                               \
    b = rotr64(b ^ c, 63);                   \
} while (0)

static void b2_compress(struct b2 *s, const uint8_t block[128], int is_last)
{
    uint64_t m[16], v[16];
    for (int i = 0; i < 16; i++) m[i] = load64_le(block + 8 * i);
    for (int i = 0; i < 8; i++) v[i] = s->h[i];
    v[8] = B2_IV0; v[9] = B2_IV1; v[10] = B2_IV2; v[11] = B2_IV3;
    v[12] = B2_IV4 ^ s->t0;
    v[13] = B2_IV5 ^ s->t1;
    v[14] = B2_IV6 ^ (is_last ? ~0ULL : 0ULL);
    v[15] = B2_IV7;   /* last-node flag: always 0, this file never trees */

    for (int r = 0; r < 12; r++) {
        const uint8_t *sg = b2_sigma[r];
        B2_G(v[0], v[4], v[8],  v[12], m[sg[0]],  m[sg[1]]);
        B2_G(v[1], v[5], v[9],  v[13], m[sg[2]],  m[sg[3]]);
        B2_G(v[2], v[6], v[10], v[14], m[sg[4]],  m[sg[5]]);
        B2_G(v[3], v[7], v[11], v[15], m[sg[6]],  m[sg[7]]);
        B2_G(v[0], v[5], v[10], v[15], m[sg[8]],  m[sg[9]]);
        B2_G(v[1], v[6], v[11], v[12], m[sg[10]], m[sg[11]]);
        B2_G(v[2], v[7], v[8],  v[13], m[sg[12]], m[sg[13]]);
        B2_G(v[3], v[4], v[9],  v[14], m[sg[14]], m[sg[15]]);
    }
    for (int i = 0; i < 8; i++) s->h[i] ^= v[i] ^ v[i + 8];
}

static void b2_update(struct b2 *s, const uint8_t *in, uint32_t inlen)
{
    if (inlen == 0) return;
    if (s->buflen + inlen > 128) {
        uint32_t fill = 128 - s->buflen;
        for (uint32_t i = 0; i < fill; i++) s->buf[s->buflen + i] = in[i];
        s->t0 += 128; if (s->t0 < 128) s->t1++;
        b2_compress(s, s->buf, 0);
        s->buflen = 0;
        in += fill; inlen -= fill;
        /* Strictly '>', not '>=': an input that ends exactly on a 128-byte
         * boundary must leave that last block BUFFERED, not compressed here
         * -- only b2_final knows whether it is the last block, and the last
         * block's compression differs (the finalisation flag). Compressing
         * it early would silently produce the wrong hash for every input
         * whose length is a multiple of 128 bytes after the first fill. */
        while (inlen > 128) {
            s->t0 += 128; if (s->t0 < 128) s->t1++;
            b2_compress(s, in, 0);
            in += 128; inlen -= 128;
        }
    }
    for (uint32_t i = 0; i < inlen; i++) s->buf[s->buflen + i] = in[i];
    s->buflen += inlen;
}

static void b2_final(struct b2 *s, uint8_t *out, uint32_t outlen)
{
    s->t0 += s->buflen; if (s->t0 < s->buflen) s->t1++;
    for (uint32_t i = s->buflen; i < 128; i++) s->buf[i] = 0;
    b2_compress(s, s->buf, 1);
    uint8_t tmp[64];
    for (int i = 0; i < 8; i++) store64_le(tmp + 8 * i, s->h[i]);
    for (uint32_t i = 0; i < outlen; i++) out[i] = tmp[i];
    crypto_wipe(tmp, sizeof tmp);
}

/* H' (RFC 9106 3.3): the variable-length hash Argon2 uses to expand H0 into
 * the first two blocks of every lane and to produce the final tag. For
 * outlen <= 64 it is one BLAKE2b call with LE32(outlen) prepended to the
 * message. For outlen > 64 (every block-sized call: ARGON2_BLOCK_SIZE =
 * 1024) it chains 64-byte BLAKE2b outputs, each re-hashed to produce the
 * next, keeping only the FIRST HALF of each 64-byte output in the result --
 * transcribed from the reference implementation's blake2b_long, which the
 * RFC's prose describes but does not give in one place as cleanly as the
 * reference code does. */
static void argon2_hprime(uint8_t *out, uint32_t outlen, const uint8_t *in, uint32_t inlen)
{
    uint8_t outlen_le[4];
    store32_le(outlen_le, outlen);

    if (outlen <= 64) {
        struct b2 s;
        b2_init(&s, outlen);
        b2_update(&s, outlen_le, 4);
        b2_update(&s, in, inlen);
        b2_final(&s, out, outlen);
        return;
    }

    uint8_t out_buf[64], in_buf[64];
    struct b2 s;
    b2_init(&s, 64);
    b2_update(&s, outlen_le, 4);
    b2_update(&s, in, inlen);
    b2_final(&s, out_buf, 64);

    for (int i = 0; i < 32; i++) out[i] = out_buf[i];
    out += 32;
    uint32_t toproduce = outlen - 32;

    while (toproduce > 64) {
        for (int i = 0; i < 64; i++) in_buf[i] = out_buf[i];
        struct b2 s2;
        b2_init(&s2, 64);
        b2_update(&s2, in_buf, 64);
        b2_final(&s2, out_buf, 64);
        for (int i = 0; i < 32; i++) out[i] = out_buf[i];
        out += 32;
        toproduce -= 32;
    }

    for (int i = 0; i < 64; i++) in_buf[i] = out_buf[i];
    struct b2 s3;
    b2_init(&s3, toproduce);
    b2_update(&s3, in_buf, 64);
    b2_final(&s3, out_buf, toproduce);
    for (uint32_t i = 0; i < toproduce; i++) out[i] = out_buf[i];

    crypto_wipe(in_buf, sizeof in_buf);
    crypto_wipe(out_buf, sizeof out_buf);
}

/* --------------------------------------------------------- the G function --
 * G(X, Y): 1024-byte X, Y -> 1024-byte block. RFC 9106 3.5. R = X xor Y,
 * viewed as an 8x8 matrix of 16-byte (2-word) registers; apply the BLAKE2b
 * round function P (with NO message injection) to each of the 8 ROWS, then
 * to each of the 8 COLUMNS; output is that result XORed with R again.
 *
 * P USES "BlaMka", NOT PLAIN BLAKE2b MIXING -- the second place after the
 * indexing-mode switch that a transcription from memory gets this file
 * wrong. Argon2's P replaces BLAKE2b's `a = a + b (+ message word)` with
 * fBlaMka(a, b) = a + b + 2*lo32(a)*lo32(b) (design credited to the Lyra PHC
 * team), and drops the message words entirely (there is no message here,
 * only the block being mixed). Swap fBlaMka for plain `a + b` and every
 * other property still holds -- deterministic, memory-hard, internally
 * consistent -- while every RFC 9106 tag is wrong. That is a real, worse
 * cousin of the indexing-mode trap this file's negative control targets, and
 * it is why the KAT comparison in argon2_test.c pins the RFC 5.3 vector's
 * intermediate BLOCK contents (not just the final tag): a mixing-function
 * bug shows up in the very first block computed, where a mode-switch bug
 * would not. */
static uint64_t fblamka(uint64_t x, uint64_t y)
{
    uint64_t xy = (x & 0xFFFFFFFFULL) * (y & 0xFFFFFFFFULL);
    return x + y + 2 * xy;
}

#define ARGON2_G(v, a, b, c, d) do {         \
    v[a] = fblamka(v[a], v[b]);              \
    v[d] = rotr64(v[d] ^ v[a], 32);          \
    v[c] = fblamka(v[c], v[d]);              \
    v[b] = rotr64(v[b] ^ v[c], 24);          \
    v[a] = fblamka(v[a], v[b]);              \
    v[d] = rotr64(v[d] ^ v[a], 16);          \
    v[c] = fblamka(v[c], v[d]);              \
    v[b] = rotr64(v[b] ^ v[c], 63);          \
} while (0)

/* The BLAKE2b round function P, no message schedule, on 16 words in place. */
static void argon2_p(uint64_t v[16])
{
    ARGON2_G(v, 0, 4,  8, 12);
    ARGON2_G(v, 1, 5,  9, 13);
    ARGON2_G(v, 2, 6, 10, 14);
    ARGON2_G(v, 3, 7, 11, 15);
    ARGON2_G(v, 0, 5, 10, 15);
    ARGON2_G(v, 1, 6, 11, 12);
    ARGON2_G(v, 2, 7,  8, 13);
    ARGON2_G(v, 3, 4,  9, 14);
}

static void argon2_fill_block(const struct argon2_block *prev,
                               const struct argon2_block *ref,
                               struct argon2_block *next, int with_xor)
{
    struct argon2_block r, tmp;
    for (int i = 0; i < ARGON2_QWORDS; i++) r.v[i] = ref->v[i] ^ prev->v[i];
    for (int i = 0; i < ARGON2_QWORDS; i++) tmp.v[i] = r.v[i];
    /* Pass > 0: XOR the new content INTO the block that is already there.
     * Pass 0: there IS nothing there yet, so `with_xor` is 0 and this is a
     * plain overwrite. Doing the XOR on pass 0 too would not crash or even
     * look wrong -- next's initial content is whatever this run's own first-
     * block generation put there, so it is still deterministic -- it is
     * simply a different, non-standard function. RFC 9106 3.4 states the
     * pass-0 exception in one clause easy to read past. */
    if (with_xor)
        for (int i = 0; i < ARGON2_QWORDS; i++) tmp.v[i] ^= next->v[i];

    /* 8 rows of 16 contiguous words each. */
    for (int i = 0; i < 8; i++) argon2_p(&r.v[16 * i]);

    /* 8 columns: word pair (2i, 2i+1) taken from each of the 8 row-groups.
     * This is NOT the same traversal as the row pass re-run on a transpose
     * of the whole matrix -- it is 16-byte (2-word) REGISTERS transposed,
     * so consecutive words 2i/2i+1 stay adjacent inside each new group. */
    for (int i = 0; i < 8; i++) {
        uint64_t v[16];
        for (int j = 0; j < 16; j++) v[j] = r.v[2 * i + 16 * (j / 2) + (j % 2)];
        argon2_p(v);
        for (int j = 0; j < 16; j++) r.v[2 * i + 16 * (j / 2) + (j % 2)] = v[j];
    }

    for (int i = 0; i < ARGON2_QWORDS; i++) next->v[i] = tmp.v[i] ^ r.v[i];
}

/* -------------------------------------------------- data-independent addressing --
 * Argon2i's (and Argon2id's first-two-slices-of-pass-0) pseudo-random stream:
 * an "address block" is itself the output of G applied twice to an all-zero
 * block and a running counter block (RFC 9106 3.4). One address block holds
 * 128 uint64 reference values -- ARGON2_QWORDS_IN_BLOCK in the reference
 * implementation's naming, which not coincidentally equals ARGON2_QWORDS
 * above; a 1024-byte block IS 128 addresses when addresses are 8 bytes. */
static void argon2_next_addresses(struct argon2_block *addr,
                                   struct argon2_block *input,
                                   const struct argon2_block *zero)
{
    input->v[6]++;
    argon2_fill_block(zero, input, addr, 0);
    argon2_fill_block(zero, addr, addr, 0);
}

/* RFC 9106 3.4's reference-window computation. `index` is this block's
 * position within its segment (0-based); `same_lane` is whether the chosen
 * reference lane is the lane currently being filled.
 *
 * THE THIRD TRAP: on the "same lane, current slice" branch the window is
 * `... + index - 1`, EXCLUDING the block immediately before this one (it is
 * about to be overwritten by the very block being computed and is already
 * covered as "prev" elsewhere) -- an off-by-one here is invisible whenever
 * there is only one lane, because with p=1 same_lane is always true and the
 * "other lane" branches (which use a *different* formula, without the -1
 * exclusion in the same place) never run at all. This file's OpenSSL
 * differential deliberately runs multi-lane cases (see the -negctl/-openssl
 * make rule at the bottom of this file) for exactly that reason; the RFC
 * vectors alone (always p=4) already exercise both branches too, but a
 * differential across VARYING lane counts is what would catch a window bug
 * that happened to cancel out at p=4 specifically. */
static uint32_t argon2_index_alpha(uint32_t pass, uint32_t slice, uint32_t index,
                                    uint32_t lane_length, uint32_t segment_length,
                                    uint32_t pseudo_rand, int same_lane)
{
    uint32_t reference_area_size;
    uint64_t relative_position;
    uint32_t start_position, absolute_position;

    if (pass == 0) {
        if (slice == 0) {
            reference_area_size = index - 1;                 /* all but the previous */
        } else if (same_lane) {
            reference_area_size = slice * segment_length + index - 1;
        } else {
            /* Unsigned wraparound is intentional and matches the reference
             * C exactly: at index 0 in a lane other than our own, the
             * window is "one less than" slice*segment_length, computed as
             * slice*segment_length + (uint32_t)-1, which wraps to
             * slice*segment_length - 1 in exactly the same arithmetic a
             * signed computation would give -- written this way because the
             * reference implementation is, and a transcription "fix" that
             * replaces it with a signed subtraction is the bug. */
            reference_area_size = slice * segment_length + (index == 0 ? (uint32_t)-1 : 0);
        }
    } else if (same_lane) {
        reference_area_size = lane_length - segment_length + index - 1;
    } else {
        reference_area_size = lane_length - segment_length + (index == 0 ? (uint32_t)-1 : 0);
    }

    relative_position = pseudo_rand;
    relative_position = (relative_position * relative_position) >> 32;
    relative_position = reference_area_size - 1 -
        (((uint64_t)reference_area_size * relative_position) >> 32);

    start_position = 0;
    if (pass != 0)
        start_position = (slice == ARGON2_SYNC_POINTS - 1) ? 0 : (slice + 1) * segment_length;

    absolute_position = (uint32_t)(((uint64_t)start_position + relative_position) % lane_length);
    return absolute_position;
}

/* One (pass, slice, lane) segment: RFC 9106 3.4's fill_segment. */
static void argon2_fill_segment(enum argon2_type type,
                                 struct argon2_block *mem,
                                 uint32_t lanes, uint32_t segment_length,
                                 uint32_t lane_length,
                                 uint32_t pass, uint32_t slice, uint32_t lane,
                                 uint32_t total_passes)
{
    uint32_t memory_blocks = lane_length * lanes;

#ifdef ARGON2_NEGCTL_ALWAYS_DATA_DEP
    /* NEGATIVE CONTROL. "Getting the switch point wrong... or never" -- this
     * file's own header, made real: force data-dependent (Argon2d-style)
     * addressing for EVERY (pass, slice, type), i.e. Argon2i and Argon2id
     * lose their independent-addressing phase entirely and become Argon2d in
     * every respect except the value tagged into H0 (the `type` field, which
     * this control does not touch). Argon2d's own RFC vector is UNCHANGED by
     * this flag -- Argon2d never used the independent generator to begin
     * with -- so the control's own honesty check is that Argon2d still
     * passes while Argon2i and Argon2id do not; see argon2_test.c. */
    int data_independent = 0;
#else
    int data_independent =
        (type == ARGON2_I) ||
        (type == ARGON2_ID && pass == 0 && slice < ARGON2_SYNC_POINTS / 2);
#endif

    struct argon2_block address_block, input_block, zero_block;
    uint32_t starting_index = 0;

    if (data_independent) {
        for (int i = 0; i < ARGON2_QWORDS; i++) { zero_block.v[i] = 0; input_block.v[i] = 0; }
        input_block.v[0] = pass;
        input_block.v[1] = lane;
        input_block.v[2] = slice;
        input_block.v[3] = memory_blocks;
        input_block.v[4] = total_passes;
        input_block.v[5] = (uint32_t)type;
        /* v[6] is the address-block counter; argon2_next_addresses increments
         * it itself before each use. v[7] is unused, left 0. */
    }

    if (pass == 0 && slice == 0) {
        starting_index = 2;   /* the first two blocks of each lane are already filled */
        if (data_independent)
            argon2_next_addresses(&address_block, &input_block, &zero_block);
    }

    uint32_t curr_offset = lane * lane_length + slice * segment_length + starting_index;
    uint32_t prev_offset = (curr_offset % lane_length == 0)
        ? curr_offset + lane_length - 1
        : curr_offset - 1;

    for (uint32_t i = starting_index; i < segment_length; i++, curr_offset++, prev_offset++) {
        if (curr_offset % lane_length == 1) prev_offset = curr_offset - 1;

        uint64_t pseudo_rand;
        if (data_independent) {
            if (i % ARGON2_QWORDS == 0)
                argon2_next_addresses(&address_block, &input_block, &zero_block);
            pseudo_rand = address_block.v[i % ARGON2_QWORDS];
        } else {
            /* Data-DEPENDENT addressing: the reference index comes from the
             * PREVIOUS block's own content, which is a function of the
             * password. This is the memory-access-pattern leak Argon2i
             * exists to avoid and Argon2id accepts for 3 of its 4 first-pass
             * slices and all later passes -- see argon2.h's constant-time
             * note. */
            pseudo_rand = mem[prev_offset].v[0];
        }

        uint32_t ref_lane = (uint32_t)(pseudo_rand >> 32) % lanes;
        if (pass == 0 && slice == 0) ref_lane = lane;   /* no other lane has anything yet */

        int same_lane = (ref_lane == lane);
        uint32_t ref_index = argon2_index_alpha(pass, slice, i, lane_length, segment_length,
                                                 (uint32_t)pseudo_rand, same_lane);

        struct argon2_block *ref_block = &mem[lane_length * ref_lane + ref_index];
        struct argon2_block *curr_block = &mem[curr_offset];
        /* Version 0x13 only (see this file's top comment): overwrite on the
         * first pass, XOR into the existing block on every pass after it. */
        argon2_fill_block(&mem[prev_offset], ref_block, curr_block, pass != 0);
    }
}

/* ---------------------------------------------------------------- H0 --
 * RFC 9106 3.2's initial hash: BLAKE2b-512 over every parameter and input in
 * a fixed order, each preceded by its own LE32 length -- including for
 * pieces that might be empty (secret, associated data), whose LE32(0) is
 * still hashed even though no bytes follow. Skipping the length word for an
 * empty field would make "secret absent" and "secret present but empty"
 * hash identically to a shorter message that happened to have the same
 * bytes shifted over -- a length-prefixing bug, the oldest kind there is. */
static void argon2_initial_hash(uint8_t h0[64],
    uint32_t lanes, uint32_t taglen, uint32_t m_cost_kib, uint32_t t_cost,
    uint32_t version, uint32_t type,
    const uint8_t *pw, uint32_t pwlen,
    const uint8_t *salt, uint32_t saltlen,
    const uint8_t *secret, uint32_t secretlen,
    const uint8_t *ad, uint32_t adlen)
{
    struct b2 s;
    uint8_t v4[4];
    b2_init(&s, 64);

    store32_le(v4, lanes);       b2_update(&s, v4, 4);
    store32_le(v4, taglen);      b2_update(&s, v4, 4);
    store32_le(v4, m_cost_kib);  b2_update(&s, v4, 4);
    store32_le(v4, t_cost);      b2_update(&s, v4, 4);
    store32_le(v4, version);     b2_update(&s, v4, 4);
    store32_le(v4, type);        b2_update(&s, v4, 4);

    store32_le(v4, pwlen);       b2_update(&s, v4, 4);
    if (pwlen && pw) b2_update(&s, pw, pwlen);

    store32_le(v4, saltlen);     b2_update(&s, v4, 4);
    if (saltlen && salt) b2_update(&s, salt, saltlen);

    store32_le(v4, secretlen);   b2_update(&s, v4, 4);
    if (secretlen && secret) b2_update(&s, secret, secretlen);

    store32_le(v4, adlen);       b2_update(&s, v4, 4);
    if (adlen && ad) b2_update(&s, ad, adlen);

    b2_final(&s, h0, 64);
}

static void load_block(struct argon2_block *b, const uint8_t *bytes)
{
    for (int i = 0; i < ARGON2_QWORDS; i++) b->v[i] = load64_le(bytes + 8 * i);
}

static void store_block(uint8_t *bytes, const struct argon2_block *b)
{
    for (int i = 0; i < ARGON2_QWORDS; i++) store64_le(bytes + 8 * i, b->v[i]);
}

#ifdef ARGON2_TEST_HOOKS
void (*argon2_test_after_pass)(uint32_t pass, const struct argon2_block *mem,
                                uint32_t lane_length, uint32_t lanes) = 0;
#endif

/* -------------------------------------------------------------- public API --
 */

uint32_t argon2_memory_blocks(uint32_t m_cost_kib, uint32_t lanes)
{
    if (lanes == 0) return 0;
    uint32_t m = m_cost_kib;
    uint32_t min_m = 2 * ARGON2_SYNC_POINTS * lanes;   /* RFC 9106's floor */
    if (m < min_m) m = min_m;
    uint32_t segment_length = m / (lanes * ARGON2_SYNC_POINTS);
    return segment_length * (lanes * ARGON2_SYNC_POINTS);
}

int argon2(enum argon2_type type,
           const uint8_t *pw, uint32_t pwlen,
           const uint8_t *salt, uint32_t saltlen,
           const uint8_t *secret, uint32_t secretlen,
           const uint8_t *ad, uint32_t adlen,
           uint32_t t_cost, uint32_t m_cost_kib, uint32_t lanes,
           struct argon2_block *mem, uint32_t mem_cap,
           uint8_t *tag, uint32_t taglen)
{
    if (lanes == 0 || t_cost == 0 || saltlen < 8 || taglen < 4) return -1;

    uint32_t memory_blocks = argon2_memory_blocks(m_cost_kib, lanes);
    if (memory_blocks == 0 || mem_cap < memory_blocks) return -2;

    uint32_t segment_length = memory_blocks / (lanes * ARGON2_SYNC_POINTS);
    uint32_t lane_length = segment_length * ARGON2_SYNC_POINTS;

    /* --- 1. H0, then the first two blocks of every lane (RFC 9106 3.2) --- */
    uint8_t blockhash[64 + 8];    /* H0 || LE32(block index) || LE32(lane) */
    argon2_initial_hash(blockhash, lanes, taglen, m_cost_kib, t_cost,
                         ARGON2_VERSION_13, (uint32_t)type,
                         pw, pwlen, salt, saltlen, secret, secretlen, ad, adlen);
    for (int i = 64; i < 72; i++) blockhash[i] = 0;

    uint8_t blockbytes[ARGON2_BLOCK_SIZE];
    for (uint32_t l = 0; l < lanes; l++) {
        store32_le(blockhash + 64, 0);
        store32_le(blockhash + 68, l);
        argon2_hprime(blockbytes, ARGON2_BLOCK_SIZE, blockhash, 72);
        load_block(&mem[l * lane_length + 0], blockbytes);

        store32_le(blockhash + 64, 1);
        argon2_hprime(blockbytes, ARGON2_BLOCK_SIZE, blockhash, 72);
        load_block(&mem[l * lane_length + 1], blockbytes);
    }
    crypto_wipe(blockbytes, sizeof blockbytes);
    crypto_wipe(blockhash, sizeof blockhash);

    /* --- 2. Fill every remaining block, pass by pass, slice by slice --- */
    for (uint32_t pass = 0; pass < t_cost; pass++) {
        for (uint32_t slice = 0; slice < ARGON2_SYNC_POINTS; slice++)
            for (uint32_t lane = 0; lane < lanes; lane++)
                argon2_fill_segment(type, mem, lanes, segment_length, lane_length,
                                     pass, slice, lane, t_cost);
#ifdef ARGON2_TEST_HOOKS
        /* Test-only seam, compiled in ONLY when a test build defines
         * ARGON2_TEST_HOOKS (see the -negctl/-openssl make rule at the
         * bottom of this file): after finishing pass N, hand the whole
         * memory array to a callback the test installs, so it can compare
         * per-pass intermediate block contents against the PHC reference
         * implementation's own KAT dump -- not just the tag at the very
         * end, which XORs every lane's last block together and could in
         * principle let two distinct bugs cancel out where a per-block
         * comparison cannot. Absent the define, this is dead code with a
         * null-checked function pointer; absent even that, it does not
         * exist -- no consumer of this file pays for it. */
        if (argon2_test_after_pass) argon2_test_after_pass(pass, mem, lane_length, lanes);
#endif
    }

    /* --- 3. XOR the last block of every lane, then H' it into the tag --- */
    struct argon2_block result = mem[lane_length - 1];
    for (uint32_t l = 1; l < lanes; l++) {
        struct argon2_block *b = &mem[l * lane_length + lane_length - 1];
        for (int i = 0; i < ARGON2_QWORDS; i++) result.v[i] ^= b->v[i];
    }

    uint8_t resultbytes[ARGON2_BLOCK_SIZE];
    store_block(resultbytes, &result);
    argon2_hprime(tag, taglen, resultbytes, ARGON2_BLOCK_SIZE);

    crypto_wipe(resultbytes, sizeof resultbytes);
    crypto_wipe(&result, sizeof result);
    crypto_wipe(mem, (size_t)memory_blocks * sizeof(struct argon2_block));
    return 0;
}
