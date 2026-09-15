/* SPDX-License-Identifier: MIT
 * Normal template contents, repeated writes and namespace insertion on shipping DOM sources.
 * Transport is absent; no third-party page or authentication is exercised. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "dom.h"
#include "js_page.h"
#include "js_webapi.h"
#include "js_dom.h"
#include "js_semantics.h"
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


int main(int argc, char **argv) {
    const char *fixture = argc > 1 ? argv[1] : "tests/fixtures/template-content/checks.js";
    FILE *fp=fopen(fixture,"rb"); if(!fp)return 2;
    fseek(fp,0,SEEK_END);long len=ftell(fp);rewind(fp);
    char *source=malloc((size_t)len+1);if(!source)return 2;
    if(fread(source,1,(size_t)len,fp)!=(size_t)len)return 2;
    source[len]=0;fclose(fp);
    css_init();
    const char html[]="<!doctype html><html><head></head><body>local fixture</body></html>";
    struct node *root=dom_parse(html,sizeof(html)-1);
    js_page_set_location("http://static-dom.example/");
    if(!root||!js_page_open(root))return 2;
    JSContext *ctx=js_page_ctx();
    JSValue value=JS_Eval(ctx,source,(size_t)len,"<normal-static-dom>",JS_EVAL_TYPE_GLOBAL);
    int failed=JS_IsException(value);
    JSValue report=failed?JS_GetException(ctx):JS_DupValue(ctx,value);
    const char *text=JS_ToCString(ctx,report);
    printf("STATIC_DOM_RESULT %s\n",text?text:"null");
    if(text)JS_FreeCString(ctx,text);
    JS_FreeValue(ctx,report);JS_FreeValue(ctx,value);free(source);
    int checks=0, failures=0;
    JSValue global=JS_GetGlobalObject(ctx);
    JSValue result=JS_GetPropertyStr(ctx,global,"templateContentResult");
    JSValue count=JS_GetPropertyStr(ctx,result,"checks");
    JSValue errors=JS_GetPropertyStr(ctx,result,"failed");
    if(JS_ToInt32(ctx,&checks,count)<0 || JS_ToInt32(ctx,&failures,errors)<0 || checks!=21)
        failed=1;
    JS_FreeValue(ctx,errors);JS_FreeValue(ctx,count);
    JS_FreeValue(ctx,result);JS_FreeValue(ctx,global);
    /* The semantics installer now returns two private callbacks. Exercise the
     * existing native invoker callback as well, so slot zero cannot silently
     * become the template callback while template-only checks stay green. */
    const char setup[]="globalThis.nativeCommandCount=0;"
        "var inv=document.createElement('button'),dst=document.createElement('div');"
        "inv.id='template-test-invoker';document.body.appendChild(inv);document.body.appendChild(dst);"
        "inv.commandForElement=dst;inv.command='--ordinary-check';"
        "dst.addEventListener('command',()=>nativeCommandCount++);";
    if(!js_page_eval(setup,sizeof(setup)-1,"<ordinary-native-invoker>",0))failed=1;
    struct node *invoker=dom_get_element_by_id(root->doc,"template-test-invoker");
    int handled=js_semantics_activate_invoker(invoker);
    const char observed[]="nativeCommandCount===1";
    JSValue seen=JS_Eval(ctx,observed,sizeof(observed)-1,"<ordinary-native-invoker>",JS_EVAL_TYPE_GLOBAL);
    int invoker_ok=handled==1&&!JS_IsException(seen)&&JS_ToBool(ctx,seen);
    JS_FreeValue(ctx,seen);
    printf("template-native-invoker: %s\n",invoker_ok?"PASS":"FAIL");
    if(!invoker_ok)failed=1;
    printf("template-content: %d checks, %d failures\n",checks,failures);
    js_page_close();dom_free(root);return failed?2:failures?1:0;
}
