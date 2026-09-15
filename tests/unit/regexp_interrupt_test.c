/* Cancellation of ordinary LINEAR matches over a large text buffer. No
 * pathological pattern is needed: a native VM must service the embedder's
 * callback even when the surrounding JS executes only one call. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
static int calls,failed,checks;
static int cancel(JSRuntime *rt,void *p){(void)rt;(void)p;calls++;return 1;}
#define CHECK(c,m) do{checks++;if(!(c)){failed++;printf("FAIL: %s\n",m);}else printf("ok: %s\n",m);}while(0)
static void test(JSRuntime *rt,JSContext *ctx,const char *source)
{
    calls=0;JS_SetInterruptHandler(rt,cancel,0);
    JSValue v=JS_Eval(ctx,source,strlen(source),"<regexp-cancel>",JS_EVAL_TYPE_GLOBAL);
    CHECK(JS_IsException(v)&&calls>0,"native regexp services cancellation");
    if(JS_IsException(v)){
        JSValue e=JS_GetException(ctx);const char *s=JS_ToCString(ctx,e);
        CHECK(s&&strstr(s,"interrupted")&&!strstr(s,"memory"),"cancellation preserves interrupted error");
        if(s)JS_FreeCString(ctx,s);JS_FreeValue(ctx,e);
    }
    JS_FreeValue(ctx,v);JS_SetInterruptHandler(rt,0,0);
    v=JS_Eval(ctx,"6*7",3,"<after-cancel>",JS_EVAL_TYPE_GLOBAL);int n=0;
    JS_ToInt32(ctx,&n,v);CHECK(!JS_IsException(v)&&n==42,"runtime remains usable after regexp cancellation");JS_FreeValue(ctx,v);
}
int main(void)
{
    JSRuntime *rt=JS_NewRuntime();JSContext *ctx=JS_NewContext(rt);
    char *s=malloc(131073);memset(s,'a',131072);s[131072]=0;
    JSValue g=JS_GetGlobalObject(ctx);JS_SetPropertyStr(ctx,g,"subject",JS_NewStringLen(ctx,s,131072));JS_FreeValue(ctx,g);free(s);
    test(rt,ctx,"/^a+$/u.exec(subject)");
    test(rt,ctx,"subject.replace(/^a+$/u,'done')");
    test(rt,ctx,"try { /^a+$/u.test(subject) } catch(e) { 'caught' }");
    const char *plain="/^a+$/u.test(subject)";
    JSValue v=JS_Eval(ctx,plain,strlen(plain),"<no-cancel>",JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(v)&&JS_ToBool(ctx,v),"uncancelled linear match keeps its real result");JS_FreeValue(ctx,v);
    JS_FreeContext(ctx);JS_FreeRuntime(rt);
    printf("regexp-interrupt: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
