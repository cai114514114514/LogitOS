/* Command-line driver for run-xchacha-openssl.sh -- exposes hchacha20(),
 * the shared chacha20() stream function (reused unchanged, not reimplemented
 * -- see chacha_core.h) and xchacha20_poly1305_seal() so a shell script can
 * diff them against openssl byte for byte. See run-xchacha-openssl.sh for
 * WHY this differential is shaped the way it is (openssl 3.6.3's `enc`
 * subcommand refuses every AEAD cipher outright, `chacha20-poly1305`
 * included -- confirmed empirically, not assumed -- so this drives the
 * plain, non-AEAD `chacha20` stream cipher instead). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "crypto.h"
#include "chacha_core.h"
#include "xchacha20poly1305.h"

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
    static uint8_t key[32], nonce16[16], nonce12[12], nonce24[24];
    static uint8_t aad[2048], pt[2048], out[2048 + 16];
    if (argc < 2) return 2;

    if (!strcmp(argv[1], "hchacha")) {            /* hchacha <key32hex> <nonce16hex> */
        if (unhex(key, argv[2], 32) != 32 || unhex(nonce16, argv[3], 16) != 16) return 2;
        uint8_t subkey[32];
        hchacha20(key, nonce16, subkey);
        hex(subkey, 32);
        return 0;
    }
    if (!strcmp(argv[1], "stream")) {             /* stream <key32hex> <nonce12hex> <counter> <len> */
        if (unhex(key, argv[2], 32) != 32 || unhex(nonce12, argv[3], 12) != 12) return 2;
        uint32_t counter = (uint32_t)strtoul(argv[4], 0, 10);
        int len = atoi(argv[5]);
        if (len < 0 || len > (int)sizeof(pt)) return 2;
        memset(pt, 0, (size_t)len);
        chacha20(key, counter, nonce12, pt, len, out);   /* shared core, reused as-is */
        hex(out, len);
        return 0;
    }
    if (!strcmp(argv[1], "seal")) {               /* seal <key32hex> <nonce24hex> <aadhex> <pthex> */
        if (unhex(key, argv[2], 32) != 32 || unhex(nonce24, argv[3], 24) != 24) return 2;
        int al = unhex(aad, argv[4], sizeof aad);
        int pl = unhex(pt, argv[5], sizeof pt);
        if (al < 0 || pl < 0) return 2;
        uint8_t tag[16];
        xchacha20_poly1305_seal(key, nonce24, aad, al, pt, pl, out, tag);
        hex(out, pl);
        hex(tag, 16);
        return 0;
    }
    fprintf(stderr, "unknown command: %s\n", argv[1]);
    return 2;
}
