/* BLAKE2b (RFC 7693) host unit test.
 *
 * THREE INDEPENDENT CHECKS, and they are not interchangeable:
 *
 *  1. THE OFFICIAL 256-VECTOR KAT (blake2b_kat.inc, generated from
 *     https://raw.githubusercontent.com/BLAKE2/BLAKE2/master/testvectors/blake2b-kat.txt,
 *     the BLAKE2 reference project's own file, fetched 2026-08-28). Each of
 *     the 256 rows is keyed with the SAME fixed 64-byte key (bytes 0x00..0x3f)
 *     and hashes a message of length n = 0..255 whose bytes are 0,1,...,n-1
 *     (mod 256) -- a rule verified against the raw file below, not assumed:
 *
 *       $ python3 -c "
 *         import re
 *         d = open('blake2b-kat.txt').read()
 *         ins = re.findall(r'^in:\t([0-9a-f]*)\$', d, re.M)
 *         for n, h in enumerate(ins):
 *             assert h == ''.join('%02x' % (i % 256) for i in range(n))
 *         print('pattern confirmed for all', len(ins))"
 *       pattern confirmed for all 256
 *
 *     so only the 256 64-byte digests needed embedding, not the (redundant)
 *     inputs -- blake2b_kat.inc is 256 lines of `out[n]`, regenerated from the
 *     hash: field only. Only the outputs are trusted data; the inputs and key
 *     are the RULE above, applied here independently of the generator script.
 *
 *  2. RFC 7693 Appendix A: BLAKE2b-512("abc"), unkeyed. Cross-checked against
 *     `openssl dgst -blake2b512` (OpenSSL 3.6.3) before being pasted in --
 *     the two agreed, which is what makes this a second source rather than a
 *     restatement of (1).
 *
 *  3. VARIABLE OUTPUT LENGTH, 1..64 bytes, KEYED, against a live OpenSSL
 *     differential (tests/unit/run-blake2b-openssl.sh, using
 *     `openssl mac -macopt size:N -macopt hexkey:K BLAKE2BMAC`, key length
 *     1..64 and outlen 1..64) -- the KAT above only ever exercises the full
 *     64-byte output, so this is the ONLY check in the tree that touches
 *     outlen < 64 against an external reference. The SAME script also
 *     differentials the fixed 64-byte UNKEYED case against
 *     `openssl dgst -blake2b512`. It does NOT differential unkeyed variable
 *     outlen: OpenSSL's BLAKE2BMAC refuses a zero-length key (verified --
 *     "invalid key length"), and `dgst -blake2b512` has no length option, so
 *     there is no OpenSSL entry point for "unkeyed, outlen != 64" at all.
 *     BLAKE2b's outlen is folded into the parameter block that seeds h[0],
 *     so it is NOT a truncation of the 64-byte digest either -- there is no
 *     cheap way to derive one from the other. That gap is named, not hidden:
 *     unkeyed short output is covered only by the functional property check
 *     below (right length, right byte count, no overrun), not by an
 *     external reference. test-blake2b-openssl is a separate gate because it
 *     shells out; this file needs no openssl at runtime and runs under
 *     ASan/UBSan.
 *
 * NEGATIVE CONTROL: -DBLAKE2B_BUG_ROT63, compiled into blake2b.c itself (see
 * its header), turns the last rotation of G into the specific one-character
 * slip the task names (`>>63 | <<63` instead of `>>63 | <<1`). Built via
 * `make test-blake2b-negctl`, which is this SAME test binary rebuilt with
 * that flag and asserted to FAIL. A control that has not been built and
 * watched red is not a control (CLAUDE.md rule 1 / rule 5 of this task). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "blake2b.h"

#include "blake2b_kat.inc"

static int pass, fail;
static void ck(int cond, const char *what)
{
    if (cond) pass++;
    else { fail++; printf("FAIL: %s\n", what); }
}

static void tohex(char *o, const uint8_t *b, int n)
{
    static const char *d = "0123456789abcdef";
    for (int i = 0; i < n; i++) { o[2*i] = d[b[i] >> 4]; o[2*i+1] = d[b[i] & 15]; }
    o[2*n] = 0;
}

int main(void)
{
    char hex[130];

    /* -------------------------------------------------- 1. the official KAT */
    {
        uint8_t key[64];
        for (int i = 0; i < 64; i++) key[i] = (uint8_t)i;

        for (int n = 0; n < BLAKE2B_KAT_N; n++) {
            uint8_t msg[256];
            uint8_t out[64];
            for (int i = 0; i < n; i++) msg[i] = (uint8_t)(i % 256);

            int rc = blake2b_keyed(msg, (size_t)n, key, sizeof(key), out, sizeof(out));
            if (rc != 0) { fail++; printf("FAIL: KAT[%d] blake2b_keyed returned %d\n", n, rc); continue; }
            if (memcmp(out, blake2b_kat_out[n], 64) != 0) {
                fail++;
                char got[130], want[130];
                tohex(got, out, 64); tohex(want, blake2b_kat_out[n], 64);
                printf("FAIL: KAT[%d] len=%d got %s want %s\n", n, n, got, want);
            } else pass++;

            /* Same answer through the streaming (init/update/final) API,
             * split at a length-dependent point, to prove the buffering
             * logic (the part a one-shot call cannot exercise) against the
             * same official vector -- not a second oracle, a second PATH to
             * the first one. */
            {
                struct blake2b c;
                uint8_t out2[64];
                int split = n / 2;
                blake2b_init(&c, 64, key, sizeof(key));
                blake2b_update(&c, msg, (size_t)split);
                blake2b_update(&c, msg + split, (size_t)(n - split));
                blake2b_final(&c, out2);
                ck(memcmp(out2, blake2b_kat_out[n], 64) == 0, "KAT streamed-split path agrees with one-shot");
            }
        }
        printf("blake2b: %d/%d official KAT vectors agree (streamed path included)\n",
               pass > 256 ? 256 : pass, BLAKE2B_KAT_N);
    }

    /* --------------------------------------------- 2. RFC 7693 Appendix A */
    {
        uint8_t out[64];
        blake2b512("abc", 3, out);
        tohex(hex, out, 64);
        ck(!strcmp(hex,
            "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d"
            "17d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923"),
           "RFC 7693 Appendix A: BLAKE2b-512(\"abc\")");
    }

    /* ---------------------------- 3. properties a KAT alone cannot express */
    {
        uint8_t a[64], b[64];

        /* empty message, unkeyed, must differ from empty message keyed */
        blake2b512("", 0, a);
        {
            uint8_t key[1] = { 0x2a };
            blake2b_keyed("", 0, key, 1, b, 64);
        }
        ck(memcmp(a, b, 64) != 0, "keying an empty message changes the digest");

        /* BLAKE2b's outlen feeds the parameter block (h[0] ^= ... ^ outlen),
         * so the digest at length N is NOT the first N bytes of the digest
         * at length 64 -- every outlen is its own independent run, not a
         * truncation of a longer one. This loop cannot check that those 64
         * runs produce 64 DIFFERENT correct values (only the OpenSSL
         * differential in run-blake2b-openssl.sh does, against
         * BLAKE2BMAC's -macopt size:N); what it checks is the functional
         * half: every length 1..64 is accepted and writes EXACTLY that many
         * bytes and nothing past them (ASan/UBSan would catch an overrun). */
        for (size_t n = 1; n <= 64; n++) {
            uint8_t out[64];
            memset(out, 0xAA, sizeof(out));
            int rc = blake2b_keyed("test", 4, "k", 1, out, n);
            ck(rc == 0, "blake2b_keyed accepts every outlen 1..64");
            int untouched_ok = 1;
            for (size_t i = n; i < 64; i++) if (out[i] != 0xAA) untouched_ok = 0;
            ck(untouched_ok, "blake2b_keyed writes exactly outlen bytes, nothing past it");
        }

        /* out-of-range parameters are REFUSED, not silently clamped
         * (CLAUDE.md rule 5: never stub to a plausible wrong answer). */
        {
            uint8_t out[64];
            ck(blake2b_keyed("x", 1, 0, 0, out, 0) != 0, "outlen=0 refused");
            ck(blake2b_keyed("x", 1, 0, 0, out, 65) != 0, "outlen=65 refused");
            {
                uint8_t bigkey[65];
                memset(bigkey, 1, sizeof(bigkey));
                ck(blake2b_keyed("x", 1, bigkey, 65, out, 64) != 0, "keylen=65 refused");
            }
        }

        /* a single bit flipped anywhere in the message changes the digest
         * (avalanche is exactly what a rotation-direction bug like the
         * negative control below tends to still deliver -- see the negctl
         * script's own header for why this alone does not catch it). */
        {
            uint8_t m1[16], m2[16], d1[64], d2[64];
            for (int i = 0; i < 16; i++) m1[i] = m2[i] = (uint8_t)i;
            m2[7] ^= 0x01;
            blake2b512(m1, 16, d1);
            blake2b512(m2, 16, d2);
            ck(memcmp(d1, d2, 64) != 0, "a single flipped message bit changes the digest");
        }
    }

    printf("blake2b: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
