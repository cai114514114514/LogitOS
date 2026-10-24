/* native_shape_test.c -- the nativize pass (c/apps/gui/browser/js/runtime/
 * native_shape.c) through the whole page: js_page_open with the full
 * platform, then the page's own view of it.
 *
 * WHAT THE PAGE MUST SEE. Every platform function a page can reach -- an
 * operation on an interface prototype, an accessor, a global function like
 * fetch, an interface object, a closure the platform makes after load --
 * stringifies as "function NAME() { [native code] }"; operations and
 * accessors have no `prototype` and are not constructors; their names are
 * WebIDL's, not '' or 'value'. And a function the PAGE writes is untouched:
 * its source, its prototype, its constructibility.
 *
 * FIVE KINDS OF CHECK, tagged, because the negative control depends on it:
 *   [native]  the pass's effect. -DNATIVE_SHAPE_NEGCTL_SKIP compiles the
 *             pass to a no-op; every check of that effect must fail and
 *             NOTHING else may -- tests/native_shape.mk counts.
 *   [idl]     the earlier IDL pass's computed methods/getters, whose names,
 *             lack of prototype and stripped source already have the right
 *             shape without the native pass: true in both builds.
 *   [engine]  QuickJS's own spelling of C built-ins: true in both builds.
 *   [page]    a page function is untouched: true in both builds -- including
 *             the <body onload> the PLATFORM compiles at install, before the
 *             pass (the eval_code rule in quickjs.c; watched failing with
 *             -DJS_NATIVE_NEGCTL_MARK_EVAL, which test-native-fn-negctl-eval
 *             keeps as a standing control at the engine level).
 *   [works]   the platform still works after the pass (it deletes
 *             `prototype`s and removes [[Construct]] from hundreds of
 *             functions its own preludes wrote): true in both builds.
 *
 * Original claim: EVERY check then tagged [native] must fail without the
 * pass. Correction (2026-09-30): the real skip build ran 48 checks but only
 * 25 of 27 [native] failed, with no other failure. readyState and sendBeacon
 * were the two survivors: idl_shape_prelude.inc rebuilds them before this
 * pass with computed getter/method syntax and 'use strip'. They remain real
 * checks under [idl]; making their already-correct shape fail would test a
 * different producer rather than the skip control this gate names.
 *
 * THREE READINGS OF "every platform function" besides the C walk's own. A
 * census written here in JS, sharing no code with the C walk, reaches
 * everything a page reaches by reflection (and, in its second form, by
 * calling the getters of the singletons a page touches first -- which hands
 * it closures made AFTER the pass). An instance census constructs every
 * interface that will construct and calls a list of factories, so what it
 * inspects was made after the pass too: that is the closure-time half
 * (JS_NATIVE_CODE_OPERATIONS), which no walk can reach. And the committed
 * fixture tests/fixtures/webidl_shape/shape.js -- the same dump that runs
 * unchanged in Chrome -- is evaluated as page code, and its `native` probes
 * and every interface's `fnsrc` are compared with chrome-153.json.
 *
 * NATIVE_SHAPE_DUMP=file writes shape.js's whole dump (for
 * tests/qmp/webidl_shape_diff.py); NATIVE_SHAPE_EVAL=file.js runs one more
 * page script at the end and prints its value. Diagnostics, not checks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "quickjs.h"
#include "dom/tree.h"
#include "js/dom/dom.h"
#include "js/runtime/page.h"
#include "js/runtime/native_shape.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
/* js_module.c's resource path; no fixture here is an image or a module. */
int res_fetch(const char *ref, unsigned char **out, int *len)
{ (void)ref; (void)out; (void)len; return -1; }

/* tests/unit/loader_fakebfetch.c: bfetch over an in-memory site. */
void fake_site_reset(void);
void fake_site_add(const char *url, const char *body);

static JSContext *ctx;
static int checks, fails;
static int n_native, f_native;   /* [native] checks and their failures */
static int f_other;              /* failures of every other tag */

static JSValue eval_page(const char *src, const char *file)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), file, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("      [js exception] %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    }
    return v;
}

static void ck(const char *tag, const char *what, const char *expr)
{
    char buf[16384];
    snprintf(buf, sizeof buf, "(function(){ try { return (%s); } catch (e) { return 'threw: ' + e; } })()", expr);
    checks++;
    int is_native = !strcmp(tag, "native");
    if (is_native) n_native++;
    JSValue v = eval_page(buf, "http://fixture.test/check.js");
    int ok = !JS_IsException(v) && !JS_IsString(v) && JS_ToBool(ctx, v);
    if (!ok) {
        fails++;
        if (is_native) f_native++; else f_other++;
        const char *s = JS_IsException(v) ? NULL : JS_ToCString(ctx, v);
        printf("FAIL [%s] %s\n     value: %s\n", tag, what, s ? s : "(exception)");
        if (s) JS_FreeCString(ctx, s);
    } else {
        printf("ok   [%s] %s\n", tag, what);
    }
    JS_FreeValue(ctx, v);
}

static void run(const char *src)
{
    JS_FreeValue(ctx, eval_page(src, "http://fixture.test/run.js"));
    js_page_pump();
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    if (b) b[n] = 0;
    fclose(f);
    return b;
}

static unsigned long long g_now;
static unsigned long long clock_fn(void) { return g_now; }
static void pump_until_idle(int max)
{
    for (int i = 0; i < max && js_page_pending(); i++) { g_now += 1; js_page_run_due(); js_page_pump(); }
}

