/* SPDX-License-Identifier: MIT
 * Document.referrer shape and lifetime on shipping DOM/page/WebAPI sources.
 * Transport is absent; no third-party page or authentication is exercised. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "dom.h"
#include "js_page.h"
#include "js_webapi.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void kfree(void *p) { free(p); }
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
    JSValue v = JS_Eval(ctx, src, strlen(src), "<referrer-test>", JS_EVAL_TYPE_GLOBAL);
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

int main(void)
{
    struct node *root = open_page("https://source.example/page?private=local#fragment");
    check("typeof document.referrer === 'string' && document.referrer === ''",
          "direct navigation has a string and no invented source");
    check("(()=>{let d=Object.getOwnPropertyDescriptor(Document.prototype,'referrer');"
          "return !!d && typeof d.get==='function' && d.set===undefined})()",
          "Document prototype owns a readonly accessor");
    check("(()=>{try{document.referrer='https://invented.example/'}catch(e){}"
          "let ok=document.referrer==='';delete document.referrer;return ok})()", "assignment cannot replace referrer");
    check("(()=>{'use strict';try{document.referrer='x';return false}"
          "catch(e){return e instanceof TypeError}finally{delete document.referrer}})()", "strict assignment throws TypeError");
    check("(()=>{let g=Object.getOwnPropertyDescriptor(Document.prototype,'referrer').get;"
          "try{g.call({});return false}catch(e){return e instanceof TypeError}})()",
          "getter rejects a non-Document receiver");
    check("document.implementation.createHTMLDocument('local').referrer === ''",
          "script-created document has no referrer");
    check("(()=>{let d=new DOMParser().parseFromString('<p>local</p>','text/html');"
          "return typeof d.referrer==='string' && d.referrer===''})()",
          "DOMParser document has its own empty string referrer");
    check("(()=>{let d=new DOMParser().parseFromString('<p>local</p>','text/html');"
          "let g=Object.getOwnPropertyDescriptor(Object.getPrototypeOf(d),'referrer').get;"
          "try{g.call(d.body);return false}catch(e){return e instanceof TypeError}})()",
          "DOMParser getter rejects an Element receiver");
    check("(globalThis.promiseResult='pending',Promise.resolve().then(()=>document.referrer.substr(0,100))"
          ".then(v=>promiseResult=v===''?'ok':'bad',()=>promiseResult='rejected'),true)",
          "start ordinary Promise string consumption");
    JSContext *jobctx;
    for (int i=0;i<16 && JS_ExecutePendingJob(JS_GetRuntime(ctx),&jobctx)>0;i++) {}
    check("promiseResult === 'ok'", "Promise referrer.substr resolves without TypeError");
    check("(history.pushState(null,'','/changed#hash'),document.referrer === '')",
          "same-document history does not turn current URL into a referrer");
    check("(location.hash='next',document.referrer === '')",
          "fragment navigation preserves the creation-time empty referrer");
    check("(Object.defineProperty(document,'referrer',{value:'https://old.example/private#old'}),"
          "document.referrer.indexOf('old.example')>0)", "page-local shadow for close/reopen control");
    js_page_close();
    dom_free(root);
    root = open_page("https://destination.example/");
    check("document.referrer === ''", "fresh document cannot inherit prior realm's referrer shadow");
    check("document.implementation.createHTMLDocument('next').referrer === ''",
          "fresh realm detached document has no prior source");
    js_page_close();
    dom_free(root);
    printf("document-referrer: %d checks, %d failures\n", checks, failed);
    return failed ? 1 : 0;
}
