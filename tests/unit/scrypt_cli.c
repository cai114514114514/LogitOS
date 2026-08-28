/* CLI wrapper over scrypt() for run-scrypt-openssl.sh's differential.
 * argv: pw salt N r p dklen -- prints dklen bytes of hex derived key. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "scrypt.h"

int main(int argc, char **argv)
{
    if (argc != 7) { fprintf(stderr, "usage: pw salt N r p dklen\n"); return 2; }
    const char *pw = argv[1], *salt = argv[2];
    uint64_t N = strtoull(argv[3], 0, 10);
    uint32_t r = (uint32_t)strtoul(argv[4], 0, 10);
    uint32_t p = (uint32_t)strtoul(argv[5], 0, 10);
    uint64_t dklen = strtoull(argv[6], 0, 10);

    uint64_t need = scrypt_scratch_len(N, r, p);
    uint8_t *scratch = malloc((size_t)need);
    uint8_t *dk = malloc((size_t)dklen);
    if (!scratch || !dk) { fprintf(stderr, "malloc failed (%llu)\n", (unsigned long long)need); return 2; }

    int rc = scrypt((const uint8_t *)pw, (int)strlen(pw), (const uint8_t *)salt, (int)strlen(salt),
                     N, r, p, dk, dklen, scratch, need);
    if (rc != 0) { fprintf(stderr, "scrypt() returned %d\n", rc); return 1; }

    for (uint64_t i = 0; i < dklen; i++) printf("%02x", dk[i]);
    printf("\n");
    return 0;
}
