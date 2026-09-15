/* SPDX-License-Identifier: MIT
 * Normal API acceptance only: complete, valid addition instances remain
 * reachable for their whole useful lifetime. No GC fault injection, retired
 * object calls, memory views, site code, or network request is involved.
 * Worker startup uses the existing local in-memory script loader.
 */
#define main worker_existing_normal_main
#include "worker_test.c"
#undef main
#include "wasm_js_modules.inc"

int main(void)
{
    char worker[2048];size_t n=0;
    n+=(size_t)snprintf(worker+n,sizeof worker-n,"var bytes=new Uint8Array([");
    for(size_t i=0;i<sizeof W_add;i++)
        n+=(size_t)snprintf(worker+n,sizeof worker-n,"%s%u",i?",":"",W_add[i]);
    n+=(size_t)snprintf(worker+n,sizeof worker-n,
        "]);var module=new WebAssembly.Module(bytes);"
        "var instance=new WebAssembly.Instance(module);"
        "postMessage(instance.exports.add(20,22));"
        "onmessage=function(){postMessage(instance.exports.add(20,22));};");
    if(n>=sizeof worker)return 2;
    fake_site_reset();
    fake_site_add("http://fixture.test/normal-add.js",worker);
    struct node *root=dom_parse(PAGE,(int)strlen(PAGE));
    if(!root)return 2;
    js_page_set_clock(clock_fn);
    js_page_set_location("http://fixture.test/page.html");
    if(!js_page_open(root))return 2;
    ctx=js_page_ctx();
    JSValue g=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,g,"normalAddBytes",JS_NewArrayBufferCopy(ctx,W_add,sizeof W_add));
    JS_FreeValue(ctx,g);
    run("var normalModule=new WebAssembly.Module(normalAddBytes);"
        "var normalInstance=new WebAssembly.Instance(normalModule);"
        "var replies=[[],[]],normalErrors=0,normalWorkers=[];"
        "function startNormal(i){var w=new Worker('http://fixture.test/normal-add.js');"
        "normalWorkers[i]=w;w.onmessage=function(e){replies[i].push(e.data);};"
        "w.onerror=function(){normalErrors++;};}");
    ckjs("normalInstance.exports.add(20,22)===42","parent performs ordinary Wasm addition");
    run("startNormal(0);startNormal(1);");
    pump_until_idle(80);
    ckjs("replies[0].length===1&&replies[0][0]===42","first Worker posts addition result 42");
    ckjs("replies[1].length===1&&replies[1][0]===42","second Worker posts addition result 42");
    ckjs("normalInstance.exports.add(20,22)===42","parent continues addition with both Workers active");
    run("normalWorkers[0].terminate();");
    pump_until_idle(80);
    ckjs("normalInstance.exports.add(20,22)===42","parent continues addition after ordinary Worker termination");
    run("normalWorkers[1].postMessage('calculate');");
    pump_until_idle(80);
    ckjs("replies[1].length===2&&replies[1][1]===42","remaining live Worker continues ordinary addition");
    ckjs("normalErrors===0","normal addition reports no Worker error");
    /* The second Worker is still valid and active when the owning page is
     * closed normally. No page/Worker object is accessed after close. */
    js_page_close();dom_free(root);
    printf("worker-wasm-normal: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
