/* AES-NI + PCLMULQDQ backend for AES-GCM.
 *
 * WHY: not speed -- constant time. The portable backend in aesgcm.c indexes a
 * 256-byte S-box with secret state and branches on secret bits inside the
 * GF(2^128) multiply, so both leak through the data cache and the branch
 * predictor. AESENC and PCLMULQDQ are single instructions with no data-
 * dependent memory access and no data-dependent timing on any CPU that
 * implements them, so this backend removes that class of leak entirely on
 * hardware that has it. (Any speedup is a side effect, and cannot be measured
 * under QEMU/TCG, where both instructions are C helper functions.)
 *
 * WHY INLINE ASM RATHER THAN <wmmintrin.h>: the kernel builds -ffreestanding
 * with only -msse -msse2, and this same TU is compiled by the host test
 * targets. Writing the four instructions directly means no new compiler flags,
 * no intrinsic headers, and no chance of the compiler emitting AES-NI in the
 * dispatch code that decides whether AES-NI is available. The one thing the
 * compiler must be told is the target attribute, so its assembler will accept
 * the mnemonics; that permits but does not require the ISA, and every function
 * here is reached only through aes_backend_ni(), which returns NULL unless
 * CPUID says both features are present.
 *
 * On a non-x86 host build the whole file collapses to `aes_backend_ni()
 * returns NULL`, matching the pattern c/apps/as/as_ll.c already uses. */

#include <stdint.h>
#include "aes_backend.h"
#include "cpufeat.h"

const struct aes_backend *aes_backend_ni(void);

#if !(defined(__x86_64__) || defined(__i386__))

const struct aes_backend *aes_backend_ni(void) { return 0; }

#else

#define AESNI __attribute__((target("aes,pclmul,sse2")))

typedef unsigned long long xmm_t __attribute__((vector_size(16)));

/* Unaligned 16-byte load/store. The round-key array is a plain uint8_t[240] on
 * a kernel stack -- 16-byte alignment is not guaranteed, so never use an
 * aligned move here. __builtin_memcpy of exactly 16 bytes compiles to movups. */
static inline xmm_t xload(const void *p)
{
    xmm_t v;
    __builtin_memcpy(&v, p, 16);
    return v;
}

static inline void xstore(void *p, xmm_t v)
{
    __builtin_memcpy(p, &v, 16);
}

/* --- instruction wrappers ------------------------------------------------
 * AT&T operand order is the reverse of the Intel manual's: Intel's
 * `AESENC xmm1, xmm2` (dst, src) is `aesenc %xmm2, %xmm1` (src, dst), and an
 * imm8 comes first. Getting this backwards produces a cipher that is
 * self-consistent and interoperates with nothing, which is why the vectors
 * matter more than the reading. */
#define X_AESENC(dst, src)      __asm__ ("aesenc %1, %0"      : "+x"(dst) : "x"(src))
#define X_AESENCLAST(dst, src)  __asm__ ("aesenclast %1, %0"  : "+x"(dst) : "x"(src))
#define X_AESDEC(dst, src)      __asm__ ("aesdec %1, %0"      : "+x"(dst) : "x"(src))
#define X_AESDECLAST(dst, src)  __asm__ ("aesdeclast %1, %0"  : "+x"(dst) : "x"(src))
#define X_AESIMC(dst, src)      __asm__ ("aesimc %1, %0"      : "=x"(dst) : "x"(src))
#define X_KEYGEN(dst, src, rc)  __asm__ ("aeskeygenassist %2, %1, %0" \
                                         : "=x"(dst) : "x"(src), "i"(rc))
#define X_PSHUFD(dst, src, im)  __asm__ ("pshufd %2, %1, %0"  : "=x"(dst) : "x"(src), "i"(im))
#define X_PSLLDQ(v, n)          __asm__ ("pslldq %1, %0"      : "+x"(v) : "i"(n))
#define X_PSRLDQ(v, n)          __asm__ ("psrldq %1, %0"      : "+x"(v) : "i"(n))
#define X_PSLLQ(v, n)           __asm__ ("psllq %1, %0"       : "+x"(v) : "i"(n))
#define X_PSRLQ(v, n)           __asm__ ("psrlq %1, %0"       : "+x"(v) : "i"(n))
#define X_PSLLD(v, n)           __asm__ ("pslld %1, %0"       : "+x"(v) : "i"(n))
#define X_PSRLD(v, n)           __asm__ ("psrld %1, %0"       : "+x"(v) : "i"(n))
#define X_CLMUL(dst, src, im)   __asm__ ("pclmulqdq %2, %1, %0" \
                                         : "+x"(dst) : "x"(src), "i"(im))

