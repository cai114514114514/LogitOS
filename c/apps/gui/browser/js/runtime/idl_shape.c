/* The WebIDL shape pass: WHERE the platform's members live.
 *
 * WHY THIS EXISTS. Cloudflare's challenge, and every anti-tamper script like
 * it, does not ask "does navigator.userAgent work" -- it asks where the member
 * lives and what the object model around it looks like:
 *
 *     Object.getOwnPropertyDescriptor(navigator, 'webdriver')   // own = a stealth override
 *     Object.prototype.toString.call(navigator)                 // '[object Navigator]'
 *     Object.getOwnPropertyDescriptor(Navigator.prototype, 'userAgent').get.call({})
 *                                                               // must THROW
 *     Object.getPrototypeOf(Performance.prototype) === EventTarget.prototype
 *
 * The installers this browser grew one feature at a time put their members on
 * the instances -- `navigator` owned 20 names, `document` 63, `document.body`
 * 22, `history` 8, `performance` 14, `screen` 4, `crypto` 3, where a browser's
 * own none but document's `location` -- and 49 interface prototypes answered
 * '[object Object]'. Measured, not supposed: tests/fixtures/webidl_shape/
 * shape.js run here and in Chrome 153, diffed by tests/qmp/webidl_shape_diff.py.
 *
 * ONE PASS, AT THE END, rather than edits to a dozen installers. Each installer
 * is right about WHAT its member does; this pass is right about WHERE it goes,
 * and doing it once, after all of them, means none of their `k in obj` guards,
 * install orders or negative controls change. The cost of that choice is that
 * the pass has to recover each member's receiver semantics from outside, and
 * the table in idl_shape_prelude.inc is where that is written down.
 *
 * WHAT IT DOES, in the order the prelude does it:
 *   1. navigator / history / performance / screen / crypto: every known own
 *      member onto the interface prototype -- operations as enumerable methods
 *      with WebIDL lengths, attributes as getters (a read-write one keeps a
 *      validating setter over per-realm storage), each brand-checked against
 *      the realm's one instance. Screen and Crypto had no interface object at
 *      all; they get one here (a native illegal constructor, mkIface below).
 *   2. document: its 62 members onto Document.prototype (`onunload`, which is
 *      not a Document member, is dropped; `location` stays own). Document.
 *      prototype is SHARED with createHTMLDocument documents, so each member
 *      dispatches on its receiver: the page document gets exactly what it had,
 *      another Document its own answer or the spec's answer for a document
 *      without a browsing context, anything else "Illegal invocation".
 *   3. document.body: the 22 Window-reflecting handlers onto
 *      HTMLBodyElement.prototype and HTMLFrameSetElement.prototype.
 *   4. chains: Performance, Screen, AbortSignal, Worker, EventSource and
 *      FontFaceSet onto EventTarget.prototype, and window onto Window.prototype
 *      > WindowProperties > EventTarget.prototype -- each only where what the
 *      instance answers today cannot change (see the prelude).
 *   5. @@toStringTag on every platform interface prototype lacking one, and
 *      interface objects made non-enumerable on window.
 *
 * WHAT IT DOES NOT, named so the next reader does not rediscover it:
 *   - Function.prototype.toString / member .name beyond what the pass itself
 *     builds: that is its own pass (it runs after this one in js_page_open).
 *   - NodeList / HTMLCollection over Array.prototype (select.c's snapshot
 *     collections ARE arrays and its own code calls forEach/slice on them).
 *   - HTMLDocument as its own prototype: select.c compares
 *     Object.getPrototypeOf(scope) with Object.getPrototypeOf(doc) at QUERY
 *     time to pick its native fast path, so re-parenting document would move
 *     every createHTMLDocument query onto the script matcher.
 *   - XMLHttpRequest, MessagePort, BroadcastChannel stay off EventTarget: they
 *     own add/removeEventListener but no dispatchEvent, and inheriting one
 *     that never reaches their listeners is worse than not having it.
 *   - Members that do not exist in this build stay absent. Nothing is added
 *     that the platform does not implement, and nothing Chrome-only is
 *     invented: the pass moves, it does not fabricate.
 *
 * The gate is tests/idl_shape.mk (host, the browser's own install order), and
 * its control compiles this file with -DIDL_SHAPE_NEGCTL_SKIP. */
