/* The guest's second Python navigation lost fetch while installing the web
 * platform: the previous page's watchdog deadline survived idle time and fired
 * after just one poll in the NEW context. Link the actual webapi installer and
 * move a fake clock between pages; no sleeping or public network is involved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dom.h"
#include "js_page.h"
void *kmalloc(unsigned long n) { return malloc(n); }
void kfree(void *p) { free(p); }
/* These linked module/image installers are not exercised by this lifecycle
 * gate. Module fetching explicitly fails; no decoder is advertised to a page. */
void img_register(void *p) { (void)p; }
int bfetch_resolve(const char *b,const char *r,char *o,int n)
{ (void)b;(void)r;(void)o;(void)n;return -1; }
int res_fetch(const char *s,unsigned char **b,int *n)
{ (void)s;(void)b;(void)n;return -1; }
void bfetch_prefetch(const char *u) { (void)u; }
void bfetch_prefetch_wait(void) {}
static unsigned long long now = 1000;
static unsigned long long clock_ms(void) { return now; }
static int fails;
static void ck(int ok, const char *s) { printf("%s %s\n",ok?"PASS":"FAIL",s);if(!ok)fails++; }
static int eval(const char *s) { return js_page_eval(s,(int)strlen(s),"<lifecycle>",0); }
static int value(const char *s) {
    JSContext *c=js_page_ctx();JSValue v=JS_Eval(c,s,strlen(s),"<check>",JS_EVAL_TYPE_GLOBAL);
    int ok=!JS_IsException(v)&&JS_ToBool(c,v)==1;
    if(JS_IsException(v))JS_FreeValue(c,JS_GetException(c));
    JS_FreeValue(c,v);return ok;
}
int main(void) {
    const char *html="<html><body></body></html>";
    struct node *a=dom_parse(html,strlen(html)),*b=dom_parse(html,strlen(html));
    js_page_set_clock(clock_ms);js_page_set_slice_ms(50);
    ck(js_page_open(a),"first page opens");
    ck(value("typeof fetch==='function'"),"real fetch installer is linked");
    ck(eval("var sum=0;for(var i=0;i<20000;i++)sum+=i;"),"first script completes");
    int hits=js_page_slice_hits();
    now+=60000;
    ck(js_page_open(b),"second page opens after idle");
    ck(js_page_slice_hits()==hits,"new page installation ignores old deadline");
    ck(value("typeof fetch==='function'"),"second page retains fetch");
    /* Ending a slice must disarm it even when no navigation occurs: native
     * entry points such as event delivery are deliberately outside that slice. */
    ck(eval("1+1"),"new page script completes");
    now+=60000;
    ck(value("(()=>{var s=0;for(var i=0;i<20000;i++)s+=i;return s>0})()"),
       "idle native entry ignores completed deadline");
    /* A finite workload with one poll of fuel proves the watchdog still works;
     * resetting lifetime boundaries must not globally disable bounded eval. */
    js_page_set_slice_fuel(1);hits=js_page_slice_hits();
    ck(!eval("for(var i=0;i<100000;i++);"),"active slice still interrupts");
    ck(js_page_slice_hits()==hits+1,"active slice interruption counted");
    js_page_set_slice_fuel(0);ck(eval("1+1"),"next script recovers after interruption");
    js_page_close();dom_free(a);dom_free(b);
    printf("page-lifecycle: %d failures\n",fails);return fails?1:0;
}
