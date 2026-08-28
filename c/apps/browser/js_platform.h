#ifndef LOGIT_JS_PLATFORM_H
#define LOGIT_JS_PLATFORM_H

#include "quickjs.h"

/* The parts of the web platform that are neither the DOM tree (js_dom.c) nor
 * the network (js_webapi.c): timing, the document lifecycle, task and message
 * queues, errors, cloning, entropy, the observers.
 *
 * WHY THIS FILE EXISTS, AND WHY ITS CONTENTS ARE NOT A GUESS
 * It was written from a measurement, not from a list of Web APIs. tests/unit/
 * webapi_probe.c loads seven real pages from committed bytes, runs their
 * scripts under a Proxy that records every global and every platform-object
 * property the runtime cannot answer, and ranks the misses by how many pages
 * need each one. The first run of that probe found something worth writing
 * down: the GLOBALS were almost fine. Seventeen names missed across seven
 * pages, and thirteen of them were the pages' own -- `_w`, `RLQ`, `_N_E` --
 * missing only because the script that defines them had already died. What the
 * pages actually die on is one level down: `performance.timing.navigationStart`
 * (bing), `performance.mark` (bing, wikipedia), `document.readyState` (bing,
 * deepseek), `localStorage.<name>` (bing), `navigator.scheduling` (deepseek).
 * Properties of objects that exist and are incomplete.
 *
 * So the order of this file is the order of that table, and every entry in it
 * is something a page in the corpus reached for. Where something is here that
 * the corpus did not reach -- Blob, FormData, crypto, the observers, Intl --
 * it is marked as such in its own comment, so the next person can tell the
 * measured half from the requested half.
 *
 * WHAT IS DELIBERATELY ABSENT, AND MUST STAY ABSENT
 * The probe also records misses that are CORRECT. `window.ActiveXObject`
 * (bing), `document.documentMode` (deepseek) and `window.MSApp` are how a page
 * detects Internet Explorer; `window.indexedDB` and `__REACT_DEVTOOLS_GLOBAL_
 * HOOK__` are feature detection whose false branch is the one we want. Defining
 * any of them would make pages take a path we cannot follow. A missing global
 * is not automatically a bug, and the probe's job is to tell you which kind you
 * are looking at -- not to be emptied.
 *
 * `window.indexedDB` earns its own paragraph because it is the one entry here
 * with a real, large spec behind it, which makes it the one a future patch is
 * most likely to "helpfully" half-build. Re-measured 2026-08-28,
 * WEBAPI_FILE_ROOT=build/wpt-full wbuild/wpt_test --root build/wpt-full
 * --subset IndexedDB: 5 PASS of 1078 IndexedDB/ subtests, and every one of the
 * five is vacuous -- `indexedDB is [SameObject]` comparing undefined to
 * undefined, three `"IDBFileHandle"/"IDBFileRequest"/"IDBMutableFile" should
 * not be supported` historical negatives, and one keyrange test whose expected
 * failure happens to be a ReferenceError. Absence is currently 100% correct on
 * this corpus. The other 1073 fail as `'indexedDB' is not defined`, which is
 * the right failure for a spec this engine does not implement -- not a defect
 * to close by making the ReferenceError go away.
 *
 * The two real call sites in this repo's own fixture corpus are why "just
 * define indexedDB" would make things worse, not better, and they are worth
 * reading side by side. tests/fixtures/webapi/bing/index.html guards every use
 * behind `window.indexedDB` truthiness (`isIotdEnabled===0 && window.indexedDB`
 * before ever calling `.open`) and wraps the callback tree in try/catch with a
 * 2s watchdog timeout that falls back regardless -- with indexedDB absent the
 * guard is false, `c(2)` never runs, and the page degrades cleanly to no
 * image-of-the-day background. tests/fixtures/jsperf/baidu-async-search.js's
 * class `S` does NOT guard: its constructor unconditionally calls
 * `window.indexedDB.open(t.databaseName)` and only checks readiness on later
 * set()/get() calls via `this.db`, queuing work in `setQueue`/`getQueue` until
 * `onsuccess` fires. With indexedDB absent that constructor throws a
 * TypeError immediately, `isSupport()` (`!!window.indexedDB`) already reported
 * false, and the caller's own fallback runs -- observed behaviour, and safe.
 * A PARTIAL indexedDB whose `.open()` returns a request that never calls
 * `onsuccess` or `onerror` (any unimplemented codepath that returns a request
 * object instead of throwing or firing an error event) would leave exactly
 * this class's two queues filling forever: no exception, no failed network
 * request, no log line, every future set()/get() silently swallowed. That is
 * the identical shape to the Node.isEqualNode/React-hydration and
 * getContext-returns-null traps recorded elsewhere in this tree -- a
 * capability present by name that answers wrongly is more dangerous than one
 * genuinely missing, because feature detection passes and the page walks into
 * a path that cannot work. So the bar for implementing indexedDB is not "cover
 * enough methods" but "every IDBRequest this build can produce, on every path
 * including the ones left unimplemented, terminates in success or a thrown/
 * fired error -- never silently pending." Until that bar is met for a real
 * subset, indexedDB stays undefined on purpose; see IDBFactory/IDBDatabase/
 * IDBObjectStore/etc. -- none of it exists anywhere in this tree
 * (`grep -rni indexeddb c/apps/browser include` finds only this comment).
 *
 * EVERYTHING INSTALLS ONLY IF ABSENT. Three lines are extending this runtime at
 * once (the DOM bindings, the module loader, this one). Whatever is already
 * there wins, so landing after another line's work cannot silently replace it.
 */

