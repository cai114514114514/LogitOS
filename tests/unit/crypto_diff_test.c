/* crypto_diff_test.c - differential asserter for Logit's hand-rolled crypto.
 * Streams the random vector file produced by tests/unit/crypto_diff_gen.py
 * (argv[1]), runs every case through the C implementation and compares
 * byte-for-byte. Mismatches print the line number and op and the run
 * continues; the exit status is nonzero if any case failed.
 *
 * Vector line format (all binary fields lowercase hex, "-" = empty):
 *   emul curveid useorder a b out          a,b,out: nbytes big-endian
 *   rexp base e n out                      big-endian, out = n-length
 *   x255 scalar point out                  32 bytes little-endian each
 *   gcm  key nonce aad pt ct tag           AES-128-GCM seal/open (96-bit IV)
 *   gcmx keylen key iv aad pt ct tag       AES-GCM arbitrary IV, 128/192/256
 *   aead key nonce aad pt ct tag           ChaCha20-Poly1305 seal/open
 *   ctr  keylen key iv in out              AES-CTR, 128/192/256
 *   cbc  keylen key iv pt ct               AES-CBC+PKCS#7 encrypt/decrypt
 *   hash algo msg out                      algo = sha224|sha256|sha384|sha512|
 *                                              sha224|sha512_224|sha512_256
 *   hmac hlen key msg out                  hlen 28/32/48/64
 *   hkdf hlen salt ikm info outlen prk okm
 *   pdf2 hlen pw salt iters dklen dk       PBKDF2, hlen 28/32/48/64
 *   exlb hlen secret labelhex ctx outlen out   labelhex = hex(label bytes)
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "crypto.h"

/* test hooks living in the crypto compilation units */
int ecdsa_modmul_test(int curveid, int useorder, const uint8_t *a,
                      const uint8_t *b, uint8_t *out);
int rsa_modexp_be(const uint8_t *base, int bl, const uint8_t *e, int el,
                  const uint8_t *n, int nl, uint8_t *out);

#define MAXFIELD 16384         /* longest hex field: 4096-byte pt + a 16-byte
                                * CBC pad block (gcm's 4096-byte ct only just
                                * fit the old 8192 limit; PKCS#7 broke it) */
#define MAXB     (MAXFIELD / 2)

static char line[MAXFIELD * 3 + 256];
static uint8_t bufs[8][MAXB + 1];

static int hv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* hex -> bytes; "-" decodes to length 0. Returns -1 on malformed input. */
static int unhex(const char *h, uint8_t *o)
{
    if (!strcmp(h, "-")) return 0;
    size_t n = strlen(h);
    if (n & 1 || n > MAXFIELD) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int a = hv(h[2 * i]), b = hv(h[2 * i + 1]);
        if (a < 0 || b < 0) return -1;
        o[i] = (uint8_t)(a << 4 | b);
    }
    return (int)(n / 2);
}

static int eq(const uint8_t *a, const uint8_t *b, int n) { return !memcmp(a, b, n); }

/* How many AES backends this machine can run: 2 when the CPU has AES-NI
 * (accelerated + portable), 1 otherwise. Set in main(). */
static int g_backends = 1;

#define NOPS 13
static const char *opnames[NOPS] =
    { "emul", "rexp", "x255", "gcm", "aead", "hash", "hmac", "hkdf", "exlb",
      "pdf2", "gcmx", "ctr", "cbc" };
static long opass[NOPS], ofail[NOPS];
static long printed;

static void report(long lineno, const char *op, const char *what)
{
    if (printed < 25) {
        printf("FAIL line %ld op %s: %s\n", lineno, op, what);
        printed++;
        if (printed == 25) printf("... further failures suppressed ...\n");
    }
}

static void fail_op(int idx, long lineno, const char *what)
{
    ofail[idx]++;
    report(lineno, opnames[idx], what);
}

static int op_index(const char *op)
{
    for (int i = 0; i < NOPS; i++) if (!strcmp(op, opnames[i])) return i;
    return -1;
}

