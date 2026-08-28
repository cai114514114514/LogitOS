/* Minimal CLI for the OpenSSL differential (run-blake2b-openssl.sh).
 * usage: blake2b_cli <outlen> [hexkey]
 * Reads the message as raw bytes from stdin, prints the digest as lowercase
 * hex (no newline handling beyond a trailing \n) to stdout. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "blake2b.h"

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: blake2b_cli <outlen> [hexkey]\n"); return 2; }
    long outlen = strtol(argv[1], 0, 10);
    if (outlen < 1 || outlen > 64) { fprintf(stderr, "outlen out of range\n"); return 2; }

    uint8_t key[64];
    size_t keylen = 0;
    if (argc >= 3) {
        const char *h = argv[2];
        size_t hl = strlen(h);
        if (hl % 2 != 0 || hl / 2 > 64) { fprintf(stderr, "bad hexkey\n"); return 2; }
        keylen = hl / 2;
        for (size_t i = 0; i < keylen; i++) {
            int hi = hexval((unsigned char)h[2*i]), lo = hexval((unsigned char)h[2*i+1]);
            if (hi < 0 || lo < 0) { fprintf(stderr, "bad hexkey\n"); return 2; }
            key[i] = (uint8_t)((hi << 4) | lo);
        }
    }

    static uint8_t msg[1 << 20];
    size_t n = fread(msg, 1, sizeof(msg), stdin);

    uint8_t out[64];
    int rc = blake2b_keyed(msg, n, keylen ? key : 0, keylen, out, (size_t)outlen);
    if (rc != 0) { fprintf(stderr, "blake2b_keyed failed\n"); return 1; }

    for (long i = 0; i < outlen; i++) printf("%02x", out[i]);
    printf("\n");
    return 0;
}
