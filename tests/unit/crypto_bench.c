/* What each crypto primitive costs, on the host, in nanoseconds.
 *
 * WHY THIS EXISTS
 * ---------------
 * tests/boot/run-tls-bench.sh says which PHASE of a handshake the time is in.
 * It cannot say which primitive, because a phase like "chain signature check"
 * is one RSA verify on one site and one ECDSA verify on the next, and those
 * differ by an order of magnitude. This file is the other half: a fixed,
 * repeatable cost per primitive, with no network and no emulator in the way.
 *
 * WHAT A NUMBER FROM HERE IS AND IS NOT
 * -------------------------------------
 * It is a HOST number. The machine anyone actually runs LogitOS on is QEMU/TCG,
 * where every one of these is slower and NOT by a uniform factor -- a
 * table-driven byte loop and a 64x64 multiply are not emulated at the same
 * cost. So this file is for two things:
 *   1. the ranking. If RSA-2048 verify costs 40x a P-256 verify here, that
 *      ordering survives emulation even though both numbers change.
 *   2. before/after on a change to one primitive, which is the only place a
 *      speedup can be attributed to the code rather than to the weather.
 * The end-to-end claim always has to come back from the guest.
 *
 * NO KEY MATERIAL IS NEEDED, ON PURPOSE. A signature verifier does the same
 * work whether or not the signature is valid: ecdsa_verify runs both scalar
 * multiplications before it compares anything, and rsa_*_verify runs the whole
 * modexp before it looks at the padding. So the inputs here are deterministic
 * pseudo-random values that pass the range and on-curve checks and then fail at
 * the end. That keeps the benchmark self-contained -- no openssl, no fixtures,
 * no expiry dates -- at the cost of measuring the failure path, which is the
 * same path.
 *
 * THE ROOT SCAN is the exception worth reading: it is not a synthetic loop, it
 * is exactly what c/net/tls/x509.c signed_by_root() does when the presented
 * chain's anchor is NOT sent in-band -- try every root of the matching key type
 * until one verifies. It runs against the REAL compiled-in bundle, so the
 * number it prints is the real worst case that path can cost.
 *
 * Build: make test-crypto-bench
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <time.h>

#include "crypto.h"
#include "roots.h"

/* ------------------------------------------------------------------ timing */

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Run `fn` until at least MIN_S has elapsed, doubling the batch each round so a
 * primitive that costs 10 ns and one that costs 10 ms both get a stable figure
 * without either being hand-tuned. Returns seconds per call. */
#define MIN_S 0.25

static double bench(void (*fn)(void *), void *ctx)
{
    long iters = 1;
    for (;;) {
        double t0 = now_s();
        for (long i = 0; i < iters; i++) fn(ctx);
        double dt = now_s() - t0;
        if (dt >= MIN_S) return dt / (double)iters;
        iters = iters < 4 ? 4 : iters * 4;
    }
}

static void report(const char *name, double sec, double bytes)
{
    if (bytes > 0)
        printf("  %-30s %10.2f us   %8.1f MB/s\n", name, sec * 1e6, bytes / sec / 1e6);
    else if (sec * 1e6 >= 1.0)
        printf("  %-30s %10.2f us\n", name, sec * 1e6);
    else
        printf("  %-30s %10.2f ns\n", name, sec * 1e9);
}

/* ------------------------------------------------------- deterministic fill */

static uint64_t rng_s = 0x9E3779B97F4A7C15ull;
static uint8_t rnd8(void)
{
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17;
    return (uint8_t)(rng_s >> 33);
}
static void fill(uint8_t *p, int n) { for (int i = 0; i < n; i++) p[i] = rnd8(); }

/* ------------------------------------------------------------- the payloads */

#define BULK (64 * 1024)
static uint8_t buf[BULK], out[BULK + 64], tag[16];
static uint8_t k32[32], iv12[12], iv16[16];

