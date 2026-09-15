/* Shipping app_main, not a copied queue loop. The fixture's input callback
 * advances the virtual guest clock; no host wall timing is product evidence.
 * Slow handlers and a fast never-empty burst both need paint opportunities. */
#define main loader_existing_main
#include "loader_test.c"
#undef main
#include "js_dom.h"
void app_main(void);

static int stage, steps, cooldown, slow, handled, base_flushes, interim;
static const char *page="<!doctype html><textarea id=entry placeholder=TYPE-HERE></textarea>"
    "<script>document.getElementById('entry').addEventListener('input',function(){__work()});</script>";
static JSValue work(JSContext *ctx,JSValueConst t,int argc,JSValueConst *argv)
{
    (void)ctx;(void)t;(void)argc;(void)argv;
    if(host_flushes>base_flushes)interim=1;
    handled++;
    if(slow)host_clock+=25;
    return JS_UNDEFINED;
}
static void post(int type,int a,int b)
{
    struct logit_event e={0};e.type=type;e.a=a;e.b=b;e.button=EV_BTN_LEFT;
    host_post_event(&e);
}
static void burst(int count)
{
    base_flushes=host_flushes;handled=interim=0;
    for(int i=0;i<count;i++)post(EV_KEY,'A'+i%26,0);
}
static int value_is(const char *want)
{
    JSContext *ctx=js_page_ctx();
    const char *src="document.getElementById('entry').value";
    JSValue v=JS_Eval(ctx,src,strlen(src),"<value-observer>",JS_EVAL_TYPE_GLOBAL);
    const char *s=JS_ToCString(ctx,v);int ok=s&&!strcmp(s,want);
    if(s)JS_FreeCString(ctx,s);JS_FreeValue(ctx,v);return ok;
}
void loader_poll_hook(void)
{
    if(stage==99)return;
    if(++steps>300){CHECK(0,"bounded fairness fixture finishes");stage=99;post(EV_CLOSE,0,0);return;}
    if(!js_page_live())return;
    if(cooldown){cooldown--;return;}
    if(stage==0){
        for(int i=paint_nops-1;i>=0;i--)if(paint_ops[i].kind==OP_TEXT&&paint_ops[i].len==9&&!memcmp(paint_ops[i].text,"TYPE-HERE",9)){
            JSContext *ctx=js_page_ctx();JSValue g=JS_GetGlobalObject(ctx);
            JS_SetPropertyStr(ctx,g,"__work",JS_NewCFunction(ctx,work,"__work",0));JS_FreeValue(ctx,g);
            post(EV_MOUSE,paint_ops[i].x+4,paint_ops[i].y+4);
            post(EV_MOUSE_UP,paint_ops[i].x+4,paint_ops[i].y+4);
            cooldown=3;stage=1;return;
        }
    }else if(stage==1){slow=1;burst(12);stage=2;
    }else if(stage==2&&handled==12){
        CHECK(interim,"slow input burst paints before queue becomes empty");
        CHECK(value_is("ABCDEFGHIJKL"),"slow burst retains every character in order");
        slow=0;stage=3;cooldown=3;
    }else if(stage==3){burst(40);stage=4;
    }else if(stage==4&&handled==40){
        CHECK(interim,"same-tick event burst yields before queue becomes empty");
        CHECK(value_is("ABCDEFGHIJKLABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMN"),"fast burst retains every character in order");
        stage=99;post(EV_CLOSE,0,0);
    }
}
int main(void)
{
    fake_site_reset();fake_site_add("http://fixture.test/event-fairness",page);
    tabs_set_store(&memfs);
    const char *s="logit-browser-session\t1\t0\n0\thttp://fixture.test/event-fairness\tfairness\t0\n";
    memfs_write(SESSION_PATH,s,(int)strlen(s));post(EV_KEY,'\n',0);
    if(setjmp(host_exit_jmp)==0)app_main();
    CHECK(stage==99,"all event fairness phases complete");
    printf("event-fairness: %s (%d virtual polls)\n",fail?"FAIL":"PASS",steps);
    return fail;
}
