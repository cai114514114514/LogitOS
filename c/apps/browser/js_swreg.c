/* c/apps/browser/js_swreg.c -- navigator.serviceWorker, part B of the
 * ServiceWorker workflow item (part A is js_cache.c; part C -- EXECUTING a
 * service worker script and intercepting fetch -- is REFUSED, below and in
 * the workflow's own scope note).
 *
 * WHY register() ALWAYS REJECTS. Two structural findings, measured before
 * writing this file:
 *
 *   1. A promise reaction queued inside a Worker's OWN JSRuntime never ran
 *      until js_worker.c's worker_drain_jobs() landed alongside this file
 *      (2026-08-29) -- and the entire service worker lifecycle is promises
 *      (`event.waitUntil(caches.open(n).then(...))` is the idiom). Even
 *      with that fixed, finding 2 stands:
 *   2. `fetch`/`Request`/`Response`/`Headers` are singleton-bound to the
 *      PAGE realm (js_webapi.c's g_mk_response/g_fetch[]/g_stores[]/g_loc
 *      are file statics; js_webapi_install(ctx,url) cannot be called twice
 *      for two realms). A ServiceWorkerGlobalScope with no fetch, no
 *      Response and no Request is not a partial service worker, it is a
 *      hang generator -- exactly the js_platform.h:66-70 shape.
 *
 * So register() does every check the spec puts BEFORE a job would start --
 * script URL parse, secure-context origin, same-origin, scope-under-the-
 * script's-own-directory, module type -- and then rejects NotSupportedError
 * naming the two blockers above, rather than handing back a registration
 * nothing ever advances.
 *
 * WHY `ready` REJECTS (a DELIBERATE SPEC DEVIATION, stated rather than
 * hidden): the spec says `ready` never rejects. But `ready` only RESOLVES
 * once a registration has an active worker, and one never exists in this
 * build -- so the spec-conformant behaviour here is a promise pending
 * forever, which is the single most common ServiceWorker idiom
 * (`navigator.serviceWorker.ready.then(...)`) turned into exactly the hang
 * this workflow exists to prevent. Rejecting is terminal; "spec-accurate
 * and pending forever" is not, and this file chooses terminal.
 *
 * WHY `controller` IS `null`, ALWAYS: it is not merely unbuilt, it is
 * honest. This tree's own corpus names the reason directly --
 * tests/fixtures/webapi/bing/index.html writes, inside a `'serviceWorker'
 * in navigator` guard:
 *
 *     new Promise(function (t, i) {
 *       r.port1.onmessage = ...;
 *       navigator.serviceWorker.controller.postMessage(n, [r.port2]);
 *     })
 *
 * a promise with NO timeout whose only settle path is a reply from the
 * controller. Before this file existed, `navigator.serviceWorker` itself
 * threw a TypeError there, which killed the rest of that script -- a
 * present-and-wrong `controller` that could not answer would leave that
 * exact promise pending forever instead. `null` makes the guard's condition
 * (`navigator.serviceWorker.controller && ...`) false, and the script
 * degrades the way it was written to.
 *
 * PART C, WHY IT IS REFUSED RATHER THAN HALF-BUILT: FetchEvent /
 * respondWith / Client / Clients / clients.claim() are simply not defined.
 * This browser has TWO doors onto the network -- js_webapi.c's fetch()
 * (also what an iframe would navigate through) and bfetch (how layout.c and
 * browser.c load every <img>/<script>/<link>) -- and they share no code. An
 * interception hook on one is a browser where half the page is intercepted
 * and half is not, and per spec a respondWith() that resolves with
 * `undefined` IS a network error: "a registration that intercepts fetches
 * and then does not answer them removes the page's network" (this
 * workflow's own brief). Not built.
 *
 * DELIBERATELY NOT HERE, refused rather than half-built, matching js_cache.c's
 * list in shape:
 *   - Persistence of a registration across a page load. Session-scoped
 *     (there is never anything to persist, since register() never succeeds)
 *     -- and it is what makes `controller === null` honest: every load
 *     really is a first visit here.
 *   - `{type:'module'}` on register() -- rejects NotSupportedError by name,
 *     checked BEFORE the generic "not executed" rejection so a page testing
 *     for module-worker support specifically gets the specific answer.
 *   - PushManager / Notification / sync / periodicSync / navigationPreload
 *     on a registration -- left `undefined`.
 *   - A secure-context gate reading `G.isSecureContext` -- js_platform.c
 *     hardcodes that `true`, so gating on it would be decoration. This file
 *     checks the document's ACTUAL scheme/host against the spec's
 *     trustworthy-origin list (https, or localhost/127.0.0.1/::1) instead.
 *     `caches` (js_cache.c) is NOT gated at all, and that asymmetry is
 *     deliberate -- caches has no execution surface to protect.
 *
 * Install AFTER js_platform_install (needs G.DOMException) -- see
 * js_swreg.h. Only if `navigator` already exists (js_page.c always creates
 * it directly, before js_webapi_install) and `navigator.serviceWorker` is
 * not already present.
 */