#include "../../../include/weaksym.h"   /* LOGIT_WEAK/_STUB: see the header */
#ifdef JS_PLATFORM_OPTIONAL
#  define PLATFORM_FN LOGIT_WEAK
#else
#  define PLATFORM_FN
#endif

/* Install into `ctx`. Call LAST -- after js_dom_init and js_webapi_install --
 * because it fills gaps in the objects they publish (document, navigator,
 * performance, localStorage) and because "only if absent" is only meaningful
 * once everyone else has had their turn. */
PLATFORM_FN void js_platform_install(JSContext *ctx);

/* Release everything held here before JS_FreeContext: the promise-rejection
 * tracker's context pointer and the observer registry. */
PLATFORM_FN void js_platform_close(JSContext *ctx);

/* The viewport an IntersectionObserver measures against. Same values the
 * embedder gives js_webapi_set_viewport. */
PLATFORM_FN void js_platform_set_viewport(int w, int h);

/* querySelectorAll / getElementsBy* / matches / closest -- js_select.c. A
 * separate entry point (and a separate file) because they are on loan from the
 * DOM bindings: they install only if absent and the whole file is meant to be
 * deleted the day js_dom.c grows its own. Call after js_dom_init. */
PLATFORM_FN void js_select_install(JSContext *ctx);

/* Intl + SuppressedError -- js_intl.c. Its own file because it is its own
 * thing: a monolingual English formatter wearing the Intl interface, which the
 * header of that file explains rather than hides. */
PLATFORM_FN void js_intl_install(JSContext *ctx);

/* crypto.subtle -- js_subtle.c. CryptoKey + SubtleCrypto + algorithm
 * normalization + raw/jwk import-export for symmetric and EC/OKP keys.
 * Deliberately no 'spki'/'pkcs8' (no DER reader/writer here) and no actual
 * digest/encrypt/sign/derive result (CRYPTO_SRC is not linked into this
 * binary) -- see js_subtle.c's header for the full boundary. Call anywhere
 * after js_platform_install has created `crypto` (order relative to
 * js_select/js_intl does not matter; it installs only if crypto.subtle is
 * absent). */
PLATFORM_FN void js_subtle_install(JSContext *ctx);

/* The Mach-O half of the weak declarations above (include/weaksym.h): an
 * undefined weak reference is an ELF property, so each optional entry point
 * needs a weak definition in the TU that may not link the provider. Emitted
 * only under JS_PLATFORM_OPTIONAL, i.e. only in that TU. */
#ifdef JS_PLATFORM_OPTIONAL
LOGIT_WEAK_STUB(js_platform_install);
LOGIT_WEAK_STUB(js_platform_close);
LOGIT_WEAK_STUB(js_platform_set_viewport);
LOGIT_WEAK_STUB(js_select_install);
LOGIT_WEAK_STUB(js_intl_install);
LOGIT_WEAK_STUB(js_subtle_install);
#endif

#endif /* LOGIT_JS_PLATFORM_H */
