/* Ordinary finite HTTP consumers over the existing in-memory socket peer.
 * The included suite main is never called: only the cases below run. */
#define main unused_h2mux_suite_main
#define hstub_open pending_peer_open
#include "h2mux_test.c"
#undef hstub_open
#undef main

static int peak_live;
int hstub_open(const char *host,int port,int flags)
{
    int fd=pending_peer_open(host,port,flags),live=0;
    for(int i=0;i<NSOCK;i++)live+=g_sk[i].used!=0;
    if(live>peak_live)peak_live=live;
    return fd;
}
static void begin_case(void)
{ reset_world();bfetch_cache_clear();bfetch_http_cache_clear();peak_live=0; }
static int resource(const char *host,const char *path)
{
    char url[160];snprintf(url,sizeof url,"https://%s%s",host,path);
    return bfetch_start_from(url,url);
}
static void pump_resource(int id,int count)
{ for(int i=0;i<count&&bfetch_state(id)==BF_PENDING;i++){bfetch_pump();g_clock+=5;} }
static int resource_ok(int id)
{
    int len=0;const unsigned char *body=bfetch_body(id,&len);
    return id>=0&&bfetch_state(id)==BF_DONE&&bfetch_status(id)==200&&body&&len>0;
}

static void pending_owner(const char *host,int owner_count)
{
    begin_case();g_hold_new_handshake=1;
    struct xfer owner[2];memset(owner,0,sizeof owner);
    for(int i=0;i<owner_count;i++)owner[i].fd=bxfer_open(host,443,1);
    int id=resource(host,"/finite-resource");
    OK(id>=0&&owner[0].fd>=0);
    if(owner_count==2)OK(owner[0].fd==owner[1].fd);
    pump_resource(id,6);
    int hc=0,streams=0;bxfer_stats(NULL,&hc,&streams,NULL);
    OKM(bfetch_state(id)==BF_PENDING&&hc==0&&streams==0&&g_accepts==1,
        "unfinished handshake must not resolve ALPN or send a request");
    g_hold_new_handshake=0;sk(owner[0].fd)->handshake_held=0;

    /* The external owners are still awaiting their next application turn.
     * Only the normal bfetch scheduler runs during this bounded interval. */
    pump_resource(id,32);
    OKM(resource_ok(id),"pending owner must not block resource completion (%s)",host);
    OKM(g_accepts==1,"pending join unexpectedly opened another connection");
    for(int i=0;i<owner_count;i++)
        OK(x_begin(&owner[i],"GET",i?"/finite-owner-b":"/finite-owner-a",host,443,0,0,0)==0);
    for(int k=0;k<100;k++) {
        for(int i=0;i<owner_count;i++)
            if(owner[i].c.state!=H1_C_DONE&&owner[i].c.state!=H1_C_ERROR)bxfer_pump(&owner[i].c);
        bfetch_pump();g_clock+=5;
    }
    for(int i=0;i<owner_count;i++)OK(owner[i].c.state==H1_C_DONE&&owner[i].c.resp.code==200);
    OK(resource_ok(id));
    int is_h2=origin_speaks_h2(host);
    OKM(peak_live<=(is_h2?1:2),"ordinary fallback exceeded live connection budget (%d)",peak_live);
    if(is_h2){struct hsock *s=sk(owner[0].fd);OK(s&&s->h2s.streams_seen==owner_count+1);}
    printf("pending-owner-case %s owners=%d accepts=%d peak_live=%d resource_status=%d\n",
        host,owner_count,g_accepts,peak_live,bfetch_status(id));
    bfetch_release(id);for(int i=0;i<owner_count;i++)x_free(&owner[i]);reset_world();
}

static void h1_busy_borrowers(void)
{
    begin_case();struct xfer owner;memset(&owner,0,sizeof owner);
    owner.fd=bxfer_open("h1.example",443,1);
    int ids[3];for(int i=0;i<3;i++){char p[40];snprintf(p,sizeof p,"/bounded-resource-%d",i);ids[i]=resource("h1.example",p);OK(ids[i]>=0);}
    /* The direct owner starts first; speculative resource handles must return
     * through the original hpool budget instead of direct H1 fallback dials. */
    OK(x_begin(&owner,"GET","/ordinary-owner","h1.example",443,0,0,0)==0);
    for(int k=0;k<100;k++){bfetch_pump();g_clock+=5;}
    for(int i=0;i<3;i++)OK(resource_ok(ids[i]));
    OKM(peak_live<=2,"H1 borrowed handles bypassed the resource pool cap (%d)",peak_live);
    struct xfer *v[]={&owner};x_spin(v,1,30);OK(owner.c.state==H1_C_DONE);
    printf("pending-h1-borrowers accepts=%d peak_live=%d\n",g_accepts,peak_live);
    for(int i=0;i<3;i++)bfetch_release(ids[i]);x_free(&owner);reset_world();
}

static void h1_fresh_dialer_waits_for_budget(void)
{
    begin_case();int id=resource("h1.example","/fresh-resource");OK(id>=0);
    struct xfer first,second;memset(&first,0,sizeof first);memset(&second,0,sizeof second);
    first.fd=bxfer_open("h1.example",443,1);
    OK(x_begin(&first,"GET","/first-owner","h1.example",443,0,0,0)==0);
    OK(x_start(&second,"GET","/second-owner","h1.example",443,0,0,0)==0);
    pump_resource(id,8);
    OKM(bfetch_state(id)==BF_PENDING&&peak_live==2,
        "fresh dialer must queue behind a full H1 budget (%d)",peak_live);
    struct xfer *v[]={&second};x_spin(v,1,30);OK(second.c.state==H1_C_DONE);x_free(&second);
    pump_resource(id,40);OK(resource_ok(id));
    OKM(peak_live<=2,"fresh resource redial exceeded H1 pool cap (%d)",peak_live);
    struct xfer *w[]={&first};x_spin(w,1,30);OK(first.c.state==H1_C_DONE);
    printf("pending-h1-fresh accepts=%d peak_live=%d resource_status=%d\n",g_accepts,peak_live,bfetch_status(id));
    bfetch_release(id);x_free(&first);reset_world();
}

int main(void)
{
    pending_owner("h2.example",2);
    pending_owner("h1.example",1);
    h1_busy_borrowers();
    h1_fresh_dialer_waits_for_budget();
    printf("bfetch-pending-owner: %d checks, %d failures\n",checks,fails);
    return fails?1:0;
}
