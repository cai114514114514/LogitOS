/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "amd/polaris/sdma/engine.h"

/* Values and ordering from Linux v6.12 sdma_v3_0.c gfx_stop/enable/gfx_resume,
 * oss_3_0_{d,sh_mask}.h, and gmc_v8_0.c. DWORD register indices are converted
 * to byte offsets here. Only our owned direct-VRAM SDMA0 ring is initialized;
 * shader CP state, VM tables, voltage and display state are not modified. */
#define F32 0xd048u
#define RB 0xd200u
#define IB 0xd228u
#define RPTR 0xd20cu
#define WPTR 0xd210u
struct start {
    struct polaris_sdma_engine *engine;
    const struct polaris_sdma_engine_ops *ops;
    uint64_t began,last;
};
static int time_ok(struct start *s)
{
    uint64_t now=s->ops->queue.now_us(s->ops->queue.opaque);
    if(now<s->last||now-s->began>=100000)return POLARIS_SDMA_ENGINE_TIMEOUT;
    s->last=now;return 0;
}
static int rd(struct start *s,uint32_t reg,uint32_t *v)
{
    int rc=time_ok(s);if(rc)return rc;
    if(s->ops->queue.read_reg(s->ops->queue.opaque,reg,v)||*v==UINT32_MAX)
        return POLARIS_SDMA_ENGINE_IO;
    return time_ok(s);
}
static int smc(struct start *s,uint32_t address,uint32_t *v)
{
    int rc=time_ok(s);if(rc)return rc;
    if(s->ops->read_smc_word(s->ops->queue.opaque,address,v)||*v==UINT32_MAX)
        return POLARIS_SDMA_ENGINE_IO;
    return time_ok(s);
}
static int wr(struct start *s,uint32_t reg,uint32_t value)
{
    uint32_t observed;
    int rc=time_ok(s);if(rc)return rc;
    s->engine->register_writes++;
    if(s->ops->write_reg(s->ops->queue.opaque,reg,value))return POLARIS_SDMA_ENGINE_IO;
    rc=rd(s,reg,&observed);
    if(rc)return rc;
    return observed==value?0:POLARIS_SDMA_ENGINE_IO;
}
static int map_valid(const struct polaris_sdma_mapping *m)
{
    return m&&m->cpu&&m->range.bytes&&
        !(((uintptr_t)m->cpu|m->range.gpu_base|m->range.bytes)&3)&&
        m->range.gpu_base<POLARIS_SDMA_GPU_LIMIT&&
        m->range.bytes<=POLARIS_SDMA_GPU_LIMIT-m->range.gpu_base&&
        m->range.bytes<=UINTPTR_MAX-(uintptr_t)m->cpu;
}
static int intersects(uint64_t a,uint64_t an,uint64_t b,uint64_t bn)
{return a<b+bn&&b<a+an;}
static int idle(struct start *s,int wait)
{
    do {
        uint32_t status,requests;
        int rc=rd(s,0xd034u,&status);if(rc)return rc;
        rc=rd(s,0xe4cu,&requests);if(rc)return rc;
        s->engine->polls++;
        if((status&7u)==7u&&!(requests&0x21u))return 0;
        if(!wait)return POLARIS_SDMA_ENGINE_BUSY;
    } while(s->engine->polls<100000);
    return POLARIS_SDMA_ENGINE_TIMEOUT;
}
int polaris_sdma_engine_start(struct polaris_sdma_engine *e,
                              const struct polaris_sdma_engine_ops *ops,
                              const struct polaris_smu_loader *loader,
                              const struct polaris_sdma_mapping *ring,
                              const struct polaris_sdma_mapping *fence,
                              struct polaris_sdma_queue *queue)
{
    int rc,queue_locked=0;
    uint32_t id,soft,flags,load,location,memsize,select,rptr,wptr,rb,ib,f32,rlc,cntl,chicken,clock;
    uint64_t base,end;
    unsigned order=0,writes_before;
    volatile uint32_t *mapped=0;
    struct start s={.engine=e,.ops=ops};
    if(!e||!queue)return POLARIS_SDMA_ENGINE_INVALID;
    if(__atomic_exchange_n(&e->lock,1u,__ATOMIC_ACQUIRE))return POLARIS_SDMA_ENGINE_BUSY;
    writes_before=e->register_writes;
    if(e->quarantined){rc=POLARIS_SDMA_ENGINE_QUARANTINED;goto done;}
    if(e->configured){rc=POLARIS_SDMA_ENGINE_BUSY;goto done;}
    if(__atomic_exchange_n(&queue->lock,1u,__ATOMIC_ACQUIRE)){rc=POLARIS_SDMA_ENGINE_BUSY;goto done;}
    queue_locked=1;
    if(queue->quarantined){rc=POLARIS_SDMA_ENGINE_QUARANTINED;goto done;}
    if(queue->attached||queue->sequence||queue->submitted){rc=POLARIS_SDMA_ENGINE_BUSY;goto done;}
    if(!ops||!ops->write_reg||!ops->read_smc_word||!ops->queue.read_identity||
       !ops->queue.read_reg||!ops->queue.write_wptr||!ops->queue.resolve_mapping||
       !ops->queue.sync||!ops->queue.now_us||!loader||!loader->loaded||loader->quarantined||
       loader->soft_regs<0x20000||(loader->soft_regs&3)||loader->soft_regs>0x3ff90||
       (loader->load_status&POLARIS_SMU_TOC_REQUIRED_MASK)!=POLARIS_SMU_TOC_REQUIRED_MASK){
        rc=POLARIS_SDMA_ENGINE_PREREQUISITE;goto done;
    }
    if(!map_valid(ring)||!map_valid(fence)||ring->range.bytes<256||ring->range.bytes>65536||
       (ring->range.bytes&(ring->range.bytes-1))||(ring->range.gpu_base&255)||
       fence->range.bytes!=4||intersects(ring->range.gpu_base,ring->range.bytes,
           fence->range.gpu_base,fence->range.bytes)||
       intersects((uintptr_t)ring->cpu,ring->range.bytes,(uintptr_t)fence->cpu,fence->range.bytes)){
        rc=POLARIS_SDMA_ENGINE_INVALID;goto done;
    }
    if(ops->queue.read_identity(ops->queue.opaque,&id)){rc=POLARIS_SDMA_ENGINE_IO;goto done;}
    if(id!=0x67df1002u){rc=POLARIS_SDMA_ENGINE_PREREQUISITE;goto done;}
    s.began=s.last=ops->queue.now_us(ops->queue.opaque);
    if((rc=smc(&s,0x20030,&soft))||(rc=smc(&s,0x3f000,&flags))||
       (rc=smc(&s,loader->soft_regs+0x6c,&load)))goto done;
    e->load_status=load;
#ifndef POLARIS_ENGINE_NEGCTL_IGNORE_LOAD_STATUS
    if(soft!=loader->soft_regs||!(flags&1u)||
       (load&POLARIS_SMU_TOC_REQUIRED_MASK)!=POLARIS_SMU_TOC_REQUIRED_MASK){
        rc=POLARIS_SDMA_ENGINE_PREREQUISITE;goto done;
    }
#endif
    if((rc=rd(&s,0xe44,&select))||(rc=rd(&s,0x2024,&location))||
       (rc=rd(&s,0x5428,&memsize)))goto done;
    base=(uint64_t)(location&0xffffu)<<24;
    end=((uint64_t)(location>>16)+1)<<24;
    if(select||end<=base||memsize<128||memsize>16384||
       ((uint64_t)memsize<<20)>end-base){rc=POLARIS_SDMA_ENGINE_PREREQUISITE;goto done;}
    end=base+((uint64_t)memsize<<20);
    if(
       ring->range.gpu_base<base||ring->range.gpu_base>end||
       ring->range.bytes>end-ring->range.gpu_base||
       fence->range.gpu_base<base||fence->range.gpu_base>end||
       fence->range.bytes>end-fence->range.gpu_base){rc=POLARIS_SDMA_ENGINE_PREREQUISITE;goto done;}
    if(ops->queue.resolve_mapping(ops->queue.opaque,ring->range.gpu_base,ring->range.bytes,&mapped)||mapped!=ring->cpu||
       ops->queue.resolve_mapping(ops->queue.opaque,fence->range.gpu_base,fence->range.bytes,&mapped)||mapped!=fence->cpu){
        rc=POLARIS_SDMA_ENGINE_PREREQUISITE;goto done;
    }
    /* No takeover of existing work. Halting another owner's queue can strand
     * writes forever even if a later pointer reset superficially looks idle. */
    if((rc=rd(&s,RPTR,&rptr))||(rc=rd(&s,WPTR,&wptr)))goto done;
    if(rptr!=wptr){rc=POLARIS_SDMA_ENGINE_BUSY;goto done;}
    const uint32_t rlc_regs[]={0xd400,0xd428,0xd600,0xd628};
    for(unsigned i=0;i<4;i++){
        if((rc=rd(&s,rlc_regs[i],&rlc)))goto done;
        if(rlc&1){rc=POLARIS_SDMA_ENGINE_BUSY;goto done;}
    }
    if((rc=idle(&s,0))||(rc=rd(&s,RB,&rb))||(rc=rd(&s,IB,&ib))||
       (rc=rd(&s,F32,&f32))||(rc=rd(&s,0xd010,&cntl)))goto done;
    if((rc=wr(&s,RB,rb&~1u))||(rc=wr(&s,IB,ib&~1u))||
       (rc=wr(&s,0xd010,cntl&~0x40018u))||
       (rc=wr(&s,F32,f32|1u))||(rc=idle(&s,1)))goto done;
    /* Linux disables automatic context switching before ring reconfiguration.
     * Keep it disabled for our single queue and explicitly clear DATA_SWAP /
     * FENCE_SWAP: clearing RB_SWAP alone does not establish little endian DMA.
     * Polaris10's AMD golden register sequence additionally sets flow-control
     * stalls (CHICKEN_BITS) and its clock policy. These are masked RMWs from
     * sdma_v3_0.c golden_settings_polaris10_a11, not guesses about reset values.
     * RLC/IB golden entries are omitted because those queues remain disabled. */
    if((rc=rd(&s,0xd014,&chicken))||(rc=rd(&s,0xd00c,&clock))||
       (rc=wr(&s,0xd014,(chicken&~0xfc910007u)|0x00810007u))||
       (rc=wr(&s,0xd00c,clock&~0xff000fffu)))goto done;
    for(uint64_t i=0;i<ring->range.bytes/4;i++)ring->cpu[i]=0;
    fence->cpu[0]=0;
    if(ops->queue.sync(ops->queue.opaque,POLARIS_SDMA_TO_DEVICE,ring)||
       ops->queue.sync(ops->queue.opaque,POLARIS_SDMA_TO_DEVICE,fence)||
       ops->queue.sync(ops->queue.opaque,POLARIS_SDMA_TO_CPU,ring)||
       ops->queue.sync(ops->queue.opaque,POLARIS_SDMA_TO_CPU,fence)){
        rc=POLARIS_SDMA_ENGINE_IO;goto done;
    }
    for(uint64_t i=0;i<ring->range.bytes/4;i++)if(ring->cpu[i]){rc=POLARIS_SDMA_ENGINE_IO;goto done;}
    if(fence->cpu[0]){rc=POLARIS_SDMA_ENGINE_IO;goto done;}
    while((UINT64_C(1)<<order)<ring->range.bytes/4)order++;
    rb=(rb&~0x0f00323fu)|(order<<1);
    const uint32_t addresses[]={0xd29c,0xd2a0,RB,RPTR,WPTR,0xd22c,0xd230,
                                0xd220,0xd224,0xd204,0xd208,0xd248,0xd214};
    const uint32_t values[]={0,0,rb,0,0,0,0,0,0,(uint32_t)(ring->range.gpu_base>>8),0,0,0};
    for(unsigned i=0;i<sizeof(addresses)/sizeof(addresses[0]);i++)
        if((rc=wr(&s,addresses[i],values[i])))goto done;
    if((rc=wr(&s,RB,rb|1u))||(rc=wr(&s,F32,f32&~1u))||
       (rc=time_ok(&s)))goto done;
    __atomic_store_n(&queue->lock,0u,__ATOMIC_RELEASE);queue_locked=0;
    if(polaris_sdma_queue_attach(queue,&ops->queue,ring,fence)){
        rc=POLARIS_SDMA_ENGINE_IO;goto done;
    }
    e->configured=1;rc=POLARIS_SDMA_ENGINE_OK;
done:
    if(rc&&e->register_writes!=writes_before){e->quarantined=1;queue->quarantined=1;}
    if(queue_locked)__atomic_store_n(&queue->lock,0u,__ATOMIC_RELEASE);
    __atomic_store_n(&e->lock,0u,__ATOMIC_RELEASE);
    return rc;
}
