/* Compile the actual driver after replacing only volatile register loads/stores
 * with a bounded hardware model. This tests allocation/address/teardown wiring,
 * not hardware fidelity or packet/audio/USB performance. No pointer equals DMA. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include "dma.h"
#include "driver.h"
unsigned char test_regs[65536] __attribute__((aligned(4096)));
static int stuck, fail_at = -1, allocations, live, failures, checks;
static int command_reject;
static uint64_t last_dma_mask;
static unsigned published;
uint64_t test_read(const volatile void *p, unsigned n);
void test_write(volatile void *p, unsigned n, uint64_t v);
#undef htons
#undef htonl
#undef ntohs
#undef ntohl
#undef memset
#undef memcpy
#include "dma_driver.inc"
static void check(int yes, const char *why) {
    checks++; if (!yes) { failures++; printf("FAIL %s\n",why); }
}
uint64_t test_read(const volatile void *p, unsigned n) {
    uint64_t v=0; memcpy(&v,(const void*)p,n); return v;
}
void test_write(volatile void *p, unsigned n, uint64_t v) {
    size_t o=(const unsigned char*)p-test_regs;
#if DRIVER_KIND == 1
    if(o==0 && (v & (1u<<26)) && !stuck) v &= ~(1u<<26);
#elif DRIVER_KIND == 2 || DRIVER_KIND == 3
    if(o==0x37 && (v&0x10) && !stuck) v &= ~0x10u;
#elif DRIVER_KIND == 4
    if(o==0x2000 && g_xhci.cmd_want && stuck!=2) {
        struct trb *ev=&g_xhci.ev.trb[g_xhci.ev.deq];
        ev->param=g_xhci.cmd_want;
        ev->status=CC_SUCCESS<<24;
        ev->control=TRB_SET_TYPE(TRB_CMD_COMPLETION) | (1u<<24) |
                    (g_xhci.ev.cycle ? TRB_C : 0);
    }
    if(o==0x40) {
        if(v&2) v=0;
        *(uint32_t*)(test_regs+0x44) = (stuck && published) ? 0 : ((v&1) ? 0 : 1);
    }
#elif DRIVER_KIND == 5
    if(stuck && (o==8 || o==0x80 || o==0xa0)) return;
#endif
    memcpy((void*)p,&v,n);
}
void dma_device_init(struct dma_device *d,const char *name,uint64_t mask) {
    memset(d,0,sizeof *d); d->name=name; d->mask=last_dma_mask=mask;
}
struct dma_buffer *dma_alloc_coherent(struct dma_device *d,size_t n,size_t a,size_t bound) {
    (void)a;(void)bound;
    if (d->blocked || allocations++==fail_at) return NULL;
    struct dma_buffer *b=calloc(1,sizeof *b);
    b->size=(n+4095)&~4095UL; b->pages=b->size/4096;
    b->cpu=aligned_alloc(4096,b->size); memset(b->cpu,0,b->size);
    b->dma.value=(d->mask==DMA_MASK_32?0x200000u:UINT64_C(0x1230000000)) + (uint64_t)allocations*0x10000;
    b->phys=b->dma.value; b->dev=d; b->state=DMA_READY;
    b->next=d->buffers; d->buffers=b; live++; return b;
}
int dma_free_coherent(struct dma_buffer *b) {
    if(b->state==DMA_DEVICE_OWNED || b->state==DMA_QUARANTINED) { check(0,"free owned DMA"); return -1; }
    struct dma_buffer **p=&b->dev->buffers; while(*p!=b)p=&(*p)->next;
    *p=b->next; free(b->cpu);free(b);live--;return 0;
}
uint64_t dma_buffer_submit(struct dma_buffer *b) { b->state=DMA_DEVICE_OWNED;published++;return ++b->token; }
int dma_buffer_complete(struct dma_buffer *b,uint64_t token) {
    if(b->state!=DMA_DEVICE_OWNED || token!=b->token)return -1;
    b->state=DMA_COMPLETED;return 0;
}
void dma_device_quiesced(struct dma_device *d) {
    for(struct dma_buffer*b=d->buffers;b;b=b->next)b->state=DMA_QUIESCED;
}
void dma_device_quarantine(struct dma_device *d) {
    d->blocked=1;for(struct dma_buffer*b=d->buffers;b;b=b->next)b->state=DMA_QUARANTINED;
}
void dma_wmb(void) {} void dma_rmb(void) {}
void kprintf(const char *f,...) {(void)f;}
uint64_t timer_ms(void) {static uint64_t t;return ++t;}
#if DRIVER_KIND == 4
/* xHCI owns PCI Command directly so the DMA test can observe the same
 * MEM-before-BME transaction as the lifecycle gate. */
