/* Real native HTTP and Cookie code against the existing owned socket fixture.
 * Tests the new owner/ancestor seam independently of the paint test's fake. */
#define hstub_send range_send_base
#define main range_original_main
#include "range_test.c"
#undef main
#undef hstub_send
#include "cookies.h"
static struct cookie_jar jar;
static int policy_mode,redirect_calls,deny_redirect;
int hstub_send(int fd,const void *buf,int len){int rc=range_send_base(fd,buf,len);struct hsock *sock=sk(fd);
 if(sock&&sock->s2c.len){struct pipebuf *p=&sock->s2c;for(int i=0;i+3<p->len;i++)if(!memcmp(p->b+i,"\r\n\r\n",4)){
  int n=p->len;unsigned char *copy=malloc(n);memcpy(copy,p->b,n);p->len=0;pb_put(p,copy,i+2);
  pb_puts(p,"Content-Security-Policy: script-src 'none'\r\nContent-Security-Policy: frame-ancestors https://outside.test\r\nX-Frame-Options: SAMEORIGIN\r\nX-Frame-Options: DENY\r\n");
  if(policy_mode){pb_puts(p,"Content-Security-Policy: ");for(int k=0;k<4100;k++)pb_puts(p,"x");pb_puts(p,"\r\n");}
  pb_put(p,copy+i+2,n-i-2);free(copy);break;
 }}return rc;}
int webapi_cookie_line_request(const char *host,const char *path,int secure,const struct cookie_request *r,char *out,int cap)
{struct cookie_ctx c={host,path,secure,1};return cookie_header_ex(&jar,&c,cookie_request_kind(&c,r),1700000000,out,cap);}
void webapi_cookie_store_request(const char *h,const char *p,int s,const struct cookie_request *r,const char *v)
{struct cookie_ctx c={h,p,s,1};cookie_set_ex(&jar,&c,cookie_request_kind(&c,r),v,1700000000);}
static int redirect(void *owner,const char *base,const char *ref,char *out,int cap)
{(void)owner;redirect_calls++;return !deny_redirect&&bfetch_resolve(base,ref,out,cap)==0;}
static void cookies(int strict,int lax){char value[2048];head_get(g_head,"Cookie",value,sizeof value);OK((strstr(value,"strict=1")!=0)==strict);OK((strstr(value,"lax=1")!=0)==lax);OK(strstr(value,"none=1")!=0);}
int main(void){struct cookie_ctx c={"origin.test","/",1,1};cookie_jar_init(&jar);
 cookie_set(&jar,&c,"strict=1; Secure; SameSite=Strict; Path=/",1700000000);cookie_set(&jar,&c,"lax=1; Secure; SameSite=Lax; Path=/",1700000000);cookie_set(&jar,&c,"none=1; Secure; SameSite=None; Path=/",1700000000);
 bfetch_init();bfetch_set_bypass(1);int id=bfetch_start_embedded("https://origin.test/full.bin","https://outside.test/page","https://outside.test/page",redirect,0);
 OK(settle(id)==BF_DONE);cookies(0,0);OK(bfetch_response_policy_known(id));OK(!strcmp(bfetch_response_header(id,"content-security-policy"),"script-src 'none'\nframe-ancestors https://outside.test"));OK(!strcmp(bfetch_response_header(id,"x-frame-options"),"SAMEORIGIN\nDENY"));bfetch_release(id);
 id=bfetch_start_embedded("https://origin.test/full.bin","https://origin.test/child","https://outside.test/page",redirect,0);
 bfetch_set_document("https://origin.test/new-parent");bfetch_set_base("https://origin.test/style.css");OK(settle(id)==BF_DONE);cookies(0,0);bfetch_release(id);
 id=bfetch_start_embedded("https://origin.test/full.bin","https://origin.test/child","https://origin.test/page",redirect,0);OK(settle(id)==BF_DONE);cookies(1,1);bfetch_release(id);
 id=bfetch_start_embedded("https://origin.test/full.bin","https://origin.test/child","http://origin.test/page",redirect,0);OK(settle(id)==BF_DONE);cookies(0,0);bfetch_release(id);
 policy_mode=1;id=bfetch_start_embedded("https://origin.test/full.bin","https://origin.test/page","https://origin.test/page",redirect,0);OK(settle(id)==BF_DONE);OK(!bfetch_response_policy_known(id));bfetch_release(id);policy_mode=0;
 deny_redirect=1;redirect_calls=0;int before=g_nreq;id=bfetch_start_embedded("https://origin.test/moved.bin","https://origin.test/page","https://origin.test/page",redirect,0);settle(id);OK(redirect_calls==1);OK(g_nreq==before+1);bfetch_release(id);
 bfetch_close_all();cookie_jar_free(&jar);printf("passive-frame-transport: %d checks, %d failures\n",checks,fails);return fails?1:0;}
