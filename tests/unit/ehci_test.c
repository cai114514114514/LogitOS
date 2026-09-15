/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Register/DMA apparatus around the entire production backend. This is not
 * USB wire emulation: QEMU separately proves actual controller transactions. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "../../c/drivers/usb/ehci.c"
static int checks,failures,quarantines,quiesces,bios_release=1;
static uint64_t now;
static uint32_t cfg[64];
static uint16_t command_drop_mask;
static int command_ignore_writes;
static struct ehci *model;
static int model_run,model_short=-1,hold_halt,hold_ass;
static uint64_t bar_base;
static unsigned bar_maps,cfg_reads,cfg_writes,master_on_writes,free_calls;
static struct dma_buffer dma_pool[8];
static uint8_t dma_storage[8][EHCI_TRANSFER+4096] __attribute__((aligned(4096)));
static unsigned dma_pool_used;
#define CHECK(c,m) do {checks++;if(!(c)){failures++;printf("FAIL: %s\n",m);}}while(0)
void kprintf(const char *f,...) {(void)f;}
uint64_t time_mono_ns(void)
{
    now+=NS_PER_MS;
    if(model) {
        uint32_t cmd=rd(model,OP_CMD),s=rd(model,OP_STS);
        if(cmd&CMD_RESET) {
            cmd&=~CMD_RESET;
            wr(model,OP_CMD,cmd);
            s&=~0x3fu;
        }
        if(cmd&CMD_RUN)s&=~STS_HALT;else if(!hold_halt)s|=STS_HALT;
        if(cmd&CMD_ASE)s|=STS_ASS;else if(!hold_ass)s&=~STS_ASS;
        if(cmd&CMD_PSE)s|=STS_PSS;else s&=~STS_PSS;
        wr(model,OP_STS,s);
        if(model_run && (cmd&CMD_ASE)) {
            int first=((model->td[0].token>>8)&3)==2?1:0;
            for(unsigned i=0;i<8;i++) {
                struct ehci_qtd *td=&model->td[i];
                if(!(td->token&EHCI_ACTIVE))continue;
                unsigned len=EHCI_REMAIN(td->token);
                td->token&=~(EHCI_ACTIVE|(0x7fffu<<16));
                if ((int)i==first && model_short>=0 && (unsigned)model_short<len)
                    td->token|=(len-(unsigned)model_short)<<16;
                if(td->next==EHCI_END)break;
            }
            model->work->token=EHCI_TOGGLE;
            memset(model->payload+64,0xa5,EHCI_TRANSFER);
        }
    }
    return now;
}
int time_ready(void){return 1;}
uint32_t pci_cfg_read(uint8_t b,uint8_t s,uint8_t f,uint16_t o)
{(void)b;(void)s;(void)f;cfg_reads++;return cfg[o/4];}
void pci_cfg_write(uint8_t b,uint8_t s,uint8_t f,uint16_t o,uint32_t v)
{(void)b;(void)s;(void)f;cfg_writes++;cfg[o/4]=v;if(o==0x40 && bios_release && (v&(1u<<24)))cfg[o/4]&=~(1u<<16);}
uint16_t pci_cfg_read16(uint8_t b,uint8_t s,uint8_t f,uint16_t o)
{return (uint16_t)(pci_cfg_read(b,s,f,o&~3u)>>((o&2)*8));}
void pci_cfg_write16(uint8_t b,uint8_t s,uint8_t f,uint16_t o,uint16_t v)
{
    (void)b;(void)s;(void)f;
    if (o==PCI_CFG_COMMAND) {
        if (command_ignore_writes) return;
        v=(uint16_t)(v&~command_drop_mask);
        cfg_writes++;
        if (v&PCI_CMD_MASTER) master_on_writes++;
        cfg[o/4]=(cfg[o/4]&0xffff0000u)|v;
        return;
    }
    pci_cfg_write(b,s,f,o&~3u,v);
}
void dev_enable(struct device *d,int m){(void)d;(void)m;}
uint64_t dev_bar_map(struct device *d,int i)
{
    (void)i;bar_maps++;
    for(unsigned n=0;n<EHCI_CONTROLLERS;n++)
        if(controllers[n].used && controllers[n].pci==d) model=&controllers[n];
    return bar_base;
}
void dma_device_init(struct dma_device *d,const char *n,uint64_t m)
{memset(d,0,sizeof(*d));d->name=n;d->mask=m;}
struct dma_buffer *dma_alloc_coherent(struct dma_device *d,size_t n,size_t a,size_t boundary)
{
    (void)a;(void)boundary;
    if(dma_pool_used>=8 || n>sizeof(dma_storage[0])) return NULL;
    struct dma_buffer *b=&dma_pool[dma_pool_used];
    memset(b,0,sizeof(*b));memset(dma_storage[dma_pool_used],0,n);
    b->cpu=dma_storage[dma_pool_used];b->dma=(dma_addr_t){0x100000u+dma_pool_used*0x10000u};
    b->size=n;b->dev=d;b->state=DMA_READY;b->next=d->buffers;d->buffers=b;
    dma_pool_used++;return b;
}
int dma_free_coherent(struct dma_buffer *b)
{
    if(!b || !b->dev || b->state!=DMA_QUIESCED) return -1;
    struct dma_buffer **p=&b->dev->buffers;
    while(*p && *p!=b)p=&(*p)->next;
    if(!*p)return -1;
    *p=b->next;b->dev=NULL;b->next=NULL;free_calls++;return 0;
}
uint64_t dma_buffer_submit(struct dma_buffer *b)
{if(!b)return 0;b->state=DMA_DEVICE_OWNED;b->token++;return b->token?b->token:++b->token;}
void dma_wmb(void)
{
    if(!model)return;
    uint32_t cmd=rd(model,OP_CMD),s=rd(model,OP_STS)&~0x3fu;
    if(cmd&CMD_RUN)s&=~STS_HALT;else if(!hold_halt)s|=STS_HALT;
    wr(model,OP_STS,s);
}
void dma_rmb(void){}
void dma_device_quarantine(struct dma_device *d)
{d->blocked=1;for(struct dma_buffer *b=d->buffers;b;b=b->next)b->state=DMA_QUARANTINED;quarantines++;}
void dma_device_quiesced(struct dma_device *d)
{d->blocked=1;for(struct dma_buffer *b=d->buffers;b;b=b->next)b->state=DMA_QUIESCED;quiesces++;}
int dma_device_resume(struct dma_device *d){d->blocked=0;return 0;}
int ktimer_add(struct ktimer *t,uint64_t a,uint64_t b,ktimer_fn f,void *p,const char *n)
{(void)t;(void)a;(void)b;(void)f;(void)p;(void)n;return 0;}
int ktimer_cancel(struct ktimer *t){(void)t;return 0;}
void usb_hc_irq(void *p){(void)p;}
int usb_hc_register(struct usb_hc *h,struct device *d,const struct usb_hc_ops *o,void *p)
{h->ops=o;h->pci=d;h->priv=p;dev_set_drvdata(d,h);return 0;}
void usb_hc_unregister(struct usb_hc *h){(void)h;}
int usb_clear_tt_buffer(struct usb_device *d,uint8_t a,unsigned t){(void)d;(void)a;(void)t;return 0;}
uint64_t dev_irq_count(const struct device *d){(void)d;return 0;}

