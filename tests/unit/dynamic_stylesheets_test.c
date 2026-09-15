/* SPDX-License-Identifier: MIT
 * The same ordinary page runs through real app_main on host and in QEMU.
 * Only transport/window are fixtures here; the gate observes both requested
 * CSS and the page's computed widths, so a fabricated load event cannot pass. */
#define main loader_existing_main
#include "loader_test.c"
#undef main
void app_main(void);
static int polls,finished;
static int expr(const char *s)
{
    JSContext *ctx=js_page_ctx();if(!ctx)return 0;
    JSValue v=JS_Eval(ctx,s,strlen(s),"<style-observer>",JS_EVAL_TYPE_GLOBAL);int ok=0;
    if(JS_IsException(v))JS_FreeValue(ctx,JS_GetException(ctx));else ok=JS_ToBool(ctx,v);
    JS_FreeValue(ctx,v);return ok;
}
void loader_poll_hook(void)
{
    host_clock+=50;
    if(finished)return;
    if(++polls<2000 && !expr("document.getElementById('result')&&document.getElementById('result').textContent.indexOf('DYNAMIC-STYLES checks=')===0"))return;
    finished=1;
    CHECK(expr("checks===13&&failures===0"),"dynamic stylesheet callbacks and cascade agree");
    CHECK(fake_site_fetched("/first.css")>0&&fake_site_fetched("/second.css")>0,"both stylesheet URLs reached transport");
    CHECK(expr("events.filter(function(x){return x==='first:load'}).length===2"),"href replacement gets exactly one new load event");
    struct logit_event e={0};e.type=EV_CLOSE;host_post_event(&e);
}
int main(void)
{
    FILE *f=fopen("tests/fixtures/browser/dynamic-styles.html","rb");if(!f)return 2;
    fseek(f,0,SEEK_END);long len=ftell(f);rewind(f);char *page=malloc((size_t)len+1);
    if(!page||fread(page,1,(size_t)len,f)!=(size_t)len)return 2;fclose(f);page[len]=0;
    fake_site_reset();fake_site_add("http://fixture.test/styles.html",page);
    fake_site_add("http://fixture.test/first.css","#target{width:52px}");
    fake_site_add("http://fixture.test/second.css","#target{width:94px}");
    fake_site_add("http://fixture.test/empty.css","");
    tabs_set_store(&memfs);const char *session="logit-browser-session\t1\t0\n0\thttp://fixture.test/styles.html\tstyles\t0\n";
    memfs_write(SESSION_PATH,session,strlen(session));
    struct logit_event e={0};e.type=EV_KEY;e.a='\n';host_post_event(&e);
    if(setjmp(host_exit_jmp)==0)app_main();
    CHECK(finished,"stylesheet real app_main reached observations");free(page);
    puts(fail?"dynamic-stylesheets: FAIL":"dynamic-stylesheets: PASS");return fail?1:0;
}
