/* Keep the parent's real optional installers linked. A core child must not
 * install, pump or close those singleton services on its parent's behalf. */
#define main dom_iface_original_main
#include "dom_iface_test.c"
#undef main
static unsigned long long page_now;
static unsigned long long page_clock(void) { return page_now; }
static void run(const char *script,const char *label)
{
    checks++;
    if (!js_page_eval(script,strlen(script),"<page-context>",0)) {
        printf("FAIL: %s\n",label); fails++;
    }
}
static void expect(const char *expr,const char *label)
{
    char script[2048]; snprintf(script,sizeof script,"if(!(%s))throw Error('assertion');",expr);
    run(script,label);
}
static void native_check(int ok,const char *label)
{ checks++; if(!ok){printf("FAIL: %s\n",label);fails++;} }
static JSValue close_in_js(JSContext *ctx,JSValueConst t,int argc,JSValueConst *argv)
{
    (void)t;(void)argc;(void)argv;
    js_page_close();
    return JS_NewBool(ctx,js_page_ctx()==ctx && js_page_cancel_requested());
}
int main(void)
{
    js_page_set_clock(page_clock);
    const char *html="<body><button id='b'>live</button></body>";
    struct node *parent=dom_parse(html,strlen(html));
    js_page_set_location("https://parent.example/");
    if(!parent || !js_page_open(parent))return 2;
    run("var ticks=0;var tid=setTimeout(function(){ticks++},10);"
        "var parentDoc=document;var parentFetch=fetch;var parentHistory=history;"
        "console.log('parent-log');", "parent schedules timer with real platform installed");
    const char *parent_output=js_page_output();
    native_check(strstr(parent_output,"parent-log")!=0,"parent console records output");
    struct js_page_context *child=js_page_context_create(),*previous=(void*)1;
    native_check(child && js_page_context_activate(child,&previous) && !previous,
                 "child activation preserves full parent");
    struct node *root=dom_parse(html,strlen(html)); page_now=2;
    js_page_set_location("https://child.example/");
    if(!root || !js_page_open(root))return 2;
    expect("typeof fetch==='undefined' && typeof Worker==='undefined' &&"
           "typeof history==='undefined' && typeof setTimeout==='function' &&"
           "document.body instanceof HTMLBodyElement",
           "core child exposes real DOM and timers, not shared parent extensions");
    run("var ticks=0,raf=0,job=0;var tid=setTimeout(function(){ticks+=100},5);"
        "clearTimeout(tid);setTimeout(function(){ticks++;Promise.resolve().then(function(){job++})},7);"
        "requestAnimationFrame(function(){raf++});console.log('child-log');",
        "child uses ordinary timer, rAF and microtask scheduler");
    native_check(!js_page_context_destroy(child),"live child context cannot be destroyed");
    native_check(strstr(js_page_output(),"child-log") && !strstr(js_page_output(),"parent-log"),
                 "child console does not share parent buffer");
    js_page_context_activate(0,0);
    expect("document===parentDoc && fetch===parentFetch && history===parentHistory && ticks===0",
           "parent document and optional services survive child install");
    native_check(strstr(js_page_output(),"parent-log") && !strstr(js_page_output(),"child-log"),
                 "parent console survives child output");
    native_check(js_page_pending(),"parent timer queue still pending");
    page_now=10; js_page_run_due();
    expect("ticks===1", "parent timer survives child queue initialization and cancellation");
    js_page_context_activate(child,0);
    expect("ticks===0 && job===0", "parent pump does not execute child tasks");
    js_page_run_due();
    expect("ticks===1 && job===1", "child timer and its microtask run in child");
    page_now=32; js_page_run_due();
    expect("raf===1", "child animation-frame callback is pumped");

    struct js_page_context *sibling=js_page_context_create();
    native_check(sibling && js_page_context_activate(sibling,&previous) && previous==child,
                 "second child coexists with first child and parent");
    struct node *sroot=dom_parse(html,strlen(html));
    if(!sroot || !js_page_open(sroot))return 2;
    run("var siblingTicks=0;setTimeout(function(){siblingTicks++},1);", "sibling schedules its own timer");
    page_now=33;js_page_run_due();
    expect("siblingTicks===1 && typeof ticks==='undefined'", "sibling timer runs only in sibling");
    js_page_close();js_page_context_activate(child,0);
    native_check(js_page_context_destroy(sibling),"destroy inactive closed sibling");dom_free(sroot);
    expect("ticks===1 && job===1 && raf===1 && typeof siblingTicks==='undefined'",
           "inactive sibling destruction restores first child");

    /* Navigation preserves queue ADDRESS but advances its epoch. Work queued
     * before close must never run in the reopened document, even though its
     * integer timer id and allocator addresses may be reused. */
    run("setTimeout(function(){throw Error('stale child task')},1);", "queue pre-navigation child task");
    js_dom_note_activation();
    js_page_close();
    if(!js_page_open(root))return 2;
    native_check(!js_dom_has_activation(),"reopened document does not inherit old activation");
    run("var fresh=0;setTimeout(function(){fresh++},1);", "reopened child schedules fresh task");
    page_now=40; js_page_run_due();
    expect("fresh===1 && typeof ticks==='undefined'", "navigation discards old tasks and old globals");
    native_check(!strstr(js_page_output(),"stale child task"),"no stale callback executed after reopen");
    js_page_set_slice_fuel(1);
    native_check(!js_page_eval("for(;;){}",9,"<expected-child-interrupt>",0),
                 "child watchdog interrupts infinite script");
    native_check(js_page_slice_hits()>0,"child has its own watchdog counters");
    js_page_context_activate(0,0);
    native_check(!js_page_cancel_requested() && js_page_slice_hits()==0,
                 "child interrupt state does not contaminate parent");
    expect("ticks===1 && fetch===parentFetch", "parent remains executable after child watchdog");
    js_page_context_activate(child,0);
    JSContext *ctx=js_page_ctx(); JSValue global=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,global,"closeInJS",JS_NewCFunction(ctx,close_in_js,"closeInJS",0));
    JS_FreeValue(ctx,global);
    expect("closeInJS()", "reentrant close cancels without freeing executing runtime");
    native_check(js_page_ctx()==ctx && !js_page_entry_active(),"cancelled child unwinds before cleanup");
    js_page_close();
    native_check(js_page_context_destroy(child),"closed child can be destroyed");
    dom_free(root);
    expect("document===parentDoc && fetch===parentFetch && history===parentHistory",
           "child destruction restores parent and leaves platform open");
    run("document.getElementById('b').addEventListener('click',function(){ticks++});"
        "document.getElementById('b').dispatchEvent(new Event('click'));",
        "parent DOM events still run after child destruction");
    expect("ticks===2", "parent listener updated parent state");
    js_page_close(); dom_free(parent);
    printf("Page contexts: %d checks, %d failures\n",checks,fails);
    return fails?1:0;
}
