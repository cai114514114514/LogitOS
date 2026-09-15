/* Finite cooperative fairness, not a watchdog, performance or memory test.
 * A native addition completes normally and advances a controlled monotonic
 * clock by 20 ms once. We inspect the actual scheduler between calls; no
 * fixture reproduces its queue logic and no elapsed host time is measured. */
#define main worker_existing_suite_main
#include "worker_test.c"
#undef main
#include "worker_fetch_net.h"

static int calls,labels[16],sums[16],input_seen[16],advance_label;
static int inputs,input_boundary;
static int parent_messages,messages_seen[16];
int worker_fairness_add(int a,int b,int label)
{
    if(calls<16){labels[calls]=label;sums[calls]=a+b;input_seen[calls]=inputs;messages_seen[calls]=parent_messages;}
    calls++;
    if(advance_label==-1||label==advance_label)g_now+=20;
    return a+b;
}
static JSValue input_handled(JSContext *c,JSValueConst self,int argc,JSValueConst *argv)
{(void)c;(void)self;(void)argc;(void)argv;inputs++;input_boundary=calls;return JS_UNDEFINED;}
static JSValue message_handled(JSContext *c,JSValueConst self,int argc,JSValueConst *argv)
{(void)c;(void)self;(void)argc;(void)argv;parent_messages++;return JS_UNDEFINED;}
static void reset_observer(int label)
{calls=inputs=parent_messages=0;input_boundary=-1;advance_label=label;memset(labels,0,sizeof labels);memset(sums,0,sizeof sums);memset(input_seen,0,sizeof input_seen);memset(messages_seen,0,sizeof messages_seen);}
static const char *TASK_JS=
    "onmessage=function(e){postMessage({id:e.data,value:fixtureAdd(20,22,e.data)});};";
static const char *FETCH_JOBS_JS=
    "onmessage=function(e){postMessage({id:e.data,value:fixtureAdd(20,22,e.data)});};"
    "fetch('/bytes').then(r=>r.arrayBuffer()).then(function(){"
    "var first=fixtureAdd(20,22,101);return Promise.resolve(first);"
    "}).then(function(first){postMessage({id:102,value:fixtureAdd(20,22,102),first:first});});";
static const char *FETCH_MESSAGES_JS=
    "onmessage=function(e){var id=e.data;fetch('/bytes').then(r=>r.arrayBuffer()).then(function(){postMessage({id:id,value:fixtureAdd(20,22,id)});});};";
