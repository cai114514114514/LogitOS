/* In-process HTTP transport. Observe actual first send, not socket opening. */
#define kmalloc pfo_unused_kmalloc
#define kfree pfo_unused_kfree
#define img_register pfo_unused_img_register
#include "stream_net.h"
extern void pfo_sent(void);
extern unsigned long long pfo_now(void);
static int first_sends;
static int pfo_open(const char *h,int p,int tls)
{int fd=f_open(h,p,tls);if(fd>=0)fs[fd].ready_after=0;return fd;}
static int pfo_send(int fd,const void *p,int n)
{int first=fd>=0&&fd<FS_MAX&&fs[fd].used&&fs[fd].req_len==0;int rc=f_send(fd,p,n);if(first&&rc>0){first_sends++;pfo_sent();}return rc;}
static void pfo_route(struct fakesock *s,const char *m,const char *p)
{(void)m;(void)p;rsp_add(s,"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 11\r\n\r\nmetadata-ok");}
static const struct webapi_net PFO_NET={pfo_open,f_poll,pfo_send,f_recv,f_close,pfo_now,f_unix};
void pfo_net_reset(void){fs_reset();first_sends=0;g_next_slice=4096;fs_set_router(pfo_route);js_webapi_set_net(&PFO_NET);}
int pfo_net_sends(void){return first_sends;}
int pfo_net_requests(void){return all_req_n;}
int pfo_net_live(void){int n=0;for(int i=0;i<FS_MAX;i++)n+=!!fs[i].used;return n;}