/* aead seal/open shared by gcm and aead ops. which: 0=gcm 1=chacha */
static void run_aead(int idx, long lineno, char **t, int which)
{
    uint8_t *key = bufs[0], *nonce = bufs[1], *aad = bufs[2], *pt = bufs[3];
    uint8_t *ct = bufs[4], *tag = bufs[5], *got = bufs[6], *out = bufs[7];
    uint8_t got_tag[16];
    int kl = unhex(t[1], key), nl = unhex(t[2], nonce), al = unhex(t[3], aad);
    int pl = unhex(t[4], pt), cl = unhex(t[5], ct), tl = unhex(t[6], tag);
    if (kl < 0 || nl < 0 || al < 0 || pl < 0 || cl < 0 || tl < 0 ||
        kl != (which ? 32 : 16) || nl != 12 || cl != pl || tl != 16) {
        fail_op(idx, lineno, "bad field lengths"); return;
    }
    const uint8_t *aadp = al ? aad : NULL, *ptp = pl ? pt : NULL;
    const uint8_t *ctp = cl ? ct : NULL;
    uint8_t badtag[16];
    if (which) {
        chacha20_poly1305_seal(key, nonce, aadp, al, ptp, pl, got, got_tag);
        if (!eq(got, ct, cl) || !eq(got_tag, tag, 16)) {
            fail_op(idx, lineno, "seal ct/tag mismatch"); return;
        }
        if (chacha20_poly1305_open(key, nonce, aadp, al, ctp, cl, tag, out) != 0 ||
            !eq(out, pt, pl)) {
            fail_op(idx, lineno, "open roundtrip failed"); return;
        }
        memcpy(badtag, tag, 16); badtag[0] ^= 1;
        if (chacha20_poly1305_open(key, nonce, aadp, al, ctp, cl, badtag, out) != -1) {
            fail_op(idx, lineno, "flipped tag not rejected"); return;
        }
    } else {
        /* Every AES-GCM vector is replayed through EVERY available backend
         * (the accelerated one and the portable reference), and each must
         * match the Python reference independently -- which crosses the two
         * implementations against each other as well as against the oracle.
         * `g_backends` is 1 when the host has no AES-NI, so the same binary is
         * meaningful on either kind of machine.
         *
         * Doing it here rather than in a separate differential means the
         * accelerated path sees all 128k randomized cases, including the
         * awkward lengths (empty AAD, empty plaintext, non-multiples of 16)
         * that a hand-written cross-check would not think to generate. */
        uint8_t first_ct[MAXB], first_tag[16];
        int first = 1;
        for (int b = 0; b < g_backends; b++) {
            crypto_simd_force_baseline(b);          /* 0 = selected, 1 = portable */
            aes128_gcm_seal(key, nonce, aadp, al, ptp, pl, got, got_tag);
            if (!eq(got, ct, cl) || !eq(got_tag, tag, 16)) {
                crypto_simd_force_baseline(0);
                fail_op(idx, lineno, "seal ct/tag mismatch"); return;
            }
            if (first) {
                if (cl) memcpy(first_ct, got, (size_t)cl);
                memcpy(first_tag, got_tag, 16);
                first = 0;
            } else if ((cl && !eq(first_ct, got, cl)) || !eq(first_tag, got_tag, 16)) {
                crypto_simd_force_baseline(0);
                fail_op(idx, lineno, "backends disagree"); return;
            }
            if (aes128_gcm_open(key, nonce, aadp, al, ctp, cl, tag, out) != 0 ||
                !eq(out, pt, pl)) {
                crypto_simd_force_baseline(0);
                fail_op(idx, lineno, "open roundtrip failed"); return;
            }
            memcpy(badtag, tag, 16); badtag[0] ^= 1;
            if (aes128_gcm_open(key, nonce, aadp, al, ctp, cl, badtag, out) != -1) {
                crypto_simd_force_baseline(0);
                fail_op(idx, lineno, "flipped tag not rejected"); return;
            }
        }
        crypto_simd_force_baseline(0);
    }
    opass[idx]++;
}

/* gcmx keylen key iv aad pt ct tag -- AES-GCM with a non-96-bit IV at every
 * key size. Same both-backends replay as the 96-bit gcm op above: the GHASH
 * construction of J0 runs inside gf_mul, so the accelerated path must agree
 * with the portable one here too. */
