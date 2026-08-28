/* Host test for c/apps/browser/js_cache.c (CacheStorage/Cache) and
 * c/apps/browser/js_swreg.c (navigator.serviceWorker) -- the two parts of
 * the ServiceWorker workflow item that ship (part C, executing a service
 * worker script and intercepting fetch, is refused by name -- see both
 * files' headers).
 *
 *     make test-cache                    the coherent-subset suite for BOTH
 *                                         files, PLUS the termination-bar
 *                                         quiescence check: every promise
 *                                         this run created must have reached
 *                                         a terminal state (resolved or
 *                                         rejected) within a bounded pump.
 *     make test-cache-negctl-put         SAME file, linked with
 *                                         -DJS_CACHE_NO_SETTLE (js_cache.c's
 *                                         own control: Cache.prototype.put
 *                                         becomes a Promise executor that
 *                                         never calls resolve/reject) --
 *                                         must FAIL, on exactly the put()-
 *                                         shaped checks.
 *     make test-cache-negctl-register    SAME file, linked with
 *                                         -DJS_SWREG_PENDING_REGISTER
 *                                         (js_swreg.c's own control:
 *                                         register() becomes a Promise
 *                                         executor that never settles) --
 *                                         must FAIL, on exactly the
 *                                         register()-shaped checks.
 *     make test-cache-quota               a SEPARATE binary, compiled with
 *                                         -DJS_CACHE_QUOTA_BYTES=200 so the
 *                                         QuotaExceededError path is
 *                                         reachable without allocating the
 *                                         real 64 MiB cap in a host test.
 *
 * Unlike tests/worker.mk's quiescence check, there is no separate
 * js_*_pending() predicate to assert empty here: neither file introduces a
 * second task queue (js_cache.c and js_swreg.c build every async result as a
 * real Promise over the PAGE's own microtask queue and, for cache.add/
 * addAll, the SAME wfetch machinery js_webapi_pump already drains) -- so
 * "every tracker reached done" together with js_page_pending()==0 after a
 * bounded pump is the whole termination story, the same shape
 * webapi_idb_test.c uses.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "quickjs.h"
#include "dom.h"
#include "js_dom.h"
#include "js_page.h"
#include "loader_fakebfetch.h"

/* cache.add()/addAll() call the WINDOW's `fetch()`, which is a DIFFERENT
 * transport from bfetch (loader_fakebfetch.c above, used by <img>/<script>
 * loading and by Worker's importScripts/startup fetch -- see js_worker.c).
 * Under -DWEBAPI_HOST, window fetch() defaults to an all-NULL net ("could
 * not open a socket" for every request) UNLESS WEBAPI_FILE_ROOT names a
 * directory, in which case js_filenet.inc serves GETs from it over a REAL
 * HTTP/1.1 parser -- js_filenet.inc's own header names exactly why this
 * matters and is not a shortcut: "a URL parser that was never asked" is
 * indistinguishable from a passing one until someone checks. This is the
 * SAME mechanism tests/unit/wpt_test.c uses, and the SAME env var the
 * workflow's own scope note warns is a 17x-magnitude apparatus trap to omit. */
static char g_fileroot[] = "/tmp/logit_cache_test_XXXXXX";
static void write_fixture(const char *dir, const char *name, const char *body)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { printf("FAIL: could not write fixture %s\n", path); exit(1); }
    fwrite(body, 1, strlen(body), f);
    fclose(f);
}

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
__attribute__((__weak__)) void img_register(void *d) { (void)d; }
__attribute__((__weak__)) void img_register_anim(void *a, void *b, void *c)
{ (void)a; (void)b; (void)c; }

static JSContext *ctx;
static int checks, failures;

static void ck(int cond, const char *name)
{
    checks++;
    if (!cond) { failures++; printf("FAIL: %s\n", name); }
    else printf("ok  : %s\n", name);
}

