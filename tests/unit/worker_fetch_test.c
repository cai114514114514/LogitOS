/* Ordinary page + dedicated Worker Fetch, with two synthetic HTTPS origins.
 * Assert API presence before the feature tests so the old implementation
 * fails once by name, without manufacturing secondary timeout failures.
 * The worker startup loader serves only this local script; fetch responses
 * use the production HTTP parser, Cookie, CORS and redirect implementation. */
#define main worker_existing_suite_main
#include "worker_test.c"
#undef main
#include "js_webapi.h"
#include "worker_fetch_net.h"
#ifndef WFT_BASELINE
extern int js_webapi_fetch_test_handle(JSContext *ctx);
extern int js_webapi_fetch_test_abort(JSContext *ctx,int handle);
#endif

static const char *FETCH_JS=
    "var controls={};"
    "postMessage({ready:true,fetch:typeof fetch,doc:typeof document,xhr:typeof XMLHttpRequest});"
    "onmessage=function(e){var q=e.data;"
    "if(q.abort){if(controls[q.tag])controls[q.tag].abort();return;}"
    "if(q.close){close();return;}"
    "var opt={};['method','credentials','redirect','headers','body','mode'].forEach(function(k){if(k in q)opt[k]=q[k];});"
    "if(q.signal){controls[q.tag]=new AbortController();opt.signal=controls[q.tag].signal;}"
    "Promise.resolve().then(function(){return fetch(q.url,opt);}).then(function(r){"
    "return (q.bytes?r.arrayBuffer():r.text()).then(function(v){"
    "if(q.closeAfter){close();postMessage({tag:q.tag,late:true});return;}"
    "postMessage({tag:q.tag,ok:true,value:q.bytes?Array.from(new Uint8Array(v)).join(','):v,status:r.status,doc:typeof document});"
    "}).then(function(){if(q.closeAfter)postMessage({tag:q.tag,lateReaction:true});});"
    "}).catch(function(err){postMessage({tag:q.tag,ok:false,name:err.name});});};";

static void frames(int n){for(int i=0;i<n;i++)tick(1);}
static void command(int which,const char *object)
{char js[1800];snprintf(js,sizeof js,"workers[%d].postMessage(%s);",which,object);run(js);}
static void finish(struct node *root)
{js_page_close();dom_free(root);}
static void check_result(const char *tag,const char *condition,const char *name)
{char js[1024];snprintf(js,sizeof js,"results.some(function(r){return r.tag==='%s'&&(%s);})",tag,condition);ckjs(js,name);}