static void run_gcmx(int idx, long lineno, char **t)
{
    int keylen = atoi(t[1]);
    uint8_t *key = bufs[0], *iv = bufs[1], *aad = bufs[2], *pt = bufs[3];
    uint8_t *ct = bufs[4], *tag = bufs[5], *got = bufs[6], *out = bufs[7];
    uint8_t got_tag[16];
    int kl = unhex(t[2], key), il = unhex(t[3], iv), al = unhex(t[4], aad);
    int pl = unhex(t[5], pt), cl = unhex(t[6], ct), tl = unhex(t[7], tag);
    if (kl != keylen || (keylen != 16 && keylen != 24 && keylen != 32) ||
        il < 1 || il > 1024 || al < 0 || pl < 0 || cl < 0 || tl != 16 || cl != pl) {
        fail_op(idx, lineno, "bad field lengths"); return;
    }
    const uint8_t *aadp = al ? aad : NULL, *ptp = pl ? pt : NULL;
    const uint8_t *ctp = cl ? ct : NULL;
    uint8_t badtag[16];
    for (int b = 0; b < g_backends; b++) {
        crypto_simd_force_baseline(b);
        if (keylen == 16)      aes128_gcm_seal_iv(key, iv, il, aadp, al, ptp, pl, got, got_tag);
        else if (keylen == 24) aes192_gcm_seal_iv(key, iv, il, aadp, al, ptp, pl, got, got_tag);
        else                   aes256_gcm_seal_iv(key, iv, il, aadp, al, ptp, pl, got, got_tag);
        if (!eq(got, ct, cl) || !eq(got_tag, tag, 16)) {
            crypto_simd_force_baseline(0);
            fail_op(idx, lineno, "seal ct/tag mismatch"); return;
        }
        int rc = keylen == 16
            ? aes128_gcm_open_iv(key, iv, il, aadp, al, ctp, cl, tag, out)
            : keylen == 24
            ? aes192_gcm_open_iv(key, iv, il, aadp, al, ctp, cl, tag, out)
            : aes256_gcm_open_iv(key, iv, il, aadp, al, ctp, cl, tag, out);
        if (rc != 0 || !eq(out, pt, pl)) {
            crypto_simd_force_baseline(0);
            fail_op(idx, lineno, "open roundtrip failed"); return;
        }
        memcpy(badtag, tag, 16); badtag[0] ^= 1;
        rc = keylen == 16
            ? aes128_gcm_open_iv(key, iv, il, aadp, al, ctp, cl, badtag, out)
            : keylen == 24
            ? aes192_gcm_open_iv(key, iv, il, aadp, al, ctp, cl, badtag, out)
            : aes256_gcm_open_iv(key, iv, il, aadp, al, ctp, cl, badtag, out);
        if (rc != -1) {
            crypto_simd_force_baseline(0);
            fail_op(idx, lineno, "flipped tag not rejected"); return;
        }
    }
    crypto_simd_force_baseline(0);
    opass[idx]++;
}

/* ctr keylen key iv in out -- CTR is symmetric, so one call cross-checks both
 * directions. Both backends: the only backend difference is the block
 * primitive, but that is exactly what the carry-across-the-whole-block
 * counter exercises hardest. */
