/* SPDX-License-Identifier: MIT */
#include "amd/polaris/sdma/engine.h"
#include <stdio.h>
#include <string.h>
static unsigned checks,failures;
#define C(x) do {checks++;if(!(x)){failures++;printf("FAIL line %d: %s\n",__LINE__,#x);}}while(0)
#include "model.h"

struct engine_model {
    struct fixture gpu;
    uint32_t status,requests,location,memsize,ib,rlc,soft,load,flags,chicken,clock_control,extra[6];
    uint32_t writes,fail_at,mismatch_at,smc_reads;
    int idle_after_halt_bad;
};
static const uint32_t write_addresses[]={0xd200,0xd228,0xd010,0xd048,0xd014,0xd00c,
    0xd29c,0xd2a0,0xd200,0xd20c,0xd210,0xd22c,0xd230,
    0xd220,0xd224,0xd204,0xd208,0xd248,0xd214,0xd200,0xd048};
static const uint32_t write_values[]={12,0x100,0,1,0x00814007,0x10000,0,0,12,0,0,0,0,0,0,0x1000,0,0,0,13,0};
static int engine_read(void *p,uint32_t address,uint32_t *v)
{
    struct engine_model *m=p;
    switch(address){
    case 0xd034:*v=m->idle_after_halt_bad&&m->writes>=4?0:m->status;return 0;
    case 0xd014:*v=m->chicken;return 0;
    case 0xd00c:*v=m->clock_control;return 0;
    case 0xe4c:*v=m->requests;return 0;
    case 0x2024:*v=m->location;return 0;
    case 0x5428:*v=m->memsize;return 0;
    case 0xd228:*v=m->ib;return 0;
    case 0xd400:case 0xd428:case 0xd600:case 0xd628:*v=m->rlc;return 0;
    case 0xd22c:*v=m->extra[0];return 0;
    case 0xd230:*v=m->extra[1];return 0;
    case 0xd220:*v=m->extra[2];return 0;
    case 0xd224:*v=m->extra[3];return 0;
    default:return read_reg(p,address,v);
    }
}
static int engine_write(void *p,uint32_t address,uint32_t value)
{
    struct engine_model *m=p;
    unsigned n=m->writes++;
    if(n>=21||address!=write_addresses[n]||value!=write_values[n]){
        m->gpu.errors++;return -1;
    }
    /* At first address setup, CPU and GPU ring/fence must already be zero.
     * This independently checks synchronization before engine enable. */
    if(n==6){
        for(unsigned i=0;i<64;i++)if(m->gpu.memory[0].gpu[i]||m->gpu.memory[0].cpu[i])m->gpu.errors++;
        if(m->gpu.memory[1].cpu[0]||m->gpu.memory[1].gpu[0])m->gpu.errors++;
    }
    if(m->fail_at==n+1)return -1;
    if(m->mismatch_at==n+1)return 0;
    switch(address){
    case 0xd200:m->gpu.control=value;break;
    case 0xd228:m->ib=value;break;
    case 0xd010:m->gpu.cntl=value;break;
    case 0xd014:m->chicken=value;break;
    case 0xd00c:m->clock_control=value;break;
    case 0xd048:m->gpu.f32=value;break;
    case 0xd29c:m->gpu.va=value;break;
    case 0xd2a0:m->gpu.ape=value;break;
    case 0xd20c:m->gpu.rptr=value;break;
    case 0xd210:m->gpu.wptr=value;break;
    case 0xd22c:m->extra[0]=value;break;
    case 0xd230:m->extra[1]=value;break;
    case 0xd220:m->extra[2]=value;break;
    case 0xd224:m->extra[3]=value;break;
    case 0xd204:m->gpu.base=value;break;
    case 0xd208:m->gpu.high=value;break;
    case 0xd248:m->gpu.doorbell=value;break;
    case 0xd214:m->gpu.poll=value;break;
    default:m->gpu.errors++;return -1;
    }return 0;
}
static int smc_word(void *p,uint32_t address,uint32_t *value)
{
    struct engine_model *m=p;m->smc_reads++;
    if(address==0x20030)*value=m->soft;
    else if(address==0x3f000)*value=m->flags;
    else if(address==0x3006c)*value=m->load;
    else {m->gpu.errors++;return -1;}
    return 0;
}
static void engine_init(struct engine_model *m)
{
    memset(m,0,sizeof(*m));init(&m->gpu);m->status=7;m->location=0x00ff0000;
    m->memsize=4096;m->soft=0x30000;m->load=0x47e;m->flags=1;m->ib=0x100;
    m->gpu.cntl=0x40018;m->chicken=0x4000;m->clock_control=0x10000;
}
static int start_engine(struct engine_model *m,struct polaris_sdma_engine *engine,
                        struct polaris_sdma_queue *queue)
{
    struct polaris_smu_loader loader={.loaded=1,.soft_regs=0x30000,.load_status=0x47e};
    struct polaris_sdma_mapping ring=mapping(&m->gpu,0),fence=mapping(&m->gpu,1);
    struct polaris_sdma_engine_ops platform={ops(&m->gpu),engine_write,smc_word};
    platform.queue.read_reg=engine_read;
    return polaris_sdma_engine_start(engine,&platform,&loader,&ring,&fence,queue);
}
static void success(void)
{
    struct engine_model m;engine_init(&m);
    struct polaris_sdma_engine e={0};struct polaris_sdma_queue q={0};
    C(start_engine(&m,&e,&q)==0);
    C(e.configured&&q.attached&&!e.quarantined&&!q.quarantined);
    C(m.writes==21&&e.register_writes==21&&!m.gpu.errors&&m.smc_reads==3);
    struct polaris_sdma_mapping src=mapping(&m.gpu,2),dst=mapping(&m.gpu,3);
    C(polaris_sdma_queue_fill(&q,&dst,0,0x88776655,256)==0);
    C(polaris_sdma_queue_copy(&q,&src,0,&dst,0,256)==0);
    C(q.completed==2&&!m.gpu.errors);
    for(unsigned i=0;i<64;i++)C(dst.cpu[i]==src.cpu[i]);
    unsigned n=m.writes;
    C(start_engine(&m,&e,&q)==POLARIS_SDMA_ENGINE_BUSY);
    C(!e.quarantined&&!q.quarantined&&m.writes==n);
}
static void prerequisite_failures(void)
{
    for(unsigned i=0;i<8;i++){
        struct engine_model m;engine_init(&m);
        struct polaris_sdma_engine e={0};struct polaris_sdma_queue q={0};
        switch(i){
        case 0:m.gpu.id=0x51591002;break;
        case 1:m.gpu.rptr=4;break;
        case 2:m.rlc=1;break;
        case 3:m.status=0;break;
        case 4:m.requests=0x20;break;
        case 5:m.location=0;break;
        case 6:m.gpu.unmapped=1;break;
        case 7:m.memsize=0;break;
        }
        int rc=start_engine(&m,&e,&q);
        C(rc==(i>=1&&i<=4?POLARIS_SDMA_ENGINE_BUSY:POLARIS_SDMA_ENGINE_PREREQUISITE));
        C(!m.writes&&!e.quarantined&&!q.quarantined&&!q.attached);
    }
    /* The mutation removes only the fresh load proof, exactly the error a
     * cached loader.loaded Boolean would permit. Other claims are not counted
     * twice, so its negative control must show exactly one failure. */
    struct engine_model m;engine_init(&m);m.load=0x47c;
    struct polaris_sdma_engine e={0};struct polaris_sdma_queue q={0};
    C(start_engine(&m,&e,&q)==POLARIS_SDMA_ENGINE_PREREQUISITE);
}
static void failure_after_write(void)
{
    for(unsigned i=1;i<=21;i++){
        struct engine_model m;engine_init(&m);m.fail_at=i;
        struct polaris_sdma_engine e={0};struct polaris_sdma_queue q={0};
        C(start_engine(&m,&e,&q)==POLARIS_SDMA_ENGINE_IO);
        C(e.quarantined&&q.quarantined&&!q.attached&&m.writes==i);
        unsigned n=m.writes,reads=m.gpu.reads;
        C(start_engine(&m,&e,&q)==POLARIS_SDMA_ENGINE_QUARANTINED);
        C(m.writes==n&&m.gpu.reads==reads);
    }
    struct engine_model m;engine_init(&m);m.mismatch_at=4;
    struct polaris_sdma_engine e={0};struct polaris_sdma_queue q={0};
    C(start_engine(&m,&e,&q)==POLARIS_SDMA_ENGINE_IO);
    C(e.quarantined&&q.quarantined&&m.writes==4);
    engine_init(&m);m.idle_after_halt_bad=1;m.gpu.step=1000;
    e=(struct polaris_sdma_engine){0};q=(struct polaris_sdma_queue){0};
    C(start_engine(&m,&e,&q)==POLARIS_SDMA_ENGINE_TIMEOUT);
    C(e.quarantined&&q.quarantined&&m.writes==4);
}
int main(void)
{
    (void)attach;(void)calls;
    success();prerequisite_failures();failure_after_write();
    printf("POLARIS_SDMA_ENGINE: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
