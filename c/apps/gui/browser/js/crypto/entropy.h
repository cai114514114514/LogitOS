/* SPDX-License-Identifier: MIT */
#ifndef LOGIT_WEB_ENTROPY_H
#define LOGIT_WEB_ENTROPY_H
/* Key generation and getRandomValues must share the same refusal boundary.
 * A clock-seeded fallback keeps a page running but changes what "random"
 * promises. Host fixtures use their OS entropy; the guest uses its DRBG only
 * when the kernel reports an actual entropy source. */
#ifdef WEBAPI_HOST
#include <unistd.h>
#ifdef __APPLE__
#include <sys/random.h> /* Darwin declares getentropy here, not in unistd.h. */
#endif
static int web_entropy(void *out, int n)
{
#ifdef WEB_ENTROPY_NEGCTL
    (void)out; (void)n; return -1;
#else
    unsigned char *p = out;
    while (n > 0) { int take = n > 256 ? 256 : n;
        if (getentropy(p, (size_t)take)) return -1;
        p += take; n -= take;
    }
    return 0;
#endif
}
#else
#include "logit.h"
static int web_entropy(void *out, int n)
{
#ifdef WEB_ENTROPY_NEGCTL
    (void)out; (void)n; return -1;
#else
    return getrandom_strong() > 0 ? getrandom_bytes(out, n) : -1;
#endif
}
#endif
#endif
