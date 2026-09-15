/* The product scheduler is included unchanged. Only this host TU wraps its
 * context constructor to install finite fixture arithmetic in child realms;
 * no test hook, clock manipulation or fixture API enters the browser binary. */
#include "quickjs.h"
extern int worker_fairness_add(int a,int b,int label);
static JSValue fairness_add(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv)
{
    (void)self;int32_t a=0,b=0,label=0;
    if(argc<3||JS_ToInt32(ctx,&a,argv[0])||JS_ToInt32(ctx,&b,argv[1])||JS_ToInt32(ctx,&label,argv[2]))return JS_EXCEPTION;
    return JS_NewInt32(ctx,worker_fairness_add(a,b,label));
}
static JSContext *fairness_context(JSRuntime *rt)
{
    JSContext *ctx=JS_NewContext(rt);
    if(ctx){JSValue g=JS_GetGlobalObject(ctx);JS_SetPropertyStr(ctx,g,"fixtureAdd",JS_NewCFunction(ctx,fairness_add,"fixtureAdd",3));JS_FreeValue(ctx,g);}
    return ctx;
}
#define JS_NewContext fairness_context
#ifndef WORKER_FAIRNESS_IMPL
#define WORKER_FAIRNESS_IMPL "../../c/apps/browser/js_worker.c"
#endif
#include WORKER_FAIRNESS_IMPL
#undef JS_NewContext
