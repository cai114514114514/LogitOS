/* SPDX-License-Identifier: GPL-3.0-or-later */
/* EHCI 1.0 sections 2, 3.5/3.6, 4.8/4.12, 5.1; Intel C600/X79 datasheet
 * sections 5.20/5.21. X79 has two controllers and one single-TT rate matching
 * hub per controller. Root-only high-speed support would miss its keyboards.
 *
 * Async transactions use one serialized control/bulk queue and a DMA32 bounce
 * arena, so a timed-out controller never retains a caller's stack/high heap.
 * ASS=0 is required before queue reuse; final release requires HCHalted. No
 * isochronous, high-bandwidth interrupt endpoints, companion UHCI/OHCI routing,
 * suspend or downstream-hub hotplug. Root-port connect/disconnect is deferred
 * to the USB core's kworker. Periodic admission reserves whole frames: a
 * conservative refusal is preferable to corrupting a shared TT's bandwidth.
 * Periodic rearming is deferred until PSS=0; an IRQ never spins for hardware.
 */
#include <stdint.h>
#include <stddef.h>
#include "ehci.h"
#include "ehci_hw.h"
#include "usb.h"
#include "usb_hc.h"
#include "driver.h"
#include "pci.h"
#include "dma.h"
#include "io_domain.h"
#include "io_lock.h"
#include "ktime.h"
#include "kprintf.h"
#ifndef memset
void *memset(void *, int, size_t);
#endif
#ifndef memcpy
void *memcpy(void *, const void *, size_t);
#endif

#define EHCI_CONTROLLERS 4
#define EHCI_ENDPOINTS 32
#define EHCI_TRANSFER 32768u
#define OP_CMD 0x00
#define OP_STS 0x04
#define OP_INTR 0x08
#define OP_FRAME 0x0c
#define OP_SEGMENT 0x10
#define OP_PERIODIC 0x14
#define OP_ASYNC 0x18
#define OP_CONFIG 0x40
#define OP_PORT(p) (0x44+4*((p)-1))
#define CMD_RUN 1u
#define CMD_RESET 2u
#define CMD_PSE (1u<<4)
#define CMD_ASE (1u<<5)
#define STS_HALT (1u<<12)
#define STS_PSS (1u<<14)
#define STS_ASS (1u<<15)
#define STS_HSE (1u<<4)
#define PORT_CCS 1u
#define PORT_CSC (1u<<1)
#define PORT_PE 4u
#define PORT_RESET (1u<<8)
#define PORT_POWER (1u<<12)
#define PORT_OWNER (1u<<13)
#define PORT_W1C ((1u<<1)|(1u<<3)|(1u<<5))

struct ehci_ep {
    struct usb_device *dev;
    uint8_t addr, type, toggle, armed, pending;
    uint16_t packet, period, phase;
    uint8_t smask, cmask;
    struct dma_buffer *mem;
    struct ehci_qh *qh;
    struct ehci_qtd *td;
    uint8_t *payload;
    uint32_t qh_dma, td_dma, payload_dma;
};
struct ehci_dev {
    struct usb_device *dev;
    uint8_t addr;
    uint16_t ep0;
};
struct ehci {
    struct usb_hc hc;
    struct device *pci;
    volatile uint8_t *op;
    unsigned used, up, failed, ports, ppc, dirty, reported_staging, reported_irq;
    uint64_t periodic_deadline;
    struct dma_device dma;
    struct dma_buffer *async_mem, *payload_mem, *periodic_mem;
    struct ehci_qh *head, *work;
    struct ehci_qtd *td;
    uint8_t *payload;
    volatile uint32_t *frames;
    struct ehci_ep *frame_owner[1024];
    struct ehci_ep ep[EHCI_ENDPOINTS];
    struct ehci_dev device[USB_MAX_DEVICES];
    struct io_domain owner;
    io_lock_t gate;
    struct ktimer timer;
    unsigned long completions, errors, reports;
};
static struct ehci controllers[EHCI_CONTROLLERS];
static io_lock_t controllers_gate=IO_LOCK_INIT;
static uint32_t rd(struct ehci *e, unsigned r)
{ return *(volatile uint32_t *)(e->op+r); }
static void wr(struct ehci *e, unsigned r, uint32_t v)
{ *(volatile uint32_t *)(e->op+r)=v; }
static struct ehci *controller(struct usb_device *d)
{ return d && d->hc ? d->hc->priv : NULL; }
static int wait_reg(struct ehci *e,unsigned reg,uint32_t mask,uint32_t value,unsigned ms)
{
    uint64_t end=time_mono_ns()+(uint64_t)ms*NS_PER_MS;
    for (unsigned i=0;i<100000000;i++) {
        uint32_t v=rd(e,reg);
        if (v==UINT32_MAX) return -1;
        if ((v&mask)==value) return 0;
        if (time_mono_ns()>=end) break;
        io_relax();
    }
    return -1;
}
static int delay(unsigned ms)
{
    uint64_t end=time_mono_ns()+(uint64_t)ms*NS_PER_MS;
    for (unsigned i=0;i<100000000;i++) {
        if (time_mono_ns()>=end) return 0;
        io_relax();
    }
    return -1;
}
static void command(struct ehci *e,uint32_t clear,uint32_t set)
{
    IO_GUARD(&e->gate);
    wr(e,OP_CMD,(rd(e,OP_CMD)&~clear)|set);
    (void)rd(e,OP_CMD);
}
static int halt(struct ehci *e)
{
    command(e,CMD_RUN|CMD_ASE|CMD_PSE,0);
    wr(e,OP_INTR,0);
    int rc=wait_reg(e,OP_STS,STS_HALT,STS_HALT,100);
#ifdef EHCI_NEGCTL_NO_HALT
    rc=0;
#endif
    e->up=0;
    return rc;
}
static int stop_dma(struct ehci *e);
static void failed(struct ehci *e,const char *reason)
{
    e->failed=1;
    kprintf("[ehci] %s: %s; stopping controller\n",e->pci->name,reason);
    (void)stop_dma(e);
}