static uint16_t xhci_test_pci_command;
uint16_t pci_cfg_read16(uint8_t b,uint8_t s,uint8_t f,uint16_t off)
{ (void)b;(void)s;(void)f;return off==PCI_CFG_COMMAND?xhci_test_pci_command:UINT16_MAX; }
void pci_cfg_write16(uint8_t b,uint8_t s,uint8_t f,uint16_t off,uint16_t value)
{ (void)b;(void)s;(void)f;if(off==PCI_CFG_COMMAND)xhci_test_pci_command=value; }
#elif DRIVER_KIND == 5
/* HDA reaches PCI Command through dev_enable/dev_disable.  Keep this model
 * local to the HDA adapter so its MEM-only and later BME readbacks are real. */
static uint16_t hda_test_pci_command;
uint16_t pci_cfg_read16(uint8_t b,uint8_t s,uint8_t f,uint16_t off)
{ (void)b;(void)s;(void)f;return off==PCI_CFG_COMMAND?hda_test_pci_command:UINT16_MAX; }
#endif
void dev_enable(struct device *d,int master) {
    (void)d;
#if DRIVER_KIND == 5
    hda_test_pci_command|=PCI_CMD_IO|PCI_CMD_MEM;
    if(master)hda_test_pci_command|=PCI_CMD_MASTER;
#else
    (void)master;
#endif
}
int dev_enable_checked(struct device *d,int master) {
    if(command_reject)return -1;
    dev_enable(d,master);return 0;
}
void dev_disable(struct device *d) {
    (void)d;
#if DRIVER_KIND == 5
    hda_test_pci_command&=(uint16_t)~(PCI_CMD_IO|PCI_CMD_MEM|PCI_CMD_MASTER);
#endif
}
int dev_disable_checked(struct device *d) { dev_disable(d); return 0; }
uint64_t dev_bar_map(struct device*d,int bar) {(void)d;(void)bar;return (uintptr_t)test_regs;}
uint64_t net_lock(void) {return 0;} void net_unlock(uint64_t x) {(void)x;}
static unsigned rx_schedules;
void net_rx_schedule(void) { rx_schedules++; }
#if DRIVER_KIND == 2
static void receive_unused(const uint8_t *frame, uint16_t len) {(void)frame;(void)len;}
#endif
void *kmalloc(size_t n) {return calloc(1,n);} void kfree(void*p) {free(p);}
#if DRIVER_KIND == 5
int dev_irq_request(struct device*d,void (*f)(void *),void*a,const char*n) {(void)d;(void)f;(void)a;(void)n;return 1;}
void snd_period_elapsed(struct snd_device*d) {(void)d;}
void snd_cap_period_elapsed(struct snd_capdevice*d) {(void)d;}
int snd_register_device(struct snd_device*d) {(void)d;return 0;}
int snd_register_capture_device(struct snd_capdevice*d) {(void)d;return 0;}
static int output_detached, input_detached, irq_detached;
void snd_unregister_device(struct snd_device*d) {(void)d;output_detached=1;}
void snd_unregister_capture_device(struct snd_capdevice*d) {(void)d;input_detached=1;}
int dev_irq_release(struct device*d) {(void)d;irq_detached=1;return 0;}
void snd_init(void) {}
uint64_t time_mono_ns(void) {static uint64_t t;return t+=100000;}
#endif
static struct device device;
int dev_count(void) { return 1; }
struct device *dev_at(int i) { return i == 0 ? &device : NULL; }
static void cleanup(struct dma_device *d) {dma_device_quiesced(d);while(d->buffers)dma_free_coherent(d->buffers);}
int main(int argc,char **argv) {
    int mode=argc>1?atoi(argv[1]):0;
    command_reject=argc>2&&!strcmp(argv[2],"command-reject");
    device.res[0].flags=DEV_RES_IO;device.res[0].start=0x1000;
#if DRIVER_KIND <= 3
#if DRIVER_KIND == 1
#define PROBE e1000_probe
#define REMOVE e1000_remove
#else
#if DRIVER_KIND == 2
#define PROBE rtl8139_probe
#define REMOVE rtl8139_remove
#else
#define PROBE rtl8169_probe
#define REMOVE rtl8169_remove
    device.res[0].flags=DEV_RES_MEM;
    device.vendor=0x10ec;device.device=0x8168;device.cap_pcie=0x40;
    *(uint32_t*)(test_regs+R_TCR)=0x3c8u<<20;
    if(mode==52) *(uint32_t*)(test_regs+R_TCR)=0x380u<<20; /* older 8168B */
    if(mode==53) {device.device=0x8169;device.cap_pcie=0;} /* conventional PCI */
    if(mode==54) *(uint32_t*)(test_regs+R_TCR)=0x7c0u<<20; /* unknown MAC */
    if(mode==55) device.device=0x8136; /* 8101 stays conservatively low */
#endif
#endif
    int allocation_failure=mode>=2;
#if DRIVER_KIND == 3
    allocation_failure=mode>=2 && mode<52;
#endif
    if(allocation_failure) fail_at=mode-2;
    int rc=PROBE(&device);
    if(command_reject) {
        check(rc<0,"probe rejects PCI Command failure");
        check(allocations==0&&published==0&&live==0,
              "PCI Command failure precedes DMA allocation and publication");
    }
    else if(allocation_failure) {check(rc<0,"probe allocation failure");check(live==0,"partial allocation cleanup");}
    else {
        check(rc==0,"probe succeeds");check(live>0 && published>0,"persistent buffers submitted");
#if DRIVER_KIND == 1
        uint64_t rb=test_read(test_regs+REG_RDBAL,4)|(test_read(test_regs+REG_RDBAH,4)<<32);
        check(rb==rx_dma->dma.value,"e1000 RX base uses device address");
        check(rx_ring[0].addr!= (uintptr_t)rx_buf[0] && rx_ring[0].addr>UINT32_MAX,"e1000 payload uses high DMA");
        unsigned char f[64]={1};check(e1000_tx_frame(f,64)==0 && tx_buf[0][0]==1,"e1000 CPU buffer write");
#elif DRIVER_KIND == 2
        check(test_read(test_regs+R_RBSTART,4)==rx_dma->dma.value,"8139 32-bit RX device address");
        check(nic_dma.mask==DMA_MASK_32 && (uintptr_t)rxbuf!=rx_dma->dma.value,"8139 CPU mapping differs");
        *(uint32_t*)(test_regs+R_TSD0)=TSD_OWN;
        unsigned char f[64]={1}; check(rtl_tx(f,64)==0,"8139 TX enqueue");
        check(test_read(test_regs+R_TSAD0,4)==tx_dma[0]->dma.value,"8139 TX device address");
        rtl_irq_on(receive_unused);
        rx_off=64;
        *(uint16_t*)(test_regs+R_CAPR)=rtl8139_capr(rx_off);
        *(uint16_t*)(test_regs+R_ISR)=ISR_ROK|ISR_RXOVW;
        unsigned schedules_before=rx_schedules;
        rtl_isr();
        check(rx_off==64 && test_read(test_regs+R_CAPR,2)==rtl8139_capr(64),
              "8139 overflow preserves queued ring consumer position");
        check(rx_schedules==schedules_before+1,"8139 overflow schedules actual RX handler");
        *(uint16_t*)(test_regs+R_ISR)=0;
        rtl_isr();
        *(uint16_t*)(test_regs+R_ISR)=UINT16_MAX;
        rtl_isr();
        check(rx_schedules==schedules_before+1,"8139 shared empty or removed IRQ does not schedule RX");
#else
        check(test_read(test_regs+R_RDSAR,8)==rx_dma->dma.value,"8169 RX device address");
        check((mode>=52 ? rxd[0].addr<=UINT32_MAX : rxd[0].addr>UINT32_MAX) &&
              rxd[0].addr!=(uintptr_t)rxbuf[0],"8169 variant DMA mask and payload address");
        check(nic_dma.mask==(mode>=52 ? DMA_MASK_32 : DMA_MASK_64),"8169 known MAC capability selection");
        check(!(test_read(test_regs+R_CPCMD,2)&(1u<<4)),"8169 does not enable legacy PCI DAC");
        unsigned char f[64]={1};check(rtl_tx(f,64)==0,"8169 TX enqueue");
        check(txd[0].addr==tx_payload[0]->dma.value,"8169 TX device address");
#endif
        int before=live;stuck=mode==1;REMOVE(&device);
        check(stuck ? (live==before && nic_dma.blocked) : live==0,"reset ack gates DMA release");
        if(stuck)cleanup(&nic_dma);
    }
#elif DRIVER_KIND == 4
    *(uint32_t*)(test_regs+XCAP_CAPLENGTH)=0x40;
    *(uint32_t*)(test_regs+XCAP_HCSPARAMS1)=8;
    *(uint32_t*)(test_regs+XCAP_HCCPARAMS1)=mode==1?0:1;
    *(uint32_t*)(test_regs+XCAP_RTSOFF)=0x1000;
    *(uint32_t*)(test_regs+XCAP_DBOFF)=0x2000;
    *(uint32_t*)(test_regs+0x44)=1;
    if(mode>=2) fail_at=mode==5?2:mode-2;
    stuck=mode==5;
    int rc=xhci_init(&device);
    if(mode>=2) {
        check(rc<0,"xhci allocation failure");
        if(stuck) {
            check(live==2 && g_xhci.dma.blocked,"xhci unconfirmed halt quarantines");
            cleanup(&g_xhci.dma);
        } else check(live==0,"xhci halt cleanup");
    }
    else {
        check(rc==0,"xhci starts");
        check(g_xhci.cmd.dma_base!=(uintptr_t)g_xhci.cmd.trb &&
              g_xhci.ev.dma_base!=(uintptr_t)g_xhci.ev.trb,"xhci rings must not expose CPU pointers");
        check(g_xhci.dma.mask==(mode==1?DMA_MASK_32:DMA_MASK_64),"xhci AC64 negotiation");
        check(test_read(test_regs+0x40+XOP_DCBAAP,8)==dma_address(g_xhci.dcbaa),"DCBAA device address");
        check(test_read(test_regs+0x40+XOP_CRCR,8)==g_xhci.cmd.dma_base+1,"command ring device address");
        check(*(uint64_t*)g_xhci.erst==g_xhci.ev.dma_base,"ERST device address");
        struct xhci_ep *ep=ep_alloc(&g_xhci,1,3);int before=allocations;
        struct usb_device d={0};d.slot=1;
        check(xhci_int_in_arm(&d,0x81)==0,"HID rearm");
        check(before==allocations,"IRQ rearm allocates nothing");
        check(ep->ring.trb[0].param==ep->buf_dma,"HID TRB device buffer");
        g_xhci.slot[1].used=1;
        int baseline=live-2;
        xhci_free_slot(1);
        check(live==baseline && !g_xhci.slot[1].used,"Disable Slot releases endpoint DMA");
        d.speed=XSPEED_HIGH;
        for(int lap=0;lap<4;lap++) {
            g_xhci.slot[1].used=1;
            check(xhci_address_device(&d,0)==0,"slot contexts and EP0 allocated");
            check(live==baseline+4,"slot owns exactly its context/ring/payload pages");
            xhci_free_slot(1);
            check(live==baseline && !g_xhci.slot[1].ep[1],"repeated slot free returns to baseline");
        }
        g_xhci.slot[1].used=1;
        check(xhci_address_device(&d,0)==0,"allocate slot before failed disable");
        int retained=live;stuck=2;
        xhci_free_slot(1);
        check(live==retained && g_xhci.dma.blocked && g_xhci.slot[1].used,
              "unconfirmed Disable Slot retains every page");
        check(xhci_shutdown()<0 && live==retained,"controller halt failure retains all DMA");
        stuck=0;
        check(xhci_shutdown()==0 && live==0 && !g_xhci.up,"controller halt acknowledgement frees ledger");
    }
#elif DRIVER_KIND == 5
    device.bus_type=DEV_BUS_PCI;device.vendor=0x8086;device.device=0x2668;
    device.class_code=0x04;device.subclass=0x03;
    if(mode>=3) {
        *(uint16_t*)(test_regs+GCAP)=0x1100u | (mode==3?1:0);
        *(uint16_t*)(test_regs+STATESTS)=1;
        fail_at=mode-3;
        check(hda_probe(&device)<0,"hda CORB allocation failure");
        check(last_dma_mask==(mode==3?DMA_MASK_64:DMA_MASK_32),"hda GCAP 64OK negotiation");
        check(live==0,"hda CORB partial allocation cleanup");
        printf("driver %d mode %d: %d checks, %d failures\n",DRIVER_KIND,mode,checks,failures);
        return failures?1:0;
    }
    struct hda *h=&g_hda;h->mmio=test_regs;h->out_base=0xa0;h->in_base=0x80;
    dma_device_init(&h->dma,"hda",mode==1?DMA_MASK_32:DMA_MASK_64);
    h->ring_dma=dma_alloc_coherent(&h->dma,HDA_RING_BYTES,4096,0);
    h->bdl_dma=dma_alloc_coherent(&h->dma,4096,4096,0);
    h->cap_ring_dma=dma_alloc_coherent(&h->dma,HDA_RING_BYTES,4096,0);
    h->cap_bdl_dma=dma_alloc_coherent(&h->dma,4096,4096,0);
    h->ring=h->ring_dma->cpu;h->bdl=h->bdl_dma->cpu;
    h->cap_ring=h->cap_ring_dma->cpu;h->cap_bdl=h->cap_bdl_dma->cpu;
    h->snd.priv=h;h->cap.priv=h;
    check(hda_start(&h->snd)==0,"playback starts");
    check(hda_cap_start(&h->cap)==0,"capture starts");
    check(test_read(test_regs+0xa0+SD_BDPL,8)==h->bdl_dma->dma.value,"playback BDL DMA");
    check(h->bdl[0].addr==h->ring_dma->dma.value,"playback PCM DMA");
    check(test_read(test_regs+0x80+SD_BDPL,8)==h->cap_bdl_dma->dma.value,"capture BDL DMA");
    check(h->cap_bdl[1].addr==h->cap_ring_dma->dma.value+HDA_PERIOD_BYTES,"capture PCM offset DMA");
    stuck=mode==2;int before=live;hda_stop(&h->snd);hda_cap_stop(&h->cap);
    check(live==before,"stream stop retains reusable buffers");
    check(!stuck || h->dma.blocked,"unacknowledged stream stop quarantines");
    if(!stuck) {
        check(h->ring_dma->state==DMA_COMPLETED && h->cap_ring_dma->state==DMA_COMPLETED,
              "stop completes only stream ownership");
        uint64_t old=h->cap_ring_dma->token;
        check(hda_cap_start(&h->cap)==0 && h->cap_ring_dma->token>old,
              "capture restart submits a fresh token");
        hda_cap_stop(&h->cap);
    }
    *(uint32_t*)(test_regs+GCTL)=1;
    int owned=live;
    hda_remove(&device);
    check(output_detached && input_detached && irq_detached,"remove detaches workers and IRQ first");
    check(stuck ? (live==owned && h->dma.blocked) : live==0,"HDA remove reset gates free");
    cleanup(&h->dma);
#endif
    printf("driver %d mode %d: %d checks, %d failures\n",DRIVER_KIND,mode,checks,failures);
    return failures?1:0;
}