static void run_ctr(int idx, long lineno, char **t)
{
    int keylen = atoi(t[1]);
    uint8_t *key = bufs[0], *iv = bufs[1], *in = bufs[2], *want = bufs[3], *got = bufs[4];
    int kl = unhex(t[2], key), il = unhex(t[3], iv);
    int nl = unhex(t[4], in), ol = unhex(t[5], want);
    if (kl != keylen || (keylen != 16 && keylen != 24 && keylen != 32) ||
        il != 16 || nl < 0 || ol != nl) {
        fail_op(idx, lineno, "bad field lengths"); return;
    }
    for (int b = 0; b < g_backends; b++) {
        crypto_simd_force_baseline(b);
        if (keylen == 16)      aes128_ctr(key, iv, nl ? in : NULL, nl, got);
        else if (keylen == 24) aes192_ctr(key, iv, nl ? in : NULL, nl, got);
        else                   aes256_ctr(key, iv, nl ? in : NULL, nl, got);
        if (!eq(got, want, nl)) {
            crypto_simd_force_baseline(0);
            fail_op(idx, lineno, "ctr mismatch"); return;
        }
        /* and back: CTR of CTR is the identity */
        if (keylen == 16)      aes128_ctr(key, iv, got, nl, got);
        else if (keylen == 24) aes192_ctr(key, iv, got, nl, got);
        else                   aes256_ctr(key, iv, got, nl, got);
        if (!eq(got, in, nl)) {
            crypto_simd_force_baseline(0);
            fail_op(idx, lineno, "ctr not an involution"); return;
        }
    }
    crypto_simd_force_baseline(0);
    opass[idx]++;
}

/* cbc keylen key iv pt ct -- encrypt must match the padded ct; decrypt must
 * return pt and its exact length. CBC decrypt is the only customer of the
 * backend's block DECRYPT primitive, so replaying through both backends is
 * what differentially tests ni_decrypt's equivalent-inverse schedule. */
static void run_cbc(int idx, long lineno, char **t)
{
    int keylen = atoi(t[1]);
    uint8_t *key = bufs[0], *iv = bufs[1], *pt = bufs[2], *ct = bufs[3], *out = bufs[4];
    int kl = unhex(t[2], key), il = unhex(t[3], iv);
    int pl = unhex(t[4], pt), cl = unhex(t[5], ct);
    if (kl != keylen || (keylen != 16 && keylen != 24 && keylen != 32) ||
        il != 16 || pl < 0 || cl != ((pl / 16 + 1) * 16)) {
        fail_op(idx, lineno, "bad field lengths"); return;
    }
    for (int b = 0; b < g_backends; b++) {
        crypto_simd_force_baseline(b);
        int n;
        if (keylen == 16)      n = aes128_cbc_encrypt(key, iv, pl ? pt : NULL, pl, out);
        else if (keylen == 24) n = aes192_cbc_encrypt(key, iv, pl ? pt : NULL, pl, out);
        else                   n = aes256_cbc_encrypt(key, iv, pl ? pt : NULL, pl, out);
        if (n != cl || !eq(out, ct, cl)) {
            crypto_simd_force_baseline(0);
            fail_op(idx, lineno, "cbc encrypt mismatch"); return;
        }
        if (keylen == 16)      n = aes128_cbc_decrypt(key, iv, ct, cl, out);
        else if (keylen == 24) n = aes192_cbc_decrypt(key, iv, ct, cl, out);
        else                   n = aes256_cbc_decrypt(key, iv, ct, cl, out);
        if (n != pl || !eq(out, pt, pl)) {
            crypto_simd_force_baseline(0);
            fail_op(idx, lineno, "cbc decrypt mismatch"); return;
        }
    }
    crypto_simd_force_baseline(0);
    opass[idx]++;
}

