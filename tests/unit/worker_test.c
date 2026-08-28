/* Host test for c/apps/browser/js_worker.c -- dedicated Worker.
 *
 *     make test-worker                    the coherent-subset suite AND the
 *                                          termination bar's own quiescence
 *                                          check, in one binary
 *     make test-worker-negctl-terminate   the SAME file, linked with
 *                                          -DJS_WORKER_NO_TERMINATE (js_worker.c's
 *                                          own control: mark_dead_ex() skips
 *                                          cancel_worker_tasks(), so a queued
 *                                          worker task survives terminate()
 *                                          instead of being dropped immediately)
 *                                          -- must FAIL
 *     make test-worker-negctl-silent      the SAME file, linked with
 *                                          -DJS_WORKER_SILENT_ERROR (js_worker.c's
 *                                          own control: deliver_error_to_parent()
 *                                          becomes a no-op) -- must FAIL
 *
 * THE ACCEPTANCE CRITERION IS ABOVE THE SCENARIO CHECKS, same shape as
 * tests/unit/webapi_idb_test.c: every tracker this file pushed into
 * `W.trackers` must have `.done === true` after a bounded pump, AND
 * js_worker_pending() -- the SAME predicate js_page.c folds into its own
 * idle check (js_page.c:301) -- must read false. That second half is the
 * check IndexedDB's gate has no equivalent of and Worker did not have until
 * this file: not just "did every visible effect eventually happen" but "is
 * the worker scheduler's own queue actually empty", which is what makes the
 * -DJS_WORKER_NO_TERMINATE control observable at all (see the terminate()
 * scenario below -- the two builds agree on every EVENTUAL outcome and
 * disagree only on whether the queue drains promptly).
 *
 * pump_until_idle bounds the pump at a fixed number of passes, never on
 * js_page_pending() forever, for the same reason webapi_idb_test.c gives:
 * this harness must not itself hang on either build. A build that leaves
 * something stuck is caught by the checks AFTER the pump, not by a hang.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include "dom.h"
#include "js_dom.h"
#include "js_page.h"
#include "js_worker.h"
#include "loader_fakebfetch.h"

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

/* ---- worker script fixtures, served by loader_fakebfetch.c ---- */
static const char *ECHO_JS =
    "postMessage('ready');"
    "onmessage = function (e) { postMessage('echo:' + e.data); };";

static const char *THROW_JS = "throw new Error('boom');";

static const char *NESTED_JS =
    "try { new Worker('x.js'); postMessage('no-throw'); }"
    "catch (e) { postMessage('caught:' + e.name); }";

static const char *LIB_JS = "self.LIBVAL = 42;";
static const char *IMPORT_OK_JS =
    "importScripts('http://fixture.test/lib.js');"
    "postMessage('lib:' + LIBVAL);";
static const char *IMPORT_FAIL_JS =
    "try { importScripts('http://fixture.test/missing.js'); postMessage('no-throw'); }"
    "catch (e) { postMessage('caught:' + e.name); }";

static const char *INTERVAL_JS = "setInterval(function () {}, 10000);";

