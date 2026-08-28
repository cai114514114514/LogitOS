/* scrypt (RFC 7914) known-answer tests.
 *
 * Every vector below is transcribed from the RFC's own text (sections 8, 9,
 * 10 and 12), fetched from https://www.rfc-editor.org/rfc/rfc7914.txt and
 * checked digit-for-digit against the printed hex -- not regenerated from
 * this implementation. Test 1's derived key was additionally cross-checked
 * against `openssl kdf -keylen 64 -kdfopt pass: -kdfopt salt: -kdfopt n:16
 * -kdfopt r:1 -kdfopt p:1 SCRYPT` (OpenSSL 3.6.3): identical.
 *
 * Sections 8/9/10 are the STANDALONE Salsa20/8 core, scryptBlockMix and
 * scryptROMix vectors -- each is the first (i=1) internal value the
 * section-12 test 1 full computation produces, so a mismatch in test 1 with
 * these three passing narrows the bug to the PBKDF2 wrapper, not the
 * memory-hard core.
 *
 * Sections 8, 9 and 10 all use r=1, which is exactly the value at which
 * scryptBlockMix's interleaved output order and the naive "produced" order
 * are IDENTICAL (see scrypt.c's SCRYPT_BREAK_INTERLEAVE comment) -- so those
 * three vectors, and even the full r=1 scrypt vector (test 1), CANNOT by
 * themselves prove the interleave is right. Only the r=8 vectors (tests 2
 * and 3) can, which is the whole reason this file's negative control
 * (run-scrypt-negctl.sh) demands they be included and watches them
 * specifically -- see that script's header for what was watched failing.
 *
 * Test 4 (N=1048576, r=8) needs a bit over 1 GiB of scratch and is gated
 * behind SCRYPT_TEST_BIG=1 in the environment. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "scrypt.h"

static int fails, passes;
static void ok(const char *name, int cond)
{ if (cond) { passes++; printf("ok   %s\n", name); } else { fails++; printf("FAIL %s\n", name); } }

static int hv(char c)
{ if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; return c - 'A' + 10; }

/* Whitespace-tolerant: the RFC's own hex is broken across 16-octet lines with
 * leading label indentation, and reflowing it into a C string literal is
 * exactly the kind of manual step that drops or duplicates a nibble. This
 * skips anything that is not a hex digit rather than assume the literal below
 * has none, so a stray space or newline in the source cannot silently shift
 * every following byte by one nibble. */
static int unhex(const char *h, uint8_t *o)
{
    int n = 0, have = 0; unsigned v = 0;
    for (const char *p = h; *p; p++) {
        char c = *p;
        int isx = (c>='0'&&c<='9') || (c>='a'&&c<='f') || (c>='A'&&c<='F');
        if (!isx) continue;
        v = (v << 4) | (unsigned)hv(c);
        if (have) { o[n++] = (uint8_t)v; v = 0; have = 0; }
        else have = 1;
    }
    return n;
}
static int eq(const uint8_t *a, const uint8_t *b, int n) { return memcmp(a, b, (size_t)n) == 0; }

/* --------------------------------------------------- RFC 7914 s8 -- Salsa20/8 core */
static void test_salsa20_8(void)
{
    uint8_t in[64], want[64], out[64];
    unhex("7e 87 9a 21 4f 3e c9 86 7c a9 40 e6 41 71 8f 26"
          "ba ee 55 5b 8c 61 c1 b5 0d f8 46 11 6d cd 3b 1d"
          "ee 24 f3 19 df 9b 3d 85 14 12 1e 4b 5a c5 aa 32"
          "76 02 1d 29 09 c7 48 29 ed eb c6 8d b8 b8 c2 5e", in);
    unhex("a4 1f 85 9c 66 08 cc 99 3b 81 ca cb 02 0c ef 05"
          "04 4b 21 81 a2 fd 33 7d fd 7b 1c 63 96 68 2f 29"
          "b4 39 31 68 e3 c9 e6 bc fe 6b c5 b7 a0 6d 96 ba"
          "e4 24 cc 10 2c 91 74 5c 24 ad 67 3d c7 61 8f 81", want);
    salsa20_8_core(out, in);
    ok("salsa20/8 core (RFC 7914 s8)", eq(out, want, 64));

    /* out == in must give the same answer -- see scrypt.c's comment on why
     * the final sum reads a pre-round copy rather than `in` itself. */
    uint8_t io[64]; memcpy(io, in, 64);
    salsa20_8_core(io, io);
    ok("salsa20/8 core, in-place", eq(io, want, 64));
}