/* The global names a bare QuickJS context has, handed to the census as
 * `__nsIntrinsic`: the census's own answer to "which globals are ES". */
static void publish_intrinsic_names(void)
{
    JSContext *bare = JS_NewContext(JS_GetRuntime(ctx));
    JSValue bg = JS_GetGlobalObject(bare);
    JSValue arr = JS_NewArray(ctx);
    JSPropertyEnum *tab;
    uint32_t n = 0;
    if (JS_GetOwnPropertyNames(bare, &tab, &n, bg, JS_GPN_STRING_MASK) == 0) {
        for (uint32_t i = 0; i < n; i++) {
            const char *s = JS_AtomToCString(bare, tab[i].atom);
            if (s) { JS_SetPropertyUint32(ctx, arr, i, JS_NewString(ctx, s)); JS_FreeCString(bare, s); }
            JS_FreeAtom(bare, tab[i].atom);
        }
        js_free(bare, tab);
    }
    /* The ES intrinsic %ThrowTypeError%'s name in a realm nobody marked, for
     * the check that the pass left QuickJS's own functions alone. */
    JSValue tte = JS_Eval(bare, "Object.getOwnPropertyDescriptor(Function.prototype, 'caller').get.name",
                          strlen("Object.getOwnPropertyDescriptor(Function.prototype, 'caller').get.name"),
                          "<bare>", JS_EVAL_TYPE_GLOBAL);
    const char *ttes = JS_ToCString(bare, tte);
    JSValue g = JS_GetGlobalObject(ctx);
    /* The platform's own globals, before any page code defines one. */
    JSValue plat = JS_NewArray(ctx);
    JSPropertyEnum *gt;
    uint32_t gn = 0;
    if (JS_GetOwnPropertyNames(ctx, &gt, &gn, g, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) == 0) {
        for (uint32_t i = 0; i < gn; i++) {
            JS_SetPropertyUint32(ctx, plat, i, JS_AtomToValue(ctx, gt[i].atom));
            JS_FreeAtom(ctx, gt[i].atom);
        }
        js_free(ctx, gt);
    }
    JS_SetPropertyStr(ctx, g, "__nsPlatformGlobals", plat);
    JS_SetPropertyStr(ctx, g, "__nsIntrinsic", arr);
    JS_SetPropertyStr(ctx, g, "__nsBareCallerGetterName", JS_NewString(ctx, ttes ? ttes : "?"));
    JS_FreeValue(ctx, g);
    if (ttes) JS_FreeCString(bare, ttes);
    JS_FreeValue(bare, tte);
    JS_FreeValue(bare, bg);
    JS_FreeContext(bare);
}

/* The census, as PAGE code. walk(false): reflection only. walk(true): also
 * the value of every getter on the first objects a page touches (window,
 * document, navigator, performance, screen, history, location, crypto),
 * called on that object. Counts, over every function reached:
 *   src      Function.prototype.toString is not the NativeFunction form
 *   proto    an operation/accessor that owns `prototype`
 *   ctor     an operation/accessor that is a constructor (probed with
 *            Reflect.construct(String, [], f): IsConstructor, f never runs)
 *   name     an operation named '' / 'value' / 'get' / 'set' under any other
 *            key (Headers.prototype.get IS named 'get'), or an accessor named
 *            one of those or its bare KEY instead of 'get KEY' / 'set KEY'
 * "Operation" is decided here without the C pass's help: an accessor
 * function, or a function-valued property under a lowercase key that is not
 * also some prototype's `constructor` or any uppercase key's value. */
static const char *CENSUS_JS =
"function __nsCensus(followGetters) {\n"
"  var I = new Set(__nsIntrinsic), seen = new Set(), q = [], info = new Map();\n"
"  var fts = Function.prototype.toString, gopd = Object.getOwnPropertyDescriptor;\n"
"  var NATIVE = /^function [^(]*\\(\\) \\{ \\[native code\\] \\}$/;\n"
"  function isObj(o) { return o !== null && (typeof o === 'object' || typeof o === 'function'); }\n"
"  function push(o, d, intr, label) { if (isObj(o) && !seen.has(o) && d <= 6) { seen.add(o); q.push([o, d, intr, label]); } }\n"
"  function keyName(k) { return typeof k === 'symbol' ? '[' + (k.description || '') + ']' : k; }\n"
"  function note(f, k, kind, where, intr, link) {\n"
"    var r = info.get(f);\n"
"    if (!r) { r = { key: keyName(k), kind: kind, where: where, ctor: false, intr: intr }; info.set(f, r); }\n"
"    if (link || (typeof k === 'string' && k !== 'constructor' && /^[A-Z]/.test(k))) r.ctor = true;\n"
"    if (!intr) r.intr = false;\n"
"  }\n"
     /* `constructor` is evidence only on an interface prototype: the object
        an ordinary function's `prototype` auto-creates is {constructor: f}
        over Object.prototype, and it exists for any function whose
        `prototype` something has read. */
"  function ifaceProto(o, keys) { return keys.length > 1 || Object.getPrototypeOf(o) !== Object.prototype; }\n"
"  function isIndex(k) { return typeof k === 'string' && /^(0|[1-9][0-9]*)$/.test(k); }\n"
"  function visit(o, d, intr, label) {\n"
"    var keys; try { keys = Reflect.ownKeys(o); } catch (e) { return; }\n"
     /* An Array's elements are data; its NAMED members (plugins.item) are
        surface. */
