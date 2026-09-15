#include "amd/polaris/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n",__LINE__,#x); } } while (0)
#define ARENA UINT64_C(0x801000000)
#define SCANOUT UINT64_C(0x800000000)
#define ARENA_BYTES 0x200000u
struct machine {
    uint8_t cpu[ARENA_BYTES], gpu[ARENA_BYTES], screen_cpu[8192], screen_gpu[8192];
    uint32_t sram[0x10000], regs[0x6000], index, smc_clock, reset, pc, events, mode, status;
    uint32_t response, argument, uploads, messages, auth, status_cleared;
    uint64_t toc, scratch, now;
    unsigned io, syncs, packets, copies, fills, fences, errors;
    unsigned missing_map, partial_load, hang, skip_sync, cold;
    uint64_t alias_gpu;
    volatile uint8_t *alias_cpu;
    unsigned resolves;
};
static uint32_t get32(const uint8_t *p)
{ return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void put32(uint8_t *p,uint32_t v)
{ for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i)); }
static uint8_t files[8][512], workspace[65536];
static struct polaris_runtime_request request;
static void firmware(void)
{
    memset(files,0,sizeof(files));memset(&request,0,sizeof(request));
    for(unsigned k=0;k<8;++k) {
        uint8_t *p=files[k];put32(p,512);put32(p+4,k<2?52:k==6?104:k==7?36:44);
        p[8]=k==6?2:1;p[10]=k<2?1:0;p[12]=k<2?3:k==7?7:8;p[14]=k<2?1:k==7?2:0;
        put32(p+16,100+k);put32(p+20,64);put32(p+24,256);
        if(k==5){put32(p+36,8);put32(p+40,4);}
        if(k==7)put32(p+32,0x20000);
        for(unsigned j=256;j<512;++j)p[j]=(uint8_t)(k*19+j);
        if(k<7)request.firmware.file[k]=(struct polaris_fw_blob){p,512};
    }
    /* Test SMC payload contains its actual SoftRegisters header pointer. The
     * protocol model recognizes boot/reset events; it does NOT emulate SMC
     * instruction execution or claim these synthetic bytes are real firmware. */
    put32(files[7]+256+0x30,0x30000);
    request.smc=(struct polaris_fw_blob){files[7],512};request.smc_security_key=1;
    request.workspace=workspace;request.workspace_bytes=sizeof(workspace);
    request.width=64;request.height=32;request.pitch=256;
    request.memory.vram=(struct polaris_memory_range){SCANOUT,UINT64_C(0x100000000)};
    request.memory.aperture=(struct polaris_memory_range){SCANOUT,0x10000000};
    request.memory.aperture_cpu_base=0xd0000000;
    request.memory.arena=(struct polaris_memory_range){ARENA,ARENA_BYTES};
    request.memory.scanout=(struct polaris_memory_range){SCANOUT,8192};
    request.memory.staging_bytes=8192;
}
static uint8_t *memory(struct machine *m,uint64_t a,uint64_t n,int device)
{
    if(a>=ARENA && a-ARENA<=ARENA_BYTES && n<=ARENA_BYTES-(a-ARENA))
        return (device?m->gpu:m->cpu)+(a-ARENA);
    if(a>=SCANOUT && a-SCANOUT<=8192 && n<=8192-(a-SCANOUT))
        return (device?m->screen_gpu:m->screen_cpu)+(a-SCANOUT);
    return 0;
}
static int resolve(void *p,uint64_t a,uint64_t n,volatile uint8_t **out)
{
    struct machine *m=p;
    ++m->resolves;
    uint8_t *v=memory(m,a,n,0);
    if(m->missing_map || !v)return -1;
    if(m->alias_gpu==a){*out=m->alias_cpu;return 0;}
    *out=v;return 0;
}
static int sync_memory(void *p,enum polaris_sdma_sync direction,uint64_t a,uint64_t n)
{
    struct machine *m=p;uint8_t *cpu=memory(m,a,n,0),*gpu=memory(m,a,n,1);
    if(!cpu||!gpu)return -1;
    ++m->syncs;
    if(m->skip_sync)return 0;
    if(direction==POLARIS_SDMA_TO_DEVICE)memcpy(gpu,cpu,(size_t)n);
    else memcpy(cpu,gpu,(size_t)n);
    return 0;
}
static uint32_t *indirect(struct machine *m)
{
    switch(m->index) {
    case 0x80000000:return &m->reset;
    case 0x80000004:return &m->smc_clock;
    case 0x80000370:return &m->pc;
    case 0xc0000004:return &m->events;
    case 0xe0003088:return &m->status;
    case 0xe00030a4:return &m->mode;
    default:if(m->index<0x40000 && !(m->index&3))return &m->sram[m->index/4];
    }
    return 0;
}
static int identity(void *p,uint32_t *v)
{struct machine *m=p;++m->io;*v=0x67df1002;return 0;}
static int read_reg(void *p,uint32_t a,uint32_t *v)
{
    struct machine *m=p;++m->io;
    if(a==0x6b0){*v=m->index;return 0;}
    if(a==0x6b4){uint32_t *r=indirect(m);if(!r)return -1;*v=*r;return 0;}
    if(a==0x254){*v=m->response;return 0;}
    if(a<sizeof(m->regs) && !(a&3)){*v=m->regs[a/4];return 0;}
    ++m->errors;return -1;
}
static uint32_t ring_word(struct machine *m,uint32_t at)
{
    uint64_t base=(uint64_t)m->regs[0xd204/4]<<8;
    uint32_t n=4u<<((m->regs[0xd200/4]>>1)&31);
    uint8_t *p=memory(m,base+(at&(n/4-1))*4,4,1);
    if(!p){++m->errors;return UINT32_MAX;}return get32(p);
}
static int execute(struct machine *m)
{
    uint32_t n=4u<<((m->regs[0xd200/4]>>1)&31),at=m->regs[0xd20c/4]/4,target=m->regs[0xd210/4]/4;
    if(!(m->regs[0xd200/4]&1) || (m->regs[0xd048/4]&1) || n<256 || n>65536)return -1;
    for(unsigned bound=0;at!=target && bound<n/4;++bound) {
        uint32_t op=ring_word(m,at),bytes=0,value=0,advance=1;
        uint64_t src=0,dst=0;uint8_t *s=0,*d=0;
        if(op==0){at=(at+1)&(n/4-1);continue;}
        ++m->packets;
        if(op==1) {
            bytes=ring_word(m,at+1);if(ring_word(m,at+2))return -1;
            src=ring_word(m,at+3)|(uint64_t)ring_word(m,at+4)<<32;
            dst=ring_word(m,at+5)|(uint64_t)ring_word(m,at+6)<<32;
            s=memory(m,src,bytes,1);advance=7;++m->copies;
        } else if(op==11) {
            dst=ring_word(m,at+1)|(uint64_t)ring_word(m,at+2)<<32;
            value=ring_word(m,at+3);bytes=ring_word(m,at+4);advance=5;++m->fills;
        } else if(op==5) {
            dst=ring_word(m,at+1)|(uint64_t)ring_word(m,at+2)<<32;
            value=ring_word(m,at+3);bytes=4;advance=4;++m->fences;
        } else return -1;
        d=memory(m,dst,bytes,1);
        if(!bytes || (bytes&3) || !d || (op==1 && !s))return -1;
        if(op==1)memcpy(d,s,bytes);else for(unsigned i=0;i<bytes;i+=4)put32(d+i,value);
        at=(at+advance)&(n/4-1);
    }
    if(at!=target)return -1;
    m->regs[0xd20c/4]=m->regs[0xd210/4];return 0;
}
static int load_directory(struct machine *m)
{
    uint8_t *toc=memory(m,m->toc,344,1),*scratch=memory(m,m->scratch,819200,1);
    if(!toc||!scratch||!m->status_cleared||get32(toc)!=1||get32(toc+4)!=9)return -1;
    for(unsigned i=0;i<819200;++i)if(scratch[i])return -1;
    unsigned mask=0;
    for(unsigned i=0;i<9;++i) {
        uint8_t *e=toc+8+i*28;unsigned id=e[0]|(unsigned)e[1]<<8;
        unsigned kind=id==10?6:id==3?2:id==4?3:id==5?4:(id>=6&&id<=8)?5:id-1;
        if(kind>6)return -1;
        unsigned n=get32(e+20),expected=id==6?32:(id==7||id==8)?16:64;
        uint64_t addr=(uint64_t)get32(e+4)<<32|get32(e+8);
        uint8_t *data=memory(m,addr,n,1);
        if(!data||n!=expected||memcmp(data,files[kind]+256+((id==7||id==8)?32:0),n))return -1;
        mask|=1u<<id;
    }
    if(mask!=0x5fe)return -1;
    m->sram[0x3006c/4]=m->partial_load?6:0x47e;return 0;
}
static int write_reg(void *p,uint32_t a,uint32_t v)
{
    struct machine *m=p;++m->io;
    if(a==0x6b0){m->index=v;return 0;}
    if(a==0x6b4) {
        uint32_t *r=indirect(m);if(!r)return -1;
        if(m->index>=0x20000 && m->index<0x20040) {
            if(!(m->reset&1))return -1;++m->uploads;
        }
        if(m->index==0x3006c && !v)m->status_cleared=1;
        if(m->index==0x80000000 && (*r&1) && !(v&1)) {
            if(m->uploads!=16)return -1;
            m->pc=0x20100;m->events|=0x10000;
            if(!(m->mode&0x10000)||m->auth)m->sram[0x3f000/4]=1;
        }
        *r=v;return 0;
    }
    if(a==0x254){m->response=v;return 0;}
    if(a==0x290){m->argument=v;return 0;}
    if(a==0x250) {
        if(m->response)return -1;++m->messages;
        switch(v) {
        case 0x100:if(m->argument!=0x20000)return -1;m->status=3;m->auth=1;break;
        case 0x252:m->scratch=(uint64_t)m->argument<<32;break;
        case 0x253:m->scratch|=m->argument;break;
        case 0x250:m->toc=(uint64_t)m->argument<<32;break;
        case 0x251:m->toc|=m->argument;break;
        case 0x254:if(m->argument!=0x47e || load_directory(m))return -1;break;
        default:return -1;
        }
        m->response=1;return 0;
    }
    if(a<sizeof(m->regs) && !(a&3)) {
        m->regs[a/4]=v;
        if(a==0xd210 && (m->regs[0xd200/4]&1) && !(m->regs[0xd048/4]&1) && !m->hang) {
            if(execute(m)){++m->errors;return -1;}
        }
        return 0;
    }
    ++m->errors;return -1;
}
static uint64_t now_us(void *p){struct machine *m=p;return m->now++;}
static void init(struct machine *m)
{
    memset(m,0,sizeof(*m));m->pc=0x20100;m->response=1;m->mode=0x20000;m->events=0x80;
    m->sram[0x3f000/4]=1;m->sram[0x20030/4]=0x30000;
    m->regs[0x2024/4]=0x08ff0800;m->regs[0x5428/4]=4096;m->regs[0xd034/4]=7;
    /* Start with byte-swap/context-switch hazards and nontrivial reserved
     * fields, so clearing everything or sharing wrong masks cannot pass. */
    m->regs[0xd010/4]=0x08040018;
    m->regs[0xd014/4]=0x5a5a5a5a;
    m->regs[0xd00c/4]=0xa5abcd55;
    memset(m->gpu,0xa6,sizeof(m->gpu));memset(m->screen_cpu,0x39,sizeof(m->screen_cpu));
    memset(m->screen_gpu,0x39,sizeof(m->screen_gpu));
}
static struct polaris_platform platform(struct machine *m)
{return(struct polaris_platform){m,identity,read_reg,write_reg,resolve,sync_memory,now_us};}
static void boundaries(struct machine *m,struct polaris_runtime *r,struct polaris_platform *p)
{
    /* Valid distinct GPU ranges are deliberately translated to colliding CPU
     * pointers. The caller must reject before the first firmware staging or
     * device memory write; catching this later in a packet encoder is too late. */
    for(unsigned which=0;which<9;++which) {
        firmware();init(m);memset(r,0,sizeof(*r));m->alias_gpu=ARENA;
        switch(which) {
        case 0:m->alias_cpu=m->cpu+4096;break; /* TOC aliases scratch */
        case 1:m->alias_gpu=SCANOUT;m->alias_cpu=m->cpu;break;
        case 2:m->alias_cpu=(volatile uint8_t *)r;break;
        case 3:m->alias_cpu=(volatile uint8_t *)&request;break;
        case 4:m->alias_cpu=(volatile uint8_t *)p;break;
        case 5:m->alias_cpu=workspace;break;
        case 6:m->alias_cpu=files[0];break;
        case 7:m->alias_cpu=files[7];break;
        case 8:m->alias_cpu=(volatile uint8_t *)(UINTPTR_MAX-3);break;
        }
        CHECK(polaris_runtime_start(r,p,&request)==-1);
        CHECK(!r->attempted && !r->present.active && !m->messages && !m->syncs);
        CHECK(m->cpu[0]==0 && m->cpu[4096]==0 && m->gpu[0]==0xa6);
    }
    /* These reject BEFORE any platform callback. The large declared capacity
     * is intentional: validating only workspace length would overwrite the
     * live lock/ops/request with staged firmware. No invalid pointer is read. */
    for(unsigned which=0;which<4;++which) {
        firmware();init(m);memset(r,0,sizeof(*r));
        request.workspace=which==0?(uint8_t *)r:which==1?(uint8_t *)&request:
                          which==2?(uint8_t *)p:(uint8_t *)(UINTPTR_MAX-4095);
        CHECK(polaris_runtime_start(r,p,&request)==-1);
        CHECK(!m->io && !m->resolves && !r->attempted);
    }
    firmware();init(m);memset(r,0,sizeof(*r));
    memcpy(workspace,files[7],sizeof(files[7]));request.smc.data=workspace;
    CHECK(polaris_runtime_start(r,p,&request)==-1);
    CHECK(!m->io && !m->resolves && !r->attempted);
    firmware();
}
int main(void)
{
    struct machine *m=malloc(sizeof(*m));struct polaris_runtime *r=calloc(1,sizeof(*r));
    if(!m||!r)return 2;firmware();init(m);struct polaris_platform p=platform(m);
    for(unsigned cold=0;cold<3;++cold) {
        init(m);memset(r,0,sizeof(*r));
        if(cold){m->smc_clock=1;m->pc=0;m->sram[0x3f000/4]=0;if(cold==2)m->mode|=0x10000;}
        int rc=polaris_runtime_start(r,&p,&request);
        if(rc)printf("start rc=%d stage=%u failed=%u err=%d io=%u\n",rc,r->stage,r->failed_stage,r->error,m->io);
        CHECK(rc==0 && r->stage==POLARIS_RUNTIME_ACTIVE && !r->quarantined);
        CHECK(r->smu.loaded && r->engine.configured && r->queue.completed==2);
        CHECK(m->fills==1 && m->copies==1 && m->fences==2 && !m->errors);
        CHECK(m->regs[0xd010/4]==0x08000000 && m->regs[0xd014/4]==0x02cb5a5f && m->regs[0xd00c/4]==0x00abc000);
        CHECK(cold?m->uploads==16:m->uploads==0);
        uint32_t pixels[64*32];for(unsigned i=0;i<64*32;++i)pixels[i]=0x12340000u^(i*0x10201u);
        unsigned busy_io=m->io,busy_resolves=m->resolves,busy_syncs=m->syncs;
        r->lock=1;
        CHECK(polaris_runtime_present(r,pixels,sizeof(pixels),256,0,0,64,32)==-2);
        CHECK(m->io==busy_io && m->resolves==busy_resolves && m->syncs==busy_syncs && r->lock==1);
        r->lock=0;
        CHECK(polaris_runtime_present(r,pixels,sizeof(pixels),256,0,0,64,32)==0);
        CHECK(memcmp(m->screen_gpu,pixels,sizeof(pixels))==0 && memcmp(m->screen_cpu,pixels,sizeof(pixels))==0);
        CHECK(r->present.frames==1 && r->queue.copied_bytes==4096+sizeof(pixels));
        /* With a pitched partial rectangle, untouched gutters must survive. */
        for(unsigned i=0;i<64*32;++i)pixels[i]^=0x55;
        CHECK(polaris_runtime_present(r,pixels,sizeof(pixels),256,3,4,17,9)==0);
        CHECK(get32(m->screen_gpu+(4*64+3)*4)==pixels[4*64+3]);
        CHECK(get32(m->screen_gpu+(4*64+2)*4)==(pixels[4*64+2]^0x55));
        m->hang=1;
        CHECK(polaris_runtime_present(r,pixels,sizeof(pixels),256,0,0,64,32)==-2);
        CHECK(r->quarantined && r->queue.quarantined && r->stage==POLARIS_RUNTIME_FAILED);
        unsigned io=m->io;
        CHECK(polaris_runtime_present(r,pixels,sizeof(pixels),256,0,0,64,32)==-2 && m->io==io);
        CHECK(polaris_runtime_start(r,&p,&request)==-2 && m->io==io);
    }
    init(m);memset(r,0,sizeof(*r));request.firmware.file[3].bytes=0;
    CHECK(polaris_runtime_start(r,&p,&request)!=0 && r->stage!=POLARIS_RUNTIME_ACTIVE && !m->io);
    firmware();init(m);memset(r,0,sizeof(*r));m->partial_load=1;
    CHECK(polaris_runtime_start(r,&p,&request)==-2 && r->failed_stage==POLARIS_RUNTIME_SMU && !r->present.active);
    init(m);memset(r,0,sizeof(*r));m->missing_map=1;
    CHECK(polaris_runtime_start(r,&p,&request)!=0 && !r->smu.loaded && !r->present.active && !m->messages);
    init(m);memset(r,0,sizeof(*r));m->skip_sync=1;
    CHECK(polaris_runtime_start(r,&p,&request)!=0 && !r->present.active);
    boundaries(m,r,&p);
    free(m);free(r);
    printf("POLARIS_RUNTIME: %u checks, %u failures (host protocol model; no physical GPU proof)\n",checks,failures);
    return failures?1:0;
}