static void run_line(long lineno, char **t, int nt)
{
    int idx = op_index(t[0]);
    if (idx < 0) { printf("FAIL line %ld: unknown op %s\n", lineno, t[0]); ofail[0]++; return; }
    uint8_t *f0 = bufs[0], *f1 = bufs[1], *f2 = bufs[2], *f3 = bufs[3];
    uint8_t *f4 = bufs[4], *f5 = bufs[5], *f6 = bufs[6];

    switch (idx) {
    case 0: { /* emul curveid useorder a b out */
        if (nt != 6) { fail_op(idx, lineno, "arity"); return; }
        int cid = atoi(t[1]), uo = atoi(t[2]);
        int al = unhex(t[3], f0), bl = unhex(t[4], f1), ol = unhex(t[5], f2);
        int nb = cid ? 48 : 32;
        if (al != nb || bl != nb || ol != nb) { fail_op(idx, lineno, "length"); return; }
        if (ecdsa_modmul_test(cid, uo, f0, f1, f3) != 0 || !eq(f3, f2, nb))
            { fail_op(idx, lineno, "modmul mismatch"); return; }
        opass[idx]++;
        return; }
    case 1: { /* rexp base e n out */
        if (nt != 5) { fail_op(idx, lineno, "arity"); return; }
        int bl = unhex(t[1], f0), el = unhex(t[2], f1), nl = unhex(t[3], f2), ol = unhex(t[4], f3);
        if (bl < 0 || el < 0 || nl < 0 || ol < 0 || ol != nl) { fail_op(idx, lineno, "length"); return; }
        if (rsa_modexp_be(f0, bl, f1, el, f2, nl, f4) != 0 || !eq(f4, f3, nl))
            { fail_op(idx, lineno, "modexp mismatch"); return; }
        opass[idx]++;
        return; }
    case 2: { /* x255 scalar point out */
        if (nt != 4) { fail_op(idx, lineno, "arity"); return; }
        if (unhex(t[1], f0) != 32 || unhex(t[2], f1) != 32 || unhex(t[3], f2) != 32)
            { fail_op(idx, lineno, "length"); return; }
        x25519(f3, f0, f1);
        if (!eq(f3, f2, 32)) { fail_op(idx, lineno, "x25519 mismatch"); return; }
        opass[idx]++;
        return; }
    case 3: /* gcm */
    case 4: /* aead */
        if (nt != 7) { fail_op(idx, lineno, "arity"); return; }
        run_aead(idx, lineno, t, idx == 4);
        return;
    case 5: { /* hash algo msg out */
        if (nt != 4) { fail_op(idx, lineno, "arity"); return; }
        int ml = unhex(t[2], f0), ol = unhex(t[3], f1);
        if (ml < 0 || ol < 0) { fail_op(idx, lineno, "length"); return; }
        const uint8_t *mp = ml ? f0 : NULL;
        if (!strcmp(t[1], "sha224") && ol == 28) { sha224(mp, ml, f2); }
        else if (!strcmp(t[1], "sha256") && ol == 32) { sha256(mp, ml, f2); }
        else if (!strcmp(t[1], "sha384") && ol == 48) { sha384(mp, ml, f2); }
        else if (!strcmp(t[1], "sha512") && ol == 64) { sha512(mp, ml, f2); }
        else if (!strcmp(t[1], "sha512_224") && ol == 28) { sha512_224(mp, ml, f2); }
        else if (!strcmp(t[1], "sha512_256") && ol == 32) { sha512_256(mp, ml, f2); }
        else { fail_op(idx, lineno, "algo"); return; }
        if (!eq(f2, f1, ol)) { fail_op(idx, lineno, "digest mismatch"); return; }
        opass[idx]++;
        return; }
    case 6: { /* hmac hlen key msg out */
        if (nt != 5) { fail_op(idx, lineno, "arity"); return; }
        int hlen = atoi(t[1]);
        int kl = unhex(t[2], f0), ml = unhex(t[3], f1), ol = unhex(t[4], f2);
        if (kl < 0 || ml < 0 || ol != hlen ||
            (hlen != 28 && hlen != 32 && hlen != 48 && hlen != 64))
            { fail_op(idx, lineno, "length"); return; }
        hmac(hlen, kl ? f0 : NULL, kl, ml ? f1 : NULL, ml, f3);
        if (!eq(f3, f2, ol)) { fail_op(idx, lineno, "hmac mismatch"); return; }
        opass[idx]++;
        return; }
    case 7: { /* hkdf hlen salt ikm info outlen prk okm */
        if (nt != 8) { fail_op(idx, lineno, "arity"); return; }
        int hlen = atoi(t[1]);
        int sl = unhex(t[2], f0), il = unhex(t[3], f1), fl = unhex(t[4], f2);
        int outlen = atoi(t[5]);
        int pl = unhex(t[6], f3), kl = unhex(t[7], f4);
        if (sl < 0 || il < 0 || fl < 0 || pl != hlen || kl != outlen ||
            (hlen != 28 && hlen != 32 && hlen != 48 && hlen != 64) || outlen < 1 || outlen > MAXB)
            { fail_op(idx, lineno, "length"); return; }
        hkdf_extract(hlen, sl ? f0 : NULL, sl, il ? f1 : NULL, il, f5);
        if (!eq(f5, f3, pl)) { fail_op(idx, lineno, "extract mismatch"); return; }
        hkdf_expand(hlen, f5, fl ? f2 : NULL, fl, f6, outlen);
        if (!eq(f6, f4, kl)) { fail_op(idx, lineno, "expand mismatch"); return; }
        opass[idx]++;
        return; }
    case 8: { /* exlb hlen secret labelhex ctx outlen out */
        if (nt != 7) { fail_op(idx, lineno, "arity"); return; }
        int hlen = atoi(t[1]);
        int sl = unhex(t[2], f0), ll = unhex(t[3], f1), cl = unhex(t[4], f2);
        int outlen = atoi(t[5]), ol = unhex(t[6], f3);
        if (sl != hlen || ll < 1 || ll > 64 || cl < 0 || ol != outlen ||
            (hlen != 32 && hlen != 48))
            { fail_op(idx, lineno, "length"); return; }
        f1[ll] = 0;
        if (hkdf_expand_label(hlen, f0, (const char *)f1, cl ? f2 : NULL, cl,
                              f4, outlen) != 0 || !eq(f4, f3, ol))
            { fail_op(idx, lineno, "expand_label mismatch"); return; }
        opass[idx]++;
        return; }
    case 9: { /* pdf2 hlen pw salt iters dklen dk */
        if (nt != 7) { fail_op(idx, lineno, "arity"); return; }
        int hlen = atoi(t[1]);
        int pl = unhex(t[2], f0), sl = unhex(t[3], f1);
        long iters = atol(t[4]);
        int dklen = atoi(t[5]), ol = unhex(t[6], f2);
        if (pl < 0 || sl < 0 || iters < 1 || dklen < 1 || dklen > MAXB ||
            ol != dklen || (hlen != 28 && hlen != 32 && hlen != 48 && hlen != 64))
            { fail_op(idx, lineno, "length"); return; }
        pbkdf2(hlen, pl ? f0 : NULL, pl, sl ? f1 : NULL, sl,
               (uint32_t)iters, f3, dklen);
        if (!eq(f3, f2, ol)) { fail_op(idx, lineno, "pbkdf2 mismatch"); return; }
        opass[idx]++;
        return; }
    case 10: { /* gcmx keylen key iv aad pt ct tag */
        if (nt != 8) { fail_op(idx, lineno, "arity"); return; }
        run_gcmx(idx, lineno, t);
        return; }
    case 11: { /* ctr keylen key iv in out */
        if (nt != 6) { fail_op(idx, lineno, "arity"); return; }
        run_ctr(idx, lineno, t);
        return; }
    case 12: { /* cbc keylen key iv pt ct */
        if (nt != 6) { fail_op(idx, lineno, "arity"); return; }
        run_cbc(idx, lineno, t);
        return; }
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: crypto_diff_test <vector-file>\n"); return 2; }

    crypto_simd_init();
    const char *sel = crypto_simd_backend_name();
    crypto_simd_force_baseline(1);
    const char *base = crypto_simd_backend_name();
    crypto_simd_force_baseline(0);
    g_backends = strcmp(sel, base) ? 2 : 1;
    printf("aes-gcm backends exercised: %d (selected=%s, baseline=%s)\n",
           g_backends, sel, base);

    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 2; }
    long lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *t[16];
        int nt = 0;
        for (char *tok = strtok(line, " \t\r\n"); tok && nt < 16;
             tok = strtok(NULL, " \t\r\n"))
            t[nt++] = tok;
        if (nt == 0) continue;
        run_line(lineno, t, nt);
    }
    fclose(f);
    long tp = 0, tf = 0;
    for (int i = 0; i < NOPS; i++) {
        printf("%-6s pass %8ld fail %8ld\n", opnames[i], opass[i], ofail[i]);
        tp += opass[i]; tf += ofail[i];
    }
    printf("total  pass %8ld fail %8ld\n", tp, tf);
    printf("%s\n", tf ? "DIFF TEST FAILED" : "DIFF TEST PASSED");
    return tf ? 1 : 0;
}