#include "js_swreg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *SWREG_JS =
"(function (G) {\n"
"if (!G.navigator) return;\n"
"if (G.navigator.serviceWorker) return;\n"
"if (typeof G.Promise !== 'function') return;\n"
"if (typeof G.EventTarget !== 'function') return;\n"
"if (typeof G.DOMException !== 'function') return;\n"
"if (typeof G.URL !== 'function') return;\n"
"\n"
"function named(f, n, len) {\n"
"  try { Object.defineProperty(f, 'name', { value: n, configurable: true }); } catch (e) {}\n"
"  try { Object.defineProperty(f, 'length', { value: len, configurable: true }); } catch (e) {}\n"
"  return f;\n"
"}\n"
"function domErr(msg, name) { return new G.DOMException(msg, name); }\n"
"function illegal(name) { return named(function () { throw new TypeError('illegal constructor: ' + name); }, name, 0); }\n"
"\n"
/* ==== ServiceWorker -- never instantiated by this build. Defined only so
 * `typeof ServiceWorker === 'function'` and idlharness can see the shape. */
"var ServiceWorkerProto = {\n"
"  postMessage: function () { throw domErr('no active service worker to receive this message', 'InvalidStateError'); }\n"
"};\n"
"Object.defineProperties(ServiceWorkerProto, {\n"
"  scriptURL: { get: function () { return this._scriptURL || ''; }, enumerable: true, configurable: true },\n"
"  state:     { get: function () { return this._state || 'redundant'; }, enumerable: true, configurable: true }\n"
"});\n"
"\n"
/* ==== ServiceWorkerRegistration -- same: shape only, never handed out.
 * update()/unregister() reject rather than silently no-op, so a future path
 * that DOES hand one out (there is none today -- register() always rejects,
 * getRegistration() always resolves undefined) cannot accidentally inherit
 * a promise that never settles. */
"var RegProto = {\n"
"  update: function () { return G.Promise.reject(domErr('this registration has no running service worker', 'InvalidStateError')); },\n"
"  unregister: function () { return G.Promise.reject(domErr('this registration has no running service worker', 'InvalidStateError')); }\n"
"};\n"
"Object.defineProperties(RegProto, {\n"
"  scope:             { get: function () { return this._scope || ''; }, enumerable: true, configurable: true },\n"
"  installing:        { get: function () { return null; }, enumerable: true, configurable: true },\n"
"  waiting:           { get: function () { return null; }, enumerable: true, configurable: true },\n"
"  active:            { get: function () { return null; }, enumerable: true, configurable: true },\n"
"  navigationPreload: { get: function () { return undefined; }, enumerable: true, configurable: true },\n"
"  pushManager:       { get: function () { return undefined; }, enumerable: true, configurable: true }\n"
"});\n"
"\n"
/* ==== the trustworthy-origin check register() gates real execution risk
 * on, in place of the decorative G.isSecureContext. RFC-ish loopback set,
 * not a hostname special-case for any real site. ==================== */
