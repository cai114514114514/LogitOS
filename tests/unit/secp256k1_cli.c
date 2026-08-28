/* Command-line driver for run-secp256k1-openssl.sh: exposes secp256k1_keygen/
 * secp256k1_sign/secp256k1_verify_der to a shell script so it can diff them
 * against openssl byte for byte, the same shape mlkem_cli.c uses for ML-KEM
 * and run-ecdsa-sign.sh uses for the NIST curves.
 *
 * Every operation here is DERANDOMISED (k and priv come in as hex, never
 * drawn internally) for the same reason mlkem_cli.c gives for ML-KEM: a
 * keygen or sign that draws its own randomness can only ever be compared
 * with itself.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "secp256k1.h"
#include "crypto.h"

static void hex(const uint8_t *b, int n) { for (int i = 0; i < n; i++) printf("%02x", b[i]); printf("\n"); }
static int unhex(uint8_t *o, const char *h, int max)
{
    int n = (int)strlen(h) / 2;
    if (n > max) return -1;
    for (int i = 0; i < n; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); o[i] = (uint8_t)v; }
    return n;
}

int main(int argc, char **argv)
{
    static uint8_t priv[32], pub[65], hash[64], k[32], sig[64];
    static uint8_t derbuf[4500];
    if (argc < 2) return 2;

    if (!strcmp(argv[1], "keygen")) {                    /* keygen <privhex> */
        if (unhex(priv, argv[2], 32) != 32) { fprintf(stderr, "bad priv len\n"); return 2; }
        if (secp256k1_keygen(priv, 0, pub) != 0) { printf("REJECTED\n"); return 0; }
        hex(pub, 65);
        return 0;
    }
    if (!strcmp(argv[1], "sign")) {           /* sign <privhex> <hashhex> <khex> */
        if (unhex(priv, argv[2], 32) != 32) { fprintf(stderr, "bad priv len\n"); return 2; }
        int hlen = unhex(hash, argv[3], (int)sizeof hash);
        if (hlen <= 0) { fprintf(stderr, "bad hash len\n"); return 2; }
        if (unhex(k, argv[4], 32) != 32) { fprintf(stderr, "bad k len\n"); return 2; }
        if (secp256k1_sign(priv, hash, hlen, k, 0, sig) != 0) { printf("REJECTED\n"); return 0; }
        hex(sig, 64);
        return 0;
    }
    if (!strcmp(argv[1], "verify")) {   /* verify <pubhex> <derhex> <hashhex> */
        if (unhex(pub, argv[2], 65) != 65) { fprintf(stderr, "bad pub len\n"); return 2; }
        int dlen = unhex(derbuf, argv[3], (int)sizeof derbuf);
        if (dlen < 0) { fprintf(stderr, "bad der len\n"); return 2; }
        int hlen = unhex(hash, argv[4], (int)sizeof hash);
        if (hlen <= 0) { fprintf(stderr, "bad hash len\n"); return 2; }
        printf("%s\n", secp256k1_verify_der(pub, derbuf, dlen, hash, hlen) ? "valid" : "invalid");
        return 0;
    }
    fprintf(stderr, "usage: secp256k1_cli keygen|sign|verify ...\n");
    return 2;
}
