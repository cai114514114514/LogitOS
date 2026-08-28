/* aexsign -- sign a .aex's embedded ELF image, on the host.
 *
 * Deliberately a HOST BUILD OF THE SAME C the kernel verifies with (aexsig.c),
 * not a second implementation in Python -- see tools/lpk.c's header for why
 * that argument holds here unchanged: two implementations of a signature
 * format disagree about one byte eventually, and nobody notices until a real
 * binary is refused.
 *
 * This tool does ONE thing: read an ELF image on stdin, sign it under
 * AEX_SIG_DOMAIN with the given seed, and print the 96-byte AEX_T_SIG record
 * (pub || sig) as hex on stdout. tools/mkaex.py calls it with --sign-seed and
 * --aexsign-bin and embeds the record as a TLV, because it, not this tool,
 * knows where the TLV region and its page-alignment padding land -- see
 * mkaex.py's build() for why the record has to exist before that padding is
 * computed.
 *
 *   aexsign sign <seed-hex> < image.elf
 *       -> "<pubkey-hex-64> <signature-hex-128>\n"
 *
 * The seed is a command-line argument for the same reason tools/lpk.c's is:
 * this is a build tool driven by a Makefile and the only seed it is ever
 * given today is the published development one
 * (tools/pkgroots/DEV-SIGNING-KEY.txt). A real signing key does not belong in
 * a process argument list.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "aexsig.h"
#include "crypto.h"

static int unhex32(uint8_t out[32], const char *h)
{
    if (strlen(h) != 64) return -1;
    for (int i = 0; i < 32; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = h[2 * i + k];
            int d = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0) return -1;
            v = (v << 4) | d;
        }
        out[i] = (uint8_t)v;
    }
    return 0;
}

static void puthex(const uint8_t *b, int n)
{ for (int i = 0; i < n; i++) printf("%02x", b[i]); }

/* Read all of stdin. Images this tree builds top out around 64 MiB (see
 * test-bigexec), so a doubling growth strategy costs a handful of reallocs
 * and never a special case for "big". */
static uint8_t *slurp_stdin(long *len)
{
    size_t cap = 1 << 20, n = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (n == cap) {
            cap *= 2;
            uint8_t *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        size_t got = fread(buf + n, 1, cap - n, stdin);
        n += got;
        if (got == 0) break;
    }
    if (ferror(stdin)) { free(buf); return NULL; }
    *len = (long)n;
    return buf;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "sign")) {
        uint8_t seed[32];
        if (unhex32(seed, argv[2]) != 0) {
            fprintf(stderr, "aexsign: seed must be 64 hex chars\n");
            return 1;
        }
        long n = 0;
        uint8_t *elf = slurp_stdin(&n);
        if (!elf) {
            fprintf(stderr, "aexsign: could not read the image from stdin\n");
            return 1;
        }
        uint8_t digest[32];
        sha256(elf, (size_t)n, digest);
        free(elf);

        uint8_t rec[AEX_SIG_LEN];
        aex_sig_sign(rec, (uint64_t)n, digest, seed);
        crypto_wipe(seed, sizeof seed);

        puthex(rec, 32);
        printf(" ");
        puthex(rec + 32, 64);
        printf("\n");
        return 0;
    }

    fprintf(stderr, "usage: aexsign sign <seed-hex> < image.elf\n");
    return 1;
}
