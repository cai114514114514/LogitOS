#include "crypto.h"
#include "aes_backend.h"
#include "aes_gcm_siv.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

/* AES-GCM-SIV (RFC 8452). NOT WIRED INTO ANYTHING -- see aes_gcm_siv.h. This
 * file is the construction (key derivation, POLYVAL driver, CTR with GCM-SIV's
 * own counter convention, tag computation, decrypt-then-verify) built on top
 * of aes_current_backend()'s key_expand/encrypt. Per rule 2 in this tree
 * ("the mode never lives in a backend"), it consumes the backend's AES block
 * primitive exactly the way aesgcm.c and chacha20poly1305.c consume theirs,
 * and it does NOT touch aes_backend.h's gf_mul -- that is GCM's GHASH
 * multiply, in GCM's bit convention, and reusing it here is the mistake this
 * file exists to avoid. Read on for why.
 *
 * ============================================================================
 * POLYVAL IS NOT GHASH, AND THE DIFFERENCE IS A BYTE-ORDER MIRROR PLUS A
 * TRAILING *x. THIS IS THE TRAP THE WHOLE FILE IS BUILT AROUND.
 *
 * GHASH (aesgcm.c's ghash()/c_gf_mul()) uses the "GCM bit convention": byte 0
 * BIT 7 (the MSB) is the coefficient of x^0. POLYVAL (RFC 8452 section 3)
 * uses the natural convention: byte 0 BIT 0 (the LSB) is the coefficient of
 * x^0, and bit k of the 16-byte string is the coefficient of x^k -- which
 * means a POLYVAL field element, read as bytes, IS a little-endian 128-bit
 * integer over GF(2). (Confirmed against RFC 8452's own worked example: the
 * value of x^-128 = x^127+x^124+x^121+x^114+1 is given in the RFC as bytes
 * 01 00..00 04 92, and 0x92 = 1001_0010b sets exactly bits 7,4,1 of byte 15 --
 * i.e. x^(8*15+7), x^(8*15+4), x^(8*15+1) = x^127, x^124, x^121, plus byte 0
 * bit 0 = x^0. That is the whole point of designing it this way: on a
 * little-endian machine with PCLMULQDQ, a POLYVAL element needs NO bit
 * reversal to feed the instruction, unlike GHASH.)
 *
 * The two are related by dot(a,b) = ByteReverse(GHASH(ByteReverse(a)*x,
 * ByteReverse(b))) (RFC 8452 Appendix A) -- note the *x, which is the part
 * that gets dropped by anyone who tries to shortcut this with a plain
 * byte-reverse wrapper around aes_backend.h's gf_mul. Rather than risk that,
 * this file implements POLYVAL's field arithmetic from scratch, in POLYVAL's
 * own convention, and is checked against RFC 8452 section 7's standalone
 * dot(a,b) example directly (tests/unit/aes_gcm_siv_test.c) -- not against a
 * transform of the GHASH vectors, so an error in the transform can't hide.
 *
 * poly_mul_raw() does a branchless (mask-selected, not branch-selected)
 * carry-less schoolbook multiply into a 256-bit product; reduce256() folds
 * that mod P(x) = x^128+x^127+x^126+x^121+1, top bit down, also branchless.
 * dot(a,b) = (a*b)*x^-128 is then two calls to that reduced multiply -- the
 * RFC defines "*" as already including the reduction (section 3: "the
 * product ... is calculated using standard polynomial multiplication
 * followed by reduction"), so dot() is not a single fused operation here, it
 * is exactly the two multiplications the definition names. Slower than a
 * fused single-pass reduction would be, but every step is checkable against
 * the RFC's own a*b AND dot(a,b) figures independently (see the test), which
 * a fused implementation would not offer for free.
 * ============================================================================ */

#define AES_RK_MAX 240   /* 4 * (14 + 1) * 4, same bound aesgcm.c uses */

