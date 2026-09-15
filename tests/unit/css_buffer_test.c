/* Real browser loader/cascade with a repeated download whose two occurrences
 * exceed the old 4 MiB author buffer. Long comments make the boundary large
 * without turning this into a selector-count or CPU benchmark. */
#define main loader_existing_main
#include "loader_test.c"
#undef main
void app_main(void);
static int polls,finished;
#ifdef CSS_BUFFER_LIMIT_TEST
static int limit_stage;
#endif
static int expr(const char *s)
{
    JSContext *ctx=js_page_ctx();if(!ctx)return 0;
    JSValue v=JS_Eval(ctx,s,strlen(s),"<css-buffer-observer>",JS_EVAL_TYPE_GLOBAL);
    int ok=0;if(JS_IsException(v))JS_FreeValue(ctx,JS_GetException(ctx));else ok=JS_ToBool(ctx,v);
    JS_FreeValue(ctx,v);return ok;
}
void loader_poll_hook(void)
{
    host_clock+=20;if(finished)return;
#ifdef CSS_BUFFER_LIMIT_TEST
    polls++;
    if(!limit_stage){
        /* Before app_main opens JS, expr returns false too. Wait for a
         * positive readiness signal; testing "undefined" ran the assertions
         * against the empty startup tab and blamed the recovery mechanism. */
        if(polls<1000&&!expr("typeof sheetEvent==='string'"))return;
        CHECK(expr("window.sheetEvent==='error'"),"omitted dynamic stylesheet reports error instead of load");
        CHECK(expr("document.getElementById('sentinel').getBoundingClientRect().width===123"),"refused occurrence does not partly alter cascade");
        expr("document.getElementById('first').remove();true");limit_stage=1;return;
    }
    if(polls<2000&&!expr("document.getElementById('sentinel').getBoundingClientRect().width===321"))return;
    CHECK(expr("document.getElementById('sentinel').getBoundingClientRect().width===321"),"retained sheet recovers after earlier occurrence is removed");
#else
    if(++polls<1000&&!expr("typeof ready==='number'&&ready===1"))return;
    CHECK(expr("document.getElementById('sentinel').getBoundingClientRect().width===321"),
          "repeated sheet beyond 4 MiB preserves the last cascade occurrence");
#endif
    finished=1;
    CHECK(fake_site_fetched("large.css")==1,"repeated stylesheet downloads only once");
    struct logit_event e={0};e.type=EV_CLOSE;host_post_event(&e);
}
int main(void)
{
    int padding=2200000;
    const char *rule="*/#sentinel{width:321px}";
    char *large=malloc((size_t)padding+strlen(rule)+3);if(!large)return 2;
    large[0]='/';large[1]='*';memset(large+2,'x',(size_t)padding);strcpy(large+padding+2,rule);
#ifdef CSS_BUFFER_LIMIT_TEST
    const char *page="<!doctype html><link id=first rel=stylesheet href=large.css>"
        "<style>#sentinel{width:123px}</style><div id=sentinel>CSS BUFFER SENTINEL</div>"
        "<script>var s=document.createElement('link');s.rel='stylesheet';s.href='large.css';"
        "s.onload=function(){window.sheetEvent='load'};s.onerror=function(){window.sheetEvent='error'};"
        "document.body.appendChild(s)</script>";
#else
    const char *page="<!doctype html><link rel=stylesheet href=large.css>"
        "<style>#sentinel{width:123px}</style><link rel=stylesheet href=large.css>"
        "<div id=sentinel>CSS BUFFER SENTINEL</div><script>window.ready=1</script>";
#endif
    fake_site_reset();fake_site_add("http://fixture.test/buffer.html",page);
    fake_site_add("http://fixture.test/large.css",large);
    tabs_set_store(&memfs);
    const char *session="logit-browser-session\t1\t0\n0\thttp://fixture.test/buffer.html\tbuffer\t0\n";
    memfs_write(SESSION_PATH,session,strlen(session));
    struct logit_event e={0};e.type=EV_KEY;e.a='\n';host_post_event(&e);
    if(setjmp(host_exit_jmp)==0)app_main();
    CHECK(finished,"large CSS observation actually ran");free(large);

    /* One 8 KiB custom property substituted 700 times expands beyond 4.5 MiB.
     * The sentinel at the end must survive. A low ceiling must leave the
     * previous complete expansion byte-for-byte intact, including its tail. */
    int val=8000,repeats=700,cap=0;
    char *src=malloc(60000);int n=sprintf(src,":root{--payload:\"");
    memset(src+n,'a',(size_t)val);n+=val;n+=sprintf(src+n,"\";}");
    for(int i=0;i<repeats;i++)n+=sprintf(src+n,".item{content:var(--payload)}");
    n+=sprintf(src+n,"#last{width:777px}");
    char *out=0;int len=css_expand_vars_alloc(src,n,&out,&cap,8*1024*1024);
    CHECK(len>4718592&&strstr(out,"#last{width:777px}"),"variable expansion beyond 4.5 MiB retains its trailing rule");
    int refused=css_expand_vars_alloc(src,n,&out,&cap,4096);
    CHECK(refused==-1&&len>0&&out[len]==0&&strstr(out,"#last{width:777px}"),"low expansion ceiling retains the previous complete sheet");
    free(src);free(out);
    puts(fail?"css-buffer: FAIL":"css-buffer: PASS");return fail?1:0;
}