"    var arr = Array.isArray(o);\n"
"    for (var i = 0; i < keys.length; i++) {\n"
"      var k = keys[i], ds; if (arr && (isIndex(k) || k === 'length')) continue;\n"
"      try { ds = gopd(o, k); } catch (e) { continue; }\n"
"      if (!ds) continue;\n"
"      var where = label + '.' + keyName(k);\n"
"      if ('value' in ds) {\n"
"        if (typeof ds.value === 'function') { note(ds.value, k, 'm', where, intr, k === 'constructor' && ifaceProto(o, keys)); push(ds.value, d + 1, intr, where); }\n"
"        else if (isObj(ds.value)) push(ds.value, d + 1, intr, where);\n"
"      } else {\n"
"        if (ds.get) note(ds.get, k, 'g', where, intr);\n"
"        if (ds.set) note(ds.set, k, 's', where, intr);\n"
"      }\n"
"    }\n"
"    try { push(Object.getPrototypeOf(o), d, intr, label + '.__proto__'); } catch (e) {}\n"
"  }\n"
"  function drain() { while (q.length) { var it = q.shift(); visit(it[0], it[1], it[2], it[3] || '?'); } }\n"
"  var G = globalThis;\n"
     /* The global first: `globalThis` is an intrinsic NAME whose value is
        the global, and walked as intrinsic it would claim every platform
        object for the intrinsic side. */
"  seen.add(G);\n"
"  Object.getOwnPropertyNames(G).forEach(function (k) { if (I.has(k)) { var d = gopd(G, k); if (d && 'value' in d) push(d.value, 0, true, k); } });\n"
"  drain();\n"
     /* Only the globals the PLATFORM defined (snapshotted before any page
        code ran): the page's own globals are the page's business. */
"  __nsPlatformGlobals.forEach(function (k) {\n"
"    if (typeof k === 'string' && I.has(k)) return;\n"
"    var d = gopd(G, k); if (!d) return;\n"
"    if ('value' in d) { if (typeof d.value === 'function') note(d.value, k, 'm', 'window.' + keyName(k), false); push(d.value, 0, false, keyName(k)); }\n"
"    else { if (d.get) note(d.get, k, 'g', 'window.' + keyName(k), false); if (d.set) note(d.set, k, 's', 'window.' + keyName(k), false); }\n"
"  });\n"
"  push(Object.getPrototypeOf(G), 0, false, 'window.__proto__');\n"
"  if (followGetters) {\n"
"    ['document','navigator','performance','screen','history','location','crypto'].forEach(function (n) { push(G[n], 1, false, n); });\n"
"    [G, G.document, G.navigator, G.performance, G.screen, G.history, G.location, G.crypto].forEach(function (s) {\n"
"      if (!isObj(s)) return;\n"
"      for (var p = s; p && p !== Object.prototype; p = Object.getPrototypeOf(p)) {\n"
"        Reflect.ownKeys(p).forEach(function (k) {\n"
"          var d = gopd(p, k); if (!d || !d.get) return;\n"
          /* An event handler IDL attribute hands back the PAGE's function
             (here, the <body onload> the page wrote), not a platform one. */
"          if (typeof k === 'string' && /^on/.test(k)) return;\n"
"          var v; try { v = d.get.call(s); } catch (e) { return; }\n"
"          if (typeof v === 'function') note(v, k, 'm', 'got.' + keyName(k), false);\n"
"          if (isObj(v)) push(v, 1, false, 'got.' + keyName(k));\n"
"        });\n"
"      }\n"
"    });\n"
"  }\n"
"  drain();\n"
"  var out = { fns: 0, src: [], proto: [], ctor: [], name: [] };\n"
"  info.forEach(function (r, f) {\n"
"    if (r.intr) return;\n"
"    out.fns++;\n"
"    var s; try { s = fts.call(f); } catch (e) { s = '!'; }\n"
"    if (!NATIVE.test(s)) out.src.push(r.where);\n"
"    if (r.ctor) return;\n"
"    if (Object.prototype.hasOwnProperty.call(f, 'prototype')) out.proto.push(r.where);\n"
"    var c = true; try { Reflect.construct(String, [], f); } catch (e) { c = false; }\n"
"    if (c) out.ctor.push(r.where);\n"
"    var nm = f.name, want = r.kind === 'g' ? 'get ' + r.key : r.kind === 's' ? 'set ' + r.key : null;\n"
"    if (want ? (nm !== want && (nm === r.key || nm === '' || nm === 'get' || nm === 'set' || nm === 'value'))\n"
"             : (nm !== r.key && (nm === '' || nm === 'value' || nm === 'get' || nm === 'set'))) out.name.push(r.where + '=' + JSON.stringify(nm));\n"
"  });\n"
"  return out;\n"
"}\n";

static void census_report(const char *label, const char *expr_json)
{
    JSValue v = eval_page(expr_json, "http://fixture.test/census.js");
    const char *s = JS_ToCString(ctx, v);
    printf("census %s: %s\n", label, s ? s : "(threw)");
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
}

/* The page carries two inline handlers because the platform compiles page
 * CONTENT through two doors: the <body onload> attribute is compiled AT
 * INSTALL by the cssom shim's `new Function('event', src)` -- before the
 * pass, in the same realm -- and the <span onclick> one at dispatch, by
 * js/dom/dom.c, after it. Both are the page's code. */
static const char *PAGE =
    "<!doctype html><html><head><title>t</title></head>"
    "<body onload=\"window.__fromAttr = function fromAttr() { return 1; }\">"
    "<p id=status>shape</p><div id=d class='x y'>hi <b>there</b></div>"
    "<span id=clk onclick=\"window.__fromClick = function fromClick() { return 2; }\">c</span>"
    "<canvas id=c width=4 height=4></canvas></body></html>";

