/* Only local in-memory scripts. The diagnostic-on/off builds must execute the
 * same callbacks and leave accessors/Proxy traps untouched. Log checks live in
 * the companion Python reader so turning the hook off cannot pass vacuously. */
#define main worker_existing_suite_main
#include "worker_test.c"
#undef main

static const char *PROMISE_JS =
    "var got=0,traps=0,caught=0,late=false,asyncRan=false;"
    "var typeNames=['Error','EvalError','RangeError','ReferenceError','SyntaxError',"
    "'TypeError','URIError','InternalError','AggregateError'];"
    "function accept(p){p.catch(function(){caught++;});}"
    "typeNames.forEach(function(n){accept(Promise.reject(new self[n]('LOCAL_ONLY_SECRET')));});"
    "var missing=['fetch','WebAssembly','URL','URLSearchParams','TextEncoder','TextDecoder',"
    "'crypto','atob','btoa','Response','Request','Headers'];"
    "missing.forEach(function(n){delete self[n];try{(0,eval)(n);}catch(e){accept(Promise.reject(e));}});"
    "accept(Promise.reject(new ReferenceError(\"'LOCAL_ONLY_SECRET' is not defined\")));"
    "var err=new ReferenceError('LOCAL_ONLY_SECRET');"
    "Object.defineProperty(err,'message',{get:function(){got++;return 'LOCAL_ONLY_SECRET';}});"
    "Object.defineProperty(err,'name',{get:function(){got++;return 'LOCAL_ONLY_SECRET';}});"
    "Object.defineProperty(err,'stack',{get:function(){got++;return 'LOCAL_ONLY_SECRET';}});"
    "accept(Promise.reject(err));"
    "var obj={get name(){got++;},get message(){got++;},get stack(){got++;}};"
    "accept(Promise.reject(obj));"
    "var px=new Proxy(new TypeError('LOCAL_ONLY_SECRET'),{get:function(){traps++;},"
    "getPrototypeOf:function(){traps++;return null;},getOwnPropertyDescriptor:function(){traps++;}});"
    "accept(Promise.reject(px));"
    "var altered=new Error('LOCAL_ONLY_SECRET');Object.setPrototypeOf(altered,px);"
    "accept(Promise.reject(altered));"
    "accept(Promise.reject('LOCAL_ONLY_SECRET'));"
    "var huge=new ReferenceError('LOCAL_ONLY_SECRET'.repeat(10000));accept(Promise.reject(huge));"
    "var p=Promise.reject(new RangeError('LOCAL_ONLY_SECRET'));"
    "setTimeout(function(){p.catch(function(){late=true;});},0);"
    "Promise.resolve().then(function(){asyncRan=true;throw new TypeError('LOCAL_ONLY_SECRET');});"
    "onmessage=function(e){postMessage({phase:'echo',value:e.data});};"
    "setTimeout(function(){postMessage({phase:'done',got:got,traps:traps,caught:caught,"
    "late:late,asyncRan:asyncRan});},5);";
static const char *RESOLVED_JS =
    "Promise.resolve(7).then(function(v){postMessage({phase:'resolved',value:v});});";

int main(void)
{
    fake_site_reset();
    fake_site_add("http://fixture.test/promises.js", PROMISE_JS);
    fake_site_add("http://fixture.test/resolved.js", RESOLVED_JS);
    struct node *root = dom_parse(PAGE, (int)strlen(PAGE));
    if (!root) return 2;
    js_page_set_clock(clock_fn);
    js_page_set_location("http://fixture.test/page.html");
    if (!js_page_open(root)) return 2;
    ctx = js_page_ctx();
    run("var results=[],errors=0,workers=[];"
        "function launch(url){var w=new Worker(url);workers.push(w);"
        "w.onmessage=function(e){results.push(e.data);};w.onerror=function(){errors++;};return w;}"
        "launch('http://fixture.test/promises.js').postMessage('queued');"
        "launch('http://fixture.test/promises.js').postMessage('queued');"
        "launch('http://fixture.test/resolved.js');");
    for (int i = 0; i < 30; ++i) tick(1);
    ckjs("results.filter(x=>x.phase==='done').length===2", "two worker promise scripts finish");
    ckjs("results.filter(x=>x.phase==='done').every(x=>x.got===0)", "diagnostics invoke zero error or object getters");
    ckjs("results.filter(x=>x.phase==='done').every(x=>x.traps===0)", "diagnostics invoke zero Proxy traps");
    ckjs("results.filter(x=>x.phase==='done').every(x=>x.caught===28)", "rejection handlers preserve their original execution count");
    ckjs("results.filter(x=>x.phase==='done').every(x=>x.late&&x.asyncRan)", "late catch and async rejection retain execution semantics");
    ckjs("results.filter(x=>x.phase==='resolved').length===1&&results.find(x=>x.phase==='resolved').value===7", "resolved-only worker remains a rejection-free control");
    ckjs("results.filter(x=>x.phase==='echo'&&x.value==='queued').length===2", "messages queued before startup dispatch normally");
    run("workers[0].postMessage('running');workers[1].postMessage('running');");
    for (int i = 0; i < 10; ++i) tick(1);
    ckjs("results.filter(x=>x.phase==='echo'&&x.value==='running').length===2", "messages queued after startup dispatch normally");
    ckjs("errors===0", "Promise rejection is not changed into a worker error event");
    run("workers.forEach(w=>w.terminate());");
    pump_until_idle(20);
    ck(js_worker_pending() == 0, "diagnostic workers terminate and drain");
    js_page_close();
    dom_free(root);
    printf("worker-promise-diagnostics: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