/* Standard FIPS-197 round counts. Duplicated from aesgcm.c's static
 * aes_rounds() rather than shared: this is a fixed mathematical fact about
 * AES (10/12/14 rounds for 128/192/256-bit keys) that cannot drift the way a
 * policy constant could, so the "one jar two doors" risk that motivates
 * sharing elsewhere does not apply the same way here. GCM-SIV itself is only
 * defined for 128 and 256 bit keys (RFC 8452 section 4: "If the
 * key-generating key is 16 bytes long, then AES-128 is used throughout.
 * Otherwise, AES-256 is used throughout") -- no 192-bit GCM-SIV exists. */
static int aes_rounds(int keylen) { return keylen == 32 ? 14 : 10; }

/* --- POLYVAL's own GF(2^128) arithmetic (RFC 8452 section 3) --------------
 * Convention: coefficient of x^k is bit (k & 7) of byte (k >> 3) -- i.e. the
 * 16 bytes read as a little-endian 128-bit integer. See the file header for
 * why this is NOT aes_backend.h's gf_mul convention. */

/* out[byte_off+j] / out[byte_off+j+1] each receive at most two contributions
 * across the whole 16-byte loop (the low part of in[j] and the high part of
 * in[j-1]), so XOR-accumulating them independently reconstructs exactly
 * `in` shifted left by `shift` bits, 0 <= shift <= 127, into a 32-byte field.
 * This is the shift-in-place step of the schoolbook multiply below; kept
 * separate because it is also exactly what the reduction step needs. */
static void shl_xor(uint8_t out[32], const uint8_t in[16], int shift)
{
    int byte_off = shift >> 3, bit = shift & 7;
    for (int j = 0; j < 16; j++) {
        uint16_t t = (uint16_t)((uint16_t)in[j] << bit);
        out[byte_off + j]     ^= (uint8_t)(t & 0xff);
        out[byte_off + j + 1] ^= (uint8_t)(t >> 8);
    }
}

/* Raw (unreduced) carry-less multiply: out[32] gets the up-to-255-degree
 * product of a and b. Touches SECRET material whenever this runs on the
 * authentication key H, so the bit test is branchless (mask = 0x00 or 0xff,
 * never a taken/not-taken branch on a secret bit) and the shift-accumulate
 * touches all 32 output bytes on every one of the 128 iterations regardless
 * of the bit's value -- no secret-dependent memory access pattern and no
 * secret-dependent control flow. This is what "constant time" means for this
 * function; it is stronger than aes_backend.h's c_gf_mul (aesgcm.c), which is
 * documented NOT constant-time because it branches on the accumulator bit. */
static void poly_mul_raw(const uint8_t a[16], const uint8_t b[16], uint8_t out[32])
{
    memset(out, 0, 32);
    uint8_t shifted[32];
    for (int i = 0; i < 128; i++) {
        uint8_t abit = (uint8_t)((a[i >> 3] >> (i & 7)) & 1);
        uint8_t mask = (uint8_t)(0 - abit);           /* 0x00 or 0xff, branchless */
        memset(shifted, 0, 32);
        shl_xor(shifted, b, i);
        for (int k = 0; k < 32; k++) out[k] ^= (uint8_t)(shifted[k] & mask);
    }
}

/* x^128 mod P(x) = x^127+x^126+x^121+x^0 (since P(x) = x^128+x^127+x^126+
 * x^121+1 = 0). Bytes: bit 0 -> byte0=0x01; bits 121,126,127 all fall in
 * byte 15 (121=8*15+1 -> 0x02, 126=8*15+6 -> 0x40, 127=8*15+7 -> 0x80,
 * sum 0xc2). */
static const uint8_t POLY_R128[16] = {
    0x01,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0xc2
};

