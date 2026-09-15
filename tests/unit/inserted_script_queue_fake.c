/* Real loader, deterministic delayed transport. All bytes use the existing
 * route fixture; pending responses count pump turns, not host wall time. */
#define bfetch_start queue_base_start
#define bfetch_state queue_base_state
#define bfetch_pump queue_base_pump
#define bfetch_take queue_base_take
#define bfetch_release queue_base_release
#include "loader_fakebfetch.c"
#undef bfetch_start
#undef bfetch_state
#undef bfetch_pump
#undef bfetch_take
#undef bfetch_release
static int delay[NREQ];
int queue_peak,queue_live,queue_pumps,queue_taken,queue_busy;
int bfetch_start(const char *ref)
{
    if(strstr(ref,"chunk.js")){
        /* Real bfetch can be full because images own its other handles. */
        if(queue_live>=3){queue_busy++;return -1;}
        int id=queue_base_start(ref);
        if(id>=0){delay[id]=12;queue_live++;if(queue_live>queue_peak)queue_peak=queue_live;}
        return id;
    }
    return queue_base_start(ref);
}
int bfetch_state(int id)
{return id>=0 && id<NREQ && delay[id]?BF_PENDING:queue_base_state(id);}
int bfetch_pump(void)
{
    queue_pumps++;
    for(int i=0;i<NREQ;i++)if(delay[i])delay[i]--;
    return 0;
}
int bfetch_take(int id,unsigned char **out)
{
    if(id>=0 && strstr(bfetch_url(id),"chunk.js")){queue_live--;queue_taken++;delay[id]=0;}
    return queue_base_take(id,out);
}
void bfetch_release(int id)
{
    if(id>=0 && strstr(bfetch_url(id),"chunk.js")){queue_live--;delay[id]=0;}
    queue_base_release(id);
}
