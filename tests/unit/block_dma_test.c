/* Compile verbatim production command/lifecycle functions. The DMA/device
 * fixtures expose different CPU/bus addresses and fragmented bus pages; DMA
 * API internals have their own real-implementation suite. No disk is touched. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include "dma.h"
#include "driver.h"
#define AHCI_DEVICE_MODEL 1
#include "blkdev.h"
#include "block_dma_types.inc"
static int failures,checks,unmapped,completed,quarantined,stopped,panic_expected;
static void *host_allocs[128];
static unsigned nhost_allocs;
static void *fixture_calloc(size_t n,size_t size){
 void *p=calloc(n,size);if(!p||nhost_allocs==128)exit(2);host_allocs[nhost_allocs++]=p;return p;
}
static void host_cleanup(void){for(unsigned i=0;i<nhost_allocs;i++)free(host_allocs[i]);}
#define calloc fixture_calloc
static size_t completed_valid;
static uint64_t ticks=100,serial=1;
static jmp_buf panic_env;
static struct dma_mapping fixture_map;
static struct dma_segment fixture_segments[300];
static struct dma_buffer *buffers[32];
static unsigned nbuffers;
static uint8_t registers[0x8000] __attribute__((aligned(4096)));
static unsigned char payload[DMA_MAX_MAPPING+4096] __attribute__((aligned(4096)));
static int hold_running,freed,offline_calls,disabled,event,offline_event,stop_event,free_event,disable_event;
static struct blkdev fixture_disk;
static struct device fixture_device,other_device;
struct blkdev *blk_find(const char *name){(void)name;return &fixture_disk;}
void blk_dev_offline(struct blkdev *disk){disk->offline=1;offline_calls++;offline_event=++event;}
void dev_disable(struct device *dev){(void)dev;disabled++;disable_event=++event;}
int dev_disable_checked(struct device *dev){dev_disable(dev);return 0;}
static void check(int x,const char *s){checks++;if(!x){printf("FAIL: %s\n",s);failures++;}}
void kprintf(const char *s,...){(void)s;}
void panic(const char *s,...){(void)s;if(panic_expected)longjmp(panic_env,1);fprintf(stderr,"unexpected panic\n");exit(2);}
uint64_t timer_ms(void){return ticks;}
void dma_device_init(struct dma_device *d,const char*n,uint64_t mask){memset(d,0,sizeof *d);d->name=n;d->mask=mask;}
struct dma_buffer *dma_alloc_coherent(struct dma_device*d,size_t n,size_t a,size_t b){
 (void)a;(void)b;struct dma_buffer*m=calloc(1,sizeof *m);m->cpu=calloc(1,n);m->size=n;m->dev=d;
 m->dma.value=(d->mask==DMA_MASK_32?0x50000000ull:0x100000000ull)+(uint64_t)nbuffers*0x10000;
 buffers[nbuffers++]=m;return m;
}
int dma_free_coherent(struct dma_buffer*b){if(b->state==DMA_DEVICE_OWNED||b->state==DMA_QUARANTINED)return -1;freed++;free_event=++event;return 0;}
uint64_t dma_buffer_submit(struct dma_buffer*b){if(b->dev->blocked||b->state==DMA_DEVICE_OWNED)return 0;b->state=DMA_DEVICE_OWNED;return b->token=++serial;}
int dma_buffer_complete(struct dma_buffer*b,uint64_t t){if(b->state!=DMA_DEVICE_OWNED||b->token!=t)return -1;b->state=DMA_COMPLETED;return 0;}
struct dma_mapping *dma_map_kernel(struct dma_device*d,void*p,size_t n,enum dma_direction dir,unsigned f){
 (void)f;memset(&fixture_map,0,sizeof fixture_map);fixture_map.dev=d;fixture_map.cpu=p;fixture_map.size=n;fixture_map.direction=dir;
 fixture_map.segments=fixture_segments;
 size_t left=n,off=(uintptr_t)p&4095;unsigned i=0;
 while(left){size_t len=4096-off;if(len>left)len=left;fixture_segments[i].addr.value=0x180000000ull+(uint64_t)i*0x10000+off;fixture_segments[i].len=len;i++;left-=len;off=0;}
 fixture_map.nsegments=i;return &fixture_map;
}
dma_addr_t dma_mapping_addr(const struct dma_mapping*m,size_t off){for(size_t i=0;i<m->nsegments;i++){if(off<m->segments[i].len)return dma_addr_add(m->segments[i].addr,off);off-=m->segments[i].len;}return (dma_addr_t){UINT64_MAX};}
uint64_t dma_mapping_submit(struct dma_mapping*m){if(m->dev->blocked||m->state==DMA_DEVICE_OWNED)return 0;m->state=DMA_DEVICE_OWNED;return m->token=++serial;}
int dma_mapping_complete(struct dma_mapping*m,uint64_t t,size_t valid){if(m->state!=DMA_DEVICE_OWNED||t!=m->token)return -1;m->state=DMA_COMPLETED;completed++;completed_valid=valid;return 0;}
int dma_unmap(struct dma_mapping*m){if(m->state==DMA_DEVICE_OWNED||m->state==DMA_QUARANTINED)return -1;unmapped++;return 0;}
int dma_sync_for_device(struct dma_mapping*m){return m->state==DMA_DEVICE_OWNED?-1:0;}
void dma_device_quiesced(struct dma_device*d){d->blocked=1;stopped++;stop_event=++event;for(unsigned i=0;i<nbuffers;i++)if(buffers[i]->dev==d)buffers[i]->state=DMA_QUIESCED;if(fixture_map.dev==d)fixture_map.state=DMA_QUIESCED;}
void dma_device_quarantine(struct dma_device*d){d->blocked=1;quarantined++;if(fixture_map.dev==d)fixture_map.state=DMA_QUARANTINED;}
int dma_device_resume(struct dma_device*d){d->blocked=0;return 0;}
void dma_rmb(void){}
static void barrier(void){}
static uint32_t r32(volatile uint8_t*b,int o){return *(volatile uint32_t*)(b+o);}
static void w32(volatile uint8_t*b,int o,uint32_t v){
#ifdef TEST_AHCI
 if(o==P_IS||o==P_SERR){*(volatile uint32_t*)(b+o)&=~v;return;}
 if(o==P_CMD&&hold_running)v|=CMD_CR|CMD_FR;
#endif
 *(volatile uint32_t*)(b+o)=v;
}
#ifdef TEST_NVME
static struct dma_device g_dma;
static volatile uint8_t*g_regs=registers;
static struct nvme_q g_io[NVME_IOQ_MAX];
static uint64_t *g_prp_list[NVME_IOQ_MAX];
static struct dma_buffer *g_prp_mem[NVME_IOQ_MAX];
static int g_ready=1,g_nioq=1,g_health_ok=1;
static struct nvme_q g_admin;
static struct device *g_device=&fixture_device;
static uint32_t g_max_sectors=65535,g_nsid=1;
#else
static struct ahci_port g_ports[AHCI_MAX_DISKS];
static int g_ndisks;
static void ahci_count_cmd(struct ahci_port*p){p->req_cmds++;}
#endif
#include "block_dma_functions.inc"
#ifdef TEST_NVME
static void setup(void){
 memset(registers,0,sizeof registers);memset(g_io,0,sizeof g_io);dma_device_init(&g_dma,"nvme",DMA_MASK_64);g_ready=1;
 struct nvme_q*q=&g_io[0];q->sq=calloc(64,sizeof *q->sq);q->cq=calloc(64,sizeof *q->cq);q->depth=64;q->cq_phase=1;
 q->sq_db=registers+4096;q->cq_db=registers+4100;
 g_prp_mem[0]=dma_alloc_coherent(&g_dma,4096,4096,0);g_prp_list[0]=g_prp_mem[0]->cpu;
}
static void done(struct blk_req*r,int status){struct nvme_q*q=&g_io[0];q->cq[q->cq_head].cid=(uint16_t)r->tag;q->cq[q->cq_head].status=(uint16_t)(status*2+q->cq_phase);}
int main(void){
 (void)hold_running;setup();struct blk_req r={.op=BLK_OP_READ,.count=20,.buf=payload+128,.dev_lba=25};
 check(nvme_issue(&r)==0,"NVMe submits mapped fragmented buffer");
 struct nvme_sqe*c=&g_io[0].sq[0];
 check(c->prp1==0x180000080ull,"NVMe PRP1 is bus address, never CPU pointer");
 check(c->prp2==dma_addr_value(g_prp_mem[0]->dma),"NVMe PRP2 identifies DMA list");
 check(g_prp_list[0][0]==0x180010000ull&&g_prp_list[0][1]==0x180020000ull,"NVMe PRPs follow actual fragmented physical pages");
 check(fixture_map.state==DMA_DEVICE_OWNED&&unmapped==0,"NVMe holds map across async poll");
 check(nvme_blk_poll(&r)==0&&unmapped==0,"NVMe pending command retains ownership");
 g_io[0].cq[g_io[0].cq_head].cid=(uint16_t)(r.tag+1);g_io[0].cq[g_io[0].cq_head].status=1;
 check(nvme_blk_poll(&r)==0&&fixture_map.state==DMA_DEVICE_OWNED&&unmapped==0,"NVMe stale CID cannot complete or release the current mapping");
 done(&r,0);check(nvme_blk_poll(&r)==1&&r.status==0&&r.done==20,"NVMe completion advances exact chunk");
 check(unmapped==1&&completed_valid==20*512,"NVMe completion returns exact valid bytes then unmaps");
 r=(struct blk_req){.op=BLK_OP_WRITE,.count=8,.buf=payload+128};check(nvme_issue(&r)==0,"NVMe two-page command submits");
 c=&g_io[0].sq[1];check(c->prp2==0x180010000ull,"NVMe two-page PRP2 is actual next page");done(&r,1);nvme_blk_poll(&r);check(r.status==-1&&completed_valid==0&&unmapped==2,"NVMe error returns no valid read bytes and releases mapping");
 r=(struct blk_req){.op=BLK_OP_READ,.count=8,.buf=payload};nvme_issue(&r);r.deadline=0;
 check(nvme_blk_poll(&r)==1&&r.status==-1&&stopped==1&&unmapped==3&&!g_ready,"NVMe timeout waits for stop before unmapping");
 setup();r=(struct blk_req){.op=BLK_OP_READ,.count=20,.buf=payload+128};nvme_issue(&r);
 done(&r,0);nvme_quiesce(); /* admin timeout after hardware queued a block CQE */
 int before_complete=completed;panic_expected=1;
 if(!setjmp(panic_env)) {
  check(nvme_blk_poll(&r)==1&&r.status==-1&&unmapped==4&&completed==before_complete&&
        !g_io[0].mapping&&!g_io[0].map_token&&!g_io[0].prp_token&&g_io[0].cq_head==0,
        "NVMe controller stop discards stale completion and releases pending mapping");
 } else check(0,"NVMe controller stop discards stale completion and releases pending mapping");
 panic_expected=0;
 setup();r=(struct blk_req){.op=BLK_OP_READ,.count=8,.buf=payload};nvme_issue(&r);r.deadline=0;*(uint32_t*)(registers+REG_CSTS)=1;
 panic_expected=1;if(!setjmp(panic_env)){nvme_blk_poll(&r);check(0,"NVMe unconfirmed stop must not return to caller");}
 check(quarantined==1&&unmapped==4,"NVMe unconfirmed stop quarantines without unmap");
 panic_expected=0;setup();g_device=&fixture_device;
 g_admin.sq_mem=dma_alloc_coherent(&g_dma,4096,4096,0);g_admin.cq_mem=dma_alloc_coherent(&g_dma,4096,4096,0);
 g_io[0].sq_mem=dma_alloc_coherent(&g_dma,4096,4096,0);g_io[0].cq_mem=dma_alloc_coherent(&g_dma,4096,4096,0);
 dma_buffer_submit(g_admin.sq_mem);dma_buffer_submit(g_admin.cq_mem);dma_buffer_submit(g_io[0].sq_mem);dma_buffer_submit(g_io[0].cq_mem);
 int before_free=freed,before_stop=stopped;
 fixture_device.drv=&nvme_driver;fixture_device.drv->remove(&fixture_device);
 check(offline_calls==1&&fixture_disk.offline&&stopped==before_stop+1&&disabled==1&&offline_event<stop_event&&stop_event<free_event&&free_event<disable_event,
       "NVMe normal remove offlines disk and acknowledges hardware stop");
 check(freed==before_free+5&&!g_regs&&!g_device&&!g_admin.sq_mem&&!g_io[0].sq_mem&&!g_prp_mem[0],
       "NVMe normal remove releases admin IO and PRP buffers");
 nvme_shutdown();check(freed==before_free+5,"NVMe repeated shutdown does not double free");
 (void)other_device;
 host_cleanup();printf("NVMe DMA: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
#else
int main(void){
 struct ahci_port p={0};p.reg=registers;p.lba48=1;
 check(ahci_dma_init(&p,CAP_S64A)==0&&p.dma_dev.mask==DMA_MASK_64,"AHCI uses CAP_S64A for full DMA mask");
 struct ahci_port low={0};check(ahci_dma_init(&low,0)==0&&low.dma_dev.mask==DMA_MASK_32,"AHCI without S64A constrains all DMA objects to 32 bits");
 dma_buffer_submit(p.metadata);port_start(&p);
 struct ahci_cmdspec spec={ATA_READ_DMA_EXT,0,29,20,payload+128,20*512};
 check(ahci_begin(&p,&spec)==0,"AHCI maps and issues fragmented buffer");uint32_t*hdr=(void*)p.dma;uint32_t*prd=(void*)(p.dma+0x880);
 check(hdr[2]==(uint32_t)(p.metadata->dma.value+0x800)&&hdr[3]==(uint32_t)((p.metadata->dma.value+0x800)>>32),"AHCI CTBA uses metadata DMA address");
 check((hdr[0]>>16)==3&&prd[0]==0x80000080&&prd[1]==1&&prd[4]==0x80010000&&prd[5]==1,"AHCI PRDT follows mapped scatter segments");
 check(fixture_map.state==DMA_DEVICE_OWNED&&ahci_step(&p,0)==0&&unmapped==0,"AHCI retains mapping while command is pending");
 *(uint32_t*)(registers+P_CI)=0;hdr[1]=spec.bytes;
 check(ahci_step(&p,0)==1&&unmapped==1&&completed_valid==spec.bytes,"AHCI completes valid bytes before unmap");
 ahci_begin(&p,&spec);uint64_t token=p.map_token;*(uint32_t*)(registers+P_IS)=IS_TFES;
 check(ahci_step(&p,0)==0&&p.map_token!=token&&fixture_map.state==DMA_DEVICE_OWNED&&unmapped==1,"AHCI retry stops and renews ownership without premature unmap");
 p.attempt=AHCI_RETRIES-1;*(uint32_t*)(registers+P_IS)=IS_TFES;
 check(ahci_step(&p,0)==-1&&p.dma_dev.blocked&&unmapped==2,"AHCI final failure leaves port stopped before releasing caller mapping");
 dma_device_resume(&p.dma_dev);dma_buffer_submit(p.metadata);port_start(&p);ahci_begin(&p,&spec);*(uint32_t*)(registers+P_IS)=IS_TFES;hold_running=1;
 panic_expected=1;if(!setjmp(panic_env)){ahci_step(&p,0);check(0,"AHCI unconfirmed stop must not return to caller");}
 check(quarantined==1&&unmapped==2,"AHCI unconfirmed stop quarantines without unmap");
 panic_expected=0;hold_running=0;memset(registers,0,sizeof registers);
 g_ndisks=2;g_ports[0].controller=&fixture_device;g_ports[1].controller=&other_device;
 g_ports[0].reg=registers;g_ports[1].reg=registers+0x100;g_ports[0].block=&fixture_disk;
 ahci_dma_init(&g_ports[0],CAP_S64A);ahci_dma_init(&g_ports[1],CAP_S64A);
 dma_buffer_submit(g_ports[0].metadata);dma_buffer_submit(g_ports[1].metadata);
 int before_free=freed,before_stop=stopped;
 fixture_device.drv=&ahci_driver;fixture_device.drv->remove(&fixture_device);
 check(offline_calls==1&&fixture_disk.offline&&stopped==before_stop+1&&disabled==1&&offline_event<stop_event&&stop_event<free_event&&free_event<disable_event,
       "AHCI normal remove offlines port and acknowledges hardware stop");
 check(freed==before_free+1&&!g_ports[0].metadata&&g_ports[1].metadata->state==DMA_DEVICE_OWNED,
       "AHCI normal remove frees only its controller DMA objects");
 ahci_shutdown(&fixture_device);check(freed==before_free+1,"AHCI repeated shutdown does not double free");
 host_cleanup();printf("AHCI DMA: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
#endif