/* --------------------------------------------------- RFC 7914 s9 -- scryptBlockMix, r=1 */
static void test_blockmix_r1(void)
{
    uint8_t B[128], want[128], out[128];
    unhex("f7 ce 0b 65 3d 2d 72 a4 10 8c f5 ab e9 12 ff dd"
          "77 76 16 db bb 27 a7 0e 82 04 f3 ae 2d 0f 6f ad"
          "89 f6 8f 48 11 d1 e8 7b cc 3b d7 40 0a 9f fd 29"
          "09 4f 01 84 63 95 74 f3 9a e5 a1 31 52 17 bc d7", B);       /* B[0] */
    unhex("89 49 91 44 72 13 bb 22 6c 25 b5 4d a8 63 70 fb"
          "cd 98 43 80 37 46 66 bb 8f fc b5 bf 40 c2 54 b0"
          "67 d2 7c 51 ce 4a d5 fe d8 29 c9 0b 50 5a 57 1b"
          "7f 4d 1c ad 6a 52 3c da 77 0e 67 bc ea af 7e 89", B + 64);  /* B[1] */
    unhex("a4 1f 85 9c 66 08 cc 99 3b 81 ca cb 02 0c ef 05"
          "04 4b 21 81 a2 fd 33 7d fd 7b 1c 63 96 68 2f 29"
          "b4 39 31 68 e3 c9 e6 bc fe 6b c5 b7 a0 6d 96 ba"
          "e4 24 cc 10 2c 91 74 5c 24 ad 67 3d c7 61 8f 81", want);       /* B'[0] */
    unhex("20 ed c9 75 32 38 81 a8 05 40 f6 4c 16 2d cd 3c"
          "21 07 7c fe 5f 8d 5f e2 b1 a4 16 8f 95 36 78 b7"
          "7d 3b 3d 80 3b 60 e4 ab 92 09 96 e5 9b 4d 53 b6"
          "5d 2a 22 58 77 d5 ed f5 84 2c b9 f1 4e ef e4 25", want + 64);  /* B'[1] */

    scrypt_blockmix(1, B, out);
    ok("scryptBlockMix r=1 (RFC 7914 s9)", eq(out, want, 128));
}

/* --------------------------------------------------- RFC 7914 s10 -- scryptROMix, r=1, N=16 */
static void test_romix_r1_n16(void)
{
    uint8_t B[128], want[128];
    unhex("f7 ce 0b 65 3d 2d 72 a4 10 8c f5 ab e9 12 ff dd"
          "77 76 16 db bb 27 a7 0e 82 04 f3 ae 2d 0f 6f ad"
          "89 f6 8f 48 11 d1 e8 7b cc 3b d7 40 0a 9f fd 29"
          "09 4f 01 84 63 95 74 f3 9a e5 a1 31 52 17 bc d7"
          "89 49 91 44 72 13 bb 22 6c 25 b5 4d a8 63 70 fb"
          "cd 98 43 80 37 46 66 bb 8f fc b5 bf 40 c2 54 b0"
          "67 d2 7c 51 ce 4a d5 fe d8 29 c9 0b 50 5a 57 1b"
          "7f 4d 1c ad 6a 52 3c da 77 0e 67 bc ea af 7e 89", B);
    unhex("79 cc c1 93 62 9d eb ca 04 7f 0b 70 60 4b f6 b6"
          "2c e3 dd 4a 96 26 e3 55 fa fc 61 98 e6 ea 2b 46"
          "d5 84 13 67 3b 99 b0 29 d6 65 c3 57 60 1f b4 26"
          "a0 b2 f4 bb a2 00 ee 9f 0a 43 d1 9b 57 1a 9c 71"
          "ef 11 42 e6 5d 5a 26 6f dd ca 83 2c e5 9f aa 7c"
          "ac 0b 9c f1 be 2b ff ca 30 0d 01 ee 38 76 19 c4"
          "ae 12 fd 44 38 f2 03 a0 e4 e1 c4 7e c3 14 86 1f"
          "4e 90 87 cb 33 39 6a 68 73 e8 f9 d2 53 9a 4b 8e", want);

    uint64_t need = scrypt_romix_scratch_len(1, 16);
    uint8_t *scratch = malloc((size_t)need);
    if (!scratch) { ok("scryptROMix r=1,N=16 (RFC 7914 s10) -- malloc", 0); return; }
    int rc = scrypt_romix(1, 16, B, scratch, need);
    ok("scryptROMix r=1,N=16 (RFC 7914 s10)", rc == 0 && eq(B, want, 128));
    free(scratch);
}