static const char *CLOSE_JS =
    "postMessage('before');"
    "close();"
    "setTimeout(function () { postMessage('after-timeout-should-not-arrive'); }, 0);";

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    fake_site_reset();
    fake_site_add("http://fixture.test/echo.js", ECHO_JS);
    fake_site_add("http://fixture.test/throw.js", THROW_JS);
    fake_site_add("http://fixture.test/nested.js", NESTED_JS);
    fake_site_add("http://fixture.test/lib.js", LIB_JS);
    fake_site_add("http://fixture.test/import-ok.js", IMPORT_OK_JS);
    fake_site_add("http://fixture.test/import-fail.js", IMPORT_FAIL_JS);
    fake_site_add("http://fixture.test/interval.js", INTERVAL_JS);
    fake_site_add("http://fixture.test/close.js", CLOSE_JS);
    /* http://fixture.test/notfound.js deliberately NOT registered. */

    struct node *root = dom_parse(PAGE, (int)strlen(PAGE));
    if (!root) { printf("FAIL: fixture did not parse\n"); return 1; }

    js_page_set_clock(clock_fn);
    js_page_set_location("http://fixture.test/page.html");
    if (!js_page_open(root)) { printf("FAIL: js_page_open\n"); return 1; }
    ctx = js_page_ctx();

    /* ==== feature detection surface ===================================== */
    ckjs("typeof Worker === 'function'", "Worker exists");
    ckjs("typeof SharedWorker === 'undefined'", "SharedWorker stays absent (refused by name, not stubbed)");
    ckjs("typeof importScripts === 'undefined'", "importScripts is not defined on the WINDOW (only inside a worker)");

    /* Every tracker created below is pushed here so the quiescence check at
     * the end has something concrete to walk: {name, done}. */
    run("var W = { trackers: [] };");
    run("function TR(name) { var t = { name: name, done: false, log: [] }; W.trackers.push(t); return t; }");

    /* ==== malformed absolute URL: SYNCHRONOUS throw, never a Worker whose
     * fate is undecided ==================================================== */
    ckjs(
        "(function () {"
        "  try { new Worker('https://{{host}}:{{port}}/x.js'); return false; }"
        "  catch (e) { return e instanceof DOMException && e.name === 'SyntaxError'; }"
        "})()",
        "new Worker() with an unsubstituted WPT-style template URL throws SyntaxError synchronously");

    /* ==== {type:'module'}: throws, never a Worker that silently never runs = */
    ckjs(
        "(function () {"
        "  try { new Worker('http://fixture.test/echo.js', { type: 'module' }); return false; }"
        "  catch (e) { return e instanceof DOMException && e.name === 'NotSupportedError'; }"
        "})()",
        "new Worker(url, {type:'module'}) throws NotSupportedError, not a Worker that never runs");

    /* ==== normal roundtrip: startup message, then postMessage both ways === */
    run("var tE = TR('echo');");
    run("var wE = new Worker('http://fixture.test/echo.js');");
    run("wE.onmessage = function (e) { tE.log.push(e.data); if (e.data === 'ready') { wE.postMessage('hi'); } "
        "else if (e.data === 'echo:hi') { tE.done = true; } };");
    run("wE.onerror = function (e) { tE.log.push('error:' + e.message); tE.done = true; };");
    pump_until_idle(20);
    ckjs("W.trackers[0].log.join(',') === 'ready,echo:hi'", "echo worker: startup message then a postMessage roundtrip, in order");
    ckjs("W.trackers[0].done === true", "echo worker: the roundtrip reached its terminal state");

    /* ==== fetch failure (404): item 9 -- error to parent, never a hang ===== */
    run("var tNF = TR('notfound');");
    run("var wNF = new Worker('http://fixture.test/notfound.js');");
    run("wNF.onerror = function (e) { tNF.log.push(e.message); tNF.done = true; };");
    run("wNF.onmessage = function () { tNF.log.push('UNEXPECTED-MESSAGE'); tNF.done = true; };");
    pump_until_idle(20);
    ckjs("W.trackers[1].done === true", "a Worker script that 404s fires error on the parent (does not hang)");
    ckjs("W.trackers[1].log.length === 1 && W.trackers[1].log[0].indexOf('UNEXPECTED') < 0",
         "the 404 case fired error, not message");

    /* ==== cross-origin script URL: item 8 =================================
     * fixture.test != evil.example, so worker_start()'s origin check must
     * fire before ever attempting to fetch. */
    run("var tXO = TR('cross-origin');");
    run("var wXO = new Worker('https://evil.example/x.js');");
    run("wXO.onerror = function (e) { tXO.log.push(e.message); tXO.done = true; };");
    pump_until_idle(20);
    ckjs("W.trackers[2].done === true", "a cross-origin Worker script URL fires error on the parent (does not hang)");
    ckjs("W.trackers[2].log[0].indexOf('cross-origin') >= 0", "the cross-origin error names what was refused");

    /* ==== uncaught top-level throw: items 10-11 ============================ */
    run("var tTH = TR('throw');");
    run("var wTH = new Worker('http://fixture.test/throw.js');");
    run("wTH.onerror = function (e) { tTH.log.push(e.message); tTH.done = true; };");
    pump_until_idle(20);
    ckjs("W.trackers[3].done === true", "a worker whose script throws at the top level fires error on the parent");
    ckjs("W.trackers[3].log[0].indexOf('boom') >= 0", "the reported error names the uncaught exception");

    /* ==== nested `new Worker` inside a worker: refused by name ============= */
    run("var tNW = TR('nested');");
    run("var wNW = new Worker('http://fixture.test/nested.js');");
    run("wNW.onmessage = function (e) { tNW.log.push(e.data); tNW.done = true; };");
    run("wNW.onerror = function (e) { tNW.log.push('error:' + e.message); tNW.done = true; };");
    pump_until_idle(20);
    ckjs("W.trackers[4].done === true", "a worker that tries `new Worker` inside itself reaches a terminal state");
    ckjs("W.trackers[4].log[0] === 'caught:NotSupportedError'",
         "nested `new Worker` throws NotSupportedError INSIDE the worker (caught there), not silently ignored");

    /* ==== importScripts: success and failure, both synchronous from the
     * worker's own point of view, both terminal from the parent's ========== */
    run("var tIS = TR('import-ok');");
    run("var wIS = new Worker('http://fixture.test/import-ok.js');");
    run("wIS.onmessage = function (e) { tIS.log.push(e.data); tIS.done = true; };");
    run("wIS.onerror = function (e) { tIS.log.push('error:' + e.message); tIS.done = true; };");
    pump_until_idle(20);
    ckjs("W.trackers[5].done === true", "importScripts() success reaches a terminal state");
    ckjs("W.trackers[5].log[0] === 'lib:42'", "importScripts() actually ran the imported script before the next statement");

    run("var tISF = TR('import-fail');");
    run("var wISF = new Worker('http://fixture.test/import-fail.js');");
    run("wISF.onmessage = function (e) { tISF.log.push(e.data); tISF.done = true; };");
    run("wISF.onerror = function (e) { tISF.log.push('error:' + e.message); tISF.done = true; };");
    pump_until_idle(20);
    ckjs("W.trackers[6].done === true", "importScripts() of a missing script reaches a terminal state");
    ckjs("W.trackers[6].log[0] === 'caught:NetworkError'",
         "importScripts() of a missing script throws NetworkError INSIDE the worker (catchable), not silently ignored");

    /* ==== transferables and non-cloneable values: refused synchronously,
     * never a silent copy and never a hang ================================= */
    ckjs(
        "(function () {"
        "  try { wE.postMessage(1, [new ArrayBuffer(4)]); return false; }"
        "  catch (e) { return e instanceof DOMException && e.name === 'DataCloneError'; }"
        "})()",
        "postMessage() with a non-empty transfer list throws DataCloneError synchronously (never a silent copy)");
    ckjs(
        "(function () {"
        "  try { wE.postMessage(function () {}); return false; }"
        "  catch (e) { return e.name === 'DataCloneError'; }"
        "})()",
        "postMessage() with a function argument throws DataCloneError synchronously (functions do not clone)");
    ckjs(
        "(function () {"
        "  if (typeof SharedArrayBuffer !== 'function') return true;"
        "  try { wE.postMessage(new SharedArrayBuffer(4)); return false; }"
        "  catch (e) { return e.name === 'DataCloneError'; }"
        "})()",
        "postMessage() with a SharedArrayBuffer throws DataCloneError (there is no second thread to share with)");

    /* ==== self.close(): item 17 ============================================
     * NOT "the synchronous script keeps its own already-queued messages" --
     * verified against the real corpus before writing this assertion, not
     * assumed: build/wpt-full/workers/Worker_terminate_event_queue.htm (a
     * worker posts thousands of messages in a tight loop; the parent
     * terminates it after the first and reassigns onmessage to
     * unreached_func) is in this file's own WPT pass set, which only holds
     * if terminate()/close() discard EVERY task this worker has queued,
     * including ones already addressed to the parent that have not been
     * delivered yet -- not just the worker's own future timers. cancel_worker_
     * tasks() matches by wid alone for exactly that reason (js_worker.c:290-
     * 301). So 'before', posted moments before close() in the SAME
     * synchronous script, is discarded right along with the setTimeout
     * scheduled after it -- the log must end up EMPTY, not {'before'}. */
    run("var tCL = TR('close');");
    run("var wCL = new Worker('http://fixture.test/close.js');");
    run("wCL.onmessage = function (e) { tCL.log.push(e.data); };");
    pump_until_idle(20);
    ckjs("W.trackers[7].log.length === 0",
         "self.close() discards this worker's own already-queued 'before' message AND the setTimeout scheduled after it -- matches Worker_terminate_event_queue.htm's contract, not a hang and not a leaked message");
    run("W.trackers[7].done = true;");  /* nothing will ever arrive for this one -- that IS the terminal state */

    /* ==== postMessage to an already-dead worker: item 15 -- drop, never
     * throw, never hang the sender =========================================== */
    run("wCL.terminate();");
    ckjs(
        "(function () { try { return wCL.postMessage('to-the-void') === undefined; } catch (e) { return false; } })()",
        "postMessage() to a dead worker returns undefined without throwing (drops the message, does not hang the sender)");

    /* =========================================================================
     * THE CONTROL: terminate() must remove queued work IMMEDIATELY, not just
     * when it happens to become due. This is what -DJS_WORKER_NO_TERMINATE
     * disables (js_worker.c: mark_dead_ex() skips cancel_worker_tasks()), and
     * it is the one property no EVENTUAL-outcome check above can see: a task
     * due ten seconds from now gets dropped either way once ten seconds of
     * virtual time pass (run_due's own WK_DEAD check catches it lazily) --
     * the two builds diverge only on whether it is gone *right now*, before
     * any further tick. Checked directly against js_worker_pending(), the
     * SAME predicate js_page.c folds into its own idle check.
     * ========================================================================= */
    run("var wIV = new Worker('http://fixture.test/interval.js');");
    pump_until_idle(5);   /* let it start and register its 10s setInterval */
    ck(js_worker_pending() != 0,
       "sanity: a live worker with a 10s setInterval leaves worker task(s) queued");
    run("wIV.terminate();");
    ck(js_worker_pending() == 0,
       "terminate() drops every queued task for that worker IMMEDIATELY, not only once each becomes due "
       "(THE CONTROL: -DJS_WORKER_NO_TERMINATE must make this one FAIL)");

    /* ==== THE ACCEPTANCE CRITERION: every tracker reached done, AND the
     * worker scheduler's own queue is empty. Read STUCK, not just FAILED --
     * a stuck tracker is the failure webapi_idb_test.c calls out by name. */
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
            printf("STUCK: W.trackers[%d] never reached .done after the bounded pump\n", stuck);
        ck(stuck < 0, "quiescence: every tracker created by this file reached a terminal state");
    }
    ck(js_worker_pending() == 0,
       "quiescence: the worker task queue (js_worker_pending()) is empty at the end of the run");

    js_page_close();
    dom_free(root);

    printf("\ntest-worker: %d checks, %d failures\n", checks, failures);
    if (failures) { printf("FAILED\n"); return 1; }
    printf("ALL PASS\n");
    return 0;
}