/* Reduce a 256-bit raw product mod P(x), top bit down. For k = 255..128: if
 * bit k is set, clear it (XOR is self-inverse: XOR-ing bit k with itself
 * clears it, a branchless "clear if set") and XOR in (x^128 mod P) shifted
 * left by (k-128) -- which only ever sets bits strictly below k (POLY_R128's
 * highest term is x^127, so shifted by at most 127 it reaches at most
 * x^254), so processing top-down never re-dirties a bit already cleared.
 * This is the standard schoolbook GF(2^m) reduction; branchless throughout
 * for the same reason poly_mul_raw is. */
static void reduce256(uint8_t v[32], uint8_t out[16])
{
    for (int k = 255; k >= 128; k--) {
        uint8_t bitval = (uint8_t)((v[k >> 3] >> (k & 7)) & 1);
        v[k >> 3] = (uint8_t)(v[k >> 3] ^ (bitval << (k & 7)));   /* clear bit k */
        int shift = k - 128;
        int byte_off = shift >> 3, bit = shift & 7;
        uint8_t mask = (uint8_t)(0 - bitval);
        for (int j = 0; j < 16; j++) {
            uint16_t t = (uint16_t)((uint16_t)POLY_R128[j] << bit);
            v[byte_off + j]     ^= (uint8_t)((t & 0xff) & mask);
            v[byte_off + j + 1] ^= (uint8_t)((t >> 8) & mask);
        }
    }
    memcpy(out, v, 16);
}

/* The RFC's "*": raw multiply then reduce mod P(x). Verified directly
 * against RFC 8452 section 7's worked a*b figure in the test -- an
 * independent check of the reduction on top of the multiply check dot()
 * gets from the same section. */
static void gf_mulmod(const uint8_t a[16], const uint8_t b[16], uint8_t out[16])
{
    uint8_t raw[32];
    poly_mul_raw(a, b, raw);
    reduce256(raw, out);
}

/* x^-128 = x^127+x^124+x^121+x^114+1, RFC 8452 section 3/7's own example of
 * how a field element is written as 16 bytes. */
static const uint8_t POLY_XINV128[16] = {
    0x01,0,0,0, 0,0,0,0, 0,0,0,0, 0,0, 0x04, 0x92
};

/* dot(a,b) = a*b*x^-128 (RFC 8452 section 3). */
static void gf_dot(const uint8_t a[16], const uint8_t b[16], uint8_t out[16])
{
    uint8_t ab[16];
    gf_mulmod(a, b, ab);
    gf_mulmod(ab, POLY_XINV128, out);
}

void polyval(const uint8_t H[16], const uint8_t *blocks, int nblocks, uint8_t out[16])
{
    uint8_t S[16]; memset(S, 0, 16);
    for (int b = 0; b < nblocks; b++) {
        uint8_t sum[16];
        for (int i = 0; i < 16; i++) sum[i] = (uint8_t)(S[i] ^ blocks[b*16 + i]);
        gf_dot(sum, H, S);
    }
    memcpy(out, S, 16);
}

/* --- key derivation (RFC 8452 section 4) -----------------------------------
 * THE TRAP: only the LOW 8 BYTES of each AES-encrypted counter block are
 * kept. Taking a whole 16-byte block per key-material chunk gives keys of
 * the wrong length silently (well-formed AES output, wrong derivation) --
 * there is no error to catch it, only a KAT mismatch, which is exactly why
 * this function is checked against all 50 RFC vectors and not just one.
 *
 * CONSTANT TIME, PER-SITE (rule 3): `kgk` is the top-level secret this whole
 * AEAD is keyed by, and every byte this function touches is either that key
 * or material derived from it. POLYVAL's own arithmetic above is
 * branch-free and access-pattern-free by construction (see poly_mul_raw's
 * comment). The AES block cipher underneath it is NOT, in general: it is
 * whatever aes_current_backend() selected, and aes_backend.h documents that
 * only the AES-NI backend is constant-time -- the portable 'c' backend
 * (aesgcm.c) indexes a secret-dependent S-box and is the one that runs on
 * any host without AES-NI, including every non-x86 build of this tree. That
 * caveat is inherited here unchanged, not hidden: this file adds no new
 * secret-dependent branch or memory access on top of whatever the selected
 * backend already has, and does not claim more than the backend gives it. */