static struct node *begin(const char *script,int advance)
{
    reset_observer(advance);g_now+=100;
    fake_site_reset();wft_net_reset();
    fake_site_add("https://page.test/task.js",script);
    const char *html="<!doctype html><body><div id='edit' contenteditable='true'>before</div></body>";
    struct node *root=dom_parse(html,(int)strlen(html));
    if(!root)return NULL;
    js_page_set_clock(clock_fn);js_page_set_location("https://page.test/page.html");
    if(!js_page_open(root)){dom_free(root);return NULL;}ctx=js_page_ctx();
    JSValue g=JS_GetGlobalObject(ctx);JS_SetPropertyStr(ctx,g,"fixtureInputHandled",JS_NewCFunction(ctx,input_handled,"fixtureInputHandled",0));
    JS_SetPropertyStr(ctx,g,"fixtureMessageHandled",JS_NewCFunction(ctx,message_handled,"fixtureMessageHandled",0));JS_FreeValue(ctx,g);
    run("var results=[],parentEvents=[];"
        "window.addEventListener('message',function(e){if(e.data==='between')parentEvents.push('message');});"
        "document.getElementById('edit').addEventListener('input',function(){fixtureInputHandled();parentEvents.push('input');});"
        "function launch(){var x=new Worker('https://page.test/task.js');x.onmessage=function(e){fixtureMessageHandled();results.push(e.data);};return x;}"
        "var w=launch();");
    return root;
}
static void end(struct node *root)
{js_page_close();dom_free(root);}
static void intervene(void)
{
    /* This exercises parent DOM/event handlers between Worker turns. It is
     * synthetic UI dispatch, not an OS keyboard or visual responsiveness claim. */
    run("window.dispatchEvent(new MessageEvent('message',{data:'between'}));"
        "var edit=document.getElementById('edit');edit.textContent='typed';edit.dispatchEvent(new Event('input'));");
    ckjs("parentEvents.join(',')==='message,input'&&document.getElementById('edit').textContent==='typed'",
         "parent message and text-input handler can execute between worker turns");
}
static void drive_ready(int limit)
{
    /* Exactly the exported idle/wake contract, with a finite iteration cap.
     * A hidden Promise queue must fail instead of being rescued by blindly
     * pumping a worker which the browser itself would regard as asleep. */
    for(int i=0;i<limit&&js_worker_pending();i++){
        long long due=js_worker_next_due();
        if(due<0)break;
        if((unsigned long long)due>g_now)g_now=(unsigned long long)due;
        js_worker_run_due();js_page_pump();
    }
}
static void message_tasks(int before_start,int page_turn)
{
    struct node *root=begin(TASK_JS,1);if(!root){ck(0,"page fixture opens");return;}
    if(!before_start)js_worker_run_due();
    run("w.postMessage(1);w.postMessage(2);");
    if(page_turn)js_page_run_due();else js_worker_run_due();
    ck(calls==1,page_turn?"page scheduler shares worker turn budget":before_start?"startup buffered messages yield after first complete task":"ready messages yield after first complete task");
    ck(sums[0]==42,"first native addition completes normally with 42");
    ck(js_worker_pending(),"deferred message remains scheduler-pending");
    ck(js_worker_next_due()>=0&&js_worker_next_due()<=(long long)g_now,"deferred message retains an immediate due time");
    intervene();
    ck(input_boundary==1,"parent input is handled before second worker addition");
    if(page_turn){
        js_page_run_due();
        ck(calls==1,"page parent-result handoff returns before another worker native call");
        js_page_run_due();
    }else js_worker_run_due();
    ck(calls==2&&labels[1]==2&&sums[1]==42,page_turn?"page turn after parent handoff completes the deferred addition with 42":"next worker pump completes the deferred addition with 42");
    ck(input_seen[1]==1,"second addition observes intervening parent input");
    drive_ready(12);
    ckjs("results.length===2&&results[0].id===1&&results[1].id===2&&results.every(r=>r.value===42)","worker result messages preserve finite task order");
    run("w.terminate();");
    ck(!js_worker_pending(),"normal terminate cleans the finite task worker");
    end(root);
}
static void fetch_jobs(int add_following_task)
{
    struct node *root=begin(FETCH_JOBS_JS,101);if(!root){ck(0,"fetch page fixture opens");return;}
    for(int i=0;i<40&&calls==0;i++)js_worker_run_due();
    ck(calls==1,add_following_task?"fetch checkpoint yields before its remaining job and later task":"fetch-only checkpoint yields after first complete addition");
    ck(wft_net_requests("page.test","/bytes","GET")==1&&wft_net_live()==0,"fetch response is complete before checking leftover microtasks");
    ck(js_worker_pending(),"fetch-only remaining microtask remains scheduler-pending");
    ck(js_worker_next_due()>=0&&js_worker_next_due()<=(long long)g_now,"fetch-only remaining microtask retains an immediate due time");
    intervene();
    if(add_following_task)run("w.postMessage(103);");
    drive_ready(16);
    ck(calls==(add_following_task?3:2)&&labels[1]==102&&sums[1]==42&&input_seen[1]==1,"remaining worker microtask resumes after parent input without loss");
    if(add_following_task)ck(labels[2]==103,"remaining checkpoint jobs precede the worker's next message task");
    ckjs("results.some(r=>r.id===102&&r.value===42&&r.first===42)","fetch-only worker eventually delivers unchanged result 42");
    run("w.terminate();");ck(!js_worker_pending(),"normal terminate cleans the fetch-only worker");
    end(root);
}
static void terminate_and_reopen(void)
{
    struct node *root=begin(TASK_JS,201);if(!root){ck(0,"reopen fixture opens");return;}
    js_worker_run_due();run("w.postMessage(201);w.postMessage(202);");js_worker_run_due();
    ck(calls==1&&js_worker_pending(),"terminate scenario reaches a completed-task budget boundary");
    run("w.terminate();");ck(!js_worker_pending(),"terminate clears work remaining after the turn budget");
    reset_observer(999);
    run("results=[];w=launch();w.postMessage(301);");drive_ready(12);
    ck(calls==1&&labels[0]==301&&sums[0]==42,"new worker completes after a budget-limited worker is terminated");
    ckjs("results.length===1&&results[0].id===301&&results[0].value===42","old turn cursor cannot retain or reorder replacement worker result");
    reset_observer(401);run("w.postMessage(401);w.postMessage(402);");js_worker_run_due();
    ck(calls==1&&js_worker_pending(),"page-close scenario reaches a completed-task budget boundary");
    end(root);
    root=begin(TASK_JS,999);if(!root){ck(0,"new page fixture opens");return;}
    run("w.postMessage(501);");drive_ready(12);
    ck(calls==1&&labels[0]==501&&sums[0]==42,"new page worker runs after budget-limited page close");
    ckjs("results.length===1&&results[0].id===501&&results[0].value===42","new page receives only its own finite worker result");
    end(root);
}
static void parent_delivery_priority(void)
{
    struct node *root=begin(FETCH_MESSAGES_JS,-1);if(!root){ck(0,"two-owner fixture opens");return;}
    run("var peer=launch();w.postMessage(601);peer.postMessage(602);");
    for(int i=0;i<40&&calls==0;i++)js_worker_run_due();
    ck(calls==1,"two fetch owners share one turn budget");
    js_worker_run_due();
    ck(calls==2&&sums[0]==42&&sums[1]==42,"second owner completes its finite addition on a later turn");
    ck(messages_seen[1]==1,"queued parent message dispatch precedes next owner fetch reaction");
    drive_ready(12);
    ckjs("results.length===2&&results.every(r=>r.value===42)","both owners deliver unchanged arithmetic results");
    run("w.terminate();peer.terminate();");
    ck(!js_worker_pending(),"both finite fetch owners terminate normally");
    end(root);
}
int main(void)
{
    message_tasks(0,0);message_tasks(1,0);message_tasks(0,1);fetch_jobs(0);fetch_jobs(1);terminate_and_reopen();parent_delivery_priority();
    printf("worker-fairness: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
