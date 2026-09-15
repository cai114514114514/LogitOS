/* Reuse the real HTTP parser/vtable fixture in its own TU: worker_test.c owns
 * the page/runtime helpers and allocators, while this file owns only sockets.
 * bfetch's in-memory loader is used for OUR startup scripts, never fetch data. */
#define kmalloc wft_unused_kmalloc
#define kfree wft_unused_kfree
#define img_register wft_unused_img_register
#include "stream_net.h"
#include "worker_fetch_net.h"

struct wft_record { char host[128],target[256],method[16],headers[REQ_MAX]; };
static struct wft_record records[128];
static int record_count;

static void wft_route(struct fakesock *s,const char *method,const char *target)
{
    if(record_count<128){
        struct wft_record *r=&records[record_count++];
        snprintf(r->host,sizeof r->host,"%s",s->host);
        snprintf(r->target,sizeof r->target,"%s",target);
        snprintf(r->method,sizeof r->method,"%s",method);
        snprintf(r->headers,sizeof r->headers,"%s",s->req);
    }
    char head[2048],origin[160]="null";
    const char *p=strstr(s->req,"\r\nOrigin: ");
    if(p){p+=10;const char *end=strstr(p,"\r\n");
        if(end&&end-p>0&&end-p<(int)sizeof origin){memcpy(origin,p,(size_t)(end-p));origin[end-p]=0;}}
    const char *body="ok";
    if(!strcmp(target,"/workers/text"))body="worker-text";
    else if(!strcmp(target,"/page/text"))body="page-text";
    else if(!strcmp(target,"/bytes"))body="ABCD";
    int hold=!strncmp(target,"/hold/",6);
    int deny=!strcmp(target,"/cors-denied");
    int redirect=!strcmp(target,"/redirect");
    int options=!strcmp(method,"OPTIONS");
    int n=snprintf(head,sizeof head,"HTTP/1.1 %s\r\nContent-Type: text/plain\r\n",
                   redirect?"302 Found":options?"204 No Content":"200 OK");
    if(!deny)n+=snprintf(head+n,sizeof head-(size_t)n,"Access-Control-Allow-Origin: %s\r\nAccess-Control-Allow-Credentials: true\r\n",origin);
    if(options)n+=snprintf(head+n,sizeof head-(size_t)n,"Access-Control-Allow-Methods: PUT\r\nAccess-Control-Allow-Headers: x-local\r\n");
    if(redirect)n+=snprintf(head+n,sizeof head-(size_t)n,"Location: https://peer.test/final\r\n");
    if(!strcmp(target,"/set"))n+=snprintf(head+n,sizeof head-(size_t)n,"Set-Cookie: remote_cookie=fixture; Secure; SameSite=None; Path=/\r\n");
    snprintf(head+n,sizeof head-(size_t)n,"Content-Length: %d\r\n\r\n",redirect||options?0:hold?4:(int)strlen(body));
    rsp_add(s,head);
    if(hold){s->avail=s->rsp_len;s->finished=0;}
    else if(!redirect&&!options)rsp_add(s,body);
}
void wft_net_reset(void)
{fs_reset();record_count=0;fs_set_router(wft_route);g_next_slice=4096;js_webapi_set_net(&FAKE);}
int wft_net_live(void)
{int n=0;for(int i=0;i<FS_MAX;i++)n+=!!fs[i].used;return n;}
int wft_net_requests(const char *host,const char *target,const char *method)
{int n=0;for(int i=0;i<record_count;i++){struct wft_record*r=&records[i];if(!strcmp(r->host,host)&&!strcmp(r->target,target)&&(!method||!strcmp(r->method,method)))n++;}return n;}
int wft_net_header(const char *host,const char *target,const char *header)
{for(int i=record_count-1;i>=0;i--){struct wft_record*r=&records[i];if(!strcmp(r->host,host)&&!strcmp(r->target,target))return strstr(r->headers,header)!=NULL;}return 0;}
void wft_net_release(const char *target,const char *body)
{for(int i=0;i<FS_MAX;i++)if(fs[i].used&&strstr(fs[i].req,target)){fs_push(i,body);fs_finish(i);}}
void wft_net_finish_all(void)
{for(int i=0;i<FS_MAX;i++)if(fs[i].used)fs_finish(i);}
