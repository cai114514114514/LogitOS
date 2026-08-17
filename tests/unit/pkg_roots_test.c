/* WHICH TABLE DID THE VERIFIER CONSULT?  (make test-pkg-roots)
 *
 * pkgsig.h promises that moving the package trust set off the ISO is "local to
 * pkg_root_count/pkg_root_key". lpk_verify used to walk the compiled-in
 * PKG_ROOTS array itself, while /bin/pkgverify --roots -- the diagnostic whose
 * whole job is to say who is trusted -- went through the accessors. Two
 * readers, one of which the header does not know about.
 *
 * WHY THE EXISTING GATES CANNOT SEE IT, and why this file exists. With
 * PKG_ROOT_N == 1 the array and the accessors name the same 32 bytes, so every
 * fixture in tests/unit/pkg_test.c and every line of tests/boot/run-pkg-test.sh
 * gives the identical verdict either way. A gate over a one-entry table proves
 * nothing about how the table was reached.
 *
 * So the table is made to have TWO entries, in a scratch build that ships
 * nowhere: the foreign signer -- the key that signs build/foreign.lpk, the
 * fixture whose entire purpose is to be a VALID signature this machine refuses
 * -- is added as a second root, and foreign.lpk must FLIP from refused to
 * accepted. Two properties fall out of that flip and neither is reachable with
 * one root:
 *
 *   1. The walk covers the whole table. The scratch root is named so it sorts
 *      SECOND (genpkgroots.py sorts by filename), so a verifier that compares
 *      against entry 0 and stops -- which is what a one-root table lets you get
 *      away with writing -- still refuses foreign.lpk and fails here.
 *   2. The index the verdict reports is an index into the accessors'
 *      enumeration. `pkg_root_key(out.root_index)` must be byte-for-byte the
 *      key that actually matched. That is the exact assertion that goes red the
 *      day pkg_root_key starts reading a disk and lpk_verify does not: the
 *      listing and the verdict would be describing different entries, and the
 *      UI would print the name of a signer that did not sign this file.
 *
 * The same binary is built against the ORDINARY one-root include and run with
 * `refused`, in the same make target, so the flip is observed rather than
 * assumed -- a scratch build that accepted foreign.lpk for some unrelated
 * reason would be indistinguishable from a passing gate otherwise.
 *
 *   pkg_roots_test <hello.lpk> <tampered.lpk> <foreign.lpk> refused|accepted
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "pkgsig.h"

static int checks, failed;
static void ok(int cond, const char *what)
{
    checks++;
    if (cond) printf("ok   %s\n", what);
    else { printf("FAIL %s\n", what); failed++; }
}

static uint8_t *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "pkg_roots_test: cannot read %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n + 1);
    if (!b || (n && fread(b, 1, (size_t)n, f) != (size_t)n)) { fprintf(stderr, "read failed\n"); exit(2); }
    fclose(f);
    *len = n;
    return b;
}

/* The scratch root's name. It must sort after "logitos-dev" or it lands at
 * index 0 and property (1) above is not being tested at all. */
#define SCRATCH_ROOT "zz-scratch-foreign"

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: pkg_roots_test <hello.lpk> <tampered.lpk> <foreign.lpk> refused|accepted\n");
        return 2;
    }
    int expect_foreign_ok = !strcmp(argv[4], "accepted");
    if (!expect_foreign_ok && strcmp(argv[4], "refused")) {
        fprintf(stderr, "pkg_roots_test: mode must be 'refused' or 'accepted'\n");
        return 2;
    }

    printf("--- trust set as the ACCESSORS report it (%d signer(s)) ---\n", pkg_root_count());
    for (int r = 0; r < pkg_root_count(); r++) {
        const uint8_t *k = pkg_root_key(r);
        printf("     [%d] %-20s ", r, pkg_root_name(r));
        for (int i = 0; i < 32; i++) printf("%02x", k[i]);
        printf("\n");
    }

    ok(pkg_root_count() == (expect_foreign_ok ? 2 : 1),
       expect_foreign_ok ? "the scratch build has TWO roots" : "the shipped build has ONE root");
    ok(pkg_root_name(0) && !strcmp(pkg_root_name(0), "logitos-dev"),
       "root 0 is the development signer in both builds");

    long n;
    struct lpk out;
    int r;

    /* --- hello.lpk: signed by root 0, in both builds ---------------------- */
    uint8_t *hello = slurp(argv[1], &n);
    r = lpk_verify(hello, (uint64_t)n, &out, 0);
    ok(r == LPK_OK, "hello.lpk verifies");
    ok(r == LPK_OK && out.root_index == 0, "...and reports root_index 0");
    ok(r == LPK_OK && out.root_index >= 0 &&
       memcmp(pkg_root_key(out.root_index), out.signer, 32) == 0,
       "...and pkg_root_key(root_index) IS the key that matched (listing == verdict)");

    /* --- tampered.lpk: a bigger trust set must not make a broken package
     * acceptable. This is the assertion that catches a 'fix' that widened the
     * trust check by loosening it. --------------------------------------- */
    uint8_t *tamp = slurp(argv[2], &n);
    r = lpk_verify(tamp, (uint64_t)n, &out, 0);
    ok(r == LPK_E_DIGEST, "REJECT: tampered.lpk is still refused on its digest");

    /* --- foreign.lpk: THE FLIP -------------------------------------------- */
    uint8_t *foreign = slurp(argv[3], &n);
    r = lpk_verify(foreign, (uint64_t)n, &out, 0);
    if (!expect_foreign_ok) {
        ok(r == LPK_E_UNTRUSTED, "REJECT: foreign.lpk, one root -- valid signature, unknown signer");
    } else {
        ok(r == LPK_OK, "foreign.lpk is ACCEPTED once its signer is a root");
        ok(r == LPK_OK && out.root_index == 1,
           "...at index 1, so the walk did not stop at entry 0");
        ok(r == LPK_OK && pkg_root_name(1) && !strcmp(pkg_root_name(1), SCRATCH_ROOT),
           "...and that index names " SCRATCH_ROOT " through the accessors");
        ok(r == LPK_OK && out.root_index >= 0 &&
           memcmp(pkg_root_key(out.root_index), out.signer, 32) == 0,
           "...and pkg_root_key(root_index) IS the key that matched");
        /* The same bytes under --any must ALSO be ok, and must now name a root
         * rather than -1: proof the acceptance came from the trust set and not
         * from trust_any leaking into the normal path. */
        r = lpk_verify(foreign, (uint64_t)n, &out, 1);
        ok(r == LPK_OK && out.root_index == 1,
           "...and trust_any on the same bytes still attributes it to root 1");
    }

    free(hello); free(tamp); free(foreign);   /* the gate runs under leak-check */

    printf("\n%d checks, %d failed\n", checks, failed);
    if (!failed) printf("PKG ROOTS %s: ALL PASS\n", argv[4]);
    return failed ? 1 : 0;
}
