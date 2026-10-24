/* HTML timer initialization and the shipping task pumps, on a controlled
 * monotonic clock. This is a semantic/ownership gate, not a host performance
 * measurement or a substitute for root's browser.aex guest fixture. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "quickjs.h"
#include "dom/tree.h"
#include "js/dom/dom.h"
#include "js/runtime/page.h"
#include "js/runtime/timers.h"
#include "js/runtime/task_budget.h"

static unsigned long long timer_clock = 1000;
static int timer_checks, timer_failures;
static int fail_malloc, fail_calloc;
void *kmalloc(unsigned long n) { return malloc(n); }
void kfree(void *p) { free(p); }
static unsigned long long timer_test_clock(void) { return timer_clock; }
static void timer_check(int ok, const char *label)
{
    timer_checks++;
    if (!ok) timer_failures++;
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
}
static JSValue timer_record(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    const char *label = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;
    char text[320];
    snprintf(text, sizeof text, "%s %s", ctx == js_page_ctx() ? "page" : "worker", label ? label : "missing label");
    timer_check(argc > 0 && JS_ToBool(ctx, argv[0]) == 1, text);
    if (label) JS_FreeCString(ctx, label);
    return JS_UNDEFINED;
}
static JSValue timer_advance(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)ctx; (void)self; (void)argc; (void)argv;
    timer_clock += JS_TASK_TURN_MS + 1;
    return JS_UNDEFINED;
}
static void timer_install_observers(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "timerCheck", JS_NewCFunction(ctx, timer_record, "timerCheck", 2));
    JS_SetPropertyStr(ctx, global, "timerAdvance", JS_NewCFunction(ctx, timer_advance, "timerAdvance", 0));
    JS_FreeValue(ctx, global);
}
static void *timer_test_malloc(size_t size)
{
    if (fail_malloc) { fail_malloc = 0; return NULL; }
    return malloc(size);
}
static void *timer_test_calloc(size_t count, size_t size)
{
    if (fail_calloc) { fail_calloc = 0; return NULL; }
    return calloc(count, size);
}

#ifdef HTML_TIMERS_WORKER
#include "loader_fakebfetch.h"
static JSContext *timer_worker_context(JSRuntime *rt)
{
    JSContext *ctx = JS_NewContext(rt);
    if (ctx) timer_install_observers(ctx);
    return ctx;
}
#define JS_NewContext timer_worker_context
#endif
/* Only the actual owner's allocation calls are faulted. QuickJS and the DOM
 * keep their ordinary allocators, so a failed argv/task allocation cannot be
 * mistaken for a failure creating the test's function or its exception. */
#define malloc timer_test_malloc
#define calloc timer_test_calloc
#ifdef HTML_TIMERS_PAGE
#include "../../c/apps/gui/browser/js/runtime/page.c"
#else
#include "../../c/apps/gui/browser/js/workers/worker.c"
#endif
#undef malloc
#undef calloc
#ifdef HTML_TIMERS_WORKER
#undef JS_NewContext
#endif

