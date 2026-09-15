/* Real JS fetch -> HTTP decoder -> arrayBuffer. zlib here only PRODUCES the
 * fixture; the consumer remains the same safe-Rust inflater shipped in guest.
 * A tiny compressed response must not conceal a >8 MiB decoded-body limit. */
#define main webapi_existing_main
#include "webapi_test.c"
#undef main
#include <zlib.h>
#include "bfetch.h"

static unsigned char *compressed;
static int compressed_len;
static int big_send(int fd,const void *buf,int len)
{
    int was=fs[fd].answered;
    int rc=f_send(fd,buf,len);
    struct fakesock *s=&fs[fd];
    if(!was&&s->answered){
        s->rsp_len=s->rsp_off=0;s->slice=4096;
        char head[256];snprintf(head,sizeof head,
            "HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\nContent-Length: %d\r\n\r\n",compressed_len);
        rsp_add(s,head);
        if(s->rsp_len+compressed_len>RSP_MAX)abort();
        memcpy(s->rsp+s->rsp_len,compressed,(size_t)compressed_len);s->rsp_len+=compressed_len;
    }
    return rc;
}
static const struct webapi_net BIG={f_open,f_poll,big_send,f_recv,f_close,f_now};

static void sample(int size,int corrupt,int expect_ok)
{
    unsigned char *plain=malloc((size_t)size);if(!plain)abort();
    memset(plain,'A',(size_t)size);plain[0]=0;plain[size-1]=255;
    uLongf cap=compressBound((uLong)size);compressed=malloc(cap);if(!compressed)abort();
    if(compress2(compressed,&cap,plain,(uLong)size,9)!=Z_OK)abort();
    compressed_len=(int)cap;if(corrupt)compressed[compressed_len-1]^=255;
    printf("body-limit fixture wire=%d decoded=%d corrupt=%d\n",compressed_len,size,corrupt);
    free(plain);fs_reset();fake_now=1000;open_ctx("http://page.example/");js_webapi_set_net(&BIG);
    run("var result=null,error=null;fetch('/compressed').then(function(r){return r.arrayBuffer()})"
        ".then(function(b){var u=new Uint8Array(b);result=[u.length,u[0],u[1],u[u.length-1]]},function(e){error=String(e)})");
    settle(300);
    if(expect_ok){char assertion[200];snprintf(assertion,sizeof assertion,
        "error===null&&result&&result[0]===%d&&result[1]===0&&result[2]===65&&result[3]===255",size);
        ckjs(assertion,"compressed body beyond 8 MiB reaches fetch arrayBuffer intact");}
    else ckjs("result===null&&typeof error==='string'","oversize or corrupt compressed body remains refused");
    ck(!js_webapi_pending(),"body limit leaves no pending transport");
    close_ctx();free(compressed);compressed=0;
}
int main(void)
{
    sample(10*1024*1024+7,0,1);
    sample(17*1024*1024,0,0);
    sample(10*1024*1024+7,1,0);
    printf("browser-body-limit: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