static void derive_keys(const uint8_t *kgk, int kgklen, const uint8_t nonce[12],
                        uint8_t mak[16], uint8_t mek[32] /* first 16 or all 32 used */)
{
    const struct aes_backend *be = aes_current_backend();
    int nr = aes_rounds(kgklen);
    uint8_t rk[AES_RK_MAX]; be->key_expand(kgk, kgklen, rk);
    int nblocks = (kgklen == 32) ? 6 : 4;
    uint8_t material[48]; /* 6 * 8 bytes, max case (AES-256) */
    for (int c = 0; c < nblocks; c++) {
        uint8_t blk[16], ks[16];
        blk[0] = (uint8_t)c; blk[1] = 0; blk[2] = 0; blk[3] = 0;   /* LE uint32 counter */
        memcpy(blk + 4, nonce, 12);
        be->encrypt(rk, nr, blk, ks);
        memcpy(material + c*8, ks, 8);              /* keep only the low 8 bytes */
        crypto_wipe(ks, sizeof ks);
    }
    memcpy(mak, material, 16);
    memcpy(mek, material + 16, (size_t)(kgklen == 32 ? 32 : 16));
    crypto_wipe(rk, sizeof rk);
    crypto_wipe(material, sizeof material);
}

/* --- CTR with GCM-SIV's OWN counter convention -----------------------------
 * RFC 8452 section 4: "advances by incrementing the first 32 bits
 * interpreted as an UNSIGNED, LITTLE-ENDIAN integer, wrapping at 2^32." This
 * is NOT aesgcm.c's inc32 (SP 800-38D: big-endian, low 4 bytes of a 16-byte
 * BIG-endian-conventioned block). Appendix C.3 exists specifically to catch
 * a big-endian increment here -- it constructs an all-zero counter that must
 * wrap 0xffffffff -> 0x00000000 within four bytes, and a big-endian
 * increment would carry into byte 12 instead of wrapping, producing a
 * different (wrong) keystream from block 2^32 onward relative to block 0. */
static void siv_ctr(const uint8_t *mek, int meklen, const uint8_t icb[16],
                    const uint8_t *in, int len, uint8_t *out)
{
    const struct aes_backend *be = aes_current_backend();
    int nr = aes_rounds(meklen);
    uint8_t rk[AES_RK_MAX]; be->key_expand(mek, meklen, rk);
    uint8_t block[16]; memcpy(block, icb, 16);
    uint8_t ks[16];
    for (int off = 0; off < len; off += 16) {
        be->encrypt(rk, nr, block, ks);
        int n = len - off; if (n > 16) n = 16;
        for (int i = 0; i < n; i++) out[off+i] = (uint8_t)(in[off+i] ^ ks[i]);
        uint32_t c = (uint32_t)block[0] | ((uint32_t)block[1]<<8) |
                    ((uint32_t)block[2]<<16) | ((uint32_t)block[3]<<24);
        c++;                                          /* wraps at 2^32 by construction */
        block[0] = (uint8_t)c; block[1] = (uint8_t)(c>>8);
        block[2] = (uint8_t)(c>>16); block[3] = (uint8_t)(c>>24);
    }
    crypto_wipe(ks, sizeof ks);
    crypto_wipe(rk, sizeof rk);
}

/* --- the AEAD (RFC 8452 sections 4-5) --------------------------------------
 * X_1..X_s fed to POLYVAL: zero-padded AAD blocks, then zero-padded
 * plaintext blocks, then one length block (bytelen(aad)*8 and
 * bytelen(pt)*8, each a 64-bit little-endian integer). `blocks16` below is
 * that whole sequence, materialised contiguously so polyval() (one loop, no
 * special-casing of the two differently-padded halves) can consume it in one
 * call -- the same shape aesgcm.c's ghash() uses for AAD-then-ciphertext. */
