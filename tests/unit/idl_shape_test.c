/* idl_shape_test.c -- the platform's OBJECT MODEL, host-side, against the
 * browser's own install order (c/apps/gui/browser/js/runtime/idl_shape.c).
 *
 *     idl_shape_test SHAPE.JS OUT.json
 *
 * WHAT IT MEASURES. Not "does navigator.userAgent work" -- every other gate
 * asks that -- but WHERE things live: which object owns a member, what a
 * prototype chains to, what Object.prototype.toString answers, and whether a
 * platform accessor refuses a receiver that is not its object. Anti-tamper
 * scripts (Cloudflare's challenge among them) read exactly those things, and a
 * page that works perfectly can still be told apart from a browser by them.
 *
 * TWO OUTPUTS, two jobs:
 *
 *   1. OUT.json is tests/fixtures/webidl_shape/shape.js's dump, evaluated in
 *      THIS link -- the same script, unchanged, that produced chrome-153.json
 *      in Chrome and that the guest runs from shape.html. The make recipe diffs
 *      it against chrome-153.json with tests/qmp/webidl_shape_diff.py and
 *      holds the per-category counts to a checked-in ratchet
 *      (tests/unit/idl_shape_ratchet.py). The dump is written BEFORE any check
 *      below touches the page, so no assertion can perturb what is measured.
 *
 *   2. Checks, in two groups, and the split is what makes the negative control
 *      mean something (tests/idl_shape.mk, -DIDL_SHAPE_NEGCTL_SKIP compiles the
 *      pass to a no-op):
 *
 *        [shape]      what the pass exists to make true. The control must fail
 *                     THESE -- named ones, not merely "some".
 *        [behaviour]  what the page must still be able to DO: read its
 *                     cookie, push history, time itself, reach readyState
 *                     'complete' through the strict-mode writes platform.c
 *                     makes into performance.timing. These must pass in BOTH
 *                     builds; a [behaviour] failure in the control means the
 *                     control broke the page rather than the shape, and the
 *                     make recipe refuses it.
 *
 * THE LINK IS THE BROWSER'S. The source list is tests/wpt.mk's WPT_TEST_SRC
 * (the browser minus its named app shell) with wpt_test.c swapped for this
 * file -- see tests/idl_shape.mk for the one further subtraction and why. A
 * shape measured over a hand-picked subset of installers would be the shape of
 * a browser that does not exist: the pass moves what the OTHER installers put
 * on `document`, so every one of them has to have run.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "dom/tree.h"
#include "js/dom/dom.h"
#include "js/runtime/page.h"
#include "js/runtime/platform.h"
#include "js/runtime/idl_shape.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

/* The embedder's half of the link, as tests/unit/wpt_test.c supplies it:
 * layout asks how wide text is and for subresource bytes; the module loader
 * asks for a fetch. This page loads no subresource and no module, so every
 * transport answer is a refusal -- a real answer, not fabricated bytes. */
int text_measure(const char *s, int len, int px, int mono)
{ (void)s; (void)mono; return len * (px / 2); }
int res_fetch(const char *u, unsigned char **b, int *l)
{ (void)u; (void)b; (void)l; return -1; }
void bfetch_prefetch(const char *ref) { (void)ref; }
void bfetch_prefetch_wait(void) {}
int bfetch_resolve(const char *base, const char *ref, char *out, int max)
{ (void)base; if (!ref || !out || max <= 0) return 0; snprintf(out, (size_t)max, "%s", ref); return 1; }
int bfetch_sync(const char *ref, unsigned char **out, int *outlen)
{ (void)ref; (void)out; (void)outlen; return -1; }

static JSContext *g_ctx;
static int g_checks, g_fails, g_shape_fails, g_beh_fails;

/* The same markup shape.html parses, minus its two scripts -- the harness
 * evaluates shape.js itself so it can take the dump as a value. */
static const char *HTML =
    "<!doctype html><html><head><meta charset=\"utf-8\"><title>shape</title></head>"
    "<body><p id=\"status\">shape</p></body></html>";

