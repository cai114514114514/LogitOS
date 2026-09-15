/* Production virtio clients + transport + DMA core. Only PCI/MMIO, physical
 * allocation and interrupt effects are modelled. CPU pointers live in a sparse
 * host arena; device addresses deliberately start above 4 GiB. This checks the
 * submitted bytes/ownership, not a copy of the driver's descriptor algorithm. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <setjmp.h>
#undef memset
#undef memcpy
#undef htons
#undef ntohs
#undef htonl
#undef ntohl
#include "mmhost.h"
#include "spinlock.h"
#include "virtio.c"
#include "virtio_gpu.c"
#include "virtio_blk.c"
#include "virtio_rng.c"
#include "virtio_balloon.c"
#define barrier net_test_barrier
#include "virtio_net.c"
#undef barrier

#define HIGH UINT64_C(0x100000000)
#define ARENA (64u * 1024u * 1024u)
uint64_t mm_host_base, mm_host_kend, mm_host_cr3;
static uint64_t next_phys = HIGH, live;
static unsigned short refs[ARENA / 4096];
static unsigned char pins[ARENA / 4096];
static int tests, fails, reset_fail, timeout_all, io_faults, unexpected_free;
static int command_reject, bar_maps;
static int blk_offlined, expect_live_blk;
void *fb_dma_test_hold_writer(void);
void fb_dma_test_release_writer(void);
static uint32_t timeout_gpu_cmd;
static int attach_seen, rng_seq, tx_seen, rx_seen;
static jmp_buf panic_jmp;
static int expect_panic, panics;
static struct device pci_dev_model, pci_second_model;
static uint8_t pci_mmio[1024] __attribute__((aligned(16)));
static uint8_t pci_second_mmio[1024] __attribute__((aligned(16)));
#define CHECK(x,why) do { tests++; if (!(x)) { fails++; printf("FAIL: %s (%d)\n",why,__LINE__); } } while(0)
static size_t pi(uint64_t p) { return (size_t)((p - HIGH) / 4096); }
void *kmalloc(size_t n) { return calloc(1,n); }
void kfree(void *p) { free(p); }
uint64_t spin_lock_irqsave(spinlock_t *l) { (void)l; return 0; }
void spin_unlock_irqrestore(spinlock_t *l,uint64_t f) { (void)l;(void)f; }
void kprintf(const char *f,...) { (void)f; }
void panic(const char *f,...) { (void)f; panics++; if (expect_panic) longjmp(panic_jmp,1); abort(); }
uint64_t timer_ms(void) { return 0; }
void net_rx_schedule(void) {}
void vmm_map_range(uint64_t a,uint64_t b,uint64_t c,uint64_t d) { (void)a;(void)b;(void)c;(void)d; }
void text_draw_sz(int x,int y,const char *s,int px,uint32_t c) { (void)x;(void)y;(void)s;(void)px;(void)c; }
int text_width_sz(const char *s,int px) { (void)s;(void)px;return 0; }
int text_line_height(int px) { return px; }
struct blkdev *blk_find(const char *name) { (void)name;return (struct blkdev *)&pci_dev_model; }
void blk_dev_offline(struct blkdev *dev) {
    (void)dev;blk_offlined++;
    if(expect_live_blk)CHECK(blkdev.started&&!blkdev.failed,"block admission fenced before live transport reset");
}
uint64_t pmm_alloc_contig_masked(size_t n,uint64_t mask,size_t align,size_t boundary)
{
    (void)boundary;
    if (align < 4096) align = 4096;
    uint64_t p = (next_phys + align - 1) & ~(uint64_t)(align - 1);
    if (!n || p + n*4096 > HIGH + ARENA || p + n*4096 - 1 > mask) return 0;
    next_phys = p + n*4096;
    for(size_t i=0;i<n;i++) refs[pi(p)+i]=1;
    live += n; return p;
}
void pmm_free(uint64_t p) { if(p<HIGH || p>=HIGH+ARENA || !refs[pi(p)]) { unexpected_free++; return; } if (!--refs[pi(p)]) live--; }
int pmm_ref(uint64_t p) { if(p<HIGH||p>=HIGH+ARENA||!refs[pi(p)])return -1;refs[pi(p)]++;return 0; }
unsigned pmm_refcount(uint64_t p) { return p>=HIGH&&p<HIGH+ARENA?refs[pi(p)]:0; }
void pmm_pin(uint64_t p) { pins[pi(p)]++; }
void pmm_unpin(uint64_t p) { pins[pi(p)]--; }
unsigned pmm_pincount(uint64_t p) { return pins[pi(p)]; }
int pmm_is_ram(uint64_t p,size_t n) { return p>=HIGH&&p<HIGH+ARENA&&n<=HIGH+ARENA-p; }
uint64_t pmm_free_frames(void) { return ARENA/4096-live; }
int pmm_physmap_ready(void) { return 1; }
uint64_t dma_host_resolve(void *p) { uint64_t va=(uint64_t)(uintptr_t)p;return va>=mm_host_base?va-mm_host_base:UINT64_MAX; }
struct device *dev_find_id(uint16_t v,uint16_t id,struct device *from)
{
    (void)v;(void)from; memset(pci_mmio,0,sizeof pci_mmio);
    pci_dev_model.device=id; pci_dev_model.res[0].size=sizeof pci_mmio;
    memcpy(pci_dev_model.name,"virtio-test",12);
    *(uint16_t*)(pci_mmio+C_QSIZE)=256;
    *(uint32_t*)(pci_mmio+C_DEVFEAT)=UINT32_MAX;
    return &pci_dev_model;
}
void dev_enable(struct device *d,int b) { (void)d;(void)b; }
int dev_enable_checked(struct device *d,int b)
{ if(command_reject)return -1;dev_enable(d,b);return 0; }
uint64_t dev_bar_map(struct device *d,int b)
{ bar_maps++;return b==0?(uint64_t)(uintptr_t)(d==&pci_second_model?pci_second_mmio:pci_mmio):0; }
uint8_t pci_cap_next(uint8_t b,uint8_t s,uint8_t f,uint8_t id,uint8_t prev)
{ (void)b;(void)s;(void)f;(void)id;return prev==0?0x40:prev==0x40?0x50:prev==0x50?0x60:0; }
uint32_t pci_cfg_read(uint8_t b,uint8_t s,uint8_t f,uint16_t off)
{
    (void)b;(void)s;(void)f;
    if(off==0x40)return 1u<<24;if(off==0x50)return 2u<<24;if(off==0x60)return 4u<<24;
    if(off==0x48)return 0;if(off==0x58)return 0x100;if(off==0x68)return 0x200;
    if(off==0x4c)return 64;if(off==0x5c)return 16;if(off==0x6c)return 256;
    return 0;
}
uint8_t virtio_test_r8(volatile uint8_t *b,int o) { return b[o]; }
void virtio_test_w8(volatile uint8_t *b,int o,uint8_t v) { if(o==C_STATUS&&v==0&&reset_fail)return;b[o]=v; }
static void *device_ptr(uint64_t p,size_t n)
{ if(!pmm_is_ram(p,n)) {io_faults++;return NULL;}return mm_physmap_ptr(p); }
void virtio_test_poll(struct virtio_dev *vd,struct virtq *vq)
{
    if(timeout_all)return;
    uint16_t head=vq->avail->ring[(uint16_t)(vq->avail->idx-1)%vq->size];
    struct virtq_desc *d=&vq->desc[head];
    uint8_t *cmd=device_ptr(d->addr,d->len);if(!cmd)return;
    uint32_t len=0;
    if(vd==&gpudev) {
        uint32_t type=*(uint32_t*)cmd;
        if(type==timeout_gpu_cmd)return;
        if(type==GPU_CMD_RESOURCE_ATTACH_BACKING) {
            struct gpu_attach_backing *a=(void*)cmd;
            CHECK(a->addr>=HIGH && a->addr<HIGH+ARENA,"GPU nested backing is high guest physical");
            CHECK(device_ptr(a->addr,a->length)!=NULL,"GPU backing DMA range resolves");
            attach_seen++;
        }
        if(d->flags&VIRTQ_DESC_F_NEXT) {
            struct virtq_desc *r=&vq->desc[d->next];
            struct gpu_hdr *h=device_ptr(r->addr,r->len);if(!h)return;
            h->type=type==GPU_CMD_GET_DISPLAY_INFO?GPU_RESP_OK_DISPLAY_INFO:GPU_RESP_OK_NODATA;
            len=sizeof *h;
            if(type==GPU_CMD_GET_DISPLAY_INFO) {
                CHECK(r->len>=sizeof(struct gpu_disp_info),"GPU response covers full display info");
                struct gpu_disp_info *di=(void*)h;
                di->pmodes[0].enabled=1;di->pmodes[0].r.width=1280;di->pmodes[0].r.height=800;
                len=sizeof *di;
            }
        }
    } else if(vd==&rngdev) { for(uint32_t i=0;i<d->len;i++)cmd[i]=(uint8_t)(++rng_seq);len=d->len; }
    else if(vd==&bdev) {
        uint32_t *pfns=(void*)cmd;
        for(uint32_t i=0;i<d->len/4;i++) {
            uint32_t base=vq==&inflate_vq?held_n:held_n-d->len/4;
            CHECK(((uint64_t)pfns[i]<<12)==held[base+i],"balloon payload equals held guest physical PFN");
            CHECK(((uint64_t)pfns[i]<<12)>=HIGH,"balloon PFNs retain physical bits above 4G");
        }
    } else if(vd==&blkdev) {
        struct blk_req_hdr *h=(void*)cmd;
        struct virtq_desc *payload=&vq->desc[d->next];
        struct virtq_desc *status=payload;
        if(h->type!=VIRTIO_BLK_T_FLUSH) {
            uint8_t *p=device_ptr(payload->addr,payload->len);if(!p)return;
            CHECK(payload->addr>=HIGH,"blk payload uses high physical DMA");
            if(h->type==VIRTIO_BLK_T_IN) { memset(p,0x5a,payload->len);len=payload->len; }
            else CHECK(p[0]==0x33,"blk DMA_TO_DEVICE sees caller contents");
            status=&vq->desc[payload->next];
        }
        uint8_t *st=device_ptr(status->addr,1);if(!st)return;*st=0;len++;
    }
    vq->used->ring[vq->used->idx%vq->size]=(struct virtq_used_elem){head,len};
    vq->used->idx++;
}
static void clear_model(void) { timeout_all=reset_fail=0;timeout_gpu_cmd=0; }
static void test_pci_command_gate(void)
{
    struct virtio_dev d={0};
    command_reject=1;bar_maps=0;
    CHECK(virtio_init(0x1052,&d,0)<0,"PCI Command failure rejects virtio transport");
    CHECK(bar_maps==0,"PCI Command failure blocks BAR mapping");
    command_reject=0;
}
/* A keyboard and mouse have the same ID but independent MMIO/reset domains.
 * The negative control restores an ID search inside the exact-device API;
 * it must alter the first device and fail these checks, without needing DMA. */
