/* Real JS fetch/parser lifecycle over the ordinary socket fixture. Only the
 * first response is removed (or truncated after its headers); no fetch state
 * is patched by this test. POST and repeated failures are the safety controls. */
#define main webapi_existing_main
#include "webapi_test.c"
#undef main
static int replies,fail_always,after_headers;
static int retry_send(int fd,const void *buf,int len)
{
    int was=fs[fd].answered;
    int rc=f_send(fd,buf,len);
    if(!was&&fs[fd].answered){
        replies++;
        if(replies==1||fail_always){
            fs[fd].rsp_len=fs[fd].rsp_off=0;
            if(after_headers)rsp_add(&fs[fd],"HTTP/1.1 200 OK\r\nContent-Length: 99\r\n\r\npartial");
        }
    }
    return rc;
}
static const struct webapi_net RETRY={f_open,f_poll,retry_send,f_recv,f_close,f_now};
static void sample(const char *method,int always,int headers,int expect_ok,int expected_requests)
{
    fs_reset();fake_now=1000;replies=0;fail_always=always;after_headers=headers;
    open_ctx("http://page.example/");js_webapi_set_net(&RETRY);
    char script[400];snprintf(script,sizeof script,
        "var done=false,ok=false;fetch('/retry',{method:'%s'}).then(function(r){return r.text()})"
        ".then(function(t){ok=true;done=true},function(e){done=true})",method);
    run(script);settle(300);
    if(expect_ok)ckjs("done&&ok","unanswered read recovers with one replacement request");
    else ckjs("done&&!ok","nonretryable or repeated failure remains a rejection");
    ck(replies==expected_requests,"retry policy sends exactly the permitted number of requests");
    ck(!js_webapi_pending(),"retry leaves no pending transport");close_ctx();
}
int main(void)
{
    sample("GET",0,0,1,2);sample("HEAD",0,0,1,2);
    sample("POST",0,0,0,1);sample("GET",0,1,0,1);sample("GET",1,0,0,2);
    printf("fetch-fresh-retry: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
