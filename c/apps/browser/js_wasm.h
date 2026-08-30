#ifndef LOGIT_JS_WASM_H
#define LOGIT_JS_WASM_H

#include "quickjs.h"

/* The WebAssembly JavaScript API over c/lib/wasm's MVP interpreter:
 * WebAssembly.{Module,Instance,Memory,Table,Global}, .compile/.instantiate/
 * .validate, their *Streaming twins, and CompileError/LinkError/RuntimeError.
 *
 * Install LATE, and the ordering requirement is real rather than a courtesy:
 * `instantiateStreaming` takes a Response (or a promise of one) and reads it
 * with `.arrayBuffer()`, so it needs the real G.Response js_webapi.c installs.
 * It degrades correctly if that is missing -- the streaming entry points are
 * simply not defined -- rather than installing a version that cannot work.
 *
 * Weak under JS_WASM_OPTIONAL, the same convention as js_idb.h / js_worker.h /
 * js_cache.h: a build that does not link this TU (the WPT runner's stock
 * source list, the host gates of other files) still links and simply has
 * `typeof WebAssembly === 'undefined'`, which is the correct feature-detect
 * answer for a browser that does not have it.  That matters more here than
 * anywhere else in this directory: a page tests for the constructor and then
 * TRUSTS what it gets, so a half-installed WebAssembly is worse than none. */
#include "../../../include/weaksym.h"   /* LOGIT_WEAK/_STUB: see the header */
#ifdef JS_WASM_OPTIONAL
#  define WASMJS_FN LOGIT_WEAK
#else
#  define WASMJS_FN
#endif

WASMJS_FN void js_wasm_install(JSContext *ctx);

/* Drop every module, instance, memory and table this context built.  Call it
 * at page teardown BEFORE JS_FreeContext, not after: it releases malloc'd
 * arenas the JS heap cannot see AND the JSValues it is holding (each memory's
 * cached ArrayBuffer, each imported function).  Called after the context is
 * gone there is no way to release the second kind, and QuickJS asserts on the
 * leak at JS_FreeRuntime. */
WASMJS_FN void js_wasm_reset(JSContext *ctx);

#ifdef JS_WASM_OPTIONAL
LOGIT_WEAK_STUB(js_wasm_install);
LOGIT_WEAK_STUB(js_wasm_reset);
#endif

#endif /* LOGIT_JS_WASM_H */
