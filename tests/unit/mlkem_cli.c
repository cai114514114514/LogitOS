/* Command-line driver for run-mlkem-openssl.sh: exposes the DERANDOMISED
 * ML-KEM-768 entry points so a shell script can diff them against openssl byte
 * for byte.
 *
 * Derandomised on purpose. A keygen that draws its own seed can only ever be
 * compared with itself -- there is no way to ask openssl for "the key you would
 * have made from this seed", so keygen would go untested against any reference
 * and the whole differential would collapse to a round-trip check. openssl
 * accepts the same (d || z) seed through `genpkey -pkeyopt hexseed:`, which is
 * what makes byte-for-byte comparison possible at all.
 *
 * Buffers are static because 2400 + 1184 bytes of decapsulation key and
 * ciphertext on the stack is pointless in a program that runs one operation and
 * exits. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "mlkem.h"

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
    static uint8_t ek[MLKEM768_EK], dk[MLKEM768_DK], ct[MLKEM768_CT], ss[MLKEM768_SS];
    static uint8_t d[32], z[32], m[32];
    if (argc < 2) return 2;

    if (!strcmp(argv[1], "keygen")) {            /* keygen <d> <z> [ek|dk|both] */
        unhex(d, argv[2], 32); unhex(z, argv[3], 32);
        mlkem768_keygen_derand(d, z, ek, dk);
        const char *w = argc > 4 ? argv[4] : "both";
        if (!strcmp(w, "ek")) hex(ek, MLKEM768_EK);
        else if (!strcmp(w, "dk")) hex(dk, MLKEM768_DK);
        else { hex(ek, MLKEM768_EK); hex(dk, MLKEM768_DK); }
        return 0;
    }
    if (!strcmp(argv[1], "encaps")) {            /* encaps <ekhex> <m> */
        if (unhex(ek, argv[2], MLKEM768_EK) != MLKEM768_EK) { fprintf(stderr, "bad ek len\n"); return 2; }
        unhex(m, argv[3], 32);
        if (mlkem768_encaps_derand(ek, m, ct, ss) != 0) { printf("REJECTED\n"); return 0; }
        hex(ct, MLKEM768_CT); hex(ss, MLKEM768_SS);
        return 0;
    }
    if (!strcmp(argv[1], "decaps")) {            /* decaps <dkhex> <cthex> */
        if (unhex(dk, argv[2], MLKEM768_DK) != MLKEM768_DK) { fprintf(stderr, "bad dk len\n"); return 2; }
        if (unhex(ct, argv[3], MLKEM768_CT) != MLKEM768_CT) { fprintf(stderr, "bad ct len\n"); return 2; }
        mlkem768_decaps(dk, ct, ss);
        hex(ss, MLKEM768_SS);
        return 0;
    }
    return 2;
}
