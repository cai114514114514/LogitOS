#ifndef LOGIT_JS_SWREG_H
#define LOGIT_JS_SWREG_H

#include "quickjs.h"

/* navigator.serviceWorker: the CLIENT-side registration surface only (part
 * B of the ServiceWorker workflow item). ServiceWorkerContainer /
 * ServiceWorkerRegistration / ServiceWorker exist for `instanceof` and
 * idlharness; register() runs every real check (URL parse, secure origin,
 * same-origin, scope-under-script-path, module type) and then ALWAYS
 * REJECTS, naming the two structural blockers this build has not closed
 * (js_cache.c/js_swreg.c's own headers, and js_worker.c's promise-drain
 * fix, name them). `controller` is `null`, always -- see js_swreg.c's
 * header for why that is the honest answer rather than a gap.
 *
 * EVERY PROMISE THIS FILE HANDS OUT SETTLES. That is the one property this
 * header exists to assert, because the alternative -- a registration object
 * nothing ever advances, or `ready` pending forever the way the spec
 * actually defines it -- is the canonical hang this whole workflow item was
 * scoped around (see js_swreg.c's header for the measured evidence: this
 * tree's own corpus, bing/index.html, awaits a controller round-trip with
 * NO timeout).
 *
 * Install AFTER js_platform_install (needs G.DOMException) and after
 * js_page.c has already created `navigator` (js_page.c does this directly,
 * before js_webapi_install -- see js_page.c's own ordering comment); this
 * file only ADDS a property to the existing object, never creates it.
 * Weak under JS_SWREG_OPTIONAL, same convention as js_idb.h/js_cache.h. */
#include "../../../include/weaksym.h"   /* LOGIT_WEAK/_STUB: see the header */
#ifdef JS_SWREG_OPTIONAL
#  define SWREG_FN LOGIT_WEAK
#else
#  define SWREG_FN
#endif

SWREG_FN void js_swreg_install(JSContext *ctx);

#ifdef JS_SWREG_OPTIONAL
LOGIT_WEAK_STUB(js_swreg_install);
#endif

#endif /* LOGIT_JS_SWREG_H */
