/* Actual bfetch/bxfer/pool consumers, with the shared HTTP wire peer below
 * the socket ABI. Unlike reset_world(), census happens before cleanup, so
 * forcibly closing a forgotten fd in the harness cannot manufacture green.
 * This proves user-space ownership only: this peer closes immediately; the
 * separate sock_closed_drain gate exercises real kernel deferred close. */
#define main h2mux_existing_main
#define hstub_close owner_peer_close
#include "h2mux_test.c"
#undef hstub_close
#undef main

int hstub_close(int fd)
{
#ifdef BFETCH_OWNER_DROP_CLOSE
    (void)fd; return 0;
#else
    return owner_peer_close(fd);
#endif
}
static int live_peer(void)
{
    int n=0; for(int i=0;i<NSOCK;i++) n+=g_sk[i].used!=0; return n;
}
static void run_owner_loop(const char *origin)
{
    reset_world(); bfetch_cache_clear(); bfetch_http_cache_clear();
    int completed=0;
    for(int i=0;i<64;i++) {
        char url[160]; snprintf(url,sizeof url,"https://%s/ownership-%d",origin,i);
        int id=bfetch_start_from(url,url);
        OK(id>=0); if(id<0) break;
        for(int k=0;k<100 && bfetch_state(id)==BF_PENDING;k++) {
            bfetch_pump(); g_clock++;
        }
        int n=0; const unsigned char *p=bfetch_body(id,&n);
        OKM(bfetch_state(id)==BF_DONE && bfetch_status(id)==200 && p && n>0,
            "owner-loop response %s iteration=%d state=%d bytes=%d",origin,i,bfetch_state(id),n);
        int done=bfetch_state(id)==BF_DONE && p && n>0;
        bfetch_release(id);
        if(!done) break;
        completed++;
    }
    /* This is production page teardown, not the harness's reset_world(). */
    bfetch_close_all(); bxfer_close_all();
    int hits=0,evicted=0,closed=0; bfetch_pool_stats(&hits,&evicted,&closed);
    printf("owner-loop %s: completed=%d accepted=%d live=%d pool_closed=%d\n",
        origin,completed,g_accepts,live_peer(),closed);
    OKM(completed==64,"owner-loop must complete 64 real responses");
    OKM(live_peer()==0,"owner-loop production close left socket owned");
    reset_world();
}
int main(void)
{
    run_owner_loop("h1.example"); run_owner_loop("h2.example");
    printf("bfetch-owner-loop: %d checks, %d failures\n",checks,fails);
    return fails?1:0;
}
