/* Host test for the AES-GCM backend dispatch and the AES-NI/PCLMULQDQ path.
 *
 * Three things have to be true and are each easy to believe without evidence:
 *
 *   1. The accelerated path and the portable path produce identical bytes on
 *      every input. Not "it decrypts what it encrypted" -- AES-GCM round-trips
 *      happily with a wrong S-box or a wrong GHASH reduction, and would
 *      interoperate with nothing. So the comparison is against the other
 *      implementation AND against published vectors.
 *   2. The dispatch actually dispatches. A test suite that silently ran the C
 *      path on a machine with AES-NI would prove nothing about the code it was
 *      supposedly testing, and the failure mode is invisible.
 *   3. The fallback still works. A fallback that has never run is not a
 *      fallback, so the vectors are replayed with the baseline pinned.
 *
 * Deliberately NOT here: any timing measurement. Development runs under
 * QEMU/TCG where AES-NI is a C helper function, so a TCG number says nothing
 * about hardware in either direction; and this host binary runs natively, but
 * a microbenchmark of two implementations in one process is not the kind of
 * evidence the constant-time argument rests on anyway. The justification for
 * this backend is the removal of a secret-dependent memory access, which is a
 * structural property, not a speed. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "crypto.h"
#include "aes_backend.h"
#include "cpufeat.h"

static int checks, failures;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("FAIL %s\n", what); }
}

/* xorshift64*: deterministic, so a failure is reproducible from the seed
 * printed in the header line. */
static uint64_t rngstate = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd(void)
{
    uint64_t x = rngstate;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    rngstate = x;
    return x * 0x2545F4914F6CDD1DULL;
}
static void rndbytes(uint8_t *p, int n) { for (int i = 0; i < n; i++) p[i] = (uint8_t)rnd(); }

static int diff(const uint8_t *a, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return i + 1;
    return 0;
}

static void hexdump(const char *tag, const uint8_t *p, int n)
{
    printf("     %s: ", tag);
    for (int i = 0; i < n && i < 32; i++) printf("%02x", p[i]);
    printf("%s\n", n > 32 ? "..." : "");
}

/* --- published vectors --------------------------------------------------
 * The same AES-128-GCM vectors tests/unit/crypto_diff_gen.py pins and
 * self-checks: two NIST SP 800-38D zero-key cases and McGrew-Viega test case
 * 3 (60-byte plaintext with 20 bytes of AAD). Every backend must produce
 * exactly these -- agreeing with each other is not enough, since two
 * identically wrong backends would agree. */
struct kat {
    const char *name;
    uint8_t key[16], iv[12];
    int aadlen, ptlen;
    uint8_t aad[20], pt[64], ct[64], tag[16];
};

static const struct kat kats[] = {
    { "nist zero-key empty",
      {0}, {0}, 0, 0, {0}, {0}, {0},
      {0x58,0xe2,0xfc,0xce,0xfa,0x7e,0x30,0x61,0x36,0x7f,0x1d,0x57,0xa4,0xe7,0x45,0x5a} },
    { "nist zero-key 16B",
      {0}, {0}, 0, 16, {0}, {0},
      {0x03,0x88,0xda,0xce,0x60,0xb6,0xa3,0x92,0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78},
      {0xab,0x6e,0x47,0xd4,0x2c,0xec,0x13,0xbd,0xf5,0x3a,0x67,0xb2,0x12,0x57,0xbd,0xdf} },
    { "mcgrew-viega tc3",
      {0xfe,0xff,0xe9,0x92,0x86,0x65,0x73,0x1c,0x6d,0x6a,0x8f,0x94,0x67,0x30,0x83,0x08},
      {0xca,0xfe,0xba,0xbe,0xfa,0xce,0xdb,0xad,0xde,0xca,0xf8,0x88},
      20, 60,
      {0xfe,0xed,0xfa,0xce,0xde,0xad,0xbe,0xef,0xfe,0xed,0xfa,0xce,0xde,0xad,0xbe,0xef,
       0xab,0xad,0xda,0xd2},
      {0xd9,0x31,0x32,0x25,0xf8,0x84,0x06,0xe5,0xa5,0x59,0x09,0xc5,0xaf,0xf5,0x26,0x9a,
       0x86,0xa7,0xa9,0x53,0x15,0x34,0xf7,0xda,0x2e,0x4c,0x30,0x3d,0x8a,0x31,0x8a,0x72,
       0x1c,0x3c,0x0c,0x95,0x95,0x68,0x09,0x53,0x2f,0xcf,0x0e,0x24,0x49,0xa6,0xb5,0x25,
       0xb1,0x6a,0xed,0xf5,0xaa,0x0d,0xe6,0x57,0xba,0x63,0x7b,0x39},
      {0x42,0x83,0x1e,0xc2,0x21,0x77,0x74,0x24,0x4b,0x72,0x21,0xb7,0x84,0xd0,0xd4,0x9c,
       0xe3,0xaa,0x21,0x2f,0x2c,0x02,0xa4,0xe0,0x35,0xc1,0x7e,0x23,0x29,0xac,0xa1,0x2e,
       0x21,0xd5,0x14,0xb2,0x54,0x66,0x93,0x1c,0x7d,0x8f,0x6a,0x5a,0xac,0x84,0xaa,0x05,
       0x1b,0xa3,0x0b,0x39,0x6a,0x0a,0xac,0x97,0x3d,0x58,0xe0,0x91},
      {0x5b,0xc9,0x4f,0xbc,0x32,0x21,0xa5,0xdb,0x94,0xfa,0xe9,0x5a,0xe7,0x12,0x1a,0x47} },
};