static char *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b || (long)fread(b, 1, (size_t)n, f) != n) { free(b); fclose(f); return 0; }
    b[n] = 0; fclose(f);
    if (len) *len = n;
    return b;
}

/* One check: `expr` must evaluate truthy. A throw is a failure and prints the
 * exception, because "threw TypeError: not a function" and "was false" are
 * different findings. */
static void ck(int shape, const char *what, const char *expr)
{
    g_checks++;
    JSValue v = JS_Eval(g_ctx, expr, strlen(expr), "<idl-shape-check>", JS_EVAL_TYPE_GLOBAL);
    int ok = 0;
    const char *why = 0;
    char buf[512];
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(g_ctx);
        const char *m = JS_ToCString(g_ctx, e);
        snprintf(buf, sizeof buf, "threw: %s", m ? m : "?");
        why = buf;
        if (m) JS_FreeCString(g_ctx, m);
        JS_FreeValue(g_ctx, e);
    } else {
        ok = JS_ToBool(g_ctx, v);
        if (!ok) why = "was false";
    }
    JS_FreeValue(g_ctx, v);
    js_page_pump();
    if (ok) { printf("ok [%s] %s\n", shape ? "shape" : "behaviour", what); return; }
    g_fails++;
    if (shape) g_shape_fails++; else g_beh_fails++;
    printf("FAIL [%s] %s\n     %s\n     expr: %s\n", shape ? "shape" : "behaviour", what, why, expr);
}
#define SHAPE(what, expr) ck(1, what, expr)
#define BEH(what, expr)   ck(0, what, expr)

/* `f` must throw a TypeError whose message is exactly Chrome's. */
#define ILLEGAL(body) \
    "(function(){ try { " body "; return false; } catch (e) {" \
    " return e instanceof TypeError && e.message === 'Illegal invocation'; } })()"

static int eval_file(const char *src, long len, const char *name)
{
    if (!js_page_eval(src, (int)len, name, 0)) return 0;
    js_page_pump();
    return 1;
}

static int write_dump(const char *out)
{
    static const char *DUMP = "JSON.stringify(shapeDump())";
    JSValue v = JS_Eval(g_ctx, DUMP, strlen(DUMP), "<shape-dump>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(g_ctx);
        const char *m = JS_ToCString(g_ctx, e);
        printf("FAIL shapeDump() threw: %s\n", m ? m : "?");
        if (m) JS_FreeCString(g_ctx, m);
        JS_FreeValue(g_ctx, e);
        return 0;
    }
    const char *s = JS_ToCString(g_ctx, v);
    FILE *f = s ? fopen(out, "w") : 0;
    int ok = f && fputs(s, f) >= 0;
    if (f) fclose(f);
    if (s) JS_FreeCString(g_ctx, s);
    JS_FreeValue(g_ctx, v);
    if (!ok) printf("FAIL cannot write %s\n", out);
    else printf("shape dump -> %s\n", out);
    return ok;
}

