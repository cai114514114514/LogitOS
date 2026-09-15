/* Drive native chrome editing and page activation through app_main. A direct
 * URL resolver test misses the defect: the resolver is correct but its caller
 * used uncommitted address text as the document base. Transport is the existing
 * exact-route fake; destination paint is a separate consumer assertion. */
#define main loader_existing_main
#include "loader_test.c"
#undef main
void app_main(void);

static int stage, polls, activated, finished, click_x, click_y;
static int source_tab, tab_opened, tab_returned;
static int key_submit, same_url_submit, handler_started, handler_returned, submit_events;
/* A destination replaces the source JS realm. Keep only observation counters
 * in the host, so an after-submit assertion cannot disappear with that realm.
 * This callback never dispatches input, submits a form, or navigates. */
static JSValue record_form_handler(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv)
{
    const char *label=argc?JS_ToCString(ctx,argv[0]):0;
    if(label){
        if(!strcmp(label,"before"))handler_started++;
        if(!strcmp(label,"after"))handler_returned++;
        if(!strcmp(label,"submit"))submit_events++;
        printf("FORM-HANDLER %s\n",label);fflush(stdout);JS_FreeCString(ctx,label);
    }
    return JS_UNDEFINED;
}
static const char *mode, *expected;
static const char *source_url = "http://fixture.test/dir/source.html?old=1";
static const char *draft_url = "http://uncommitted.test/wrong.html";
static const char *destination = "<!doctype html><body>NAV-DESTINATION</body>";