static void run_kats(const char *label)
{
    for (unsigned i = 0; i < sizeof kats / sizeof kats[0]; i++) {
        const struct kat *k = &kats[i];
        uint8_t ct[64], tag[16], pt[64];
        aes128_gcm_seal(k->key, k->iv, k->aadlen ? k->aad : NULL, k->aadlen,
                        k->ptlen ? k->pt : NULL, k->ptlen, ct, tag);
        char what[128];
        snprintf(what, sizeof what, "[%s] %s ciphertext", label, k->name);
        if (k->ptlen && diff(ct, k->ct, k->ptlen)) { hexdump("got", ct, k->ptlen); }
        ok(k->ptlen == 0 || diff(ct, k->ct, k->ptlen) == 0, what);
        snprintf(what, sizeof what, "[%s] %s tag", label, k->name);
        if (diff(tag, k->tag, 16)) hexdump("got", tag, 16);
        ok(diff(tag, k->tag, 16) == 0, what);
        snprintf(what, sizeof what, "[%s] %s open round-trip", label, k->name);
        ok(aes128_gcm_open(k->key, k->iv, k->aadlen ? k->aad : NULL, k->aadlen,
                           k->ptlen ? k->ct : NULL, k->ptlen, k->tag, pt) == 0 &&
           (k->ptlen == 0 || diff(pt, k->pt, k->ptlen) == 0), what);
        snprintf(what, sizeof what, "[%s] %s rejects a flipped tag", label, k->name);
        uint8_t bad[16]; memcpy(bad, k->tag, 16); bad[7] ^= 0x40;
        ok(aes128_gcm_open(k->key, k->iv, k->aadlen ? k->aad : NULL, k->aadlen,
                           k->ptlen ? k->ct : NULL, k->ptlen, bad, pt) == -1, what);
    }
}

/* --- primitive-level differential ---------------------------------------
 * Cross the three backend primitives directly, not just the mode built on
 * them: a key schedule that differs in the last round key produces a wrong
 * ciphertext far from where the bug is, and a GHASH reduction that is wrong
 * in one bit only shows up for some inputs. */
static void diff_primitives(const struct aes_backend *a, const struct aes_backend *b,
                            int rounds)
{
    int sched_bad = 0, enc_bad = 0, gf_bad = 0;

    for (int i = 0; i < rounds; i++) {
        uint8_t key[32], blk[16];
        rndbytes(key, 32);
        rndbytes(blk, 16);
        int keylen = (i & 1) ? 32 : 16;
        int nr = keylen == 32 ? 14 : 10;

        uint8_t ra[240], rb[240];
        memset(ra, 0, sizeof ra); memset(rb, 0, sizeof rb);
        a->key_expand(key, keylen, ra);
        b->key_expand(key, keylen, rb);
        if (diff(ra, rb, 16 * (nr + 1))) {
            if (!sched_bad) { hexdump("sched A", ra, 32); hexdump("sched B", rb, 32); }
            sched_bad++;
        }

        uint8_t oa[16], ob[16];
        a->encrypt(ra, nr, blk, oa);
        b->encrypt(rb, nr, blk, ob);
        if (diff(oa, ob, 16)) {
            if (!enc_bad) { hexdump("enc A", oa, 16); hexdump("enc B", ob, 16); }
            enc_bad++;
        }

        uint8_t xa[16], xb[16], y[16];
        rndbytes(xa, 16); memcpy(xb, xa, 16);
        rndbytes(y, 16);
        a->gf_mul(xa, y);
        b->gf_mul(xb, y);
        if (diff(xa, xb, 16)) {
            if (!gf_bad) { hexdump("gf A", xa, 16); hexdump("gf B", xb, 16); }
            gf_bad++;
        }
    }

    printf("     %s vs %s over %d cases: key_expand %d bad, encrypt %d bad, gf_mul %d bad\n",
           a->name, b->name, rounds, sched_bad, enc_bad, gf_bad);
    ok(sched_bad == 0, "key schedules are byte-identical across backends");
    ok(enc_bad == 0, "block encryption is byte-identical across backends");
    ok(gf_bad == 0, "GF(2^128) multiply is byte-identical across backends");
}