/* --- key schedule --------------------------------------------------------
 * FIPS-197 in three instructions per round key. AESKEYGENASSIST does the
 * RotWord/SubWord/Rcon of the last column; the running XOR of the previous
 * words is the pslldq-by-4 cascade. Output is byte-for-byte the schedule the
 * C key_expand produces -- asserted by the host test, because a schedule that
 * merely round-trips with itself is the classic invisible AES bug. */
AESNI static xmm_t key_combine(xmm_t k, xmm_t t)
{
    xmm_t s = k;
    X_PSLLDQ(s, 4); k ^= s;
    X_PSLLDQ(s, 4); k ^= s;
    X_PSLLDQ(s, 4); k ^= s;
    return k ^ t;
}

/* AES-128: 10 round-key derivations, Rcon 01 02 04 08 10 20 40 80 1b 36. */
#define AES128_STEP(rc)                                    \
    do {                                                   \
        xmm_t t_;                                          \
        X_KEYGEN(t_, k0, rc);                              \
        X_PSHUFD(t_, t_, 0xff);   /* broadcast word 3 */   \
        k0 = key_combine(k0, t_);                          \
        xstore(rk + 16 * (++idx), k0);                     \
    } while (0)

/* AES-256 consumes two 128-bit halves. The even step is the AES-128 step on
 * k0 seeded from k1; the odd step re-runs SubWord ONLY (Rcon 0, broadcast of
 * word 2 rather than word 3), which is FIPS-197 5.2's "nk > 6" case -- the
 * same extra SubWord the C implementation calls out. */
#define AES256_EVEN(rc)                                    \
    do {                                                   \
        xmm_t t_;                                          \
        X_KEYGEN(t_, k1, rc);                              \
        X_PSHUFD(t_, t_, 0xff);                            \
        k0 = key_combine(k0, t_);                          \
        xstore(rk + 16 * (++idx), k0);                     \
    } while (0)

#define AES256_ODD()                                       \
    do {                                                   \
        xmm_t t_;                                          \
        X_KEYGEN(t_, k0, 0x00);                            \
        X_PSHUFD(t_, t_, 0xaa);   /* broadcast word 2 */   \
        k1 = key_combine(k1, t_);                          \
        xstore(rk + 16 * (++idx), k1);                     \
    } while (0)

/* AES-192 (nk = 6): the word recurrence has lag 6, which does not line up with
 * the 4-word pslldq cascade the 128/256 steps are built on, so the schedule is
 * computed word by word. SubWord(RotWord(.)) still comes from AESKEYGENASSIST
 * rather than an S-box table: this TU deliberately holds no substitution table,
 * so the instruction stays the single source of the S-box for this backend.
 * AESKEYGENASSIST takes its Rcon as an immediate, so the helper below folds in
 * 0 and the caller XORs the (public, never-wrapping 1..128) Rcon itself.
 *
 * The output must be byte-identical to the portable schedule -- pinned by
 * crypto_simd_selftest() and tests/unit/aes_ni_test.c, which is the check that
 * matters here, because a schedule that is self-consistent but non-standard
 * encrypts to garbage that only another implementation can name. */
/* SubWord(RotWord(w)) in FIPS-197 word order. AESKEYGENASSIST's dword3 is
 * SubWord(RotWord(w)) with the rotation in the OPPOSITE direction to FIPS-197
 * (its bytes run the other way round the register), and its imm8 Rcon lands
 * on the word's LAST byte rather than its first -- so the imm is 0 here, the
 * caller XORs its own Rcon into the top byte, and the two-byte rotation below
 * turns the instruction's rotation into the standard's. The mapping was
 * pinned empirically (a key with W5 = 00010203, both backends, against the
 * published S-box), because two of those three convention differences are
 * invisible on the all-equal-byte inputs a casual probe uses. */
AESNI static uint32_t ni_sub_rot(uint32_t w)
{
    uint32_t s[4] = { w, w, w, w };
    xmm_t v = xload(s);
    X_KEYGEN(v, v, 0x00);
    X_PSHUFD(v, v, 0xff);                     /* broadcast dword3 */
    xstore(s, v);
    return (s[0] >> 16) | (s[0] << 16);       /* byte-rotate into FIPS order */
}