"function isLoopbackHost(h) {\n"
"  if (h === 'localhost' || (/\\.localhost$/).test(h)) return true;\n"
"  if (h === '127.0.0.1' || (/^127\\.\\d+\\.\\d+\\.\\d+$/).test(h)) return true;\n"
"  if (h === '[::1]' || h === '::1') return true;\n"
"  return false;\n"
"}\n"
"function isTrustworthyOrigin(u) {\n"
"  if (u.protocol === 'https:' || u.protocol === 'wss:') return true;\n"
"  return isLoopbackHost(u.hostname);\n"
"}\n"
"\n"
/* ==== ServiceWorkerContainer -- the one live instance, navigator.serviceWorker ==== */
"var container = new G.EventTarget();\n"
"var oncc = null;\n"
"Object.defineProperty(container, 'oncontrollerchange', {\n"
"  get: function () { return oncc; },\n"
   /* NEVER FIRES -- correct, not a gap: the controller never changes,
      because it is always null. */
"  set: function (v) { oncc = (typeof v === 'function') ? v : null; },\n"
"  enumerable: true, configurable: true\n"
"});\n"
"var onmsg = null;\n"
"Object.defineProperty(container, 'onmessage', {\n"
"  get: function () { return onmsg; },\n"
"  set: function (v) { onmsg = (typeof v === 'function') ? v : null; },\n"
"  enumerable: true, configurable: true\n"
"});\n"
"Object.defineProperty(container, 'controller', {\n"
"  get: function () { return null; }, enumerable: true, configurable: true\n"
"});\n"
"\n"
   /* register(): every check the spec runs BEFORE starting a job, run for
      real; the promise ALWAYS settles because every branch below either
      rejects explicitly or falls through to the final, unconditional
      rejection -- there is no branch that falls off the end silently, and
      the whole body is one more try/catch outside all of it in case a step
      itself throws (a Symbol argument, an unexpected URL edge). */
"container.register = function (scriptURL, options) {\n"
"  return new G.Promise(function (resolve, reject) {\n"
"    void resolve;\n"
"    try {\n"
"      var opts = options || {};\n"
"      var docHref = (G.location && G.location.href) || 'about:blank';\n"
"      var docURL;\n"
"      try { docURL = new G.URL(docHref); }\n"
"      catch (eDoc) { reject(domErr('the document has no valid URL', 'SecurityError')); return; }\n"
"      if (!isTrustworthyOrigin(docURL)) {\n"
"        reject(domErr('service workers may only be registered from a secure context (https, or localhost/127.0.0.1/::1)', 'SecurityError'));\n"
"        return;\n"
"      }\n"
"      var scriptStr = String(scriptURL);\n"
"      var parsedScript;\n"
"      try { parsedScript = new G.URL(scriptStr, docURL.href); }\n"
"      catch (eScript) { reject(domErr('the script URL could not be parsed', 'SyntaxError')); return; }\n"
"      var scopeStr = (opts.scope !== undefined) ? String(opts.scope) : parsedScript.href.replace(/[^\\/]*$/, '');\n"
"      var parsedScope;\n"
"      try { parsedScope = new G.URL(scopeStr, docURL.href); }\n"
"      catch (eScope) { reject(domErr('the scope URL could not be parsed', 'SyntaxError')); return; }\n"
"      if (parsedScript.origin !== docURL.origin) {\n"
"        reject(domErr('the script URL must be same-origin with the document', 'SecurityError')); return;\n"
"      }\n"
"      if (parsedScope.origin !== docURL.origin) {\n"
"        reject(domErr('the scope URL must be same-origin with the document', 'SecurityError')); return;\n"
"      }\n"
"      var scriptDir = parsedScript.href.replace(/[^\\/]*$/, '');\n"
"      if (parsedScope.href.indexOf(scriptDir) !== 0) {\n"
"        reject(domErr(\"the scope must be at or under the script URL's own directory\", 'SecurityError')); return;\n"
"      }\n"
"      if (opts.type === 'module') {\n"
"        reject(domErr('module service workers are not supported', 'NotSupportedError')); return;\n"
"      }\n"
"      reject(domErr(\n"
"        'this build validates and refuses service worker registration -- URL, scope, origin and ' +\n"
"        'secure-context checks above all passed, but this engine does not execute a service worker ' +\n"
"        'script or intercept fetches (see c/apps/browser/js_swreg.c and js_worker.c)',\n"
"        'NotSupportedError'));\n"
"    } catch (eOuter) { reject(eOuter); }\n"
"  });\n"
"};\n"
"container.getRegistration = function (clientURL) {\n"
"  return new G.Promise(function (resolve) { void clientURL; resolve(undefined); });\n"
"};\n"
"container.getRegistrations = function () {\n"
"  return new G.Promise(function (resolve) { resolve([]); });\n"
"};\n"
   /* startMessages(): per spec it only un-buffers messages held for a
      controller that has not yet had a listener attached. There is never a
      controller, so this is a real no-op, not a stub standing in for one. */
