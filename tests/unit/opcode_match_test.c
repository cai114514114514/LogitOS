#include "quickjs.h"
#include <stdio.h>
#include <string.h>
int main(void)
{
    JSRuntime *rt=JS_NewRuntime();JSContext *ctx=JS_NewContext(rt);
    /* Nonconstant operands force the real compiler's typeof peephole; this
     * is not a copied M4 expression passing independently of the product. */
    const char *src="var a=[void 0,12,function(){},'x'];"
        "var ok=typeof a[0]==='undefined'&&typeof a[1]==='number'&&typeof a[2]==='function'&&typeof a[3]!=='number';"
        "var o={get blocked(){return 1}},named=false;"
        "try{(function(){'use strict';o.blocked=2})()}catch(e){named=e instanceof TypeError&&e.message.indexOf('blocked')>=0}"
        "ok&&named&&o.blocked===1";
    JSValue v=JS_Eval(ctx,src,strlen(src),"opcode-match.js",JS_EVAL_TYPE_GLOBAL);
    int ok=!JS_IsException(v)&&JS_ToBool(ctx,v)>0;
    if(JS_IsException(v)){JSValue e=JS_GetException(ctx);const char *s=JS_ToCString(ctx,e);printf("exception: %s\n",s?s:"");if(s)JS_FreeCString(ctx,s);JS_FreeValue(ctx,e);}
    JS_FreeValue(ctx,v);JS_FreeContext(ctx);JS_FreeRuntime(rt);
    puts(ok?"opcode-match: PASS":"opcode-match: FAIL");return ok?0:1;
}
