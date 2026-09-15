/* Two simultaneously live DOMs, not sequential page reopen. Parent timers
 * use the shipping js_page queue; the child uses a separate QuickJS runtime
 * and the SAME js_dom implementation. No child network/platform API is
 * implied by this test. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "dom.h"
#include "js_dom.h"
#include "js_page.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void kfree(void *p) { free(p); }
static int checks, failures;
static unsigned long long now;
static unsigned long long clock_now(void) { return now; }
static int focused(void) { return 1; }
static void check(int ok, const char *name)
{ checks++; if (!ok) { failures++; printf("FAIL: %s\n",name); } }
static void eval(JSContext *ctx, const char *src, const char *name)
{
    js_page_slice_begin();
    JSValue v=JS_Eval(ctx,src,strlen(src),"<dom-context>",JS_EVAL_TYPE_GLOBAL);
    int ok=!JS_IsException(v) && JS_ToBool(ctx,v)>0;
    if (JS_IsException(v)) {
        JSValue e=JS_GetException(ctx); const char *s=JS_ToCString(ctx,e);
        printf("exception: %s\n",s?s:"?");
        JS_FreeCString(ctx,s); JS_FreeValue(ctx,e);
    }
    JS_FreeValue(ctx,v); js_page_slice_end(); check(ok,name);
}
static JSValue try_switch(JSContext *ctx,JSValueConst t,int argc,JSValueConst *v)
{
    (void)t;(void)argc;(void)v;
    return JS_NewBool(ctx,!js_dom_context_activate(0,0));
}
int main(void)
{
    const char *html="<body><button id='same'>parent</button></body>";
    struct node *parent=dom_parse(html,strlen(html));
    js_page_set_clock(clock_now); js_dom_set_focus_query(focused);
    if (!parent || !js_page_open(parent)) return 2;
    JSContext *pctx=js_page_ctx();
    eval(pctx,"var saved=document.getElementById('same'); saved.memo=17;"
         "HTMLButtonElement.prototype.owner='parent'; var clicks=0,ticks=0;"
         "saved.addEventListener('click',function(){clicks++});"
         "setTimeout(function(){ticks++;saved.textContent='timer'},10);true",
         "parent installs persistent DOM state and timer");
    js_dom_set_scroll(13,71); js_dom_note_activation();
    js_dom_clear_dirty();
    unsigned baseline=js_dom_wrapper_count();

    struct js_dom_context *child=js_dom_context_create(), *previous=(void *)1;
    check(child && js_dom_context_activate(child,&previous) && !previous,
          "select fresh child without closing parent");
    JSRuntime *crt=JS_NewRuntime(); JSContext *cctx=JS_NewContext(crt);
    const char *ch="<body><button id='same'>child</button></body>";
    struct node *root=dom_parse(ch,strlen(ch));
    if (!crt || !cctx || !root) return 2;
    js_dom_init(cctx,root);
    check(!js_dom_has_activation() && !js_dom_has_focus(),
          "child does not inherit parent activation or focus callback");
    eval(cctx,"var saved=document.getElementById('same');"
         "saved.textContent==='child' && saved.memo===undefined &&"
         "saved instanceof HTMLButtonElement && saved.owner===undefined",
         "child has its own full DOM and interface prototypes");
    eval(cctx,"var clicks=0; saved.memo=29;"
         "HTMLButtonElement.prototype.owner='child';"
         "saved.addEventListener('click',function(){clicks++;saved.textContent='clicked'});"
         "var list=document.body.children;true", "child listeners and live collections");
    js_dom_set_scroll(3,9);
    check(!js_dom_context_destroy(child),"cannot destroy a live DOM owner");
    JSValue g=JS_GetGlobalObject(cctx);
    JS_SetPropertyStr(cctx,g,"trySwitch",JS_NewCFunction(cctx,try_switch,"trySwitch",0));
    JS_FreeValue(cctx,g);
    eval(cctx,"trySwitch()", "cannot switch owner inside a JS entry");
    eval(cctx,"saved.addEventListener('probe',function(){if(!trySwitch())throw Error('switched')});"
         "saved.dispatchEvent(new Event('probe'));true", "nested dispatch preserves owner");
    js_dom_clear_dirty();
    for (int i=0;i<8;i++) {
        check(js_dom_context_activate(0,0),"restore parent context");
        check(js_dom_root()==parent,"parent native root restored");
        int x,y; js_dom_get_scroll(&x,&y);
        check(x==13 && y==71,"parent viewport restored");
        eval(pctx,"saved===document.getElementById('same') && saved.memo===17 &&"
             "saved.owner==='parent' && clicks===0", "parent wrappers and listeners preserved");
        check(js_dom_has_activation() && js_dom_has_focus(),"parent activation and focus restored");
        check(js_dom_context_activate(child,0),"restore child context");
        js_dom_get_scroll(&x,&y); check(x==3 && y==9,"child viewport restored");
        eval(cctx,"saved===document.getElementById('same') && saved.memo===29 &&"
             "saved.owner==='child' && list.length===1", "child wrappers and collection identity preserved");
    }
    struct js_event_init event={0}; event.bubbles=1;
    js_dom_dispatch(dom_get_element_by_id(root->doc,"same"),"click",&event);
    eval(cctx,"clicks===1 && saved.textContent==='clicked'", "native child click executes child listener");
    check(js_dom_dirty(),"child mutation dirties child only");
    js_dom_context_activate(0,0);
    check(!js_dom_dirty() && js_dom_listener_count()==1,"child event does not dirty parent or add listeners");
    now=11; js_page_run_due();
    eval(pctx,"ticks===1 && saved.textContent==='timer' && clicks===0",
         "parent timer survives child initialization and interaction");

    /* Collect child cycles while the PARENT is selected. Class identity and
     * wrapper accounting must follow the object, not working globals. */
    js_dom_context_activate(child,0); unsigned child_base=js_dom_wrapper_count();
    eval(cctx,"(function(){for(var i=0;i<80;i++){var n=document.createElement('i');n.loop=n}})();true",
         "create unreachable child wrapper cycles");
    js_dom_context_activate(0,0); unsigned parent_live=js_dom_wrapper_count();
    JS_RunGC(crt);
    check(js_dom_wrapper_count()==parent_live,"inactive child GC leaves parent accounting unchanged");
    js_dom_context_activate(child,0);
    check(js_dom_wrapper_count()==child_base,"inactive child GC releases child wrappers");

    js_dom_context_activate(0,0);
    dom_destroy_subtree(dom_get_element_by_id(root->doc,"same"));
    check(js_dom_root()==parent,"inactive child native mutation restores parent owner");
    js_dom_context_activate(child,0);
    eval(cctx,"saved.getAttribute('id')===null && list.length===0",
         "native destruction invalidates inactive child wrapper and live list");
    js_dom_cleanup(cctx); JS_FreeContext(cctx); JS_FreeRuntime(crt);
    check(js_dom_wrapper_count()==0,"child runtime close releases all child wrappers");
    check(js_dom_context_destroy(child),"destroy cleaned child owner");
    dom_free(root);
    check(js_dom_root()==parent && js_dom_wrapper_count()==baseline,
          "destroy child restores live parent without wrapper loss");
    js_dom_dispatch(dom_get_element_by_id(parent->doc,"same"),"click",&event);
    eval(pctx,"clicks===1 && saved.memo===17", "parent click still works after child destruction");
    js_page_close(); dom_free(parent);
    check(js_dom_wrapper_count()==0,"parent cleanup releases all wrappers");
    printf("DOM contexts: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
