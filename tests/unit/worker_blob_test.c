/* A Blob worker must use the creator's live object-URL registry, then own a
 * source snapshot. The ordinary HTTP echo is a positive loader control. */
#define main worker_existing_main
#include "worker_test.c"
#undef main
#include "js_webapi.h"
#ifdef JS_WORKER_BLOB_ALLOC_AUDIT
static void *pinned[64];static int pin_count;
void worker_blob_test_allocated(void *p){for(int i=0;i<64;i++)if(!pinned[i]){pinned[i]=p;pin_count++;return;}abort();}
void worker_blob_test_free(void *p){if(p)for(int i=0;i<64;i++)if(pinned[i]==p){pinned[i]=0;pin_count--;break;}free(p);}
#endif

int main(void)
{
    fake_site_reset();fake_site_add("http://fixture.test/echo.js",ECHO_JS);
    fake_site_add("http://fixture.test/math.js","self.answer=6*7;");
    struct node *root=dom_parse(PAGE,(int)strlen(PAGE));
    js_page_set_clock(clock_fn);js_page_set_location("http://fixture.test/page.html");
    if(!root||!js_page_open(root))return 2;ctx=js_page_ctx();
#ifdef JS_WORKER_TEST_START_OOM
    run("var oom=0;for(var i=0;i<12;i++){var ou=URL.createObjectURL(new Blob(['postMessage(42)']));try{new Worker(ou)}catch(e){if(e.message.indexOf('out of memory')>=0)oom++}URL.revokeObjectURL(ou)}");
    ckjs("oom===12","startup-task OOM rejects every constructor without exhausting slots");
    ck(js_worker_pending()==0&&pin_count==0,"startup-task OOM releases all pinned source and task state");
    js_page_close();dom_free(root);return failures?1:0;
#endif
    run("var control=[],hw=new Worker('http://fixture.test/echo.js');hw.onmessage=function(e){control.push(e.data)};hw.postMessage('hi');");
    pump_until_idle(100);
    ckjs("control.join(',')==='ready,echo:hi'","HTTP worker control still exchanges real messages");
    run("if(hw)hw.terminate();var seen=[],ctorError='',blobCode=\"postMessage({kind:'ready',href:location.href,origin:origin,locOrigin:location.origin,noDOM:typeof document==='undefined'});onmessage=function(e){postMessage({kind:'echo',answer:e.data.n*7})}\";"
        "var u=URL.createObjectURL(new Blob([blobCode],{type:'text/javascript'})),bw;"
        "try{bw=new Worker(u);bw.onmessage=function(e){seen.push(e.data)};bw.postMessage({n:6});}catch(e){ctorError=e.name}URL.revokeObjectURL(u);");
    pump_until_idle(100);
    ckjs("ctorError===''&&seen.length===2","Blob worker starts and echoes after immediate URL revocation");
    ckjs("seen[0].kind==='ready'&&seen[0].href===u&&seen[0].origin==='http://fixture.test'&&seen[0].locOrigin===seen[0].origin&&seen[0].noDOM","Blob worker keeps URL and creator origin in its isolated global");
    ckjs("seen[1].answer===42","Blob worker processes cloned parent input");
    run("var stringified=false,fu=URL.createObjectURL(new Blob([\"postMessage(String(location)===location.href&&new URL('http://fixture.test/math.js',location).href==='http://fixture.test/math.js')\"])),lw;try{lw=new Worker('BLOB:'+fu.slice(5)+'#label');lw.onmessage=function(e){stringified=e.data}}catch(e){}URL.revokeObjectURL(fu);");
    pump_until_idle(100);
    ckjs("stringified===true","Blob scheme/fragment resolution and WorkerLocation stringifier agree");
    run("if(lw)lw.terminate();var sharedURL=URL.createObjectURL(new Blob(['same-table'])),fetchedBlob='';fetch(sharedURL).then(function(r){return r.text()}).then(function(t){fetchedBlob=t});");
    pump_until_idle(100);ckjs("fetchedBlob==='same-table'","fetch and Worker share the same object URL registry");
    JSValue shared=eval("sharedURL");const char *shared_name=JS_ToCString(ctx,shared);
    JSRuntime *foreign_rt=JS_NewRuntime();JSContext *foreign_ctx=JS_NewContext(foreign_rt);
    unsigned char *copy=0;int copy_len=0;char creator[160];
    ck(js_webapi_blob_snapshot(foreign_ctx,shared_name,&copy,&copy_len,1024,creator,sizeof creator)==0&&!copy,"foreign realm cannot dereference creator Blob table");
    JS_FreeContext(foreign_ctx);JS_FreeRuntime(foreign_rt);JS_FreeCString(ctx,shared_name);JS_FreeValue(ctx,shared);
    run("URL.revokeObjectURL(sharedURL);");
    run("if(bw)bw.terminate();var revokedCtor='',revokedErrors=0,rw;try{rw=new Worker(u);rw.onerror=function(){revokedErrors++}}catch(e){revokedCtor=e.name}");
    pump_until_idle(100);
    ckjs("revokedCtor===''&&revokedErrors===1","revocation before construction produces one deferred error");
    run("var badCtor='',badErrors=0,fw;try{fw=new Worker('blob:http://other.test/not-owned');fw.onerror=function(){badErrors++}}catch(e){badCtor=e.name}");
    pump_until_idle(100);
    ckjs("badCtor===''&&badErrors===1","a fabricated foreign Blob URL never executes a script");
    run("var imported=0,importError='',iu=URL.createObjectURL(new Blob([\"importScripts('http://fixture.test/math.js');postMessage(answer)\"])),iw;try{iw=new Worker(iu);iw.onmessage=function(e){imported=e.data}}catch(e){importError=e.name}URL.revokeObjectURL(iu);");
    pump_until_idle(100);
    ckjs("importError===''&&imported===42","Blob script can import an absolute ordinary local script");
    run("if(iw)iw.terminate();var cancelled=0,tu=URL.createObjectURL(new Blob([\"postMessage('unexpected')\"])),tw;try{tw=new Worker(tu);tw.onmessage=function(){cancelled++};tw.terminate()}catch(e){}URL.revokeObjectURL(tu);");
    pump_until_idle(100);
    ckjs("cancelled===0","terminate before deferred start drops Blob code and messages");
    ck(js_worker_pending()==0,"Blob worker queue is quiescent after completion and termination");
    js_page_close();dom_free(root);
    root=dom_parse(PAGE,(int)strlen(PAGE));if(!root||!js_page_open(root))return 2;ctx=js_page_ctx();
    run("var oldURL=URL.createObjectURL(new Blob([\"postMessage('old')\"]));");
    JSValue old=eval("oldURL");const char *s=JS_ToCString(ctx,old);char stale[600];snprintf(stale,sizeof stale,"%s",s?s:"");if(s)JS_FreeCString(ctx,s);JS_FreeValue(ctx,old);
    js_page_close();dom_free(root);
    root=dom_parse(PAGE,(int)strlen(PAGE));if(!root||!js_page_open(root))return 2;ctx=js_page_ctx();
    JSValue g=JS_GetGlobalObject(ctx);JS_SetPropertyStr(ctx,g,"oldURL",JS_NewString(ctx,stale));JS_FreeValue(ctx,g);
    run("var fresh=URL.createObjectURL(new Blob([\"postMessage('new')\"]));");
    ckjs("fresh!==oldURL","new page object URLs cannot resurrect a retired page URL");
    run("var staleErrors=0,staleRun=0,sw;try{sw=new Worker(oldURL);sw.onerror=function(){staleErrors++};sw.onmessage=function(){staleRun++}}catch(e){}URL.revokeObjectURL(fresh);");
    pump_until_idle(100);
    ckjs("staleErrors===1&&staleRun===0","retired realm Blob URLs stay unavailable to new page workers");
    /* Do not pump: this is exactly constructor -> page close before START.
     * The allocation audit follows actual free(), not a cleared state flag. */
    JS_FreeValue(ctx,eval("var pu=URL.createObjectURL(new Blob(['postMessage(42)']));try{new Worker(pu)}catch(e){}"));
    js_page_close();dom_free(root);
#ifdef JS_WORKER_BLOB_ALLOC_AUDIT
    ck(pin_count==0,"page close before startup releases every pinned Blob allocation");
#endif
    printf("worker-blob: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
