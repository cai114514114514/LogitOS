/* A real app_main burst larger than the old 64-script queue, with delayed
 * external responses and a 340-step inline continuation. Observe execution,
 * resource events and window.load; a request count alone missed dropped work. */
#define main loader_existing_main
#include "loader_test.c"
#undef main
void app_main(void);
extern int queue_peak,queue_live,queue_pumps,queue_taken,queue_busy;
static int polls,finished;
static int expr(const char *s)
{
    JSContext *ctx=js_page_ctx();if(!ctx)return 0;
    JSValue v=JS_Eval(ctx,s,strlen(s),"<queue-observer>",JS_EVAL_TYPE_GLOBAL);
    int r=0;if(JS_IsException(v))JS_FreeValue(ctx,JS_GetException(ctx));else r=JS_ToBool(ctx,v);
    JS_FreeValue(ctx,v);return r;
}
void loader_poll_hook(void)
{
    host_clock+=10;
    if(finished)return;
    if(++polls<3000 && !expr("loaded===96&&chain===340&&loads===1"))return;
    finished=1;
    CHECK(expr("executed.length===96&&loaded===96&&errors===0"),"all 96 inserted resources execute and dispatch load without drops");
    CHECK(expr("executed.every(function(x,i){return x===i})"),"parallel downloads retain async false execution order");
    CHECK(expr("chain===340"),"340 inline continuations survive the frame work budget");
    CHECK(expr("loads===1&&completeAtLoad"),"window load waits for the whole inserted script queue");
    CHECK(queue_peak>1&&queue_peak<=4,"inserted downloads overlap within bounded admission");
    CHECK(queue_taken==96&&queue_live==0,"all response handles consumed exactly once");
    CHECK(queue_busy>0,"request table backpressure actually exercised");
    printf("inserted-queue polls=%d pumps=%d peak=%d taken=%d busy=%d\n",polls,queue_pumps,queue_peak,queue_taken,queue_busy);
    struct logit_event e={0};e.type=EV_CLOSE;host_post_event(&e);
}
int main(void)
{
    const char *page="<!doctype html><h1>Queue completion</h1><script>"
        "var executed=[],loaded=0,errors=0,chain=0,loads=0,completeAtLoad=false;"
        "function next(){if(chain>=340)return;var s=document.createElement('script');"
        "s.textContent='chain++;next()';document.head.appendChild(s)}"
        "for(var i=0;i<96;i++){var s=document.createElement('script');s.id=String(i);s.async=false;s.src='chunk.js';"
        "s.onload=function(){loaded++;if(loaded===96)next()};s.onerror=function(){errors++};document.head.appendChild(s)}"
        "addEventListener('load',function(){loads++;completeAtLoad=loaded===96&&chain===340});</script>";
    fake_site_reset();fake_site_add("http://fixture.test/queue.html",page);
    fake_site_add("http://fixture.test/chunk.js","executed.push(+document.currentScript.id)");
    tabs_set_store(&memfs);const char *s="logit-browser-session\t1\t0\n0\thttp://fixture.test/queue.html\tqueue\t0\n";
    memfs_write(SESSION_PATH,s,strlen(s));
    struct logit_event e={0};e.type=EV_KEY;e.a='\n';host_post_event(&e);
    if(setjmp(host_exit_jmp)==0)app_main();
    CHECK(finished,"real event loop reached queue completion observation");
    puts(fail?"inserted-script-queue: FAIL":"inserted-script-queue: PASS");return fail?1:0;
}
