/* Reuse the deterministic transport model, but execute only this ordinary
 * half-close state transition. No live peer, timer, or generated fault input. */
#define main tcp_model_unused_main
#include "tcp_test.c"
#undef main
int main(void)
{
    srv_reset();
    enum { LISTEN_PORT=8080 };
    int lid=tcp_listen(LISTEN_PORT,4,1);
    struct peer p={40050,0x50500000u,0,0};
    int id=srv_connect(&p,lid,LISTEN_PORT,NULL);
    if(id<0)return 2;
    tcp_shutdown_write(id);
    /* A normal response filled the receive ring after our request EOF.
     * Draining half must advertise room even though our FIN has been sent. */
    conns[id].rx_len=RXBUF;conns[id].rcv_nxt=conns[id].read_seq+RXBUF;
    conns[id].adv_wnd=0;
    int before=g_ncap;static char drain[RXBUF/2];
    int n=tcp_recv(id,drain,sizeof drain);
    struct cap *reply=cap_to(p.port,before);
    int ok=n==sizeof drain&&reply&&(reply->flags&ACK)&&reply->win>0;
    printf("%s half-close drain advertises receive window\n",ok?"PASS":"FAIL");
    srv_reset();return ok?0:1;
}
