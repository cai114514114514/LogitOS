/* Drive the shipping timer queue with a deterministic clock. Ordinary one-shot
 * timers used to read their freed queue entry even with profiling disabled;
 * ASan is essential here because correct JS output alone misses that defect.
 * This is a runtime unit gate, not evidence of a usable guest web page. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dom.h"
#include "js_page.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void kfree(void *p) { free(p); }
static unsigned long long clock_ms = 1000;
static unsigned long long test_clock(void) { return clock_ms; }
static int checks, failures;
static int fake_fetch_pending, fake_fetch_checkpoints;

/* Strong test providers replace js_page.c's optional weak Web API stubs. */
int js_webapi_pending(void) { return fake_fetch_pending; }
int js_webapi_pump(JSContext *ctx) { (void)ctx; return 0; }
int js_webapi_fetch_checkpoint(JSContext *ctx)
{
    (void)ctx;
    fake_fetch_checkpoints++;
    return 0;
}

static JSValue burn_clock(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    clock_ms += 20;
    return JS_UNDEFINED;
}
static void check(int ok, const char *name)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}
static void script(const char *src)
{
    check(js_page_eval(src, (int)strlen(src), "<timer-test>", 0), "script evaluation");
}
static void expect(const char *expr, const char *name)
{
    JSContext *ctx = js_page_ctx();
    JSValue v = JS_Eval(ctx, expr, strlen(expr), "<timer-assert>", JS_EVAL_TYPE_GLOBAL);
    int ok = !JS_IsException(v) && JS_ToBool(ctx, v) == 1;
    if (JS_IsException(v)) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, v);
    check(ok, name);
}
static int tick(unsigned long long now)
{
    clock_ms = now;
    return js_page_run_due();
}

int main(int argc, char **argv)
{
    setvbuf(stdout, 0, _IONBF, 0);
    puts("page-timers: shipping queue entered");
    const char *html = "<html><body></body></html>";
    struct node *root = dom_parse(html, (int)strlen(html));
    js_page_set_clock(test_clock);
    if (!root || !js_page_open(root)) return 2;
    JSContext *ctx = js_page_ctx();
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "burnClock",
                      JS_NewCFunction(ctx, burn_clock, "burnClock", 0));
    JS_FreeValue(ctx, global);

    /* This interval is rearmed before dispatch, so it isolates callback-this
     * from the separate freed-entry bug in one-shot timers. */
    script("var intervalThis=false, calls=0;"
           "var interval=setInterval(function(){'use strict';"
           "intervalThis=this===window;calls++;clearInterval(interval);},10);");
    check(tick(1010) == 1, "interval runs once");
    expect("intervalThis && calls===1", "strict interval receives window");
    check(!js_page_pending(), "self-cancel interval stays cancelled");
    if (argc > 1 && !strcmp(argv[1], "--interval")) goto done;

    script("var token={}, seenArgs=false, timeoutThis=false;"
           "setTimeout(function(a,b,c){'use strict';timeoutThis=this===window;"
           "seenArgs=a===token&&b===42&&c==='tail';},5,token,42,'tail');");
    check(tick(1014) == 0, "timeout does not run early");
    check(tick(1015) == 1, "one-shot callback survives queue removal");
    expect("timeoutThis", "strict timeout receives window");
    expect("seenArgs", "extra arguments survive queue removal");
    check(!js_page_pending(), "one-shot removed after firing");

    script("var frameThis=false, frameStamp=-1, cancelled=false;"
           "requestAnimationFrame(function(t){'use strict';"
           "frameThis=this===undefined;frameStamp=t;});"
           "var cancelledFrame=requestAnimationFrame(function(){cancelled=true;});"
           "cancelAnimationFrame(cancelledFrame);");
    check(tick(1031) == 1, "only uncancelled frame fires");
    /* HTML's animation callback invocation omits callback-this; unlike timers,
     * Web IDL defaults it to undefined. Strict mode makes the distinction real. */
    expect("frameThis && frameStamp===31 && !cancelled", "frame receiver and timestamp");

    script("var order=[];setTimeout(function(){order.push('outer');"
           "Promise.resolve().then(function(){order.push('microtask');});"
           "setTimeout(function(){order.push('inner');},0);},0);");
    check(tick(1031) == 1, "nested timer waits for next pump");
    expect("order.join(',')==='outer,microtask'", "microtasks drain before next timer");
    check(tick(1031) == 1, "next pump runs nested timer");
    expect("order.join(',')==='outer,microtask,inner'", "nested timer order");

    script("var boundThis=false, arrowThis=false, receiver={};"
           "setTimeout((function(){'use strict';boundThis=this===receiver;}).bind(receiver),0);"
           "(function(){'use strict';setTimeout(()=>{arrowThis=this===receiver;},0);}).call(receiver);");
    check(tick(1031) == 2, "bound and arrow callbacks run");
    expect("boundThis && arrowThis", "explicit and lexical receivers preserved");

    script("var afterThrow=false;setTimeout(function(){throw new Error('expected timer error');},0);"
           "setTimeout(function(){afterThrow=true;},0);");
    check(tick(1031) == 2, "exception does not stop later timer");
    expect("afterThrow", "later callback survives exception");

    /* The first callback crosses the deterministic 8 ms turn budget while a
     * second callback remains in the same timer snapshot.  The next entry
     * therefore resumes at the timer phase.  A live fetch must still receive
     * a socket checkpoint on that entry instead of waiting for the whole
     * timer backlog to drain. */
    fake_fetch_pending = 1;
    fake_fetch_checkpoints = 0;
    script("var fairness=[];"
           "setTimeout(function(){fairness.push('slow');burnClock();},0);"
           "setTimeout(function(){fairness.push('tail');},0);");
    check(tick(clock_ms) == 1, "budgeted timer batch yields with work remaining");
    check(fake_fetch_checkpoints == 0, "first phase uses the ordinary Web API pump");
    check(tick(clock_ms) == 1, "resumed timer batch completes");
    check(fake_fetch_checkpoints == 1, "resumed turn checkpoints pending fetches");
    expect("fairness.join(',')==='slow,tail'", "timer snapshot order survives fetch checkpoint");
    fake_fetch_pending = 0;
    script("setTimeout(function(){throw new Error('closed page timer');},100);");
done:
    js_page_close();
    check(!js_page_pending(), "page close clears timers");
    dom_free(root);
    printf("page-timers: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