AESNI static void ni_key_expand_192(const uint8_t *key, uint8_t *rk)
{
    uint32_t w[13 * 4];                       /* 52 words: 4*(12+1) */
    for (int i = 0; i < 6; i++)
        w[i] = ((uint32_t)key[4*i] << 24) | ((uint32_t)key[4*i+1] << 16) |
               ((uint32_t)key[4*i+2] << 8) | (uint32_t)key[4*i+3];
    uint32_t rcon = 1;
    for (int i = 6; i < 52; i++) {
        uint32_t t = w[i - 1];
        if (i % 6 == 0) {
            /* rcon 1,2,4,...,128: never wraps in GF(2^8), a plain shift
             * doubles it. It XORs into the FIRST byte of the FIPS word --
             * the most significant one in the big-endian serialization. */
            t = ni_sub_rot(t) ^ (rcon << 24);
            rcon <<= 1;
        }
        w[i] = w[i - 6] ^ t;
    }
    for (int i = 0; i < 52; i++) {
        rk[4*i]   = (uint8_t)(w[i] >> 24);
        rk[4*i+1] = (uint8_t)(w[i] >> 16);
        rk[4*i+2] = (uint8_t)(w[i] >> 8);
        rk[4*i+3] = (uint8_t)w[i];
    }
}

AESNI static void ni_key_expand(const uint8_t *key, int keylen, uint8_t *rk)
{
    int idx = 0;
    if (keylen == 32) {
        xmm_t k0 = xload(key), k1 = xload(key + 16);
        xstore(rk, k0);
        xstore(rk + 16, k1);
        idx = 1;
        AES256_EVEN(0x01); AES256_ODD();
        AES256_EVEN(0x02); AES256_ODD();
        AES256_EVEN(0x04); AES256_ODD();
        AES256_EVEN(0x08); AES256_ODD();
        AES256_EVEN(0x10); AES256_ODD();
        AES256_EVEN(0x20); AES256_ODD();
        AES256_EVEN(0x40);                 /* 15th round key; no trailing odd */
    } else if (keylen == 24) {
        ni_key_expand_192(key, rk);
    } else {
        xmm_t k0 = xload(key);
        xstore(rk, k0);
        AES128_STEP(0x01); AES128_STEP(0x02); AES128_STEP(0x04);
        AES128_STEP(0x08); AES128_STEP(0x10); AES128_STEP(0x20);
        AES128_STEP(0x40); AES128_STEP(0x80); AES128_STEP(0x1b);
        AES128_STEP(0x36);
    }
}

AESNI static void ni_encrypt(const uint8_t *rk, int nr,
                             const uint8_t in[16], uint8_t out[16])
{
    xmm_t s = xload(in) ^ xload(rk);
    for (int r = 1; r < nr; r++)
        X_AESENC(s, xload(rk + 16 * r));
    X_AESENCLAST(s, xload(rk + 16 * nr));
    xstore(out, s);
}

/* --- inverse cipher --------------------------------------------------------
 * AESDEC/AESDECLAST implement FIPS-197's EQUIVALENT inverse cipher (5.3.5),
 * whose round keys differ from the encryption schedule: every round key
 * except the first and last must be passed through InvMixColumns. AESIMC is
 * that transform as one instruction. The transformation is folded into a
 * stack copy here rather than stored in the backend, so key_expand keeps
 * emitting the plain FIPS-197 schedule byte-for-byte and the schedule-
 * identity check the differential rests on stays meaningful.
 *
 * Leaving the AESIMC step out is the classic silent CBC-decrypt bug: the
 * chain runs to completion, "decrypts" to garbage, and only ever shows up
 * against another implementation -- which is exactly what the cbc diff
 * battery and the SP 800-38A F.2 decrypt vectors are for. */
AESNI static void ni_decrypt(const uint8_t *rk, int nr,
                             const uint8_t in[16], uint8_t out[16])
{
    xmm_t irk[15];                              /* nr+1 <= 15 for all key sizes */
    irk[0] = xload(rk);
    for (int r = 1; r < nr; r++)
        X_AESIMC(irk[r], xload(rk + 16 * r));   /* equivalent inverse round key */
    irk[nr] = xload(rk + 16 * nr);

    xmm_t s = xload(in) ^ irk[nr];
    for (int r = nr - 1; r >= 1; r--)
        X_AESDEC(s, irk[r]);
    X_AESDECLAST(s, irk[0]);
    xstore(out, s);
}

