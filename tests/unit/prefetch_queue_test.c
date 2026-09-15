/* Real bfetch + HTTP/2 parser over the existing socket peer. Offering more
 * URLs than request handles must not silently turn the tail into serial IO.
 * Server-side HEADERS counts distinguish prefetch from a later cache miss. */
#define main h2mux_existing_main
#include "h2mux_test.c"
#undef main
static int server_requests(void)
{
    int n=0;for(int i=0;i<NSOCK;i++)if(g_sk[i].used&&g_sk[i].h2)n+=g_sk[i].h2s.streams_seen;
    return n;
}
int main(void)
{
    reset_world();
    for(int i=0;i<24;i++) {
        char url[128];snprintf(url,sizeof url,"https://h2.example/prefetch-%d",i);
        bfetch_prefetch(url);bfetch_prefetch(url);
    }
    bfetch_prefetch_wait();
    OKM(server_requests()==24,"all 24 offered resources prefetched before consumption (got %d)",server_requests());
    for(int i=0;i<24;i++) {
        char url[128],prefix[64];unsigned char *body=NULL;int len=0;
        snprintf(url,sizeof url,"https://h2.example/prefetch-%d",i);
        snprintf(prefix,sizeof prefix,"REPLY(/prefetch-%d):",i);
        OKM(!res_fetch(url,&body,&len)&&body&&len==900&&!memcmp(body,prefix,strlen(prefix)),"prefetch body %d retains exact identity",i);
        free(body);
    }
    OKM(server_requests()==24,"consuming cached bodies never repeats requests");
    OKM(g_accepts==1,"queued prefetch retains one h2 connection");
    reset_world();
    for(int i=0;i<24;i++) {
        char url[128];snprintf(url,sizeof url,"https://h2.example/cancel-%d",i);bfetch_prefetch(url);
    }
    bfetch_cache_clear();bfetch_prefetch_wait();
    OKM(server_requests()==0,"clearing the queue cancels deferred and active requests");
    bfetch_prefetch("ftp://invalid.test/unsupported");bfetch_prefetch_wait();
    OKM(server_requests()==0,"unsupported URL never enters a retry loop");
    reset_world();
    /* Request handles held by another owner cannot be released by prefetch.
     * A speculative waiter must yield that ownership instead of deadlocking. */
    int ids[16];for(int i=0;i<16;i++)ids[i]=bfetch_start("https://h2.example/held");
    bfetch_prefetch("https://h2.example/deferred");bfetch_prefetch_wait();
    OKM(1,"prefetch returns while unrelated owners retain all handles");
    for(int i=0;i<16;i++)bfetch_release(ids[i]);
    bfetch_prefetch_wait();
    unsigned char *body=NULL;int len=0;
    OKM(!res_fetch("https://h2.example/deferred",&body,&len)&&len==900,"deferred request resumes after handles are released");free(body);
    reset_world();
    printf("prefetch-queue: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
