/* Real HTTP parser + browser_rt take API. The loader's older fake always
 * allocated a buffer for empty bodies, so its green empty-script check missed
 * the production NULL-body representation. This fixture sends real headers. */
#define main h2mux_existing_main
#define hstub_send h2mux_original_send
#include "h2mux_test.c"
#undef hstub_send
#undef main
static int reply_bytes,reply_code;
int hstub_send(int fd,const void *buf,int len)
{
    struct hsock *s=sk(fd);if(!s)return -1;
    pb_put(&s->c2s,buf,len);
    const unsigned char *p=s->c2s.b+s->c2s.off;int n=pb_avail(&s->c2s),end=-1;
    for(int i=0;i+3<n;i++)if(!memcmp(p+i,"\r\n\r\n",4)){end=i+4;break;}
    if(end<0)return len;
    s->c2s.off+=end;
    char head[200];int hl=snprintf(head,sizeof head,
        "HTTP/1.1 %d Result\r\nContent-Length: %d\r\nCache-Control: no-store\r\n\r\n",reply_code,reply_bytes);
    pb_put(&s->s2c,head,hl);if(reply_bytes)pb_put(&s->s2c,"x",1);
    return len;
}
static void take_case(int code,int bytes)
{
    reset_world();bfetch_http_cache_clear();bfetch_cache_clear();reply_code=code;reply_bytes=bytes;
    int id=bfetch_start("https://h1.example/resource");OK(id>=0);if(id<0)return;
    for(int i=0;i<100&&bfetch_state(id)==BF_PENDING;i++)bfetch_pump();
    OKM(bfetch_state(id)==BF_DONE,"response state=%d error=%s",bfetch_state(id),bfetch_error(id));OK(bfetch_status(id)==code);
    unsigned char *out=0;int n=bfetch_take(id,&out);
    OKM(n==bytes,"empty-resource completed response take: status=%d got=%d expected=%d",code,n,bytes);
    OK(out!=0);if(out){OK(out[bytes]==0);if(bytes)OK(out[0]=='x');}free(out);
}
int main(void)
{
    take_case(200,0);take_case(204,0);take_case(200,1);
    unsigned char *out=0;OK(bfetch_take(-1,&out)==-1);
    reset_world();printf("empty-resource: %d checks, %d failures\n",checks,fails);return !!fails;
}