static void checks_shape(void);
static void checks_behaviour(void);
static void checks_lifecycle(void);

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s shape.js out.json\n", argv[0]); return 2; }
    long jl = 0;
    char *js = slurp(argv[1], &jl);
    if (!js) { printf("FAIL cannot read %s\n", argv[1]); return 1; }

    struct node *root = dom_parse(HTML, (int)strlen(HTML));
    if (!root) { printf("FAIL parse\n"); return 1; }
    /* The origin the reference was captured on (PROVENANCE.md: Chrome 153 over
     * http://127.0.0.1). It decides cookieEnabled and document.domain, and
     * both are values the dump does not record but the checks do. */
    js_page_set_location("http://127.0.0.1:8765/shape.html");
    if (!js_page_open(root)) { printf("FAIL js_page_open\n"); return 1; }
    g_ctx = js_page_ctx();
    /* One evaluation first: js_dom.c runs a compatibility bridge out of the
     * job queue, and a dump taken before it would be of a state no page sees. */
    js_page_eval("void 0;", 7, "<warmup>", 0);

    if (!eval_file(js, jl, "shape.js")) { printf("FAIL shape.js did not evaluate\n"); return 1; }
    free(js);
    if (!write_dump(argv[2])) return 1;

    /* An optional third argument is a script whose completion value is
     * printed: the cheapest way to ask this exact link a question while
     * reading a finding, without a guest boot. */
    if (argc > 3) {
        long xl = 0;
        char *x = slurp(argv[3], &xl);
        if (!x) { printf("FAIL cannot read %s\n", argv[3]); return 1; }
        JSValue v = JS_Eval(g_ctx, x, (size_t)xl, argv[3], JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) { JS_FreeValue(g_ctx, v); v = JS_GetException(g_ctx); }
        const char *s = JS_ToCString(g_ctx, v);
        printf("%s\n", s ? s : "?");
        if (s) JS_FreeCString(g_ctx, s);
        JS_FreeValue(g_ctx, v);
        free(x);
    }

    /* Behaviour FIRST: several [shape] checks write (delete navigator.webdriver,
     * cookie, scrollRestoration), and in the control build those writes land
     * on the instance -- the first version of this file ran shape first and
     * the control's `delete navigator.webdriver` failed a [behaviour] check
     * that had nothing to do with it. */
    printf("== [behaviour] what the page can still do ==\n");
    checks_behaviour();
    printf("== [shape] where members live, what the chains are ==\n");
    checks_shape();
    printf("== [behaviour] the document lifecycle through the moved members ==\n");
    checks_lifecycle();

    js_page_close();
    dom_free(root);
    printf("\nidl-shape: %d checks, %d failed (%d shape, %d behaviour)\n",
           g_checks, g_fails, g_shape_fails, g_beh_fails);
    printf(g_fails ? "IDL SHAPE TEST FAILED\n" : "IDL SHAPE TEST PASSED\n");
    return g_fails ? 1 : 0;
}