static JSValue timer_eval(JSContext *ctx, const char *source)
{
    JSValue result = JS_Eval(ctx, source, strlen(source), "<html-timers>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue error = JS_GetException(ctx);
        const char *message = JS_ToCString(ctx, error);
        printf("FAIL evaluation: %s\n", message ? message : "unknown exception");
        timer_failures++;
        if (message) JS_FreeCString(ctx, message);
        JS_FreeValue(ctx, error);
    }
    return result;
}
static void timer_script(JSContext *ctx, const char *source)
{
    JS_FreeValue(ctx, timer_eval(ctx, source));
}
static int timer_truth(JSContext *ctx, const char *source)
{
    JSValue result = timer_eval(ctx, source);
    int value = !JS_IsException(result) && JS_ToBool(ctx, result) == 1;
    JS_FreeValue(ctx, result);
    return value;
}
static char *timer_fixture(void)
{
    FILE *file = fopen("tests/fixtures/html-priority/timers.js", "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return NULL; }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET)) { fclose(file); return NULL; }
    char *source = malloc((size_t)length + 1);
    if (!source) { fclose(file); return NULL; }
    if (fread(source, 1, (size_t)length, file) != (size_t)length) { free(source); fclose(file); return NULL; }
    source[length] = 0;
    fclose(file);
    return source;
}
static struct node *timer_open_page(void)
{
    const char *html = "<!doctype html><body><button id='probe'>timer</button></body>";
    struct node *root = dom_parse(html, (int)strlen(html));
    js_page_set_clock(timer_test_clock);
    js_page_set_location("http://fixture.test/page.html");
    if (!root || !js_page_open(root)) { if (root) dom_free(root); return NULL; }
    timer_install_observers(js_page_ctx());
    return root;
}
static void timer_drive(int limit)
{
    for (int pass = 0; pass < limit && !timer_truth(js_page_ctx(), "globalThis.htmlTimerDone===true"); pass++) {
        js_page_pump();
        if (!js_page_pending()) break;
        long long due = js_page_next_due();
        if (due < 0) break;
        if ((unsigned long long)due > timer_clock) timer_clock = (unsigned long long)due;
        js_page_run_due();
    }
}
static void timer_shared_rules(void)
{
    struct js_timer_state timer;
    js_timer_init(&timer, 0, 5, 0);
    timer_check(timer.timeout == 0 && timer.nesting == 6, "shared nesting 5 remains zero before increment");
    js_timer_init(&timer, 0, 6, 1);
    timer_check(timer.timeout == 4 && timer.nesting == 7 && timer.repeat, "shared nesting 6 clamps to 4 ms");
    js_timer_init(&timer, -1, 0, 1);
    timer_check(timer.timeout == 0 && timer.repeat, "shared negative delay preserves zero interval repeat flag");
    int next = INT32_MAX;
    timer_check(js_timer_next_id(&next) == INT32_MAX && next == 1 && js_timer_next_id(&next) == 1,
                "shared ID wrap stays positive without signed overflow");
}

#ifdef HTML_TIMERS_PAGE
static JSValue timer_reuse_id(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)ctx; (void)self; (void)argc; (void)argv;
    g_next_id = 1;
    return JS_UNDEFINED;
}
static void timer_page_ids_and_oom(void)
{
    JSContext *ctx = js_page_ctx();
    g_next_id = 1;
    timer_script(ctx, "var protectedID=setTimeout(function(){},100);");
    g_next_id = INT32_MAX;
    timer_script(ctx, "var wrapMax=setTimeout(function(){},100),wrapNext=setTimeout(function(){},100);");
    timer_check(timer_truth(ctx, "protectedID===1&&wrapMax===2147483647&&wrapNext===2"),
                "page ID wrap skips the live handle at 1");
    timer_script(ctx, "clearTimeout(protectedID);clearTimeout(wrapMax);clearTimeout(wrapNext);");
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "timerReuseID", JS_NewCFunction(ctx, timer_reuse_id, "timerReuseID", 0));
    JS_FreeValue(ctx, global);
    g_next_id = 1;
    timer_script(ctx, "var reuseCalls=0,replacementCalls=0,oldID=setInterval(function(){reuseCalls++;"
                      "clearInterval(oldID);timerReuseID();var replacement=setTimeout(function(){replacementCalls++;},0);"
                      "if(replacement!==oldID)throw Error('fixture did not reuse ID');},0);");
    js_page_run_due();js_page_run_due();js_page_run_due();
    timer_check(timer_truth(ctx, "reuseCalls===1&&replacementCalls===1") && !js_page_pending(),
                "page cancelled running handle cannot resurrect after exact ID reuse");
    JSValue fn = timer_eval(ctx, "(function(){throw Error('OOM timer must not execute');})");
    JSValue args[] = {fn, JS_NewInt32(ctx, 0), JS_NewInt32(ctx, 41)};
    size_t before = g_page->count;
    fail_malloc = 1;
    JSValue result = js_setTimeout(ctx, JS_UNDEFINED, 3, (JSValueConst *)args);
    timer_check(JS_IsException(result) && !fail_malloc && g_page->count == before,
                "page argv OOM throws and leaves no silently truncated timer");
    JS_FreeValue(ctx, result);JS_FreeValue(ctx, JS_GetException(ctx));
    fail_calloc = 1;
    result = js_setInterval(ctx, JS_UNDEFINED, 2, (JSValueConst *)args);
    timer_check(JS_IsException(result) && !fail_calloc && g_page->count == before,
                "page task OOM throws and retains no callback owner");
    JS_FreeValue(ctx, result);JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, fn);
}
static void timer_public_scenarios(const char *fixture)
{
    JSContext *ctx = js_page_ctx();
    timer_script(ctx, fixture);
    timer_script(ctx, "var htmlTimerDone=false;testHTMLTimers(timerCheck).then(function(){htmlTimerDone=true;},"
                      "function(e){timerCheck(false,'public fixture rejected: '+e);htmlTimerDone=true;});");
    js_page_pump();timer_drive(300);
    timer_check(timer_truth(ctx, "htmlTimerDone"), "page public fixture reaches completion");
    timer_check(!js_page_pending(), "page public fixture leaves no queued callbacks");
}
#else
static const char TIMER_WORKER_PUBLIC[] =
    "importScripts('http://fixture.test/timers.js');"
    "testHTMLTimers(timerCheck).then(function(){postMessage('done');},"
    "function(e){timerCheck(false,'public fixture rejected: '+e);postMessage('done');});";
