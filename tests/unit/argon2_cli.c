/* Command-line driver for run-argon2-openssl.sh: exposes argon2() so a shell
 * script can diff it against `openssl kdf ARGON2{D,I,ID}` byte for byte
 * across randomised parameters -- see that script for why the RFC vector
 * alone (fixed at p=4) is not enough.
 *
 * Usage: argon2_cli <d|i|id> <t_cost> <m_cost_kib> <lanes> <taglen>
 *                    <pwhex> <salthex> <sechex> <adhex>
 * `sechex`/`adhex` may be the empty string. Prints the tag in hex, or
 * "ERR <code>" and exits 2 if argon2() itself refused the call. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "argon2.h"

static int unhex(uint8_t *o, const char *h, int max)
{
    int n = (int)strlen(h) / 2;
    if (n > max) return -1;
    for (int i = 0; i < n; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); o[i] = (uint8_t)v; }
    return n;
}

int main(int argc, char **argv)
{
    if (argc != 10) { fprintf(stderr, "usage: argon2_cli <d|i|id> t m p taglen pw salt sec ad\n"); return 2; }

    enum argon2_type type;
    if (!strcmp(argv[1], "d")) type = ARGON2_D;
    else if (!strcmp(argv[1], "i")) type = ARGON2_I;
    else if (!strcmp(argv[1], "id")) type = ARGON2_ID;
    else { fprintf(stderr, "bad type\n"); return 2; }

    uint32_t t_cost = (uint32_t)strtoul(argv[2], 0, 10);
    uint32_t m_cost = (uint32_t)strtoul(argv[3], 0, 10);
    uint32_t lanes  = (uint32_t)strtoul(argv[4], 0, 10);
    uint32_t taglen = (uint32_t)strtoul(argv[5], 0, 10);

    static uint8_t pw[256], salt[256], sec[256], ad[256], tag[256];
    int pwlen  = unhex(pw, argv[6], sizeof pw);
    int saltlen = unhex(salt, argv[7], sizeof salt);
    int seclen  = unhex(sec, argv[8], sizeof sec);
    int adlen   = unhex(ad, argv[9], sizeof ad);
    if (pwlen < 0 || saltlen < 0 || seclen < 0 || adlen < 0 || taglen > sizeof tag) {
        fprintf(stderr, "bad hex/length\n"); return 2;
    }

    uint32_t blocks = argon2_memory_blocks(m_cost, lanes);
    struct argon2_block *mem = malloc((size_t)blocks * sizeof *mem);
    if (!mem) { fprintf(stderr, "OOM\n"); return 2; }

    int rc = argon2(type, pw, (uint32_t)pwlen, salt, (uint32_t)saltlen,
                     seclen ? sec : 0, (uint32_t)seclen,
                     adlen ? ad : 0, (uint32_t)adlen,
                     t_cost, m_cost, lanes, mem, blocks, tag, taglen);
    free(mem);
    if (rc != 0) { printf("ERR %d\n", rc); return 2; }

    for (uint32_t i = 0; i < taglen; i++) printf("%02x", tag[i]);
    printf("\n");
    return 0;
}
