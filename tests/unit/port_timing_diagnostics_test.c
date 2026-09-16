/* Queue delay and callback duration deliberately differ. The synthetic clock
 * tests field provenance; it is not a performance benchmark or a page clock
 * override in the product. The callback closes its endpoint before returning. */
#include "js_ports.h"
#include <stdio.h>
#include <string.h>
static unsigned long long now=100;
unsigned long long js_page_now_ms(void){return now;}
static JSValue advance(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv)
{(void)ctx;(void)self;(void)argc;(void)argv;now+=23;return JS_UNDEFINED;}
int main(void)
{
    JSRuntime *rt=JS_NewRuntime();JSContext *ctx=JS_NewContext(rt);
    if(!ctx||!js_ports_install(ctx))return 2;
    JSValue g=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,g,"localStep",JS_NewCFunction(ctx,advance,"localStep",0));JS_FreeValue(ctx,g);
    const char *src="var seen=0,c=new MessageChannel();c.port2.onmessage=function(e){seen=e.data;localStep();c.port2.close()};c.port1.postMessage(41);";
    JSValue v=JS_Eval(ctx,src,strlen(src),"<local port timing>",JS_EVAL_TYPE_GLOBAL);
    int good=!JS_IsException(v)&&js_ports_pending(ctx);JS_FreeValue(ctx,v);
    now=350;good=good&&js_ports_pump(ctx)==1&&!js_ports_pending(ctx);
    v=JS_Eval(ctx,"seen===41",9,"<local result>",JS_EVAL_TYPE_GLOBAL);
    good=good&&!JS_IsException(v)&&JS_ToBool(ctx,v)>0&&now==373;JS_FreeValue(ctx,v);
    js_ports_close(ctx);JS_FreeContext(ctx);JS_FreeRuntime(rt);
    puts(good?"port-timing: PASS":"port-timing: FAIL");return good?0:1;
}
