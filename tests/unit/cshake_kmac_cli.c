/* Host-only CLI wrapping cshake.c/kmac.c for run-cshake-openssl.sh's
 * differential against OpenSSL -- same shape as mlkem_cli.c next to it.
 * Not part of any product build; string.h/stdlib.h are fine here.
 *
 * usage: cshake_kmac_cli <mode> <outlen_bytes> <hex_key> <hex_msg> <hex_custom>
 *   mode: cshake128 | cshake256 | kmac128 | kmac256 | kmacxof128 | kmacxof256
 *   hex_key is ignored (but must be present, may be "") for the two cshake
 *   modes. hex_custom "" means S/N of length 0. Prints the digest as lower
 *   case hex with a trailing newline, nothing else, on success.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cshake.h"
#include "kmac.h"

static size_t unhex(const char *h, uint8_t *out)
{
    size_t n = strlen(h) / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
    return n;
}

int main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr, "usage: %s <mode> <outlen> <hex_key> <hex_msg> <hex_custom>\n", argv[0]);
        return 2;
    }
    const char *mode = argv[1];
    size_t outlen = (size_t)strtoul(argv[2], NULL, 10);

    static uint8_t key[1 << 16], msg[1 << 20], cust[1 << 16], out[1 << 16];
    size_t keylen = unhex(argv[3], key);
    size_t msglen = unhex(argv[4], msg);
    size_t custlen = unhex(argv[5], cust);

    if (outlen > sizeof out) { fprintf(stderr, "outlen too large\n"); return 2; }

    if (!strcmp(mode, "cshake128"))
        cshake128(out, outlen, msg, msglen, NULL, 0, cust, custlen);
    else if (!strcmp(mode, "cshake256"))
        cshake256(out, outlen, msg, msglen, NULL, 0, cust, custlen);
    else if (!strcmp(mode, "kmac128"))
        kmac128(out, outlen, key, keylen, msg, msglen, cust, custlen);
    else if (!strcmp(mode, "kmac256"))
        kmac256(out, outlen, key, keylen, msg, msglen, cust, custlen);
    else if (!strcmp(mode, "kmacxof128"))
        kmacxof128(out, outlen, key, keylen, msg, msglen, cust, custlen);
    else if (!strcmp(mode, "kmacxof256"))
        kmacxof256(out, outlen, key, keylen, msg, msglen, cust, custlen);
    else { fprintf(stderr, "unknown mode %s\n", mode); return 2; }

    for (size_t i = 0; i < outlen; i++) printf("%02x", out[i]);
    printf("\n");
    return 0;
}