static void test_transport_device_selection(void)
{
    struct virtio_dev first={0},second={0};
    CHECK(virtio_init(0x1052,&first,0)==0,"first input transport initialize");
    pci_second_model=pci_dev_model;pci_second_model.slot=4;
    memcpy(pci_second_mmio,pci_mmio,sizeof pci_mmio);
    pci_mmio[C_STATUS]=VIRTIO_S_DRIVER_OK;
    CHECK(virtio_init_device(&pci_second_model,&second,0)==0,"second input transport initialize");
    CHECK(second.dev==&pci_second_model&&second.slot==4,"explicit transport retains selected PCI function");
    CHECK(second.common!=first.common,"same-ID devices have separate MMIO domains");
    CHECK(pci_mmio[C_STATUS]==VIRTIO_S_DRIVER_OK,"second probe does not reset first input device");
    CHECK(virtio_init_device(NULL,&second,0)<0,"missing selected device is refused");
    CHECK(virtio_init_device(&pci_second_model,NULL,0)<0,"missing transport storage is refused");
}
static void test_transport(void)
{
    struct virtio_dev d={0};struct virtq q={0};
    CHECK(virtio_init(0x1001,&d,0)==0,"transport initialize");
    CHECK(virtio_queue_setup(&d,0,&q)==0,"queue coherent allocate");
    CHECK(*(uint64_t*)(pci_mmio+C_QDESC)==q.desc_mem->phys,"queue register uses physical, not CPU alias");
    CHECK((uint64_t)(uintptr_t)q.desc!=q.desc_mem->phys,"test apparatus CPU differs from physical");
    virtio_driver_ok(&d);
    struct dma_buffer *b=dma_alloc_coherent(&d.dma,64,64,0);
    struct virtio_buf sg={b->dma,64,0,b};
    timeout_all=1;reset_fail=1;
    CHECK(virtio_request(&d,&q,0,&sg,1)<0,"request timeout surfaced");
    CHECK(d.failed&&!d.quiesced&&b->state==DMA_QUARANTINED,"failed reset quarantines submitted payload");
    int freed=dma_free_coherent(b);
    CHECK(freed<0,"cannot free quarantined payload");
    /* Keep the negative control diagnosable: if broken reset logic allowed the
     * free, do not turn that assertion into a secondary use-after-free crash. */
    if (!freed) { b=NULL; sg.owner=NULL; }
    unsigned avail=q.avail->idx;
    CHECK(virtio_request(&d,&q,0,&sg,1)<0&&q.avail->idx==avail,"failed device rejects resubmission");
    virtio_queue_release(&d,&q);CHECK(q.desc!=NULL,"failed reset retains rings");
    reset_fail=0;CHECK(!virtio_stop(&d),"acknowledged reset permits reclamation");
    CHECK(!dma_free_coherent(b),"quiesced payload free");virtio_queue_release(&d,&q);clear_model();
}
static void test_gpu(void)
{
    CHECK(fb_init(0)==1,"GPU initializes through framebuffer consumer and real transport");
    CHECK(attach_seen==2,"framebuffer and cursor backing both attached");
    CHECK(fb_mem->state==DMA_DEVICE_OWNED&&cursor_mem->state==DMA_DEVICE_OWNED,"GPU backing remains owned after ATTACH completion");
    CHECK(virtio_gpu_fb()==fb_mem->cpu,"fb consumer borrows CPU alias");
    uint32_t cursor[4]={~0u,~0u,~0u,~0u};
    CHECK(!virtio_gpu_cursor_define(cursor,2,2,0,0),"cursor accepts zero-byte used completion");
    virtio_gpu_flush(0,0,32,32);
    CHECK(gpu_driver.probe(&pci_dev_model)==0,"existing early GPU binds normal remove callback");
    gpu_driver.remove(&pci_dev_model);
    CHECK(!virtio_gpu_present()&&!fb_cursor_hw(),"GPU normal remove revokes display availability");
    fb_fb_put(0,0,1); /* ASan catches a retained pointer after backing free */
    CHECK(!gpudev.dma.buffers,"GPU reset releases all coherent owners");
    memset(&gpudev,0,sizeof gpudev);memset(&gpuvq,0,sizeof gpuvq);memset(&gpucurvq,0,sizeof gpucurvq);
    timeout_gpu_cmd=GPU_CMD_RESOURCE_ATTACH_BACKING;reset_fail=1;
    CHECK(virtio_gpu_init()<0,"GPU failed ATTACH cannot initialize");
    CHECK(fb_mem&&fb_mem->state==DMA_QUARANTINED,"unacknowledged GPU backing retained on init unwind");
    clear_model();gpu_release();CHECK(!gpudev.dma.buffers,"GPU later confirmed reset reclaims quarantine");
    memset(&gpudev,0,sizeof gpudev);memset(&gpuvq,0,sizeof gpuvq);memset(&gpucurvq,0,sizeof gpucurvq);
    CHECK(fb_init(0)==1,"GPU re-created for borrower drain test");
    uint32_t *writer=fb_dma_test_hold_writer();
    CHECK(writer==gpu_fb,"AP writer holds the actual borrowed framebuffer");
    int drain_rc=virtio_gpu_shutdown();
    CHECK(drain_rc<0,"stalled CPU writer prevents normal free");
    CHECK(fb_mem&&fb_mem->state==DMA_QUARANTINED,"CPU drain timeout quarantines GPU backing");
    fb_fb_put(0,0,0x55);
    CHECK(fb_mem&&writer[0]==0&&!fb_cursor_hw(),"revoked pointer prevents new CPU writes and cursor commands");
    fb_dma_test_release_writer();
    CHECK(!virtio_gpu_shutdown()&&!gpudev.dma.buffers,"retired AP plus acknowledged reset permits normal reclamation");
    fb_fb_put(0,0,1);
    memset(&gpudev,0,sizeof gpudev);memset(&gpuvq,0,sizeof gpuvq);memset(&gpucurvq,0,sizeof gpucurvq);
    CHECK(fb_init(0)==1,"GPU re-created for normal remove reset failure");
    uint32_t *retained=gpu_fb;reset_fail=1;
    gpu_driver.remove(&pci_dev_model);
    CHECK(fb_mem&&fb_mem->state==DMA_QUARANTINED,"normal GPU remove retains backing until reset ACK");
    fb_fb_put(0,0,0x66);
    CHECK(fb_mem&&retained[0]==0&&!fb_cursor_hw(),"failed device reset still revokes CPU framebuffer borrowing");
    clear_model();CHECK(!virtio_gpu_shutdown()&&!gpudev.dma.buffers,"normal remove quarantine reclaimed after later reset ACK");
}
static void test_rng_balloon(void)
{
    CHECK(!rng_probe(&pci_dev_model),"RNG coherent probe");uint8_t r[160];
    CHECK(virtio_rng_get(r,sizeof r)==sizeof r,"RNG staged multi-request read");
    rng_remove(&pci_dev_model);CHECK(!rngdev.dma.buffers,"RNG stop frees queue and stage");
    CHECK(!balloon_probe(&pci_dev_model),"balloon coherent probe and self-test");
    uint64_t before=live;
    CHECK(balloon_inflate(7)==7&&live==before+7,"balloon ANY pages donated");
    CHECK(balloon_deflate(7)==7&&live==before,"balloon ACK returns exact physical pages");
    timeout_all=reset_fail=1;
    CHECK(balloon_inflate(3)==0,"balloon inflate timeout reported");
    CHECK(pending_n==3&&live==before+3,"unacknowledged donations remain allocated");
    CHECK(stage_mem->state==DMA_QUARANTINED,"balloon PFN stage quarantined too");
    clear_model();balloon_remove(&pci_dev_model);
    CHECK(held_n==0&&pending_n==0&&!bdev.dma.buffers,"balloon reset resolves uncertain donations and stage");
}
static void test_blk(void)
{
    CHECK(!virtio_blk_init(),"blk coherent initialize");
    uint64_t p=pmm_alloc_contig_masked(2,DMA_MASK_64,4096,0);uint8_t *cpu=mm_physmap_ptr(p);
    CHECK(!virtio_blk_read(0,16,cpu)&&cpu[0]==0x5a&&cpu[8191]==0x5a,"blk high alias read roundtrip");
    struct dma_stats before_write,after_write;dma_get_stats(&before_write);
    memset(cpu,0x33,8192);CHECK(!virtio_blk_write(0,16,cpu),"blk high alias write");
    dma_get_stats(&after_write);
    CHECK(after_write.completed_high_bytes-before_write.completed_high_bytes==8192,"successful high write counted without TO_DEVICE copyback");
    CHECK(!virtio_blk_flush(),"blk coherent flush controls");
    CHECK(blk_driver.probe(&pci_dev_model)==0,"existing root disk binds normal remove callback");
    expect_live_blk=1;blk_driver.remove(&pci_dev_model);expect_live_blk=0;
    CHECK(!blkdev.dma.buffers&&!blkdev.dma.mappings&&!virtio_blk_present(),"normal blk remove releases backing after admission fence");
    memset(&blkdev,0,sizeof blkdev);memset(&blkvq,0,sizeof blkvq);
    CHECK(!virtio_blk_init(),"blk re-created for failed-reset ownership test");
    timeout_all=reset_fail=1;expect_panic=1;
    if(!setjmp(panic_jmp)) { virtio_blk_read(0,16,cpu);CHECK(0,"unsafe direct DMA must not return caller ownership"); }
    else CHECK(panics==1&&blk_payload&&blk_payload->state==DMA_QUARANTINED,"reset failure fail-stops with mapping retained");
    expect_panic=0;clear_model();blk_driver.remove(&pci_dev_model);
    CHECK(blk_offlined==2,"blk remove fences block admission before transport release");
    pmm_free(p);pmm_free(p+4096);CHECK(!blkdev.dma.buffers&&!blkdev.dma.mappings,"blk stop frees controls and mapping");
}
static void got_frame(const uint8_t *p,uint16_t len) { CHECK(len==64&&((const uint8_t*)p)[0]==0x7c,"RX callback sees completed CPU backing");rx_seen++; }
static void test_net(void)
{
    pci_dev_model.device=0x1000;
    CHECK(!virtio_net_probe(&pci_dev_model),"net coherent probe");
    CHECK(rxq.desc[0].addr==rx_mem[0]->phys,"net direct descriptor path uses DMA address");
    uint8_t frame[64];memset(frame,0x77,sizeof frame);
    CHECK(!vnet_tx(frame,sizeof frame),"net TX publishes coherent buffer");
    CHECK(tx_mem[0]->state==DMA_DEVICE_OWNED&&!tx_free[0],"net TX not freed at enqueue return");
    txq.used->ring[0]=(struct virtq_used_elem){0,0};txq.used->idx=1;tx_reclaim();
    CHECK(tx_mem[0]->state==DMA_COMPLETED&&tx_free[0],"net TX reclaimed only after used ACK");
    memset(rx_buf[0]+VNET_HDR_LEN,0x7c,64);
    rxq.used->ring[0]=(struct virtq_used_elem){0,VNET_HDR_LEN+64};rxq.used->idx=1;
    CHECK(vnet_rx_poll(got_frame)==1&&rx_seen==1,"RX used entry consumed once");
    CHECK(rx_mem[0]->state==DMA_DEVICE_OWNED,"RX backing reposted after callback");
    virtio_net_remove(&pci_dev_model);CHECK(!vnet.dma.buffers,"NIC removal waits reset then frees persistent DMA");
}
int main(void)
{
    void *arena=mmap(NULL,HIGH+ARENA,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(arena==MAP_FAILED){perror("DMA virtio sparse arena");return 2;}mm_host_base=(uint64_t)(uintptr_t)arena;
    test_pci_command_gate();test_transport_device_selection();test_transport();test_gpu();test_rng_balloon();test_blk();test_net();
    CHECK(io_faults==0,"all device addresses resolve independently of CPU aliases");
    CHECK(unexpected_free==0,"all frees use allocated physical ownership");
    CHECK(live==0,"all acknowledged devices release backing without leaks");
    struct dma_stats stats;dma_get_stats(&stats);
    CHECK(!stats.coherent_buffers&&!stats.active_mappings&&!stats.quarantined_buffers,"DMA core lifecycle accounting balanced");
    printf("dma-virtio: %d checks, %d failed\n",tests,fails);
    munmap(arena,HIGH+ARENA);return fails?1:0;
}