static void checks_shape(void)
{
    /* --- where members live: the singletons own nothing but what a browser's
     * own. `location` is [LegacyUnforgeable] and is the one name Chrome's
     * document keeps (chrome-153.json singletons.document.own). */
    SHAPE("document owns exactly [location]",
          "JSON.stringify(Reflect.ownKeys(document)) === '[\"location\"]'");
    SHAPE("navigator owns nothing",   "Reflect.ownKeys(navigator).length === 0");
    SHAPE("screen owns nothing",      "Reflect.ownKeys(screen).length === 0");
    SHAPE("history owns nothing",     "Reflect.ownKeys(history).length === 0");
    SHAPE("performance owns nothing", "Reflect.ownKeys(performance).length === 0");
    SHAPE("crypto owns nothing",      "Reflect.ownKeys(crypto).length === 0");
    SHAPE("document.body owns none of the 22 Window-reflecting handlers",
          "Reflect.ownKeys(document.body).filter(function (k) { return /^on/.test(k); }).length === 0");

    /* --- Object.prototype.toString, the cheapest fingerprint there is. */
    SHAPE("Object.prototype.toString.call(navigator) === '[object Navigator]'",
          "Object.prototype.toString.call(navigator) === '[object Navigator]'");
    SHAPE("screen, history, performance, crypto, window answer their interface's name",
          "Object.prototype.toString.call(screen) === '[object Screen]' &&"
          "Object.prototype.toString.call(history) === '[object History]' &&"
          "Object.prototype.toString.call(performance) === '[object Performance]' &&"
          "Object.prototype.toString.call(crypto) === '[object Crypto]' &&"
          "Object.prototype.toString.call(window) === '[object Window]' &&"
          "Object.prototype.toString.call(localStorage) === '[object Storage]'");
    SHAPE("a @@toStringTag is {writable:false, enumerable:false, configurable:true}",
          "(function () { var d = Object.getOwnPropertyDescriptor(Navigator.prototype, Symbol.toStringTag);"
          " return d && d.value === 'Navigator' && !d.writable && !d.enumerable && d.configurable; })()");
    /* The pass tags PLATFORM interfaces. An ECMAScript constructor must keep
     * ECMAScript's answer: Uint8Array.prototype has no tag of its own on
     * purpose, and SuppressedError is intl.c's polyfill of an ECMAScript error
     * (whose instances answer '[object Object]' here today, pass or no pass --
     * the polyfill's business, not this gate's; what is asserted is that the
     * pass minted nothing on it). This check PASSES in the control too, so it
     * is a guard on the pass, not part of what the control must break. */
    BEH("no tag is minted on an ECMAScript prototype (Uint8Array, SuppressedError, Error)",
        "!Object.prototype.hasOwnProperty.call(Uint8Array.prototype, Symbol.toStringTag) &&"
        "Object.prototype.toString.call(Uint8Array.prototype) === '[object Object]' &&"
        "!Object.prototype.hasOwnProperty.call(Error.prototype, Symbol.toStringTag) &&"
        "(typeof SuppressedError !== 'function' ||"
        " !Object.prototype.hasOwnProperty.call(SuppressedError.prototype, Symbol.toStringTag))");

    /* --- the interfaces that had instances and no interface object. */
    SHAPE("Screen and Crypto exist, and are illegal constructors",
          "typeof Screen === 'function' && typeof Crypto === 'function' &&"
          "Object.getPrototypeOf(screen) === Screen.prototype &&"
          "Object.getPrototypeOf(crypto) === Crypto.prototype &&"
          "(function () { try { new Screen(); return false; } catch (e) {"
          " return e instanceof TypeError && e.message === 'Illegal constructor'; } })()");
    SHAPE("interface objects do not enumerate on window",
          "['Navigator','Screen','History','Performance','Crypto','Location','Storage','Window']"
          ".every(function (n) { var d = Object.getOwnPropertyDescriptor(window, n);"
          " return d && !d.enumerable && d.writable && d.configurable; }) &&"
          "Object.keys(window).indexOf('Navigator') < 0");

    /* --- chains. */
    SHAPE("Performance.prototype chains to EventTarget.prototype",
          "Object.getPrototypeOf(Performance.prototype) === EventTarget.prototype &&"
          "Object.getPrototypeOf(Performance) === EventTarget && performance instanceof EventTarget");
    SHAPE("Screen.prototype chains to EventTarget.prototype",
          "Object.getPrototypeOf(Screen.prototype) === EventTarget.prototype");
    SHAPE("window: Window.prototype > WindowProperties > EventTarget.prototype",
          "Object.getPrototypeOf(window) === Window.prototype &&"
          "Object.prototype.toString.call(Object.getPrototypeOf(Window.prototype)) === '[object WindowProperties]' &&"
          "Object.getPrototypeOf(Object.getPrototypeOf(Window.prototype)) === EventTarget.prototype &&"
          "Window.prototype.constructor === Window && window instanceof Window &&"
          "!Object.prototype.hasOwnProperty.call(Window, Symbol.hasInstance)");
    SHAPE("AbortSignal, Worker, EventSource, FontFaceSet inherit EventTarget",
          "[AbortSignal, Worker, EventSource, FontFaceSet].every(function (C) {"
          " return Object.getPrototypeOf(C.prototype) === EventTarget.prototype; })");
    SHAPE("performance inherits a WORKING addEventListener (not a node-bound one)",
          "(function () { var hit = 0, f = function () { hit++; };"
          " performance.addEventListener('idl-shape-x', f);"
          " performance.dispatchEvent(new Event('idl-shape-x'));"
          " performance.removeEventListener('idl-shape-x', f);"
          " performance.dispatchEvent(new Event('idl-shape-x')); return hit === 1; })()");

    /* --- member kinds and names: what a descriptor dump sees. */
    SHAPE("navigator.userAgent is a getter named 'get userAgent' on Navigator.prototype",
          "(function () { var d = Object.getOwnPropertyDescriptor(Navigator.prototype, 'userAgent');"
          " return d && typeof d.get === 'function' && d.set === undefined && d.enumerable &&"
          " d.configurable && d.get.name === 'get userAgent' && !('prototype' in d.get); })()");
    SHAPE("moved operations are enumerable, non-constructible, named, WebIDL length",
          "(function () { var d = Object.getOwnPropertyDescriptor(History.prototype, 'pushState');"
          " var j = Navigator.prototype.javaEnabled, m = Performance.prototype.mark;"
          " return d && d.enumerable && d.writable && d.value.name === 'pushState' &&"
          " d.value.length === 2 && History.prototype.go.length === 0 && m.length === 1 &&"
          " j.name === 'javaEnabled' && !('prototype' in j) &&"
          " (function () { try { new j(); return false; } catch (e) { return e instanceof TypeError; } })(); })()");
    SHAPE("Document.prototype carries readyState, cookie (get+set), the on* and the rest",
          "(function () { var P = Document.prototype, g = function (k) { return Object.getOwnPropertyDescriptor(P, k); };"
          " return g('readyState').get && !g('readyState').set && g('cookie').get && g('cookie').set &&"
          " g('onclick').get && g('onclick').set && typeof g('createEvent').value === 'function' &&"
          " g('fonts').get && g('timeline').get && typeof g('getSelection').value === 'function' &&"
          " !('onunload' in document); })()");
    SHAPE("HTMLBodyElement.prototype carries onload .. onbeforeprint as get/set",
          "['onload','onhashchange','onmessage','onpopstate','onbeforeprint'].every(function (k) {"
          " var d = Object.getOwnPropertyDescriptor(HTMLBodyElement.prototype, k);"
          " return d && d.get && d.set && d.enumerable; })");

    /* --- brand checks: a browser REFUSES the wrong receiver, and a detector
     * tests that it throws rather than returning undefined. */
    SHAPE("brand: reading Navigator.prototype.userAgent throws 'Illegal invocation'",
          ILLEGAL("Navigator.prototype.userAgent"));
    SHAPE("brand: the userAgent getter called on {} throws 'Illegal invocation'",
          ILLEGAL("Object.getOwnPropertyDescriptor(Navigator.prototype, 'userAgent').get.call({})"));
    SHAPE("brand: a navigator Proxy is not navigator",
          ILLEGAL("new Proxy(navigator, {}).userAgent"));
    SHAPE("brand: Document.prototype.cookie on a <div> / on Document.prototype throws",
          ILLEGAL("Object.getOwnPropertyDescriptor(Document.prototype, 'cookie').get.call(document.createElement('div'))") " && "
          ILLEGAL("Document.prototype.readyState"));
    SHAPE("brand: History.prototype.pushState.call({}) throws, and pushed nothing",
          "(function () { var n = history.length; return " ILLEGAL("History.prototype.pushState.call({}, null, '', '#brand')")
          " && history.length === n; })()");
    SHAPE("brand: performance / screen / crypto members refuse {}",
          ILLEGAL("Performance.prototype.now.call({})") " && "
          ILLEGAL("Object.getOwnPropertyDescriptor(Screen.prototype, 'width').get.call({})") " && "
          ILLEGAL("Crypto.prototype.getRandomValues.call({}, new Uint8Array(4))"));
    SHAPE("brand: the body handlers refuse a <div>",
          ILLEGAL("Object.getOwnPropertyDescriptor(HTMLBodyElement.prototype, 'onload').get.call(document.createElement('div'))"));

    /* --- ANOTHER Document shares Document.prototype (createHTMLDocument), and
     * must get its own answer or a spec default -- never the page's state. */
    SHAPE("createHTMLDocument: cookie '' and cookie-averse, never the page jar",
          "(function () { document.cookie = 'idlpage=1';"
          " var x = document.implementation.createHTMLDocument('t');"
          " var r = x.cookie === ''; x.cookie = 'idlother=2';"
          " return r && x.cookie === '' && document.cookie.indexOf('idlother') < 0 &&"
          " document.cookie.indexOf('idlpage=1') >= 0; })()");
    SHAPE("createHTMLDocument: readyState complete, currentScript null, CSS1Compat, no selection",
          "(function () { var x = document.implementation.createHTMLDocument('t');"
          " return x.readyState === 'complete' && x.currentScript === null &&"
          " x.compatMode === 'CSS1Compat' && x.getSelection() === null &&"
          " x.activeElement === x.body && x.execCommand('bold') === false &&"
          " x.styleSheets.length === 0 && x.fonts !== document.fonts &&"
          " x.timeline !== document.timeline; })()");
    SHAPE("createHTMLDocument: onclick is its OWN slot -- set, fired, and not the page's",
          "(function () { var x = document.implementation.createHTMLDocument('t'), hit = 0, page = 0;"
          " var f = function () { hit++; }; document.onclick = function () { page++; };"
          " x.onclick = f; var own = Object.getOwnPropertyNames(x).indexOf('onclick') < 0;"
          " x.dispatchEvent(new MouseEvent('click'));"
          " var ok = own && x.onclick === f && hit === 1 && page === 0 && document.onclick !== f;"
          " x.onclick = null; document.onclick = null; return ok && x.onclick === null; })()");
    SHAPE("createHTMLDocument: its <body>'s onload is null and does not reach window",
          "(function () { var x = document.implementation.createHTMLDocument('t'), f = function () {};"
          " var before = window.onload; x.body.onload = f;"
          " return x.body.onload === null && window.onload === before; })()");
    SHAPE("createHTMLDocument: getElementsByName and links search IT, not the page",
          "(function () { var x = document.implementation.createHTMLDocument('t');"
          " x.body.innerHTML = '<a name=\"q\" href=\"#\">a</a>';"
          " return x.getElementsByName('q').length === 1 && x.links.length === 1 &&"
          " x.links instanceof HTMLCollection && x.getElementsByName('q') instanceof NodeList &&"
          " document.getElementsByName('q').length === 0; })()");

    SHAPE("a DOMParser document reaching an extracted Document member gets ITS answer",
          "typeof DOMParser !== 'function' || (function () {"
          " var p = new DOMParser().parseFromString('<p name=\"a\">x</p>', 'text/html');"
          " var P = Document.prototype, g = function (k) { return Object.getOwnPropertyDescriptor(P, k).get; };"
          " return P.getElementsByName.call(p, 'a').length === 1 && g('cookie').call(p) === '' &&"
          " g('readyState').call(p) === 'complete' && g('currentScript').call(p) === null; })()");

    /* --- per-realm storage with WebIDL setter semantics. */
    SHAPE("history.scrollRestoration: 'manual' sticks, an invalid enum value is ignored",
          "(function () { history.scrollRestoration = 'manual'; var a = history.scrollRestoration;"
          " history.scrollRestoration = 'bogus'; var b = history.scrollRestoration;"
          " history.scrollRestoration = 'auto'; return a === 'manual' && b === 'manual'; })()");
    SHAPE("navigator.languages is one frozen array ([SameObject] FrozenArray)",
          "navigator.languages === navigator.languages && Object.isFrozen(navigator.languages)");
    SHAPE("delete navigator.webdriver removes nothing (it is not navigator's to delete)",
          "(function () { delete navigator.webdriver; return navigator.webdriver === false; })()");
}

