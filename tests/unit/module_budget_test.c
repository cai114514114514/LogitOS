/* Real module loader + real watchdog; only transport data and the clock are
 * deterministic. A delayed dependency must not consume interpreter time, but
 * CPU time already spent, CPU time after loading and frozen-clock fuel must. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dom.h"
#include "js_page.h"
#include "js_module.h"
void *kmalloc(unsigned long n){return malloc(n);} void kfree(void*p){free(p);}
void img_register(void*p){(void)p;}
static unsigned long long now=1000;
static int fetch_delay, prefetch_delay, fetch_fail, fetches, waits, failures, checks, serial;
static unsigned long long clock_ms(void){return now;}
int bfetch_resolve(const char*b,const char*r,char*o,int n){(void)b;if(strstr(r,"://"))snprintf(o,n,"%s",r);else snprintf(o,n,"https://module.test/%s",r[0]=='.'&&r[1]=='/'?r+2:r);return 0;}
void bfetch_prefetch(const char*u){(void)u;}
void bfetch_prefetch_wait(void){waits++;now+=(unsigned)prefetch_delay;}
int res_fetch(const char*u,unsigned char**p,int*n){(void)u;fetches++;now+=(unsigned)fetch_delay;if(fetch_fail)return -1;const char*s="export const v=7;";*n=(int)strlen(s);*p=malloc(*n+1);memcpy(*p,s,*n+1);return 0;}
static void ck(int ok,const char*s){checks++;printf("%s %s\n",ok?"PASS":"FAIL",s);if(!ok)failures++;}
static int value(const char*s){JSContext*c=js_page_ctx();JSValue v=JS_Eval(c,s,strlen(s),"<assert>",0);int ok=!JS_IsException(v)&&JS_ToBool(c,v);if(JS_IsException(v))JS_FreeValue(c,JS_GetException(c));JS_FreeValue(c,v);return ok;}
static JSValue cpu(JSContext*c,JSValueConst t,int ac,JSValueConst*av){(void)t;int n=0;if(ac)JS_ToInt32(c,&n,av[0]);now+=n>0?n:0;return JS_UNDEFINED;}
static int module(const char*body){char src[2048],url[128];++serial;snprintf(url,sizeof url,"https://module.test/entry-%d.js",serial);snprintf(src,sizeof src,"import {v} from './dep-%d.js';%s",serial,body);return js_module_eval(src,(int)strlen(src),url);}
int main(void){
 struct node*root=dom_parse("<html><body></body></html>",26);if(!root)return 2;
 js_page_set_clock(clock_ms);js_page_set_slice_ms(50);if(!js_page_open(root))return 2;
 JSContext*c=js_page_ctx();JSValue g=JS_GetGlobalObject(c);JS_SetPropertyStr(c,g,"cpu",JS_NewCFunction(c,cpu,"cpu",1));JS_FreeValue(c,g);
 const char*work="var sum=0;for(var i=0;i<20000;i++)sum+=i;globalThis.answer=sum+v;";
 int hits=js_page_slice_hits();prefetch_delay=60000;
 ck(module(work)&&value("answer===199990007"),"prefetch wait does not consume module CPU budget");
 ck(waits>0&&js_page_slice_hits()==hits,"prefetch consumer really waited without a watchdog hit");
 prefetch_delay=0;fetch_delay=60000;hits=js_page_slice_hits();
 ck(module(work)&&value("answer===199990007"),"cache miss fetch does not consume module CPU budget");
 ck(fetches>=2&&js_page_slice_hits()==hits,"module loader really fetched without a watchdog hit");
 fetch_delay=0;hits=js_page_slice_hits();
 ck(!module("cpu(51);for(var i=0;i<20000;i++);"),"module CPU wall time still interrupts");
 ck(js_page_slice_hits()==hits+1,"module CPU wall-time hit counted");
 js_page_set_slice_fuel(1);hits=js_page_slice_hits();
 ck(!module("for(var i=0;i<100000;i++);"),"module frozen-clock fuel still interrupts");
 ck(js_page_slice_hits()==hits+1,"module fuel hit counted");js_page_set_slice_fuel(0);
 /* A scope extends the old deadline; restarting a fresh 50ms slice would
  * incorrectly forgive the 30ms of computation before this 60-second wait. */
 js_page_slice_begin();now+=30;unsigned long long io=js_page_slice_io_begin();now+=60000;js_page_slice_io_end(io);now+=30;hits=js_page_slice_hits();
 ck(!value("(()=>{for(var i=0;i<20000;i++);return true})()")&&js_page_slice_hits()==hits+1,"I/O scope preserves CPU time already spent");js_page_slice_end();
 /* Overlapping I/O intervals must not buy extra compute time. */
 js_page_slice_begin();io=js_page_slice_io_begin();now+=30000;unsigned long long inner=js_page_slice_io_begin();now+=30000;js_page_slice_io_end(inner);now+=30000;js_page_slice_io_end(io);now+=51;hits=js_page_slice_hits();
 ck(!value("(()=>{for(var i=0;i<20000;i++);return true})()")&&js_page_slice_hits()==hits+1,"nested I/O scopes exclude elapsed time once");js_page_slice_end();
 js_page_slice_begin();io=js_page_slice_io_begin();js_page_slice_end();now+=60000;js_page_slice_begin();js_page_slice_io_end(io);now+=51;hits=js_page_slice_hits();
 ck(!value("(()=>{for(var i=0;i<20000;i++);return true})()")&&js_page_slice_hits()==hits+1,"old I/O token cannot extend a new slice");js_page_slice_end();
 fetch_fail=1;fetch_delay=60000;ck(!module(work),"failed network dependency is still reported");now+=60000;hits=js_page_slice_hits();
 ck(value("(()=>{for(var i=0;i<20000;i++);return true})()")&&js_page_slice_hits()==hits,"failed module leaves no stale watchdog deadline");
 fetch_fail=0;fetch_delay=0;ck(module(work),"module recovers after failed dependency");now+=60000;hits=js_page_slice_hits();
 ck(value("(()=>{for(var i=0;i<20000;i++);return true})()")&&js_page_slice_hits()==hits,"completed module leaves no stale watchdog deadline");
 js_page_close();dom_free(root);printf("module-budget: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