#define SIV_MAXBLOCKS 8   /* enough for every RFC vector (max input 64B AAD + 64B PT) and the KAT harness; a caller needing more calls polyval() directly */

static int build_blocks(const uint8_t *aad, int aadlen, const uint8_t *msg, int msglen,
                        uint8_t *blocks /* SIV_MAXBLOCKS*16, caller-owned */)
{
    int nb = 0;
    int aad_pad = (aadlen + 15) & ~15;
    int msg_pad = (msglen + 15) & ~15;
    if (aad_pad + msg_pad + 16 > SIV_MAXBLOCKS * 16) return -1;   /* caller's buffer too small */
    memset(blocks, 0, (size_t)(aad_pad + msg_pad + 16));
    memcpy(blocks, aad, (size_t)aadlen);
    nb = aad_pad / 16;
    memcpy(blocks + aad_pad, msg, (size_t)msglen);
    nb += msg_pad / 16;
    uint8_t *lenblk = blocks + aad_pad + msg_pad;
    uint64_t ab = (uint64_t)aadlen * 8, mb = (uint64_t)msglen * 8;
    for (int i = 0; i < 8; i++) lenblk[i]   = (uint8_t)(ab >> (8*i));   /* little-endian */
    for (int i = 0; i < 8; i++) lenblk[8+i] = (uint8_t)(mb >> (8*i));
    nb += 1;
    return nb;
}

static void siv_seal(const uint8_t *key, int keylen, const uint8_t nonce[12],
                     const uint8_t *aad, int aadlen, const uint8_t *pt, int len,
                     uint8_t *ct, uint8_t tag[16])
{
    uint8_t mak[16], mek[32];
    derive_keys(key, keylen, nonce, mak, mek);

    uint8_t blocks[SIV_MAXBLOCKS * 16];
    int nb = build_blocks(aad, aadlen, pt, len, blocks);
    uint8_t S[16];
    if (nb < 0) {
        /* Oversized input for the static scratch buffer: POLYVAL directly
         * over AAD, then PT, then the length block, without materialising
         * them contiguously first -- three polyval "extends" chained by
         * hand, matching the same S_0=0; S_j=dot(S_{j-1}+X_j,H) iteration,
         * just fed incrementally. Kept as the fallback path (not the
         * default) because it is unexercised by any RFC vector; every
         * vector fits SIV_MAXBLOCKS, so this branch has no KAT covering it
         * and should not be trusted to the same degree as the path above. */
        uint8_t S0[16]; memset(S0, 0, 16);
        uint8_t tmp[16];
        int off = 0;
        while (off < aadlen) {
            memset(tmp, 0, 16);
            int n = aadlen - off; if (n > 16) n = 16;
            memcpy(tmp, aad + off, (size_t)n);
            for (int i = 0; i < 16; i++) tmp[i] ^= S0[i];
            gf_dot(tmp, mak, S0);
            off += 16;
        }
        off = 0;
        while (off < len) {
            memset(tmp, 0, 16);
            int n = len - off; if (n > 16) n = 16;
            memcpy(tmp, pt + off, (size_t)n);
            for (int i = 0; i < 16; i++) tmp[i] ^= S0[i];
            gf_dot(tmp, mak, S0);
            off += 16;
        }
        uint8_t lenblk[16];
        uint64_t ab = (uint64_t)aadlen * 8, mb = (uint64_t)len * 8;
        for (int i = 0; i < 8; i++) lenblk[i]   = (uint8_t)(ab >> (8*i));
        for (int i = 0; i < 8; i++) lenblk[8+i] = (uint8_t)(mb >> (8*i));
        for (int i = 0; i < 16; i++) lenblk[i] ^= S0[i];
        gf_dot(lenblk, mak, S0);
        memcpy(S, S0, 16);
    } else {
        polyval(mak, blocks, nb, S);
    }

    /* XOR the first twelve bytes of S with the nonce; clear the MSB of the
     * last byte. THE TRAP: forgetting the clear makes exactly half of all
     * messages (those where that bit happened to be 0 already) encrypt
     * "correctly" and the other half silently wrong -- it reads as
     * flakiness, not a bug, which is why it is worth naming rather than
     * trusting a KAT to be the only thing standing between this and
     * shipping.
     *
     * SIV_CTL_NO_TAG_MASK (negative control, tests/unit/run-aes-gcm-siv-negctl.sh):
     * skip the clear, i.e. run exactly the described trap. Both siv_seal and
     * siv_open have this line, so the encrypt/decrypt round trip inside a
     * single process stays internally self-consistent either way -- ONLY a
     * comparison against an independently-produced answer (the RFC vectors)
     * can see this defect, which is exactly the point rule 1 makes about
     * unwatchable controls: a round-trip check alone would pass. */
    uint8_t Sx[16]; memcpy(Sx, S, 16);
    for (int i = 0; i < 12; i++) Sx[i] = (uint8_t)(Sx[i] ^ nonce[i]);
#ifndef SIV_CTL_NO_TAG_MASK
    Sx[15] &= 0x7f;
#endif

    const struct aes_backend *be = aes_current_backend();
    int nr = aes_rounds(keylen);
    uint8_t rk[AES_RK_MAX]; be->key_expand(mek, keylen, rk);
    be->encrypt(rk, nr, Sx, tag);
    crypto_wipe(rk, sizeof rk);

    /* Initial counter block: the tag with the MSB of the last byte SET (the
     * mirror image of the clear above -- easy to reverse the two by
     * accident since they are one line apart in the RFC and in this file). */
    uint8_t icb[16]; memcpy(icb, tag, 16); icb[15] |= 0x80;
    siv_ctr(mek, keylen, icb, pt, len, ct);

    crypto_wipe(mak, sizeof mak); crypto_wipe(mek, sizeof mek);
    crypto_wipe(S, sizeof S); crypto_wipe(Sx, sizeof Sx);
    crypto_wipe(blocks, sizeof blocks);
}

