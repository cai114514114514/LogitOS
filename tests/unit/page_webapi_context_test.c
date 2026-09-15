/* Real page/DOM/WebAPI runtimes; only the socket transport is injected.
 * The response body can remain suspended after headers, so switching and
 * teardown are checked with live native requests, not resolved Promises. */
#define main dom_iface_original_main
#include "dom_iface_test.c"
#undef main
#include "js_webapi.h"
#include "js_platform.h"
#include "logit_abi.h"

static void check(int ok,const char *label)
{ checks++; if(!ok){fails++;printf("FAIL: %s\n",label);} }
static void run(const char *s,const char *label)
{ check(js_page_eval(s,strlen(s),"<web-document>",0),label); }
static void expect(const char *s,const char *label)
{
    char code[4096];snprintf(code,sizeof code,"if(!(%s))throw Error('assertion');",s);
    run(code,label);
}
static unsigned long long tick;
static unsigned long long now(void){return tick;}
enum { SOCKETS=32 };
struct socket_state {
    int closed,req_len,len,off,headers,release;
    char host[128],req[4096],response[512];
};
static struct socket_state sockets[SOCKETS];
static int opened;
static int net_open(const char *host,int port,int tls)
{
    (void)port;(void)tls;
    if(opened==SOCKETS)return -1;
    struct socket_state *s=&sockets[opened];
    snprintf(s->host,sizeof s->host,"%s",host);
    return opened++;
}
static int net_poll(int fd)
{
    struct socket_state *s=&sockets[fd];
    if(s->closed)return SOCK_P_ERROR;
    return SOCK_P_CONNECTED|SOCK_P_WRITABLE|
        ((s->off<s->headers || (s->release && s->off<s->len))?SOCK_P_READABLE:0);
}
static int net_send(int fd,const void *p,int n)
{
    struct socket_state *s=&sockets[fd];
    if(n>17)n=17; /* short writes are normal transport behavior */
    if(n>(int)sizeof s->req-1-s->req_len)return -1;
    memcpy(s->req+s->req_len,p,n);s->req_len+=n;s->req[s->req_len]=0;
    if(strstr(s->req,"\r\n\r\n") && !s->len){
        if(strstr(s->req,"GET /redirect-policy ")){
            s->headers=snprintf(s->response,sizeof s->response,
                "HTTP/1.1 302 Found\r\nLocation: /blocked-policy\r\nContent-Length: 0\r\n\r\n");
            s->len=s->headers;return n;
        }
        const char *cors=strstr(s->req,"GET /allowed ")?
            "Access-Control-Allow-Origin: https://child.example\r\n":"";
        s->headers=snprintf(s->response,sizeof s->response,
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n%s\r\n",cors);
        memcpy(s->response+s->headers,"hello",5);s->len=s->headers+5;
    }
    return n;
}
static int net_recv(int fd,void *p,int n)
{
    struct socket_state *s=&sockets[fd];
    int end=s->release?s->len:s->headers;
    if(s->off>=end)return 0;
    if(n>11)n=11;
    if(n>end-s->off)n=end-s->off;
    memcpy(p,s->response+s->off,n);s->off+=n;return n;
}
static void net_close(int fd){sockets[fd].closed=1;}
static const struct webapi_net net={net_open,net_poll,net_send,net_recv,net_close,now,0};
static void pump(void)
{ for(int i=0;i<100;i++){tick++;js_page_run_due();} }
static void release_all(void)
{ for(int i=0;i<opened;i++)sockets[i].release=1; }
static struct node *open_page(const char *url)
{
    const char *html="<body><p id='result'>pending</p></body>";
    struct node *root=dom_parse(html,strlen(html));
    js_page_set_location(url);
    if(!root || !js_page_open(root))exit(2);
    return root;
}
static int refuse_handler(void){return 0;}
static int connect_policy(void *owner,const char *url)
{int *calls=owner;(*calls)++;return strstr(url,"/blocked-policy")==NULL;}
int main(void)
{
    js_page_set_clock(now);js_webapi_set_net(&net);
    js_webapi_set_storage_session(73);
    struct node *parent=open_page("https://parent.example/start");
    js_webapi_set_viewport(1100,700);
    run("var parentDoc=document;var parentFetch=fetch;var parentHistory=history;"
        "document.cookie='ancestorstrict=1; Secure; SameSite=Strict; Path=/';"
        "localStorage.setItem('owner','parent');sessionStorage.setItem('tab','73');"
        "history.pushState({owner:'parent'},'', '/parent/path');"
        "var headers=0,body='';fetch('data').then(function(r){headers++;return r.text()})"
        ".then(function(s){body=s;document.getElementById('result').textContent=s});",
        "parent starts real streaming fetch");
    pump();
    expect("headers===1 && body===''","parent headers resolve before body");
    check(strstr(sockets[0].req,"GET /parent/data ")!=0,
          "same-document history updates relative request base");
    run("location.href='/parent-next'","parent queues navigation");

    struct js_page_context *child=js_page_context_create();
    /* A child whose ancestor is cross-site has an opaque site-for-cookies,
     * not merely the ancestor's URL (which could authorize ancestor cookies). */
    check(js_page_context_enable_webapi(child,NULL),"enable owned child WebAPI");
    check(js_page_context_enable_platform(child),"enable owned child platform");
    check(!js_page_context_enable_platform(child),"duplicate platform enable is refused");
    check(!js_page_context_enable_webapi(child,0),"duplicate enable is refused");
    check(js_page_context_activate(child,0),"select network child");
    struct node *root=open_page("https://child.example/start");
    JSContext *child_ctx=js_page_ctx();
    run("var childQuery=document.querySelectorAll;"
        "var selected=document.querySelector('body > p[id=result]');"
        "if(selected!==document.getElementById('result') ||"
        "document.querySelectorAll('body > p').length!==1 ||"
        "!selected.matches('p[id]') || selected.closest('body')!==document.body)"
        "throw Error('child selectors');",
        "child installs real selector consumers");
    js_platform_document_parsed(child_ctx);
    expect("document.readyState==='interactive' && typeof MutationObserver==='function'",
           "child parser boundary reaches its own platform lifecycle");
    js_webapi_set_viewport(320,240);
    expect("typeof fetch==='function' && typeof XMLHttpRequest==='function' &&"
           "typeof Worker==='undefined' && history.length===1 && location.host==='child.example'",
           "child has real WebAPI without singleton-only extensions");
    expect("localStorage.getItem('owner')===null && sessionStorage.getItem('tab')===null",
           "cross-origin storage is isolated");
    run("localStorage.setItem('owner','child');history.pushState({owner:'child'},'', '/child/path');"
        "var headers=0,body='';fetch('data').then(function(r){headers++;return r.text()})"
        ".then(function(s){body=s;document.getElementById('result').textContent=s});",
        "child starts independent streaming fetch");
    pump();
    expect("headers===1 && body===''","child headers belong to child");
    run("location.href='/child-next'","child queues separate navigation");
    check(js_webapi_pump(NULL)==0,"wrong-context pump refuses callbacks");
    check(!js_page_context_destroy(child),"pending child cannot be destroyed");
    check(!sockets[0].closed && !sockets[1].closed,"two native requests coexist");

    js_page_context_activate(0,0);
    expect("document.querySelectorAll('body > p').length===1 &&"
           "document.querySelector('p[id=result]').ownerDocument===document",
           "parent selectors retain their document");
    expect("document===parentDoc && fetch===parentFetch && history===parentHistory &&"
           "history.state.owner==='parent' && location.pathname==='/parent/path' &&"
           "localStorage.getItem('owner')==='parent'",
           "parent WebAPI state restored");
    char navigation[2048];
    check(js_webapi_take_navigation(navigation,sizeof navigation) &&
          !strcmp(navigation,"https://parent.example/parent-next"),"parent navigation state restored");
    check(js_webapi_pump(child_ctx)==0,"inactive child pump refuses callbacks");
    sockets[0].release=1;pump();
    expect("body==='hello' && document.getElementById('result').textContent==='hello'",
           "parent body consumer updates parent DOM");
    run("var mutations=[],rejects=[];var observer=new MutationObserver(function(rs){"
        "mutations=mutations.concat(rs)});observer.observe(document.getElementById('result').firstChild,"
        "{characterDataOldValue:true});onunhandledrejection=function(e){rejects.push(e.reason);e.preventDefault()};",
        "parent installs native mutation and rejection consumers");
    js_page_context_activate(child,0);
    expect("document.querySelectorAll===childQuery &&"
           "document.querySelector('p[id=result]').ownerDocument===document",
           "child selector closure survives context switch");
    check(js_webapi_take_navigation(navigation,sizeof navigation) &&
          !strcmp(navigation,"https://child.example/child-next"),"child navigation state restored");
    expect("body==='' && document.getElementById('result').textContent==='pending'",
           "parent pump does not consume child body");
    sockets[1].release=1;pump();
    expect("body==='hello' && document.getElementById('result').textContent==='hello' &&"
           "history.state.owner==='child' && location.pathname==='/child/path'",
           "child body consumer updates child DOM");
    run("var mutations=[],rejects=[];var observer=new MutationObserver(function(rs){"
        "mutations=mutations.concat(rs)});observer.observe(document.getElementById('result').firstChild,"
        "{characterDataOldValue:true});onunhandledrejection=function(e){rejects.push(e.reason);e.preventDefault()};",
        "child installs native mutation and rejection consumers");
    struct node *parent_text=dom_get_element_by_id(parent->doc,"result")->first_child;
    struct node *child_text=dom_get_element_by_id(root->doc,"result")->first_child;
    check(dom_text_replace(parent_text,0,dom_text_length(parent_text),"parent-edit",11),
          "native producer edits inactive parent document");
    expect("mutations.length===0","inactive parent edit does not notify child");
    js_page_context_activate(0,0);
    check(dom_text_replace(child_text,0,dom_text_length(child_text),"child-edit",10),
          "native producer edits inactive child document");
    js_page_pump();
    expect("mutations.length===1 && mutations[0].oldValue==='hello' &&"
           "mutations[0].target===document.getElementById('result').firstChild &&"
           "mutations[0].target.data==='parent-edit'","parent observer receives only its own native record");
    run("Promise.reject('parent-rejection');","parent queues deferred rejection notification");
    js_page_context_activate(child,0);js_page_pump();
    expect("mutations.length===1 && mutations[0].oldValue==='hello' &&"
           "mutations[0].target===document.getElementById('result').firstChild &&"
           "mutations[0].target.data==='child-edit'","inactive child observer record delivered in child runtime");
    run("Promise.reject('child-rejection');","child queues deferred rejection notification");
    pump();expect("rejects.join(',')==='child-rejection'","child rejection notification is isolated");
    js_page_context_activate(0,0);
    expect("rejects.length===0","child pump did not report parent rejection");
    pump();expect("rejects.join(',')==='parent-rejection'","parent rejection survives child platform installation");
    js_page_context_activate(child,0);
    run("var denied=false,allowed='';fetch('https://other.example/denied').catch(function(){denied=true});"
        "fetch('https://other.example/allowed').then(function(r){return r.text()}).then(function(s){allowed=s});",
        "child requests cross-origin responses");
    release_all();pump();
    expect("denied && allowed==='hello'","child enforces CORS refusal and explicit permission");
    expect("(function(){try{history.pushState({},'', 'https://parent.example/');return false}"
           "catch(e){return location.host==='child.example'}})()","history cannot change origin");

    /* Seed the child's origin through a first-party slot, then fetch it from
     * the embedded child. Never emit Cookie values into the test log. */
    js_page_context_activate(0,0);
    struct js_page_context *first=js_page_context_create();
    check(js_page_context_enable_webapi(first,"https://child.example/"),"enable first-party test document");
    js_page_context_activate(first,0);
    struct node *first_root=open_page("https://child.example/");
    run("document.cookie='strict=1; Secure; SameSite=Strict; Path=/';"
        "document.cookie='loose=1; Secure; SameSite=None; Path=/';",
        "seed same-site policy fixtures");
    expect("document.cookie.indexOf('strict=')>=0 && localStorage.getItem('owner')==='child'",
           "first-party cookie visible and same-origin local storage shared");
    js_page_close();js_page_context_destroy(first);dom_free(first_root);
    js_page_context_activate(child,0);
    expect("document.cookie.indexOf('strict=')<0 && document.cookie.indexOf('loose=')>=0",
           "embedded cookie read keeps ancestor site policy");
    run("history.back();var cookieDone=false;fetch('/cookies',{credentials:'include'})"
        ".then(function(r){return r.text()}).then(function(){cookieDone=true});",
        "history traversal cannot promote child to first-party");
    int cookie_fd=opened-1;release_all();pump();
    expect("cookieDone","embedded cookie request completes");
    check(!strstr(sockets[cookie_fd].req,"strict=") && strstr(sockets[cookie_fd].req,"loose="),
          "embedded request preserves ancestor cookie policy after history");
    run("fetch('https://parent.example/cookies',{credentials:'include'}).catch(function(){});",
        "cross-site child requests an ancestor-origin resource");
    int ancestor_fd=opened-1;release_all();pump();
    check(!strstr(sockets[ancestor_fd].req,"ancestorstrict="),
          "opaque child site also excludes ancestor Strict cookies");

    js_page_context_activate(0,0);
    struct js_page_context *same=js_page_context_create();
    check(js_page_context_enable_webapi(same,"https://parent.example/"),"enable same-origin sibling");
    js_page_context_activate(same,0);
    struct node *same_root=open_page("https://parent.example/sibling");
    expect("sessionStorage.getItem('tab')==='73' && localStorage.getItem('owner')==='parent'",
           "same-origin documents inherit the top-level storage session");
    run("sessionStorage.setItem('tab','shared');","sibling writes shared session area");
    js_page_close();js_page_context_destroy(same);dom_free(same_root);
    expect("sessionStorage.getItem('tab')==='shared'","parent sees same-tab session mutation");
    run("var alive='';fetch('/survive').then(function(r){return r.text()}).then(function(s){alive=s});",
        "parent request remains live during child close");
    int parent_fd=opened-1;
    js_page_context_activate(child,0);
    run("fetch('/retire').then(function(r){return r.text()});location.href='/next';",
        "child queues request and navigation before close");
    int child_fd=opened-1;
    js_page_close();
    check(sockets[child_fd].closed && !sockets[parent_fd].closed,"child close cancels only child sockets");
    check(js_page_open(root),"closed child runtime reopens with owned services");
    expect("typeof mutations==='undefined' && location.host==='child.example' &&"
           "document.cookie.indexOf('strict=')<0 && localStorage.getItem('owner')==='child'",
           "reopen discards old globals but preserves storage and ancestor policy");
    run("var freshBody='';fetch('/fresh').then(function(r){return r.text()})"
        ".then(function(s){freshBody=s});","reopened child issues a new realm request");
    release_all();pump();expect("freshBody==='hello'","reopened child request completes");
    js_page_close();
    check(js_page_context_destroy(child),"closed WebAPI child releases its owner state");
    dom_free(root);
    check(!js_webapi_take_navigation(navigation,sizeof navigation),"retired child navigation cannot escape to parent");
    check(dom_text_replace(parent_text,0,dom_text_length(parent_text),"still-live",10),
          "parent native subscription survives child teardown");
    release_all();pump();expect("alive==='hello'","parent request survives child teardown");
    expect("mutations.length===2 && mutations[1].target.data==='still-live'",
           "parent observer remains usable after child teardown");
    struct js_page_context *opaque=js_page_context_create();
    check(js_page_context_enable_webapi(opaque,NULL),"enable opaque child without inferred site");
    js_page_context_activate(opaque,0);
    struct node *opaque_root=open_page("about:blank");
    expect("(function(){try{history.pushState({},'', 'https://parent.example/');return false}"
           "catch(e){return location.href==='about:blank' && document.cookie===''}})()",
           "opaque document cannot promote its origin through history");
    js_page_close();js_page_context_destroy(opaque);dom_free(opaque_root);
    JS_SetStringCodeGenerationAllowed(js_page_ctx(),0);
    expect("(function(){var denied=0;var calls=[function(){eval('1')},function(){(0,eval)('1')},"
        "function(){Function('return 1')},function(){(async function(){}).constructor('return 1')},"
        "function(){(function*(){}).constructor('yield 1')},function(){(async function*(){}).constructor('yield 1')}];"
        "for(var i=0;i<calls.length;i++){try{calls[i]()}catch(e){if(e instanceof EvalError)denied++}}return denied===6})()",
        "document eval policy covers direct indirect and all Function constructors");
    expect("eval(42)===42","eval policy preserves non-string passthrough and host evaluation");
    struct js_page_context *eval_sibling=js_page_context_create();
    js_page_context_activate(eval_sibling,0);struct node *eval_root=open_page("https://other.example/");
    expect("eval('6*7')===42 && Function('return 7')()===7","eval policy is not shared with sibling runtime");
    js_page_close();js_page_context_destroy(eval_sibling);dom_free(eval_root);
    expect("(function(){try{eval('1');return false}catch(e){return e instanceof EvalError}})()","parent eval restriction survives sibling teardown");
    js_dom_set_inline_handler_policy(refuse_handler);
    run("var policyClicks=0;var policyButton=document.createElement('button');"
        "policyButton.setAttribute('onclick','policyClicks++');document.body.appendChild(policyButton);"
        "policyButton.dispatchEvent(new Event('click'));","dispatch attribute handler under native policy");
    expect("policyClicks===0","native handler policy refuses lazy attribute compilation");
    run("policyButton.onclick=function(){policyClicks++};policyButton.dispatchEvent(new Event('click'));",
        "script-assigned event listener remains available");
    expect("policyClicks===1","handler policy does not deny already compiled listener functions");
    int policy_calls=0,policy_before=opened;
    check(js_webapi_set_connect_policy(js_page_ctx(),connect_policy,&policy_calls),"install context-owned native connect policy");
    run("var policyDenied=0;fetch('/blocked-policy').catch(function(){policyDenied++})","fetch policy refusal returns a promise");
    pump();expect("policyDenied===1","initial connect policy rejects before network");
    check(opened==policy_before&&policy_calls==1,"blocked initial request opens no socket");
    run("fetch('/redirect-policy').catch(function(){policyDenied++})","allowed initial request reaches redirect policy");
    release_all();pump();expect("policyDenied===2","redirect connect policy rejects before following location");
    check(opened==policy_before+1&&policy_calls==3,"redirect destination checked without another dial");
    js_page_close();dom_free(parent);js_webapi_drop_storage_session(73);
    printf("Page WebAPI contexts: %d checks, %d failures\n",checks,fails);
    return fails?1:0;
}