/* --- full-GCM differential ----------------------------------------------
 * Same key/nonce/aad/plaintext through the whole AEAD on each backend.
 * Lengths deliberately include 0, non-multiples of 16 and a length that
 * straddles the CTR block boundary, because that is where a GHASH driver's
 * partial-block handling goes wrong. */
static void diff_gcm(int rounds)
{
    static uint8_t pt[512], ct_a[512], ct_b[512], out[512], aad[64];
    uint8_t key[32], nonce[12], tag_a[16], tag_b[16];
    int bad = 0, cases = 0;

    for (int i = 0; i < rounds; i++) {
        int keylen = (i & 1) ? 32 : 16;
        int ptlen  = (int)(rnd() % 200);
        int aadlen = (int)(rnd() % 40);
        if (i < 8) { ptlen = i; aadlen = (i * 3) % 17; }     /* the short edge cases */
        rndbytes(key, 32);
        rndbytes(nonce, 12);
        rndbytes(aad, 64);
        rndbytes(pt, 512);

        const uint8_t *ap = aadlen ? aad : NULL;
        const uint8_t *pp = ptlen ? pt : NULL;

        crypto_simd_force_baseline(0);           /* the selected (accelerated) path */
        if (keylen == 16) aes128_gcm_seal(key, nonce, ap, aadlen, pp, ptlen, ct_a, tag_a);
        else              aes256_gcm_seal(key, nonce, ap, aadlen, pp, ptlen, ct_a, tag_a);

        crypto_simd_force_baseline(1);           /* the portable reference */
        if (keylen == 16) aes128_gcm_seal(key, nonce, ap, aadlen, pp, ptlen, ct_b, tag_b);
        else              aes256_gcm_seal(key, nonce, ap, aadlen, pp, ptlen, ct_b, tag_b);

        if ((ptlen && diff(ct_a, ct_b, ptlen)) || diff(tag_a, tag_b, 16)) {
            if (!bad) {
                printf("     first mismatch: keylen=%d ptlen=%d aadlen=%d\n",
                       keylen, ptlen, aadlen);
                hexdump("ct accel", ct_a, ptlen);
                hexdump("ct  base", ct_b, ptlen);
                hexdump("tag accel", tag_a, 16);
                hexdump("tag  base", tag_b, 16);
            }
            bad++;
        }

        /* Cross-open: what the accelerated path sealed, the portable path
         * must open, and vice versa. This is the property TLS actually needs
         * -- a peer is running neither of our implementations. */
        crypto_simd_force_baseline(1);
        int r1 = (keylen == 16)
            ? aes128_gcm_open(key, nonce, ap, aadlen, ptlen ? ct_a : NULL, ptlen, tag_a, out)
            : aes256_gcm_open(key, nonce, ap, aadlen, ptlen ? ct_a : NULL, ptlen, tag_a, out);
        if (r1 != 0 || (ptlen && diff(out, pt, ptlen))) bad++;

        crypto_simd_force_baseline(0);
        int r2 = (keylen == 16)
            ? aes128_gcm_open(key, nonce, ap, aadlen, ptlen ? ct_b : NULL, ptlen, tag_b, out)
            : aes256_gcm_open(key, nonce, ap, aadlen, ptlen ? ct_b : NULL, ptlen, tag_b, out);
        if (r2 != 0 || (ptlen && diff(out, pt, ptlen))) bad++;

        cases++;
    }
    crypto_simd_force_baseline(0);
    printf("     full AES-GCM differential: %d cases (128 and 256), %d mismatches\n",
           cases, bad);
    ok(bad == 0, "AES-GCM output is byte-identical across backends, both directions");
}

