/* Host unit tests for c/apps/browser/js_platform.c and js_select.c -- the web
 * platform outside the DOM tree and outside the network.
 *
 *     make test-platform          the tests
 *     make test-platform-control  the SAME file against a browser built without
 *                                 either module: every check below must FAIL
 *
 * WHY THE CONTROL EXISTS. Almost everything under test here is installed into
 * the JS runtime by a prelude, and a prelude that silently did nothing would
 * still let this file link, run and print "ok" for anything that a plain
 * QuickJS happens to satisfy. The control build (-DPLATFORM_CONTROL, which
 * links neither js_platform.o nor js_select.o) is how "this assertion fails
 * without the change" is demonstrated rather than asserted: it runs the exact
 * same checks and requires them to fail.
 *
 * The tests run against a REAL parsed document -- html_parse over a fixture
 * string, then js_page_open, which is the same call the browser makes -- so
 * document.readyState, the selector engine and the MutationObserver wrappers
 * are exercised over the same bindings a page gets, not over a mock.
 *
 * WHAT IS NOT ASSERTED, ON PURPOSE. Nothing here checks that a missing global
 * is missing... except the three that MUST stay missing (ActiveXObject,
 * documentMode, crypto.subtle). Those have their own checks, because they are
 * the ones a well-meaning future change would add. `window.indexedDB` used to
 * be a fourth; it is real now (c/apps/browser/js_idb.c). This build still
 * links js_idb.c (PLATFORM_TEST_SRC does not exclude it) but not js_events.c
 * -- and js_idb.c's own install guard checks `typeof G.EventTarget ===
 * 'function'` before doing anything, so `window.indexedDB` stays absent HERE
 * for that reason, not because the file is missing. That is checked below
 * too, so a change that links js_events.c into this build without meaning to
 * (which would silently turn indexedDB on here) fails a test instead of
 * going unnoticed. The real positive suite, with js_events.c linked, is
 * tests/unit/webapi_idb_test.c / `make test-idb`. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include "dom.h"
#include "js_dom.h"
#include "js_page.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
/* WEAK: tests/unit/rust_host_shim.c supplies the same two stubs and is in this
 * link too; which file defines them is somebody else's Makefile line, and a
 * strong definition here breaks the build the moment that line changes. */
__attribute__((__weak__)) void img_register(void *d) { (void)d; }
__attribute__((__weak__)) void img_register_anim(void *a, void *b, void *c)
{ (void)a; (void)b; (void)c; }

/* ---- harness ---------------------------------------------------------- */
static JSContext *ctx;
static int checks, failures, inverted;

/* In the control build every check is expected to FAIL. `inverted` flips the
 * verdict so the control run is a pass/fail test in its own right rather than
 * a wall of red someone has to read. */
static void ck(int cond, const char *name)
{
    checks++;
    int good = inverted ? !cond : cond;
    if (!good) { failures++; printf("FAIL: %s%s\n", name, inverted ? "  (control: should NOT have worked)" : ""); }
    else printf("ok  : %s\n", name);
}

static JSValue eval(const char *src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        if (!inverted) printf("      [js exception] %s\n         while evaluating: %s\n",
                              m ? m : "?", src);
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    }
    return v;
}

