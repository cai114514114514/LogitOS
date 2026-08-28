/* Argon2 (RFC 9106) known-answer test.
 *
 * THREE OFFICIAL SOURCES, as required:
 *   (1) RFC 9106 section 5.1 (Argon2d), 5.2 (Argon2i) and 5.3 (Argon2id) --
 *       the SAME password/salt/secret/associated-data, t=3, m=32 KiB, p=4,
 *       32-byte tag, run through the SAME code with only `type` changed.
 *       Both secret and associated data are non-empty, which the task brief
 *       correctly notes most from-scratch implementations never exercise.
 *   (2) The PHC reference implementation's own KAT file for Argon2id
 *       (https://raw.githubusercontent.com/P-H-C/phc-winner-argon2/master/
 *       kats/argon2id -- the RFC 9106 5.3 vector with full per-pass block
 *       state), embedded in argon2_id_kat.inc. Checked via the
 *       ARGON2_TEST_HOOKS seam so a bug that only shows up mid-computation
 *       (and happens to cancel out by the time the last blocks get XORed
 *       into the tag) cannot hide behind a correct final tag.
 *   (3) A differential against OpenSSL 3.6.3's EVP_KDF ARGON2D/ARGON2I/
 *       ARGON2ID, across randomised (type, t_cost, m_cost, lanes) -- see
 *       run-argon2-openssl.sh and argon2_cli.c. The RFC vector alone is
 *       always p=4; the differential is what exercises p=1 (where the
 *       same-lane/other-lane branches in argon2_index_alpha collapse to one
 *       case) and p=2,3 (where they do not), which argon2.c's own comment on
 *       that function explains is exactly where an off-by-one in the
 *       reference window would hide.
 *
 * This binary does ONLY (1) and (2); (3) is a separate CLI + shell script,
 * matching the shape tests/unit/mlkem_test.c (RFC/FIPS vectors) and
 * tests/unit/run-mlkem-openssl.sh (the differential) already split into.
 *
 * NEGATIVE CONTROL. Compiled with -DARGON2_NEGCTL_ALWAYS_DATA_DEP (passed to
 * BOTH this file and argon2.c -- see the make rule at the bottom of this
 * file's neighbour argon2.c), argon2.c forces every block to use
 * data-DEPENDENT addressing, i.e. Argon2i and Argon2id both degrade to
 * Argon2d's addressing throughout. This file knows about that flag too, so
 * ONE binary serves both gates: built plain, it expects all three types
 * correct and exits 0 only if they are; built with the flag, it expects
 * Argon2d to remain correct (untouched by the flag -- Argon2d never used
 * independent addressing) and Argon2i/Argon2id to come out WRONG, and exits
 * 0 only if that specific pattern holds. Either way, exit 0 means "the
 * binary behaved exactly as this build should", so `run-argon2-negctl.sh`
 * (or just this binary run twice) has one clean signal instead of having to
 * parse pass/fail text. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "argon2.h"

#ifdef ARGON2_TEST_HOOKS
extern void (*argon2_test_after_pass)(uint32_t pass, const struct argon2_block *mem,
                                       uint32_t lane_length, uint32_t lanes);
#include "argon2_id_kat.inc"

static int g_deep_ok = 1;
static int g_deep_ran = 0;

static void after_pass_cb(uint32_t pass, const struct argon2_block *mem,
                           uint32_t lane_length, uint32_t lanes)
{
    if (pass > 2 || lanes != 4) return;
    g_deep_ran = 1;
    const uint64_t *want0, *want31;
    switch (pass) {
        case 0: want0 = argon2id_kat_p0_b0; want31 = argon2id_kat_p0_b31; break;
        case 1: want0 = argon2id_kat_p1_b0; want31 = argon2id_kat_p1_b31; break;
        default: want0 = argon2id_kat_p2_b0; want31 = argon2id_kat_p2_b31; break;
    }
    const struct argon2_block *b0  = &mem[0];
    const struct argon2_block *b31 = &mem[3 * lane_length + 7];
    for (int i = 0; i < ARGON2_QWORDS; i++) {
        if (b0->v[i] != want0[i]) {
            printf("  DEEP MISMATCH pass %u block0 word %d: got %016llx want %016llx\n",
                   pass, i, (unsigned long long)b0->v[i], (unsigned long long)want0[i]);
            g_deep_ok = 0;
        }
        if (b31->v[i] != want31[i]) {
            printf("  DEEP MISMATCH pass %u block31 word %d: got %016llx want %016llx\n",
                   pass, i, (unsigned long long)b31->v[i], (unsigned long long)want31[i]);
            g_deep_ok = 0;
        }
    }
}
#endif

static void hex2bin(const char *hex, uint8_t *out, int outlen)
{
    for (int i = 0; i < outlen; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

/* Runs argon2(type, ...) over the RFC 9106 5.1/5.2/5.3 shared input and
 * compares the 32-byte tag against `want_hex`. Returns 1 if it matches. */