int main(void)
{
    fake_site_reset();wft_net_reset();
    fake_site_add("https://page.test/workers/entry.js",FETCH_JS);
    struct node *root=dom_parse(PAGE,(int)strlen(PAGE));if(!root)return 2;
    js_page_set_clock(clock_fn);js_page_set_location("https://page.test/page/index.html");
    if(!js_page_open(root)){dom_free(root);return 2;}ctx=js_page_ctx();
    run("var results=[],workers=[],pageResult='',parentFetch=fetch,parentLocation=location.href;"
        "function launch(src){var w=new Worker(src);workers.push(w);w.onmessage=function(e){results.push(e.data);};"
        "w.onerror=function(){results.push({workerError:true});};return w;}"
        "launch('https://page.test/workers/entry.js');launch('https://page.test/workers/entry.js');");
    frames(30);
    ckjs("results.filter(r=>r.ready&&r.fetch==='function').length===2","both dedicated Workers expose fetch");
    if(failures){finish(root);printf("worker-fetch: %d checks, %d failures\n",checks,failures);return 1;}
    ckjs("results.filter(r=>r.ready).every(r=>r.doc==='undefined'&&r.xhr==='undefined')","fetch-only install does not publish page DOM or XHR");
    run("fetch('/page/text').then(r=>r.text()).then(v=>pageResult=v);");
    command(0,"{tag:'text',url:'text'}");command(1,"{tag:'bytes',url:'/bytes',bytes:true}");frames(80);
    ckjs("pageResult==='page-text'&&fetch===parentFetch&&location.href===parentLocation","concurrent worker requests preserve page response and globals");
    check_result("text","r.ok&&r.value==='worker-text'&&r.doc==='undefined'","worker text resolves relative to script URL");
    check_result("bytes","r.ok&&r.value==='65,66,67,68'","other worker receives its own arrayBuffer bytes");
    ck(wft_net_requests("page.test","/workers/text","GET")==1,"worker base differs from parent document path");
#ifdef WFT_LIVENESS_ONLY
    finish(root);printf("worker-fetch: %d checks, %d failures\n",checks,failures);return failures?1:0;
#endif

    command(0,"{tag:'cors-ok',url:'https://peer.test/ok'}");
    command(1,"{tag:'cors-deny',url:'https://peer.test/cors-denied'}");frames(80);
    check_result("cors-ok","r.ok&&r.value==='ok'","worker cross-origin response honors valid CORS");
    check_result("cors-deny","!r.ok&&r.name==='TypeError'","worker rejects response without CORS permission");
    ck(wft_net_header("peer.test","/ok","Origin: https://page.test\r\n"),"worker CORS initiator is creator origin");

    command(0,"{tag:'set-cookie',url:'https://peer.test/set',credentials:'include'}");frames(60);
    command(0,"{tag:'omit-cookie',url:'https://peer.test/omit-cookie'}");frames(60);
    ck(!wft_net_header("peer.test","/omit-cookie","Cookie:"),"worker default credentials omit cross-origin cookies");
    command(1,"{tag:'include-cookie',url:'https://peer.test/include-cookie',credentials:'include'}");frames(60);
    check_result("include-cookie","r.ok","worker credentialed CORS fetch completes");
    ck(wft_net_header("peer.test","/include-cookie","remote_cookie=fixture"),"worker include credentials uses shared valid cookie jar");
    command(0,"{tag:'put',url:'https://peer.test/put',method:'PUT',headers:{'X-Local':'fixture'},body:'local'}");frames(100);
    check_result("put","r.ok","worker non-simple request completes after preflight");
    ck(wft_net_requests("peer.test","/put","OPTIONS")==1&&wft_net_requests("peer.test","/put","PUT")==1,"worker preflight and actual request both use shared policy path");
    command(1,"{tag:'redirect',url:'/redirect',headers:{Authorization:'local-fixture'}}");frames(100);
    check_result("redirect","r.ok","worker follows cross-origin redirect with valid CORS");
    ck(!wft_net_header("peer.test","/final","Authorization:"),"cross-origin worker redirect drops author authorization");
    ck(wft_net_header("peer.test","/final","Origin: https://page.test\r\n"),"worker redirect preserves request origin classification");

    command(0,"{tag:'a-held',url:'/hold/a',signal:true}");
    command(1,"{tag:'b-held',url:'/hold/b'}");
    run("var pageHeld='';fetch('/hold/page').then(r=>r.text()).then(v=>pageHeld=v).catch(e=>pageHeld=e.name);");frames(60);
    ck(wft_net_live()==3,"page and two Workers keep independent requests in flight");
    ck(js_page_pending()&&js_worker_pending(),"worker fetch activity remains visible to the shared scheduler");
    command(0,"{tag:'a-held',abort:true}");frames(30);
    check_result("a-held","!r.ok&&r.name==='AbortError'","worker AbortController rejects its own pending body");
    ck(wft_net_live()==2,"aborting one worker preserves peer and page transports");

#ifndef WFT_BASELINE
    /* Only native host hooks know opaque request handles. A foreign caller is
     * rejected without touching transport or invoking callbacks in its realm.
     * No web page receives a handle, and the negative keeps original owner
     * cleanup so it cannot turn a routing check into a wrong-runtime free. */
    int peer_handle=js_webapi_fetch_test_handle(NULL);
    ck(peer_handle>=0,"owner fixture identifies one pending worker request");
    int refused=js_webapi_fetch_test_abort(ctx,peer_handle);
    ck(refused==0,"page owner cannot abort a worker request handle");
#endif
    wft_net_release("/hold/b","BBBB");wft_net_release("/hold/page","PPPP");frames(60);
    check_result("b-held","r.ok&&r.value==='BBBB'","peer body survives foreign-owner abort attempt");
    ckjs("pageHeld==='PPPP'","page body survives worker abort and peer completion");

    /* A Blob URL has an opaque resolution base but the creator's origin and
     * site-for-cookies. Absolute fetch works; relative fetch must reject. */
    JSValue g=JS_GetGlobalObject(ctx);JS_SetPropertyStr(ctx,g,"workerSource",JS_NewString(ctx,FETCH_JS));JS_FreeValue(ctx,g);
    run("document.cookie='strict_cookie=fixture; Secure; SameSite=Strict; Path=/';"
        "var blobURL=URL.createObjectURL(new Blob([workerSource],{type:'text/javascript'}));launch(blobURL);URL.revokeObjectURL(blobURL);");frames(40);
    command(2,"{tag:'blob-relative',url:'relative'}");
    command(2,"{tag:'blob-absolute',url:'https://page.test/blob-ok'}");frames(80);
    check_result("blob-relative","!r.ok&&r.name==='TypeError'","Blob worker relative fetch rejects opaque URL base");
    check_result("blob-absolute","r.ok","Blob worker absolute fetch survives object URL revocation");
    ck(wft_net_header("page.test","/blob-ok","strict_cookie=fixture"),"Blob worker credentials use creator site instead of opaque base");
    ck(wft_net_requests("page.test","/page/relative",NULL)==0&&wft_net_requests("page.test","/workers/relative",NULL)==0,"Blob relative fetch never falls back to parent or script directory");

    command(1,"{tag:'terminated',url:'/hold/terminate'}");frames(40);
    ck(wft_net_live()==1,"terminate fixture has one worker-owned transport");
    run("workers[1].terminate();");frames(30);
    ck(wft_net_live()==0,"terminate closes the worker-owned transport");
    ckjs("!results.some(r=>r.tag==='terminated')","terminated worker produces no delayed parent callback");
    command(2,"{tag:'self-closed',url:'https://page.test/hold/self-close'}");frames(40);
    ck(wft_net_live()==1,"Blob self-close fixture reaches a real held transport");
    command(2,"{close:true}");frames(30);
    ck(wft_net_live()==0,"worker close releases its pending fetch resources");
    ckjs("!results.some(r=>r.tag==='self-closed')","self-closed worker produces no delayed parent callback");
    run("launch('https://page.test/workers/entry.js');");frames(30);
    command(3,"{tag:'reaction-close',url:'/bytes',closeAfter:true}");frames(60);
    ck(wft_net_requests("page.test","/bytes","GET")==2,"close-in-reaction fixture completes its real fetch");
    ckjs("!results.some(r=>r.tag==='reaction-close')","close inside fetch reaction prevents same-callback and later-reaction delivery");
    run("fetch('/page/text').then(r=>r.text()).then(v=>pageResult=v);workers[0].terminate();");frames(60);
    ckjs("pageResult==='page-text'&&!results.some(r=>r.workerError)","page continues after independent worker teardown");
    ck(!js_worker_pending(),"worker task and fetch queues drain after teardown");
    run("launch('https://page.test/workers/entry.js');");frames(30);
    command(4,"{tag:'navigation-worker',url:'/hold/navigation-worker'}");
    run("fetch('/hold/navigation-page').then(r=>r.text()).then(function(){results.push({navigationLate:true});});");frames(40);
    ck(wft_net_live()==2,"page-close fixture holds one page and one worker response");
    finish(root);ck(wft_net_live()==0,"page close releases all remaining network ownership");
    printf("worker-fetch: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
