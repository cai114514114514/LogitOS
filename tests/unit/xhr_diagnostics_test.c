/* Real local HTTP/XHR path, with synthetic fields only. No diagnostic test
 * may manufacture a response or replace Fetch to make its log appear. */
#define main xhr_progress_base_main
#include "xhr_progress_test.c"
#undef main
static const char *diag_body;
static int diag_json;
static void diag_route(struct fakesock *s,const char *method,const char *target)
{
    (void)method;(void)target;
    /* The 66 KiB case tests the diagnostic parse cap, not 9,000 individual
     * network reads. Other cases keep the harness's awkward 7-byte slices. */
    if(strlen(diag_body)>65536)s->slice=4096;
    char h[160];
    snprintf(h,sizeof h,"HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n\r\n",diag_json?"application/json":"text/plain",strlen(diag_body));
    rsp_add(s,h);rsp_add(s,diag_body);
}
static void diag_case(const char *label,const char *body,int json,const char *setup)
{
    printf("diag-case=%s\n",label);
    begin();diag_body=body;diag_json=json;fs_set_router(diag_route);
    run("var getterCalls=0;globalThis.reportError=function(){};");
    if(setup)run(setup);
    run("x.open('POST','/metadata');x.send('local-fixture');");settle(60);
    ckjs("loads===1&&ends===1&&errors===0&&x.status===200","diagnostics preserve successful XHR completion");
    ckjs("getterCalls===0","diagnostics invoke no exception or JSON prototype getter");
    ckjs("typeof globalThis.__xhrDiag==='undefined'","diagnostic native hook remains private");
    close_ctx();
}
int main(void)
{
    xhr_progress_base_main();
    diag_case("integer","{\"code\":0,\"data\":{\"biz_code\":104},\"msg\":\"PRIVATE_MESSAGE_SENTINEL\",\"biz_data\":\"PRIVATE_PAYLOAD_SENTINEL\"}",1,0);
    diag_case("non_integer","{\"code\":\"PRIVATE_CODE_SENTINEL\",\"data\":{\"biz_code\":1.5}}",1,0);
    diag_case("missing","{}",1,"['code','data','biz_code'].forEach(function(k){Object.defineProperty(Object.prototype,k,{configurable:true,get:function(){getterCalls++;return 999}})});");
    diag_case("type_error","{\"code\":-17}",1,"var problem=new TypeError('PRIVATE_EXCEPTION_SENTINEL');['name','stack','message'].forEach(function(k){Object.defineProperty(problem,k,{get:function(){getterCalls++;return 'PRIVATE_GETTER_SENTINEL'}})});x.onprogress=function(){throw problem};");
    diag_case("proxy","{}",1,"var problem=new Proxy({}, {get:function(){getterCalls++;return 'PRIVATE_PROXY_SENTINEL'},getPrototypeOf:function(){getterCalls++;return null}});x.onprogress=function(){throw problem};");
    diag_case("invalid_json","{\"code\":",1,0);
    diag_case("plain_mime","{\"code\":7351,\"data\":{\"biz_code\":7352}}",0,0);
    char *big=malloc(66001);memset(big,' ',66000);memcpy(big,"{\"code\":8351}",13);big[66000]=0;
    diag_case("oversize",big,1,0);free(big);
    const char *prefix="{\"code\":9351,\"msg\":\"";
    big=malloc(70000);size_t n=strlen(prefix);memcpy(big,prefix,n);
    for(int i=0;i<23000;i++){memcpy(big+n,"\xe4\xbd\xa0",3);n+=3;}
    memcpy(big+n,"\"}",3);
    diag_case("utf8_byte_cap",big,1,0);free(big);
    printf("xhr-diagnostics: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