static const char TIMER_WORKER_YIELD[] =
    "var tickCount=0,trace=[],argument={},argsOK=true;"
    "var interval=setInterval(function(value){tickCount++;argsOK=argsOK&&value===argument;"
    "if(tickCount===1)Promise.resolve().then(function(){trace.push('first');timerAdvance();})"
    ".then(function(){trace.push('second');});else{clearInterval(interval);postMessage('done');}},0,argument);";
static struct jsworker *timer_start_worker(const char *source)
{
    fake_site_add("http://fixture.test/timer-worker.js", source);
    JSContext *ctx = js_page_ctx();
    timer_script(ctx, "var htmlTimerDone=false;var timerWorker=new Worker('http://fixture.test/timer-worker.js');"
                      "timerWorker.onmessage=function(e){if(e.data==='done')htmlTimerDone=true;};"
                      "timerWorker.onerror=function(e){timerCheck(false,'worker error: '+e.message);htmlTimerDone=true;};");
    JSValue value = timer_eval(ctx, "timerWorker._wid");
    int32_t id = 0;JS_ToInt32(ctx, &id, value);JS_FreeValue(ctx, value);
    js_worker_run_due();
    return find_worker(id);
}
static JSValue timer_worker_reuse_id(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)ctx; (void)self; (void)argc; (void)argv;
    g_next_timer_id = 1;
    return JS_UNDEFINED;
}
static void timer_worker_foreign_id(struct jsworker *owner)
{
    /* The flat task list contains several workers' independent ID maps.
     * Force a real peer to own ID 1, then clear 1 from the original worker:
     * scanning by integer alone used to cancel the peer's timer silently. */
    fake_site_add("http://fixture.test/timer-peer.js", "var peerTimer=setTimeout(function(){},100);");
    g_next_timer_id = 1;
    JSContext *parent = js_page_ctx();
    timer_script(parent, "var timerPeer=new Worker('http://fixture.test/timer-peer.js');");
    JSValue value = timer_eval(parent, "timerPeer._wid");
    int32_t id = 0;JS_ToInt32(parent, &id, value);JS_FreeValue(parent, value);
    struct jsworker *peer = find_worker(id);
    for (int pass = 0; peer && peer->state == WK_STARTING && pass < 4; pass++) js_worker_run_due();
    int ready = peer && peer->state == WK_RUNNING && wtimer_id_used(peer, 1);
    timer_check(ready, "worker foreign-ID fixture has a real peer owning timer 1");
    timer_script(owner->wctx, "clearTimeout(1);");
    timer_check(ready && wtimer_id_used(peer, 1), "worker clear cannot remove another worker's matching integer ID");
    timer_script(parent, "timerPeer.terminate();");js_worker_run_due();
}
static void timer_worker_ids_and_oom(struct jsworker *w)
{
    JSContext *ctx = w->wctx;
    g_next_timer_id = 1;
    timer_script(ctx, "var protectedID=setTimeout(function(){},100);");
    g_next_timer_id = INT32_MAX;
    timer_script(ctx, "var wrapMax=setTimeout(function(){},100),wrapNext=setTimeout(function(){},100);");
    timer_check(timer_truth(ctx, "protectedID===1&&wrapMax===2147483647&&wrapNext===2"),
                "worker ID wrap skips the live handle at 1");
    timer_script(ctx, "clearTimeout(protectedID);clearTimeout(wrapMax);clearTimeout(wrapNext);");
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "timerReuseID", JS_NewCFunction(ctx, timer_worker_reuse_id, "timerReuseID", 0));
    JS_FreeValue(ctx, global);
    g_next_timer_id = 1;
    timer_script(ctx, "var reuseCalls=0,replacementCalls=0,oldID=setInterval(function(){reuseCalls++;"
                      "clearInterval(oldID);timerReuseID();var replacement=setTimeout(function(){replacementCalls++;},0);"
                      "if(replacement!==oldID)throw Error('fixture did not reuse ID');},0);");
    for (int pass = 0; pass < 4; pass++) js_worker_run_due();
    timer_check(timer_truth(ctx, "reuseCalls===1&&replacementCalls===1") && !wtimer_id_used(w, 1),
                "worker cancelled running handle cannot resurrect after exact ID reuse");
    timer_worker_foreign_id(w);
    JSValue fn = timer_eval(ctx, "(function(){throw Error('OOM timer must not execute');})");
    JSValue args[] = {fn, JS_NewInt32(ctx, 0), JS_NewInt32(ctx, 41)};
    unsigned long long before = g_wseq;
    fail_malloc = 1;
    JSValue result = wtimer_add(ctx, w->id, 0, 3, (JSValueConst *)args);
    timer_check(JS_IsException(result) && !fail_malloc && g_wseq == before,
                "worker argv OOM throws and leaves no silently truncated timer");
    JS_FreeValue(ctx, result);JS_FreeValue(ctx, JS_GetException(ctx));
    fail_calloc = 1;
    result = wtimer_add(ctx, w->id, 1, 3, (JSValueConst *)args);
    timer_check(JS_IsException(result) && !fail_calloc && g_wseq == before,
                "worker task OOM throws and releases copied arguments");
    JS_FreeValue(ctx, result);JS_FreeValue(ctx, JS_GetException(ctx));JS_FreeValue(ctx, fn);
}
static void timer_public_scenarios(const char *fixture)
{
    fake_site_add("http://fixture.test/timers.js", fixture);
    struct jsworker *w = timer_start_worker(TIMER_WORKER_PUBLIC);
    timer_check(w && w->state == WK_RUNNING, "worker public fixture starts in its own runtime");
    if (!w || w->state != WK_RUNNING) return;
    timer_drive(400);
    timer_check(timer_truth(js_page_ctx(), "htmlTimerDone"), "worker public fixture reaches parent completion");
    timer_worker_ids_and_oom(w);
    timer_script(js_page_ctx(), "timerWorker.terminate();");js_worker_run_due();
    timer_check(!js_worker_pending(), "worker public fixture and allocator probes release all ownership");
}
static struct jsworker *timer_yield_checkpoint(void)
{
    fake_site_reset();
    struct jsworker *w = timer_start_worker(TIMER_WORKER_YIELD);
    if (!w || w->state != WK_RUNNING) return NULL;
    js_worker_run_due();
    timer_check(w->running_timer && w->timer_finishing && worker_jobs_pending(w),
                "worker budget yield retains its detached timer through the pending checkpoint");
    timer_check(timer_truth(w->wctx, "tickCount===1&&trace.join(',')==='first'"),
                "worker first job completes normally before the budget boundary");
    return w;
}
static void timer_worker_checkpoint_ownership(struct node **root)
{
    struct jsworker *w = timer_yield_checkpoint();
    if (!w) { timer_check(0, "worker yield fixture starts"); return; }
    js_worker_run_due();
    timer_check(timer_truth(w->wctx, "tickCount===1&&trace.join(',')==='first,second'"),
                "worker resumes remaining jobs before repeating its timer");
    timer_check(!w->running_timer && !w->timer_finishing,
                "worker completed checkpoint transfers timer ownership back to its task queue");
    timer_drive(20);
    timer_check(timer_truth(w->wctx, "tickCount===2&&argsOK"), "worker repeat preserves its argument after checkpoint yield");
    timer_script(js_page_ctx(), "timerWorker.terminate();");js_worker_run_due();
    timer_check(!js_worker_pending(), "worker completed interval terminates cleanly");

    w = timer_yield_checkpoint();
    if (!w) { timer_check(0, "worker terminate-yield fixture starts"); return; }
    timer_script(js_page_ctx(), "timerWorker.terminate();");js_worker_run_due();
    timer_check(!js_worker_pending(), "terminate releases a timer retained by an unfinished checkpoint");

    w = timer_yield_checkpoint();
    if (!w) { timer_check(0, "worker navigation-yield fixture starts"); return; }
    js_page_close();dom_free(*root);*root = timer_open_page();
    timer_check(*root && !js_worker_pending(), "page close releases a timer retained by an unfinished worker checkpoint");
}
#endif

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    char *fixture = timer_fixture();
    timer_check(fixture != NULL, "shared public timer fixture is readable");
    if (!fixture) return 2;
#ifdef HTML_TIMERS_WORKER
    fake_site_reset();
#endif
    struct node *root = timer_open_page();
    timer_check(root != NULL, "timer test opens the production page runtime");
    if (!root) { free(fixture); return 2; }
    timer_shared_rules();
#ifdef HTML_TIMERS_PAGE
    timer_page_ids_and_oom();
#endif
    timer_public_scenarios(fixture);
#ifdef HTML_TIMERS_WORKER
    timer_worker_checkpoint_ownership(&root);
#endif
    js_page_close();dom_free(root);free(fixture);
    printf("html-timers-%s: %d checks, %d failures\n",
#ifdef HTML_TIMERS_PAGE
           "page",
#else
           "worker",
#endif
           timer_checks, timer_failures);
    return timer_failures ? 1 : 0;
}
