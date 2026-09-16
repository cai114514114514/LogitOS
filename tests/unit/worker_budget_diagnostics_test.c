/* SPDX-License-Identifier: MIT
 * An advancing synthetic clock makes a finite-cost test hit the unchanged
 * time rail. This is a diagnostic apparatus test, not a speed measurement. */
#define main old_worker_main
#include "worker_test.c"
#undef main
static int advancing,clock_reads;
/* A jump on EVERY host clock read starves the outer 12 ms scheduler before
 * it can start the worker. Permit ordinary scheduler entries between jumps;
 * keep advancing until the worker is terminal, never freeze a queued loop. */
static unsigned long long budget_clock(void){if(advancing&&++clock_reads%32==0)g_now+=1000;return g_now;}
static JSValue page_clock_step(JSContext *c,JSValueConst self,int argc,JSValueConst *argv)
{(void)c;(void)self;(void)argc;(void)argv;g_now+=250;return JS_UNDEFINED;}
int main(void)
{
    fake_site_reset();js_page_set_clock(budget_clock);
    struct node *root=dom_parse(PAGE,strlen(PAGE));
    js_page_set_location("https://worker.example/budget");
    if(!root||!js_page_open(root))return 2;ctx=js_page_ctx();
    run("var done=0,errs=0,u=URL.createObjectURL(new Blob(['while(true){};postMessage(1)']));"
        "var w=new Worker(u);w.onmessage=()=>done++;w.onerror=()=>errs++;URL.revokeObjectURL(u);");
    advancing=1;for(int i=0;i<50&&js_worker_pending();i++)tick(1);
    advancing=0;for(int i=0;i<8;i++)tick(1);
    ckjs("done===0&&errs===0","watchdog still stops runaway evaluation without author error dispatch");
    ck(!js_worker_pending(),"interrupted Worker leaves no queued tasks");
    run("var finite=0;u=URL.createObjectURL(new Blob(['Promise.resolve().then(function(){var n=0;for(var i=0;i<1000000;i++)n+=i;postMessage(n)})']));"
        "w=new Worker(u);w.onmessage=function(e){finite=e.data};URL.revokeObjectURL(u);");
    advancing=1;for(int i=0;i<50&&js_worker_pending();i++)tick(1);
    advancing=0;for(int i=0;i<8;i++)tick(1);
    ckjs("finite===499999500000","finite Promise job completes without changing watchdog allowance");
    JSValue global=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,global,"localClockStep",JS_NewCFunction(ctx,page_clock_step,"localClockStep",0));
    JS_FreeValue(ctx,global);
    run("var pageDone=0;Promise.resolve().then(function(){localClockStep();pageDone=1})");
    ckjs("pageDone===1","page timing observes a real completed Promise reaction");
    js_page_close();dom_free(root);
    printf("worker-budget-diagnostics: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