/* The census of what the platform makes AFTER the pass: construct every
 * global interface that will construct with no argument (or one), call a
 * list of factories, and inspect what comes back -- own members and their
 * prototype chains, to a small depth. The functions the census itself
 * hands in (observer callbacks) are the page's and are skipped. */
static const char *INSTANCES_JS =
"function __nsInstances() {\n"
"  var fts = Function.prototype.toString, NATIVE = /^function [^(]*\\(\\) \\{ \\[native code\\] \\}$/;\n"
"  var I = new Set(__nsIntrinsic), mine = new Set(), seen = new Set();\n"
"  var out = { made: 0, fns: 0, src: [], proto: [], ctor: [] };\n"
"  function cb() {} mine.add(cb);\n"
"  function isObj(o) { return o !== null && (typeof o === 'object' || typeof o === 'function'); }\n"
"  function isIndex(k) { return typeof k === 'string' && /^(0|[1-9][0-9]*)$/.test(k); }\n"
"  function fn(f, where, k) {\n"
"    if (mine.has(f)) return;\n"
"    out.fns++;\n"
"    var s; try { s = fts.call(f); } catch (e) { s = '!'; }\n"
"    if (!NATIVE.test(s)) { out.src.push(where); return; }\n"
"    if (typeof k === 'string' && (k === 'constructor' || /^[A-Z]/.test(k))) return;\n"
"    if (Object.prototype.hasOwnProperty.call(f, 'prototype')) out.proto.push(where);\n"
"    var c = true; try { Reflect.construct(String, [], f); } catch (e) { c = false; }\n"
"    if (c) out.ctor.push(where);\n"
"  }\n"
"  function inspect(label, o, depth) {\n"
"    if (!isObj(o) || seen.has(o) || depth > 3 || o === Object.prototype || o === Function.prototype) return;\n"
"    seen.add(o);\n"
"    var keys; try { keys = Reflect.ownKeys(o); } catch (e) { return; }\n"
"    keys.forEach(function (k) {\n"
"      if (isIndex(k)) return;\n"
"      var d; try { d = Object.getOwnPropertyDescriptor(o, k); } catch (e) { return; }\n"
"      if (!d) return;\n"
"      var w = label + '.' + String(k);\n"
"      if ('value' in d) {\n"
"        if (typeof d.value === 'function') fn(d.value, w, k);\n"
"        else if (isObj(d.value)) inspect(w, d.value, depth + 1);\n"
"      } else { if (d.get) fn(d.get, w + '(get)', null); if (d.set) fn(d.set, w + '(set)', null); }\n"
"    });\n"
"    try { inspect(label + '.__proto__', Object.getPrototypeOf(o), depth + 1); } catch (e) {}\n"
"  }\n"
"  function keep(label, v) { if (isObj(v)) { out.made++; inspect(label, v, 0); } }\n"
"  Object.getOwnPropertyNames(globalThis).forEach(function (k) {\n"
"    if (!/^[A-Z]/.test(k) || I.has(k)) return;\n"
"    var C; try { C = globalThis[k]; } catch (e) { return; }\n"
"    if (typeof C !== 'function') return;\n"
"    try { keep('new ' + k, new C()); } catch (e) { try { keep('new ' + k, new C('x')); } catch (e2) {} }\n"
"  });\n"
"  var probes = [\n"
"    ['getElementsByTagName', function () { return document.getElementsByTagName('p'); }],\n"
"    ['querySelectorAll', function () { return document.querySelectorAll('p'); }],\n"
"    ['childNodes', function () { return document.body.childNodes; }],\n"
"    ['attributes', function () { return document.body.attributes; }],\n"
"    ['classList.entries', function () { return document.body.classList.entries(); }],\n"
"    ['matchMedia', function () { return matchMedia('(min-width: 1px)'); }],\n"
"    ['getComputedStyle', function () { return getComputedStyle(document.body); }],\n"
"    ['getEntries', function () { performance.mark('inst'); return performance.getEntries(); }],\n"
"    ['fetch', function () { return fetch('data:,x'); }],\n"
"    ['response.body', function () { return new Response('x').body; }],\n"
"    ['reader', function () { return new ReadableStream().getReader(); }],\n"
"    ['writer', function () { return new WritableStream().getWriter(); }],\n"
"    ['abort.signal', function () { return new AbortController().signal; }],\n"
"    ['observers', function () { return [new IntersectionObserver(cb), new ResizeObserver(cb), new MutationObserver(cb)]; }],\n"
"    ['canvas2d', function () { return document.getElementById('c').getContext('2d'); }],\n"
"    ['treeWalker', function () { return document.createTreeWalker(document.body); }],\n"
"    ['htmlDocument', function () { return document.implementation.createHTMLDocument('x'); }],\n"
"    ['domParser', function () { return new DOMParser().parseFromString('<p>a</p>', 'text/html'); }],\n"
"    ['animate', function () { return document.body.animate([{ opacity: 0 }, { opacity: 1 }], 100); }],\n"
"    ['intl', function () { return new Intl.DateTimeFormat(); }],\n"
"    ['shadowRoot', function () { return document.createElement('div').attachShadow({ mode: 'open' }); }],\n"
"    ['url.searchParams', function () { return new URL('http://a/b?c=1').searchParams; }]\n"
"  ];\n"
"  probes.forEach(function (p) { var v; try { v = p[1](); } catch (e) { return; }\n"
"    keep(p[0], v);\n"
"    if (Array.isArray(v)) v.forEach(function (x, i) { keep(p[0] + '[' + i + ']', x); }); });\n"
"  ['a','div','input','select','form','canvas','img','video','iframe','template','dialog','button','table','style','svg','body']\n"
"    .forEach(function (t) { var e = document.createElement(t); keep('<' + t + '>', e); keep('<' + t + '>.style', e.style); });\n"
"  return out;\n"
"}\n";

