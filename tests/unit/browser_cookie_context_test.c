/* SPDX-License-Identifier: MIT
 * Reuse only the range gate's socket fixture; browser_rt and the shared Cookie
 * parser are real. Snapshot tests change document/base between dial and send. */
#define hstub_send range_fixture_send
#define main range_existing_main
#include "range_test.c"
#undef main
#undef hstub_send
#include "cookies.h"
static struct cookie_jar jar;
static int store_kind, bounce;
int hstub_send(int fd,const void *buf,int len)
{
    int rc=range_fixture_send(fd,buf,len);
    struct hsock *sock=sk(fd);
    if(sock && sock->s2c.len>0){
        struct pipebuf *out=&sock->s2c;
        if(bounce && (!strncmp(g_head,"GET /moved.bin ",15) || !strncmp(g_head,"GET /hop ",9))){
            out->len=out->off=0;
            pb_puts(out,!strncmp(g_head,"GET /hop ",9)?
                "HTTP/1.1 302 Found\r\nLocation: https://origin.test/full.bin\r\nContent-Length: 0\r\n\r\n":
                "HTTP/1.1 302 Found\r\nLocation: https://outside.test/hop\r\nContent-Length: 0\r\n\r\n");
        }
        for(int i=0;i+3<out->len;i++)if(!memcmp(out->b+i,"\r\n\r\n",4)){
            int old=out->len;unsigned char *copy=malloc((size_t)old);
            memcpy(copy,out->b,(size_t)old);out->len=0;
            pb_put(out,copy,i+2);
            pb_puts(out,"Set-Cookie: received=1; Secure; SameSite=Strict; Path=/\r\n");
            pb_put(out,copy+i+2,old-i-2);free(copy);break;
        }
    }
    return rc;
}
int webapi_cookie_line_request(const char *host,const char *path,int secure,
                              const struct cookie_request *r,char *out,int cap)
{ struct cookie_ctx c={host,path,secure,1};return cookie_header_ex(&jar,&c,cookie_request_kind(&c,r),1700000000,out,cap); }
void webapi_cookie_store_request(const char *host,const char *path,int secure,
                                const struct cookie_request *r,const char *v)
{ struct cookie_ctx c={host,path,secure,1};store_kind=cookie_request_kind(&c,r);cookie_set_ex(&jar,&c,store_kind,v,1700000000); }
static void check_cookie(const char *name,int expected)
{ char value[2048];head_get(g_head,"Cookie",value,sizeof value);OK((strstr(value,name)!=0)==expected); }
int main(void)
{
    struct cookie_ctx c={"origin.test","/",1,1};cookie_jar_init(&jar);
    cookie_set(&jar,&c,"strict=1; SameSite=Strict; Secure; Path=/",1700000000);
    cookie_set(&jar,&c,"lax=1; SameSite=Lax; Secure; Path=/",1700000000);
    cookie_set(&jar,&c,"none=1; SameSite=None; Secure; Path=/",1700000000);
    bfetch_init();bfetch_set_bypass(1);
    bfetch_set_document("https://origin.test/page");
    bfetch_set_base("https://stylesheet.test/style.css");
    int id=bfetch_start_from("https://stylesheet.test/style.css","https://origin.test/full.bin");
    bfetch_set_document("https://replacement.test/");
    OK(id>=0);OK(settle(id)==BF_DONE);check_cookie("strict=1",1);OK(store_kind==CK_REQ_SAME_SITE);bfetch_release(id);

    bfetch_set_document("https://outside.test/");bfetch_set_base("https://origin.test/");
    id=bfetch_start("https://origin.test/full.bin");
    OK(settle(id)==BF_DONE);check_cookie("strict=1",0);check_cookie("lax=1",0);check_cookie("none=1",1);OK(store_kind==CK_REQ_CROSS_SITE);bfetch_release(id);
    id=bfetch_start_nav_from("https://origin.test/moved.bin","https://outside.test/");
    bfetch_set_document("https://origin.test/");
    OK(settle(id)==BF_DONE);check_cookie("strict=1",0);check_cookie("lax=1",1);OK(store_kind==CK_REQ_CROSS_SITE_NAV);bfetch_release(id);
    id=bfetch_start_nav_from("https://origin.test/full.bin",0);
    OK(settle(id)==BF_DONE);check_cookie("strict=1",1);bfetch_release(id);
    id=bfetch_start_nav_from("https://origin.test/full.bin","about:blank");
    OK(settle(id)==BF_DONE);check_cookie("strict=1",0);bfetch_release(id);
    bounce=1;bfetch_set_document("https://origin.test/");
    id=bfetch_start("https://origin.test/moved.bin");
    OK(settle(id)==BF_DONE);check_cookie("strict=1",0);OK(store_kind==CK_REQ_CROSS_SITE);
    bfetch_release(id);bounce=0;
    /* Total header pressure must fail BOTH the cache lookup and wire doors.
     * Each synthetic cookie fits the parser's own bound; their sum does not. */
    char large[3200];
    for(int i=0;i<3;i++){
        snprintf(large,sizeof large,"big%d=",i);int n=(int)strlen(large);
        memset(large+n,'x',3000);strcpy(large+n+3000,"; Secure; Path=/");
        OK(cookie_set(&jar,&c,large,1700000000)==0);
    }
    bfetch_set_document("https://origin.test/");
    for(int bypass=0;bypass<2;bypass++){
        bfetch_set_bypass(bypass);int before=g_nreq;
        id=bfetch_start("https://origin.test/full.bin");
        OK(id>=0);OK(settle(id)==BF_FAILED);OK(g_nreq==before);bfetch_release(id);
    }
    bfetch_close_all();cookie_jar_free(&jar);
    printf("browser_cookie_context: %d checks, %d failures\n",checks,fails);
    return fails?1:0;
}
