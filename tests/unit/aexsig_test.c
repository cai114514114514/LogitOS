/* AEX_T_SIG -- the OPTIONAL Ed25519 signature record, tested against the real
 * loader (c/kernel/exec/load/aex.c, unmodified) and the real crypto (aexsig.c,
 * pkgsig.c, ed25519.c, sha256.c, unmodified).
 *
 * Four things this exists to WATCH FAIL, per CLAUDE.md's rule 5 -- a control
 * that cannot be watched failing is worse than no control:
 *
 *   1. A validly-signed .aex from the development root loads with
 *      sig_status == AEX_SIG_OK and sig_root naming that root.
 *   2. A payload tampered AFTER signing (with its CRC-32 repaired to match --
 *      see tests/unit/aex_tamper.py for why a mere bit-flip proves nothing
 *      here) loads with sig_status == AEX_SIG_INVALID, never OK.
 *   3. A signature from a real key that is NOT a compiled-in root loads with
 *      sig_status == AEX_SIG_UNTRUSTED, sig_root == -1, never OK.
 *   4. THE DOMAIN-SEPARATION CONTROL, and the one that matters most: a
 *      validly-signed .lpk header's raw signer+signature bytes, presented to
 *      aex_sig_verify() as though they were an AEX_T_SIG record over the SAME
 *      length and digest they actually cover, must NOT verify. If it did,
 *      AEX_SIG_DOMAIN would be decorative and any .lpk ever signed by a root
 *      could be replayed as a verified .aex.
 *
 * In every one of the four cases the loader still returns AEX_OK and would
 * load the program -- LOG BUT ALLOW, see aex.c. This file asserts the
 * VERDICT, not a refusal, because there is no refusal to assert.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "elf.h"
#include "aex.h"
#include "aexsig.h"
#include "pkgsig.h"
#include "crypto.h"

static int g_checks, g_fails;

static void ck(int cond, const char *what)
{
    g_checks++;
    if (!cond) { g_fails++; printf("FAIL: %s\n", what); }
    else printf("ok: %s\n", what);
}

static uint8_t *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)*n);
    if (!b) { fclose(f); return NULL; }
    if (*n && fread(b, 1, (size_t)*n, f) != (size_t)*n) { free(b); fclose(f); return NULL; }
    fclose(f);
    return b;
}

static int parse_file(const char *path, struct aex_info *out)
{
    long n = 0;
    uint8_t *buf = slurp(path, &n);
    if (!buf) { printf("FAIL: cannot read %s\n", path); g_fails++; g_checks++; return -1; }
    int rc = aex_parse(buf, (uint64_t)n, out);
    free(buf);
    return rc;
}

/* ---- parts 1-3: real fixtures, through the real loader ------------------ */
static void check_fixture(const char *path, int want_status, const char *label)
{
    struct aex_info in;
    int rc = parse_file(path, &in);
    char msg[256];
    snprintf(msg, sizeof msg, "%s: aex_parse succeeds (%s)", label, path);
    ck(rc == AEX_OK, msg);
    if (rc != AEX_OK) return;

    snprintf(msg, sizeof msg, "%s: sig_status == %d (got %d)", label, want_status, in.sig_status);
    ck(in.sig_status == want_status, msg);

    if (want_status == AEX_SIG_OK) {
        snprintf(msg, sizeof msg, "%s: sig_root >= 0 when OK (got %d)", label, in.sig_root);
        ck(in.sig_root >= 0, msg);
    } else {
        snprintf(msg, sizeof msg, "%s: sig_root == -1 when not OK (got %d)", label, in.sig_root);
        ck(in.sig_root == -1, msg);
    }
}