static int check_tag(enum argon2_type type, const char *name, const char *want_hex,
                      int install_deep_hook)
{
    uint8_t pw[32], salt[16], secret[8], ad[12];
    memset(pw, 0x01, sizeof pw);
    memset(salt, 0x02, sizeof salt);
    memset(secret, 0x03, sizeof secret);
    memset(ad, 0x04, sizeof ad);

    uint32_t t_cost = 3, m_cost = 32, lanes = 4, taglen = 32;
    uint32_t blocks = argon2_memory_blocks(m_cost, lanes);
    if (blocks != 32) { printf("FAIL %s: argon2_memory_blocks(32,4) = %u, want 32\n", name, blocks); return 0; }

    struct argon2_block *mem = malloc((size_t)blocks * sizeof *mem);
    if (!mem) { printf("FAIL %s: out of host memory\n", name); return 0; }

#ifdef ARGON2_TEST_HOOKS
    g_deep_ok = 1; g_deep_ran = 0;
    argon2_test_after_pass = install_deep_hook ? after_pass_cb : 0;
#else
    (void)install_deep_hook;
#endif

    uint8_t tag[32];
    int rc = argon2(type, pw, sizeof pw, salt, sizeof salt, secret, sizeof secret,
                     ad, sizeof ad, t_cost, m_cost, lanes, mem, blocks, tag, taglen);

#ifdef ARGON2_TEST_HOOKS
    argon2_test_after_pass = 0;
#endif
    free(mem);

    if (rc != 0) { printf("FAIL %s: argon2() returned %d, want 0\n", name, rc); return 0; }

    uint8_t want[32];
    hex2bin(want_hex, want, 32);
    int ok = (memcmp(tag, want, 32) == 0);
    if (!ok) {
        printf("FAIL %s: tag mismatch\n  got  ", name);
        for (int i = 0; i < 32; i++) printf("%02x", tag[i]);
        printf("\n  want %s\n", want_hex);
    }

#ifdef ARGON2_TEST_HOOKS
    if (install_deep_hook) {
        if (!g_deep_ran) { printf("FAIL %s: deep hook never fired\n", name); ok = 0; }
        else if (!g_deep_ok) { printf("FAIL %s: per-pass block state disagreed with the PHC KAT\n", name); ok = 0; }
        else printf("  (deep per-pass block check against the PHC KAT: agrees)\n");
    }
#endif

    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    return ok;
}

/* argon2_memory_blocks() rounding: RFC 9106's own floor and multiple-of-4p
 * rule, checked against hand-worked values independent of argon2()'s own
 * internals. */
