/* Offline X.509 chain gate over REAL captured flights (tests/fixtures/tls/).
 *
 * Why a fixture gate when test-tls-chain-live dials the real hosts: the live
 * gate proves the fix against today's wire but cannot run on a networkless CI
 * host, and the servers rotate certificates -- a chain captured today expires.
 * This gate replays the exact flights that were captured at the moment the
 * bug was live, including the one that was REFUSED (misc.360buyimg.com's
 * duplicated intermediate), and links the same x509.c + roots.c the kernel
 * links. Nothing here talks to the network; nothing expires while the
 * fixture's own notAfter is in the future (the fixtures carry certs valid to
 * late 2026 -- regenerate them with openssl s_client -showcerts when they
 * age out, see tests/fixtures/tls/ (the .handshake.txt files record the exact
 * capture commands).
 *
 * Usage:
 *   tls_chain_test <chain.pem> <host> <accept|reject> [mutations...]
 *     --flip-sig <i>   flip one bit near the END of certificate i's DER (the
 *                      signatureValue region): a chain that is well-formed but
 *                      cryptographically false
 *     --dup <i>        append a copy of certificate i to the flight (what
 *                      misc/static.360buyimg.com actually send)
 *     --swap12         swap flight positions 1 and 2 (out-of-order chain)
 *     --wrong-host     verify against "not-the-sni.example" instead of <host>
 * Exit 0 = the chain verdict matched the expectation; 1 = it did not (and the
 * message says which side moved). A gate that cannot say which side moved is
 * a coin toss with extra steps.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "x509.h"

/* PEM -> DER. Self-contained: the kernel tree has no host-side base64, and
 * depending on `openssl base64` at test time makes the gate hostage to a
 * binary the gate otherwise does not need. */
static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int pem_load(const char *path, uint8_t der[8][8192], int derlen[8], int maxcerts)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL: cannot open %s\n", path); return -1; }
    char line[512];
    int ncert = 0, in = 0, acc = 0, nbits = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "-----BEGIN CERTIFICATE-----", 27) == 0) {
            if (ncert >= maxcerts) { printf("FAIL: more than %d certs\n", maxcerts); fclose(f); return -1; }
            in = 1; acc = 0; nbits = 0; derlen[ncert] = 0;
            continue;
        }
        if (strncmp(line, "-----END CERTIFICATE-----", 25) == 0) {
            if (nbits % 8) { /* PEM base64 pads to 3-byte groups; leftover bits are padding */ }
            in = 0; ncert++;
            continue;
        }
        if (!in) continue;
        for (const char *p = line; *p && *p != '\n' && *p != '\r'; p++) {
            if (*p == '=') break;
            int v = b64_val(*p);
            if (v < 0) continue;
            /* 24-bit window, not an unbounded int: the accumulator only ever
             * needs the low `nbits` (< 14) bits and a plain `acc << 6` on a
             * 2 KB certificate walks off the end of an int -- UBSan aborted
             * the very first fixture on exactly that (left shift of 203457419
             * by 6 places cannot be represented). */
            acc = ((acc << 6) | v) & 0xFFFFFF; nbits += 6;
            if (nbits >= 8) {
                nbits -= 8;
                if (derlen[ncert] >= 8192) { printf("FAIL: cert %d over 8192 B\n", ncert); fclose(f); return -1; }
                der[ncert][derlen[ncert]++] = (uint8_t)(acc >> nbits);
            }
        }
    }
    fclose(f);
    return ncert;
}

static const char *why(int rc)
{
    switch (rc) {
    case X509_OK:           return "trusted";
    case X509_E_PARSE:      return "malformed";
    case X509_E_SIG:        return "bad signature";
    case X509_E_UNTRUSTED:  return "no path to a trusted root";
    case X509_E_NAME:       return "host name mismatch";
    case X509_E_EXPIRED:    return "outside validity";
    default:                return "?";
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        printf("usage: %s <chain.pem> <host> <accept|reject> "
               "[--flip-sig i] [--dup i] [--swap12] [--wrong-host]\n", argv[0]);
        return 2;
    }
    static uint8_t der[8][8192];
    static int derlen[8];
    int n = pem_load(argv[1], der, derlen, 8);
    if (n < 1) return 2;

    const char *host = argv[2];
    int expect_ok = strcmp(argv[3], "accept") == 0;
    if (strcmp(argv[3], "accept") != 0 && strcmp(argv[3], "reject") != 0) {
        printf("FAIL: verdict must be accept|reject, not '%s'\n", argv[3]);
        return 2;
    }

    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--flip-sig") == 0 && i + 1 < argc) {
            int ci = atoi(argv[++i]);
            if (ci < 0 || ci >= n) { printf("FAIL: --flip-sig %d out of range\n", ci); return 2; }
            /* The signatureValue BIT STRING is the last field of the DER; a
             * bit flipped 5 bytes from the end lands inside it (after the tag,
             * length and unused-bits octet) rather than in the length framing,
             * so the cert still parses and the SIGNATURE is what becomes
             * false. That distinction is the point: this must be a
             * well-formed-but-false chain, not a parse error. */
            if (derlen[ci] < 16) { printf("FAIL: cert %d too short to flip\n", ci); return 2; }
            der[ci][derlen[ci] - 5] ^= 0x40;
        } else if (strcmp(argv[i], "--dup") == 0 && i + 1 < argc) {
            int ci = atoi(argv[++i]);
            if (n >= 8) { printf("FAIL: flight full, cannot duplicate\n"); return 2; }
            if (ci < 0 || ci >= n) { printf("FAIL: --dup %d out of range\n", ci); return 2; }
            memcpy(der[n], der[ci], (size_t)derlen[ci]);
            derlen[n] = derlen[ci];
            n++;
        } else if (strcmp(argv[i], "--swap12") == 0) {
            if (n < 3) { printf("FAIL: --swap12 needs 3 certs\n"); return 2; }
            /* positions 1 and 2 exchange DERs; leaf stays first */
            uint8_t tmp[8192]; int tl = derlen[1];
            memcpy(tmp, der[1], (size_t)tl);
            memcpy(der[1], der[2], (size_t)derlen[2]); derlen[1] = derlen[2];
            memcpy(der[2], tmp, (size_t)tl);            derlen[2] = tl;
        } else if (strcmp(argv[i], "--wrong-host") == 0) {
            host = "not-the-sni.example";
        } else {
            printf("FAIL: unknown argument %s\n", argv[i]);
            return 2;
        }
    }

    static struct cert chain[8];
    for (int i = 0; i < n; i++) {
        int pr = x509_parse(der[i], derlen[i], &chain[i]);
        if (pr != X509_OK) {
            printf("FAIL: certificate %d does not parse (rc=%d) -- the fixture "
                   "or a mutation broke the DER itself\n", i, pr);
            return 2;
        }
    }

    int pathlen = n;
    int rc = x509_verify_chain(chain, n, host, (int64_t)time(0), &pathlen);
    int ok = (rc == X509_OK);

    printf("chain of %d (path %d) for %s: %s (%d)%s\n", n, rc == X509_OK ? pathlen : -1,
           host, why(rc), rc, expect_ok ? "" : "");

    if (ok == expect_ok) {
        printf("ok   %s: %s as expected (%s)\n", argv[1], why(rc), argv[3]);
        return 0;
    }
    printf("FAIL %s: expected %s, got %s (%d)\n", argv[1], argv[3], why(rc), rc);
    return 1;
}