#include <stdio.h>
#include <string.h>
#include "quickjs.h"
#include "../../../../../../include/weaksym.h"
extern JSValue js_dom_event_types(JSContext *) LOGIT_WEAK;
LOGIT_WEAK_STUB(js_dom_event_types);
#include "js/runtime/idl_shape.h"

/* Relative to this file, like every other prelude in js/: that is how
 * tools/browser_wiring_audit.py (test-browser-wiring) resolves a textual
 * include, and a browser-root-qualified spelling sent it to a path that does
 * not exist. */
static const char IDL_SHAPE_PRELUDE[] =
#include "idl_shape_prelude.inc"
    ;

/* Interface objects are native functions in a browser, and "Illegal
 * constructor" is what calling or constructing one says. */
static JSValue shape_illegal_ctor(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

/* mkIface(name, proto): the prelude's one C primitive. JS_SetConstructor gives
 * ctor.prototype the WebIDL flags (none) and proto.constructor writable +
 * configurable, the same as webapi.c's singleton loop does for Navigator. */
static JSValue shape_mk_iface(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 2 || !JS_IsObject(argv[1])) return JS_ThrowTypeError(ctx, "mkIface(name, proto)");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    JSValue ctor = JS_NewCFunction2(ctx, shape_illegal_ctor, name, 0, JS_CFUNC_constructor_or_func, 0);
    JS_FreeCString(ctx, name);
    if (JS_IsException(ctor)) return ctor;
    JS_SetConstructor(ctx, ctor, argv[1]);
    return ctor;
}

static void shape_report(JSContext *ctx, const char *what)
{
    JSValue e = JS_GetException(ctx);
    const char *m = JS_ToCString(ctx, e);
    printf("[idl-shape] %s: %s\n", what, m ? m : "?");
    if (m) JS_FreeCString(ctx, m);
    JS_FreeValue(ctx, e);
}

JSValue js_idl_shape_install_roots(JSContext *ctx)
{
#ifdef IDL_SHAPE_NEGCTL_SKIP
    /* The negative control: the same link, the same page, no pass. */
    (void)ctx;
    (void)shape_mk_iface;
    (void)shape_report;
    (void)IDL_SHAPE_PRELUDE;
    return JS_UNDEFINED;
#else
    if (!ctx) return JS_UNDEFINED;
    JSValue fn = JS_Eval(ctx, IDL_SHAPE_PRELUDE, sizeof IDL_SHAPE_PRELUDE - 1,
                         "<idl-shape>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fn)) { shape_report(ctx, "prelude did not compile"); return JS_UNDEFINED; }
    JSValue args[3];
    args[0] = JS_GetGlobalObject(ctx);
    args[1] = JS_NewCFunction(ctx, shape_mk_iface, "mkIface", 2);
    args[2] = LOGIT_HAVE(js_dom_event_types)?js_dom_event_types(ctx):JS_NewArray(ctx);
    JSValue r = JS_IsException(args[2])?JS_EXCEPTION:
        JS_Call(ctx, fn, JS_UNDEFINED, 3, (JSValueConst *)args);
    /* A throw part-way leaves what was already moved moved: every step is
     * complete for the member it touches, so a half-run pass is a page with
     * fewer members relocated, never one with a member lost. */
    if (JS_IsException(r)) {
        shape_report(ctx, "pass threw");
        r = JS_UNDEFINED;
    }
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, args[2]);
    JS_FreeValue(ctx, fn);
    return r;
#endif
}

void js_idl_shape_install(JSContext *ctx)
{
    if (ctx) JS_FreeValue(ctx, js_idl_shape_install_roots(ctx));
}
