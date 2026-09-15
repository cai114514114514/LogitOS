#define main syntax_existing_main
#include "js_syntax_test.c"
#undef main
int main(int argc,char **argv)
{
    JSRuntime *rt=JS_NewRuntime();JSContext *ctx=JS_NewContext(rt);
    expect_value(ctx,"Unicode escaped hyphen in a class","/[\\-]/u.test('-')","true");
    expect_value(ctx,"hyphen remains literal not range","/[a\\-z]/u.test('m')","false");
    expect_value(ctx,"escaped hyphen constructor","new RegExp('[\\\\-]','u').test('-')","true");
    expect_value(ctx,"negated escaped hyphen","/[^\\-]/u.test('-')","false");
    expect_value(ctx,"ordinary range retained","/[a-z]/u.test('m')","true");
    expect_value(ctx,"legacy identity escape retained","/\\-/.test('-')","true");
    expect_value(ctx,"site editor character class",
        "/[a-zA-Z0-9_<>\\-\\./\\\\:\\*\\?\\+\\[\\]\\^,#@;\"%\\$\\p{L}-]+/uy.test('文件-name.ts')","true");
    expect_syntax_error(ctx,"hyphen outside Unicode class remains invalid","/\\-/u");
    expect_syntax_error(ctx,"arbitrary Unicode identity escape remains invalid","/[\\q]/u");
    expect_syntax_error(ctx,"reversed range remains invalid","/[z-a]/u");
    if(argc>1){
        size_t len=0;char *source=slurp(argv[1],&len);checks++;
        if(!source)fail("real module input","could not read file");
        else{
            JSValue v=JS_Eval(ctx,source,len,argv[1],JS_EVAL_TYPE_MODULE|JS_EVAL_FLAG_COMPILE_ONLY);
            if(JS_IsException(v)){
                JSValue e=JS_GetException(ctx);const char *s=JS_ToCString(ctx,e);
                fail("real module compiles",s);if(s)JS_FreeCString(ctx,s);JS_FreeValue(ctx,e);
            }else printf("real module: %zu bytes compiled (execution not asserted)\n",len);
            JS_FreeValue(ctx,v);free(source);
        }
    }
    JS_FreeContext(ctx);JS_FreeRuntime(rt);
    printf("regexp-class-escape: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
