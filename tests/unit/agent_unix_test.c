/* SPDX-License-Identifier: MIT */
/* Actual unix.c with the existing wait-hook harness. The hook changes the
 * modelled process identity only after Unix drops its sleeping owner. Queue
 * bytes and allocations are production state; this is not a guest test. */
#define LOGIT_WEAK_LOCAL_proc_agent_identity 1
#define LOGIT_WEAK_LOCAL_proc_agent_channel_valid 1
#define main unix_legacy_main
#include "unix_test.c"
#undef main

static struct aex_agent_identity identities[2];
static int current_pid;

int proc_agent_identity(int pid, struct aex_agent_identity *out)
{
    for (unsigned i=0;i<2;i++) if (identities[i].pid==pid && identities[i].abi) {
        *out=identities[i]; return 1;
    }
    return 0;
}
int proc_agent_channel_valid(int pid, uint64_t generation)
{
    struct aex_agent_identity live={0};
    return current_pid==pid && proc_agent_identity(pid,&live) && live.generation==generation;
}
static void identity_reset(void)
{
    for (unsigned i=0;i<2;i++) identities[i]=(struct aex_agent_identity){
        .size=sizeof identities[i], .abi=AEX_AGENT_ABI,
        .pid=201+(int)i, .uid=1000, .gid=1000, .generation=11+i,
        .mode=i?AEX_ACT_WORKER:AEX_ACT_UI
    };
    current_pid=201;
}
static void pair(struct usock **a, struct usock **b)
{
    int err=0;identity_reset();
    CHECK_EQ("identity: fresh pair",unix_pair(LOGIT_SOCK_STREAM,201,a,b,&err),0);
    CHECK_EQ("identity: worker receives endpoint",unix_agent_transfer(*b,201,&identities[1]),0);
    CHECK_EQ("identity: broker sees worker pid",unix_peer_pid(*a),202);
}
static void peers(void)
{
    struct usock *a,*b;pair(&a,&b);
    struct aex_agent_identity peer={0};
    CHECK_EQ("identity: original peer accepted",unix_agent_peer(a,&peer),0);
    CHECK_EQ("identity: captured generation",peer.generation,12);
    identities[1].uid++;
    CHECK_EQ("identity: changed peer uid refused",unix_agent_peer(a,&peer),-1);
    identities[1].uid--;identities[1].gid++;
    CHECK_EQ("identity: changed peer gid refused",unix_agent_peer(a,&peer),-1);
    identities[1].gid--;identities[1].generation++;
    CHECK_EQ("identity: replaced peer generation refused",unix_agent_peer(a,&peer),-1);
    identities[1].generation--;identities[1].abi=0;
    CHECK_EQ("identity: exited peer refused",unix_agent_peer(a,&peer),-1);
    identities[1].abi=AEX_AGENT_ABI;
    current_pid=203;
    CHECK_EQ("identity: inherited endpoint is not original owner",unix_write(a,"x",1,1),LSK_E_PERM);
    current_pid=201;identities[0].uid++;
    CHECK_EQ("identity: changed local uid cannot write",unix_write(a,"x",1,1),LSK_E_PERM);
    identities[0].uid--;identities[0].gid++;
    CHECK_EQ("identity: changed local gid cannot read",unix_read(a,&peer,1,1),LSK_E_PERM);
    identities[0].gid--;
    CHECK_EQ("identity: unchanged owner can write",unix_write(a,"ok",2,1),2);
    current_pid=202;char bytes[2];
    CHECK_EQ("identity: worker reads channel",unix_read(b,bytes,2,1),2);
    CHECK("identity: channel content intact",!memcmp(bytes,"ok",2));
    unix_release(a);unix_release(b);
}

struct wait_change { struct usock *peer; int reading, generation; };
static void change_while_parked(void *arg)
{
    struct wait_change *change=arg;
    ustub_on_park=0;current_pid=202;
    if (change->reading)
        CHECK_EQ("wait identity: peer supplies data",unix_write(change->peer,"q",1,1),1);
    else {
        char drained[64];
        CHECK_EQ("wait identity: peer makes room",unix_read(change->peer,drained,sizeof drained,1),sizeof drained);
    }
    if (change->generation) identities[0].generation++;
    else identities[0].gid++;
    current_pid=201;
}
static void waiting(void)
{
    struct usock *a,*b;pair(&a,&b);
    struct wait_change change={b,1,0};long parks=ustub_parks;
    ustub_on_park=change_while_parked;ustub_park_arg=&change;ustub_where="identity read";
    char got='!';
    CHECK_EQ("wait identity: resumed read refused",unix_read(a,&got,1,0),LSK_E_PERM);
    CHECK_EQ("wait identity: read actually parked",ustub_parks,parks+1);
    CHECK_EQ("wait identity: refused read leaves output untouched",got,'!');
    CHECK_EQ("wait identity: refused read retains queued byte",a->conn->ch[1].count,1);
    identities[0].gid--;
    CHECK_EQ("wait identity: original identity can read retained byte",unix_read(a,&got,1,1),1);
    CHECK_EQ("wait identity: retained content",got,'q');
    unix_release(a);unix_release(b);

    pair(&a,&b);char data[UNIX_BUF+17];memset(data,'p',sizeof data);
    CHECK_EQ("wait identity: fill write queue",unix_write(a,data,UNIX_BUF,1),UNIX_BUF);
    change=(struct wait_change){b,0,0};parks=ustub_parks;
    ustub_on_park=change_while_parked;ustub_park_arg=&change;ustub_where="identity write";
    CHECK_EQ("wait identity: resumed write refused",unix_write(a,"x",1,0),LSK_E_PERM);
    CHECK_EQ("wait identity: write actually parked",ustub_parks,parks+1);
    CHECK_EQ("wait identity: no byte added after credential change",a->conn->ch[0].count,UNIX_BUF-64);
    unix_release(a);unix_release(b);

    pair(&a,&b);change=(struct wait_change){b,0,1};parks=ustub_parks;
    ustub_on_park=change_while_parked;ustub_park_arg=&change;ustub_where="identity partial write";
    CHECK_EQ("wait identity: short write reports committed prefix",unix_write(a,data,sizeof data,0),UNIX_BUF);
    CHECK_EQ("wait identity: partial write actually parked",ustub_parks,parks+1);
    CHECK_EQ("wait identity: no suffix after generation change",a->conn->ch[0].count,UNIX_BUF-64);
    current_pid=202;
    CHECK_EQ("wait identity: peer receives committed prefix",unix_read(b,data,sizeof data,1),UNIX_BUF-64);
    int intact=1;for (unsigned i=0;i<UNIX_BUF-64;i++) intact &= data[i]=='p';
    CHECK("wait identity: prefix content intact",intact);
    unix_release(a);unix_release(b);reset_all();
}
int main(void)
{
    /* Missing identity providers remain ordinary POSIX-style Unix sockets. */
    if (unix_legacy_main()) return 1;
    peers();waiting();
    CHECK_EQ("identity: all channel buffers reclaimed",ustub_live_allocs,0);
    printf("AGENT_UNIX checks=%d failures=%d\n",checks,fails);
    return fails?1:0;
}