int main(void)
{
    printf("aes_ni_test: AES-GCM backend dispatch (seed %#llx)\n",
           (unsigned long long)rngstate);

    const struct aes_backend *c = aes_backend_c();
    const struct aes_backend *ni = aes_backend_ni();
    int cpu_can = cpu_has(CPU_AES) && cpu_has(CPU_PCLMULQDQ);

    printf("     cpu: aes=%d pclmulqdq=%d avx=%d avx2=%d xsave=%d\n",
           cpu_has(CPU_AES), cpu_has(CPU_PCLMULQDQ), cpu_has(CPU_AVX),
           cpu_has(CPU_AVX2), cpu_has(CPU_XSAVE));

    /* --- the baseline exists and is named -------------------------------- */
    ok(c != NULL, "portable backend is always present");
    ok(c && !strcmp(c->name, "c"), "portable backend is named \"c\"");
    ok(c && c->constant_time == 0, "portable backend does not claim to be constant-time");

    /* --- availability matches what CPUID says ---------------------------- */
    ok((ni != NULL) == cpu_can,
       "AES-NI backend is offered exactly when CPUID reports aes+pclmulqdq");

    /* --- the dispatch actually dispatches -------------------------------- */
    crypto_simd_init();

#ifdef AESNI_CONTROL_NO_ACCEL
    /* NEGATIVE CONTROL BUILD (`make test-aes-ni-control`). Nothing in the
     * kernel or the crypto changes -- this pins the portable backend here, in
     * the test, to simulate exactly the failure this suite has to be able to
     * see: an accelerated path that exists, builds, and is never selected.
     * The assertions immediately below MUST fail in this build. If they pass,
     * they were never testing anything and the whole "AES-GCM is now
     * constant-time" claim is unsupported. */
    crypto_simd_force_baseline(1);
    printf("     NEGATIVE CONTROL BUILD: baseline pinned before the dispatch checks\n");
#endif

    if (cpu_can) {
        ok(!strcmp(crypto_simd_backend_name(), "aesni"),
           "on an AES-NI CPU the selected backend is aesni, not the C fallback");
        /* This is the negative control for the whole change: before it, AES-GCM
         * was never constant-time on any CPU. It fails if aes_ni.c is removed,
         * if the dispatch stops selecting it, or if someone marks the C path
         * constant-time to make a report look better. */
        ok(crypto_simd_constant_time() == 1,
           "NEGATIVE CONTROL: AES-GCM reports constant-time on an AES-NI CPU");
    } else {
        printf("SKIP dispatch-selects-aesni (this CPU has no AES-NI); "
               "the C path is the only one and is exercised below\n");
        ok(!strcmp(crypto_simd_backend_name(), "c"),
           "without AES-NI the selected backend is the portable one");
        ok(crypto_simd_constant_time() == 0,
           "without AES-NI, constant-time is not claimed");
    }

    /* --- the two implementations are genuinely distinct ------------------ */
    if (ni) {
        ok(ni->key_expand != c->key_expand, "backends have distinct key_expand");
        ok(ni->encrypt != c->encrypt, "backends have distinct encrypt");
        ok(ni->gf_mul != c->gf_mul, "backends have distinct gf_mul");
        ok(strcmp(ni->name, c->name) != 0, "backends have distinct names");
    }

    /* --- published vectors, under each backend --------------------------- */
    crypto_simd_force_baseline(0);
    run_kats(crypto_simd_backend_name());
    crypto_simd_force_baseline(1);
    ok(!strcmp(crypto_simd_backend_name(), "c"),
       "force_baseline pins the portable backend");
    run_kats("forced-baseline");
    crypto_simd_force_baseline(0);
    ok(!strcmp(crypto_simd_backend_name(), cpu_can ? "aesni" : "c"),
       "clearing force_baseline restores the selected backend");

    /* --- the differential ------------------------------------------------ */
    if (ni) {
        diff_primitives(c, ni, 20000);
        diff_gcm(4000);

        /* Control on the comparison itself: the differential above would be
         * worthless if it were accidentally comparing a buffer with itself.
         * Flip one bit of a known-good result and require the same comparison
         * to report it. */
        uint8_t a[16], b[16];
        memcpy(a, kats[2].tag, 16); memcpy(b, kats[2].tag, 16);
        ok(diff(a, b, 16) == 0, "comparator: identical buffers compare equal");
        b[9] ^= 0x01;
        ok(diff(a, b, 16) == 10,
           "CONTROL: comparator reports a single flipped bit at the right offset");
    } else {
        printf("SKIP C-vs-AES-NI differential: no AES-NI on this host\n");
    }

    /* selftest hook used by the on-target boot check */
    ok(crypto_simd_selftest() == 0, "crypto_simd_selftest agrees with the reference");
    crypto_simd_force_baseline(1);
    ok(crypto_simd_selftest() == 0, "crypto_simd_selftest passes with the baseline pinned");
    crypto_simd_force_baseline(0);

    printf("\n%d checks, %d failed\n", checks, failures);
    printf("%s\n", failures ? "AES-NI TEST FAILED" : "AES-NI TEST PASSED");
    return failures ? 1 : 0;
}