static void checks_behaviour(void)
{
    BEH("navigator answers what it answered before",
        "navigator.userAgent === 'Mozilla/5.0 (LogitOS; x86_64) Logit/1.0' &&"
        "navigator.cookieEnabled === true && navigator.webdriver === false &&"
        "navigator.language === 'en-US' && navigator.languages[0] === 'en-US' &&"
        "navigator.plugins.length === 0 && navigator.javaEnabled() === false &&"
        "typeof navigator.sendBeacon === 'function' && navigator.onLine === true");
    BEH("[SameObject]: navigator.serviceWorker / clipboard / plugins read back the same object",
        "navigator.plugins === navigator.plugins && navigator.clipboard === navigator.clipboard &&"
        "(!('serviceWorker' in navigator) || navigator.serviceWorker === navigator.serviceWorker)");
    BEH("the page's cookie jar reads and writes",
        "(function () { document.cookie = 'idlbeh=7'; return document.cookie.indexOf('idlbeh=7') >= 0; })()");
    BEH("history.pushState / state / length",
        "(function () { var n = history.length; history.pushState({ a: 1 }, '', '#idl');"
        " return history.state.a === 1 && history.length === n + 1 && location.hash === '#idl'; })()");
    BEH("history.scrollRestoration is writable",
        "(function () { history.scrollRestoration = 'manual'; var v = history.scrollRestoration;"
        " history.scrollRestoration = 'auto'; return v === 'manual'; })()");
    BEH("performance: now, mark, getEntriesByName, timing, navigation, toJSON",
        "(function () { performance.mark('idl-m'); var t = performance.timing;"
        " return typeof performance.now() === 'number' && performance.getEntriesByName('idl-m').length === 1 &&"
        " t.navigationStart > 0 && t === performance.timing && performance.navigation.type === 0 &&"
        " typeof performance.timeOrigin === 'number' && 'timeOrigin' in performance.toJSON(); })()");
    BEH("screen and crypto answer",
        "typeof screen.width === 'number' && screen.colorDepth === 24 &&"
        "crypto.getRandomValues(new Uint8Array(8)).length === 8 &&"
        "/^[0-9a-f-]{36}$/.test(crypto.randomUUID()) && typeof crypto.subtle === 'object'");
    BEH("document.onclick fires for a click bubbling from <body>",
        "(function () { var hit = 0; document.onclick = function () { hit++; };"
        " document.body.dispatchEvent(new MouseEvent('click', { bubbles: true }));"
        " document.onclick = null; return hit === 1 && document.onclick === null; })()");
    BEH("document.addEventListener / dispatchEvent / removeEventListener",
        "(function () { var hit = 0, f = function () { hit++; };"
        " document.addEventListener('idl-ev', f); document.dispatchEvent(new Event('idl-ev'));"
        " document.removeEventListener('idl-ev', f); document.dispatchEvent(new Event('idl-ev'));"
        " return hit === 1; })()");
    BEH("window.onhashchange and document.body.onhashchange are one handler",
        "(function () { var f = function () {}; window.onhashchange = f;"
        " var ok = document.body.onhashchange === f; document.body.onhashchange = null;"
        " return ok && window.onhashchange === null; })()");
    BEH("document queries, currentScript outside a script, readyState before parse end",
        "document.getElementById('status') !== null && document.querySelectorAll('p').length === 1 &&"
        "document.scripts.length === 0 && document.currentScript === null &&"
        "document.readyState === 'loading' && document.visibilityState === 'visible' &&"
        "document.hidden === false && typeof document.domain === 'string' &&"
        "document.compatMode === 'CSS1Compat' && document.styleSheets.length === 0");
    /* The receiver dispatch keys on the page document's IDENTITY. Every path
     * that hands a script the page document must hand it THAT object, or it
     * would be served another document's defaults. */
    BEH("the page document reached through ownerDocument / parentNode / getRootNode is the page's",
        "document.body.ownerDocument === document && document.documentElement.parentNode === document &&"
        "document.body.getRootNode() === document &&"
        "document.body.ownerDocument.cookie === document.cookie &&"
        "document.body.ownerDocument.readyState === document.readyState");
    BEH("document.domain is stored when written",
        "(function () { var d = document.domain; document.domain = d; return document.domain === d; })()");
    BEH("document.createEvent / createRange / createTreeWalker / getSelection work on the page",
        "document.createEvent('Event') instanceof Event && typeof document.createRange() === 'object' &&"
        "typeof document.createTreeWalker(document.body).nextNode === 'function' &&"
        "document.getSelection() !== null");
}

