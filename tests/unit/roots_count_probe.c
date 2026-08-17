/* roots_count_probe -- report and assert the SIZE of the compiled-in trust store.
 *
 * Fixture for the fail-closed block of tests/unit/run-tls-interop.sh, and it
 * exists for one specific reason.
 *
 * That block proves "zero anchors refuses everything" by generating a bundle
 * from an EMPTY roots directory and linking it in place of the real store. The
 * whole proof rests on the substitute store actually being empty -- and there
 * are two ways for that to be false without anything looking wrong:
 *
 *   1. tools/genroots.py silently emits something. A bundle with one root in it
 *      would make every case in the block connect successfully, which reads as
 *      "the fail-closed property is broken" -- the right verdict for the wrong
 *      reason, and the wrong thing to go fix.
 *
 *   2. The bundle does not compile at all. genroots.py writes
 *      `logit_roots[] = {` + rows + `};`, so with no rows it emits an EMPTY C
 *      INITIALISER (tools/genroots.py:222-224). That is a C23 feature and a GNU
 *      extension; clang accepts it at the default -std (measured: two warnings
 *      under -std=c11 -pedantic, none at the default, logit_nroots == 0 either
 *      way). But a harness that only asks "did the run fail?" would score a
 *      BUILD FAILURE as a pass, on any toolchain where that stops being true.
 *
 * So the block builds this, requires it to LINK, and requires it to say the
 * count it expects -- before a single handshake is attempted.
 *
 * `logit_nroots` is deliberately the only thing asserted: it is the exact bound
 * of both trust loops (c/net/tls/x509.c:422 is_pinned_root, :447
 * signed_by_root), so it is the number whose being zero is what makes
 * x509_verify_chain return X509_E_UNTRUSTED at x509.c:534-536. Anything else
 * about the store would be a proxy for it.
 *
 * usage: roots_count_probe zero|nonzero
 *   zero     -- require logit_nroots == 0   (the substitute store)
 *   nonzero  -- require logit_nroots >  0   (the positive control: the same
 *               probe against the real bundle, so a probe that reports 0
 *               unconditionally cannot pass both invocations)
 */
#include <stdio.h>
#include <string.h>
#include "roots.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s zero|nonzero\n", argv[0]);
        return 2;
    }
    printf("logit_nroots=%d logit_nroots_skipped=%d\n",
           logit_nroots, logit_nroots_skipped);

    if (strcmp(argv[1], "zero") == 0) {
        if (logit_nroots != 0) {
            fprintf(stderr, "roots_count_probe: expected an EMPTY trust store, "
                            "got %d root(s) -- the substitute bundle is not empty, "
                            "so the fail-closed cases would prove nothing\n",
                    logit_nroots);
            return 1;
        }
        return 0;
    }
    if (strcmp(argv[1], "nonzero") == 0) {
        if (logit_nroots <= 0) {
            fprintf(stderr, "roots_count_probe: expected a NON-EMPTY trust store, "
                            "got %d -- this probe cannot tell the two bundles "
                            "apart, so its 'zero' verdict means nothing\n",
                    logit_nroots);
            return 1;
        }
        return 0;
    }
    fprintf(stderr, "roots_count_probe: unknown expectation '%s'\n", argv[1]);
    return 2;
}
