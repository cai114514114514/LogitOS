/* Command-line driver for run-blake2s-openssl.sh: exposes blake2s() so a
 * shell script can diff it against `openssl dgst -blake2s256` (unkeyed,
 * fixed 32-byte output) and `openssl mac ... BLAKE2SMAC` (keyed, VARIABLE
 * output length) byte for byte.
 *
 * The KAT battery (tests/unit/blake2s_test.c) is the primary, official-vector
 * gate and covers every input length 0..255 at outlen==32. What it does NOT
 * cover -- because the reference blake2s-kat.txt does not vary it -- is
 * outlen != 32. That is exactly the dimension the OpenSSL differential adds:
 * BLAKE2SMAC's `size:` option asks openssl for the same short/long digest
 * this file's blake2s_init(ctx, outlen) computes a DIFFERENT parameter word
 * for (see blake2s.c's BLAKE2S_PARAM_WORD comment) -- so this differential
 * and the KAT battery each catch a defect the other structurally cannot.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "blake2s.h"

static void hexprint(const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) printf("%02x", b[i]); printf("\n"); }

static long unhex(uint8_t *o, const char *h, size_t max)
{
    size_t n = strlen(h) / 2;
    if (strlen(h) % 2 != 0 || n > max) return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(h + 2 * i, "%2x", &v) != 1) return -1;
        o[i] = (uint8_t)v;
    }
    return (long)n;
}

int main(int argc, char **argv)
{
    /* blake2s_cli <msghex> <keyhex|-> <outlen> */
    if (argc != 4) { fprintf(stderr, "usage: blake2s_cli <msghex> <keyhex|-> <outlen>\n"); return 2; }

    static uint8_t msg[8192], key[BLAKE2S_KEYBYTES], out[BLAKE2S_OUTBYTES];
    long msglen = unhex(msg, argv[1], sizeof(msg));
    if (msglen < 0) { fprintf(stderr, "bad msg hex\n"); return 2; }

    long keylen = 0;
    if (strcmp(argv[2], "-") != 0) {
        keylen = unhex(key, argv[2], sizeof(key));
        if (keylen < 0) { fprintf(stderr, "bad key hex\n"); return 2; }
    }

    long outlen = strtol(argv[3], NULL, 10);
    if (outlen < 1 || outlen > BLAKE2S_OUTBYTES) { fprintf(stderr, "bad outlen\n"); return 2; }

    blake2s(msg, (size_t)msglen, keylen > 0 ? key : NULL, (size_t)keylen, out, (size_t)outlen);
    hexprint(out, (size_t)outlen);
    return 0;
}
