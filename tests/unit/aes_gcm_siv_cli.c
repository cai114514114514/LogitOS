/* Command-line driver for run-aes-gcm-siv-openssl.sh: exposes THIS TREE's
 * aes_gcm_siv.c over hex stdin/argv so the differential script can drive it
 * exactly the way it drives the openssl side (tests/unit/aes_gcm_siv_openssl_cli.c).
 * Host-only glue -- not part of c/crypto, not freestanding-constrained.
 *
 *   seal <keyhex> <noncehex> <aadhex> <pthex>       -> prints  <ct||tag hex>
 *   open <keyhex> <noncehex> <aadhex> <cthex> <taghex> -> prints <pt hex>, or
 *                                                        "FAIL" + exit 1
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "aes_gcm_siv.h"

static int hex2bin(const char *hex, uint8_t *out, int maxout)
{
    int n = 0;
    size_t len = strlen(hex);
    if (len % 2) return -1;
    for (size_t i = 0; i < len; i += 2) {
        unsigned v;
        if (sscanf(hex + i, "%2x", &v) != 1) return -1;
        if (n >= maxout) return -1;
        out[n++] = (uint8_t)v;
    }
    return n;
}

static void bin2hex(const uint8_t *b, int n, char *out)
{
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[2*i] = H[b[i]>>4]; out[2*i+1] = H[b[i]&15]; }
    out[2*n] = 0;
}

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: seal|open ...\n"); return 2; }
    uint8_t key[32], nonce[12], aad[256], data[256], tag[16];
    int keylen = hex2bin(argv[2], key, sizeof key);
    int noncelen = hex2bin(argv[3], nonce, sizeof nonce);
    if (noncelen != 12 || (keylen != 16 && keylen != 32)) {
        fprintf(stderr, "bad key/nonce length\n"); return 2;
    }

    if (strcmp(argv[1], "seal") == 0) {
        if (argc != 6) { fprintf(stderr, "seal needs aadhex pthex\n"); return 2; }
        int aadlen = hex2bin(argv[4], aad, sizeof aad);
        int ptlen = hex2bin(argv[5], data, sizeof data);
        uint8_t ct[256];
        if (keylen == 16) aes128_gcm_siv_seal(key, nonce, aad, aadlen, data, ptlen, ct, tag);
        else               aes256_gcm_siv_seal(key, nonce, aad, aadlen, data, ptlen, ct, tag);
        char hex[520];
        bin2hex(ct, ptlen, hex);
        bin2hex(tag, 16, hex + ptlen*2);
        printf("%s\n", hex);
        return 0;
    }
    if (strcmp(argv[1], "open") == 0) {
        if (argc != 7) { fprintf(stderr, "open needs aadhex cthex taghex\n"); return 2; }
        int aadlen = hex2bin(argv[4], aad, sizeof aad);
        int ctlen = hex2bin(argv[5], data, sizeof data);
        int taglen = hex2bin(argv[6], tag, sizeof tag);
        if (taglen != 16) { fprintf(stderr, "bad tag length\n"); return 2; }
        uint8_t pt[256];
        int rc = (keylen == 16)
            ? aes128_gcm_siv_open(key, nonce, aad, aadlen, data, ctlen, tag, pt)
            : aes256_gcm_siv_open(key, nonce, aad, aadlen, data, ctlen, tag, pt);
        if (rc != 0) { printf("FAIL\n"); return 1; }
        char hex[520];
        bin2hex(pt, ctlen, hex);
        printf("%s\n", hex);
        return 0;
    }
    fprintf(stderr, "unknown mode %s\n", argv[1]);
    return 2;
}