/* --- GHASH multiply ------------------------------------------------------
 * GCM's field uses the reversed bit order (byte 0 bit 7 is the x^0
 * coefficient), so a block loaded as a big-endian 128-bit integer has the
 * polynomial's bits reflected. Intel's carry-less-multiplication note
 * ("Intel Carry-Less Multiplication Instruction and its Usage for Computing
 * the GCM Mode", Algorithm 4 + the shift/xor reduction) multiplies the
 * reflected operands directly: the 255-bit carry-less product of two
 * reflected values is the reflection of the true product shifted one bit, so
 * the 256-bit result is shifted left by 1 and then reduced modulo the
 * reflected polynomial x^128 + x^127 + x^126 + x^121 + 1.
 *
 * Every step below is verified against the portable gf_mul, not against my
 * reading of the paper: tests/unit/aes_ni_test.c crosses them over random
 * inputs and the 128k-case differential replays every AES-GCM vector through
 * both. A reduction that is wrong in one bit still produces plausible-looking
 * output, so "it decrypts what it encrypted" proves nothing here. */

/* big-endian load: bits 127:64 of the register = first 8 bytes of the block */
static inline xmm_t ghash_load(const uint8_t b[16])
{
    uint64_t lo, hi;
    __builtin_memcpy(&hi, b, 8);
    __builtin_memcpy(&lo, b + 8, 8);
    xmm_t v = { __builtin_bswap64(lo), __builtin_bswap64(hi) };
    return v;
}

static inline void ghash_store(uint8_t b[16], xmm_t v)
{
    uint64_t hi = __builtin_bswap64(v[1]), lo = __builtin_bswap64(v[0]);
    __builtin_memcpy(b, &hi, 8);
    __builtin_memcpy(b + 8, &lo, 8);
}

AESNI static void ni_gf_mul(uint8_t x[16], const uint8_t y[16])
{
    xmm_t a = ghash_load(x), b = ghash_load(y);

    /* 128x128 -> 256 carry-less, schoolbook. imm8[0] picks a's qword,
     * imm8[4] picks b's. */
    xmm_t lo = a, mid1 = a, mid2 = a, hi = a;
    X_CLMUL(lo,   b, 0x00);          /* a.lo * b.lo */
    X_CLMUL(mid1, b, 0x10);          /* a.lo * b.hi */
    X_CLMUL(mid2, b, 0x01);          /* a.hi * b.lo */
    X_CLMUL(hi,   b, 0x11);          /* a.hi * b.hi */

    mid1 ^= mid2;
    xmm_t t = mid1;
    X_PSRLDQ(mid1, 8);
    X_PSLLDQ(t, 8);
    lo ^= t;
    hi ^= mid1;                      /* [hi:lo] = the 256-bit product */

    /* <<1 across the 256-bit value, to undo the reflection's off-by-one. */
    xmm_t cl = lo, ch = hi;
    X_PSRLQ(cl, 63);
    X_PSRLQ(ch, 63);
    X_PSLLQ(lo, 1);
    X_PSLLQ(hi, 1);
    xmm_t carry = cl;                /* qword 1 of cl crosses into hi */
    X_PSRLDQ(carry, 8);
    X_PSLLDQ(cl, 8);
    X_PSLLDQ(ch, 8);
    lo |= cl;
    hi |= ch;
    hi |= carry;

    /* Reduction, phase 1: fold lo's low 128 bits against x^127+x^126+x^121. */
    xmm_t r1 = lo, r2 = lo, r3 = lo;
    X_PSLLD(r1, 31);
    X_PSLLD(r2, 30);
    X_PSLLD(r3, 25);
    r1 ^= r2;
    r1 ^= r3;
    xmm_t spill = r1;
    X_PSRLDQ(spill, 4);              /* the part that lands in the high half */
    X_PSLLDQ(r1, 12);
    lo ^= r1;

    /* Reduction, phase 2: the matching right shifts, then fold in the high half. */
    xmm_t s1 = lo, s2 = lo, s3 = lo;
    X_PSRLD(s1, 1);
    X_PSRLD(s2, 2);
    X_PSRLD(s3, 7);
    s1 ^= s2;
    s1 ^= s3;
    s1 ^= spill;
    lo ^= s1;
    lo ^= hi;

    ghash_store(x, lo);
}

static const struct aes_backend ni_backend = {
    "aesni", ni_key_expand, ni_encrypt, ni_decrypt, ni_gf_mul, 1
};

const struct aes_backend *aes_backend_ni(void)
{
    /* AES-NI without PCLMULQDQ exists in principle; we refuse the pair rather
     * than shipping a half-accelerated GCM whose GHASH still leaks, because
     * the reason for this backend is the side channel, not the speed. */
    if (!cpu_has(CPU_AES) || !cpu_has(CPU_PCLMULQDQ)) return 0;
    return &ni_backend;
}

#endif /* x86 */