/* ---- part 4: the domain-separation replay control ------------------------ */
static void check_domain_separation(void)
{
    /* An arbitrary "image" -- its bytes never touch a real loader here, only
     * lpk_sign (which hashes them as an .lpk PAYLOAD) and sha256 (which
     * hashes them as an .aex ELF DIGEST), so any bytes exercise the point. */
    static const uint8_t image[] = "this is not really an ELF image, and that "
                                   "is fine -- only its length and digest matter here";
    uint64_t image_len = sizeof image - 1;

    /* The published development seed (tools/pkgroots/DEV-SIGNING-KEY.txt) --
     * a REAL compiled-in root, so a failure here cannot be blamed on an
     * untrusted key; only the domain can be the reason this fails. */
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = 0; /* overwritten below */
    {
        static const char *hex =
            "dd5e99f7c5f19186cc0053d5905b54016586d7b366672597d5b61976d965a00b";
        /* This constant is 66 hex chars in the Makefile/DEV-SIGNING-KEY.txt
         * (one stray trailing byte beyond the 32-byte seed) -- only the
         * first 64 matter for unhex32, mirrored here rather than imported so
         * this file has no dependency on tools/lpk.c. */
        for (int i = 0; i < 32; i++) {
            int v = 0;
            for (int k = 0; k < 2; k++) {
                char c = hex[2 * i + k];
                int d = (c >= '0' && c <= '9') ? c - '0'
                      : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0;
                v = (v << 4) | d;
            }
            seed[i] = (uint8_t)v;
        }
    }

    /* Sign `image` as a REAL .lpk, under LPK_DOMAIN. */
    uint8_t lpk_hdr[LPK_HDR_LEN];
    int rc = lpk_sign(lpk_hdr, "not-an-aex", image, image_len, seed);
    ck(rc == 0, "domain control: lpk_sign succeeds");
    if (rc != 0) return;

    /* Confirm it is a REAL, verifying .lpk signature first -- if this fails,
     * the control below is meaningless, not passing. */
    struct lpk lout;
    /* lpk_sign wrote its own SHA-256 of `image` into hdr[96..128); reuse the
     * same bytes buffer for lpk_verify by concatenating header + payload. */
    uint8_t *lpk_file = malloc(LPK_HDR_LEN + image_len);
    memcpy(lpk_file, lpk_hdr, LPK_HDR_LEN);
    memcpy(lpk_file + LPK_HDR_LEN, image, image_len);
    rc = lpk_verify(lpk_file, LPK_HDR_LEN + image_len, &lout, 0);
    free(lpk_file);
    ck(rc == LPK_OK && lout.root_index >= 0,
       "domain control: the .lpk itself verifies under a real root (sanity check)");

    /* THE REPLAY. Lift the raw signer(32)+sig(64) bytes straight out of the
     * .lpk header -- exactly LPK_HDR_LEN's layout at offsets 128 and 160,
     * exactly AEX_SIG_LEN (96) bytes, the identical shape aex_sig_verify()
     * reads out of an AEX_T_SIG record -- and hand them to it along with the
     * SAME (length, digest) the .lpk signature actually covers. If domain
     * separation holds, this must NOT verify: the .lpk signature is over
     * SHA-256(LPK_DOMAIN || lpk_hdr[0..160)), never over
     * SHA-256(AEX_SIG_DOMAIN || elf_size || elf_sha256), even though the
     * bytes handed to aex_sig_verify() below are the (length, digest) pair
     * that WOULD make it verify under the AEX domain. */
    uint8_t sigrec[AEX_SIG_LEN];
    memcpy(sigrec, lpk_hdr + 128, 32);   /* signer, same offset lpk_verify reads */
    memcpy(sigrec + 32, lpk_hdr + 160, 64); /* sig, same offset lpk_verify reads */

    uint8_t digest[32];
    sha256(image, (size_t)image_len, digest);

    int root = -2; /* poison: aex_sig_verify must set it, not leave it alone */
    int v = aex_sig_verify(sigrec, image_len, digest, &root);
    ck(v != AEX_SIG_OK && v != AEX_SIG_UNTRUSTED,
       "DOMAIN SEPARATION: a real .lpk signature does NOT verify as an .aex signature");
    ck(v == AEX_SIG_INVALID, "DOMAIN SEPARATION: verdict is specifically AEX_SIG_INVALID");
    ck(root == -1, "DOMAIN SEPARATION: root_index_out is -1 on a failed verify");
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: aexsig_test <ok.aex> <tampered.aex> <foreign.aex> <unsigned.aex>\n");
        return 1;
    }

    check_fixture(argv[1], AEX_SIG_OK, "signed by dev root");
    check_fixture(argv[2], AEX_SIG_INVALID, "tampered payload, CRC repaired");
    check_fixture(argv[3], AEX_SIG_UNTRUSTED, "signed by a non-root key");
    check_fixture(argv[4], AEX_SIG_ABSENT, "unsigned");

    check_domain_separation();

    printf("%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