static void do_sha256(void *c) { uint8_t h[32]; sha256(buf, (size_t)(long)c, h); }
static void do_sha384(void *c) { uint8_t h[48]; sha384(buf, (size_t)(long)c, h); }
static void do_sha512(void *c) { uint8_t h[64]; sha512(buf, (size_t)(long)c, h); }
static void do_sha512t(void *c) { uint8_t h[32]; sha512_256(buf, (size_t)(long)c, h); }

static void do_aes128(void *c)
{ aes128_gcm_seal(k32, iv12, 0, 0, buf, (int)(long)c, out, tag); }
static void do_aes256(void *c)
{ aes256_gcm_seal(k32, iv12, 0, 0, buf, (int)(long)c, out, tag); }
static void do_chacha(void *c)
{ chacha20_poly1305_seal(k32, iv12, 0, 0, buf, (int)(long)c, out, tag); }
static void do_ctr128(void *c)
{ aes128_ctr(k32, iv16, buf, (int)(long)c, out); }
static void do_ctr256(void *c)
{ aes256_ctr(k32, iv16, buf, (int)(long)c, out); }
static void do_cbc128(void *c)
{ aes128_cbc_encrypt(k32, iv16, buf, (int)(long)c, out); }
static void do_cbc128_dec(void *c)
{ aes128_cbc_decrypt(k32, iv16, buf, (int)(long)c, out); }

static uint8_t x_scalar[32], x_point[32], x_out[32];
static void do_x25519_base(void *c) { (void)c; x25519_base(x_out, x_scalar); }
static void do_x25519(void *c)      { (void)c; x25519(x_out, x_scalar, x_point); }

static uint8_t ec_priv[48], ec_pub[97], ec_shared[48];
static void do_ecdh_keygen(void *c) { ecdh_keygen((int)(long)c, ec_priv, 0x40000001u, ec_pub); }
static void do_ecdh_shared(void *c)
{ int cu = (int)(long)c; ecdh_shared(cu, ec_priv, 0x40000001u, ec_pub, 1 + 2*(cu/8), ec_shared); }

static uint8_t sig_rs[96], msghash[64];
static void do_ecdsa(void *c)
{
    int cu = (int)(long)c;
    /* The public key is the curve's own base point: on-curve, in range, so the
     * verify runs to completion and fails only at the final comparison. */
    ecdsa_verify(cu, ec_pub + 1, sig_rs, msghash, 32);
}

/* RSA: a modulus that is odd and has its top bit set (so the limb count is the
 * nominal one) and a signature below it. rsa_pkcs1_verify does the full modexp
 * and then rejects the padding, which is the same modexp a real verify does. */
static uint8_t rsa_n[512], rsa_e[3] = { 0x01, 0x00, 0x01 }, rsa_sig[512];
static void do_rsa(void *c)
{
    int nl = (int)(long)c;
    /* Odd modulus (Montgomery needs it) and top bit set, for THIS length --
     * the bytes that have to carry those two properties move with nl, and
     * setting them only for the 512-byte case made every shorter modulus even,
     * which rsa_public rejects before doing any work at all. A benchmark that
     * measures the rejection path reports a 167 ns RSA verify, which is how
     * this was caught. */
    rsa_n[0] |= 0x80; rsa_n[nl - 1] |= 1;
    rsa_sig[0] &= 0x7f;
    rsa_pkcs1_verify(rsa_n, nl, rsa_e, 3, rsa_sig, nl, msghash, 32);
}

/* The real trust-store scan: every RSA root tried in turn, which is what
 * signed_by_root() does when the anchor is not in the presented chain. */
static void do_root_scan(void *c)
{
    (void)c;
    for (int i = 0; i < logit_nroots; i++) {
        const struct root_ca *r = &logit_roots[i];
        if (r->type != ROOT_RSA) continue;
        rsa_pkcs1_verify(r->n, r->nlen, r->e, r->elen, rsa_sig, r->nlen, msghash, 32);
    }
}

