#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "dom.h"
#include "css.h"
#include "js_dom.h"
#include "js_page.h"
#include "forms.h"

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

int text_measure(const char *s,int len,int px,int mono){(void)s;(void)mono;return len*(px/2);}
static void native_check(const char *name,int ok){checks++;if(!ok){printf("FAIL %s\n",name);failures++;}}
static unsigned attrs;
static void observe(void *p,const struct dom_mutation *m){(void)p;if(m->kind==DOM_MUT_ATTRIBUTE)attrs++;}
int main(void)
{
 struct node *root=dom_parse("<html><body></body></html>",26);
 if(!root)return 2;
 struct node *p=dom_create_element(root->doc,"div",3),*t=dom_create_text(root->doc,"abc",3),*u=dom_create_element(root->doc,"b",1);
 dom_append_child(root,p);dom_append_child(p,t);
 struct dom_live_range r;dom_range_init(&r,root->doc);dom_range_set(&r,0,p,1);dom_range_set(&r,1,p,1);
 dom_insert_before(p,u,t);
 native_check("native insert shifts boundary",r.start_offset==2&&r.end_offset==2);
 dom_range_set(&r,0,t,1);dom_range_set(&r,1,t,3);dom_remove_child(p,t);
 native_check("native remove relocates descendant",r.start==p&&r.end==p&&r.start_offset==1&&r.end_offset==1);
 dom_range_set(&r,0,t,1);dom_range_set(&r,1,t,3);dom_text_replace(t,0,0,"好",3);
 native_check("UTF16 replacement shifts boundary",r.start_offset==2&&r.end_offset==4);
 struct dom_subscription sub={0};dom_subscribe(root->doc,&sub,observe,0);
 dom_set_attr(p,"class","x");dom_remove_attr(p,"class");
 native_check("attribute removal is one record",attrs==2&&p->nclass==0&&!dom_attr(p,"class"));
 dom_unsubscribe(&sub);
 dom_destroy_subtree(t);native_check("destroy invalidates detached boundaries",!r.start&&!r.end);
 dom_range_dispose(&r);
 js_page_set_location("http://example.com/");if(!js_page_open(root))return 2;
 js_page_eval("void 0;",7,"<warmup>",0);
 ck("prepare native edit range", "var editor=document.createElement('div');editor.id='native-editor';editor.contentEditable='true';editor.textContent='A😀bc';document.body.appendChild(editor);var nr=new Range();nr.setStart(editor.firstChild,3);nr.setEnd(editor.firstChild,5);true");
 struct node *editor=dom_get_element_by_id(root->doc,"native-editor");
 fc_ce_set_caret(editor->first_child,1);
 native_check("native editor inserts complete Unicode",fc_ce_insert("中",3));
 ck("native editor updates JS live Range", "nr.startOffset===4&&nr.endOffset===6&&nr.toString()==='bc'");
 fc_ce_clear();

 FILE *f=fopen("tests/fixtures/engine-expansion/range-checks.js","rb");if(!f)return 2;
 fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);char *src=malloc(n+1);if(!src)return 2;
 if(fread(src,1,n,f)!=(size_t)n)return 2;src[n]=0;fclose(f);
 JSValue run=JS_Eval(js_page_ctx(),src,n,"<range-fixture>",JS_EVAL_TYPE_GLOBAL);native_check("shared range fixture executes",!JS_IsException(run));JS_FreeValue(js_page_ctx(),run);free(src);
 JSContext *ctx=js_page_ctx();JSValue g=JS_GetGlobalObject(ctx),list=JS_GetPropertyStr(ctx,g,"__rangeChecks"),lv=JS_GetPropertyStr(ctx,list,"length");
 uint32_t count=0;JS_ToUint32(ctx,&count,lv);JS_FreeValue(ctx,lv);
 for(uint32_t i=0;i<count;i++){JSValue item=JS_GetPropertyUint32(ctx,list,i),ok=JS_GetPropertyStr(ctx,item,"ok"),name=JS_GetPropertyStr(ctx,item,"name"),error=JS_GetPropertyStr(ctx,item,"error");
 const char *label=JS_ToCString(ctx,name);checks++;if(!JS_ToBool(ctx,ok)){const char *why=JS_ToCString(ctx,error);printf("FAIL %s: %s\n",label,why?why:"");JS_FreeCString(ctx,why);failures++;}
 JS_FreeCString(ctx,label);JS_FreeValue(ctx,ok);JS_FreeValue(ctx,name);JS_FreeValue(ctx,error);JS_FreeValue(ctx,item);}
 JS_FreeValue(ctx,list);JS_FreeValue(ctx,g);
 js_page_close();
 dom_range_init(&r,root->doc);dom_free(root);native_check("document teardown clears subscriber before finalizer",!r.start&&!r.sub.doc);dom_range_dispose(&r);
 printf("live-range: %d checks, %d failed\n",checks,failures);return failures?1:0;
}