/* PCI Command is part of controller ownership.  Firmware may leave an EHCI
 * schedule running across the boot handoff, so merely enabling MEM while
 * preserving an old Bus Master bit can resume DMA through firmware-owned
 * QH/qTD addresses.  First make config space quiet and prove the readback;
 * Bus Master is admitted only after ownership, an acknowledged halt/reset,
 * and publication of our own schedules. */
static int pci_command_quiet(struct ehci *e,uint16_t old)
{
    uint16_t wanted=(uint16_t)((old|PCI_CMD_MEM|PCI_CMD_INTX_DIS)&~PCI_CMD_MASTER);
#ifdef EHCI_NEGCTL_PRE_HANDOFF_BME
    wanted=(uint16_t)(wanted|(old&PCI_CMD_MASTER));
#endif
    pci_cfg_write16(e->pci->bus,e->pci->slot,e->pci->func,PCI_CFG_COMMAND,wanted);
    uint16_t after=pci_cfg_read16(e->pci->bus,e->pci->slot,e->pci->func,PCI_CFG_COMMAND);
    int ok=after!=UINT16_MAX && (after&PCI_CMD_MEM) && !(after&PCI_CMD_MASTER) &&
           (after&PCI_CMD_INTX_DIS);
#ifdef EHCI_NEGCTL_COMMAND_NO_READBACK
    ok=1;
#endif
    return ok?0:-1;
}
static int pci_command_restore(struct ehci *e,uint16_t old)
{
    const uint16_t touched=PCI_CMD_MEM|PCI_CMD_MASTER|PCI_CMD_INTX_DIS;
    pci_cfg_write16(e->pci->bus,e->pci->slot,e->pci->func,PCI_CFG_COMMAND,old);
    uint16_t after=pci_cfg_read16(e->pci->bus,e->pci->slot,e->pci->func,PCI_CFG_COMMAND);
    int ok=after!=UINT16_MAX && !((after^old)&touched);
#ifdef EHCI_NEGCTL_RESTORE_NO_READBACK
    ok=1;
#endif
    return ok?0:-1;
}
static int pci_command_master(struct ehci *e)
{
    uint32_t cmd=rd(e,OP_CMD),sts=rd(e,OP_STS);
    int halted=cmd!=UINT32_MAX && sts!=UINT32_MAX && !(cmd&(CMD_RUN|CMD_ASE|CMD_PSE)) &&
               (sts&STS_HALT);
#ifdef EHCI_NEGCTL_MASTER_BEFORE_HALT
    halted=1;
#endif
    if (!halted) return -1;
    uint16_t old=pci_cfg_read16(e->pci->bus,e->pci->slot,e->pci->func,PCI_CFG_COMMAND);
    if (old==UINT16_MAX || !(old&PCI_CMD_MEM)) return -1;
    uint16_t wanted=(uint16_t)(old|PCI_CMD_MASTER);
    pci_cfg_write16(e->pci->bus,e->pci->slot,e->pci->func,PCI_CFG_COMMAND,wanted);
    uint16_t after=pci_cfg_read16(e->pci->bus,e->pci->slot,e->pci->func,PCI_CFG_COMMAND);
    return after!=UINT16_MAX && (after&(PCI_CMD_MEM|PCI_CMD_MASTER))==
           (PCI_CMD_MEM|PCI_CMD_MASTER)?0:-1;
}
static int pci_command_stop_master(struct ehci *e)
{
    uint16_t old=pci_cfg_read16(e->pci->bus,e->pci->slot,e->pci->func,PCI_CFG_COMMAND);
    if (old==UINT16_MAX) return -1;
    uint16_t wanted=(uint16_t)((old|PCI_CMD_INTX_DIS)&~PCI_CMD_MASTER);
    pci_cfg_write16(e->pci->bus,e->pci->slot,e->pci->func,PCI_CFG_COMMAND,wanted);
    uint16_t after=pci_cfg_read16(e->pci->bus,e->pci->slot,e->pci->func,PCI_CFG_COMMAND);
    int ok=after!=UINT16_MAX && !(after&PCI_CMD_MASTER) && (after&PCI_CMD_INTX_DIS);
#ifdef EHCI_NEGCTL_CLEAR_MASTER_NO_READBACK
    ok=1;
#endif
    return ok?0:-1;
}

