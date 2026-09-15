#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "dom.h"
#include "css.h"
#include "js_dom.h"
#include "js_page.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

/* Link stubs, the same set tests/unit/wpt_test.c carries and for the same
 * reason: this links the SHIPPING browser files (js_page/js_dom/js_webapi/
 * js_platform/js_select/js_module), and two of them reach for the fetcher and
 * the image registry, neither of which exists off the machine. A runner over
 * stubbed DOM files would measure the stubs; stubbing the network does not. */
__attribute__((__weak__)) void img_register(void *d) { (void)d; }
__attribute__((__weak__)) void img_register_anim(void *a, void *b, void *c)
{ (void)a; (void)b; (void)c; }
int bfetch_resolve(const char *base, const char *ref, char *out, int max)
{ (void)base; if (!ref || !out || max <= 0) return 0; snprintf(out, (size_t)max, "%s", ref); return 1; }
int bfetch_sync(const char *ref, unsigned char **out, int *outlen)
{ (void)ref; (void)out; (void)outlen; return 0; }
/* The module installer is linked but no case imports a module. Its browser_rt
 * fetch hooks must still resolve at link time; report not found if called. */
void bfetch_prefetch(const char *ref) { (void)ref; }
void bfetch_prefetch_wait(void) { }
int  res_fetch(const char *src, unsigned char **buf, int *len)
{ (void)src; (void)buf; (void)len; return -1; }


/* IDs are allowed to collide during component replacement. Query results must
 * follow the tree after each mutation, not the insertion order of the ID index.
 * These tests use the shipping JS binding and keep shadow isConnected checks
 * beside lookup checks: disconnecting shadow roots would hide the bug while
 * breaking an independent DOM contract. No network response is used. */
static int checks, failures;
static void ck(const char *name, const char *src)
{
    checks++;
    JSContext *ctx = js_page_ctx();
    JSValue v = JS_Eval(ctx, src, strlen(src), "<dom-id>", JS_EVAL_TYPE_GLOBAL);
    int ok = !JS_IsException(v) && JS_ToBool(ctx, v);
    if (!ok) {
        printf("FAIL %s", name);
        if (JS_IsException(v)) {
            JSValue e = JS_GetException(ctx);
            const char *s = JS_ToCString(ctx, e);
            printf(": %s", s ? s : "exception");
            JS_FreeCString(ctx, s); JS_FreeValue(ctx, e);
        }
        puts(""); failures++;
    }
    JS_FreeValue(ctx, v);
}
int main(void)
{
    const char *html="<!doctype html><html><body></body></html>";
    struct node *root=dom_parse(html, strlen(html));
    if (!root) return 2;
    js_page_set_location("http://example.com/");
    if (!js_page_open(root)) { dom_free(root); return 2; }
    js_page_eval("void 0;",7,"<warmup>",0);
    ck("setup", "var box=document.createElement('div');document.body.appendChild(box);"
       "var a=document.createElement('div'),b=document.createElement('div');"
       "a.id='shared';b.id='shared';box.appendChild(a);box.appendChild(b);true");
    ck("duplicate tree order", "document.getElementById('shared')===a");
    ck("reorder B before A", "box.insertBefore(b,a);document.getElementById('shared')===b");
    ck("restore A before B", "box.insertBefore(a,b);document.getElementById('shared')===a");
    ck("ancestor before descendant", "a.appendChild(b);document.getElementById('shared')===a");
    ck("ID attribute change", "a.setAttribute('id','other');document.getElementById('shared')===b && document.getElementById('other')===a");
    ck("restore ID does not reorder", "a.setAttribute('id','shared');document.getElementById('shared')===a");
    ck("remove ID attribute", "a.removeAttribute('id');document.getElementById('shared')===b");
    ck("detached subtree invisible", "box.removeChild(a);document.getElementById('shared')===null");
    ck("reattached subtree visible", "box.appendChild(a);document.getElementById('shared')===b");
    ck("case sensitive", "document.getElementById('SHARED')===null && document.getElementById('')===null");
    ck("shadow setup", "var host=document.createElement('div');box.appendChild(host);"
       "var sr=host.attachShadow({mode:'open'}),s=document.createElement('span');"
       "s.id='shadow-only';sr.appendChild(s);true");
    ck("document excludes shadow", "document.getElementById('shadow-only')===null");
    ck("shadow remains connected", "sr.isConnected && s.isConnected && sr.host===host");
    ck("shadow own ID query", "sr.getElementById('shadow-only')===s");
    ck("shadow ID argument conversion", "sr.getElementById({toString:function(){return 'shadow-only';}})===s");
    ck("shadow query receiver check", "(function(){try{sr.getElementById.call(a,'shadow-only');return false;}catch(e){return e instanceof TypeError;}})()");
    ck("shadow query requires argument", "(function(){try{sr.getElementById();return false;}catch(e){return e instanceof TypeError;}})()");
    ck("shadow own selector query", "sr.querySelector('#shadow-only')===s && document.querySelector('#shadow-only')===null");
    ck("shadow query excludes light tree", "sr.getElementById('shared')===null");
    ck("shadow duplicates setup", "var s2=document.createElement('span');s2.id='shadow-only';sr.appendChild(s2);true");
    ck("shadow duplicate tree order", "sr.getElementById('shadow-only')===s");
    ck("shadow duplicate reorder", "sr.insertBefore(s2,s);sr.getElementById('shadow-only')===s2");
    ck("nested shadow setup", "var nested=s.attachShadow({mode:'closed'}),n=document.createElement('b');n.id='nested-only';nested.appendChild(n);true");
    ck("outer shadow excludes nested shadow", "sr.getElementById('nested-only')===null && document.getElementById('nested-only')===null");
    ck("closed shadow own query", "nested.getElementById('nested-only')===n && n.isConnected");
    ck("shadow detach keeps local lookup", "box.removeChild(host);!s.isConnected && !sr.isConnected && !n.isConnected && sr.getElementById('shadow-only')===s2");
    ck("shadow reattach preserves isolation", "box.appendChild(host);s.isConnected && n.isConnected && document.getElementById('shadow-only')===null");
    ck("normal element has no ID query", "typeof a.getElementById==='undefined'");
    js_page_close(); dom_free(root);
    printf("dom-id: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
