#include "quickjs.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    JSRuntime *rt=JS_NewRuntime();JSContext *ctx=JS_NewContext(rt);
    if(!ctx)return 2;
    const double values[]={4294967295.0,-2147483649.0,2147483648.0,
        2147483647.0,-2147483648.0,2147483647.5,-2147483648.5,
        1e300,-1e300,INFINITY,-INFINITY,NAN,-0.0,0.0,1.5};
    int failures=0,checks=0;
    for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);i++){
        double actual=0,d=values[i];
        JSValue v=JS_NewFloat64(ctx,d);checks++;
        if(JS_ToFloat64(ctx,&actual,v)||
           (isnan(d)?!isnan(actual):actual!=d||(d==0&&signbit(d)!=signbit(actual)))){
            printf("FAIL number roundtrip %u\n",i);failures++;
        }
        JS_FreeValue(ctx,v);
    }
    /* Page arithmetic must use the same boxing door as embedders. */
    const char *src="var x=2147483647; x+1===2147483648 && x*2+1===4294967295 && Object.is(-0,-1/Infinity) && Number.isNaN(0/0)";
    JSValue v=JS_Eval(ctx,src,strlen(src),"number-boxing.js",JS_EVAL_TYPE_GLOBAL);checks++;
    if(JS_IsException(v)||JS_ToBool(ctx,v)<=0){puts("FAIL page numeric semantics");failures++;}
    JS_FreeValue(ctx,v);JS_FreeContext(ctx);JS_FreeRuntime(rt);
    printf("number-boxing: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
