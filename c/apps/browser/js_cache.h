#ifndef LOGIT_JS_CACHE_H
#define LOGIT_JS_CACHE_H

#include "quickjs.h"

/* CacheStorage (`caches` / `Cache`): a real, session-scoped store on the
 * WINDOW realm, over the SAME Request/Response/Headers/fetch this file's
 * neighbour js_webapi.c already publishes -- not a second implementation of
 * any of the four. See js_cache.c's own header for the full design note and
 * the measurement that produced this scope (part A of the ServiceWorker
 * workflow item; parts B/C are js_swreg.c and a deliberate refusal).
 *
 * Install AFTER js_webapi_install (needs G.fetch/Request/Response/Headers/
 * URL, all real) and after js_platform_install (needs G.DOMException). A
 * build missing any of those gets no `caches` rather than a half-built one,
 * matching js_platform.h's rule that present-but-wrong is worse than absent.
 *
 * Weak under JS_CACHE_OPTIONAL, same convention as js_idb.h / js_worker.h. */
#include "../../../include/weaksym.h"   /* LOGIT_WEAK/_STUB: see the header */
#ifdef JS_CACHE_OPTIONAL
#  define CACHE_FN LOGIT_WEAK
#else
#  define CACHE_FN
#endif

CACHE_FN void js_cache_install(JSContext *ctx);

#ifdef JS_CACHE_OPTIONAL
LOGIT_WEAK_STUB(js_cache_install);
#endif

#endif /* LOGIT_JS_CACHE_H */