static const char *WORKER_JS =
    "postMessage(JSON.stringify({"
    "  ael: Function.prototype.toString.call(self.addEventListener),"
    "  aelName: self.addEventListener.name,"
    "  aelProto: 'prototype' in self.addEventListener,"
    "  st: String(setTimeout),"
    "  mine: String(function mine() { return 1; })"
    "}));"
    "onmessage = function (e) { postMessage('pd:' + String(e.preventDefault) + '|' + ('prototype' in e.preventDefault)); };";

int main(int argc, char **argv)
{
    const char *fixtures = argc > 1 ? argv[1] : "tests/fixtures/webidl_shape";
    char path[512];

    fake_site_reset();
    fake_site_add("http://fixture.test/w.js", WORKER_JS);

    struct node *root = dom_parse(PAGE, (int)strlen(PAGE));
    if (!root) { printf("FAIL: the fixture page did not parse\n"); return 1; }
    js_page_set_clock(clock_fn);
    js_page_set_location("http://fixture.test/page.html");
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (!js_page_open(root)) { printf("FAIL: js_page_open\n"); return 1; }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    /* Informational (host wall clock, native code): compare with the
     * negative control's line to see what the pass costs. */
    printf("census js_page_open: %.2f ms\n",
           (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    ctx = js_page_ctx();
    js_page_eval("void 0;", 7, "<warmup>", 0);
    publish_intrinsic_names();

    /* Page code, compiled after js_page_open like every script a page runs. */
    run("function pageThing(a) { return a + 1; }\n"
        "var pageArrow = (x) => x * 2;\n"
        "class PageEl extends HTMLElement { connectedCallback() { this.dataset.up = '1'; } }\n"
        "var pageObj = { method() { return 'm'; }, get acc() { return 1; } };");
    run(CENSUS_JS);
    run(INSTANCES_JS);

    printf("== the named probes ==\n");
    ck("native", "Element.prototype.querySelector (a <select> prelude function) is native code",
       "/^function querySelector\\(\\) \\{ \\[native code\\] \\}$/.test(Function.prototype.toString.call(Element.prototype.querySelector))");
    ck("native", "...has no prototype and is not a constructor",
       "!('prototype' in Element.prototype.querySelector) &&"
       "(function(){ try { new Element.prototype.querySelector('p'); return false; } catch (e) { return e instanceof TypeError; } })()");
    ck("native", "Document.prototype.querySelectorAll: native, no prototype, named",
       "/^function querySelectorAll\\(\\) \\{ \\[native code\\] \\}$/.test(Function.prototype.toString.call(Document.prototype.querySelectorAll)) &&"
       "!('prototype' in Document.prototype.querySelectorAll)");
    /* A <select> prelude function since the qsa family moved onto
     * Document.prototype -- so the PASS's effect, not the engine's. */
    ck("native", "Document.prototype.querySelector matches /\\{ \\[native code\\] \\}$/",
       "/\\{ \\[native code\\] \\}$/.test(Function.prototype.toString.call(Document.prototype.querySelector))");
    ck("native", "EventTarget.prototype.addEventListener: native code, no prototype",
       "String(EventTarget.prototype.addEventListener) === 'function addEventListener() { [native code] }' &&"
       "!('prototype' in EventTarget.prototype.addEventListener)");
    ck("native", "Node.prototype.appendChild and document.createElement: native code, no prototype",
       "/\\{ \\[native code\\] \\}$/.test(String(Node.prototype.appendChild)) && !('prototype' in Node.prototype.appendChild) &&"
       "/\\{ \\[native code\\] \\}$/.test(String(document.createElement)) && !('prototype' in document.createElement)");
    ck("native", "fetch: 'function fetch() { [native code] }', no prototype, not a constructor",
       "fetch.toString() === 'function fetch() { [native code] }' && !('prototype' in fetch) && fetch.name === 'fetch' &&"
       "(function(){ try { new fetch('x'); return false; } catch (e) { return e instanceof TypeError; } })()");
    ck("engine", "setTimeout (a C function) is native code, one line",
       "String(setTimeout) === 'function setTimeout() { [native code] }'");
    ck("engine", "Function.prototype.toString of itself", "String(Function.prototype.toString) === 'function toString() { [native code] }'");
    /* These used to be [native], but the IDL prelude creates their complete
     * observed shape before the native walk. Keep them in both builds so a
     * skip control cannot hide collateral damage to that earlier producer. */
    ck("idl", "document.readyState's IDL getter is 'get readyState', native, no prototype",
       "(function(){ var d = Object.getOwnPropertyDescriptor(document, 'readyState') || Object.getOwnPropertyDescriptor(Document.prototype, 'readyState');"
       "  return d.get.name === 'get readyState' && d.get.toString() === 'function get readyState() { [native code] }' && !('prototype' in d.get); })()");
    ck("idl", "navigator.sendBeacon's IDL method is named, native, without prototype",
       "typeof navigator.sendBeacon === 'function' && navigator.sendBeacon.name === 'sendBeacon' &&"
       "String(navigator.sendBeacon) === 'function sendBeacon() { [native code] }' && !('prototype' in navigator.sendBeacon)");
    ck("native", "a C getter made with a bare name reads 'get KEY': screen.width",
       "(function(){ var d = Object.getOwnPropertyDescriptor(screen, 'width') || Object.getOwnPropertyDescriptor(Object.getPrototypeOf(screen), 'width');"
       "  return !d || !d.get || d.get.name === 'get width'; })() &&"
       "(function(){ var d = Object.getOwnPropertyDescriptor(window, 'onclick'); return !d || !d.get || d.get.name === 'get onclick'; })()");
    ck("native", "an interface written as a JS class keeps [[Construct]] and prototype, and reads native: MutationObserver",
       "String(MutationObserver) === 'function MutationObserver() { [native code] }' &&"
       "typeof MutationObserver.prototype.observe === 'function' && new MutationObserver(function () {}) instanceof MutationObserver");
    ck("native", "...and Event: constructible, native, prototype methods native",
       "String(Event) === 'function Event() { [native code] }' && new Event('x').type === 'x' &&"
       "/\\{ \\[native code\\] \\}$/.test(String(Event.prototype.preventDefault)) && !('prototype' in Event.prototype.preventDefault)");
    /* The engine's doing (a callable Proxy is spelled with its target's
     * name, read through the slot), and true without the pass: [engine]. */
    ck("engine", "HTMLElement -- a Proxy around the native constructor -- still reads as HTMLElement",
       "String(HTMLElement) === 'function HTMLElement() { [native code] }'");
    /* Adaptive, and therefore [works] (true in both builds): Chrome's
     * userAgent is a getter on Navigator.prototype; here it is a data
     * property on navigator until the member-placement pass lands. When a
     * getter carries it, the getter must be WebIDL-named and native. */
    ck("works", "the userAgent: a data property today; if an accessor carries it, 'get userAgent' and native",
       "(function(){ var d = (typeof Navigator === 'function' && Object.getOwnPropertyDescriptor(Navigator.prototype, 'userAgent')) || Object.getOwnPropertyDescriptor(navigator, 'userAgent');"
       "  if (!d.get) return typeof d.value === 'string';"
       "  return d.get.name === 'get userAgent' && /\\{ \\[native code\\] \\}$/.test(String(d.get)); })()");
    ck("native", "a platform function's .fileName no longer names its prelude",
       "Element.prototype.querySelector.fileName === undefined && fetch.fileName === undefined");
    /* Measured: `matches` owned a `prototype` because something had read
     * it before the pass, and the walk then took matches.prototype.
     * constructor -- and a webkit-prefix rule took webkitMatchesSelector --
     * as evidence of an interface. */
    ck("native", "Element.prototype.matches and its webkit alias: an operation, not an interface",
       "!('prototype' in Element.prototype.matches) && Element.prototype.webkitMatchesSelector === Element.prototype.matches &&"
       "(function(){ try { new Element.prototype.matches('p'); return false; } catch (e) { return e instanceof TypeError; } })()");
    ck("native", "a closure the platform makes AFTER the pass is born an operation: a collection's item()",
       "(function(){ var c = document.getElementsByTagName('p'), f = c.item;"
       "  if (typeof f !== 'function') return true;"
       "  var n = true; try { new f(0); } catch (e) { n = false; }"
       "  return !('prototype' in f) && !n && /\\{ \\[native code\\] \\}$/.test(String(f)) && f.call(c, 0) === c[0]; })()");
    ck("native", "an Array's NAMED members are platform surface too: navigator.plugins.item",
       "(function(){ var p = navigator.plugins; if (!p || typeof p.item !== 'function') return true;"
       "  return !('prototype' in p.item) && p.item.name === 'item' && /\\{ \\[native code\\] \\}$/.test(String(p.item)); })()");

    printf("== the page's own code is untouched ==\n");
    ck("page", "a page function keeps its source text",
       "pageThing.toString() === 'function pageThing(a) { return a + 1; }' && String(pageArrow) === '(x) => x * 2'");
    ck("page", "...its prototype and its [[Construct]]",
       "typeof pageThing.prototype === 'object' && new pageThing() instanceof pageThing");
    ck("page", "...and its .fileName and methods",
       "pageThing.fileName === 'http://fixture.test/run.js' && pageObj.method.toString() === 'method() { return \\'m\\'; }'");
    ck("page", "a page class extending a platform interface is the page's",
       "PageEl.toString().indexOf('connectedCallback') > 0 && typeof PageEl.prototype.connectedCallback === 'function'");
    ck("page", "a page callback's caller is still reported when page code calls it",
       "(function(){ var seen; function outer() { inner(); } function inner() { seen = inner.caller; } outer(); return seen && seen.name === 'outer'; })()");
    /* The one door through which the PLATFORM compiles page code BEFORE the
     * pass: the cssom shim turns <body onload="..."> into
     * `new Function('event', src)` at install. Measured: marking everything
     * the realm had compiled made the page's fromAttr read as native code
     * and lose [[Construct]] (quickjs.c eval_code). */
    ck("page", "<body onload> compiled by the platform AT INSTALL is still the page's code",
       "(function(){ if (typeof window.onload === 'function') window.onload(new Event('load'));"
       "  return typeof __fromAttr === 'function' && String(__fromAttr) === 'function fromAttr() { return 1; }' &&"
       "  typeof __fromAttr.prototype === 'object' && new __fromAttr() instanceof __fromAttr; })()");
    ck("page", "...and an inline handler compiled at dispatch (<span onclick>) too",
       "(function(){ document.getElementById('clk').dispatchEvent(new MouseEvent('click', { bubbles: true }));"
       "  return typeof __fromClick === 'function' && String(__fromClick) === 'function fromClick() { return 2; }' &&"
       "  new __fromClick() instanceof __fromClick; })()");

    printf("== the platform still works ==\n");
    ck("works", "querySelector / querySelectorAll / getElementsByClassName",
       "document.querySelector('#d').id === 'd' && document.querySelectorAll('p,div').length === 2 &&"
       "document.getElementsByClassName('x').length === 1 && document.getElementById('d').querySelector('b').textContent === 'there'");
    ck("works", "createElement + appendChild + classList + dataset",
       "(function(){ var e = document.createElement('span'); e.classList.add('k'); e.dataset.v = '1';"
       "  document.body.appendChild(e); return document.querySelector('span.k[data-v=\"1\"]') === e && e instanceof HTMLSpanElement; })()");
    ck("works", "addEventListener / dispatchEvent / CustomEvent",
       "(function(){ var n = 0; var el = document.getElementById('d');"
       "  el.addEventListener('ping', function (e) { n += e.detail; }); el.dispatchEvent(new CustomEvent('ping', { detail: 2 }));"
       "  return n === 2; })()");
    ck("works", "a custom element through the HTMLElement Proxy",
       "(function(){ customElements.define('page-el', PageEl); var e = document.createElement('page-el');"
       "  document.body.appendChild(e); return e instanceof PageEl && e.dataset.up === '1'; })()");
    ck("works", "fetch returns a promise; timers and performance",
       "fetch('data:,x') instanceof Promise && typeof setTimeout(function(){}, 1) === 'number' &&"
       "(performance.mark('m'), performance.getEntriesByName('m').length === 1) && typeof performance.now() === 'number'");
    ck("works", "history.pushState, URL, TextEncoder, structuredClone",
       "(history.pushState({a:1}, '', '/x'), history.state.a === 1) && new URL('http://a/b?c=1').searchParams.get('c') === '1' &&"
       "new TextEncoder().encode('ab').length === 2 && structuredClone({a:[1]}).a[0] === 1");
    ck("works", "canvas 2D, DOMParser, Range, MutationObserver deliver",
       "(function(){ var c = document.getElementById('c').getContext('2d'); if (c) { c.fillRect(0,0,1,1); }"
       "  var d = new DOMParser().parseFromString('<p>a</p>', 'text/html');"
       /* createRange lives in js_forms.c's editing layer, not linked here. */
       "  var r = typeof document.createRange === 'function' ? document.createRange() : null;"
       "  if (r) r.selectNodeContents(document.getElementById('d'));"
       "  return !!c && d.querySelector('p').textContent === 'a' && (!r || r.toString().indexOf('hi') === 0); })()");
    ck("works", "the ES intrinsics are QuickJS's own: untouched names and shapes",
       "Array.prototype.push.name === 'push' && typeof Array.prototype.push === 'function' && !('prototype' in Array.prototype.push) &&"
       "Object.getOwnPropertyDescriptor(Function.prototype, 'caller').get.name === __nsBareCallerGetterName &&"
       "typeof Promise.prototype === 'object' && new Promise(function(){}) instanceof Promise");
    run("var __mo = []; new MutationObserver(function (r) { __mo.push(r.length); }).observe(document.body, { childList: true });"
        "document.body.appendChild(document.createElement('i'));");
    pump_until_idle(50);
    ck("works", "a MutationObserver callback ran after the pass", "__mo.length >= 1");

    printf("== the census (page-side reflection, no C) ==\n");
    census_report("reflection", "JSON.stringify((function(c){ return { fns: c.fns, src: c.src.length, proto: c.proto.length, ctor: c.ctor.length, name: c.name.length,"
                  " firstSrc: c.src.slice(0,6), firstProto: c.proto.slice(0,6), firstCtor: c.ctor.slice(0,6), firstName: c.name.slice(0,6) }; })(__nsCensus(false)))");
    census_report("getters", "JSON.stringify((function(c){ return { fns: c.fns, src: c.src.length, proto: c.proto.length, ctor: c.ctor.length, name: c.name.length,"
                  " firstSrc: c.src.slice(0,6), firstProto: c.proto.slice(0,6), firstCtor: c.ctor.slice(0,6), firstName: c.name.slice(0,6) }; })(__nsCensus(true)))");
    ck("native", "census, reflection: every platform function a page can reflect on reads native",
       "(function(){ var c = __nsCensus(false); return c.fns > 1000 && c.src.length === 0; })()");
    ck("native", "census, reflection: no operation or accessor owns a prototype",
       "__nsCensus(false).proto.length === 0");
    ck("native", "census, reflection: no operation or accessor is a constructor",
       "__nsCensus(false).ctor.length === 0");
    ck("native", "census, reflection: no '' / 'value' / 'get' / 'set' names",
       "__nsCensus(false).name.length === 0");
    ck("native", "census, through the singletons' getters: every function still reads native",
       "__nsCensus(true).src.length === 0");
    /* Original claim: what getters hand out is made AFTER the pass (a fresh
     * collection per call), so this measures only closure-time shape.
     * Correction (2026-09-30): IDL getters also close over EXISTING namespace
     * objects, e.g. navigator.plugins. The positive run found 20 operations
     * with prototype/[[Construct]] behind those getters: a quiet descriptor
     * walk cannot reach the closed-over values. Keep that public surface in
     * the check; deleting it would hide a producer handoff defect. */
    ck("native", "census, through the singletons' getters: no operation or accessor owns a prototype or constructs",
       "(function(){ var c = __nsCensus(true); return c.proto.length === 0 && c.ctor.length === 0; })()");
    census_report("instances", "JSON.stringify((function(c){ return { made: c.made, fns: c.fns, src: c.src.length, proto: c.proto.length, ctor: c.ctor.length,"
                  " firstSrc: c.src.slice(0,6), firstProto: c.proto.slice(0,6), firstCtor: c.ctor.slice(0,6) }; })(__nsInstances()))");
    ck("native", "census, objects the platform makes after the pass: every function native, none owns a prototype or constructs",
       "(function(){ var c = __nsInstances(); return c.made > 60 && c.fns > 1000 &&"
       "  c.src.length === 0 && c.proto.length === 0 && c.ctor.length === 0; })()");

    printf("== shape.js, the committed fixture, against Chrome 153 ==\n");
    snprintf(path, sizeof path, "%s/shape.js", fixtures);
    char *shape = slurp(path);
    snprintf(path, sizeof path, "%s/chrome-153.json", fixtures);
    char *chrome = slurp(path);
    if (!shape || !chrome) {
        printf("FAIL [works] cannot read %s/{shape.js,chrome-153.json} -- run from the tree root\n", fixtures);
        fails++; f_other++;
    } else {
        JS_FreeValue(ctx, eval_page(shape, "http://fixture.test/shape.js"));
        JSValue cj = JS_ParseJSON(ctx, chrome, strlen(chrome), "chrome-153.json");
        JSValue g = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, g, "__chrome", cj);
        JS_FreeValue(ctx, g);
        run("var __ours = shapeDump();");
        /* NATIVE_SHAPE_DUMP=file: the whole dump, for tests/qmp/
         * webidl_shape_diff.py (run it on this and on the negative
         * control's dump for a before/after of every category). */
        const char *dump = getenv("NATIVE_SHAPE_DUMP");
        if (dump) {
            JSValue v = eval_page("JSON.stringify(__ours)", "http://fixture.test/dump.js");
            const char *s = JS_ToCString(ctx, v);
            FILE *f = s ? fopen(dump, "w") : NULL;
            if (f) { fputs(s, f); fclose(f); printf("census dump written: %s\n", dump); }
            if (s) JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, v);
        }
        census_report("shape.js native probes (ours)", "JSON.stringify(__ours.native)");
        /* The one probe that is not this pass's to answer: Chrome's
         * userAgent is a getter on Navigator.prototype, ours a data
         * property on navigator until the member-placement pass lands. */
        ck("native", "every shape.js `native` probe agrees with Chrome 153 (userAgent's placement excepted)",
           "Object.keys(__chrome.native).every(function (k) {"
           "  if (k === 'navigator.userAgent getter' && __ours.native[k] === 'undefined') return true;"
           "  return __ours.native[k] === __chrome.native[k]; })");
        census_report("shape.js interfaces whose fnsrc is not native",
                      "JSON.stringify(Object.keys(__ours.ifaces).filter(function (k) { var o = __ours.ifaces[k], c = __chrome.ifaces[k];"
                      "  return o && c && o.type === 'function' && o.fnsrc !== c.fnsrc; }))");
        ck("native", "every interface present in both dumps stringifies as Chrome's does (native)",
           "(function(){ var n = 0, bad = Object.keys(__ours.ifaces).filter(function (k) { var o = __ours.ifaces[k], c = __chrome.ifaces[k];"
           "  if (!o || !c || o.type !== 'function') return false; n++; return o.fnsrc !== c.fnsrc; }); return n > 50 && bad.length === 0; })()");
    }
    free(shape);
    free(chrome);

    printf("== a worker realm ==\n");
    run("var __w = []; var __wk = new Worker('http://fixture.test/w.js');"
        "__wk.onmessage = function (e) { __w.push(e.data); if (__w.length === 1) __wk.postMessage('go'); };");
    pump_until_idle(200);
    census_report("worker said", "JSON.stringify(__w)");
    ck("native", "the worker's own platform functions read native, named, without prototype",
       "(function(){ var r = JSON.parse(__w[0]); return r.ael === 'function addEventListener() { [native code] }' &&"
       "  r.aelName === 'addEventListener' && r.aelProto === false; })()");
    ck("native", "...including a per-message closure made after the pass (event.preventDefault): native, no prototype",
       "/^pd:function preventDefault\\(\\) \\{ \\[native code\\] \\}\\|false$/.test(__w[1])");
    ck("page", "the worker's own script keeps its source",
       "(function(){ var r = JSON.parse(__w[0]); return r.mine === 'function mine() { return 1; }' &&"
       "  r.st === 'function setTimeout() { [native code] }'; })()");

    /* NATIVE_SHAPE_EVAL=file.js: evaluate one more PAGE script at the end
     * and print its value -- a diagnostic, for asking this harness a
     * question the checks above do not. */
    const char *extra = getenv("NATIVE_SHAPE_EVAL");
    if (extra) {
        char *src = slurp(extra);
        if (src) {
            JSValue v = eval_page(src, "http://fixture.test/extra.js");
            js_page_pump();
            const char *s = JS_ToCString(ctx, v);
            printf("eval %s: %s\n", extra, s ? s : "(threw)");
            if (s) JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, v);
            free(src);
        } else printf("eval %s: cannot read\n", extra);
    }

    js_page_close();
    dom_free(root);

    printf("\nnative-shape: %d checks, %d failed (%d of %d [native]; %d other)\n",
           checks, fails, f_native, n_native, f_other);
    return fails ? 1 : 0;
}
