/* lpk -- make and inspect signed packages, on the host.
 *
 * Deliberately a HOST BUILD OF THE SAME C, not a second implementation in
 * Python. A signer written in Python would be a second opinion about the
 * format, and the first thing that goes wrong with two implementations of a
 * signature format is that they disagree about one padding byte and nobody
 * notices until a real package is refused. This links c/crypto/trust/pkgsig.c
 * and c/crypto/pubkey/ed25519.c -- the exact objects the kernel verifies with.
 *
 *   lpk sign   <seed-hex> <name> <in> <out>
 *   lpk verify <file> [--any]      --any: accept any valid self-signature,
 *                                  which is how you tell "not intact" apart
 *                                  from "not trusted"
 *   lpk keygen                     prints a fresh seed/public pair
 *   lpk roots                      lists the compiled-in trusted signers
 *
 * The seed is passed on the command line because this is a build tool driven
 * by a Makefile, and the only seed it is ever given is the published
 * development one (tools/pkgroots/DEV-SIGNING-KEY.txt). A real signing key does
 * not belong in a process argument list; when one exists, it gets a file
 * argument and a mode check, not this. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "pkgsig.h"
#include "crypto.h"

static int unhex32(uint8_t out[32], const char *h)
{
    if (strlen(h) != 64) return -1;
    for (int i = 0; i < 32; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = h[2*i+k];
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

static uint8_t *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    if (n && fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    *len = n;
    return b;
}

/* The host has no kernel DRBG; keygen is a convenience, and it says so. */
static void hostrand(uint8_t *b, int n)
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { size_t got = fread(b, 1, (size_t)n, f); fclose(f); if (got == (size_t)n) return; }
    fprintf(stderr, "lpk: /dev/urandom unavailable -- refusing to invent a key\n");
    exit(2);
}

static const char *errname(int e)
{
    switch (e) {
    case LPK_OK:          return "ok";
    case LPK_E_SHORT:     return "truncated";
    case LPK_E_MAGIC:     return "not a package (bad magic)";
    case LPK_E_VERSION:   return "unknown format version";
    case LPK_E_FIELD:     return "malformed header field";
    case LPK_E_DIGEST:    return "payload does not match its digest";
    case LPK_E_SIG:       return "SIGNATURE INVALID";
    case LPK_E_UNTRUSTED: return "signature valid, signer NOT TRUSTED";
    default:              return "?";
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: lpk sign <seed-hex> <name> <in> <out>\n"
                        "       lpk verify <file> [--any]\n"
                        "       lpk keygen\n"
                        "       lpk roots\n");
        return 1;
    }

    if (!strcmp(argv[1], "keygen")) {
        uint8_t pub[32], seed[32];
        if (ed25519_keypair(pub, seed, hostrand) != 0) { fprintf(stderr, "lpk: keygen failed\n"); return 2; }
        printf("seed   "); puthex(seed, 32); printf("\n");
        printf("public "); puthex(pub, 32); printf("\n");
        return 0;
    }

    if (!strcmp(argv[1], "roots")) {
        printf("%d trusted package signer(s), compiled in:\n", pkg_root_count());
        for (int i = 0; i < pkg_root_count(); i++) {
            printf("  %-24s ", pkg_root_name(i));
            puthex(pkg_root_key(i), 32);
            printf("\n");
        }
        return 0;
    }

    if (!strcmp(argv[1], "sign") && argc == 6) {
        uint8_t seed[32];
        if (unhex32(seed, argv[2]) != 0) { fprintf(stderr, "lpk: seed must be 64 hex chars\n"); return 1; }
        long n = 0;
        uint8_t *payload = slurp(argv[4], &n);
        if (!payload) { fprintf(stderr, "lpk: cannot read %s\n", argv[4]); return 1; }
        uint8_t hdr[LPK_HDR_LEN];
        if (lpk_sign(hdr, argv[3], payload, (uint64_t)n, seed) != 0) {
            fprintf(stderr, "lpk: name too long (max %d)\n", LPK_NAME_MAX); return 1;
        }
        FILE *o = fopen(argv[5], "wb");
        if (!o) { fprintf(stderr, "lpk: cannot write %s\n", argv[5]); return 1; }
        fwrite(hdr, 1, sizeof hdr, o);
        if (n) fwrite(payload, 1, (size_t)n, o);
        fclose(o);
        free(payload);
        printf("lpk: %s  name='%s'  payload=%ld  signer=", argv[5], argv[3], n);
        puthex(hdr + 128, 32);
        printf("\n");
        return 0;
    }

    if (!strcmp(argv[1], "verify") && argc >= 3) {
        int any = (argc > 3 && !strcmp(argv[3], "--any"));
        long n = 0;
        uint8_t *buf = slurp(argv[2], &n);
        if (!buf) { fprintf(stderr, "lpk: cannot read %s\n", argv[2]); return 1; }
        struct lpk out;
        int r = lpk_verify(buf, (uint64_t)n, &out, any);
        if (r != LPK_OK) { printf("%s: REFUSED (%d) %s\n", argv[2], r, errname(r)); return 3; }
        printf("%s: ok  name='%s'  payload=%llu  signer=", argv[2], out.name,
               (unsigned long long)out.payload_len);
        puthex(out.signer, 32);
        if (out.root_index >= 0) printf("  (%s)\n", pkg_root_name(out.root_index));
        else printf("  (UNTRUSTED KEY -- accepted only because of --any)\n");
        free(buf);
        return 0;
    }

    fprintf(stderr, "lpk: bad arguments\n");
    return 1;
}