"container.startMessages = function () {};\n"
"\n"
"var readyPromise = null;\n"
"Object.defineProperty(container, 'ready', {\n"
"  get: function () {\n"
"    if (!readyPromise) {\n"
"      readyPromise = G.Promise.reject(domErr(\n"
"        'no service worker will ever become active in this build -- see navigator.serviceWorker.register(). ' +\n"
"        'This is a deliberate deviation from the spec, which defines ready as never rejecting: the spec-conformant ' +\n"
"        'behaviour here would be a promise pending forever, and this build refuses to hand back a promise that ' +\n"
"        'never settles',\n"
"        'NotSupportedError'));\n"
"    }\n"
"    return readyPromise;\n"
"  },\n"
"  enumerable: true, configurable: true\n"
"});\n"
"\n"
"function installIface(name, proto) {\n"
"  var C = illegal(name);\n"
"  C.prototype = proto; proto.constructor = C;\n"
"  Object.defineProperty(G, name, { value: C, writable: true, configurable: true });\n"
"  return C;\n"
"}\n"
"installIface('ServiceWorker', ServiceWorkerProto);\n"
"installIface('ServiceWorkerRegistration', RegProto);\n"
"named(container.register, 'register', 1);\n"
"named(container.getRegistration, 'getRegistration', 0);\n"
"named(container.getRegistrations, 'getRegistrations', 0);\n"
"named(container.startMessages, 'startMessages', 0);\n"
"Object.defineProperty(G.navigator, 'serviceWorker', { value: container, writable: true, configurable: true, enumerable: true });\n"
"})(typeof globalThis !== 'undefined' ? globalThis : this);\n";

#ifdef JS_SWREG_PENDING_REGISTER
/* THE CONTROL FOR THE TERMINATION BAR ITSELF: register() returns a REAL
 * Promise (`instanceof Promise` holds) whose executor never calls resolve
 * or reject -- the exact js_platform.h:66-70 shape. tests/cache.mk's
 * (shared with swreg) negctl build links this in; the quiescence check must
 * go RED on exactly the register()-shaped checks and pass on everything
 * that does not call register(). */
static const char *SWREG_NEGCTL_JS =
"(function (G) {\n"
"  if (!G.navigator || !G.navigator.serviceWorker) return;\n"
"  G.navigator.serviceWorker.register = function () {\n"
"    return new G.Promise(function (resolve, reject) { void resolve; void reject; });\n"
"  };\n"
"})(typeof globalThis !== 'undefined' ? globalThis : this);\n";
#endif

void js_swreg_install(JSContext *ctx)
{
    if (!ctx) return;
    JSValue r = JS_Eval(ctx, SWREG_JS, strlen(SWREG_JS), "<js_swreg>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, e);
        fprintf(stderr, "js_swreg: install failed: %s\n", s ? s : "(unprintable)");
        if (s) JS_FreeCString(ctx, s);
        JSValue st = JS_GetPropertyStr(ctx, e, "stack");
        if (!JS_IsUndefined(st)) {
            const char *ss = JS_ToCString(ctx, st);
            if (ss) { fprintf(stderr, "%s\n", ss); JS_FreeCString(ctx, ss); }
        }
        JS_FreeValue(ctx, st);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);

#ifdef JS_SWREG_PENDING_REGISTER
    {
        JSValue n = JS_Eval(ctx, SWREG_NEGCTL_JS, strlen(SWREG_NEGCTL_JS),
                            "<js_swreg_negctl>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(n)) {
            JSValue e = JS_GetException(ctx);
            const char *m = JS_ToCString(ctx, e);
            fprintf(stderr, "js_swreg NEGCTL: stub did not install: %s\n", m ? m : "(unprintable)");
            fprintf(stderr, "js_swreg NEGCTL: this build is identical to the real one -- its verdict is meaningless\n");
            if (m) JS_FreeCString(ctx, m);
            JS_FreeValue(ctx, e);
            exit(2);
        }
        JS_FreeValue(ctx, n);
    }
#endif
}