/* HCHalted and PCI Bus Master are separate isolation barriers.  A controller
 * that ignores RUN=0 may still be stopped at the PCI boundary, so always try
 * both and classify DMA as quiesced only after both readbacks agree. */
static int stop_dma(struct ehci *e)
{
    int halt_rc=halt(e);
#ifdef EHCI_NEGCTL_SKIP_ISOLATE_ON_HALT_FAIL
    int master_rc=halt_rc?0:pci_command_stop_master(e);
#else
    int master_rc=pci_command_stop_master(e);
#endif
    if (halt_rc || master_rc) {
        dma_device_quarantine(&e->dma);
        return -1;
    }
    dma_device_quiesced(&e->dma);
    return 0;
}

/* EECP is a PCI CONFIG byte offset, unlike xHCI's MMIO DWORD offset. Refuse
 * cycles/malformed lists, request OS ownership, and never force BIOS release.
 * Clear only defined SMI enables/W1C statuses after BIOS acknowledges.  Once
 * an OS-owned semaphore write has been attempted, failure is no longer an
 * untouched decline: the caller must retain the quiet PCI ownership state. */
static int handoff(struct ehci *e,uint32_t hcc,int *touched)
{
#ifdef EHCI_NEGCTL_IGNORE_BIOS
    (void)e;(void)hcc;(void)touched;return 0;
#endif
    uint16_t command=pci_cfg_read16(e->pci->bus,e->pci->slot,e->pci->func,PCI_CFG_COMMAND);
    if (command==UINT16_MAX || (command&PCI_CMD_MASTER)) return -1;
    unsigned off=(hcc>>8)&255u;
    uint64_t seen=0;
    while (off) {
        if (off<0x40 || off>0xf8 || (off&3) || (seen&(UINT64_C(1)<<(off/4)))) return -1;
        seen|=UINT64_C(1)<<(off/4);
        uint32_t v=pci_cfg_read(e->pci->bus,e->pci->slot,e->pci->func,off);
        if ((v&255u)==1) {
            if (touched) *touched=1;
            pci_cfg_write(e->pci->bus,e->pci->slot,e->pci->func,off,v|(1u<<24));
            uint64_t end=time_mono_ns()+NS_PER_SEC;
            unsigned tries=0;
            do {
                v=pci_cfg_read(e->pci->bus,e->pci->slot,e->pci->func,off);
                if (!(v&(1u<<16))) break;
                if (++tries==100000000 || time_mono_ns()>=end) return -1;
                io_relax();
            } while (1);
            if (!(v&(1u<<24))) return -1;
            uint32_t ctl=pci_cfg_read(e->pci->bus,e->pci->slot,e->pci->func,off+4);
            pci_cfg_write(e->pci->bus,e->pci->slot,e->pci->func,off+4,
                          (ctl&~0xe03fu)|0xe03f0000u);
        }
        off=(v>>8)&255u;
    }
    return 0;
}
static struct dma_buffer *arena(struct ehci *e,unsigned size)
{
    struct dma_buffer *b=dma_alloc_coherent(&e->dma,size,4096,0);
    if (!b) return NULL;
    memset(b->cpu,0,b->size);
    if (!dma_buffer_submit(b)) { dma_free_coherent(b); return NULL; }
    return b;
}
static struct ehci_ep *endpoint(struct ehci *e,struct usb_device *d,uint8_t addr)
{
    for (unsigned i=0;i<EHCI_ENDPOINTS;i++)
        if (e->ep[i].dev==d && e->ep[i].addr==addr) return &e->ep[i];
    return NULL;
}
static int qh_device(struct ehci_qh *q,struct usb_device *d,unsigned ep,
                     unsigned packet,int control,unsigned smask,unsigned cmask)
{
    return ehci_qh_init(q,d->addr,ep,d->speed,packet,control,
                       d->tt_hub?d->tt_hub->addr:0,d->tt_port,smask,cmask);
}
static int async_stop(struct ehci *e)
{
    command(e,CMD_ASE,0);
    if (!wait_reg(e,OP_STS,STS_ASS,0,100)) return 0;
    failed(e,"async schedule did not stop"); return -1;
}

/* Caller holds the thread owner. USB NAK is not completion. For IN, walk only
 * through the first short qTD: later descriptors can remain Active because
 * the hardware followed Alternate Next to the control status stage/end. */
