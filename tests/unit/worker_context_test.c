/* SPDX-License-Identifier: MIT
 * Two real document runtimes, each with its own native Worker runtime and
 * queue. Reuse the existing Worker harness/transport instead of duplicating
 * its socket/runtime stubs. The negative keeps both documents but selects
 * the former shared worker table, proving effects and teardown ownership. */
#define main legacy_worker_main
#include "worker_test.c"
#undef main
#include "js_webapi.h"
#include "bfetch.h"
#include "worker_fetch_net.h"

struct policy_state { int deny_create, loads, denied_connect, entries, releases, response_loads, allow_eval; };
struct response_state {struct policy_state *owner;};
static int response_allow(void *opaque,int op,const char *url)
{
    (void)opaque;
    if(op==JSW_EVAL)return 0;
    if(op==JSW_IMPORT)return url&&!strcmp(url,"https://child.example/final/library.js");
    return op==JSW_CONNECT&&url&&!strcmp(url,"https://child.example/final/net-body");
}
static int response_load(void *opaque,const char *url,unsigned char **bytes,int *length)
{
    struct response_state *r=opaque;r->owner->response_loads++;
    return bfetch_sync(url,bytes,length);
}
static void response_release(void *opaque)
{struct response_state *r=opaque;r->owner->releases++;free(r);}
static int load_entry(void *opaque,char *url,int cap,unsigned char **bytes,int *length,struct js_worker_policy *out)
{
    struct policy_state *p=opaque;p->entries++;
    if(strcmp(url,"https://child.example/entry.js"))return -1;
    if(cap<64||bfetch_sync(url,bytes,length))return -1;
    struct response_state *r=calloc(1,sizeof *r);
    if(!r){free(*bytes);*bytes=0;return -1;}r->owner=p;
    strcpy(url,"https://child.example/final/entry.js");
    *out=(struct js_worker_policy){r,response_allow,response_load,0,0,response_release};return 0;
}
static int allow_worker(void *opaque,int op,const char *url)
{
    struct policy_state *p=opaque;
    if(op==JSW_CREATE)return !p->deny_create;
    if(op==JSW_EVAL)return p->allow_eval;
    if(op==JSW_CONNECT){
        if(url&&!strcmp(url,"https://child.example/worker-body"))return 1;
        p->denied_connect++;return 0;
    }
    return url&&!strcmp(url,"https://child.example/library.js");
}
static int load_worker(void *opaque,const char *url,unsigned char **bytes,int *length)
{
    struct policy_state *p=opaque;p->loads++;
    return bfetch_sync(url,bytes,length);
}
static struct node *open_document(const char *url)
{
    struct node *root=dom_parse(PAGE,strlen(PAGE));
    js_page_set_location(url);if(!root||!js_page_open(root))exit(2);
    ctx=js_page_ctx();return root;
}
int main(void)
{
    fake_site_reset();wft_net_reset();
    fake_site_add("https://child.example/library.js","var imported=17;");
    fake_site_add("https://child.example/final/library.js","var imported=29;");
    fake_site_add("https://child.example/entry.js",
        "var port=new MessageChannel();port.port2.onmessage=function(e){postMessage(e.data)};"
        "port.port1.postMessage('worker-port-ready');"
        "var blocked=false;try{eval('1')}catch(e){blocked=e.name==='EvalError'};"
        "importScripts('./library.js');fetch('./net-body',{credentials:'include'}).then(r=>r.text()).then(function(s){"
        "postMessage(blocked&&imported===29&&s==='ok'?'network-ready':'bad-policy')});");
    js_page_set_clock(clock_fn);
    /* Seed through a first-party document. A cross-site child correctly
     * cannot create or read a SameSite=Strict cookie in the first place. */
    struct node *seed=open_document("https://child.example/setup");
    run("document.cookie='strict_cookie=fixture;Secure;SameSite=Strict;Path=/';");
    ckjs("document.cookie.indexOf('strict_cookie=fixture')>=0",
         "first-party cookie fixture is present before embedding");
    js_page_close();dom_free(seed);
    struct node *parent=open_document("https://parent.example/page");
    run("var events=[];var u=URL.createObjectURL(new Blob(["
        "'postMessage(1);onmessage=function(e){postMessage(e.data+1)}'"
        "],{type:'text/javascript'}));var w=new Worker(u);"
        "w.onmessage=function(e){events.push(e.data)};URL.revokeObjectURL(u);");
    struct js_page_context *child=js_page_context_create();
    struct policy_state state={0};
    struct js_worker_policy policy={&state,allow_worker,load_worker,NULL,load_entry,0};
    ck(js_page_context_enable_webapi(child,NULL),"child WebAPI enabled");
    ck(js_page_context_enable_platform(child),"child platform enabled");
    ck(js_page_context_enable_workers(child,&policy),"child Worker owner enabled");
    ck(!js_page_context_enable_workers(child,&policy),"duplicate Worker enable refused");
    ck(js_page_context_activate(child,NULL),"select child owner");
    struct node *root=open_document("https://child.example/page");
    run("var events=[],errors=[];var u=URL.createObjectURL(new Blob(["
        "\"importScripts('https://child.example/library.js');\""
        "+\"try{eval('1')}catch(e){postMessage(e.name)};\""
        "+\"try{importScripts('https://denied.example/no.js')}catch(e){postMessage(e.name)};\""
        "+\"fetch('https://denied.example/no').catch(function(){postMessage('connect-denied')});\""
        "+\"fetch('https://child.example/worker-body',{credentials:'include'}).then(r=>r.text()).then(v=>postMessage('body:'+v));\""
        "+\"postMessage(imported);onmessage=function(e){postMessage(e.data*2)};\""
        "],{type:'text/javascript'}));var w=new Worker(u);"
        "w.onmessage=function(e){events.push(e.data)};w.onerror=function(e){errors.push(e.message)};"
        "w.postMessage(6);URL.revokeObjectURL(u);");
    for(int i=0;i<90;i++)tick(1);
    ckjs("errors.length===0 && events.indexOf(17)>=0 && events.indexOf(12)>=0",
         "child Worker imports and replies in child runtime");
    ckjs("events.indexOf('EvalError')>=0 && events.indexOf('SecurityError')>=0 && events.indexOf('connect-denied')>=0",
         "child Worker enforces eval import and connect policy");
    ck(state.loads==1&&state.denied_connect==1,"policy rejection occurs before denied transport");
    ck(js_page_context_activate(NULL,NULL),"context switch after worker callbacks succeeds");ctx=js_page_ctx();
    ckjs("events.length===0","child pump does not advance parent Worker queue");
    /* Stop the negative before intentionally shared references reach runtime
     * destruction; its failure must be an owner assertion, not an ASan abort. */
    if(failures){printf("worker-context: %d checks, %d failures\n",checks,failures);return 1;}
    for(int i=0;i<12;i++)tick(1);
    ckjs("events.length===1 && events[0]===1","parent Worker resumes on its own queue");
    js_page_context_activate(child,NULL);ctx=js_page_ctx();
    ckjs("document.cookie.indexOf('strict_cookie=fixture')<0",
         "cross-site child document cannot read the strict cookie");
    ckjs("events.indexOf('body:ok')>=0",
         "child Worker consumes an allowed real HTTP response");
    ck(wft_net_requests("child.example","/worker-body","GET")==1,
       "child Worker fetch reaches its own origin exactly once");
    ck(!wft_net_header("child.example","/worker-body","strict_cookie=fixture"),
       "cross-site ancestor prevents Worker SameSite cookie leakage");
    state.deny_create=1;
    ckjs("(function(){try{new Worker(URL.createObjectURL(new Blob(['postMessage(9)'])))}catch(e){return e.name==='SecurityError'}return false})()",
         "child Worker constructor consults native policy");
    ckjs("(function(){try{new Worker('https://child.example/entry.js')}catch(e){return e.name==='SecurityError'}return false})()",
         "network entry constructor obeys creator policy");
    state.deny_create=0;state.allow_eval=1;
    run("var netEvents=[],netErrors=[];var nw=new Worker('./entry.js');"
        "nw.onmessage=function(e){netEvents.push(e.data)};nw.onerror=function(e){netErrors.push(e.message)};");
    for(int i=0;i<90;i++)tick(1);
    ckjs("netErrors.length===0&&netEvents.indexOf('network-ready')>=0",
         "network Worker uses response policy and final URL for imports and fetch");
    ckjs("netEvents.indexOf('worker-port-ready')>=0",
         "network Worker pumps its native MessageChannel callbacks");
    ck(state.entries==1&&state.response_loads==1&&state.loads==1,
         "network imports do not reuse creator loader");
    ck(!wft_net_header("child.example","/final/net-body","strict_cookie=fixture"),
         "network Worker retains opaque ancestor cookie site");
    ck(state.releases==0,"response policy lives while Worker is alive");
    run("nw.terminate()");for(int i=0;i<8;i++)tick(1);
    ck(state.releases==1,"network response policy released exactly once");
    run("var bad=new Worker('https://other.example/entry.js');bad.onerror=function(){netEvents.push('origin-denied')}");
    for(int i=0;i<20;i++)tick(1);
    ckjs("netEvents.indexOf('origin-denied')>=0","cross-origin network entry errors asynchronously");
    ck(state.entries==1,"cross-origin refusal occurs before entry loader");
    /* Exercise direct owner close as well as lazy terminate/reap above:
     * these used different teardown paths and direct close leaked policy. */
    run("var liveAtClose=new Worker('./entry.js');liveAtClose.onmessage=function(){};");
    for(int i=0;i<90;i++)tick(1);
    ck(state.entries==2&&state.releases==1,"second response policy is alive before document close");
    run("w.postMessage(9)");js_page_close();dom_free(root);
    ck(state.releases==2,"document close releases live network Worker response policy");
    ck(js_page_context_destroy(child),"child closes queued Worker without dangling references");
    js_page_context_activate(NULL,NULL);ctx=js_page_ctx();
    run("w.postMessage(20)");for(int i=0;i<12;i++)tick(1);
    ckjs("events.length===2 && events[1]===21","parent Worker survives child close");
    run("w.terminate()");for(int i=0;i<4;i++)tick(1);
    ck(!js_worker_pending(),"terminated owner has no pending tasks");
    js_page_close();dom_free(parent);
    printf("worker-context: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