/* The readyState machine lives in platform.c and advances through STRICT-mode
 * writes into performance.timing's fields (G.__platMarkTiming). If the timing
 * getter handed out a copy, or the fields became getter-only, the write at
 * domInteractive would throw BEFORE change('interactive') and readyState would
 * stick -- silently, because platform.c catches nothing there. So this drives
 * the lifecycle exactly as app/main.c does and reads the answer back through
 * the moved getters. */
static void checks_lifecycle(void)
{
    static const char *RSC = "window.__idlRSC = 0; document.addEventListener('readystatechange',"
                             " function () { window.__idlRSC++; });";
    js_page_eval(RSC, (int)strlen(RSC), "<idl-lifecycle>", 0);
    js_platform_document_parsed(g_ctx);
    js_page_pump();
    BEH("readyState is 'interactive' after the parser boundary",
        "document.readyState === 'interactive' && window.__idlRSC === 1");
    struct js_event_init li = { 0 };
    li.bubbles = 1;
    js_dom_dispatch(js_dom_root(), "DOMContentLoaded", &li);
    struct js_event_init le = { 0 };
    js_dom_dispatch(js_dom_root(), "load", &le);
    js_page_pump();
    BEH("readyState is 'complete' after load, and readystatechange fired twice",
        "document.readyState === 'complete' && window.__idlRSC === 2");
    BEH("performance.timing recorded the lifecycle (the strict writes landed)",
        "(function () { var t = performance.timing;"
        " return t.domComplete >= t.navigationStart && t.loadEventEnd >= t.domComplete &&"
        " t.domContentLoadedEventEnd >= t.domInteractive; })()");
    /* A writer that runs AFTER js_page_open -- at parse end, at DCL, at load --
     * would put a member back on the instance, and the dump above (taken
     * before the lifecycle ran) could not see it. The guest dump is taken
     * after load and would; this is the same question asked in seconds. */
    SHAPE("after DOMContentLoaded and load, the singletons still own nothing new",
          "JSON.stringify(Reflect.ownKeys(document)) === '[\"location\"]' &&"
          "Reflect.ownKeys(navigator).length === 0 && Reflect.ownKeys(performance).length === 0 &&"
          "Reflect.ownKeys(history).length === 0 &&"
          "Reflect.ownKeys(document.body).filter(function (k) { return /^on/.test(k); }).length === 0");

    /* THE SECOND CALL, which frame/passive.c makes: frame_focus_install defines
     * an own, enumerable document.activeElement on a network frame's document
     * AFTER js_page_open (and so after the pass) returned. Re-running the pass
     * must move exactly that and re-wrap nothing it already moved -- a second
     * wrapper around a getter would still answer, and would double the brand
     * check and the stack depth of every read for the rest of the page. */
    static const char *LATE =
        "window.__idlUA = Object.getOwnPropertyDescriptor(Navigator.prototype, 'userAgent');"
        "window.__idlRS = Object.getOwnPropertyDescriptor(History.prototype, 'scrollRestoration');"
        "window.__idlPS = History.prototype.pushState;"
        "Object.defineProperty(document, 'activeElement', { configurable: true, enumerable: true,"
        " get: function () { return document.documentElement; } });";
    js_page_eval(LATE, (int)strlen(LATE), "<idl-late-writer>", 0);
    js_idl_shape_install(g_ctx);
    js_page_pump();
    SHAPE("a second pass moves a late own activeElement and re-wraps nothing",
          "JSON.stringify(Reflect.ownKeys(document)) === '[\"location\"]' &&"
          "document.activeElement === document.documentElement &&"
          "Object.getOwnPropertyDescriptor(Document.prototype, 'activeElement').enumerable &&"
          "Object.getOwnPropertyDescriptor(Navigator.prototype, 'userAgent').get === __idlUA.get &&"
          "Object.getOwnPropertyDescriptor(History.prototype, 'scrollRestoration').set === __idlRS.set &&"
          "History.prototype.pushState === __idlPS &&"
          "document.implementation.createHTMLDocument('t').activeElement !== document.documentElement");
}
