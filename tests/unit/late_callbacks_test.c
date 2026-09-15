/* Actual app_main + real DOM/JS/layout. The only additional apparatus is the
 * host park seam: observe the first wait_idle, instead of spinning until an
 * unrelated later poll accidentally consumes work owed by this callback. */
#define main loader_existing_main
#include "loader_test.c"
#undef main
#include "js_dom.h"
void app_main(void);
static int mode, stage, steps, parked, hook_seen, closing;
static int js_int(const char *s) {
    JSContext *ctx=js_page_ctx(); if(!ctx)return -999;
    JSValue v=JS_Eval(ctx,s,strlen(s),"<park-observer>",JS_EVAL_TYPE_GLOBAL);
    int32_t n=-999;if(!JS_IsException(v))JS_ToInt32(ctx,&n,v);
    else {JSValue e=JS_GetException(ctx);JS_FreeValue(ctx,e);}
    JS_FreeValue(ctx,v);return n;
}
static void post(int type,int x,int y,int wheel,int mods) {
    struct logit_event e={0};e.type=type;e.a=x;e.b=y;e.wheel=wheel;e.mods=mods;
    host_post_event(&e);
}
void loader_poll_hook(void) {
    host_clock+=5;
    if(++steps>500 && !closing) {
        CHECK(0,"bounded loop reached park boundary");closing=1;post(EV_CLOSE,0,0,0,0);
    }
    if(closing)return;
    if(stage || !js_page_live())return;
    for(int i=paint_nops-1;i>=0;i--) {
        if(paint_ops[i].kind!=OP_TEXT||paint_ops[i].len!=13||memcmp(paint_ops[i].text,"LATE-CALLBACK",13))continue;
        stage=1;if(mode!=3)post(EV_WHEEL,paint_ops[i].x+5,paint_ops[i].y+5,4,EV_MOD_SHIFT);
        return;
    }
}
void late_callback_park(int ms) {
    if(!stage || parked || closing)return;
    /* The old observer required ms==0. The async insertion queue now keeps
     * a pending job awake with a bounded pump wait, so sabotaging its consumer
     * produces a pump wait (10ms measured here), not an indefinite park. Ignoring
     * that boundary
     * spun 500 host polls and hid the actual missing execution. These fixtures
     * insert only ready inline text, with no timer/network/animation: that work
     * must have run before the FIRST scheduler wait, regardless of its delay.
     * This does not require a pending external fetch to finish before sleep. */
    parked=1;
    int dest=fake_site_fetched("target.html");
    hook_seen=dest?1:js_int("window.scrollCalls===1");
    CHECK(hook_seen,"real queued scroll listener executed before park");
    if(mode==1)CHECK(js_int("window.inserted===42"),"late inserted script executed before first scheduler wait");
    else CHECK(dest==1,"late callback navigation consumed before first scheduler wait");
    printf("late-callback park evidence: mode=%d wait_ms=%d destination_requests=%d polls=%d\n",mode,ms,dest,steps);
    /* A direct app_exit longjmp used to bypass app_main teardown. The new
     * queue owns a pinned JSValue; js_page_close with that queue alive aborts
     * QuickJS rather than reporting our negative-control assertion. A native
     * close lets pending_scripts_reset release its owners before context close.
     * Assertions are already recorded above, so the next turn cannot erase red. */
    closing=1;post(EV_CLOSE,0,0,0,0);
}
int main(int argc,char **argv) {
    mode=argc>1?atoi(argv[1]):0;
    const char *actions[]={
        "location.href='target.html';",
        "var s=document.createElement('script');s.textContent='window.inserted=42';document.body.appendChild(s);",
        "var s=document.createElement('script');s.textContent=\"location.href='target.html'\";document.body.appendChild(s);",
        "location.href='target.html';"
    };
    char page[4096];snprintf(page,sizeof page,
        "<!doctype html><style>body{margin:0;width:1800px;height:700px}</style><div>LATE-CALLBACK</div>"
        "<script>window.scrollCalls=0;document.addEventListener('scroll',function(){scrollCalls++;if(scrollCalls===1){%s}});%s</script>",actions[mode],mode==3?"scrollTo(100,0);":"");
    fake_site_reset();fake_site_add("http://fixture.test/late.html",page);
    fake_site_add("http://fixture.test/target.html","<!doctype html><body>DESTINATION</body>");
    tabs_set_store(&memfs);
    const char *session="logit-browser-session\t1\t0\n0\thttp://fixture.test/late.html\tfixture\t0\n";
    memfs_write(SESSION_PATH,session,strlen(session));post(EV_KEY,'\n',0,0,0);
    if(setjmp(host_exit_jmp)==0)app_main();
    CHECK(parked,"observer intercepted first actual scheduler wait");
    CHECK(host_exited && host_exit_code==0,"native close cleaned pending queue and runtime");
    js_page_close();
    printf("late-callback mode=%d: %s\n",mode,fail?"FAIL":"PASS");return !!fail;
}