static void run_scrypt_kat(const char *name, const char *pw, const char *salt,
                            uint64_t N, uint32_t r, uint32_t p, const char *want_hex)
{
    uint8_t want[64];
    int wlen = unhex(want_hex, want);
    uint8_t dk[64];
    uint64_t need = scrypt_scratch_len(N, r, p);
    uint8_t *scratch = malloc((size_t)need);
    if (!scratch) { printf("SKIP %s (malloc %llu failed)\n", name, (unsigned long long)need); return; }
    int rc = scrypt((const uint8_t *)pw, (int)strlen(pw), (const uint8_t *)salt, (int)strlen(salt),
                     N, r, p, dk, (uint64_t)wlen, scratch, need);
    ok(name, rc == 0 && eq(dk, want, wlen));
    free(scratch);
}

/* --------------------------------------------------- RFC 7914 s12 -- full scrypt */
static void test_scrypt_full(void)
{
    run_scrypt_kat("scrypt test 1 (N=16,r=1,p=1)", "", "", 16, 1, 1,
        "77 d6 57 62 38 65 7b 20 3b 19 ca 42 c1 8a 04 97"
        "f1 6b 48 44 e3 07 4a e8 df df fa 3f ed e2 14 42"
        "fc d0 06 9d ed 09 48 f8 32 6a 75 3a 0f c8 1f 17"
        "e8 d3 e0 fb 2e 0d 36 28 cf 35 e2 0c 38 d1 89 06");

    run_scrypt_kat("scrypt test 2 (N=1024,r=8,p=16)", "password", "NaCl", 1024, 8, 16,
        "fd ba be 1c 9d 34 72 00 78 56 e7 19 0d 01 e9 fe"
        "7c 6a d7 cb c8 23 78 30 e7 73 76 63 4b 37 31 62"
        "2e af 30 d9 2e 22 a3 88 6f f1 09 27 9d 98 30 da"
        "c7 27 af b9 4a 83 ee 6d 83 60 cb df a2 cc 06 40");

    run_scrypt_kat("scrypt test 3 (N=16384,r=8,p=1)", "pleaseletmein", "SodiumChloride", 16384, 8, 1,
        "70 23 bd cb 3a fd 73 48 46 1c 06 cd 81 fd 38 eb"
        "fd a8 fb ba 90 4f 8e 3e a9 b5 43 f6 54 5d a1 f2"
        "d5 43 29 55 61 3f 0f cf 62 d4 97 05 24 2a 9a f9"
        "e6 1e 85 dc 0d 65 1e 40 df cf 01 7b 45 57 58 87");

    if (getenv("SCRYPT_TEST_BIG")) {
        run_scrypt_kat("scrypt test 4 (N=1048576,r=8,p=1) [SCRYPT_TEST_BIG=1]",
            "pleaseletmein", "SodiumChloride", 1048576, 8, 1,
            "21 01 cb 9b 6a 51 1a ae ad db be 09 cf 70 f8 81"
            "ec 56 8d 57 4a 2f fd 4d ab e5 ee 98 20 ad aa 47"
            "8e 56 fd 8f 4b a5 d0 9f fa 1c 6d 92 7c 40 f4 c3"
            "37 30 40 49 e8 a9 52 fb cb f4 5c 6f a7 7a 41 a4");
    } else {
        printf("SKIP scrypt test 4 (N=1048576,r=8,p=1) -- needs ~1 GiB scratch, "
               "set SCRYPT_TEST_BIG=1 to run it\n");
    }
}

/* --------------------------------------------------- parameter validation, not stubbed to success */
static void test_bad_params(void)
{
    uint8_t dk[32], scratch[4096];
    /* N not a power of two */
    ok("scrypt rejects N=15 (not power of two)",
       scrypt((const uint8_t*)"x",1,(const uint8_t*)"y",1, 15,1,1, dk,32, scratch,sizeof scratch) == -1);
    /* N <= 1 */
    ok("scrypt rejects N=1", scrypt((const uint8_t*)"x",1,(const uint8_t*)"y",1, 1,1,1, dk,32, scratch,sizeof scratch) == -1);
    /* undersized scratch must be REFUSED, not silently overrun */
    uint64_t need = scrypt_scratch_len(16, 1, 1);
    ok("scrypt rejects undersized scratch",
       scrypt((const uint8_t*)"x",1,(const uint8_t*)"y",1, 16,1,1, dk,32, scratch, need - 1) == -1);
    ok("scrypt accepts exactly-sized scratch",
       need <= sizeof scratch ? scrypt((const uint8_t*)"x",1,(const uint8_t*)"y",1, 16,1,1, dk,32, scratch, need) == 0 : 1);
}

int main(void)
{
    test_salsa20_8();
    test_blockmix_r1();
    test_romix_r1_n16();
    test_scrypt_full();
    test_bad_params();
    printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
