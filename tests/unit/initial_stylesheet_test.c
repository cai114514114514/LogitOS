/* Run app_main through initial paint, page load, and a later author edit.
 * Geometry proves CSS still applies; the production counter distinguishes
 * skipping redundant work from merely doing it faster on this host. */
#define main loader_existing_main
#include "loader_test.c"
#undef main
void app_main(void);
int browser_stylesheet_rebuilds(void);
unsigned browser_dom_restyles(void);
static int done,polls,initial_seen,event_edit_started;
static unsigned initial_restyles;
static int expr(const char *s)
{
    JSContext *ctx=js_page_ctx();if(!ctx)return 0;
    JSValue v=JS_Eval(ctx,s,strlen(s),"<initial-sheet-observer>",0);
    int ok=!JS_IsException(v)&&JS_ToBool(ctx,v);
    if(JS_IsException(v))JS_FreeValue(ctx,JS_GetException(ctx));
    JS_FreeValue(ctx,v);return ok;
}
void loader_poll_hook(void)
{
    host_clock+=10;if(done)return;
    if(++polls<2000&&!expr("typeof ready!=='undefined'&&ready"))return;
    if(!initial_seen) {
        CHECK(expr("target.getBoundingClientRect().width===47"),"initial stylesheet produces real width");
        CHECK(browser_stylesheet_rebuilds()==0,"initial committed stylesheet is not rebuilt");
        initial_restyles=browser_dom_restyles();
        initial_seen=1;
        expr("document.getElementById('sheet').textContent='#target{width:93px}';true");
        return;
    }
    if(polls<2000&&browser_stylesheet_rebuilds()==0)return;
    if(!event_edit_started) {
    CHECK(expr("target.getBoundingClientRect().width===93"),"later stylesheet edit still changes real width");
    CHECK(browser_stylesheet_rebuilds()==1,"one author edit rebuilds exactly once");
    CHECK(browser_dom_restyles()==initial_restyles,"applied author edit does not recascade through DOM dirty again");
        event_edit_started=1;
        expr("var callbackEdited=false,link=document.createElement('link');"
             "link.setAttribute('rel','stylesheet');link.setAttribute('href','http://fixture.test/late.css');"
             "link.onload=function(){target.style.width='137px';callbackEdited=true};"
             "document.head.appendChild(link);true");
        return;
    }
    if(polls<2000&&!expr("callbackEdited"))return;
    CHECK(expr("callbackEdited&&target.getBoundingClientRect().width===137"),
          "stylesheet load callback's new invalidation survives the completed cascade");
    done=1;struct logit_event e={0};e.type=EV_CLOSE;host_post_event(&e);
}
int main(void)
{
    const char *page="<!doctype html><style id=sheet>#target{width:47px}</style><div id=target>SHEET TEXT</div>"
        "<script>var target=document.getElementById('target'),ready=false;window.addEventListener('load',function(){ready=true});</script>";
    fake_site_reset();fake_site_add("http://fixture.test/initial-sheet.html",page);
    fake_site_add("http://fixture.test/late.css","#target{width:51px}");
    tabs_set_store(&memfs);
    const char *session="logit-browser-session\t1\t0\n0\thttp://fixture.test/initial-sheet.html\tsheet\t0\n";
    memfs_write(SESSION_PATH,session,strlen(session));
    struct logit_event e={0};e.type=EV_KEY;e.a='\n';host_post_event(&e);
    if(setjmp(host_exit_jmp)==0)app_main();
    CHECK(done&&initial_seen,"initial and mutated sheet observations completed");
    return fail?1:0;
}
