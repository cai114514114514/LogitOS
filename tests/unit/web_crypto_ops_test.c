#define main iface_fixture_main
#include "dom_iface_test.c"
#undef main
int main(void)
{
    struct node *root = dom_parse(HTML, (int)strlen(HTML));
    js_page_set_location("https://example.com/");
    if (!root || !js_page_open(root)) return 2;
    g_ctx = js_page_ctx();
    FILE *f=fopen("tests/fixtures/engine-expansion/crypto-checks.js","rb");
    if(!f)return 2;
    char source[65536];size_t len=fread(source,1,sizeof source,f);fclose(f);
    if(len==sizeof source)return 2;
    JSValue v=JS_Eval(g_ctx,source,len,"crypto-checks.js",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(v)){JS_FreeValue(g_ctx,v);return 2;}JS_FreeValue(g_ctx,v);
    JSContext *jobctx;int turns=0,ran;
    while((ran=JS_ExecutePendingJob(JS_GetRuntime(g_ctx),&jobctx))>0&&++turns<1000){}
    ckjs("cryptoTestDone", "crypto promises terminate");
    ckjs("cryptoTestResults.length===152", "all vector and rejection cases ran");
    ckjs("cryptoTestResults.every(function(x){return x[1]})", "real crypto operations match independent vectors");
    const char *expr="JSON.stringify({total:cryptoTestResults.length,failed:cryptoTestResults.filter(function(x){return !x[1]})})";
    v=JS_Eval(g_ctx,expr,strlen(expr),"results",JS_EVAL_TYPE_GLOBAL);
    if(!JS_IsException(v)){const char *str=JS_ToCString(g_ctx,v);if(str){printf("mismatches: %s\n",str);JS_FreeCString(g_ctx,str);}}else{JSValue e=JS_GetException(g_ctx);JS_FreeValue(g_ctx,e);}JS_FreeValue(g_ctx,v);
    js_page_close();dom_free(root);
    printf("web-crypto-ops: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
