/* SPDX-License-Identifier: MIT
 * Document.hasFocus state and ordinary transitions on shipping DOM/page/WebAPI sources.
 * Transport is absent; no third-party page or authentication is exercised. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "dom.h"
#include "js_page.h"
#include "js_webapi.h"
#include "js_dom.h"
#include "css.h"
#include "focus.h"
#include "forms.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void kfree(void *p) { free(p); }
/* The state gate has no framebuffer; forms needs its usual host font-width
 * provider at link time, but no assertion uses a pixel or caret coordinate. */
int text_measure(const char *s, int len, int px, int mono)
{ (void)s; (void)mono; return len * (px / 2); }
__attribute__((__weak__)) void img_register(void *d) { (void)d; }
__attribute__((__weak__)) void img_register_anim(void *a, void *b, void *c)
{ (void)a; (void)b; (void)c; }
int bfetch_resolve(const char *base, const char *ref, char *out, int max)
{ (void)base; if (!ref || !out || max <= 0) return 0; snprintf(out, (size_t)max, "%s", ref); return 1; }
int bfetch_sync(const char *ref, unsigned char **out, int *len)
{ (void)ref; (void)out; (void)len; return 0; }
void bfetch_prefetch(const char *ref) { (void)ref; }
void bfetch_prefetch_wait(void) { }
int res_fetch(const char *ref, unsigned char **out, int *len)
{ (void)ref; (void)out; (void)len; return -1; }

static int checks, failed;
static JSContext *ctx;
static void check(const char *src, const char *name)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<document-focus-test>", JS_EVAL_TYPE_GLOBAL);
    int ok = !JS_IsException(v) && JS_ToBool(ctx, v);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, v);
    checks++;
    if (!ok) failed++;
    printf("%s: %s\n", ok ? "ok" : "FAIL", name);
}

static struct node *open_page(const char *url)
{
    const char html[] = "<!doctype html><html><head></head><body>local</body></html>";
    struct node *root = dom_parse(html, sizeof html - 1);
    js_page_set_location(url);
    if (!root || !js_page_open(root)) { puts("FAIL: page initialization"); exit(1); }
    ctx = js_page_ctx();
    return root;
}

static int platform_focus;
static int query_focus(void) { return platform_focus; }
static int deliver(struct node *n, const char *type, int bubbles, int cancelable)
{
    struct js_event_init init={0}; init.bubbles=bubbles; init.cancelable=cancelable;
    return js_dom_dispatch(n,type,&init);
}

int main(void)
{
    css_init();
    js_dom_set_focus_query(query_focus);
    struct node *root=open_page("http://focus.example/");
    css_apply(root,"",0);
    fc_set_dispatch(deliver);
    check("typeof document.hasFocus==='function' && document.hasFocus()===false",
          "unfocused document has a real method returning false");
    check("Object.prototype.hasOwnProperty.call(Document.prototype,'hasFocus') && document.hasFocus.length===0",
          "Document prototype has a zero-argument method");
    check("(()=>{try{Document.prototype.hasFocus.call({});return false}catch(e){return e instanceof TypeError}})()",
          "method checks its Document receiver");
    check("(globalThis.transitions=[],addEventListener('focus',()=>transitions.push('focus:'+document.hasFocus())),"
          "addEventListener('blur',()=>transitions.push('blur:'+document.hasFocus())),true)",
          "listen for ordinary native focus transitions");
    platform_focus=1;
    check("document.hasFocus()===true && transitions.length===0", "synchronous query precedes queued notification");
    js_dom_sync_focus(); js_dom_sync_focus();
    check("transitions.join(',')==='focus:true' && document.activeElement===document.body",
          "body focus is true and unchanged notification is silent");
    check("document.implementation.createHTMLDocument('scratch').hasFocus()===false", "created document has no focus");
    check("(globalThis.parsed=new DOMParser().parseFromString('<p>local</p>','text/html'),parsed.hasFocus()===false)",
          "parsed data document has no focus");
    check("(()=>{try{parsed.hasFocus.call(parsed.body);return false}catch(e){return e instanceof TypeError}})()",
          "parsed method checks its Document receiver");
    check("(globalThis.input=document.createElement('input'),document.body.appendChild(input),input.focus(),"
          "document.activeElement===input && document.hasFocus())", "element focus shares the real document owner");
    platform_focus=0; js_dom_sync_focus();
    check("document.activeElement===input && document.hasFocus()===false && transitions.join(',')==='focus:true,blur:false'",
          "background window retains activeElement but loses document focus");
    platform_focus=-1; js_dom_sync_focus();
    check("document.hasFocus()===false && transitions.length===2", "unknown platform query cannot turn minus one into true");
    platform_focus=1; js_dom_sync_focus();
    check("(input.blur(),document.activeElement===document.body && document.hasFocus())", "element blur returns to the focused body");
    check("(globalThis.a=document.createElement('iframe'),globalThis.b=document.createElement('iframe'),"
          "document.body.appendChild(a),document.body.appendChild(b),"
          "a.contentDocument.hasFocus()===false && b.contentDocument.hasFocus()===false)", "unfocused sibling frames are false");
    check("(a.focus(),document.activeElement===a && a.contentDocument.hasFocus() && !b.contentDocument.hasFocus() && document.hasFocus())",
          "focused iframe and its document ancestor are true");
    check("(b.focus(),!a.contentDocument.hasFocus() && b.contentDocument.hasFocus())", "moving to sibling changes the frame focus chain");
    platform_focus=0; js_dom_sync_focus();
    check("!document.hasFocus() && !a.contentDocument.hasFocus() && !b.contentDocument.hasFocus()", "background owner makes all frame documents false");
    platform_focus=1; js_dom_sync_focus();
    check("(input.focus(),!a.contentDocument.hasFocus() && !b.contentDocument.hasFocus() && document.hasFocus())",
          "returning to parent input clears descendant focus");
    js_dom_set_focus_query(NULL); js_dom_sync_focus();
    check("!document.hasFocus()", "embedder without a focus provider does not invent focus");
    fc_set_dispatch(NULL); focus_reset();
    js_page_close();dom_free(root);
    printf("document-focus: %d checks, %d failures\n",checks,failed);
    return failed?1:0;
}
