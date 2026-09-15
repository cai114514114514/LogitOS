/* Real local HTTP parser -> Fetch stream -> Response.json. All text and field
 * names below are invented sentinels; no response is injected into JavaScript. */
#include "stream_net.h"
static const char *body_text;
static int response_status,plain_mime;
static void route(struct fakesock *s,const char *method,const char *target)
{
    (void)method;(void)target;
    char headers[200];
    snprintf(headers,sizeof headers,"HTTP/1.1 %d Fixture\r\nContent-Type: %s\r\nContent-Length: %zu\r\n\r\n",response_status,plain_mime?"text/plain":"application/json",strlen(body_text));
    rsp_add(s,headers);s->avail=s->rsp_len;s->finished=0;
    if(strlen(body_text)>2000)s->slice=4096;
}
enum { NORMAL, POISON, CLONE, PLAIN };
static void specimen(const char *label,const char *body,int status,int valid,int mode)
{
    printf("fetch-json-case=%s\n",label);
    fs_reset();body_text=body;response_status=status;plain_mime=mode==PLAIN;
    fs_set_router(route);open_ctx("http://fixture.test/consumer");
    JSValue g=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,g,"expectedText",JS_NewString(ctx,body));JS_FreeValue(ctx,g);
    run("var gotHeaders=false,done=false,parseCalls=0,getterCalls=0,result,error,response,chosen;"
        "var originalParse=JSON.parse;JSON.parse=function(t){parseCalls++;return originalParse(t);};");
    char script[2000];
    snprintf(script,sizeof script,
        "fetch('/classification',{method:'POST',body:'local-fixture'}).then(function(r){"
        " response=r;gotHeaders=true;chosen=%s;"
        "%s"
        " chosen.json().then(function(v){result=v;done=true;},function(e){error=e;done=true;});"
        "});",
        mode==CLONE?"r.clone()":"r",
        mode==POISON?"['detail','type','loc','input'].forEach(function(k){Object.defineProperty(Object.prototype,k,{configurable:true,get:function(){getterCalls++;return 'PRIVATE_GETTER_SENTINEL';}});});Object.defineProperty(r,'status',{get:function(){getterCalls++;return 200;}});":"");
    run(script);settle(20);int fd=fs_live();
    ck(fd>=0&&all_req_n==1&&req_has(nth_req(0),"POST /classification HTTP/1.1"),"classification uses one real local HTTP request");
    ckjs("gotHeaders&&!done&&chosen.bodyUsed","json waits after headers while the HTTP body is incomplete");
    size_t n=strlen(body),half=n/2;char *first=malloc(half+1);memcpy(first,body,half);first[half]=0;
    fs_push(fd,first);free(first);settle(20);
    ckjs("!done&&parseCalls===0","partial HTTP text neither parses nor completes json");
    puts("fetch-json-release=complete");fs_push(fd,body+half);fs_finish(fd);settle(50);
    ckjs(valid?"done&&!error&&JSON.stringify(result)===JSON.stringify(originalParse(expectedText))":"done&&error instanceof SyntaxError",
         valid?"json resolves with the unmodified application value":"malformed JSON retains its ordinary SyntaxError rejection");
    ckjs("parseCalls===1","the public JSON parser is called exactly once");
    ckjs("getterCalls===0","classification invokes no prototype or public status getters");
    ckjs("typeof globalThis.__xhrDiag==='undefined'","the diagnostic hook remains private");
    run("var secondRejected=false;chosen.json().then(function(){},function(e){secondRejected=e instanceof TypeError;});");settle(10);
    ckjs("secondRejected&&parseCalls===1","body is consumed once and a second json call rejects normally");
    if(mode==CLONE){run("var cloneText=false;response.text().then(function(t){cloneText=t===expectedText;});");settle(15);ckjs("cloneText","cloned json consumption leaves the original tee readable");}
    ck(all_req_n==1,"diagnostics issue no additional request");
    close_ctx();puts("fetch-json-case-end");
}
int main(void)
{
    const char *mixed="{\"detail\":["
      "{\"type\":\"missing\",\"loc\":[\"body\",\"PRIVATE_FIELD_SENTINEL\"],\"msg\":\"PRIVATE_MESSAGE_SENTINEL\",\"input\":\"PRIVATE_INPUT_SENTINEL\"},"
      "{\"type\":\"missing\",\"loc\":[\"body\",\"PRIVATE_SECOND_FIELD_SENTINEL\"]},"
      "{\"type\":\"string_type\",\"loc\":[\"query\",\"PRIVATE_QUERY_SENTINEL\"]},"
      "{\"type\":\"string_pattern_mismatch\",\"loc\":[\"path\",\"PRIVATE_PATH_SENTINEL\"]},"
      "{\"type\":\"json_invalid\",\"loc\":[\"body\",4]},"
      "{\"type\":\"value_error\",\"loc\":[\"header\",\"PRIVATE_HEADER_SENTINEL\"]},"
      "{\"type\":\"PRIVATE_TYPE_SENTINEL\",\"loc\":[\"PRIVATE_LOCATION_SENTINEL\"]},"
      "{\"type\":\"missing\\u0000PRIVATE_SUFFIX_SENTINEL\",\"loc\":[\"body\\u0000PRIVATE_SUFFIX_SENTINEL\"]}]}";
    specimen("mixed",mixed,422,1,NORMAL);
    specimen("missing_poison","{}",422,1,POISON);
    specimen("object","{\"detail\":{\"type\":\"value_error\",\"loc\":[\"body\",\"PRIVATE_FIELD_SENTINEL\"]}}",400,1,NORMAL);
    specimen("string","{\"detail\":\"PRIVATE_DETAIL_SENTINEL\"}",401,1,NORMAL);
    specimen("null","{\"detail\":null}",403,1,NORMAL);
    specimen("number","{\"detail\":4287319}",404,1,NORMAL);
    specimen("boolean","{\"detail\":false}",409,1,NORMAL);
    specimen("empty_array","{\"detail\":[]}",422,1,NORMAL);
    specimen("invalid_json","{\"detail\": [",422,0,NORMAL);
    specimen("success",mixed,200,1,NORMAL);
    specimen("server_error",mixed,500,1,NORMAL);
    specimen("plain_mime","{\"detail\":[]}",422,1,PLAIN);
    specimen("clone","{\"detail\":[{\"type\":\"string_type\",\"loc\":[\"query\"]}]}",422,1,CLONE);
    char *large=malloc(70001);memset(large,' ',70000);memcpy(large,"{\"detail\":[]}",13);large[65536]=0;
    specimen("exact_cap",large,422,1,NORMAL);
    large[65536]=' ';large[66000]=0;specimen("over_cap",large,422,1,NORMAL);free(large);
    large=malloc(70000);const char *prefix="{\"detail\":[],\"input\":\"";size_t n=strlen(prefix);memcpy(large,prefix,n);
    for(int i=0;i<22000;i++){memcpy(large+n,"\xe4\xbd\xa0",3);n+=3;}memcpy(large+n,"\"}",3);
    specimen("utf8_byte_cap",large,422,1,NORMAL);free(large);
    printf("fetch-json-diagnostics: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