static void ckjs(const char *expr, const char *name)
{
    char buf[8192];
    snprintf(buf, sizeof buf, "(function(){ try { return (%s); } catch (e) { return 'threw: ' + e; } })()", expr);
    JSValue v = eval(buf);
    int ok = !JS_IsException(v) && JS_ToBool(ctx, v) && !JS_IsString(v);
    if (!ok && !JS_IsException(v) && !inverted) {
        const char *s = JS_ToCString(ctx, v);
        printf("      value was: %s\n", s ? s : "?");
        if (s) JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    ck(ok, name);
}

/* A check whose substance lives in js_dom.c (or another always-linked TU),
 * present in this file because it was FOUND here -- by running the event loop
 * against real pages -- not because js_platform.c/js_select.c implement it.
 * The control build links those two TUs out and inverts every ckjs(); a check
 * js_dom satisfies passes anyway and used to be counted as a control FAILURE
 * (29 of them by 2026-08-16, growing every time this file learned a DOM
 * fact). In control mode these are SKIPPED, stated per line, because
 * "js_dom still provides this without js_platform" is true, expected, and
 * says nothing about what the control exists to prove. */
static void ckjs_dom(const char *expr, const char *name)
{
    if (inverted) { printf("skip: %s  (js_dom-satisfied; not this control's subject)\n", name); return; }
    ckjs(expr, name);
}

/* Evaluate, then drain the microtask queue -- js_page_eval does this for a
 * page and the tests must see the same world a page does. */
static void run(const char *src) { JS_FreeValue(ctx, eval(src)); js_page_pump(); }

/* The page runtime drains its own microtask queue on eval; this is for the
 * setTimeout-driven parts (MessageChannel, the observers, rIC). */
static unsigned long long g_now;
static unsigned long long clock_fn(void) { return g_now; }
static void tick(int ms) { g_now += (unsigned long long)ms; js_page_run_due(); js_page_pump(); }

static const char *PAGE =
"<!doctype html><html><head><title>t</title></head><body id='b'>"
"<div id='wrap' class='box outer' data-role='panel'>"
"  <p class='lead'>one</p>"
"  <p class='lead special'>two</p>"
"  <span class='lead'>three</span>"
"  <a href='/x' id='lnk'>link</a>"
"</div>"
"<div id='second' class='box'><p class='lead'>four</p></div>"
"<script id='s1'>var ran = 1;</script>"
"</body></html>";

int main(int argc, char **argv)
{
    (void)argc;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--control")) inverted = 1;

    struct node *root = dom_parse(PAGE, (int)strlen(PAGE));
    if (!root) { printf("FAIL: fixture did not parse\n"); return 1; }

    js_page_set_clock(clock_fn);
    js_page_set_location("http://fixture.test/dir/page.html?a=1");
    if (!js_page_open(root)) { printf("FAIL: js_page_open\n"); return 1; }
    ctx = js_page_ctx();

    /* ==== performance ==================================================
     * MEASURED on bing (performance.timing) and bing+wikipedia
     * (performance.mark). The duration check is the one that matters: a
     * mark/measure pair that returns 0 passes a "does it exist" test and fails
     * the page, which compares the number. */
    ckjs("typeof performance.mark === 'function'", "performance.mark exists");
    run("performance.mark('a');");
    run("__t0 = performance.now();");
    tick(40);
    run("performance.mark('b'); __m = performance.measure('span', 'a', 'b');");
    ckjs("__m.entryType === 'measure' && __m.name === 'span'", "measure returns a PerformanceMeasure");
    ckjs("__m.duration >= 39 && __m.duration <= 41", "measure duration is the real elapsed time");
    ckjs("performance.getEntriesByName('a').length === 1", "getEntriesByName finds the mark");
    ckjs("performance.getEntriesByType('mark').length === 2", "getEntriesByType filters");
    ckjs("performance.getEntries().length === 3", "getEntries returns marks and measures");
    run("performance.clearMarks('a');");
    ckjs("performance.getEntriesByName('a').length === 0", "clearMarks removes one by name");
    ckjs("(function(){ try { performance.measure('x','nope'); return false; } catch (e) { return e instanceof SyntaxError; } })()",
         "measure from a missing mark throws SyntaxError");
    /* The legacy interface bing's telemetry bundle dies on. */
    ckjs("typeof performance.timing.navigationStart === 'number' && performance.timing.navigationStart > 0",
         "performance.timing.navigationStart is a real epoch time");
    ckjs("performance.timing.responseEnd >= performance.timing.navigationStart",
         "timing phases are ordered, not zero (a 0 makes every duration negative)");
    ckjs("typeof performance.timeOrigin === 'number'", "performance.timeOrigin");
    ckjs("performance.navigation.type === 0", "performance.navigation");

    /* User Timing L2's "convert a name to a timestamp" is two lookups, not one:
     * a name that is not a user mark is looked up in the PerformanceTiming
     * interface BEFORE it is rejected. MEASURED on stripe.com, which calls
     * performance.measure(x, 'navigationStart') from its Next.js hydration
     * path -- the most common form of the call, because it is how a bundle
     * measures anything against the start of navigation. We threw
     * `SyntaxError: mark 'navigationStart' does not exist`, twice per load.
     *
     * The check above (line ~155, 'nope') is the other half and must keep
     * passing: a name in NEITHER place is still a SyntaxError. Without both,
     * "make markTime never throw" would satisfy this one. */
    ckjs("(function(){ var m = performance.measure('nav', 'navigationStart');"
         "  return m.startTime === 0 && m.duration >= 0; })()",
         "measure from 'navigationStart' resolves via performance.timing");
    ckjs("(function(){ var m = performance.measure('re', 'responseEnd');"
         "  return typeof m.startTime === 'number' && m.startTime >= 0; })()",
         "measure from any PerformanceTiming attribute resolves");
    ckjs("(function(){ try { performance.measure('y','toJSON'); return false; }"
         "  catch (e) { return e instanceof SyntaxError; } })()",
         "a non-numeric timing member is not a timestamp");

    /* ==== PerformanceObserver ===========================================
     * MEASURED IN THE GUEST: bing.com's own inline script constructs one
     * un-guarded (tests/scoreboard/full-corpus/bing.serial.txt) and threw a
     * ReferenceError that took the rest of its IIFE with it -- the
     * indexedDB image-of-the-day fetch, a placeholder cleanup, a 2s
     * fallback timer. These checks are the contract that fix rests on:
     * exactly two entry types delivered for real, everything else a silent
     * no-op rather than a second exception. */
    ckjs("typeof PerformanceObserver === 'function'", "PerformanceObserver exists");
    /* ONE JAR: supportedEntryTypes must be EXACTLY ['mark','measure'] -- not
     * a superset (which would promise entries -- 'resource', 'element',
     * 'longtask', 'navigation' -- this machine never produces, breaking the
     * honest false branch every measured reach already takes) and not a
     * subset (which would take mark/measure observation, the one real
     * consumer, away from a page that checks first). */
    ckjs("(function(){ var t = PerformanceObserver.supportedEntryTypes;"
         "  return Array.isArray(t) && t.length === 2 && t.indexOf('mark') >= 0"
         "      && t.indexOf('measure') >= 0 && t.indexOf('resource') < 0"
         "      && t.indexOf('element') < 0 && t.indexOf('longtask') < 0"
         "      && t.indexOf('navigation') < 0 && t.indexOf('paint') < 0; })()",
         "supportedEntryTypes is exactly ['mark','measure'], not a superset or subset");
    ckjs("Object.isFrozen(PerformanceObserver.supportedEntryTypes)",
         "supportedEntryTypes is frozen (a page cannot widen its own capability check)");
    ckjs("(function(){ try { new PerformanceObserver(); return false; }"
         "  catch (e) { return e instanceof TypeError; } })()",
         "PerformanceObserver requires a callback");

    /* The real consumer: mark+measure in ONE script turn deliver as ONE
     * batched callback carrying both, after the script yields -- not one
     * callback per entry, and not synchronously (a page that reads
     * `po.observe(...)` and immediately checks its own side effect on the
     * next line must still see nothing yet). */
    /* Raw eval, not run() -- run() pumps microtasks itself, which would hide
     * the one thing worth proving here: the callback does not fire inside
     * the same script turn that made the entries. */
    JS_FreeValue(ctx, eval(
        "__po1 = []; __po1n = 0; var po1 = new PerformanceObserver(function(list){"
        "  __po1n++; __po1 = __po1.concat(list.getEntries()); });"
        "po1.observe({ entryTypes: ['mark', 'measure'] });"
        "performance.mark('po-a'); performance.mark('po-b');"
        "__m1 = performance.measure('po-span', 'po-a', 'po-b');"));
    ckjs("__po1.length === 0", "the observer callback has not fired synchronously");
    js_page_pump();
    ckjs("__po1n === 1", "three entries made in one turn deliver as ONE callback, not three");
    ckjs("__po1.length === 3", "the batched callback carries all three entries");
    ckjs("__po1.indexOf(__m1) >= 0", "the delivered list is the SAME entries getEntries() returns");
    /* length > 0 first -- an empty array would make the loop vacuously true
     * and pass this check in a build with no PerformanceObserver at all. */
    ckjs("(function(){ if (__po1.length !== 3) return false;"
         "  for (var i = 0; i < __po1.length; i++)"
         "    if (!(__po1[i] instanceof PerformanceEntry)) return false; return true; })()",
         "delivered entries are instanceof PerformanceEntry");

    /* Naming an unsupported type is a SILENT NO-OP, not a throw -- this is
     * the exact bing.com line: an un-guarded observe({type:'element',...})
     * must not trade one ReferenceError for a TypeError. */
    run("__po2fired = false; var po2 = new PerformanceObserver(function(){ __po2fired = true; });"
        "po2.observe({ type: 'element', buffered: true });"
        "performance.mark('po-unsupported');");
    js_page_pump();
    ckjs("__po2fired === false",
         "observe() of an unsupported type never fires (bing.com's exact un-guarded call)");

    /* buffered:true replays entries that already existed before observe(). */
    run("performance.mark('po-early');"
        "__po3 = []; var po3 = new PerformanceObserver(function(list){ __po3 = __po3.concat(list.getEntries()); });"
        "po3.observe({ type: 'mark', buffered: true });");
    js_page_pump();
    ckjs("(function(){ for (var i = 0; i < __po3.length; i++) if (__po3[i].name === 'po-early') return true; return false; })()",
         "buffered:true replays a mark made before observe() was called");

    /* disconnect() stops future delivery; takeRecords() drains what is
     * pending without waiting for the callback turn. */
    run("__po4 = []; var po4 = new PerformanceObserver(function(list){ __po4 = __po4.concat(list.getEntries()); });"
        "po4.observe({ type: 'mark' });"
        "performance.mark('po-before-disconnect');"
        "po4.disconnect();"
        "performance.mark('po-after-disconnect');");
    js_page_pump();
    ckjs("__po4.length === 0", "disconnect() removes the observer before its callback ever ran");
    run("__po5rec = null; var po5 = new PerformanceObserver(function(){});"
        "po5.observe({ type: 'mark' }); performance.mark('po-taken');"
        "__po5rec = po5.takeRecords();");
    ckjs("__po5rec.length === 1 && __po5rec[0].name === 'po-taken'",
         "takeRecords drains pending entries synchronously, without a callback turn");
    js_page_pump();
    ckjs("po5.takeRecords().length === 0",
         "takeRecords clears what it returned -- the second call is empty");

    /* canvas.getContext is a REAL context now (js_canvas.c), which this build
     * does not link -- so the right assertion here is not "absent" and not
     * "present", it is that nothing in THIS file quietly provides one. A
     * getContext appearing without js_canvas.c on the link line would be a
     * stub, and a stub is what the corpus evidence in js_platform.c refuses.
     * The real surface is gated by make test-canvas, 46 checks against read-back
     * pixels. */
    ckjs("typeof document.createElement('canvas').getContext === 'undefined'",
         "js_platform.c provides no getContext of its own (js_canvas.c owns it)");

    /* WHERE Node's methods actually reach, asked because the callee-naming
     * instrument returned `appendChild is not a function (it is undefined)`
     * from 2345.com's Vue runtime -- and "it is undefined" says the object
     * exists and its chain does not carry the method, which is a different
     * bug from a null receiver and points at one of the objects below.
     * doc_funcs (js_dom.c) lists createElement and querySelector and NOT
     * appendChild, so whether the document has it at all is a question about
     * the interface hierarchy rather than about that table. */
    ckjs_dom("typeof document.appendChild === 'function'",
             "the document inherits Node.appendChild");
    ckjs_dom("typeof document.createDocumentFragment().appendChild === 'function'",
             "a DocumentFragment inherits Node.appendChild");
    ckjs_dom("typeof document.head.appendChild === 'function'",
             "document.head has appendChild");
    ckjs_dom("typeof document.documentElement.appendChild === 'function'",
             "documentElement has appendChild");

    /* ==== document lifecycle ============================================
     * MEASURED on bing and deepseek -- the most-wanted property in the corpus.
     * It must be 'loading' while script runs, or a page skips its own
     * DOMContentLoaded registration and never initialises. */
    ckjs("document.readyState === 'loading'", "readyState is 'loading' during script");
    run("__rs = []; document.addEventListener('readystatechange', function(){ __rs.push(document.readyState); });");
    {
        struct js_event_init li;
        memset(&li, 0, sizeof li);
        li.bubbles = 1;
        js_dom_dispatch(js_dom_root(), "DOMContentLoaded", &li);
    }
    ckjs("document.readyState === 'interactive'", "DOMContentLoaded moves readyState to 'interactive'");
    {
        struct js_event_init li;
        memset(&li, 0, sizeof li);
        js_dom_dispatch(js_dom_root(), "load", &li);
    }
    ckjs("document.readyState === 'complete'", "load moves readyState to 'complete'");
    ckjs("__rs.length === 2 && __rs[0] === 'interactive' && __rs[1] === 'complete'",
         "readystatechange fires once per transition, in order");
    ckjs("performance.timing.domContentLoadedEventEnd >= performance.timing.navigationStart",
         "the lifecycle fills in Navigation Timing as it happens");
    ckjs("document.visibilityState === 'visible' && document.hidden === false", "visibilityState");

    /* ==== task queues ==================================================== */
    ckjs("typeof queueMicrotask === 'function'", "queueMicrotask exists");
    JS_FreeValue(ctx, eval("__mt = []; queueMicrotask(function(){ __mt.push('micro'); }); __mt.push('sync');"));
    ckjs("__mt.length === 1", "the microtask has NOT run while the script is still running");
    js_page_pump();
    ckjs("__mt.length === 2 && __mt[0] === 'sync' && __mt[1] === 'micro'",
         "queueMicrotask runs AFTER the current script, not during it");
    ckjs("typeof reportError === 'function'", "reportError exists");

    ckjs("typeof MessageChannel === 'function'", "MessageChannel exists");
    run("__mc = new MessageChannel(); __got = []; __mc.port2.onmessage = function(e){ __got.push(e.data); };"
        "__mc.port1.postMessage('one'); __mc.port1.postMessage('two');");
    ckjs("__got.length === 0", "MessagePort delivery is asynchronous");
    tick(1);
    ckjs("__got.length === 2 && __got[0] === 'one' && __got[1] === 'two'",
         "MessagePort delivers in order (React's scheduler depends on FIFO)");
    /* A port that only used addEventListener stays silent until start(). Code
     * that forgets start() must behave here as it does in a browser. */
    run("__mc2 = new MessageChannel(); __g2 = []; __mc2.port2.addEventListener('message', function(e){ __g2.push(e.data); });"
        "__mc2.port1.postMessage('held');");
    tick(1);
    ckjs("__g2.length === 0", "a port with only addEventListener is not started");
    run("__mc2.port2.start();");
    tick(1);
    ckjs("__g2.length === 1 && __g2[0] === 'held'", "start() flushes what was held");

    run("__pm = null; window.onmessage = function(e){ __pm = e; }; postMessage({n:7}, '*');");
    tick(1);
    ckjs("__pm && __pm.data.n === 7 && __pm.type === 'message'", "window.postMessage to self");

    /* targetOrigin honoured for real, even though there is only one window to
     * check it against -- see the comment over the definition in
     * js_platform.c. This is the half of the cross-window contract that is
     * checkable WITHOUT a second browsing context, and it used to be a
     * silently-ignored second argument. */
    run("__pm2 = null; window.onmessage = function(e){ __pm2 = e; }; postMessage({n:8}, 'http://fixture.test');");
    tick(1);
    ckjs("__pm2 && __pm2.data.n === 8", "postMessage delivers when targetOrigin matches self.origin exactly");

    /* THREW3 IS PART OF THE ASSERTION, NOT DECORATION: "__pm3 stays untouched"
     * is also exactly what happens in the CONTROL build, where postMessage
     * does not exist at all and the call throws before ever reaching a
     * delivery decision -- a check of __pm3 alone cannot tell "correctly
     * refused" from "is not there", which is the rule-3 trap this file's own
     * project exists to avoid. Requiring !__threw3 closes it: the control
     * build throws (postMessage is not a function), so this check now FAILS
     * there as it must, and only a build that both accepts the call AND
     * declines to deliver passes. */
    run("__pm3 = 'untouched'; __threw3 = false; window.onmessage = function(e){ __pm3 = e; };"
        "try { postMessage({n:9}, 'https://evil.example'); } catch (e) { __threw3 = true; }");
    tick(5);
    ckjs("!__threw3 && __pm3 === 'untouched'",
         "postMessage with a mismatched targetOrigin does not throw but is silently NOT delivered "
         "(negative control -- watch this NOT fire)");

    /* THIS CHECK USED TO ASSERT THE DEFECT, AND THAT IS THE INTERESTING PART.
     * It read
     *
     *   ckjs("__errName === 'TypeError'",
     *        "postMessage() with no targetOrigin throws TypeError ...");
     *
     * which is what you write when the expectation is a hand-written belief
     * rather than a reference. There are TWO overloads and only the legacy one
     * requires the second argument:
     *
     *   undefined postMessage(any message, USVString targetOrigin,
     *                         optional sequence<object> transfer = []);
     *   undefined postMessage(any message,
     *                         optional WindowPostMessageOptions options = {});
     *
     * With one argument, Web IDL picks the second and `options.targetOrigin`
     * defaults to '/' -- same origin -- so `postMessage(m)` DELIVERS in every
     * current browser. The gate was green on a behaviour no browser has, which
     * is the exact shape of "a gate that records what its author already
     * believed". What broke it in the field: google.com/search, measured in the
     * guest 2026-08-30, whose only JS exception was the SyntaxError from the
     * OTHER unimplemented half of the same overload set.
     *
     * Delivery, not merely "does not throw": a check that only asserted the
     * absence of an exception would also pass in a build where postMessage was
     * a no-op. */
    run("__errName = null; __pm7 = 'untouched'; window.onmessage = function(e){ __pm7 = e; };"
        "try { postMessage({n:11}); } catch (e) { __errName = e.name; }");
    tick(1);
    ckjs("__errName === null && __pm7 !== 'untouched' && __pm7.data.n === 11",
         "postMessage(message) -- the one-argument overload -- delivers with targetOrigin '/'");

    /* The options-dictionary overload, both ways round: a matching origin
     * delivers and a mismatched one is silently dropped, exactly as the string
     * form is tested above. Two checks rather than one because a build that
     * ignored `options` entirely and defaulted to '*' would pass the first. */
    run("__pm8 = 'untouched'; __errName8 = null; window.onmessage = function(e){ __pm8 = e; };"
        "try { postMessage({n:12}, {targetOrigin: '*'}); } catch (e) { __errName8 = e.name; }");
    tick(1);
    ckjs("__errName8 === null && __pm8 !== 'untouched' && __pm8.data.n === 12",
         "postMessage(message, {targetOrigin:'*'}) delivers (was: SyntaxError on '[object Object]')");

    run("__pm9 = 'untouched'; __errName9 = null; window.onmessage = function(e){ __pm9 = e; };"
        "try { postMessage({n:13}, {targetOrigin: 'https://evil.example'}); } catch (e) { __errName9 = e.name; }");
    tick(5);
    ckjs("__errName9 === null && __pm9 === 'untouched'",
         "a mismatched targetOrigin inside the options dictionary is honoured, not ignored");

    /* null and undefined are the dictionary overload too (Web IDL: a nullish
     * distinguishing argument selects the dictionary), so they deliver rather
     * than stringifying to the literal origins 'null' and 'undefined' -- which
     * is what String(targetOrigin) did and is a SyntaxError. */
    run("__pm10 = 'untouched'; window.onmessage = function(e){ __pm10 = e; };"
        "postMessage({n:14}, null);");
    tick(1);
    ckjs("__pm10 !== 'untouched' && __pm10.data.n === 14",
         "postMessage(message, null) selects the options overload and delivers");

    /* The zero-argument case still throws, and the MESSAGE is part of the
     * assertion for the reason every other check here gives: in the control
     * build `postMessage` does not exist, so calling it is also a TypeError,
     * and a check on the name alone would pass there -- a control that cannot
     * be watched failing. Only a build that HAS the function and rejects the
     * empty call for the right reason says "1 argument required". */
    run("__errMsg11 = null; try { postMessage(); } catch (e) { __errMsg11 = String(e.message); }");
    ckjs("__errMsg11 !== null && __errMsg11.indexOf('1 argument required') >= 0",
         "postMessage() with NO arguments still throws TypeError naming the arity");

    run("__errName2 = null; __pm4 = 'untouched'; window.onmessage = function(e){ __pm4 = e; };"
        "try { postMessage({n:1}, 'not a valid origin'); } catch (e) { __errName2 = e.name; }");
    tick(1);
    ckjs("__errName2 === 'SyntaxError'", "postMessage with a malformed targetOrigin throws SyntaxError");
    /* Same trap as __pm3 above, closed the same way: "__pm4 untouched" alone
     * is also true of a control build where postMessage does not exist (a
     * TypeError, not a SyntaxError). Requiring the SyntaxError alongside it
     * means this check can only pass in a build that threw the RIGHT error
     * for the RIGHT reason and still delivered nothing. */
    ckjs("__errName2 === 'SyntaxError' && __pm4 === 'untouched'", "a malformed targetOrigin never queues a delivery");

    /* The clone is real: mutating the object AFTER the call must not reach
     * the delivered event, and it must not reach it because a NEW object was
     * made at call time -- not because delivery happened to run first. */
    run("__src = {n:5}; window.onmessage = function(e){ __pm5 = e; }; postMessage(__src, '*'); __src.n = 999;");
    tick(1);
    ckjs("__pm5.data.n === 5 && __pm5.data !== __src",
         "postMessage clones data at call time (later mutation of the source object does not leak through)");

    run("__errName3 = null; __pm6 = 'untouched'; window.onmessage = function(e){ __pm6 = e; };"
        "try { postMessage(function(){}, '*'); } catch (e) { __errName3 = e.name; }");
    tick(1);
    ckjs("__errName3 === 'DataCloneError'", "postMessage with a function value throws DataCloneError synchronously");
    /* Same trap again, same fix: gate on the specific error name so a control
     * build (postMessage undefined -> TypeError, not DataCloneError) fails
     * this check instead of passing it by accident. */
    ckjs("__errName3 === 'DataCloneError' && __pm6 === 'untouched'", "a clone failure never queues a delivery");

    ckjs("typeof requestIdleCallback === 'function'", "requestIdleCallback exists");
    run("__idle = null; requestIdleCallback(function(d){ __idle = d; });");
    tick(2);
    ckjs("__idle && typeof __idle.timeRemaining === 'function'", "rIC delivers a deadline");
    /* A constant timeRemaining() would hang any page that loops on it. */
    run("__tr0 = __idle.timeRemaining();");
    tick(60);
    ckjs("__idle.timeRemaining() < __tr0 && __idle.timeRemaining() === 0",
         "the idle deadline actually counts down to zero");

    /* ==== errors ========================================================= */
    ckjs_dom("typeof DOMException === 'function'", "DOMException exists");
    ckjs_dom("(function(){ var e = new DOMException('no', 'AbortError');"
         " return e instanceof Error && e.name === 'AbortError' && e.code === 20 && e.message === 'no'; })()",
         "DOMException carries name, message and the legacy code");
    ckjs("typeof PromiseRejectionEvent === 'function'", "PromiseRejectionEvent exists");
    run("__rej = null; window.onunhandledrejection = function(e){ __rej = e; e.preventDefault(); };"
        "Promise.reject(new Error('boom'));");
    tick(1);
    ckjs("__rej && String(__rej.reason).indexOf('boom') >= 0",
         "an unhandled rejection reaches window.onunhandledrejection");

    /* ==== Storage named properties ======================================
     * MEASURED on bing: localStorage.eventLogQueue_Offline. */
    run("localStorage.clear(); localStorage.setItem('a', '1'); localStorage.q = 'named';");
    ckjs("localStorage.a === '1'", "localStorage.<name> reads through getItem");
    ckjs("localStorage.getItem('q') === 'named'", "localStorage.<name> = v writes through setItem");
    ckjs("localStorage.length === 2", "named writes count towards length");
    /* Every check below reads a key that was written through setItem, so a
     * build without the wrapper -- where `localStorage.q = v` merely made a JS
     * property -- answers differently. A check that passed either way would
     * make the control meaningless. */
    ckjs("('a' in localStorage) && !('zz' in localStorage)",
         "`in` consults the store, not just own properties");
    ckjs("localStorage.a === '1' && localStorage.zz === undefined",
         "a name written through setItem reads back; a missing one is undefined");
    run("delete localStorage.a;");
    ckjs("localStorage.getItem('a') === null && localStorage.length === 1",
         "delete localStorage.<name> removes it from the store");
    ckjs("typeof localStorage.setItem === 'function' && localStorage.getItem('q') === 'named'",
         "the real methods still work through the wrapper");

    /* ==== navigator / window ============================================ */
    ckjs("navigator.scheduling.isInputPending() === false", "navigator.scheduling.isInputPending");
    ckjs_dom("window.top === window && window.parent === window",
         "an unframed document is its own top (the frame-buster line)");

    /* ==== crypto ========================================================= */
    ckjs("typeof crypto.getRandomValues === 'function'", "crypto.getRandomValues exists");
    ckjs("(function(){ var a = new Uint8Array(32); crypto.getRandomValues(a);"
         " var b = new Uint8Array(32); crypto.getRandomValues(b);"
         " var same = 0; for (var i = 0; i < 32; i++) if (a[i] === b[i]) same++;"
         " var zero = 0; for (var j = 0; j < 32; j++) if (a[j] === 0) zero++;"
         " return same < 24 && zero < 8; })()",
         "getRandomValues fills with bytes that differ between calls");
    ckjs("crypto.getRandomValues(new Uint8Array(4)) instanceof Uint8Array", "it returns the view");
    ckjs("/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(crypto.randomUUID())",
         "randomUUID is a well-formed v4 UUID");

    /* ==== structuredClone =============================================== */
    ckjs("typeof structuredClone === 'function'", "structuredClone exists");
    ckjs("(function(){ var o = { d: new Date(5), m: new Map([['k',[1,2]]]), s: new Set([3]),"
         "                       u: new Uint8Array([1,2,3]) }; o.self = o;"
         " var c = structuredClone(o);"
         " return c !== o && c.self === c && c.d instanceof Date && c.d.getTime() === 5"
         "     && c.m.get('k')[1] === 2 && c.m.get('k') !== o.m.get('k')"
         "     && c.s.has(3) && c.u[2] === 3; })()",
         "structuredClone handles Date/Map/Set/TypedArray and cycles");
    ckjs("(function(){ try { structuredClone({ f: function(){} }); return false; }"
         " catch (e) { return e.name === 'DataCloneError'; } })()",
         "cloning a function throws DataCloneError, as the spec says");

    /* ==== FormData ========================================================
     * Still this file's own: FormData (unlike Blob, below) has not moved. */
    ckjs("(function(){ var f = new FormData(); f.append('a','1'); f.append('a','2'); f.append('b','3');"
         " return f.getAll('a').length === 2 && f.get('a') === '1'"
         "     && Array.from(f.keys()).join(',') === 'a,a,b'; })()",
         "FormData keeps duplicate names in insertion order");
    ckjs("(function(){ var f = new FormData(); f.append('a','1'); f.append('a','2'); f.set('a','9');"
         " return f.getAll('a').length === 1 && f.get('a') === '9'; })()",
         "FormData.set collapses duplicates");
    /* FormData needs js_platform.c (this check stays inverted, unlike the
     * block below): what it proves is that the Blob FormData sees is the REAL
     * class from js_webapi.c, not some Blob-shaped object of its own -- so it
     * has to run where FormData exists, which is only the positive build. */
    ckjs("(function(){ var f = new FormData(); var b = new Blob(['x']); f.append('file', b);"
         " return f.get('file') === b && b instanceof Blob; })()",
         "FormData.append keeps a Blob a Blob (js_platform.c sees js_webapi.c's real class)");

    /* ==== Blob / createObjectURL =========================================
     * NOT inverted, on purpose, unlike the rest of this section: Blob and
     * URL.createObjectURL/revokeObjectURL moved to js_webapi.c (own tests in
     * tests/unit/webapi_test.c, which also covers the object-URL table's
     * bound), and js_webapi.c is one of the files the control build still
     * links (see the file header). A check here that stayed inverted would
     * FAIL in test-platform-control the moment js_platform.c's `if (!G.Blob)`
     * fallback stopped mattering -- which is exactly what moving Blob here
     * was for. What is still worth asserting from THIS file is that the
     * pieces js_platform.c hands Blob (FormData.append/.set, just above) see
     * the real class, not the fallback: `v instanceof G.Blob` has to be true
     * for the same object regardless of which file happened to define it. */
    { int save = inverted; inverted = 0;
    ckjs("new Blob(['abc']).size === 3 && new Blob([new Uint8Array([1,2])]).size === 2",
         "Blob sizes are BYTES, not characters (js_webapi.c, always linked)");
    ckjs("new Blob(['\\u00e9']).size === 2", "a two-byte character counts as two bytes");
    ckjs("new Blob(['hello'], {type:'text/plain'}).type === 'text/plain'", "Blob type");
    run("__bt = null; new Blob(['hi']).text().then(function(t){ __bt = t; });");
    tick(1);
    ckjs("__bt === 'hi'", "Blob.text() round-trips");
    ckjs("/^blob:/.test(URL.createObjectURL(new Blob(['hi'], {type:'text/plain'})))",
         "createObjectURL produces a real blob: URL, not js_platform.c's data: fallback");
    /* ... and it really resolves. This is the half of that claim that would
     * otherwise be a comment: fetch() of the URL createObjectURL just returned
     * must resolve to the bytes that went in, with no socket involved. */
    run("__d1 = null; var __u = URL.createObjectURL(new Blob(['hi'], {type:'text/plain'}));"
        "fetch(__u)"
        "  .then(function(r){ __d1 = [r.status, r.headers.get('content-type')]; return r.text(); })"
        "  .then(function(t){ __d1.push(t); URL.revokeObjectURL(__u);"
        "    fetch(__u).then(function(){ __d1.push('resolved'); }, function(e){ __d1.push(e.name); }); });");
    tick(1);
    ckjs("__d1 && __d1[0] === 200 && __d1[1] === 'text/plain' && __d1[2] === 'hi'",
         "fetch() of a createObjectURL blob: URL resolves to the original bytes");
    ckjs("__d1 && __d1[3] === 'TypeError'",
         "...and after revokeObjectURL, the same URL no longer resolves");
    inverted = save; }
    /* The bound on the object-URL table (64 entries / 8 MiB, refused loudly
     * past it) is asserted once, in tests/unit/webapi_test.c, which is where
     * it is implemented; repeating it here would test js_webapi.c through a
     * second, heavier harness for no second fact. */

    /* ==== observers ====================================================== */
    ckjs("typeof IntersectionObserver === 'function' && typeof ResizeObserver === 'function'"
         " && typeof MutationObserver === 'function'", "the three observers exist");
    run("__io = []; var io = new IntersectionObserver(function(rs){ __io = __io.concat(rs); });"
        "io.observe(document.getElementById('wrap'));");
    tick(1);
    ckjs("__io.length === 1 && __io[0].target.id === 'wrap' && typeof __io[0].intersectionRatio === 'number'",
         "IntersectionObserver delivers an initial entry for its target");
    run("__mu = []; var mo = new MutationObserver(function(rs){ __mu = __mu.concat(rs); });"
        "mo.observe(document.getElementById('wrap'), { childList: true, attributes: true, subtree: true });"
        "var d = document.createElement('em'); document.getElementById('wrap').appendChild(d);"
        "document.getElementById('lnk').setAttribute('href', '/y');");
    tick(1);
    ckjs("__mu.length === 2", "MutationObserver batches both mutations into one callback");
    ckjs("__mu[0].type === 'childList' && __mu[0].addedNodes.length === 1", "childList record");
    ckjs("__mu[1].type === 'attributes' && __mu[1].attributeName === 'href' && __mu[1].oldValue === '/x'",
         "attributes record carries the old value");
    run("__mu2 = []; var mo2 = new MutationObserver(function(rs){ __mu2 = __mu2.concat(rs); });"
        "mo2.observe(document.getElementById('second'), { childList: true });"
        "document.getElementById('wrap').appendChild(document.createElement('i'));");
    tick(1);
    /* mo still watches #wrap and so picks up this third mutation; mo2 watches
     * #second and must see none of it. Asserting BOTH halves is what makes the
     * check discriminating -- "mo2 saw nothing" is also true of a build with no
     * MutationObserver at all. */
    ckjs("__mu2.length === 0 && __mu.length === 3",
         "an observer sees only its own subtree (and the other one did see this)");

    /* ==== selectors (js_select.c) ======================================
     * MEASURED on bing: document.querySelectorAll and getElementsByTagName,
     * two references each, and they were the last two things killing its
     * scripts once performance and the lifecycle were filled in. */
    ckjs("typeof document.querySelectorAll === 'function'", "document.querySelectorAll exists");
    ckjs("document.querySelectorAll('p.lead').length === 3", "type + class");
    ckjs("document.querySelectorAll('.lead').length === 4", "class alone matches the span too");
    ckjs("document.querySelectorAll('#wrap .lead').length === 3", "descendant combinator");
    ckjs("document.querySelectorAll('#wrap > p').length === 2", "child combinator");
    ckjs("document.querySelectorAll('p.lead + p').length === 1", "adjacent sibling");
    ckjs("document.querySelectorAll('p.lead ~ span').length === 1", "general sibling");
    ckjs("document.querySelectorAll('p, span').length === 4", "selector list");
    ckjs("document.querySelectorAll('[data-role]').length === 1", "attribute presence");
    ckjs("document.querySelectorAll('[data-role=\"panel\"]').length === 1", "attribute equality");
    ckjs("document.querySelectorAll('[href^=\"/\"]').length === 1", "attribute prefix");
    ckjs("document.querySelectorAll('p.lead:not(.special)').length === 2", ":not()");
    ckjs("document.querySelectorAll('div.box').length === 2", "compound type + class");
    ckjs("document.getElementById('wrap').querySelector('p.lead').textContent.indexOf('one') >= 0",
         "querySelector takes the first match in document order");
    /* A pseudo-class we cannot evaluate matches NOTHING. Matching everything
     * would style or click elements the page never selected. */
    ckjs("document.querySelectorAll('p:hover').length === 0", "an unevaluable pseudo-class matches nothing");
    /* An unparsable selector must throw, not return []. An empty result is
     * indistinguishable from "no matches" and hides the bug for a month. */
    /* `e.name`, NOT `instanceof SyntaxError`: per spec (and per Chrome) an
     * invalid selector throws a DOMException whose .name is 'SyntaxError',
     * which is NOT an instance of the SyntaxError constructor. The first
     * version of this check asserted instanceof and started failing the day
     * js_select.c became spec-correct -- the test was wrong, not the code
     * (this is what WPT's assert_throws_dom(SYNTAX_ERR, ...) checks too). */
    ckjs("(function(){ try { document.querySelectorAll('<<'); return false; } catch (e) { return e.name === 'SyntaxError'; } })()",
         "a bad selector throws SyntaxError rather than returning nothing");
    ckjs("document.getElementsByTagName('p').length === 3", "getElementsByTagName");
    ckjs("document.getElementsByTagName('*').length > 8", "getElementsByTagName('*')");
    ckjs("document.getElementsByClassName('lead special').length === 1", "getElementsByClassName is AND");
    ckjs("document.getElementById('wrap').querySelectorAll('p').length === 2",
         "element.querySelectorAll is scoped to descendants");
    ckjs("document.getElementById('wrap').getElementsByTagName('p').length === 2",
         "element.getElementsByTagName is scoped");
    ckjs("document.getElementById('lnk').matches('a[href]')", "Element.matches");
    ckjs("!document.getElementById('lnk').matches('p')", "Element.matches says no");
    ckjs("document.getElementById('lnk').closest('.box').id === 'wrap'", "Element.closest walks up");
    ckjs("document.scripts.length === 1 && document.links.length === 1", "document.scripts / links");
    ckjs("typeof document.querySelectorAll('p').item === 'function'"
         " && document.querySelectorAll('p').item(9) === null", "the result is collection-shaped");
    ckjs("Array.from(document.querySelectorAll('p')).length === 3", "the result is iterable");

    /* ==== base64 =========================================================
     * MEASURED ONLY ON THE MACHINE. btoa is nowhere in the host probe's table,
     * because bing reaches it from a setTimeout callback and the probe used to
     * stop after evaluating scripts. tests/qmp/qmp_bing.py found it in the
     * serial log; the probe now runs its event loop because of it. */
    ckjs("btoa('hello') === 'aGVsbG8='", "btoa");
    ckjs("btoa('a') === 'YQ==' && btoa('ab') === 'YWI=' && btoa('') === ''",
         "btoa pads both partial-group lengths");
    ckjs("atob('aGVsbG8=') === 'hello' && atob('YQ==') === 'a'", "atob round-trips");
    /* The Latin-1 restriction is the behaviour every
     * `btoa(unescape(encodeURIComponent(s)))` idiom is written around; silently
     * UTF-8 encoding instead makes those idioms double-encode. */
    ckjs("(function(){ try { btoa('\\u00e9\\u4e2d'); return false; }"
         " catch (e) { return e.name === 'InvalidCharacterError'; } })()",
         "btoa throws InvalidCharacterError above Latin-1, it does not UTF-8 encode");
    ckjs("btoa('\\u00e9') === 'w6k=' ? false : btoa('\\u00e9') === '6Q=='",
         "a Latin-1 character encodes as ONE byte");
    ckjs("(function(){ try { atob('a'); return false; } catch (e) { return e.name === 'InvalidCharacterError'; } })()",
         "atob rejects a length that cannot be base64");

    /* ==== element constructors ==========================================
     * MEASURED ONLY ON THE MACHINE, same story as btoa: `new Image()` is
     * bing's beacon, reached from a timer. */
    ckjs("new Image() && new Image().tagName.toLowerCase() === 'img'",
         "new Image() constructs an <img> element");
    ckjs("new Image(12, 34).getAttribute('width') === '12'", "...with its size attributes");
    ckjs("new Option('label', 'v').getAttribute('value') === 'v'", "new Option()");

    /* ==== Intl ===========================================================
     * MEASURED on deepseek -- and only after MessageChannel let React's
     * scheduler start, which is what got the app as far as formatting a
     * number. It is English-only by construction (there is no locale data in
     * this system); resolvedOptions is asserted to SAY so rather than to
     * pretend otherwise. */
    ckjs("typeof Intl === 'object' && typeof Intl.NumberFormat === 'function'", "Intl exists");
    ckjs("new Intl.NumberFormat().format(1234567.891) === '1,234,567.891'",
         "NumberFormat groups thousands");
    ckjs("new Intl.NumberFormat('en', {minimumFractionDigits:2}).format(5) === '5.00'",
         "minimumFractionDigits pads");
    ckjs("new Intl.NumberFormat('en', {maximumFractionDigits:2}).format(1.005) === '1'"
         " || new Intl.NumberFormat('en', {maximumFractionDigits:2}).format(1.005) === '1.01'",
         "maximumFractionDigits rounds");
    ckjs("new Intl.NumberFormat('en', {style:'percent'}).format(0.42) === '42%'", "percent style");
    ckjs("new Intl.NumberFormat('en', {style:'currency',currency:'USD'}).format(3.5) === '$3.50'",
         "currency style");
    ckjs("new Intl.NumberFormat('en', {useGrouping:false}).format(1234) === '1234'",
         "useGrouping:false");
    ckjs("new Intl.NumberFormat().format(-1234.5) === '-1,234.5'", "negatives keep their sign");
    ckjs("(1234.5).toLocaleString() === '1,234.5'",
         "Number.prototype.toLocaleString routes through the same formatter");
    run("__dt = new Intl.DateTimeFormat('en', {year:'numeric',month:'long',day:'numeric'})"
        "        .format(new Date(2026, 7, 8));");
    ckjs("__dt === 'August 8 2026' || __dt === 'August 8, 2026'",
         "DateTimeFormat renders the month by name");
    ckjs("new Intl.PluralRules().select(1) === 'one' && new Intl.PluralRules().select(2) === 'other'",
         "PluralRules, English cardinal");
    ckjs("new Intl.PluralRules('en',{type:'ordinal'}).select(21) === 'one'"
         " && new Intl.PluralRules('en',{type:'ordinal'}).select(11) === 'other'",
         "...and the ordinal 21st/11th rule");
    ckjs("new Intl.ListFormat().format(['a','b','c']) === 'a, b, and c'", "ListFormat");
    ckjs("new Intl.Collator().compare('a','b') < 0 && new Intl.Collator().compare('b','a') > 0",
         "Collator compares");
    /* The honesty check: a page that asks for Japanese is TOLD it got en-US. */
    ckjs("new Intl.NumberFormat('ja-JP').resolvedOptions().locale === 'en-US'",
         "resolvedOptions reports the locale it actually used, not the one asked for");
    ckjs("typeof SuppressedError === 'function' && new SuppressedError(1,2,'m').name === 'SuppressedError'",
         "SuppressedError (ES2024; bundlers feature-test it at module top level)");

    /* ==== document gaps found by running the event loop ================== */
    ckjs_dom("document.ownerDocument === null", "document.ownerDocument is null, not undefined");
    ckjs_dom("document.getRootNode() === document", "document.getRootNode()");
    ckjs_dom("document.getElementById('wrap').getRootNode() === document", "Element.getRootNode()");
    /* The fixture opens with <!doctype html>, so per spec document.firstChild
     * is the DocumentType node (nodeType 10), NOT documentElement -- the
     * original `=== documentElement` expectation was written before the tree
     * grew real doctype nodes and became wrong the day it did. Both halves
     * asserted: the doctype leads, and <html> follows it. */
    ckjs_dom("document.firstChild.nodeType === 10", "document.firstChild is the doctype");
    ckjs_dom("document.firstChild.nextSibling === document.documentElement",
         "documentElement follows the doctype");
    ckjs_dom("document.contains(document.getElementById('wrap'))", "document.contains");

    /* ==== dataset =======================================================
     * Top of the Chrome differential: deepseek's React reads
     * `link.dataset.precedence` in its commit phase and MDN writes
     * `documentElement.dataset.theme` in its first inline script. Chrome
     * throws on neither. Read AND write AND delete are asserted separately
     * because a getter-only shim satisfies deepseek and silently loses MDN's
     * write, which is the failure that produced no exception at all. */
    run("var __d = document.getElementById('wrap');"
         "__d.setAttribute('data-precedence', 'high');"
         "__d.setAttribute('data-two-words', 'x');");
    ckjs("__d.dataset.precedence === 'high'", "dataset: read data-precedence");
    ckjs("__d.dataset.twoWords === 'x'", "dataset: data-two-words -> twoWords");
    ckjs("__d.dataset.nope === undefined", "dataset: absent key is undefined");
    ckjs("('precedence' in __d.dataset) && !('nope' in __d.dataset)", "dataset: `in`");
    run("__d.dataset.theme = 'dark';");
    ckjs("__d.getAttribute('data-theme') === 'dark'", "dataset: WRITE reaches the attribute");
    /* The write and the delete are ONE expression on purpose. Split in two,
     * the delete check passed in the control build for the wrong reason: with
     * no dataset at all the write never happened, so the attribute was already
     * null and "delete removed it" looked true. An assertion that is satisfied
     * by the feature being absent is not an assertion. */
    ckjs("(function(){ __d.dataset.theme = 'dark';"
         " var a = __d.getAttribute('data-theme'); delete __d.dataset.theme;"
         " return a === 'dark' && __d.getAttribute('data-theme') === null; })()",
         "dataset: delete removes the attribute the write created");
    ckjs("(function(){ __d.setAttribute('data-live','1');"
         " return __d.dataset.live === '1'; })()", "dataset: is live, not a snapshot");

    /* ==== TreeWalker + NodeFilter ========================================
     * lit-html walks for comment nodes with whatToShow=128. The SKIP/REJECT
     * distinction is asserted directly because getting it backwards returns
     * an empty walk with no error -- the component renders nothing and
     * nothing says why. */
    ckjs("typeof document.createTreeWalker === 'function'", "document.createTreeWalker exists");
    ckjs("NodeFilter.SHOW_COMMENT === 128 && NodeFilter.FILTER_REJECT === 2",
         "NodeFilter constants");
    run("var __wrap = document.getElementById('wrap');");
    ckjs("(function () {"
         "  var w = document.createTreeWalker(document.documentElement, NodeFilter.SHOW_ELEMENT);"
         "  var n = 0; while (w.nextNode()) n++; return n > 2;"
         "})()", "TreeWalker: walks elements in pre-order");
    /* The load-bearing one: every element is SKIPPED by a comment walker, and
     * skip must still descend. A walker that treated skip as reject would
     * return 0 here. */
    ckjs("(function () {"
         "  var d = document.createElement('div');"
         "  d.innerHTML = '<span><!--a--></span><!--b-->';"
         "  var w = document.createTreeWalker(d, NodeFilter.SHOW_COMMENT);"
         "  var n = 0; while (w.nextNode()) n++; return n === 2;"
         "})()", "TreeWalker: SKIP still descends (the lit-html case)");
    ckjs("(function () {"
         "  var d = document.createElement('div');"
         "  d.innerHTML = '<span><i></i></span><b></b>';"
         "  var w = document.createTreeWalker(d, NodeFilter.SHOW_ELEMENT, function (n) {"
         "    return n.tagName.toLowerCase() === 'span' ? NodeFilter.FILTER_REJECT"
         "                                              : NodeFilter.FILTER_ACCEPT; });"
         "  var out = []; var n; while ((n = w.nextNode())) out.push(n.tagName.toLowerCase());"
         "  return out.join(',') === 'b';"
         "})()", "TreeWalker: REJECT prunes the subtree");

    /* ==== the interface objects ==========================================
     * MDN's `'closedBy' in HTMLDialogElement.prototype` is a module-body
     * feature test, so a ReferenceError there rejects the whole module. */
    ckjs_dom("typeof HTMLElement === 'function' && typeof HTMLDialogElement === 'function'",
         "HTMLElement / HTMLDialogElement exist");
    ckjs_dom("document.createElement('div') instanceof HTMLElement",
         "instanceof HTMLElement is true for an element");
    /* The one js_select.c's façades got WRONG: one prototype for every name
     * made this true, and a page branching on it takes a path it must not. */
    ckjs_dom("!(document.createElement('div') instanceof HTMLInputElement)",
         "instanceof HTMLInputElement is FALSE for a div");
    ckjs_dom("document.createElement('input') instanceof HTMLInputElement",
         "instanceof HTMLInputElement is true for an input");
    ckjs_dom("!('closedBy' in HTMLDialogElement.prototype)",
         "HTMLDialogElement.prototype answers a feature test (falsely, correctly)");
    /* Document has its own prototype in js_dom.c, so publishing it over the
     * ELEMENT prototype would make `document instanceof Document` false and
     * every <div> true. baidu's sniffer reaches the bare name. */
    ckjs_dom("typeof Document === 'function' && document instanceof Document",
         "Document exists and document is one");
    ckjs_dom("!(document.createElement('div') instanceof Document)",
         "an element is NOT a Document");

    /* ==== navigator.mimeTypes / navigator.plugins =======================
     * baidu's s006.js: `if (navigator.mimeTypes.length > 0)`. Empty is the
     * truthful answer -- no plugins are installed -- so the page takes the
     * branch that is correct for this browser instead of throwing. */
    ckjs("navigator.mimeTypes && navigator.mimeTypes.length === 0",
         "navigator.mimeTypes is an empty collection, not undefined");
    ckjs("navigator.plugins && navigator.plugins.length === 0",
         "navigator.plugins is an empty collection, not undefined");
    ckjs("navigator.mimeTypes.item(0) === null && navigator.plugins.namedItem('x') === null",
         "the collections answer item/namedItem");

    /* ==== customElements =================================================
     * A REAL upgrade: the node already in the document becomes `this` inside
     * the component's constructor. The identity check is the whole assertion
     * -- a registry that merely remembered the class would pass a `typeof
     * customElements.define === 'function'` test and fail this one. */
    ckjs("typeof customElements === 'object' && typeof customElements.define === 'function'",
         "customElements.define exists");
    run("var __seen = null, __conn = 0, __attr = null;"
         "class XProbe extends HTMLElement {"
         "  static get observedAttributes() { return ['label']; }"
         "  constructor() { super(); __seen = this; this.built = 1; }"
         "  connectedCallback() { __conn++; }"
         "  attributeChangedCallback(n, o, v) { __attr = n + '=' + v; }"
         "}"
         "var __host = document.getElementById('wrap');"
         "var __ce = document.createElement('x-probe');"
         "__ce.setAttribute('label', 'hi');"
         "__host.appendChild(__ce);"
         "customElements.define('x-probe', XProbe);");
    ckjs("__seen === __ce",
         "customElements: the EXISTING node is the constructor's `this`");
    ckjs("__ce.built === 1", "customElements: constructor fields land on the node");
    ckjs("__ce instanceof XProbe", "customElements: instanceof the component class");
    ckjs("__conn === 1", "customElements: connectedCallback fired once");
    ckjs("__attr === 'label=hi'", "customElements: attributeChangedCallback for a present attribute");
    ckjs("customElements.get('x-probe') === XProbe", "customElements.get");
    run("var __later = null; customElements.whenDefined('x-later').then(function (c) { __later = c; });"
         "class XLater extends HTMLElement {} customElements.define('x-later', XLater);");
    js_page_pump();
    ckjs("__later === XLater", "customElements.whenDefined resolves on define");
    /* Upgrade on INSERTION, not only on define: a component created after its
     * definition is the other half and has its own code path. */
    run("var __post = document.createElement('x-probe'); __host.appendChild(__post);");
    ckjs("__post instanceof XProbe && __post.built === 1",
         "customElements: a node inserted after define is upgraded too");

    /* ==== cloneNode =====================================================
     * The exact shapes jQuery 1.10.2's feature detection uses, because that
     * is the code path the real baidu page dies on -- one missing method,
     * then fourteen of its twenty-eight scripts fail with `'$' is not
     * defined`. Asserted as the page uses it, not as an API tour. */
    ckjs_dom("typeof document.createElement('div').cloneNode === 'function'",
         "Node.cloneNode exists");
    ckjs_dom("document.createElement('nav').cloneNode(true).tagName.toLowerCase() === 'nav'",
         "cloneNode: jQuery's html5Clone probe");
    ckjs_dom("(function(){ var i = document.createElement('input');"
         " i.setAttribute('type','checkbox'); i.setAttribute('checked','checked');"
         " return i.cloneNode(true).getAttribute('type') === 'checkbox'; })()",
         "cloneNode: attributes survive (there is no way to enumerate them)");
    ckjs_dom("(function(){ var d = document.createElement('div');"
         " d.innerHTML = '<b>x</b><i>y</i>';"
         " return d.cloneNode(true).childNodes.length === 2; })()",
         "cloneNode(true) is deep");
    /* jQuery.support line 98 verbatim in shape: the node being cloned is one
     * the PARSER made inside a detached div, reached through
     * getElementsByTagName, and it is cloned while still attached to it.
     * `input.cloneNode(true).checked` -- a null clone here is what stops
     * jQuery, and the earlier "deep" check above does not cover it because
     * that one clones the container, not a child of it. */
    ckjs("(function(){ var d = document.createElement('div');"
         " d.innerHTML = \"  <link/><table></table><a href='/a'>a</a><input type='checkbox'/>\";"
         " var i = d.getElementsByTagName('input')[0];"
         " if (!i) return 'no input parsed';"
         " var c = i.cloneNode(true);"
         " return !!c && c.getAttribute('type') === 'checkbox'; })()",
         "cloneNode: a parsed child cloned in place (jQuery noCloneChecked)");
    ckjs_dom("(function(){ var d = document.createElement('div');"
         " d.innerHTML = '<b>x</b>';"
         " return d.cloneNode(false).childNodes.length === 0; })()",
         "cloneNode(false) is shallow");
    ckjs_dom("(function(){ var d = document.createElement('div'); d.id = 'orig';"
         " var c = d.cloneNode(true); c.setAttribute('id','copy');"
         " return d.getAttribute('id') === 'orig'; })()",
         "cloneNode: the copy is independent of the original");
    /* jQuery's support.checkClone, verbatim in shape: a fragment cloned
     * twice, then lastChild read. It is what caught my first attempt. */
    ckjs_dom("(function(){ var f = document.createDocumentFragment();"
         " var i = document.createElement('input'); i.setAttribute('type','checkbox');"
         " f.appendChild(i);"
         " var c = f.cloneNode(true).cloneNode(true);"
         " return !!(c.lastChild && c.lastChild.tagName); })()",
         "cloneNode: a DocumentFragment cloned twice keeps its child (jQuery checkClone)");
    /* The node being cloned must come back exactly where it was. A clone that
     * quietly detached a live node would be far worse than a missing method. */
    /* Its own subtree, not the fixture's: earlier tests in this file mutate
     * #wrap, and js_dom.c's removeChild RECYCLES rather than orphans, so a
     * node looked up by selector here may already be gone. The claim under
     * test is about position restoration, so the position is built here. */
    run("var __ch = document.getElementById('wrap');"
        "var __a = document.createElement('i'); var __b = document.createElement('u');"
        "__ch.appendChild(__a); __ch.appendChild(__b);");
    ckjs_dom("(function(){ var n = __ch.childNodes.length;"
         " var par = __a.parentNode, nx = __a.nextSibling;"
         " __a.cloneNode(true);"
         " return __a.parentNode === par && __a.nextSibling === nx"
         "        && __ch.childNodes.length === n; })()",
         "cloneNode: an attached node is restored to its exact position");
    /* ==== children / childNodes are LIVE ===============================
     * Measured 2026-08-28 against 221 independent js-framework-benchmark
     * implementations (jsfb_matrix.py --include-built): SEVEN failed the
     * "update every 10th row" / "swap two rows" operations SILENTLY -- no
     * exception anywhere -- because `children`/`childNodes` used to be Array
     * SNAPSHOTS taken at property-access time. Every one of the seven
     * captures the collection ONCE ("const ROWS = tbody.children") before
     * `run()` has inserted any rows, so the captured snapshot is length 0
     * forever; every later `ROWS[i]` is `undefined`, and neither a `for`
     * loop's `r = ROWS[i]` condition nor a falsy-guarded `if (ROWS[998])`
     * throws on that. Driven case: keyed/vanillajs-lite,
     * webapi_probe --drive tests/unit/jsfb_drive.js.
     *
     * -DPLATFORM_NO_LIVE_COLLECTIONS (js_dom.c's child_array) restores the
     * snapshot exactly, and every check below must FAIL there --
     * test-platform-livecollection-negctl asserts the exact count. */
    ckjs_dom("(function(){ var d = document.createElement('div');"
         " d.appendChild(document.createElement('span'));"
         " var kids = d.children;"                     /* captured ONCE, like ROWS */
         " d.appendChild(document.createElement('span'));"
         " return kids.length === 2; })()",
         "children.length reflects a later appendChild without re-reading the property");
    ckjs_dom("(function(){ var d = document.createElement('div');"
         " var kids = d.children;"                      /* captured before ANY child exists */
         " var s = document.createElement('span'); s.id = 'late'; d.appendChild(s);"
         " return kids[0] === s; })()",
         "children[i] sees an element inserted after the collection was captured");
    ckjs_dom("(function(){ var d = document.createElement('div');"
         " d.appendChild(document.createElement('a'));"
         " d.appendChild(document.createElement('a'));"
         " d.appendChild(document.createElement('a'));"
         " var kids = d.children;"
         " kids[0].remove();"                            /* the exact swaprows/update shape: */
         " return kids.length === 2 && kids[0] === d.firstElementChild; })()",
         "children re-indexes after a sibling captured earlier is removed");
    ckjs_dom("(function(){ var d = document.createElement('div');"
         " d.appendChild(document.createElement('b'));"
         " var nodes = d.childNodes;"
         " d.appendChild(document.createTextNode('x'));"
         " return nodes.length === 2; })()",
         "childNodes.length is live too, not just children (Element vs Node)");
    /* The destructuring shape vanillajs-lite's swaprows actually uses --
     * `const [, r1, r2] = ROWS` reads the collection's OWN Symbol.iterator,
     * not indexed Gets one at a time, so this exercises a different code
     * path than the checks above. */
    ckjs_dom("(function(){ var d = document.createElement('div');"
         " var kids = d.children;"
         " d.appendChild(document.createElement('em'));"
         " d.appendChild(document.createElement('em'));"
         " var a = [].slice.call(kids);"                 /* forces the iterator/length path */
         " return a.length === 2 && a[0] === kids[0]; })()",
         "children supports Array-generic iteration (slice.call) while live");
    ckjs_dom("document.createElement('div').children instanceof HTMLCollection"
         " && document.createElement('div').childNodes instanceof NodeList",
         "children/childNodes keep their real interface identity as live objects");
    /* Cost control for the fix above: live_list_handle caches its backing
     * array against dom.h's child_gen (js_dom.c, child_array's file comment),
     * so a full sequential pass over a collection that has not been mutated
     * since the last read is O(1) per index, not O(index). Measured
     * host-native, one full `for (i=0;i<kids.length;i++)` pass over a
     * 10,000-child parent: 253.0 ms naive (re-walking `first_child` on every
     * index/length read) -> 0.0 ms cached, against 1.0 ms for the Array
     * snapshot this collection replaced -- the cache does not just avoid
     * being SLOWER than the snapshot it replaced, it matches it. A `-D` that
     * disabled only the cache (kept the walk, dropped the memo) would show
     * this check passing while the O(n^2) cost above came back; nothing here
     * exercises that path in isolation because the cache is not a separate
     * feature a page can observe going missing -- see the file comment for
     * why a THIRD variant was not built for it. */
    ckjs_dom("(function(){ var d = document.createElement('div');"
         " for (var i = 0; i < 10000; i++) d.appendChild(document.createElement('span'));"
         " var kids = d.children;"
         " var t0 = Date.now(); var sum = 0;"
         " for (var i = 0; i < kids.length; i++) sum += 1;"
         " return (Date.now() - t0) < 200 && sum === 10000; })()",
         "children stays O(1)-per-index on repeat reads (cached against child_gen, not re-walked)");

    ckjs("(function(){ var seen = 0;"
         " var mo = new MutationObserver(function(r){ seen += r.length; });"
         " mo.observe(__ch, { childList: true, subtree: true });"
         " __a.cloneNode(true);"
         " mo.disconnect(); return seen === 0; })()",
         "cloneNode: the internal move is not observable as a mutation");

    /* ==== attribute enumeration =========================================
     * js_dom.c can say what an attribute's value is and not which attributes
     * exist. jQuery.support line 99 needs the second:
     *     div.setAttribute(eventName = "on" + i, "t");
     *     support[i+"Bubbles"] = eventName in window ||
     *                            div.attributes[eventName].expando === false;
     * `div.attributes` undefined is `cannot read property 'onfocusin' of
     * undefined`, and jQuery stops there. */
    ckjs_dom("typeof document.createElement('div').getAttributeNames === 'function'",
         "Element.getAttributeNames exists");
    ckjs_dom("(function(){ var d = document.createElement('div');"
         " d.setAttribute('data-a','1'); d.setAttribute('title','t');"
         " var n = d.getAttributeNames();"
         " return n.indexOf('data-a') >= 0 && n.indexOf('title') >= 0; })()",
         "getAttributeNames lists what was set");
    /* The one that matters most: names the PARSER wrote, which no script ever
     * passed to setAttribute and which a set-tracking shim could not know. */
    ckjs_dom("(function(){ var n = document.getElementById('wrap').getAttributeNames();"
         " return n.indexOf('class') >= 0 && n.indexOf('data-role') >= 0; })()",
         "getAttributeNames lists PARSER-set attributes too");
    ckjs_dom("(function(){ var d = document.createElement('div');"
         " d.setAttribute('onfocusin','t');"
         " return !!d.attributes['onfocusin'] && d.attributes['onfocusin'].value === 't'"
         "        && d.attributes['onfocusin'].expando === undefined; })()",
         "Element.attributes by name (jQuery support line 99)");
    ckjs_dom("document.getElementById('wrap').attributes.length >= 3",
         "Element.attributes has a length");
    /* dataset enumeration was a named gap until getAttributeNames existed; it
     * calls it, so it comes for free and is asserted rather than assumed. */
    ckjs("Object.keys(document.getElementById('wrap').dataset).indexOf('role') >= 0",
         "dataset now enumerates (it routes through getAttributeNames)");

    /* ==== document.currentScript =========================================
     * MEASURED on two unrelated sites. nodejs.org: the Next.js chunk loader
     * derives its base URL from document.currentScript.src, finds nothing and
     * throws its OWN error -- 15 of 30 scripts died. x.com: four exceptions,
     * all the `document.currentScript.remove()` idiom.
     *
     * The fixture's inline <script id='s1'> is the one running when the page
     * was loaded, so the value here is null (we are past load) -- which is the
     * assertion that matters for the OTHER half of the spec: currentScript is
     * null outside a classic script's own synchronous execution. The positive
     * case is covered on real documents by the probe, where nodejs.org goes
     * from 15 uncaught exceptions to 1. */
    /* .src is the OTHER half of the webpack path and currentScript is not
     * sufficient without it: the chunk loader does
     *   if (s) u = s.src;  if (!u || !/^http(s?):/.test(u)) throw
     * so the property has to exist AND be absolute. Returning the attribute
     * would pass `typeof s.src === 'string'` and still fail the scheme test,
     * which is worse than absence because it looks like it worked. */
    run("var __sc = document.createElement('script'); __sc.setAttribute('src','/x/y.js');");
    /* Each of these names the PROPERTY in the same expression that checks the
     * rest of the claim. Split out, two of them passed in the control build for
     * the wrong reason -- "the attribute stays relative" and "a div has no
     * .src" are both trivially true when there is no .src at all, and a check
     * satisfied by the feature being absent is not a check. */
    ckjs("/^https?:/.test(__sc.src)", "script.src is ABSOLUTE, not the attribute");
    ckjs("/^https?:/.test(__sc.src) && __sc.getAttribute('src') === '/x/y.js'",
         "...and the attribute stays relative");
    ckjs("(function(){ __sc.src = '/z.js'; return __sc.getAttribute('src') === '/z.js'; })()",
         "writing .src goes back through setAttribute");
    ckjs("/^https?:/.test(__sc.src) && document.createElement('div').src === undefined",
         "a div has no .src (feature detection must not get a yes for everything)");
    ckjs("(function(){ var a = document.createElement('a'); a.setAttribute('href','/p');"
         " return /^https?:/.test(a.href); })()", "anchor.href is absolute too");

    ckjs("'currentScript' in document", "document.currentScript is defined");
    ckjs("document.currentScript === null",
         "document.currentScript is null outside a running classic script");

    /* Non-special URL schemes -- `new URL(s, 'x:/')`, the webpack 5 asset-module
     * idiom -- are asserted in tests/unit/webapi_test.c and NOT here. That fix
     * is in js_webapi.c, which the control build still links, so a check here
     * would pass in both builds and prove nothing. It has its own compile-time
     * control instead: make test-webapi-url-negctl. */

    /* ==== the misses that MUST STAY misses ===============================
     * These are how a page detects Internet Explorer or a feature it should
     * take the false branch on. The probe reports them as misses; defining any
     * of them would send a page down a path we cannot follow. This block is
     * here so a future well-meaning change to "reduce the miss list" fails a
     * test instead of a page. NOT inverted in the control run -- they are true
     * in both builds, which is the point. */
    int save = inverted;
    inverted = 0;
    ckjs("typeof window.ActiveXObject === 'undefined'", "window.ActiveXObject stays absent (IE detection)");
    ckjs("typeof document.documentMode === 'undefined'", "document.documentMode stays absent (IE detection)");
    ckjs("typeof window.MSApp === 'undefined'", "window.MSApp stays absent");
    /* NOT a "must stay absent" check -- indexedDB is real in this tree now
     * (c/apps/browser/js_idb.c). This assertion is the OTHER door of the
     * "one jar, two doors" pair for js_idb.c's own install guard: this build
     * links js_idb.c (PLATFORM_TEST_SRC does not exclude it, unlike
     * js_events.c/js_canvas.c/js_semantics.c) but not js_events.c, so
     * js_idb_install's `typeof G.EventTarget === 'function'` check should
     * fail and indexedDB should stay undefined HERE for that reason. If this
     * ever flips true without js_events.c being linked, js_idb.c's install
     * guard has stopped checking what it claims to. */
    ckjs("typeof window.indexedDB === 'undefined'",
         "window.indexedDB stays absent in a build that links js_idb.c but not js_events.c "
         "(its install guard requires a real EventTarget) -- see make test-idb for the real thing");
    ckjs("!(typeof crypto === 'object' && crypto && crypto.subtle)",
         "crypto.subtle stays absent (a stub would get something encrypted with it)");
    /* Element.attachShadow, added to this list after the Chrome differential
     * left it as the LAST thing on the corpus we throw on and Chrome does not
     * -- MDN's eight remaining exceptions are all this one call, from lit's
     * createRenderRoot. It is on the absent list rather than implemented for
     * the same reason as crypto.subtle: a light-DOM stand-in that returned the
     * element itself would make every component render with NO STYLE
     * ENCAPSULATION, and a page cannot detect that its shadow root does not
     * encapsulate. A real one needs a shadow tree in js_dom.c, which is the
     * DOM line's file. Absent and loud beats present and wrong. */
    ckjs("typeof document.createElement('div').attachShadow === 'undefined'",
         "Element.attachShadow stays absent (a light-DOM stand-in would silently "
         "drop style encapsulation -- needs a shadow tree in js_dom.c)");
    inverted = save;

    js_page_close();
    dom_free(root);

    printf("\n%s: %d checks, %d failures\n", inverted ? "test-platform-control" : "test-platform",
           checks, failures);
    if (failures) { printf("FAILED\n"); return 1; }
    printf("%s: ALL PASS\n", inverted ? "test-platform-control" : "test-platform");
    return 0;
}