static int siv_open(const uint8_t *key, int keylen, const uint8_t nonce[12],
                    const uint8_t *aad, int aadlen, const uint8_t *ct, int len,
                    const uint8_t tag[16], uint8_t *pt)
{
    uint8_t mak[16], mek[32];
    derive_keys(key, keylen, nonce, mak, mek);

    /* Decrypt FIRST using the caller-supplied (untrusted) tag as the initial
     * counter block -- GCM-SIV, unlike GCM, has no way to check the tag
     * before decrypting, because the tag IS derived from the plaintext via
     * POLYVAL, and the plaintext is exactly what decryption produces. So the
     * order here is: decrypt, recompute POLYVAL over the RECOVERED
     * plaintext, recompute the expected tag, and only THEN compare -- never
     * trust `pt` until the compare below succeeds. */
    uint8_t icb[16]; memcpy(icb, tag, 16); icb[15] |= 0x80;
    siv_ctr(mek, keylen, icb, ct, len, pt);

    uint8_t blocks[SIV_MAXBLOCKS * 16];
    int nb = build_blocks(aad, aadlen, pt, len, blocks);
    uint8_t S[16];
    if (nb < 0) {
        /* Same oversized-input fallback as siv_seal; see the comment there. */
        uint8_t S0[16]; memset(S0, 0, 16);
        uint8_t tmp[16]; int off = 0;
        while (off < aadlen) {
            memset(tmp, 0, 16); int n = aadlen - off; if (n > 16) n = 16;
            memcpy(tmp, aad + off, (size_t)n);
            for (int i = 0; i < 16; i++) tmp[i] ^= S0[i];
            gf_dot(tmp, mak, S0); off += 16;
        }
        off = 0;
        while (off < len) {
            memset(tmp, 0, 16); int n = len - off; if (n > 16) n = 16;
            memcpy(tmp, pt + off, (size_t)n);
            for (int i = 0; i < 16; i++) tmp[i] ^= S0[i];
            gf_dot(tmp, mak, S0); off += 16;
        }
        uint8_t lenblk[16];
        uint64_t ab = (uint64_t)aadlen * 8, mb = (uint64_t)len * 8;
        for (int i = 0; i < 8; i++) lenblk[i]   = (uint8_t)(ab >> (8*i));
        for (int i = 0; i < 8; i++) lenblk[8+i] = (uint8_t)(mb >> (8*i));
        for (int i = 0; i < 16; i++) lenblk[i] ^= S0[i];
        gf_dot(lenblk, mak, S0);
        memcpy(S, S0, 16);
    } else {
        polyval(mak, blocks, nb, S);
    }

    uint8_t Sx[16]; memcpy(Sx, S, 16);
    for (int i = 0; i < 12; i++) Sx[i] = (uint8_t)(Sx[i] ^ nonce[i]);
#ifndef SIV_CTL_NO_TAG_MASK
    Sx[15] &= 0x7f;
#endif

    const struct aes_backend *be = aes_current_backend();
    int nr = aes_rounds(keylen);
    uint8_t rk[AES_RK_MAX]; be->key_expand(mek, keylen, rk);
    uint8_t expected[16]; be->encrypt(rk, nr, Sx, expected);
    crypto_wipe(rk, sizeof rk);

    /* Constant-time compare (rule 3): accumulate the XOR of every byte
     * rather than returning on the first mismatch, same shape aesgcm.c's
     * gcm_open and chacha20poly1305.c's open both use. */
    int diff = 0;
    for (int i = 0; i < 16; i++) diff |= expected[i] ^ tag[i];

    crypto_wipe(mak, sizeof mak); crypto_wipe(mek, sizeof mek);
    crypto_wipe(S, sizeof S); crypto_wipe(Sx, sizeof Sx); crypto_wipe(expected, sizeof expected);
    crypto_wipe(blocks, sizeof blocks);

    if (diff) {
        crypto_wipe(pt, (size_t)len);   /* never hand back unauthenticated plaintext */
        return -1;
    }
    return 0;
}

