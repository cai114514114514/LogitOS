/* Shared test-only SDMA executor. Never linked into the kernel. */
struct bank { uint64_t address; uint32_t bytes, cpu[8192], gpu[8192]; };
struct fixture {
    struct bank memory[4];
    uint32_t id, control, cntl, f32, base, high, rptr, wptr, poll, doorbell, va, ape;
    unsigned reads, ids, resolves, syncs, submissions, packets, errors;
    int unmapped, hang, corrupt, fail_write, fail_sync, bad_reg;
    uint64_t clock, step;
    struct polaris_sdma_queue *reenter;
};
static struct bank *locate(struct fixture *f, uint64_t address, uint64_t bytes)
{
    for (unsigned i=0;i<4;i++) {
        struct bank *b=&f->memory[i];
        if (address>=b->address && address-b->address<=b->bytes &&
            bytes<=b->bytes-(address-b->address)) return b;
    }
    return 0;
}
static uint32_t *gpu(struct fixture *f,uint64_t address,uint32_t bytes)
{
    struct bank *b=locate(f,address,bytes);
    if (!b || (address&3) || (bytes&3)) { f->errors++; return 0; }
    return b->gpu+(address-b->address)/4;
}
static uint32_t word(struct fixture *f,unsigned offset)
{ return f->memory[0].gpu[offset&(f->memory[0].bytes/4-1)]; }
/* Independent literal SDMA v3 decoder, executing GPU-side memory only. CPU
 * stores do not become visible until TO_DEVICE; GPU writes do not reach CPU
 * until TO_CPU. A no-op barrier cannot pass this apparatus. */
static void execute(struct fixture *f)
{
    unsigned at=f->rptr/4,target=f->wptr/4,n=0;
    while (at!=target && n++<f->memory[0].bytes/4) {
        uint32_t op=word(f,at), bytes;
        uint64_t source,destination;
        uint32_t *s,*d;
        if (op==0) { at=(at+1)&(f->memory[0].bytes/4-1); continue; }
        f->packets++;
        if (op==1) {
            bytes=word(f,at+1);
            if (word(f,at+2)!=0) { f->errors++; return; }
            source=word(f,at+3)|((uint64_t)word(f,at+4)<<32);
            destination=word(f,at+5)|((uint64_t)word(f,at+6)<<32);
            s=gpu(f,source,bytes);d=gpu(f,destination,bytes);
            if (!s||!d) return;
            for (unsigned i=0;i<bytes/4;i++) d[i]=s[i];
            at=(at+7)&(f->memory[0].bytes/4-1);
        } else if (op==11) {
            destination=word(f,at+1)|((uint64_t)word(f,at+2)<<32);
            bytes=word(f,at+4); d=gpu(f,destination,bytes);
            if (!d) return;
            for (unsigned i=0;i<bytes/4;i++) d[i]=word(f,at+3);
            at=(at+5)&(f->memory[0].bytes/4-1);
        } else if (op==5) {
            destination=word(f,at+1)|((uint64_t)word(f,at+2)<<32);
            d=gpu(f,destination,4); if (!d) return;
            *d=word(f,at+3); at=(at+4)&(f->memory[0].bytes/4-1);
        } else { f->errors++; return; }
    }
    if (at!=target) f->errors++;
    f->rptr=f->wptr;
    if (f->corrupt) f->memory[3].gpu[3]^=1;
}
static int identity(void *p,uint32_t *id)
{ struct fixture *f=p;f->ids++;*id=f->id;return 0; }
static int read_reg(void *p,uint32_t address,uint32_t *value)
{
    struct fixture *f=p;f->reads++;
    if (f->reenter) {
        struct polaris_sdma_queue *q=f->reenter; f->reenter=0;
        C(polaris_sdma_queue_fill(q,&q->fence,0,1,4)==POLARIS_SDMA_QUEUE_BUSY);
    }
    if (f->bad_reg) {*value=UINT32_MAX;return 0;}
    switch(address) {
    case 0xe44:*value=0;break;
    case 0xd010:*value=f->cntl;break;
    case 0xd048:*value=f->f32;break;
    case 0xd200:*value=f->control;break;
    case 0xd204:*value=f->base;break;
    case 0xd208:*value=f->high;break;
    case 0xd20c:*value=f->rptr;break;
    case 0xd210:*value=f->wptr;break;
    case 0xd214:*value=f->poll;break;
    case 0xd248:*value=f->doorbell;break;
    case 0xd29c:*value=f->va;break;
    case 0xd2a0:*value=f->ape;break;
    default:f->errors++;return -1;
    }return 0;
}
static int write_wptr(void *p,uint32_t value)
{
    struct fixture *f=p;f->submissions++;
    /* Fetch alignment is always 16 DWORDs, independent of ring capacity. */
    if (value>=f->memory[0].bytes || (value&63u)) {f->errors++;return -1;}
    f->wptr=value;
    if (!f->hang) execute(f);
    return f->fail_write ? -1 : 0;
}
static int resolve(void *p,uint64_t address,uint64_t bytes,volatile uint32_t **cpu)
{
    struct fixture *f=p;f->resolves++;
    struct bank *b=locate(f,address,bytes);
    if (!b||f->unmapped) return -1;
    *cpu=b->cpu+(address-b->address)/4;return 0;
}
static int sync_memory(void *p,enum polaris_sdma_sync direction,
                         const struct polaris_sdma_mapping *map)
{
    struct fixture *f=p;f->syncs++;
    struct bank *b=locate(f,map->range.gpu_base,map->range.bytes);
    if (!b||f->fail_sync) return -1;
    unsigned offset=(unsigned)(map->range.gpu_base-b->address)/4;
    for (unsigned i=0;i<map->range.bytes/4;i++) {
        if(direction==POLARIS_SDMA_TO_DEVICE)b->gpu[offset+i]=b->cpu[offset+i];
        else b->cpu[offset+i]=b->gpu[offset+i];
    }return 0;
}
static uint64_t clock_us(void *p)
{ struct fixture *f=p;uint64_t now=f->clock;f->clock+=f->step;return now; }
static void init(struct fixture *f)
{
    memset(f,0,sizeof(*f)); f->id=0x67df1002;f->control=13;f->base=0x1000;f->step=1;
    for(unsigned i=0;i<4;i++) {
        f->memory[i].address=0x100000+i*0x1000;
        f->memory[i].bytes=i==1?4:256;
        for(unsigned j=0;j<64;j++)f->memory[i].cpu[j]=0x55550000+i*64+j;
    }
}
static struct polaris_sdma_mapping mapping(struct fixture *f,unsigned n)
{return (struct polaris_sdma_mapping){{f->memory[n].address,f->memory[n].bytes},f->memory[n].cpu};}
static struct polaris_sdma_queue_ops ops(struct fixture *f)
{return(struct polaris_sdma_queue_ops){f,identity,read_reg,write_wptr,resolve,sync_memory,clock_us};}
static int attach(struct fixture *f,struct polaris_sdma_queue *q)
{
    struct polaris_sdma_queue_ops o=ops(f);
    struct polaris_sdma_mapping ring=mapping(f,0),fence=mapping(f,1);
    return polaris_sdma_queue_attach(q,&o,&ring,&fence);
}
static unsigned calls(struct fixture *f)
{return f->reads+f->ids+f->resolves+f->syncs+f->submissions;}