/* ------------------------------------------------------------------- main */

/* The gate: a claim about this code that a machine can check, expressed as a
 * RATIO rather than as microseconds.
 *
 * An absolute threshold ("RSA-2048 verify under 150 us") is a statement about
 * the laptop it was written on and starts failing on a slower CI box for
 * reasons that have nothing to do with the code. A ratio against another
 * primitive in the same file family cancels the machine out: x25519 and RSA are
 * both integer bignum code with no SIMD and no tables, so a box that is half as
 * fast is half as fast at both.
 *
 * Before the 64-bit limbs and the Montgomery-form conversion, RSA-2048 verify
 * cost 13x an x25519 shared secret (357 us vs 27 us). After, it is 2.0x (55 vs
 * 27). The gate is 4x -- a 2x margin over the current value and a 3x margin
 * below the old one, so it distinguishes the two without being tuned to either.
 * `make test-crypto-bench-control` builds rsa.c with -DRSA_SLOW_CONTROL and
 * requires this to FAIL. */
#define GATE_RSA_OVER_X25519 4.0

static int gate(void)
{
    double x = bench(do_x25519, 0);
    double r = bench(do_rsa, (void *)256L);
    double ratio = r / x;
    printf("gate: rsa2048 %.1f us / x25519 %.1f us = %.2fx (limit %.1fx)\n",
           r * 1e6, x * 1e6, ratio, GATE_RSA_OVER_X25519);
    if (ratio > GATE_RSA_OVER_X25519) {
        printf("FAIL: RSA-2048 verification is %.2fx an x25519 shared secret\n", ratio);
        return 1;
    }
    printf("PASS: crypto_bench gate\n");
    return 0;
}

