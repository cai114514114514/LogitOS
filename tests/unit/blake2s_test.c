/* tests/unit/blake2s_test.c -- BLAKE2s (RFC 7693) known-answer test.
 *
 * Two independent official sources, both checked here:
 *
 *   1. RFC 7693 Appendix B: BLAKE2s("abc"), unkeyed, 32-byte output. This is
 *      the spec's own worked example -- useful, but on its own it CANNOT
 *      distinguish a correct parameter-block computation from the hardcoded
 *      shortcut 0x01010020 (see blake2s.c), because that shortcut is exactly
 *      "unkeyed, 32-byte output".
 *
 *   2. build/blake2-kat/blake2s-kat.txt -- the BLAKE2 reference
 *      implementation's own testvectors/blake2s-kat.txt (256 vectors, fetched
 *      by tools/blake2_fetch.sh at the pin in tools/blake2_revision.txt).
 *      EVERY vector is KEYED (32-byte key 00..1f) and input length runs
 *      0..255 bytes -- the one shape of corpus that (1) cannot satisfy: it
 *      exercises the keyed parameter word, the padded key block being
 *      counted into t, and every input length relative to the 64-byte block
 *      boundary (0, mid-block, exactly one block, one-block-plus-one, several
 *      blocks). A missing corpus is a missing CAPABILITY, not a code defect
 *      -- this SKIPS (exit 0) rather than failing, per CLAUDE.md rule 5 /
 *      the test-wpt precedent (WPT_ROOT absent -> "the runner says so and
 *      exits 0").
 *
 * Usage: blake2s_test [path-to-blake2s-kat.txt]
 *        default: build/blake2-kat/blake2s-kat.txt (relative to CWD, which
 *        the Makefile recipe sets to the repo root).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blake2s.h"

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decodes hex text (no separators, even length) into buf; returns byte count,
 * or -1 on malformed input. buf must be at least strlen(hex)/2 bytes. */
static long hexdecode(const char *hex, uint8_t *buf, size_t bufcap)
{
    size_t n = strlen(hex);
    if (n % 2 != 0) return -1;
    size_t nbytes = n / 2;
    if (nbytes > bufcap) return -1;
    for (size_t i = 0; i < nbytes; i++) {
        int hi = hexval(hex[2*i]), lo = hexval(hex[2*i+1]);
        if (hi < 0 || lo < 0) return -1;
        buf[i] = (uint8_t)((hi << 4) | lo);
    }
    return (long)nbytes;
}

static int check_appendix_b(void)
{
    /* RFC 7693 Appendix B, worked example: BLAKE2s("abc"), unkeyed, 32-byte
     * digest. Also independently reproduced against this tree's OpenSSL
     * 3.6.3 (`openssl dgst -blake2s256`), which agrees byte for byte. */
    static const uint8_t want[32] = {
        0x50,0x8c,0x5e,0x8c,0x32,0x7c,0x14,0xe2,0xe1,0xa7,0x2b,0xa3,0x4e,0xeb,
        0x45,0x2f,0x37,0x45,0x8b,0x20,0x9e,0xd6,0x3a,0x29,0x4d,0x99,0x9b,0x4c,
        0x86,0x67,0x59,0x82,
    };
    uint8_t got[32];
    blake2s((const uint8_t *)"abc", 3, NULL, 0, got, 32);
    if (memcmp(got, want, 32) != 0) {
        fprintf(stderr, "FAIL RFC 7693 Appendix B: BLAKE2s(\"abc\") mismatch\n");
        fprintf(stderr, "  want: "); for (int i=0;i<32;i++) fprintf(stderr, "%02x", want[i]); fprintf(stderr, "\n");
        fprintf(stderr, "  got:  "); for (int i=0;i<32;i++) fprintf(stderr, "%02x", got[i]); fprintf(stderr, "\n");
        return 0;
    }
    printf("PASS RFC 7693 Appendix B: BLAKE2s(\"abc\") = 508c5e8c...675982\n");
    return 1;
}