void aes128_gcm_siv_seal(const uint8_t key[16], const uint8_t nonce[12],
                         const uint8_t *aad, int aadlen,
                         const uint8_t *pt, int len, uint8_t *ct, uint8_t tag[16])
{
    if (aadlen < 0 || len < 0) return;
    siv_seal(key, 16, nonce, aad, aadlen, pt, len, ct, tag);
}

int aes128_gcm_siv_open(const uint8_t key[16], const uint8_t nonce[12],
                        const uint8_t *aad, int aadlen,
                        const uint8_t *ct, int len, const uint8_t tag[16], uint8_t *pt)
{
    if (aadlen < 0 || len < 0) return -1;
    return siv_open(key, 16, nonce, aad, aadlen, ct, len, tag, pt);
}

void aes256_gcm_siv_seal(const uint8_t key[32], const uint8_t nonce[12],
                         const uint8_t *aad, int aadlen,
                         const uint8_t *pt, int len, uint8_t *ct, uint8_t tag[16])
{
    if (aadlen < 0 || len < 0) return;
    siv_seal(key, 32, nonce, aad, aadlen, pt, len, ct, tag);
}

int aes256_gcm_siv_open(const uint8_t key[32], const uint8_t nonce[12],
                        const uint8_t *aad, int aadlen,
                        const uint8_t *ct, int len, const uint8_t tag[16], uint8_t *pt)
{
    if (aadlen < 0 || len < 0) return -1;
    return siv_open(key, 32, nonce, aad, aadlen, ct, len, tag, pt);
}