int main(int argc, char **argv)
{
    int only_rsa = (argc > 1 && strcmp(argv[1], "--rsa") == 0);
    int only_gate = (argc > 1 && strcmp(argv[1], "--gate") == 0);

    fill(buf, BULK); fill(k32, 32); fill(iv12, 12); fill(iv16, 16);
    fill(x_scalar, 32); fill(x_point, 32);
    fill(msghash, 64); fill(sig_rs, 96);
    fill(rsa_n, 512); fill(rsa_sig, 512);
    fill(ec_priv, 48);
    /* The EC private scalar must land in [1, n-1] or ecdh_keygen rejects it and
     * returns in nanoseconds without touching the curve. Clearing the top byte
     * is enough for both P-256 and P-384 and keeps the input deterministic. */
    ec_priv[0] = 0x01;

    /* r and s must be in [1, n-1]; clearing the top byte is enough for both
     * curve orders and keeps the input deterministic. */
    sig_rs[0] = 1; sig_rs[48] = 1;

    printf("crypto_bench -- host, %s\n", crypto_simd_backend_name());

    if (only_gate) return gate();

    if (!only_rsa) {
        printf("\n-- hash (64 KiB) --\n");
        report("sha256", bench(do_sha256, (void *)(long)BULK), BULK);
        report("sha384", bench(do_sha384, (void *)(long)BULK), BULK);
        report("sha512", bench(do_sha512, (void *)(long)BULK), BULK);
        report("sha512_256", bench(do_sha512t, (void *)(long)BULK), BULK);

        /* Both AES backends, back to back, on the same buffer. This is the
         * comparison crypto_simd_force_baseline() exists for; on hardware the
         * accelerated one should win by a lot, and under TCG (where AESENC and
         * PCLMULQDQ are interpreter helpers) it is an open question, which is
         * why the same pair is also measured in the guest at boot. */
        printf("\n-- AEAD (64 KiB) --\n");
        crypto_simd_force_baseline(1);
        report("aes128-gcm  [C]", bench(do_aes128, (void *)(long)BULK), BULK);
        report("aes256-gcm  [C]", bench(do_aes256, (void *)(long)BULK), BULK);
        crypto_simd_force_baseline(0);
        printf("  (backend now: %s)\n", crypto_simd_backend_name());
        report("aes128-gcm  [selected]", bench(do_aes128, (void *)(long)BULK), BULK);
        report("aes256-gcm  [selected]", bench(do_aes256, (void *)(long)BULK), BULK);
        report("chacha20-poly1305", bench(do_chacha, (void *)(long)BULK), BULK);

        /* The unauthenticated modes, for comparison against their AEAD
         * versions: CTR is GCM's keystream half (no GHASH, no tag), and CBC
         * decrypt is the only user of the backend's block-decrypt primitive.
         * CBC encrypts 64 KiB + one pad block, so its MB/s and CTR's are not
         * directly comparable to each other -- they are both here to rank
         * against the same number for GCM. */
        printf("\n-- AES modes (64 KiB) --\n");
        crypto_simd_force_baseline(1);
        report("aes128-ctr  [C]", bench(do_ctr128, (void *)(long)BULK), BULK);
        report("aes128-cbc  [C]", bench(do_cbc128, (void *)(long)BULK), BULK);
        report("aes128-cbc-dec [C]", bench(do_cbc128_dec, (void *)(long)BULK), BULK);
        crypto_simd_force_baseline(0);
        report("aes128-ctr  [selected]", bench(do_ctr128, (void *)(long)BULK), BULK);
        report("aes256-ctr  [selected]", bench(do_ctr256, (void *)(long)BULK), BULK);
        report("aes128-cbc  [selected]", bench(do_cbc128, (void *)(long)BULK), BULK);
        report("aes128-cbc-dec [selected]", bench(do_cbc128_dec, (void *)(long)BULK), BULK);

        printf("\n-- key exchange --\n");
        report("x25519 base (keygen)", bench(do_x25519_base, 0), 0);
        report("x25519 (shared)", bench(do_x25519, 0), 0);
        if (ecdh_keygen(256, ec_priv, 0x40000001u, ec_pub) != 0) return fprintf(stderr, "P-256 keygen setup failed\n"), 1;
        report("ecdh keygen P-256", bench(do_ecdh_keygen, (void *)256L), 0);
        report("ecdh shared P-256", bench(do_ecdh_shared, (void *)256L), 0);
        if (ecdh_keygen(384, ec_priv, 0x40000001u, ec_pub) != 0) return fprintf(stderr, "P-384 keygen setup failed\n"), 1;
        report("ecdh keygen P-384", bench(do_ecdh_keygen, (void *)384L), 0);
        report("ecdh shared P-384", bench(do_ecdh_shared, (void *)384L), 0);

        printf("\n-- signature verification --\n");
        ecdh_keygen(256, ec_priv, 0x40000001u, ec_pub);
        report("ecdsa verify P-256", bench(do_ecdsa, (void *)256L), 0);
        ecdh_keygen(384, ec_priv, 0x40000001u, ec_pub);
        report("ecdsa verify P-384", bench(do_ecdsa, (void *)384L), 0);
    } else {
        printf("\n-- signature verification --\n");
    }

    double r2 = bench(do_rsa, (void *)256L);      /* 2048-bit */
    double r3 = bench(do_rsa, (void *)384L);      /* 3072-bit */
    double r4 = bench(do_rsa, (void *)512L);      /* 4096-bit */
    report("rsa verify 2048", r2, 0);
    report("rsa verify 3072", r3, 0);
    report("rsa verify 4096", r4, 0);

    int nrsa = 0, nec = 0;
    for (int i = 0; i < logit_nroots; i++)
        (logit_roots[i].type == ROOT_RSA) ? nrsa++ : nec++;
    printf("\n-- trust store (%d roots: %d RSA, %d EC) --\n", logit_nroots, nrsa, nec);
    report("signed_by_root full RSA scan", bench(do_root_scan, 0), 0);
    printf("  (that is the cost of ONE chain whose anchor is not sent in-band)\n");

    return 0;
}
