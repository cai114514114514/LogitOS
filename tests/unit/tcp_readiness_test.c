/* Only readiness and descriptor ownership states are executed. The imported
 * harness supplies deterministic TCP setup and captures ordinary output. */
struct poll_table;struct waitq;
#define LOGIT_WEAK_LOCAL_poll_wait 1
void poll_wait(struct poll_table *,struct waitq *);
#define main tcp_model_unused_main
#include "tcp_test.c"
#undef main
static struct waitq *registered;
void poll_wait(struct poll_table *pt,struct waitq *q){if(pt)registered=q;}
int main(void)
{
    enum { PORT=8080 };
    srv_reset();
    int lid=tcp_listen(PORT,4,1);
    struct poll_table *pt=(struct poll_table *)&registered;
    short mask=tcp_file_poll(lid,1,pt);
    CHECK(mask==0&&registered==&listeners[lid].wq,"TCP poll listener parks on its accept queue");
    struct peer p={40050,0x50500000u,0,0};int id=srv_connect(&p,lid,PORT,NULL);
    if(id<0)return 2;
    registered=0;mask=tcp_file_poll(id,0,pt);
    CHECK(mask==LPOLLOUT&&registered==&rx_wq,"TCP poll idle connection registers the transport queue");
    conns[id].rx_len=1;
    CHECK(tcp_file_poll(id,0,0)&LPOLLIN,"TCP poll received data is readable");
    conns[id].rx_len=0;conns[id].snd_end=conns[id].snd_una+SNDBUF;
    CHECK(!(tcp_file_poll(id,0,0)&LPOLLOUT),"TCP poll full send ring is not writable");
    conns[id].state=CLOSE_WAIT;conns[id].peer_fin=1;
    CHECK(tcp_file_poll(id,0,0)&LPOLLIN,"TCP poll peer EOF remains readable");
    conns[id].state=CLOSED;conns[id].app_owned=1;
    char b;
    CHECK(tcp_recv(id,&b,1)<0&&conns[id].used,"TCP EOF retains descriptor ownership until close");
    CHECK(tcp_file_poll(id,0,0)&LPOLLHUP,"TCP poll closed transport reports hangup");
    tcp_close(id);CHECK(!conns[id].used,"TCP final descriptor close releases the slot");
    printf("TCP readiness: %d passed, %d failed\n",passed,failed);
    srv_reset();return failed?1:0;
}