static void post(int type, int a, int b, int button, int mods)
{
    struct logit_event e={0}; e.type=type; e.a=a; e.b=b;
    e.button=button; e.mods=mods; host_post_event(&e);
}
static const struct paintop *last_text(const char *s)
{
    int n=(int)strlen(s);
    for(int i=paint_nops-1;i>=0;i--)
        if(paint_ops[i].kind==OP_TEXT && paint_ops[i].len==n &&
           !memcmp(paint_ops[i].text,s,n)) return &paint_ops[i];
    return 0;
}
static int exact_requests(const char *url)
{
    int n=0;
    for(int i=0;i<fake_site_requests();i++)
        if(!strcmp(fake_site_request(i),url))n++;
    return n;
}
static void finish(void)
{
    finished=1;
    for(int i=0;i<fake_site_requests();i++)
        printf("navigation-base %s request[%d]=%s\n",mode,i,fake_site_request(i));
    CHECK(activated,"native activation was delivered after address editing");
#ifdef NAVIGATION_COOKIE_CONTEXT
    CHECK(fake_site_nav_first_browser(),"initial chrome navigation has no page initiator");
    CHECK(!strcmp(fake_site_nav_initiator(),source_url),"page navigation captures committed initiator before teardown");
#endif
    if(key_submit){
        CHECK(handler_started==1,"native keydown entered form submission handler once");
        CHECK(handler_returned==1,"form submission returned to the still-live keydown handler");
        CHECK(submit_events==(!strcmp(mode,"key-request-submit")?1:0),
              "requestSubmit fires submit event while submit bypasses it");
    }
    if(!strcmp(mode,"tab-link"))
        CHECK(tab_opened && tab_returned,"native tab round trip actually completed");
    CHECK(exact_requests(expected)==(same_url_submit?2:1),"native navigation requests exact document-origin destination");
    if(key_submit)CHECK(fake_site_requests()==2,"shared navigation queue performs only the final requested navigation");
    CHECK(!strcmp(js_page_location(),expected),"committed location reaches exact destination");
    CHECK(last_text(same_url_submit?"NAV-ACTIVATE":"NAV-DESTINATION")!=0,"destination reaches actual painter");
    stage=99; post(EV_CLOSE,0,0,0,0);
}
void loader_poll_hook(void)
{
    host_clock+=10;
    if(stage==99)return;
    if(++polls>400){finish();return;}
    if(stage==0){
        const struct paintop *p=last_text("NAV-ACTIVATE");
        if(!p)return;
        if(key_submit){
            JSContext *ctx=js_page_ctx();if(!ctx)return;
            JSValue global=JS_GetGlobalObject(ctx);
            JS_SetPropertyStr(ctx,global,"recordFormHandler",JS_NewCFunction(ctx,record_form_handler,"recordFormHandler",1));
            JS_FreeValue(ctx,global);
            const char *form_api="typeof document.getElementById('f').submit==='function' && typeof document.getElementById('f').requestSubmit==='function'";
            JSValue ready=JS_Eval(ctx,form_api,strlen(form_api),"<form-adapter-apparatus>",JS_EVAL_TYPE_GLOBAL);
            CHECK(!JS_IsException(ready)&&JS_ToBool(ctx,ready)==1,"real form submit and requestSubmit bindings installed");
            if(JS_IsException(ready)){JSValue e=JS_GetException(ctx);JS_FreeValue(ctx,e);}JS_FreeValue(ctx,ready);
        }
        CHECK(!strcmp(js_page_location(),source_url),"initial committed document has its original query");
        source_tab=tabs_active();
        click_x=p->x+5; click_y=p->y+5;
        /* A valid but different origin makes an accidental address-field base
         * visible even if fake bfetch can resolve a bare ref as a fallback.
         * There is deliberately no Enter, URL assignment or JS navigation. */
        post(EV_KEY,12,0,0,EV_MOD_CTRL);
        for(const char *s=draft_url;*s;s++)post(EV_KEY,*s,0,0,0);
        stage=1;return;
    }
    if(stage==1){
        if(host_evq_head!=host_evq_tail)return;
        CHECK(fake_site_requests()==1,"typing the address does not navigate");
        CHECK(!strcmp(js_page_location(),source_url),"typing preserves the committed page URL");
        if(!strcmp(mode,"tab-link")){
            /* Both shortcuts tear down a document, so discard borrowed paint
             * strings before each one. Read tab state to prove the shortcuts
             * happened; do not replace them with browser_tab_switch(). */
            CHECK(tabs_count()==1,"tab fixture starts with one tab");
            paint_nops=0;stage=10;post(EV_KEY,20,0,0,EV_MOD_CTRL);return;
        }
        stage=2;post(EV_MOUSE,click_x,click_y,EV_BTN_LEFT,0);return;
    }
    if(stage==10){
        if(host_evq_head!=host_evq_tail)return;
        if(tabs_count()!=2 || tabs_active()==source_tab)return;
        tab_opened=1;
        CHECK(tab_cur() && !tab_cur()->url[0],"native Ctrl T selects a new empty tab");
        /* The hydration JS base comes from tab.base, independently of tab.url.
         * Checking only location after return misses a corrupted saved address;
         * inspect the original tab identity while it is actually dehydrated. */
        CHECK(tab_at(source_tab) && !strcmp(tab_at(source_tab)->url,source_url),
              "tab dehydration saves the committed document URL");
        paint_nops=0;stage=11;post(EV_KEY,23,0,0,EV_MOD_CTRL);return;
    }
    if(stage==11){
        if(host_evq_head!=host_evq_tail)return;
        if(tabs_count()!=1 || tabs_active()!=source_tab)return;
        const struct paintop *p=last_text("NAV-ACTIVATE");
        if(!p)return;
        tab_returned=1;
        CHECK(fake_site_requests()==1,"native Ctrl W rehydrates the original bytes without fetching");
        CHECK(!strcmp(js_page_location(),source_url),"tab hydration preserves the committed document origin");
        click_x=p->x+5;click_y=p->y+5;
        stage=2;post(EV_MOUSE,click_x,click_y,EV_BTN_LEFT,0);return;
    }
    if(stage==2){
        /* Old document paint text borrows its source storage. Clear recorder
         * pointers before release can navigate and free that storage. */
        paint_nops=0;activated=1;stage=key_submit?20:3;
        post(EV_MOUSE_UP,click_x,click_y,EV_BTN_LEFT,0);return;
    }
    if(stage==20){
        if(host_evq_head!=host_evq_tail)return;
        /* Real focus was obtained by mouse down/up above. A keydown listener
         * submits on native Enter and prevents the ordinary Enter default. */
        paint_nops=0;stage=3;post(EV_KEY,'\n',0,0,0);return;
    }
    if(stage==3){
        if(fake_site_requests()<2)return;
        paint_nops=0;stage=4;post(EV_RESIZE,1180,620,0,0);return;
    }
    if(stage==4 && last_text(same_url_submit?"NAV-ACTIVATE":"NAV-DESTINATION"))finish();
}
int main(int argc,char **argv)
{
    mode=argc>1?argv[1]:"link";
    const char *control;
    char form_control[1600];
    if(!strcmp(mode,"link") || !strcmp(mode,"tab-link")){
        control="<a id=activate href='target.html'>NAV-ACTIVATE</a>";
        expected="http://fixture.test/dir/target.html";
    }else if(!strcmp(mode,"relative-form")){
        control="<form method=get action='target.html'><input type=hidden name=q value=needle>"
                "<button id=activate type=submit>NAV-ACTIVATE</button></form>";
        expected="http://fixture.test/dir/target.html?q=needle";
    }else if(!strcmp(mode,"no-action-form")){
        control="<form method=get><input type=hidden name=q value=needle>"
                "<button id=activate type=submit>NAV-ACTIVATE</button></form>";
        expected="http://fixture.test/dir/source.html?q=needle";
    }else if(!strcmp(mode,"key-submit") || !strcmp(mode,"key-request-submit") ||
             !strcmp(mode,"key-same-url") || !strcmp(mode,"key-submit-then-location") ||
             !strcmp(mode,"key-location-then-submit")){
        key_submit=1;same_url_submit=!strcmp(mode,"key-same-url");
        const char *action=!strcmp(mode,"key-request-submit")?"f.requestSubmit();":"f.submit();";
        if(!strcmp(mode,"key-submit-then-location"))action="f.submit();location.href='last.html';";
        if(!strcmp(mode,"key-location-then-submit"))action="location.href='loser.html';f.submit();";
        snprintf(form_control,sizeof form_control,
            "<form id=f method=get action='%s'><input type=hidden name='%s' value='%s'>"
            "<input id=activate value='NAV-ACTIVATE'></form><script>"
            "var f=document.getElementById('f');f.addEventListener('submit',function(){recordFormHandler('submit')});"
            "document.getElementById('activate').addEventListener('keydown',function(e){if(e.key==='Enter'){"
            "e.preventDefault();recordFormHandler('before');%srecordFormHandler('after');}});</script>",
            same_url_submit?"source.html":"target.html",same_url_submit?"old":"q",same_url_submit?"1":"needle",action);
        control=form_control;expected=same_url_submit?source_url:
            !strcmp(mode,"key-submit-then-location")?"http://fixture.test/dir/last.html":"http://fixture.test/dir/target.html?q=needle";
    }else {fprintf(stderr,"unknown mode: %s\n",mode);return 2;}
    char page[2048];
    snprintf(page,sizeof page,"<!doctype html><style>body{margin:0}"
        "#activate{position:absolute;left:40px;top:140px;width:220px;height:40px}</style>%s",control);
    fake_site_reset();fake_site_add(source_url,page);
    /* Same-URL submission reloads the exact original resource. Keeping its
     * route unchanged makes the second fetch observable without inventing a
     * special response or disguising same-document navigation as another URL. */
    if(!same_url_submit)fake_site_add(expected,destination);
    tabs_set_store(&memfs);
    char session[512];snprintf(session,sizeof session,"logit-browser-session\t1\t0\n0\t%s\tfixture\t0\n",source_url);
    memfs_write(SESSION_PATH,session,(int)strlen(session));
    post(EV_KEY,'\n',0,0,0);
    if(setjmp(host_exit_jmp)==0)app_main();
    CHECK(finished,"bounded real event loop reached navigation observations");
    CHECK(host_exited && host_exit_code==0,"browser event loop exits normally");
    printf("navigation-base %s: %s (%d polls)\n",mode,fail?"FAIL":"PASS",polls);
    return fail?1:0;
}