static int check_rounding(void)
{
    struct { uint32_t m, lanes, want; } cases[] = {
        {32, 4, 32},        /* already a multiple of 4*lanes=16 */
        {1, 1, 8},          /* below the 2*SYNC_POINTS*lanes=8 floor */
        {100, 1, 100},      /* 100 is already a multiple of 4 */
        {101, 1, 100},      /* rounds DOWN to the nearest multiple of 4 */
        {65536, 1, 65536},
        {1000, 4, 992},     /* 1000/16=62.5 -> 62*16=992 */
    };
    int ok = 1;
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint32_t got = argon2_memory_blocks(cases[i].m, cases[i].lanes);
        if (got != cases[i].want) {
            printf("FAIL rounding: m=%u lanes=%u got %u want %u\n",
                   cases[i].m, cases[i].lanes, got, cases[i].want);
            ok = 0;
        }
    }
    if (argon2_memory_blocks(32, 0) != 0) { printf("FAIL rounding: lanes=0 must return 0\n"); ok = 0; }
    printf("%s rounding\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* The caller-supplied-scratch refusal: mem_cap smaller than
 * argon2_memory_blocks() computes must return -2 and touch nothing. */
static int check_refuses_small_buffer(void)
{
    uint8_t pw[1] = {0}, salt[8] = {0}, tag[32];
    uint32_t blocks = argon2_memory_blocks(32, 4);   /* 32 */
    struct argon2_block *mem = malloc((size_t)(blocks - 1) * sizeof *mem);
    /* Sentinel so we can prove the (undersized) buffer was never touched. */
    memset(mem, 0xAA, (size_t)(blocks - 1) * sizeof *mem);
    int rc = argon2(ARGON2_ID, pw, 1, salt, 8, 0, 0, 0, 0, 3, 32, 4,
                     mem, blocks - 1, tag, 32);
    int ok = (rc == -2);
    if (ok) {
        uint8_t *b = (uint8_t *)mem;
        for (size_t i = 0; i < (size_t)(blocks - 1) * sizeof *mem; i++)
            if (b[i] != 0xAA) { ok = 0; break; }
        if (!ok) printf("FAIL refuse-small-buffer: mem was written despite -2\n");
    } else {
        printf("FAIL refuse-small-buffer: argon2() returned %d, want -2\n", rc);
    }
    free(mem);
    printf("%s refuse-small-buffer\n", ok ? "PASS" : "FAIL");
    return ok;
}

int main(void)
{
    int ok = 1;

    ok &= check_rounding();
    ok &= check_refuses_small_buffer();

    int d_ok  = check_tag(ARGON2_D,  "argon2d (RFC 9106 5.1)",
                           "512b391b6f1162975371d30919734294f868e3be3984f3c1a13a4db9fabe4acb", 0);
    int i_ok  = check_tag(ARGON2_I,  "argon2i (RFC 9106 5.2)",
                           "c814d9d1dc7f37aa13f0d77f2494bda1c8de6b016dd388d29952a4c4672b6ce8", 0);
    int id_ok = check_tag(ARGON2_ID, "argon2id (RFC 9106 5.3)",
                           "0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659", 1);

#ifdef ARGON2_NEGCTL_ALWAYS_DATA_DEP
    /* This build forces data-dependent addressing everywhere. Argon2d must
     * be UNAFFECTED (it never used the other mode); Argon2i and Argon2id
     * MUST be wrong -- if either still matched the RFC tag, the flag failed
     * to reach the code it is supposed to corrupt and this "control" would
     * be satisfied by nothing at all, exactly the trap CLAUDE.md names. */
    printf("-- ARGON2_NEGCTL_ALWAYS_DATA_DEP build: expecting d correct, i/id WRONG --\n");
    ok &= d_ok;
    ok &= !i_ok;
    ok &= !id_ok;
    if (i_ok || id_ok)
        printf("FAIL negctl: expected argon2i/argon2id to diverge from the RFC vector and they did not\n");
    if (!d_ok)
        printf("FAIL negctl: argon2d should have been UNAFFECTED by this flag and was not\n");
#else
    ok &= d_ok;
    ok &= i_ok;
    ok &= id_ok;
#endif

    printf(ok ? "ALL OK\n" : "SOME CHECKS FAILED\n");
    return ok ? 0 : 1;
}
