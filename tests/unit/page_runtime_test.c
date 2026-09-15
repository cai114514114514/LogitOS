/* Bounded functional checks for the queue AND its shipping js_page consumer.
 * A fake monotonic clock makes ordering observable without timing the host.
 * This proves callback/lifetime semantics in a host runtime, not guest painting. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dom.h"
#include "js_page.h"
#include "page_runtime.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void kfree(void *p) { free(p); }
static int checks, failures;
static unsigned long long clock_ms = 1000;
static unsigned long long test_clock(void) { return clock_ms; }
static void check(int ok, const char *name)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}
static void dispose(void *context, void *payload)
{
    (*(int *)context)++;
    (*(int *)payload)++;
}
static void queue_checks(void)
{
    struct page_runtime page = {0};
    struct page_task a = {0}, b = {0}, c = {0};
    int disposed = 0, da = 0, db = 0, dc = 0;
    check(page_runtime_open(&page, 0, &disposed, 0, 2), "open bounded queue");
    struct page_runtime_token first = page_runtime_token(&page);
    check(page_task_enqueue(&page, &a, first, 20, PAGE_TASK_TIMER, &da, dispose) == PAGE_TASK_OK,
          "enqueue first task");
    check(page_task_enqueue(&page, &b, first, 10, PAGE_TASK_ANIMATION_FRAME, &db, dispose) == PAGE_TASK_OK,
          "enqueue frame in shared queue");
    check(page_task_enqueue(&page, &c, first, 0, PAGE_TASK_TIMER, &dc, dispose) == PAGE_TASK_FULL,
          "queue capacity enforced");
    /* Dispose an accepted control-only third task so later checks continue on
     * a valid bounded fixture; the failure above remains the control verdict. */
    if (c.queued) page_task_cancel(&page, &c);
    check(page_runtime_next_due(&page) == 10, "earliest deadline");
    uint64_t turn = page_runtime_turn(&page);
    check(!page_runtime_ready(&page, 9, turn), "no early dispatch");
    check(page_runtime_ready(&page, 20, turn) == &b, "deadline before creation order");
    check(page_task_rearm(&page, &b, 20), "interval rearm");
    check(page_runtime_ready(&page, 20, turn) == &a, "rearmed task waits for next turn");
    check(page_task_cancel(&page, &a) && !page_task_cancel(&page, &a) && da == 1,
          "cancel disposes exactly once");
    check(!page_runtime_ready(&page, 20, turn), "rearm cannot run twice in one turn");
    check(page_runtime_ready(&page, 20, page_runtime_turn(&page)) == &b,
          "rearm runs on following turn");
    page_runtime_invalidate(&page);
    check(!page_runtime_accepts(first) && !page_runtime_ready(&page, 100, UINT64_MAX),
          "closing page does not dispatch");
    check(page_task_enqueue(&page, &a, first, 0, PAGE_TASK_TIMER, &da, dispose) == PAGE_TASK_CLOSED,
          "closing page refuses enqueue");
    page_runtime_close(&page);
    check(db == 1 && !page.count && page_runtime_next_due(&page) == -1,
          "close cancels pending task before context release");
    check(page_runtime_open(&page, 0, &disposed, 0, 2), "reuse owner for next page");
    check(!page_runtime_accepts(first), "old page token rejected after reopen");
    check(page_task_enqueue(&page, &a, first, 0, PAGE_TASK_TIMER, &da, dispose) == PAGE_TASK_STALE,
          "old producer cannot enqueue into next page");
    if (a.queued) page_task_cancel(&page, &a);
    struct page_runtime_token second = page_runtime_token(&page);
    check(page_task_enqueue(&page, &a, second, 5, PAGE_TASK_TIMER, &da, dispose) == PAGE_TASK_OK &&
          page_task_enqueue(&page, &b, second, 5, PAGE_TASK_ANIMATION_FRAME, &db, dispose) == PAGE_TASK_OK,
          "new page accepts timer and frame");
    check(page_runtime_ready(&page, 5, page_runtime_turn(&page)) == &a,
          "equal deadline preserves registration order");
    page_runtime_close(&page);
    int previous = disposed;
    page_runtime_close(&page);
    check(disposed == previous, "repeated close is idempotent");
}
static void script(const char *src)
{
    check(js_page_eval(src, (int)strlen(src), "<page-runtime-test>", 0), "script evaluation");
}
static void expect(const char *expr, const char *name)
{
    JSContext *ctx = js_page_ctx();
    JSValue v = JS_Eval(ctx, expr, strlen(expr), "<page-runtime-assert>", JS_EVAL_TYPE_GLOBAL);
    int ok = !JS_IsException(v) && JS_ToBool(ctx, v) == 1;
    if (JS_IsException(v)) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, v);
    check(ok, name);
}
static int old_page_calls;
static JSValue record_old(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)ctx; (void)self; (void)argc; (void)argv;
    old_page_calls++;
    return JS_UNDEFINED;
}
static void page_checks(void)
{
    const char *html = "<html><body></body></html>";
    struct node *root = dom_parse(html, (int)strlen(html));
    js_page_set_clock(test_clock);
    if (!root || !js_page_open(root)) { check(0, "open JS page"); return; }
    script("var order=[], thisOK=false, argsOK=false, token={};"
           "setTimeout(function(a){'use strict';thisOK=this===window;argsOK=a===token;"
           "order.push('first');Promise.resolve().then(function(){order.push('micro');});"
           "setTimeout(function(){order.push('nested');},0);},10,token);"
           "setTimeout(function(){order.push('second');},10);");
    check(js_page_pending() && js_page_next_due() == 1010, "shipping queue reports pending deadline");
    check(js_page_run_due() == 0, "shipping queue waits for deadline");
    clock_ms = 1010;
    check(js_page_run_due() == 2, "one turn runs original callbacks only");
    expect("thisOK&&argsOK&&order.join(',')==='first,micro,second'", "receiver arguments and microtask order");
    check(js_page_run_due() == 1, "next turn runs callback-created timer");
    expect("order.join(',')==='first,micro,second,nested'", "nested callback order");
    script("var intervalCalls=0;var handle=setInterval(function(){intervalCalls++;clearInterval(handle);},0);");
    clock_ms++;
    check(js_page_run_due() == 1 && !js_page_pending(), "interval self-cancel releases rearmed task");
    expect("intervalCalls===1", "interval invoked once");
    script("var frames=[],frameThis=false;requestAnimationFrame(function(t){'use strict';"
           "frameThis=this===undefined;frames.push(t);requestAnimationFrame(function(t){frames.push(t);});});"
           "var cancelled=requestAnimationFrame(function(){frames.push(-1);});cancelAnimationFrame(cancelled);");
    clock_ms += 16;
    check(js_page_run_due() == 1, "frame cancellation and first frame");
    expect("frameThis&&frames.length===1&&frames[0]===27", "frame receiver and shared clock timestamp");
    check(js_page_run_due() == 0 && js_page_next_due() == 1043, "nested frame waits for next boundary");
    clock_ms += 16;
    check(js_page_run_due() == 1, "nested frame runs next boundary");
    /* Compile the capacity into the script from the production authority;
     * changing the ceiling cannot silently leave a 4096-only fixture green. */
    char capacity_script[512];
    snprintf(capacity_script, sizeof capacity_script,
             "var handles=[];for(var i=0;i<%d;i++)handles.push(setTimeout(function(){},100));"
             "var full=setTimeout(function(){},100);clearTimeout(handles[0]);"
             "var reused=requestAnimationFrame(function(){});", JS_PAGE_TASK_CAPACITY);
    script(capacity_script);
    expect("handles.every(function(x){return x>0;})&&full===0&&reused>0", "shipping queue capacity and reclaimed slot");
    JSContext *ctx = js_page_ctx();
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "recordOld", JS_NewCFunction(ctx, record_old, "recordOld", 0));
    JS_FreeValue(ctx, global);
    script("clearTimeout(handles[1]);setTimeout(recordOld,0);");
    js_page_close();
    check(!js_page_pending() && js_page_next_due() == -1 && js_page_run_due() == 0,
          "closed page exposes no pending callbacks");
    check(js_page_open(root), "open replacement JS page");
    clock_ms += 1000;
    check(js_page_run_due() == 0 && old_page_calls == 0, "old callback never runs after replacement");
    script("var fresh=false;setTimeout(function(){fresh=true;},0);");
    check(js_page_run_due() == 1, "replacement page dispatches its own timer");
    expect("fresh", "replacement callback ran");
    js_page_close();
    dom_free(root);
}
int main(void)
{
    queue_checks();
    page_checks();
    printf("page-runtime: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