static int transfer(struct ehci *e,struct usb_device *d,struct ehci_ep *ep,
                    const uint8_t *setup,void *data,unsigned len,int in)
{
    if (!e->up || e->failed || len>EHCI_TRANSFER || (len&&!data)) return -1;
    struct ehci_dev *sd=d->hcpriv;
    if (!sd || async_stop(e)) return -1;
    unsigned packet=ep?ep->packet:sd->ep0;
    if (qh_device(e->work,d,ep?USB_EP_NUM(ep->addr):0,packet,!ep,0,0)) return -1;
    uint32_t base=(uint32_t)dma_addr_value(e->async_mem->dma);
    uint32_t td_base=base+256;
    e->head->link=(base+128)|EHCI_QH_LINK;
    e->work->link=base|EHCI_QH_LINK;
    unsigned count=0,first=setup?1:0,amount[8]={0},data_count=0;
    unsigned off=0,dt=setup?1:0;
    if (setup) {
        memcpy(e->payload,setup,8);
        if (ehci_qtd_init(&e->td[count++],dma_addr_value(e->payload_mem->dma),8,2,0)) return -1;
    }
    if (len && !in) memcpy(e->payload+64,data,len);
    do {
        unsigned n=len-off;
        if (n>16384) n=16384;
        if (setup && !len) break;
        amount[count]=n;
        if (ehci_qtd_init(&e->td[count],dma_addr_value(e->payload_mem->dma)+64+off,n,in?1:0,dt)) return -1;
        dt^=((n+packet-1)/packet)&1u;
        count++; data_count++; off+=n;
    } while (off<len);
    unsigned status=count;
    if (setup) {
        if (ehci_qtd_init(&e->td[count++],dma_addr_value(e->payload_mem->dma),0,in?0:1,1)) return -1;
    }
    for (unsigned i=0;i<count;i++) {
        e->td[i].next=i+1<count?td_base+(i+1)*sizeof(struct ehci_qtd):EHCI_END;
        if (setup && in && i>=first && i<status)
            e->td[i].alt=td_base+status*sizeof(struct ehci_qtd);
    }
    e->td[count-1].token|=EHCI_IOC;
    e->work->next=td_base;
    if (ep && ep->toggle) e->work->token=EHCI_TOGGLE;
    dma_wmb();
    command(e,0,CMD_ASE);
    uint64_t end=time_mono_ns()+3*NS_PER_SEC;
    int done=0,bad=0;
    for (unsigned spin=0;spin<100000000;spin++) {
        dma_rmb();
        for (unsigned i=0;i<count;i++) if (e->td[i].token&EHCI_ERRORS) bad=1;
        if (bad || e->failed || (rd(e,OP_STS)&STS_HSE)) break;
        if (setup) done=!(e->td[count-1].token&EHCI_ACTIVE);
        else for (unsigned i=0;i<count;i++) {
            uint32_t t=e->td[i].token;
            if (t&EHCI_ACTIVE) break;
            if (EHCI_REMAIN(t) || i+1==count) { done=1; break; }
        }
        if (done || time_mono_ns()>=end) break;
        io_relax();
    }
    if (async_stop(e)) return -1;
    dma_rmb();
    if (!done || bad || e->failed) {
        e->errors++;
        kprintf("[ehci] transfer failed addr=%u ep=%x len=%u token=%x done=%d\n",
                d->addr,ep?ep->addr:0,len,e->work->token,done);
        return -1;
    }
    int total=0;
    for (unsigned i=first;i<first+data_count;i++) {
        int n=ehci_actual(amount[i],e->td[i].token);
        if (n<0) return -1;
        total+=n;
        if ((unsigned)n<amount[i]) break;
    }
    if (ep) ep->toggle=!!(e->work->token&EHCI_TOGGLE);
    if (in && total) memcpy(data,e->payload+64,(unsigned)total);
    if (ep && total>=512 && !e->reported_staging) {
        e->reported_staging=1;
        kprintf("USB_BULK_DMA hc=ehci dma=%p bytes=%u\n",
            (void *)(uintptr_t)(dma_addr_value(e->payload_mem->dma)+64),(unsigned)total);
    }
    e->completions++;
    return total;
}
static int ehci_control(struct usb_device *d,uint8_t rt,uint8_t req,uint16_t val,
                        uint16_t idx,void *data,uint16_t len)
{
    struct ehci *e=controller(d);
    if (!e) return -1;
    IO_DOMAIN_GUARD(&e->owner);
    uint8_t s[8]={rt,req,val,val>>8,idx,idx>>8,len,len>>8};
    int rc=transfer(e,d,NULL,s,data,len,!!(rt&0x80));
    if (rc<0 && !e->failed && d->tt_hub) {
        (void)usb_clear_tt_buffer(d,0,USB_XFER_CONTROL);
        (void)usb_clear_tt_buffer(d,0x80,USB_XFER_CONTROL);
    }
    return rc;
}
static int ehci_bulk(struct usb_device *d,uint8_t addr,void *data,uint32_t len)
{
    struct ehci *e=controller(d);
    if (!e) return -1;
    IO_DOMAIN_GUARD(&e->owner);
    struct ehci_ep *ep=endpoint(e,d,addr);
    if (!ep || ep->type!=USB_XFER_BULK) return -1;
    int rc=transfer(e,d,ep,NULL,data,len,USB_EP_IS_IN(addr));
    /* Only after ASS=0: an aborted split may leave its TT holding a packet. */
    if (rc<0 && !e->failed) (void)usb_clear_tt_buffer(d,addr,USB_XFER_BULK);
    return rc;
}
static int device_open(struct usb_device *d)
{
    struct ehci *e=controller(d);
    if (!e || ehci_eps(d->speed)<0 || (d->speed!=USB_SPEED_HIGH &&
        (!d->tt_hub || !d->tt_port))) return -1;
    IO_DOMAIN_GUARD(&e->owner);
    struct ehci_dev *sd=NULL;
    for (unsigned i=0;i<USB_MAX_DEVICES;i++) if (!e->device[i].dev) { sd=&e->device[i]; break; }
    if (!sd) return -1;
    unsigned addr=1;
    for (;addr<128;addr++) {
        int taken=0;
        for (unsigned i=0;i<USB_MAX_DEVICES;i++) if(e->device[i].dev&&e->device[i].addr==addr) taken=1;
        if (!taken) break;
    }
    if (addr==128) return -1;
    *sd=(struct ehci_dev){.dev=d,.addr=addr,.ep0=d->speed==USB_SPEED_HIGH?64:8};
    d->hcpriv=sd; d->addr=0;
    if (ehci_control(d,0,USB_REQ_SET_ADDRESS,addr,0,NULL,0)<0 || delay(2)) return -1;
    d->addr=addr;
    return 0;
}
static int set_ep0(struct usb_device *d,int packet)
{
    struct ehci_dev *sd=d->hcpriv;
    if (!sd || (packet!=8 && packet!=16 && packet!=32 && packet!=64) ||
        (d->speed==USB_SPEED_LOW && packet!=8) ||
        (d->speed==USB_SPEED_HIGH && packet!=64)) return -1;
    sd->ep0=packet; return 0;
}