static JSValue eval(const char *src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("      [js exception] %s\n         while evaluating: %s\n", m ? m : "?", src);
        if (m) JS_FreeCString(ctx, m);
        JSValue st = JS_GetPropertyStr(ctx, e, "stack");
        if (!JS_IsUndefined(st)) {
            const char *ss = JS_ToCString(ctx, st);
            if (ss) { printf("      %s\n", ss); JS_FreeCString(ctx, ss); }
        }
        JS_FreeValue(ctx, st);
        JS_FreeValue(ctx, e);
    }
    return v;
}

static void run(const char *src) { JS_FreeValue(ctx, eval(src)); js_page_pump(); }

static void ckjs(const char *expr, const char *name)
{
    char buf[8192];
    snprintf(buf, sizeof buf, "(function(){ try { return (%s); } catch (e) { return 'threw: ' + e; } })()", expr);
    JSValue v = eval(buf);
    int ok = !JS_IsException(v) && JS_ToBool(ctx, v) && !JS_IsString(v);
    if (!ok && !JS_IsException(v)) {
        const char *s = JS_ToCString(ctx, v);
        printf("      value was: %s\n", s ? s : "?");
        if (s) JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    ck(ok, name);
}

static unsigned long long g_now;
static unsigned long long clock_fn(void) { return g_now; }
static void tick(int ms) { g_now += (unsigned long long)ms; js_page_run_due(); js_page_pump(); }

static int pump_until_idle(int max_passes)
{
    int n = 0;
    while (js_page_pending() && n < max_passes) { tick(1); n++; }
    return n;
}

static const char *PAGE =
    "<!doctype html><html><head><title>t</title></head><body>"
    "<script>var ran=1;</script></body></html>";

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    fake_site_reset();
    fake_site_add("http://fixture.test/a.txt", "hello-a");
    fake_site_add("http://fixture.test/b.txt", "hello-b");
    /* http://fixture.test/missing.txt deliberately NOT registered. */

    /* The REAL fixture root for window fetch() (cache.add/addAll) -- see the
     * comment on g_fileroot above. Same body as the bfetch fixture, on
     * purpose: only the transport differs between the two, not the corpus. */
    if (!mkdtemp(g_fileroot)) { printf("FAIL: mkdtemp for WEBAPI_FILE_ROOT\n"); return 1; }
    write_fixture(g_fileroot, "a.txt", "hello-a");
    /* missing.txt deliberately NOT written -- fnet_serve answers a 404,
     * with a body, for anything not on disk. */
    setenv("WEBAPI_FILE_ROOT", g_fileroot, 1);

    struct node *root = dom_parse(PAGE, (int)strlen(PAGE));
    if (!root) { printf("FAIL: fixture did not parse\n"); return 1; }

    js_page_set_clock(clock_fn);
    /* https, not http: navigator.serviceWorker.register()'s secure-context
     * gate (js_swreg.c's isTrustworthyOrigin) is a REAL check against the
     * document's own scheme, not decoration -- see js_swreg.c's header on
     * why it does not read the hardcoded G.isSecureContext. An http origin
     * would make every register() call below fail at that gate before ever
     * reaching the checks each scenario means to exercise. */
    js_page_set_location("https://fixture.test/page.html");
    if (!js_page_open(root)) { printf("FAIL: js_page_open\n"); return 1; }
    ctx = js_page_ctx();

    /* Every tracker created below is pushed here; the quiescence check at
     * the end walks all of them. {name, done, ok} */
    run("var W = { trackers: [] };");
    run("function TR(name) { var t = { name: name, done: false, ok: false, log: [] }; W.trackers.push(t); return t; }");
    run("function settle(t, p, checker) { p.then(function (v) { t.ok = !checker || !!checker(v, null); t.done = true; }, "
        "function (e) { t.ok = !!checker && !!checker(null, e); t.done = true; }); }");
    /* Looked up by NAME, not index -- a tracker earlier in the file that
     * later gets removed or reordered must not silently shift every check
     * below it onto the wrong tracker. */
    run("function TOK(name) { for (var i = 0; i < W.trackers.length; i++) "
        "if (W.trackers[i].name === name) return W.trackers[i].done === true && W.trackers[i].ok === true; return false; }");

    /* ==== feature detection ============================================== */
    ckjs("typeof caches === 'object' && caches !== null", "caches exists on the window");
    ckjs("typeof Cache === 'function'", "Cache constructor exists");
    ckjs("typeof CacheStorage === 'function'", "CacheStorage constructor exists");
    ckjs("(function(){ try { new Cache(); return false; } catch (e) { return e instanceof TypeError; } })()",
         "`new Cache()` throws (illegal constructor -- Cache has no public constructor)");
    ckjs("typeof navigator === 'object' && 'serviceWorker' in navigator", "'serviceWorker' in navigator");
    ckjs("typeof navigator.serviceWorker === 'object'", "navigator.serviceWorker is an object");
    ckjs("typeof navigator.serviceWorker.register === 'function'", "register() exists");
    ckjs("typeof ServiceWorker === 'function' && typeof ServiceWorkerRegistration === 'function'",
         "ServiceWorker/ServiceWorkerRegistration exist for instanceof/idlharness");
    ckjs("navigator.serviceWorker.controller === null", "controller is null (honest: nothing is ever registered successfully)");

    /* ==== caches.open() / has() / keys() / delete() ====================== */
    run("var tOpen = TR('open'); settle(tOpen, caches.open('v1'), function (c) { return c instanceof Cache; });");
    ckjs("TOK('open')", "caches.open() resolves with a real Cache instance");
    run("var tHas = TR('has'); settle(tHas, caches.has('v1'), function (v) { return v === true; });");
    ckjs("TOK('has')", "caches.has('v1') resolves true after open()");
    run("var tHasNo = TR('has-absent'); settle(tHasNo, caches.has('nope'), function (v) { return v === false; });");
    ckjs("TOK('has-absent')", "caches.has() of an unopened name resolves false");
    run("var tKeys = TR('keys'); settle(tKeys, caches.keys(), function (v) { return v.length === 1 && v[0] === 'v1'; });");
    ckjs("TOK('keys')", "caches.keys() lists the opened name");

    /* ==== put() / match(): the round trip, and the body really moved ===== */
    run("var v1;");
    run("caches.open('v1').then(function (c) { v1 = c; });");
    js_page_pump();
    run("var tPut = TR('put'); settle(tPut, v1.put(new Request('http://fixture.test/a.txt'), new Response('stored-body')), "
        "function (v) { return v === undefined; });");
    ckjs("TOK('put')", "cache.put() resolves undefined");
    run("var tMatch = TR('match'); v1.match('http://fixture.test/a.txt').then(function (r) { "
        "if (!r) { tMatch.done = true; return; } r.text().then(function (t) { tMatch.ok = (t === 'stored-body'); tMatch.done = true; }); });");
    pump_until_idle(10);
    ckjs("TOK('match')", "cache.match() returns a Response whose body is the bytes that were put");
    run("var tMiss = TR('match-miss'); settle(tMiss, v1.match('http://fixture.test/nope.txt'), function (v) { return v === undefined; });");
    ckjs("TOK('match-miss')", "cache.match() of an absent URL resolves undefined (not a hang, not a throw)");

    /* Re-matching does NOT consume the stored body -- each call gets its own
     * fresh Response/stream, unlike returning the same object twice. */
    run("var tMatch2 = TR('match-again'); v1.match('http://fixture.test/a.txt').then(function (r) { "
        "r.text().then(function (t) { tMatch2.ok = (t === 'stored-body'); tMatch2.done = true; }); });");
    pump_until_idle(10);
    ckjs("TOK('match-again')", "a SECOND cache.match() for the same URL still returns a full, unconsumed body");

    /* ==== put() refusals: every one is a REJECTED promise, never a throw
     * out of a Promise<T>-typed method and never a hang ==================== */
    run("var tPostReq = TR('put-post'); settle(tPostReq, v1.put(new Request('http://fixture.test/a.txt', { method: 'POST', body: 'x' }), "
        "new Response('y')), function (v, e) { return e instanceof TypeError; });");
    ckjs("TOK('put-post')", "cache.put() of a POST request REJECTS TypeError (spec: only GET)");
    run("var t206 = TR('put-206'); settle(t206, v1.put('http://fixture.test/a.txt', new Response(null, { status: 206 })), "
        "function (v, e) { return e instanceof TypeError; });");
    ckjs("TOK('put-206')", "cache.put() of a 206 response REJECTS TypeError (spec: no partial content)");
    run("var tScheme = TR('put-scheme'); settle(tScheme, v1.put(new Request('data:text/plain,hi'), new Response('y')), "
        "function (v, e) { return e instanceof TypeError; });");
    ckjs("TOK('put-scheme')", "cache.put() of a non-http(s) request REJECTS TypeError");
    run("var tUsed = TR('put-used'); var rr = new Response('z'); rr.text(); "
        "settle(tUsed, v1.put('http://fixture.test/a.txt', rr), function (v, e) { return e instanceof TypeError; });");
    ckjs("TOK('put-used')", "cache.put() of an already-consumed response body REJECTS TypeError");

    /* ==== Vary: two variants under the same URL, selected by a request
     * header -- the property CacheQueryOptions.ignoreVary exists to skip === */
    run("caches.open('vary').then(function (c) {\n"
        "  return Promise.all([\n"
        "    c.put(new Request('http://fixture.test/v.txt', { headers: { 'X-Lang': 'en' } }), "
        "new Response('english', { headers: { Vary: 'X-Lang' } })),\n"
        "    c.put(new Request('http://fixture.test/v.txt', { headers: { 'X-Lang': 'fr' } }), "
        "new Response('francais', { headers: { Vary: 'X-Lang' } }))\n"
        "  ]).then(function () { return c; });\n"
        "}).then(function (c) { globalThis.__vc = c; });");
    js_page_pump();
    run("var tVaryEn = TR('vary-en'); __vc.match(new Request('http://fixture.test/v.txt', { headers: { 'X-Lang': 'en' } }))"
        ".then(function (r) { r.text().then(function (t) { tVaryEn.ok = (t === 'english'); tVaryEn.done = true; }); });");
    pump_until_idle(10);
    ckjs("TOK('vary-en')", "Vary: a request with X-Lang:en matches the English variant");
    run("var tVaryFr = TR('vary-fr'); __vc.match(new Request('http://fixture.test/v.txt', { headers: { 'X-Lang': 'fr' } }))"
        ".then(function (r) { r.text().then(function (t) { tVaryFr.ok = (t === 'francais'); tVaryFr.done = true; }); });");
    pump_until_idle(10);
    ckjs("TOK('vary-fr')", "Vary: a request with X-Lang:fr matches the French variant");
    run("var tVaryDe = TR('vary-de'); settle(tVaryDe, __vc.match(new Request('http://fixture.test/v.txt', { headers: { 'X-Lang': 'de' } })), "
        "function (v) { return v === undefined; });");
    ckjs("TOK('vary-de')", "Vary: a request with an unmatched X-Lang resolves undefined, not a stale variant");

    /* ==== ignoreSearch / ignoreMethod / ignoreVary, honoured for real ===== */
    run("caches.open('opts').then(function (c) { globalThis.__oc = c; return c.put('http://fixture.test/q.txt?x=1', new Response('q')); });");
    js_page_pump();
    run("var tNoIS = TR('no-ignore-search'); settle(tNoIS, __oc.match('http://fixture.test/q.txt'), function (v) { return v === undefined; });");
    ckjs("TOK('no-ignore-search')", "without ignoreSearch, a differing query string does not match");
    run("var tIS = TR('ignore-search'); settle(tIS, __oc.match('http://fixture.test/q.txt', { ignoreSearch: true }), function (v) { return !!v; });");
    ckjs("TOK('ignore-search')", "ignoreSearch:true finds the entry regardless of the query string");
    run("var tNoIM = TR('no-ignore-method'); settle(tNoIM, __oc.match(new Request('http://fixture.test/q.txt?x=1', { method: 'POST' })), function (v) { return v === undefined; });");
    ckjs("TOK('no-ignore-method')", "without ignoreMethod, a POST request does not match a stored GET entry");
    run("var tIM = TR('ignore-method'); settle(tIM, __oc.match(new Request('http://fixture.test/q.txt?x=1', { method: 'POST' }), { ignoreMethod: true }), function (v) { return !!v; });");
    ckjs("TOK('ignore-method')", "ignoreMethod:true finds the entry regardless of request method");

    /* ==== delete() ========================================================= */
    run("var tDel = TR('delete'); settle(tDel, v1.delete('http://fixture.test/a.txt'), function (v) { return v === true; });");
    ckjs("TOK('delete')", "cache.delete() of a present entry resolves true");
    run("var tDelGone = TR('delete-gone'); settle(tDelGone, v1.match('http://fixture.test/a.txt'), function (v) { return v === undefined; });");
    ckjs("TOK('delete-gone')", "after delete(), match() no longer finds the entry");
    run("var tDelAbsent = TR('delete-absent'); settle(tDelAbsent, v1.delete('http://fixture.test/never.txt'), function (v) { return v === false; });");
    ckjs("TOK('delete-absent')", "cache.delete() of an absent entry resolves false (not an error)");

    /* ==== top-level caches.match() searches across named caches =========== */
    run("var tTopMatch = TR('top-match'); settle(tTopMatch, caches.match('http://fixture.test/v.txt', { ignoreVary: true }), function (v) { return !!v; });");
    ckjs("TOK('top-match')", "caches.match() finds an entry in a NAMED cache other than the default");

    /* ==== keys() returns real Request objects ============================= */
    run("var tKR = TR('cache-keys'); settle(tKR, __oc.keys(), function (v) { return v.length === 1 && v[0] instanceof Request && v[0].url === 'http://fixture.test/q.txt?x=1'; });");
    ckjs("TOK('cache-keys')", "cache.keys() returns real Request objects with the stored URL");

    /* ==== add()/addAll(): real fetch, real success/failure/duplicate paths */
    run("caches.open('addall').then(function (c) { globalThis.__ac = c; });");
    js_page_pump();
    run("var tAdd = TR('add'); settle(tAdd, __ac.add('http://fixture.test/a.txt'), function (v) { return v === undefined; });");
    pump_until_idle(10);
    ckjs("TOK('add')", "cache.add() resolves after a real fetch + put");
    run("var tAddCheck = TR('add-check'); __ac.match('http://fixture.test/a.txt').then(function (r) { "
        "r.text().then(function (t) { tAddCheck.ok = (t === 'hello-a'); tAddCheck.done = true; }); });");
    pump_until_idle(10);
    ckjs("TOK('add-check')", "add() actually stored the fetched body, not an empty entry");
    run("var tAdd404 = TR('add-404'); settle(tAdd404, __ac.add('http://fixture.test/missing.txt'), function (v, e) { return e instanceof TypeError; });");
    pump_until_idle(10);
    ckjs("TOK('add-404')", "cache.add() of a 404 REJECTS TypeError, never stores a 404 as if it were a 200");
    run("var tAddDup = TR('addall-dup'); settle(tAddDup, __ac.addAll(['http://fixture.test/a.txt', 'http://fixture.test/a.txt']), "
        "function (v, e) { return e instanceof TypeError; });");
    ckjs("TOK('addall-dup')", "cache.addAll() with a duplicate request REJECTS TypeError synchronously (never even fetches)");

    /* ==== navigator.serviceWorker.register(): every check runs for real,
     * and it ALWAYS settles by rejecting -- this build does not execute a
     * service worker script (see js_swreg.c's header for why) ============= */
    run("var tReg = TR('register'); settle(tReg, navigator.serviceWorker.register('https://fixture.test/sw.js'), "
        "function (v, e) { return e instanceof DOMException && e.name === 'NotSupportedError'; });");
    ckjs("TOK('register')",
         "register() of a well-formed same-origin script URL REJECTS NotSupportedError (validated, then refused -- never a Worker that silently never runs)");
    run("var tRegBad = TR('register-syntax'); settle(tRegBad, navigator.serviceWorker.register('http://[::not a url'), "
        "function (v, e) { return e instanceof DOMException && e.name === 'SyntaxError'; });");
    ckjs("TOK('register-syntax')", "register() of an unparseable script URL REJECTS SyntaxError");
    run("var tRegXO = TR('register-cross-origin'); settle(tRegXO, navigator.serviceWorker.register('http://evil.example/sw.js'), "
        "function (v, e) { return e instanceof DOMException && e.name === 'SecurityError'; });");
    ckjs("TOK('register-cross-origin')", "register() of a cross-origin script URL REJECTS SecurityError");
    run("var tRegScope = TR('register-scope'); settle(tRegScope, navigator.serviceWorker.register('https://fixture.test/deep/sw.js', { scope: '/' }), "
        "function (v, e) { return e instanceof DOMException && e.name === 'SecurityError'; });");
    ckjs("TOK('register-scope')", "register() with a scope broader than the script's own directory REJECTS SecurityError");
    run("var tRegMod = TR('register-module'); settle(tRegMod, navigator.serviceWorker.register('https://fixture.test/sw.js', { type: 'module' }), "
        "function (v, e) { return e instanceof DOMException && e.name === 'NotSupportedError' && e.message.indexOf('module') >= 0; });");
    ckjs("TOK('register-module')", "register() with type:'module' REJECTS NotSupportedError naming module workers, checked BEFORE the generic refusal");

    run("var tReady = TR('ready'); settle(tReady, navigator.serviceWorker.ready, "
        "function (v, e) { return e instanceof DOMException && e.name === 'NotSupportedError'; });");
    ckjs("TOK('ready')",
         "navigator.serviceWorker.ready REJECTS (deliberate spec deviation -- see js_swreg.c: the spec-conformant behaviour is pending forever)");
    run("var tReady2 = TR('ready-memoized'); settle(tReady2, navigator.serviceWorker.ready, function () { return navigator.serviceWorker.ready === navigator.serviceWorker.ready; });");
    ckjs("navigator.serviceWorker.ready === navigator.serviceWorker.ready", "`ready` is memoized (the same rejected promise object every access), not a fresh one per read");

    run("var tGetReg = TR('getRegistration'); settle(tGetReg, navigator.serviceWorker.getRegistration('/'), function (v) { return v === undefined; });");
    ckjs("TOK('getRegistration')", "getRegistration() resolves undefined (nothing is ever registered)");
    run("var tGetRegs = TR('getRegistrations'); settle(tGetRegs, navigator.serviceWorker.getRegistrations(), function (v) { return Array.isArray(v) && v.length === 0; });");
    ckjs("TOK('getRegistrations')", "getRegistrations() resolves an empty array");
    ckjs("navigator.serviceWorker.startMessages() === undefined", "startMessages() is a synchronous no-op (spec: it only un-buffers a controller's messages, and there is never a controller)");

    /* ==== ServiceWorkerRegistration.prototype.update()/unregister(): never
     * reachable from an object this build hands out, but defined so a
     * FUTURE path that does cannot inherit a promise nobody drives ======== */
    run("var tUpd = TR('reg-update'); settle(tUpd, ServiceWorkerRegistration.prototype.update.call(Object.create(ServiceWorkerRegistration.prototype)), "
        "function (v, e) { return e instanceof DOMException && e.name === 'InvalidStateError'; });");
    ckjs("TOK('reg-update')", "ServiceWorkerRegistration.prototype.update() on a shape instance REJECTS InvalidStateError");

#ifdef CACHE_TEST_QUOTA
    /* ==== the hard cap, watched actually firing (JS_CACHE_QUOTA_BYTES=200,
     * compiled into js_cache.c for THIS binary only -- see tests/cache.mk).
     * A 150-byte body fits under 200; a second 100-byte body in the SAME
     * cache does not (150+100 > 200), proving the accounting is a RUNNING
     * TOTAL across puts, not a per-call size check. The promise REJECTS
     * with a real QuotaExceededError DOMException -- settled, never a
     * silently-truncated body and never a pending promise. ================ */
    run("caches.open('quota-test').then(function (c) { globalThis.__qc = c; });");
    js_page_pump();
    run("var tQ1 = TR('quota-under'); settle(tQ1, __qc.put('http://fixture.test/q1.bin', new Response('x'.repeat(150))), "
        "function (v) { return v === undefined; });");
    ckjs("TOK('quota-under')", "a put() under the (test-compiled) 200-byte cap resolves normally");
    run("var tQ2 = TR('quota-over'); settle(tQ2, __qc.put('http://fixture.test/q2.bin', new Response('x'.repeat(100))), "
        "function (v, e) { return e instanceof DOMException && e.name === 'QuotaExceededError'; });");
    ckjs("TOK('quota-over')", "a put() that pushes the running total over the cap REJECTS a real QuotaExceededError");
    run("var tQ2gone = TR('quota-over-not-stored'); settle(tQ2gone, __qc.match('http://fixture.test/q2.bin'), function (v) { return v === undefined; });");
    ckjs("TOK('quota-over-not-stored')", "the rejected put() did not store a truncated/partial entry -- it stored nothing");
#endif

    /* ==== THE ACCEPTANCE CRITERION: every tracker reached a terminal state
     * with the RIGHT outcome, AND the page's own event loop is idle. ======= */
    pump_until_idle(20);
    {
        JSValue stuckv = eval(
            "(function () {"
            "  for (var i = 0; i < W.trackers.length; i++) if (!W.trackers[i].done) return i;"
            "  return -1;"
            "})()");
        int32_t stuck = -1;
        JS_ToInt32(ctx, &stuck, stuckv);
        JS_FreeValue(ctx, stuckv);
        if (stuck >= 0)
            printf("STUCK: W.trackers[%d] (%s) never reached .done after the bounded pump\n", stuck, "?");
        ck(stuck < 0, "quiescence: every promise this run created reached a terminal state (resolved or rejected)");
    }
    {
        JSValue wrongv = eval(
            "(function () {"
            "  for (var i = 0; i < W.trackers.length; i++) if (W.trackers[i].done && !W.trackers[i].ok) return i;"
            "  return -1;"
            "})()");
        int32_t wrong = -1;
        JS_ToInt32(ctx, &wrong, wrongv);
        JS_FreeValue(ctx, wrongv);
        if (wrong >= 0)
            printf("WRONG: W.trackers[%d] terminated but with the wrong outcome\n", wrong);
        ck(wrong < 0, "quiescence: every terminated tracker settled with the RIGHT outcome, not merely SOME outcome");
    }
    ck(js_page_pending() == 0, "quiescence: the page event loop (js_page_pending()) is idle at the end of the run");

    js_page_close();
    dom_free(root);

    printf("\ntest-cache: %d checks, %d failures\n", checks, failures);
    if (failures) { printf("FAILED\n"); return 1; }
    printf("ALL PASS\n");
    return 0;
}