int main(void)
{
    struct ehci_qh q;
    struct ehci_qtd td;
    CHECK(ehci_eps(1)==0 && ehci_eps(2)==1 && ehci_eps(3)==2,"USB speed translated to EHCI EPS");
    CHECK(ehci_eps(4)<0,"SuperSpeed is rejected");
    CHECK(!ehci_qh_init(&q,7,0,1,64,1,4,5,0,0),"FS control split is admitted");
    CHECK(((q.caps>>16)&127)==4 && ((q.caps>>23)&127)==5,"FS split records actual TT hub and port");
    CHECK(q.ep&(1u<<27),"FS control sets control endpoint flag");
    CHECK(q.ep&(1u<<14),"control toggle comes from each qTD");
    CHECK(ehci_qh_init(&q,7,0,1,64,1,0,0,0,0)<0,"FS root without TT is rejected");
    CHECK(!ehci_qh_init(&q,1,2,2,8,0,5,1,1,0x1c),"LS interrupt split accepted");
    CHECK((q.caps&65535)==0x1c01,"split interrupt schedules SS then three CS opportunities");
    CHECK(!(q.ep&(15u<<28)) && !(q.ep&(1u<<27)),"periodic has zero NAK reload and no control flag");
    CHECK(ehci_qh_init(&q,1,2,2,9,0,5,1,1,0x1c)<0,"LS oversized packet refused");
    CHECK(!ehci_qtd_init(&td,0x120ff0,16400,1,1),"qTD accepts five-page offset span");
    CHECK(td.page[0]==0x120ff0 && td.page[1]==0x121000 && td.page[4]==0x124000,"qTD uses exact page addresses");
    CHECK(ehci_qtd_init(&td,0x120ff0,16401,1,1)<0,"qTD rejects sixth page");
    CHECK(ehci_qtd_init(&td,UINT64_C(0x100000000),8,1,0)<0,"qTD refuses above DMA32");
    CHECK(ehci_qtd_init(&td,UINT64_C(0xfffffff0),32,1,0)<0,"qTD refuses DMA32 wrap");
    CHECK(!ehci_qtd_init(&td,0x120000,0,0,1),"zero length status transaction admitted");
    CHECK(ehci_actual(64,56u<<16)==8,"short packet reports exact actual bytes");
    CHECK(ehci_actual(64,64u<<16)==0,"zero-length short packet is successful zero");
    CHECK(ehci_actual(64,65u<<16)<0 && ehci_actual(64,EHCI_HALTED)<0,"invalid residual and stalled qTD refused");
    unsigned mask;
    CHECK(ehci_period(1,10,&mask)==8 && mask==1,"FS polling period does not exceed bInterval");
    CHECK(ehci_period(3,2,&mask)==1 && mask==0x55,"HS subframe polling mask is encoded");
    CHECK(ehci_period(3,17,&mask)<0,"invalid HS interval refused");

    static struct ehci e;
    static uint32_t regs[64];
    struct device pci={0};
    e.pci=&pci;e.op=(void *)regs;e.owner=(struct io_domain)IO_DOMAIN_INIT;
    cfg[PCI_CFG_COMMAND/4]=PCI_CMD_IO|PCI_CMD_MASTER;
    CHECK(!pci_command_quiet(&e,(uint16_t)cfg[PCI_CFG_COMMAND/4]) &&
          (cfg[PCI_CFG_COMMAND/4]&(PCI_CMD_MEM|PCI_CMD_MASTER|PCI_CMD_INTX_DIS))==
          (PCI_CMD_MEM|PCI_CMD_INTX_DIS),
          "pre-ownership PCI gate clears bus master and confirms MEM/INTx state");
    cfg[PCI_CFG_COMMAND/4]=PCI_CMD_MASTER;command_drop_mask=PCI_CMD_MEM;
    CHECK(pci_command_quiet(&e,PCI_CMD_MASTER)<0 &&
          !(cfg[PCI_CFG_COMMAND/4]&PCI_CMD_MASTER),
          "PCI quiet gate refuses a dropped MEM-decode readback");
    command_drop_mask=0;
    cfg[PCI_CFG_COMMAND/4]=PCI_CMD_MEM|PCI_CMD_MASTER;
    int handoff_touched=0;
    CHECK(handoff(&e,0,&handoff_touched)<0 && !handoff_touched,
          "firmware handoff refuses pre-ownership bus mastering");
    cfg[PCI_CFG_COMMAND/4]=PCI_CMD_MEM|PCI_CMD_INTX_DIS;
    wr(&e,OP_CMD,CMD_RUN);wr(&e,OP_STS,0);
    CHECK(pci_command_master(&e)<0 && !(cfg[PCI_CFG_COMMAND/4]&PCI_CMD_MASTER),
          "bus master cannot start before an acknowledged controller halt");
    wr(&e,OP_CMD,0);wr(&e,OP_STS,STS_HALT);
    CHECK(!pci_command_master(&e) && (cfg[PCI_CFG_COMMAND/4]&PCI_CMD_MASTER),
          "bus master starts only from the halted owned state");
    cfg[PCI_CFG_COMMAND/4]=PCI_CMD_MEM|PCI_CMD_INTX_DIS;command_drop_mask=PCI_CMD_MASTER;
    CHECK(pci_command_master(&e)<0 && !(cfg[PCI_CFG_COMMAND/4]&PCI_CMD_MASTER),
          "bus-master enable requires config-space readback");
    command_drop_mask=0;cfg[PCI_CFG_COMMAND/4]=PCI_CMD_MEM|PCI_CMD_MASTER;
    CHECK(!pci_command_restore(&e,PCI_CMD_IO) && cfg[PCI_CFG_COMMAND/4]==PCI_CMD_IO,
          "early refusal restores the original PCI Command state");
    cfg[PCI_CFG_COMMAND/4]=PCI_CMD_MEM;command_ignore_writes=1;
    CHECK(pci_command_restore(&e,PCI_CMD_IO)<0,
          "unconfirmed PCI Command restore is retained as an unsafe state");
    cfg[PCI_CFG_COMMAND/4]=PCI_CMD_MEM|PCI_CMD_MASTER;
    CHECK(pci_command_stop_master(&e)<0 &&
          (cfg[PCI_CFG_COMMAND/4]&PCI_CMD_MASTER),
          "teardown retains DMA when bus-master clear is not confirmed");
    command_ignore_writes=0;
    CHECK(!pci_command_stop_master(&e) &&
          !(cfg[PCI_CFG_COMMAND/4]&PCI_CMD_MASTER) &&
          (cfg[PCI_CFG_COMMAND/4]&PCI_CMD_INTX_DIS),
          "teardown confirms bus-master clear before releasing DMA");
    cfg[PCI_CFG_COMMAND/4]=PCI_CMD_MEM|PCI_CMD_INTX_DIS;
    cfg[0x40/4]=1|(1u<<16);cfg[0x44/4]=0xe03f;bios_release=1;
    handoff_touched=0;
    CHECK(!handoff(&e,0x40u<<8,&handoff_touched) && handoff_touched,
          "acknowledged BIOS ownership handoff succeeds");
    CHECK(!(cfg[0x44/4]&0xe03fu),"legacy SMI enables cleared after handoff");
    cfg[0x40/4]=1|(1u<<16);bios_release=0;
    handoff_touched=0;
    CHECK(handoff(&e,0x40u<<8,&handoff_touched)<0 && handoff_touched,
          "firmware ownership timeout refuses controller");
    cfg[0x40/4]=2|(0x40u<<8);
    handoff_touched=0;
    CHECK(handoff(&e,0x40u<<8,&handoff_touched)<0 && !handoff_touched,"EECP cycle refused");

    static uint8_t arena_mem[4096] __attribute__((aligned(4096)));
    static uint8_t payload_mem[EHCI_TRANSFER+4096];
    struct dma_buffer arena_buf={.cpu=arena_mem,.dma={0x100000},.size=sizeof(arena_mem)};
    struct dma_buffer payload_buf={.cpu=payload_mem,.dma={0x200000},.size=sizeof(payload_mem)};
    e.async_mem=&arena_buf;e.payload_mem=&payload_buf;e.head=(void *)arena_mem;
    e.work=(void *)(arena_mem+128);e.td=(void *)(arena_mem+256);e.payload=payload_mem;
    e.up=1;e.hc.priv=&e;wr(&e,OP_CMD,CMD_RUN);model=&e;model_run=1;
    struct usb_device d={.hc=&e.hc,.speed=USB_SPEED_HIGH,.addr=4};
    struct ehci_dev sd={.dev=&d,.addr=4,.ep0=64};d.hcpriv=&sd;
    uint8_t data[32768];memset(data,0x33,sizeof(data));model_short=8;
    CHECK(ehci_control(&d,0x80,6,0x100,0,data,18)==8,"production control preserves short descriptor length");
    CHECK(data[7]==0xa5 && data[8]==0x33,"short control copies only completed bytes");
    CHECK(((e.td[0].token>>8)&3)==2 && (e.td[1].token&EHCI_TOGGLE) &&
        (e.td[2].token&EHCI_TOGGLE),"production control emits SETUP DATA1 STATUS1");
    CHECK(e.td[1].alt==0x100000+256+2*sizeof(struct ehci_qtd),"control short packet branches to status qTD");
    e.ep[0]=(struct ehci_ep){.dev=&d,.addr=0x81,.packet=512,.type=USB_XFER_BULK};
    model_short=-1;
    CHECK(ehci_bulk(&d,0x81,data,sizeof(data))==sizeof(data),"production bulk spans multiple qTDs");
    CHECK(!(rd(&e,OP_CMD)&CMD_ASE),"completed async schedule is disabled before reuse");
    CHECK(e.ep[0].toggle==1,"bulk preserves controller-produced endpoint toggle");
    CHECK(!clear_halt(&d,0x81) && !e.ep[0].toggle,"clear halt restores DATA0");
    model_short=0;memset(data,0x33,sizeof(data));
    CHECK(ehci_bulk(&d,0x81,data,64)==0 && data[0]==0x33,"zero-short bulk does not fabricate or copy payload");
    static uint8_t periodic_mem[4096] __attribute__((aligned(4096)));
    struct ehci_ep *pe=&e.ep[1];
    *pe=(struct ehci_ep){.dev=&d,.addr=0x82,.type=USB_XFER_INT,.packet=8,
        .smask=1,.qh=(void *)periodic_mem,.td=(void *)(periodic_mem+128),
        .payload=periodic_mem+256,.qh_dma=0x300000,.td_dma=0x300080,.payload_dma=0x300100};
    wr(&e,OP_STS,STS_PSS);wr(&e,OP_CMD,CMD_RUN|CMD_PSE);
    CHECK(!int_arm(&d,0x82) && pe->pending && !pe->armed,"periodic arm defers while controller owns overlay");
    CHECK(!pe->td->token && !(rd(&e,OP_CMD)&CMD_PSE),"periodic arm requests stop without editing active schedule");
    wr(&e,OP_STS,0);events(&e.hc);
    CHECK(pe->armed && !pe->pending && (rd(&e,OP_CMD)&CMD_PSE),"event service rearms after acknowledged PSS clear");
    CHECK(pe->qh->next==pe->td_dma && (pe->td->token&EHCI_ACTIVE),"periodic rearm publishes real qTD DMA address");
    pe->td->token=3u<<16;pe->qh->token=EHCI_TOGGLE;uint8_t *got=NULL;
    CHECK(int_poll(&d,0x82,&got)==5 && got==pe->payload && pe->toggle==1,"periodic completion preserves short report and toggle");
    CHECK(int_poll(&d,0x82,&got)<0,"periodic completion is consumed exactly once");
    model_run=0;hold_halt=1;wr(&e,OP_STS,0);quiesces=quarantines=0;
    CHECK(halt(&e)<0 && quarantines==0 && quiesces==0,
          "unacknowledged hardware halt is reported before DMA classification");
    hold_halt=0;wr(&e,OP_STS,STS_HALT);
    CHECK(!halt(&e) && quiesces==0,"hardware halt alone does not release DMA ownership");

    /* A valid legacy capability that points to itself fails only after the OS
     * semaphore write. Full probe must remember that irreversible boundary:
     * restoring firmware's old BME or reusing the static slot could restart its
     * schedule through unknown QH/qTD addresses. */
    static uint32_t probe_regs[128];
    memset(probe_regs,0,sizeof(probe_regs));
    probe_regs[0]=(0x100u<<16)|0x20u;probe_regs[1]=1;probe_regs[2]=0x40u<<8;
    struct device partial={.prog_if=0x20};
    memcpy(partial.name,"0000:00:1d.0",13);
    partial.res[0]=(struct dev_resource){.start=(uintptr_t)probe_regs,
        .size=sizeof(probe_regs),.flags=DEV_RES_MEM};
    memset(cfg,0,sizeof(cfg));cfg[PCI_CFG_COMMAND/4]=PCI_CMD_IO|PCI_CMD_MASTER;
    cfg[0x40/4]=1|(0x40u<<8)|(1u<<16);cfg[0x44/4]=0xe03f;
    bios_release=1;command_drop_mask=0;command_ignore_writes=0;
    bar_base=(uintptr_t)probe_regs;bar_maps=cfg_reads=cfg_writes=master_on_writes=0;model=NULL;
    int partial_rc=ehci_probe(&partial),partial_slot=-1;
    for(unsigned i=0;i<EHCI_CONTROLLERS;i++)
        if(controllers[i].used&&controllers[i].pci==&partial)partial_slot=(int)i;
    unsigned accesses=bar_maps+cfg_reads+cfg_writes;
    int partial_again=ehci_probe(&partial);
    CHECK(partial_rc<0 && partial_again<0 && partial_slot>=0 &&
          controllers[partial_slot].failed && controllers[partial_slot].dma.blocked &&
          !(cfg[PCI_CFG_COMMAND/4]&PCI_CMD_MASTER) &&
          (cfg[PCI_CFG_COMMAND/4]&(PCI_CMD_MEM|PCI_CMD_INTX_DIS))==
              (PCI_CMD_MEM|PCI_CMD_INTX_DIS) && !master_on_writes &&
          accesses==bar_maps+cfg_reads+cfg_writes,
          "post-semaphore handoff failure retains quiet ownership and blocks reprobe");

    /* Drive a second controller through the complete production probe, then
     * make HCHalted stick low. shutdown still has to cut PCI Bus Master and
     * retain every submitted arena because either barrier failing is unsafe. */
    memset(probe_regs,0,sizeof(probe_regs));
    probe_regs[0]=(0x100u<<16)|0x20u;probe_regs[1]=1;
    probe_regs[(0x20+OP_STS)/4]=STS_HALT;
    struct device live_pci={.prog_if=0x20};
    memcpy(live_pci.name,"0000:00:1a.0",13);
    live_pci.res[0]=(struct dev_resource){.start=(uintptr_t)probe_regs,
        .size=sizeof(probe_regs),.flags=DEV_RES_MEM};
    memset(cfg,0,sizeof(cfg));cfg[PCI_CFG_COMMAND/4]=PCI_CMD_MEM;
    bar_base=(uintptr_t)probe_regs;model=NULL;hold_halt=0;model_run=0;
    dma_pool_used=free_calls=0;memset(dma_pool,0,sizeof(dma_pool));
    int live_rc=ehci_probe(&live_pci),live_slot=-1;
    for(unsigned i=0;i<EHCI_CONTROLLERS;i++)
        if(controllers[i].used&&controllers[i].pci==&live_pci)live_slot=(int)i;
    struct ehci *live=live_slot>=0?&controllers[live_slot]:NULL;
    struct dma_buffer *submitted=live?live->dma.buffers:NULL;
    hold_halt=1;
    int shutdown_rc=live?shutdown(&live->hc):-1;
    CHECK(live_rc==0 && live && submitted && shutdown_rc<0 &&
          !(cfg[PCI_CFG_COMMAND/4]&PCI_CMD_MASTER) &&
          (cfg[PCI_CFG_COMMAND/4]&PCI_CMD_INTX_DIS) && live->dma.blocked &&
          live->dma.buffers==submitted && submitted->state==DMA_QUARANTINED && !free_calls,
          "halt-timeout shutdown still isolates BME and retains submitted DMA");
    hold_halt=0;
    printf("EHCI production: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
