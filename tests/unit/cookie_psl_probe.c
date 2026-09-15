/* SPDX-License-Identifier: MIT
 * Compile the product TU, then expose its private registrable-domain helper
 * for upstream PSL fixtures. Deriving that helper by repeatedly asking whether
 * a shorter suffix is public is WRONG around wildcard gaps (kobe.jp versus
 * c.kobe.jp), so the test must observe the actual prevailing-rule result. */
#include "../../c/net/http/cookies.c"
int cookie_psl_probe_domain(const char *host, char *out, int cap)
{
    if (cookie_domain_is_public_suffix(host)) return -1;
    return registrable_domain(host, out, cap);
}
