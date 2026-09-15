/* Drive the REAL browser.c app_main loop. Only the existing host window,
 * in-memory transport and bounded event/clock source replace kernel services.
 * There is no replacement scroll callback, dispatcher or paint routine. */
#define main loader_existing_main
#include "loader_test.c"
#undef main
#include "js_dom.h"
void app_main(void);

static int stage, steps, initial_seen, scroll_seen, click_seen;
static int click_x,click_y;
/* Explicit link width isolates scrolling from shrink-to-fit of an absolute
 * inline whose left edge starts beyond its containing block. */
static const char *PAGE =
"<!doctype html><style>body{margin:0;width:1800px;height:700px;background:white}"
"#marker{position:absolute;left:600px;top:120px}"
"#link{position:absolute;left:1400px;top:220px;width:200px}</style>"
"<div id=marker>SCROLL-MARKER</div>"
"<a id=link href='target.html'>CLICK-TARGET</a>"
"<script>window.calls=0;window.depth=0;window.maxDepth=0;"
"document.addEventListener('scroll',function(){depth++;maxDepth=Math.max(maxDepth,depth);"
"calls++;if(calls===1)scrollTo(200,0);depth--;});"
"setTimeout(function(){scrollTo(100,0)},200);</script>";
static const char *TARGET="<!doctype html><body>CLICK-DESTINATION</body>";

static int js_int(const char *src) {
    JSContext *ctx=js_page_ctx();if(!ctx)return -999;
    JSValue v=JS_Eval(ctx,src,strlen(src),"<runtime-scroll-observer>",JS_EVAL_TYPE_GLOBAL);
    int32_t n=-999;
    if(JS_IsException(v)){JSValue e=JS_GetException(ctx);JS_FreeValue(ctx,e);}
    else JS_ToInt32(ctx,&n,v);
    JS_FreeValue(ctx,v);return n;
}
static const struct paintop *last_text(const char *s) {
    int n=(int)strlen(s);
    for(int i=paint_nops-1;i>=0;i--)
        if(paint_ops[i].kind==OP_TEXT && paint_ops[i].len==n &&
           !memcmp(paint_ops[i].text,s,n))return &paint_ops[i];
    return 0;
}
static void post(int type,int x,int y,int button,int wheel,int mods) {
    struct logit_event e={0};e.type=type;e.a=x;e.b=y;e.button=button;e.wheel=wheel;e.mods=mods;
    host_post_event(&e);
}

void loader_poll_hook(void) {
    /* No wall-clock timeout or concurrent writer: each real event-loop poll
     * advances virtual time. The fixed budget fails explicitly and enqueues a
     * normal window close even if a timer/event/paint path never makes progress. */
    host_clock+=5;
    if(++steps>1200){
        if(stage!=99){printf("runtime-scroll budget: stage=%d win=%dx%d scrollX=%d calls=%d ops=%d\n",stage,host_win_w,host_win_h,js_int("scrollX"),js_int("calls"),paint_nops); const struct paintop *p=last_text("CLICK-TARGET");if(p)printf("runtime-scroll last link: x=%d y=%d\n",p->x,p->y);CHECK(0,"bounded browser loop reached its step budget");stage=99;post(EV_CLOSE,0,0,0,0,0);}
        return;
    }
    if(stage==99)return;
    if(!js_page_live())return;
    if(stage==0){
        const struct paintop *m=last_text("SCROLL-MARKER");
        if(!m)return;
        CHECK(m->x==600,"initial real painter places marker at document x600");
        CHECK(js_int("scrollX")==0,"fixture starts at zero scroll");
        initial_seen=1;stage=1;
    }
    if(stage==1){
        if(js_int("calls")<2)return;
        const struct paintop *m=last_text("SCROLL-MARKER");
        if(!m || m->x!=400)return; /* wait for the real frame after callbacks */
        int calls=js_int("calls"),depth=js_int("maxDepth");
        printf("runtime-scroll evidence: calls=%d maxDepth=%d paintedX=%d scrollX=%d rectX=%d\n",
            calls,depth,m->x,js_int("scrollX"),js_int("document.getElementById('marker').getBoundingClientRect().x"));
        CHECK(calls==2,"bounded listener adjustment produces exactly two scroll events");
        CHECK(depth==1,"scroll listener never reenters itself");
        CHECK(js_int("scrollX")==200,"listener second adjustment reaches browser position");
        CHECK(js_int("document.getElementById('marker').getBoundingClientRect().x")==400,
              "DOMRect and actual moved paint agree");
        scroll_seen=1;stage=2;
        /* A separate native horizontal input, not another script scroll. */
        post(EV_WHEEL,m->x+8,m->y+8,0,10,EV_MOD_SHIFT);
        return;
    }
    if(stage==2){
        const struct paintop *l=last_text("CLICK-TARGET");
        if(!l || l->x!=800)return;
        CHECK(js_int("scrollX")==600,"shift-wheel moves actual browser horizontal position");
        CHECK(js_int("document.getElementById('link').getBoundingClientRect().x")==800,
              "newly visible link rect follows horizontal wheel");
        click_x=l->x+8;click_y=l->y+8;
        printf("runtime-scroll click: documentX=1400 paintedX=%d screenPoint=%d,%d\n",l->x,click_x,click_y);
        stage=3;post(EV_MOUSE,click_x,click_y,EV_BTN_LEFT,0,0);return;
    }
    if(stage==3){
        /* The old page's op pointers die on navigation. Discard them BEFORE
         * release, then request one fresh target frame below before inspecting. */
        paint_nops=0;stage=4;post(EV_MOUSE_UP,click_x,click_y,EV_BTN_LEFT,0,0);return;
    }
    if(stage==4){
        if(!fake_site_fetched("target.html"))return;
        paint_nops=0;stage=5;post(EV_RESIZE,1180,620,0,0,0);return;
    }
    if(stage==5){
        if(!last_text("CLICK-DESTINATION"))return;
        /* Starting a fetch is not committing the next document. The poll hook
         * can run between those phases; observe the URL only once the actual
         * destination reaches paint, while still asserting the exact target. */
        CHECK(strstr(js_page_location(),"target.html")!=0,"scrolled mouse click navigates to target URL");
        CHECK(fake_site_fetched("target.html")==1,"click fetches target exactly once");
        CHECK(1,"navigation destination reaches actual painter");
        click_seen=1;stage=99;post(EV_CLOSE,0,0,0,0,0);
    }
}
int main(void) {
    fake_site_reset();fake_site_add("http://fixture.test/runtime-scroll.html",PAGE);
    fake_site_add("http://fixture.test/target.html",TARGET);
    tabs_set_store(&memfs);
    const char *session="logit-browser-session\t1\t0\n0\thttp://fixture.test/runtime-scroll.html\tfixture\t0\n";
    memfs_write(SESSION_PATH,session,(int)strlen(session));
    post(EV_KEY,'\n',0,0,0,0);
    if(setjmp(host_exit_jmp)==0)app_main();
    CHECK(host_exited && host_exit_code==0,"real browser event loop exits normally");
    CHECK(initial_seen && scroll_seen && click_seen,"all runtime scroll phases were observed");
    printf("runtime-scroll: %s (%d polls)\n",fail?"FAIL":"PASS",steps);
    return fail;
}
