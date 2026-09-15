/* Real app_main event dispatch and native edits. QEMU must separately prove
 * hardware delivery; this gate must not turn posted host events into that
 * claim. The diagnostic command must preserve the exact live document. */
#define main loader_existing_main
#include "loader_test.c"
#undef main
#include "focus.h"
#include "js_dom.h"
void app_main(void);

static int stage,steps,cooldown,x,y,last_flushes;
static const char *page="<!doctype html><style>body{margin:24px}textarea{display:block;width:300px;height:50px}</style>"
    "<div style='pointer-events:none'><textarea id=entry style='pointer-events:auto' placeholder=TYPE-HERE></textarea></div>"
    "<textarea id=locked readonly placeholder=READONLY-HERE></textarea>"
    "<script>var changes=0,lockedKeys=0,keyShape='';document.getElementById('entry').addEventListener('input',function(){changes++});"
    "document.getElementById('locked').addEventListener('keydown',function(e){lockedKeys++;if(e.keyCode===32)keyShape=e.key+'/'+e.code+'/'+e.keyCode});</script>";
static int observe(const char *src)
{
    JSContext *ctx=js_page_ctx();if(!ctx)return 0;
    JSValue v=JS_Eval(ctx,src,strlen(src),"<input-delivery-observer>",JS_EVAL_TYPE_GLOBAL);
    int ok=!JS_IsException(v)&&JS_ToBool(ctx,v)>0;
    if(JS_IsException(v))JS_FreeValue(ctx,JS_GetException(ctx));
    JS_FreeValue(ctx,v);return ok;
}
static int point(const char *s)
{
    int len=(int)strlen(s);
    for(int i=paint_nops-1;i>=0;i--)
        if(paint_ops[i].kind==OP_TEXT&&paint_ops[i].len==len&&!memcmp(paint_ops[i].text,s,len)){
            x=paint_ops[i].x+5;y=paint_ops[i].y+5;return 1;
        }
    return 0;
}
static void post(int type,int a,int b)
{
    struct logit_event e={0};e.type=type;e.a=a;e.b=b;e.button=EV_BTN_LEFT;
    /* The recorder can see drawing commands that never reach the screen.
     * Require a real flush too: the old control signature hashed layout
     * text, while a textarea's live value lives only in forms.c. */
    paint_nops=0;last_flushes=host_flushes;
    host_post_event(&e);cooldown=3;
}
void loader_poll_hook(void)
{
    host_clock+=5;
    if(stage==99)return;
    if(++steps>160){CHECK(0,"input delivery reached bounded event budget");stage=99;post(EV_CLOSE,0,0);return;}
    if(!js_page_live())return;
    if(cooldown){cooldown--;return;}
    switch(stage){
    case 0:{
        if(!point("TYPE-HERE"))return;
        struct node *root=js_dom_root();
        browser_load("about:input");
        int same=js_dom_root()==root&&observe("document.getElementById('entry')!==null");
        CHECK(same,"input diagnostic preserves live page and runtime");
        if(!same){stage=99;post(EV_CLOSE,0,0);return;}
        stage++;post(EV_MOUSE,x,y);break;
    }
    case 1:stage++;post(EV_MOUSE_UP,x,y);break;
    case 2:
        CHECK(observe("document.activeElement===document.getElementById('entry')"),"native mouse focuses pointer-events override textarea");
        stage++;post(EV_KEY,'A',0);break;
    case 3:
        CHECK(observe("document.getElementById('entry').value==='A'&&changes===1"),"native key default edits and fires input");
        CHECK(point("A")&&host_flushes>last_flushes,"native edit paints and submits changed control frame");
        stage++;post(EV_KEY,KEY_LEFT,0);break;
    case 4:
        CHECK(observe("document.getElementById('entry').value==='A'&&changes===1")&&point("A")&&host_flushes>last_flushes,
              "caret-only left movement submits unchanged text frame");
        stage++;post(EV_KEY,KEY_RIGHT,0);break;
    case 5:
        CHECK(observe("document.getElementById('entry').value==='A'&&changes===1")&&point("A")&&host_flushes>last_flushes,
              "caret-only right movement submits unchanged text frame");
        stage++;post(EV_KEY,8,0);break;
    case 6:
        CHECK(observe("document.getElementById('entry').value===''&&changes===2"),"native backspace shares editable state and input event");
        CHECK(point("TYPE-HERE")&&host_flushes>last_flushes,"native backspace submits restored placeholder frame");
        CHECK(point("READONLY-HERE"),"real painter supplies readonly target geometry");
        stage++;post(EV_MOUSE,x,y);break;
    case 7:stage++;post(EV_MOUSE_UP,x,y);break;
    case 8:
        CHECK(observe("document.activeElement===document.getElementById('locked')"),"readonly control can hold keyboard focus");
        stage++;post(EV_KEY,'B',0);break;
    case 9:
        CHECK(observe("lockedKeys===1&&document.getElementById('locked').value===''&&changes===2"),"readonly receives key event without native insertion");
        stage++;post(EV_KEY,' ',0);break;
    case 10:
        CHECK(observe("lockedKeys===2&&keyShape===' /Space/32'"),
              "native Space reports layout-independent KeyboardEvent.code=Space");
        stage=99;post(EV_CLOSE,0,0);break;
    }
}
int main(void)
{
    fake_site_reset();fake_site_add("http://fixture.test/input-delivery",page);
#ifdef BROWSER_INPUT_TRACE_NAVIGATES
    /* The mutation turns about:input back into an ordinary navigation. Feed
     * that resolved URL a real document so the control observes replacement;
     * a transport 404 leaves the old DOM alive and used to spin this negative
     * test forever while printing a misleading sequence of passing checks. */
    fake_site_add("http://fixture.test/about:input",
                  "<!doctype html><title>navigation-control</title>");
#endif
    tabs_set_store(&memfs);
    const char *session="logit-browser-session\t1\t0\n0\thttp://fixture.test/input-delivery\tinput\t0\n";
    memfs_write(SESSION_PATH,session,(int)strlen(session));post(EV_KEY,'\n',0);
    if(setjmp(host_exit_jmp)==0)app_main();
    CHECK(stage==99,"input delivery completed all event turns");
    printf("input-delivery: %s (%d virtual polls)\n",fail?"FAIL":"PASS",steps);
    return fail?1:0;
}