/* Reads one line into buf, stripping the trailing newline(s). Returns 0 at
 * EOF, 1 otherwise. */
static int getline_trim(char *buf, size_t cap, FILE *f)
{
    if (!fgets(buf, (int)cap, f)) return 0;
    size_t n = strlen(buf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
    return 1;
}

/* A "key: "/"in: " field's value starts after the label and a separator
 * (colon then whitespace, which in the upstream file is a single tab -- but
 * this does not hardcode that: it skips ANY run of whitespace, so a
 * re-wrapped or re-indented copy of the same corpus still parses). */
static const char *field_value(const char *line, const char *label)
{
    size_t ll = strlen(label);
    if (strncmp(line, label, ll) != 0) return NULL;
    const char *p = line + ll;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "build/blake2-kat/blake2s-kat.txt";
    int ok = 1;

    if (!check_appendix_b()) ok = 0;

    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("SKIP blake2s-kat.txt not found at %s\n", path);
        printf("     fetch it with: bash tools/blake2_fetch.sh\n");
        printf("     (RFC 7693 Appendix B above still ran and is the result "
               "of this run.)\n");
        return ok ? 0 : 1;
    }

    char line[8192];
    long vectors = 0, passed = 0;
    while (getline_trim(line, sizeof(line), f)) {
        const char *v;
        if (!(v = field_value(line, "in:"))) continue;
        char in_hex[4096];
        strncpy(in_hex, v, sizeof(in_hex) - 1); in_hex[sizeof(in_hex)-1] = 0;

        if (!getline_trim(line, sizeof(line), f) || !(v = field_value(line, "key:"))) {
            fprintf(stderr, "FAIL malformed KAT file: expected key: after in:\n");
            ok = 0; break;
        }
        char key_hex[128];
        strncpy(key_hex, v, sizeof(key_hex) - 1); key_hex[sizeof(key_hex)-1] = 0;

        if (!getline_trim(line, sizeof(line), f) || !(v = field_value(line, "hash:"))) {
            fprintf(stderr, "FAIL malformed KAT file: expected hash: after key:\n");
            ok = 0; break;
        }
        char hash_hex[128];
        strncpy(hash_hex, v, sizeof(hash_hex) - 1); hash_hex[sizeof(hash_hex)-1] = 0;

        uint8_t in[2048], key[32], want[32], got[32];
        long inlen = hexdecode(in_hex, in, sizeof(in));
        long keylen = hexdecode(key_hex, key, sizeof(key));
        long wantlen = hexdecode(hash_hex, want, sizeof(want));
        if (inlen < 0 || keylen < 0 || wantlen != 32) {
            fprintf(stderr, "FAIL vector %ld: malformed hex\n", vectors);
            ok = 0; vectors++; continue;
        }

        vectors++;
        blake2s(in, (size_t)inlen, key, (size_t)keylen, got, 32);
        if (memcmp(got, want, 32) != 0) {
            fprintf(stderr, "FAIL vector %ld (inlen=%ld): mismatch\n", vectors - 1, inlen);
            fprintf(stderr, "  want: "); for (int i=0;i<32;i++) fprintf(stderr, "%02x", want[i]); fprintf(stderr, "\n");
            fprintf(stderr, "  got:  "); for (int i=0;i<32;i++) fprintf(stderr, "%02x", got[i]); fprintf(stderr, "\n");
            ok = 0;
        } else {
            passed++;
        }
    }
    fclose(f);

    printf("blake2s-kat: %ld/%ld vectors passed\n", passed, vectors);
    if (vectors != 256) {
        fprintf(stderr, "FAIL expected 256 vectors (the reference KAT's own "
                         "count), got %ld -- the corpus or the parser changed\n", vectors);
        ok = 0;
    }

    return ok ? 0 : 1;
}
