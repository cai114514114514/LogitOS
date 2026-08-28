/* Command-line driver for run-mldsa-openssl.sh: exposes the DERANDOMISED
 * ML-DSA entry points so a shell script can diff them against openssl byte
 * for byte. Same reasoning as mlkem_cli.c: a keygen/sign that draws its own
 * randomness can only ever be compared with itself, and openssl accepts the
 * same seed (genpkey -pkeyopt hexseed:) and the same deterministic-signing
 * mode (pkeyutl -pkeyopt deterministic:1) that this file's derandomised API
 * takes as an argument, which is what makes byte-for-byte comparison
 * possible at all.
 *
 * Buffers are static, sized for the largest parameter set (ML-DSA-87), for
 * the same reason mlkem_cli.c's are: this runs one operation and exits. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "mldsa.h"

static void hex(const uint8_t *b, int n) { for (int i = 0; i < n; i++) printf("%02x", b[i]); printf("\n"); }
static int unhex(uint8_t *o, const char *h, int max)
{
    int n = (int)strlen(h) / 2;
    if (n > max) return -1;
    for (int i = 0; i < n; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); o[i] = (uint8_t)v; }
    return n;
}

static const mldsa_params *pick(const char *s)
{
    if (!strcmp(s, "44")) return &mldsa44_params;
    if (!strcmp(s, "65")) return &mldsa65_params;
    if (!strcmp(s, "87")) return &mldsa87_params;
    return 0;
}

int main(int argc, char **argv)
{
    static uint8_t pk[MLDSA87_PK], sk[MLDSA87_SK], sig[MLDSA87_SIG];
    static uint8_t xi[32], rnd[32], msg[8192], ctx[256];
    if (argc < 3) return 2;

    const mldsa_params *p = pick(argv[2]);
    if (!p) { fprintf(stderr, "bad parameter set '%s' (want 44, 65 or 87)\n", argv[2]); return 2; }

    if (!strcmp(argv[1], "keygen")) {                     /* keygen <set> <xihex> [pk|sk|both] */
        if (unhex(xi, argv[3], 32) != 32) { fprintf(stderr, "bad xi len\n"); return 2; }
        mldsa_keygen_internal(p, xi, pk, sk);
        const char *w = argc > 4 ? argv[4] : "both";
        if (!strcmp(w, "pk")) hex(pk, p->pk_bytes);
        else if (!strcmp(w, "sk")) hex(sk, p->sk_bytes);
        else { hex(pk, p->pk_bytes); hex(sk, p->sk_bytes); }
        return 0;
    }
    if (!strcmp(argv[1], "sign")) {                       /* sign <set> <skhex> <msghex> <ctxhex> */
        if (unhex(sk, argv[3], p->sk_bytes) != p->sk_bytes) { fprintf(stderr, "bad sk len\n"); return 2; }
        int mlen = unhex(msg, argv[4], (int)sizeof msg);
        if (mlen < 0) { fprintf(stderr, "message too long\n"); return 2; }
        int ctxlen = unhex(ctx, argv[5], (int)sizeof ctx);
        if (ctxlen < 0) { fprintf(stderr, "context too long\n"); return 2; }
        for (int i = 0; i < 32; i++) rnd[i] = 0;          /* deterministic: rnd = 0^32 */
        if (mldsa_sign(p, sk, msg, (size_t)mlen, ctx, (size_t)ctxlen, rnd, sig) != 0) {
            printf("SIGN_FAILED\n"); return 1;
        }
        hex(sig, p->sig_bytes);
        return 0;
    }
    if (!strcmp(argv[1], "verify")) {                     /* verify <set> <pkhex> <msghex> <ctxhex> <sighex> */
        if (unhex(pk, argv[3], p->pk_bytes) != p->pk_bytes) { fprintf(stderr, "bad pk len\n"); return 2; }
        int mlen = unhex(msg, argv[4], (int)sizeof msg);
        if (mlen < 0) { fprintf(stderr, "message too long\n"); return 2; }
        int ctxlen = unhex(ctx, argv[5], (int)sizeof ctx);
        if (ctxlen < 0) { fprintf(stderr, "context too long\n"); return 2; }
        if (unhex(sig, argv[6], p->sig_bytes) != p->sig_bytes) { fprintf(stderr, "bad sig len\n"); return 2; }
        printf("%s\n", mldsa_verify(p, pk, msg, (size_t)mlen, ctx, (size_t)ctxlen, sig) ? "VALID" : "INVALID");
        return 0;
    }
    return 2;
}
