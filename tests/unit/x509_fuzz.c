/* ASan/UBSan fuzz for the hand-written X.509 DER parser (runs on every HTTPS
 * cert -- attacker-controlled). Parses a real cert, then every truncation, then
 * heavy single/multi-byte mutations, then truncated+mutated. ASan catches any
 * out-of-bounds read in the DER length/structure handling. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "x509.h"

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb"); if (!f) { printf("no cert\n"); return 1; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *der = malloc(len); fread(der, 1, len, f); fclose(f);

    struct cert c;
    printf("full parse rc=%d (len=%ld)\n", x509_parse(der, (int)len, &c), len);

    for (int n = 0; n <= (int)len; n++) { struct cert t; x509_parse(der, n, &t); }   /* every truncation */

    srand(1234);
    for (int it = 0; it < 150000; it++) {                /* mutate full-length */
        uint8_t *m = malloc(len); memcpy(m, der, len);
        int k = 1 + rand() % 6;
        for (int j = 0; j < k; j++) m[rand() % len] ^= (uint8_t)(1 + rand() % 255);
        struct cert t; x509_parse(m, (int)len, &t);
        free(m);
    }
    for (int it = 0; it < 150000; it++) {                /* truncated + mutated */
        int n = 1 + rand() % (int)len;
        uint8_t *m = malloc(n); memcpy(m, der, n);
        int k = 1 + rand() % 4;
        for (int j = 0; j < k; j++) m[rand() % n] ^= (uint8_t)(1 + rand() % 255);
        struct cert t; x509_parse(m, n, &t);
        free(m);
    }
    free(der);
    printf("X509 FUZZ DONE\n");
    return 0;
}