/* gate held. Rebuilding only with PSS clear avoids modifying an overlay that
 * the controller has prefetched. The timer advances this handshake when a
 * NAK-only keyboard produces no USB interrupt at all. */
static void periodic_service(struct ehci *e)
{
    if (!e->dirty || !e->up || e->failed) return;
    if (rd(e,OP_STS)&STS_PSS) {
        if (e->periodic_deadline && time_mono_ns()>e->periodic_deadline) {
            /* IRQ context: stop admission/run now, retain every DMA page.
             * Thread-context shutdown performs the acknowledged halt/drain. */
            e->failed=1;wr(e,OP_INTR,0);wr(e,OP_CMD,rd(e,OP_CMD)&~CMD_RUN);
        }
        return;
    }
    int live=0;
    for (unsigned i=0;i<EHCI_ENDPOINTS;i++) {
        struct ehci_ep *p=&e->ep[i];
        if (!p->dev || p->type!=USB_XFER_INT) continue;
        live=1;
        if (!p->pending) continue;
        if (qh_device(p->qh,p->dev,USB_EP_NUM(p->addr),p->packet,0,p->smask,p->cmask) ||
            ehci_qtd_init(p->td,p->payload_dma,p->packet,1,0)) { p->pending=0; continue; }
        p->td->token|=EHCI_IOC;
        p->qh->token=p->toggle?EHCI_TOGGLE:0;
        p->qh->next=p->td_dma;
        p->pending=0; p->armed=1;
    }
    dma_wmb(); e->dirty=0;
    if (live) wr(e,OP_CMD,rd(e,OP_CMD)|CMD_PSE);
}
static int int_arm(struct usb_device *d,uint8_t addr)
{
    struct ehci *e=controller(d);
    if (!e) return -1;
    IO_GUARD(&e->gate);
    struct ehci_ep *p=endpoint(e,d,addr);
    if (!e->up || e->failed || !p || p->type!=USB_XFER_INT) return -1;
    if (p->armed || p->pending) return 0;
    p->pending=1;
    if (!e->dirty) e->periodic_deadline=time_mono_ns()+100*NS_PER_MS;
    e->dirty=1;
    wr(e,OP_CMD,rd(e,OP_CMD)&~CMD_PSE);
    periodic_service(e);
    return 0;
}
static int int_poll(struct usb_device *d,uint8_t addr,uint8_t **data)
{
    struct ehci *e=controller(d);
    if (!e || !data) return -1;
    IO_GUARD(&e->gate);
    struct ehci_ep *p=endpoint(e,d,addr);
    if (!e->up || !p || !p->armed) return -1;
    dma_rmb();
    uint32_t t=p->td->token;
    if (t&EHCI_ACTIVE) return -1;
    p->armed=0;
    int n=ehci_actual(p->packet,t);
    if (n<0) { e->errors++; return -1; }
    p->toggle=!!(p->qh->token&EHCI_TOGGLE);
    *data=p->payload; e->completions++;e->reports++;
    return n;
}
static int configure(struct usb_device *d,const struct usb_interface *iface)
{
    struct ehci *e=controller(d);
    if (!e || !iface) return -1;
    IO_DOMAIN_GUARD(&e->owner);
    for (unsigned i=0;i<iface->n_ep;i++) {
        const struct usb_endpoint *desc=&iface->ep[i];
        if (endpoint(e,d,desc->addr)) continue;
        unsigned type=USB_EP_XFER(desc->attr),packet=desc->max_packet;
        if (!USB_EP_NUM(desc->addr) || (desc->addr&0x70) || !packet ||
            (type!=USB_XFER_BULK && type!=USB_XFER_INT) ||
            (type==USB_XFER_INT && !USB_EP_IS_IN(desc->addr)) ||
            (d->speed==USB_SPEED_LOW && (type==USB_XFER_BULK || packet>8)) ||
            (d->speed==USB_SPEED_FULL && packet>64) ||
            (d->speed==USB_SPEED_HIGH && packet>(type==USB_XFER_BULK?512u:1024u))) return -1;
        struct ehci_ep *p=NULL;
        for (unsigned j=0;j<EHCI_ENDPOINTS;j++) if (!e->ep[j].dev) { p=&e->ep[j]; break; }
        if (!p) return -1;
        *p=(struct ehci_ep){.addr=desc->addr,.type=type,.packet=packet};
        if (type==USB_XFER_BULK) { IO_GUARD(&e->gate);p->dev=d;continue; }
        unsigned mask=0;
        int period=ehci_period(d->speed,desc->interval,&mask);
        if (period<1 || period>1024) { memset(p,0,sizeof(*p)); return -1; }
        p->period=period; p->smask=mask; p->cmask=d->speed==USB_SPEED_HIGH?0:0x1c;
        p->mem=arena(e,4096);
        if (!p->mem) { memset(p,0,sizeof(*p)); return -1; }
        p->qh=p->mem->cpu; p->td=(void *)((uint8_t *)p->mem->cpu+128);
        p->payload=(uint8_t *)p->mem->cpu+256;
        p->qh_dma=(uint32_t)dma_addr_value(p->mem->dma);
        p->td_dma=p->qh_dma+128; p->payload_dma=p->qh_dma+256;
        p->qh->link=p->qh->next=p->qh->alt=EHCI_END;
        command(e,CMD_PSE,0);
        if (wait_reg(e,OP_STS,STS_PSS,0,100)) { failed(e,"periodic schedule did not stop"); return -1; }
        IO_GUARD(&e->gate);
        int phase=-1;
        for (unsigned j=0;j<(unsigned)period;j++) {
            int free=1;
            for (unsigned f=j;f<1024;f+=period) if(e->frame_owner[f]) {free=0;break;}
            if (free) {phase=(int)j;break;}
        }
        if (phase<0) {
            kprintf("[ehci] periodic bandwidth unavailable addr=%u ep=%x interval=%u\n",d->addr,desc->addr,desc->interval);
            return -1;
        }
        p->phase=phase;p->dev=d;
        for (unsigned f=phase;f<1024;f+=period) {
            e->frame_owner[f]=p; e->frames[f]=p->qh_dma|EHCI_QH_LINK;
        }
        dma_wmb(); e->dirty=1;e->periodic_deadline=time_mono_ns()+100*NS_PER_MS;
        periodic_service(e);
    }
    return 0;
}
static int configure_hub(struct usb_device *d,int ports,int multi,int think)
{
    (void)multi; (void)think;
    return d && ports>0 && ports<=127 ? 0 : -1;
}
static int clear_halt(struct usb_device *d,uint8_t addr)
{
    struct ehci *e=controller(d);
    if (!e) return -1;
    IO_DOMAIN_GUARD(&e->owner);
    struct ehci_ep *p=endpoint(e,d,addr);
    if (!p || p->type!=USB_XFER_BULK || e->failed) return -1;
    p->toggle=0; /* bulk QH is reconstructed for every transfer */
    return 0;
}
static void device_close(struct usb_device *d)
{
    struct ehci *e=controller(d);
    if (!e || !d->hcpriv) return;
    IO_DOMAIN_GUARD(&e->owner);
    command(e,CMD_PSE,0);
    if (e->up && wait_reg(e,OP_STS,STS_PSS,0,100)) failed(e,"device close schedule stop failed");
    if (e->dma.blocked) return; /* keep the hardware-visible schedule intact */
    IO_GUARD(&e->gate);
    for (unsigned i=0;i<EHCI_ENDPOINTS;i++) {
        struct ehci_ep *p=&e->ep[i];
        if (p->dev!=d) continue;
        for (unsigned f=0;f<1024;f++) if(e->frame_owner[f]==p) {e->frames[f]=EHCI_END;e->frame_owner[f]=NULL;}
        /* Coherent endpoint memory remains in the controller ledger until
         * HCHalted at shutdown. This also makes failed-stop pointers stable. */
        memset(p,0,sizeof(*p));
    }
    memset(d->hcpriv,0,sizeof(struct ehci_dev)); d->hcpriv=NULL;
    e->dirty=1; periodic_service(e);
}
static int root_count(struct usb_hc *h) { struct ehci *e=h->priv;return e->ports; }
static int root_connected(struct usb_hc *h,int p)
{
    struct ehci *e=h->priv;
    return p>=1 && p<=(int)e->ports && (rd(e,OP_PORT(p))&PORT_CCS);
}
static int root_change_pending(struct usb_hc *h)
{
    struct ehci *e=h->priv;
    if (!e || !e->up) return 0;
    for (unsigned p=1;p<=e->ports;p++)
        if (rd(e,OP_PORT(p))&PORT_CSC) return 1;
    return 0;
}
static void port_write(struct ehci *e,int p,uint32_t clear,uint32_t set)
{
    uint32_t v=rd(e,OP_PORT(p));
    wr(e,OP_PORT(p),(v&~(PORT_W1C|clear))|set);
}
static int root_changed(struct usb_hc *h,int p,int *connected)
{
    struct ehci *e=h->priv;
    if (!e || !connected || p<1 || p>(int)e->ports) return -1;
    IO_DOMAIN_GUARD(&e->owner);
    uint32_t v=rd(e,OP_PORT(p));
    if (!(v&PORT_CSC)) return 0;
    *connected=!!(v&PORT_CCS);
    port_write(e,p,0,PORT_CSC); /* acknowledge only CSC; PORT_PE is preserved */
    return 1;
}
static int root_reset(struct usb_hc *h,int p,int *speed)
{
    struct ehci *e=h->priv;
    if (!speed || !root_connected(h,p)) return -1;
    if (e->ppc) port_write(e,p,0,PORT_POWER);
    port_write(e,p,PORT_OWNER|PORT_PE,PORT_RESET);
    if (delay(50)) return -1;
    port_write(e,p,PORT_RESET,0);
    if (wait_reg(e,OP_PORT(p),PORT_RESET,0,100) || delay(10)) return -1;
    uint32_t v=rd(e,OP_PORT(p));
    if (!(v&PORT_CCS) || !(v&PORT_PE)) {
        kprintf("[ehci] root port %d is not high-speed; companion required\n",p);
        return -1;
    }
    *speed=USB_SPEED_HIGH;return 0;
}
static void events(struct usb_hc *h)
{
    struct ehci *e=h->priv;
    IO_GUARD(&e->gate);
    if (!e->up) return;
    uint32_t s=rd(e,OP_STS);
    if (s==UINT32_MAX) {e->failed=1;return;}
    if (s&0x3fu) wr(e,OP_STS,s&0x3fu);
    if (s&STS_HSE) {e->failed=1;wr(e,OP_INTR,0);wr(e,OP_CMD,rd(e,OP_CMD)&~CMD_RUN);}
    periodic_service(e);
}
static void timer_tick(struct ktimer *t)
{
    struct ehci *e=t->arg;
    usb_hc_irq(&e->hc);
    uint64_t irqs=dev_irq_count(e->pci);
    if (!e->reported_irq && e->reports && irqs) {
        e->reported_irq=1;
        kprintf("EHCI_IRQ vector=%d delivered=%lu reports=%lu\n",e->pci->irq_vec,(unsigned long)irqs,e->reports);
    }
}
static void irq_enable(struct usb_hc *h)
{
    struct ehci *e=h->priv;
    wr(e,OP_INTR,0x17); /* qTD/error, root-port change, host system error */
    if (ktimer_add(&e->timer,NS_PER_MS,NS_PER_MS,timer_tick,e,"ehci-periodic"))
        kprintf("[ehci] periodic rearm timer unavailable; interrupt input may stall\n");
}
static int shutdown(struct usb_hc *h)
{
    struct ehci *e=h->priv;
    (void)ktimer_cancel(&e->timer);
    IO_DOMAIN_GUARD(&e->owner);
    int rc=stop_dma(e);
    if (!rc) while (e->dma.buffers) dma_free_coherent(e->dma.buffers);
    kprintf("[ehci] %s stop=%s transfers=%lu errors=%lu\n",e->pci->name,
            rc?"quarantined":"halted",e->completions,e->errors);
    return rc;
}
static const struct usb_hc_ops ops={
    .name="ehci",.root_port_count=root_count,.root_port_connected=root_connected,
    .root_port_reset=root_reset,.root_change_pending=root_change_pending,.root_port_changed=root_changed,
    .device_open=device_open,.device_close=device_close,
    .set_ep0_packet=set_ep0,.configure=configure,.configure_hub=configure_hub,
    .control=ehci_control,.bulk=ehci_bulk,.int_in_arm=int_arm,.int_in_poll=int_poll,
    .clear_halt=clear_halt,.events=events,.irq_enable=irq_enable,.shutdown=shutdown,
};
int ehci_probe(struct device *pci)
{
    if (!time_ready() || pci->prog_if!=0x20 || !(pci->res[0].flags&DEV_RES_MEM) || pci->res[0].size<0x60) return -1;
    struct ehci *e=NULL;
    { IO_GUARD(&controllers_gate);
      for (unsigned i=0;i<EHCI_CONTROLLERS;i++) if(controllers[i].used && controllers[i].pci==pci) return -1;
      for (unsigned i=0;i<EHCI_CONTROLLERS;i++) if(!controllers[i].used) {e=&controllers[i];e->used=1;break;} }
    if (!e) return -1;
    e->pci=pci;e->timer.heap_idx=-1;e->owner=(struct io_domain)IO_DOMAIN_INIT;e->gate=(io_lock_t)IO_LOCK_INIT;
    int owned=0;
    uint16_t old_command=pci_cfg_read16(pci->bus,pci->slot,pci->func,PCI_CFG_COMMAND);
    if (old_command==UINT16_MAX || pci_command_quiet(e,old_command)) goto retained;
    uint64_t base=dev_bar_map(pci,0);
    if (!base) goto unused;
    volatile uint32_t *cap=(volatile uint32_t *)(uintptr_t)base;
    uint32_t first=cap[0],hcs=cap[1],hcc=cap[2];
    unsigned length=first&255u;
    e->ports=hcs&15u;e->ppc=!!(hcs&16u);
    if (length<0x10 || (length&3) || (first>>16)<0x100 || !e->ports ||
        length+0x44+4*e->ports>pci->res[0].size) goto unused;
    e->op=(volatile uint8_t *)(uintptr_t)(base+length);
    int handoff_touched=0;
    if (handoff(e,hcc,&handoff_touched)) {
        kprintf("[ehci] %s firmware handoff refused\n",pci->name);
#ifdef EHCI_NEGCTL_POST_HANDOFF_RESTORE_BME
        handoff_touched=0;
#endif
        if (handoff_touched) goto retained;
        goto unused;
    }
    owned=1;
    dma_device_init(&e->dma,"ehci",DMA_MASK_32);
    if (halt(e)) {dma_device_quarantine(&e->dma);return -1;}
    dma_device_quiesced(&e->dma);
    wr(e,OP_CMD,CMD_RESET);
    if (wait_reg(e,OP_CMD,CMD_RESET,0,100)) goto retained;
    if (dma_device_resume(&e->dma)) goto unused;
    e->async_mem=arena(e,4096);e->payload_mem=arena(e,EHCI_TRANSFER+4096);e->periodic_mem=arena(e,4096);
    if (!e->async_mem || !e->payload_mem || !e->periodic_mem) goto stop;
    e->head=e->async_mem->cpu;e->work=(void *)((uint8_t *)e->async_mem->cpu+128);
    e->td=(void *)((uint8_t *)e->async_mem->cpu+256);e->payload=e->payload_mem->cpu;e->frames=e->periodic_mem->cpu;
    e->head->link=(uint32_t)dma_addr_value(e->async_mem->dma)|EHCI_QH_LINK;
    e->head->ep=(2u<<12)|(1u<<15)|(64u<<16);
    e->head->caps=1u<<30;e->head->next=e->head->alt=EHCI_END;e->head->token=EHCI_HALTED;
    for (unsigned i=0;i<1024;i++) e->frames[i]=EHCI_END;
    wr(e,OP_SEGMENT,0);wr(e,OP_FRAME,0);
    wr(e,OP_PERIODIC,(uint32_t)dma_addr_value(e->periodic_mem->dma));
    wr(e,OP_ASYNC,(uint32_t)dma_addr_value(e->async_mem->dma));
    wr(e,OP_STS,0x3f);dma_wmb();
    if (pci_command_master(e)) goto stop;
    wr(e,OP_CMD,CMD_RUN|(8u<<16));
    if (wait_reg(e,OP_STS,STS_HALT,0,100)) goto stop;
    wr(e,OP_CONFIG,1);e->up=1;
    for (unsigned p=1;p<=e->ports;p++) if(e->ppc) port_write(e,p,0,PORT_POWER);
    if (delay(20)) goto stop;
    kprintf("[ehci] %s up: ports=%u DMA32 async/periodic TT-split\n",pci->name,e->ports);
    if (usb_hc_register(&e->hc,pci,&ops,e)) goto stop;
    return 0;
stop:
    if (shutdown(&(struct usb_hc){.priv=e})) return -1;
unused:
    if (pci_command_restore(e,owned?(uint16_t)(old_command&~PCI_CMD_MASTER):old_command)) goto retained;
    memset(e,0,sizeof(*e));return -1;
retained:
    e->failed=1;
    dma_device_quarantine(&e->dma);
    kprintf("[ehci] %s ownership state uncertain; controller retained\n",pci->name);
    return -1;
}
void ehci_remove(struct device *pci)
{
    struct usb_hc *h=dev_get_drvdata(pci);
    if (!h || h->ops!=&ops) return;
    struct ehci *e=h->priv;
    /* Controller storage is static: even a timer already claimed by another
     * CPU sees a closed admission gate, never a freed controller object. */
    (void)ktimer_cancel(&e->timer);
    usb_hc_unregister(h);
    /* Keep static identity reserved: a late timer must not target a new bind. */
}
static const struct dev_match ids[]={
    {DEV_ANY,DEV_ANY,0x0c,0x03,0x20,0},DEV_MATCH_END
};
static struct driver driver={.name="ehci",.bus_type=DEV_BUS_PCI,.match=ids,.probe=ehci_probe,.remove=ehci_remove};
DRIVER_DECLARE(driver);
