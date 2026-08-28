#ifndef LOGIT_JS_IDB_H
#define LOGIT_JS_IDB_H

#include "quickjs.h"

/* IndexedDB: IDBFactory/IDBDatabase/IDBTransaction/IDBObjectStore/IDBIndex/
 * IDBCursor/IDBKeyRange/IDBRequest, a coherent in-memory subset scoped to
 * the page session (one JSContext's lifetime -- see js_idb.c's header for
 * why that is the honest amount of durability on this machine).
 *
 * Install AFTER js_events_install: every object here is a real subclass of
 * the constructible EventTarget js_events.c publishes (new.target-checked,
 * addEventListener/removeEventListener/dispatchEvent, no DOM ancestors to
 * walk), and after js_platform_install, which is what creates
 * G.DOMException and G.structuredClone -- both load-bearing here. "Only if
 * `G.indexedDB` is absent and all three dependencies exist" is the whole
 * install guard; a build missing any of EventTarget/DOMException/
 * structuredClone gets no indexedDB rather than a half-built one, which
 * matches this file's own rule that a present-but-wrong indexedDB is worse
 * than an absent one.
 *
 * Weak under JS_IDB_OPTIONAL, the same convention as js_events.h /
 * js_platform.h / js_webapi.h: a build that does not link this TU (the WPT
 * runner's stock source list, most host tests) still links, and simply has
 * no indexedDB -- `if (LOGIT_HAVE(js_idb_install)) js_idb_install(ctx);` at
 * the call site is what makes that true. */
#include "../../../include/weaksym.h"   /* LOGIT_WEAK/_STUB: see the header */
#ifdef JS_IDB_OPTIONAL
#  define IDB_FN LOGIT_WEAK
#else
#  define IDB_FN
#endif

IDB_FN void js_idb_install(JSContext *ctx);

#ifdef JS_IDB_OPTIONAL
LOGIT_WEAK_STUB(js_idb_install);
#endif

#endif /* LOGIT_JS_IDB_H */
