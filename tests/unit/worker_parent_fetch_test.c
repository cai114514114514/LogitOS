/* Finite ready-transport ordering. No real socket, wall wait, site input,
 * timeout/fault/GC scenario, or product hook is used. */
#define main original_worker_main
#include "worker_test.c"
#undef main
void pfo_net_reset(void);int pfo_net_sends(void);int pfo_net_requests(void);int pfo_net_live(void);
static int seq,first_add,second_add,created,first_send,calls,values[2],created_count;
unsigned long long pfo_now(void){return g_now;}
void pfo_sent(void){if(!first_send)first_send=++seq;}
int worker_fairness_add(int a,int b,int label)
{if(calls<2)values[calls]=a+b;calls++;if(label==1)first_add=++seq;else if(label==2)second_add=++seq;g_now+=20;return a+b;}
static JSValue pfo_created(JSContext *c,JSValueConst self,int argc,JSValueConst *argv)
{(void)c;(void)self;(void)argc;(void)argv;created=++seq;created_count++;return JS_UNDEFINED;}
static void scenario(int microtask)
{
    seq=first_add=second_add=created=first_send=calls=created_count=0;memset(values,0,sizeof values);g_now+=100;
    fake_site_reset();pfo_net_reset();fake_site_add("https://page.test/task.js","onmessage=function(e){postMessage({id:e.data,value:fixtureAdd(20,22,e.data)});};");
    const char *html="<!doctype html><body>local</body>";struct node *root=dom_parse(html,(int)strlen(html));
    if(!root){ck(0,"fixture document opens");return;}js_page_set_clock(clock_fn);js_page_set_location("https://page.test/page.html");
    if(!js_page_open(root)){ck(0,"fixture page opens");dom_free(root);return;}ctx=js_page_ctx();
    JSValue g=JS_GetGlobalObject(ctx);JS_SetPropertyStr(ctx,g,"fixtureFetchCreated",JS_NewCFunction(ctx,pfo_created,"fixtureFetchCreated",0));JS_FreeValue(ctx,g);
    run("var results=[],reply=null,errors=0;var w1=new Worker('https://page.test/task.js'),w2=new Worker('https://page.test/task.js');"
        "function localFetch(){var p=fetch('/parent-metadata',{method:'POST',credentials:'omit',headers:{'X-Client-Metadata':'local-fixture'},body:'ordinary'});fixtureFetchCreated();p.then(r=>r.text()).then(t=>reply=t,()=>errors++);};"
        "w2.onmessage=e=>results.push(e.data);");
    if(microtask)run("w1.onmessage=function(e){results.push(e.data);Promise.resolve().then(localFetch);};");
    else run("w1.onmessage=function(e){results.push(e.data);localFetch();};");
    js_worker_run_due();run("w1.postMessage(1);w2.postMessage(2);");
    js_page_run_due();
    ck(calls==1&&first_add>0&&second_add==0,"first finite worker task spends only its own turn");
    ck(values[0]==42,"first native addition returns 42 normally");
    ck(created_count==0&&pfo_net_sends()==0,"page request is not fabricated before its result callback");
    for(int i=0;i<20;i++)js_page_run_due();
    ck(created_count==1,"parent result callback creates exactly one fetch");
    ck(calls==2&&values[0]==42&&values[1]==42,"both finite worker additions complete with 42");
    ck(first_send>created&&second_add>first_send,microtask?"parent microtask fetch actually sends before next worker native call":"parent callback fetch actually sends before next worker native call");
    ck(pfo_net_sends()==1&&pfo_net_requests()==1,"ready transport carries one actual HTTP request");
    ckjs("reply==='metadata-ok'&&errors===0&&results.length===2&&results.every(r=>r.value===42)","parent fetch and both worker results finish normally");
    printf("parent-fetch sequence %s: first-add=%d fetch-created=%d first-send=%d second-add=%d\n",microtask?"microtask":"callback",first_add,created,first_send,second_add);
    run("w1.terminate();w2.terminate();");
    ck(!js_worker_pending()&&pfo_net_live()==0,"normal terminate leaves no worker task or socket");
    js_page_close();dom_free(root);
}
int main(void){scenario(0);scenario(1);printf("parent-fetch-order: %d checks, %d failures\n",checks,failures);return failures?1:0;}
