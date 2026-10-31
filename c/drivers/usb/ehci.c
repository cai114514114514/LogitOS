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
 * to the USB core's kworker. The old whole-frame reservation and PSS-stop on
 * every rearm are replaced by bandwidth admission, a shared periodic tree and
 * inactive dummy-qTD swaps. IRQ-side unlink uses timestamped grace periods;
 * it never spins for hardware. Not verified on X79-VG4 hardware.
 */
#include "ehci_diag.h"
#include <stdint.h>
#include <stddef.h>
#include "ehci.h"
#include "ehci_hw.h"
#include "ehci_sched.h"
#include "usb.h"
#include "usb_hc.h"
#include "driver.h"
#include "pci.h"
#include "dma.h"
#include "io_domain.h"
#include "io_lock.h"
#include "ktime.h"
#include "kprintf.h"
#include "work.h"
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
    uint8_t addr, type, toggle, armed; /* toggle drives bulk; INT keeps only a diagnostic copy. */
    uint16_t packet;
    uint8_t dev_addr;                   /* for the panel: dev may be freed */
    uint32_t done, errs, last_token, last_qh_token;
    struct dma_buffer *mem;
    struct ehci_qh *qh;
    struct ehci_qtd *td;
    uint8_t *payload;
    uint32_t qh_dma, td_dma, payload_dma;
    struct ehci_ps ps;
    struct ehci_pnode node;
    struct ehci_tt_acct *tt;
    struct ehci_qtd *q[2];
    uint32_t q_dma[2];
    uint8_t cur, state, interval, stall_pending;
    uint8_t stall_attempts, closing, reported_stalled;
    uint64_t unlink_ns, arm_ns, stall_retry_ns;
    uint32_t arm_frame;
    struct work stall_work;
    struct ehci *owner;
    uint32_t rearms, err_halt, err_xact, err_mmf, err_dbe, err_babble, stalls;
};
/* Software state, not hardware bits. UNLINKING retains QH/TD ownership until
 * ehci-timer.c's 9-microframe (1125 us) grace period has expired. */
enum { EP_IDLE, EP_LINKED, EP_UNLINKING };
#define EHCI_UNLINK_NS UINT64_C(1125000)
#define EHCI_PSE_OFF_NS (10 * NS_PER_MS) /* Linux EHCI_HRTIMER_DISABLE_PERIODIC. */
/* P2 bring-up policy, not timings mandated by USB: three CLEAR_HALT attempts
 * at 100 ms intervals, and a one-shot 3 s no-report diagnostic. EHCI 2.3.1/
 * 2.3.2 require the PSE/PSS handshake; 100 ms retains this driver's previous
 * refusal bound rather than claiming a specification-defined timeout. */
#define EHCI_STALL_RETRIES 3u
#define EHCI_STALL_RETRY_NS (100 * NS_PER_MS)
#define EHCI_INPUT_WARN_NS (3 * NS_PER_SEC)
#define EHCI_PSS_TIMEOUT_MS 100u
struct ehci_dev {
    struct usb_device *dev;
    uint8_t addr;
    uint16_t ep0;
};
struct ehci {
    struct usb_hc hc;
    struct device *pci;
    volatile uint8_t *op;
    unsigned used, up, failed, ports, ppc, reported_staging, reported_irq;
    struct dma_device dma;
    struct dma_buffer *async_mem, *payload_mem, *periodic_mem;
    struct ehci_qh *head, *work;
    struct ehci_qtd *td;
    uint8_t *payload;
    volatile uint32_t *frames;
    struct ehci_sched sched;
    struct ehci_ptree tree;
    unsigned pse_toggles, intr_count, reported_sched, reported_poll;
    uint64_t pse_off_deadline, pse_check_deadline;
    struct ehci_sched_diag sched_diag;
    struct ehci_ep ep[EHCI_ENDPOINTS];
    struct ehci_dev device[USB_MAX_DEVICES];
    struct io_domain owner;
    io_lock_t gate;
    struct ktimer timer;
    unsigned long completions, errors, reports;
};
static struct ehci controllers[EHCI_CONTROLLERS];

int ehci_ctrl_diag(unsigned c, uint32_t *sts, uint32_t *intr, unsigned long *errors)
{
    if (c >= EHCI_CONTROLLERS || !controllers[c].used || !controllers[c].op) return -1;
    *sts = *(volatile uint32_t *)(controllers[c].op + 0x04);
    *intr = *(volatile uint32_t *)(controllers[c].op + 0x08);
    *errors = (unsigned long)controllers[c].errors;
    return 0;
}

int ehci_diag_get(int i, struct ehci_ep_diag *out)
{
    for (unsigned c = 0, seen = 0; c < EHCI_CONTROLLERS; c++) {
        if (!controllers[c].used) continue;
        for (unsigned j = 0; j < EHCI_ENDPOINTS; j++) {
            struct ehci_ep *p = &controllers[c].ep[j];
            if (!p->dev || p->type != USB_XFER_INT || !p->qh) continue;
            if ((int)seen++ != i) continue;
            *out = (struct ehci_ep_diag){ .controller = (uint8_t)c, .dev_addr = p->dev_addr,
                .ep = p->addr, .armed = p->armed, .period = p->ps.period, .phase = p->ps.phase,
                .done = p->done, .errors = p->errs, .td_token = p->last_token,
                .qh_token = p->last_qh_token, .phase_uf = p->ps.phase_uf,
                .smask = p->ps.smask, .cmask = p->ps.cmask, .speed = p->ps.speed,
                .state = p->state, .us_s = p->ps.us_s, .us_c = p->ps.us_c,
                .us_tt = p->ps.us_tt, .rearms = p->rearms, .err_halt = p->err_halt,
                .err_xact = p->err_xact, .err_mmf = p->err_mmf, .err_dbe = p->err_dbe,
                .err_babble = p->err_babble, .stalls = p->stalls,
                .overlay_status = p->last_qh_token & 255 };
            return 0;
        }
    }
    return -1;
}
static io_lock_t controllers_gate=IO_LOCK_INIT;

/* Default MMIO remains exactly a volatile access. The host model overrides
 * these leaves so every read advances DMA and every PSE edge is observable;
 * otherwise a test of volatile stores would only be watching its own clock. */
#ifndef EHCI_MMIO_RD
#define EHCI_MMIO_RD(e, r) (*(volatile uint32_t *)((e)->op + (r)))
#endif
#ifndef EHCI_MMIO_WR
#define EHCI_MMIO_WR(e, r, value) (*(volatile uint32_t *)((e)->op + (r)) = (value))
#endif
/* Read an EHCI operational register (EHCI 2.3); model hook owns no policy. */
static uint32_t rd(struct ehci *e, unsigned r)
{
    return EHCI_MMIO_RD(e, r);
}
/* Write an EHCI operational register, preserving the existing volatile ABI. */
static void wr(struct ehci *e, unsigned r, uint32_t v)
{
    EHCI_MMIO_WR(e, r, v);
}
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
#ifdef EHCI_DELAY_ENTER
    EHCI_DELAY_ENTER();
#endif
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
        if (off<0x40 || off>0xf8 || (off&3) || (seen&(UINT64_C(1)<<(off/4)))) {
            kprintf("[ehci] %s EECP chain refused at %x (HCCPARAMS %x)\n",e->pci->name,off,hcc);
            return -1;
        }
        seen|=UINT64_C(1)<<(off/4);
        uint32_t v=pci_cfg_read(e->pci->bus,e->pci->slot,e->pci->func,off);
        if ((v&255u)==1) {
            if (touched) *touched=1;
            pci_cfg_write(e->pci->bus,e->pci->slot,e->pci->func,off,v|(1u<<24));
            uint64_t end=time_mono_ns()+NS_PER_SEC;
            unsigned tries=0;
            uint64_t t0=time_mono_ns();
            do {
                v=pci_cfg_read(e->pci->bus,e->pci->slot,e->pci->func,off);
                if (!(v&(1u<<16))) break;
                if (++tries==100000000 || time_mono_ns()>=end) {
#ifndef EHCI_NEGCTL_NO_FORCE
                    /* The X79-VG4's AMI BIOS never releases (photos,
                     * 2026-10-31). Linux quirk_usb_disable_ehci() waits 1 s
                     * and then clears the BIOS semaphore itself. Here Bus
                     * Master is already off (pci_command_quiet), so the
                     * BIOS's schedule cannot DMA; the SMI enables are
                     * cleared below as on the ordinary path. */
                    kprintf("[ehci] %s BIOS kept ownership for 1 s (USBLEGSUP=%x): taking it\n",e->pci->name,v);
                    pci_cfg_write(e->pci->bus,e->pci->slot,e->pci->func,off,(v&~(1u<<16))|(1u<<24));
                    v=pci_cfg_read(e->pci->bus,e->pci->slot,e->pci->func,off);
                    if (v&(1u<<16)) return -1;
                    break;
#else
                    return -1;
#endif
                }
                io_relax();
            } while (1);
            if (!(v&(1u<<16)) && time_mono_ns()-t0<NS_PER_SEC)
                kprintf("[ehci] %s BIOS released ownership in %lu ms\n",e->pci->name,
                        (unsigned long)((time_mono_ns()-t0)/1000000u));
            /* Linux quirk_usb_disable_ehci() does not read the OS semaphore
             * back either: once the BIOS bit is clear nobody else owns it */
            if (!(v&(1u<<24)))
                kprintf("[ehci] %s OS semaphore reads back clear (USBLEGSUP=%x)\n",e->pci->name,v);
            uint32_t ctl=pci_cfg_read(e->pci->bus,e->pci->slot,e->pci->func,off+4);
            pci_cfg_write(e->pci->bus,e->pci->slot,e->pci->func,off+4,
                          (ctl&~0xe03fu)|0xe03f0000u);
#ifndef EHCI_NEGCTL_WALK_PAST_LEGSUP
            /* EHCI 1.0 section 5.1 defines one legacy-support capability,
             * and once it is handed off nothing further in the chain matters.
             * On the X79-VG4 the walk refused both controllers right after a
             * clean release (photos, 2026-10-31); Linux's quirk does not
             * validate the rest of the chain at all. */
            break;
#endif
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

/* Snapshot aggregate bandwidth without reading any DMA. The diagnostic ABI
 * is append-only because the hardware panel also consumes its older fields. */
int ehci_sched_diag_get(unsigned controller_index, struct ehci_sched_diag *out)
{
    if (!out || controller_index >= EHCI_CONTROLLERS || !controllers[controller_index].used) {
        return -1;
    }
    *out = controllers[controller_index].sched_diag;
    return 0;
}

/* Keep measured high-water reservations for the next physical-machine log.
 * A rejected endpoint must not change these values or any schedule pointer. */
static void sched_measure(struct ehci *e)
{
    for (unsigned i = 0; i < EHCI_BW_UFRAMES; i++) {
        if (e->sched.bw_us[i] > e->sched_diag.max_uframe_us) {
            e->sched_diag.max_uframe_us = e->sched.bw_us[i];
        }
    }
    for (unsigned i = 0; i < EHCI_TT_MAX; i++) {
        for (unsigned frame = 0; frame < EHCI_BW_FRAMES; frame++) {
            if (e->sched.tt[i].frame_us[frame] > e->sched_diag.max_tt_frame_us) {
                e->sched_diag.max_tt_frame_us = e->sched.tt[i].frame_us[frame];
            }
        }
    }
    e->sched_diag.eps = e->intr_count;
    e->sched_diag.pse_toggles = e->pse_toggles;
}

/* One stable line at publication, first completion and each error. This is
 * evidence collection, not a claim to have identified the silent keyboard's
 * physical cause. Tokens here were copied under the endpoint gate. */
static void ep_report(struct ehci *e, struct ehci_ep *p)
{
    char status[64];
    ehci_ov_decode(p->last_qh_token, status, sizeof(status));
    kprintf("EHCI_EP hc=%u dev=%u ep=%02x spd=%s tt=%u/%u ivl=%u per=%u ph=%u uf=%u s=%02x c=%02x us=%u/%u/%u armed=%u done=%u err=%u/%u/%u/%u/%u ov=%02x(%s) tok=%08x not verified on X79-VG4 hardware\n",
            e->hc.index, p->dev_addr, p->addr,
            p->ps.speed == USB_SPEED_HIGH ? "high" : p->ps.speed == USB_SPEED_LOW ? "low" : "full",
            p->dev->tt_hub ? p->dev->tt_hub->addr : 0, p->dev->tt_port,
            p->interval, p->ps.period, p->ps.phase, p->ps.phase_uf, p->ps.smask, p->ps.cmask,
            p->ps.us_s, p->ps.us_c, p->ps.us_tt, p->armed, p->done,
            p->err_halt, p->err_xact, p->err_mmf, p->err_dbe, p->err_babble,
            p->last_qh_token & 255, status, p->last_token);
}

/* Caller holds gate. Linux qh_unlink_periodic deliberately does NOT set the
 * EHCI 4.12.2.5 I bit (its "We won't bother" decision). Keep the old next
 * intact and retain the reservation while recovery is pending, so another
 * endpoint cannot consume bandwidth still used by a prefetched QH. */
static void ep_recover(struct ehci *e, struct ehci_ep *p)
{
    if (p->state != EP_LINKED) {
        return;
    }
    ehci_ptree_unlink(&e->tree, &p->node);
    dma_wmb();
    p->unlink_ns = time_mono_ns();
    p->state = EP_UNLINKING;
    e->sched_diag.unlinks++;
}

/* A queued per-endpoint worker pins its static slot until it has run. The
 * device may already have been closed; inspect dev only under the controller
 * thread owner, which device_close also holds. No control request runs from
 * periodic_tick/int_poll (timer/INTx context). USB2 9.4.1 CLEAR_FEATURE resets
 * device DATA0 before the backend resets the safely unlinked overlay. */
static void stall_work(void *arg)
{
    struct ehci_ep *p = arg;
    struct ehci *e = p->owner;
    IO_DOMAIN_GUARD(&e->owner);
    if (!p->dev || p->closing || !e->up || e->failed) {
        IO_GUARD(&e->gate);
        p->stall_pending = 0;
        return;
    }
    int result = -1;
#ifndef EHCI_NEGCTL_NO_STALL_RECOVERY
    result = usb_clear_halt(p->dev, p->addr);
#endif
    IO_GUARD(&e->gate);
    p->stall_pending = 0;
    if (result) {
        /* A failed class control request must not pretend that either side's
         * toggle was reset. Retry in thread context, at a bounded cadence. */
        p->stall_retry_ns = time_mono_ns() + EHCI_STALL_RETRY_NS;
        if (p->stall_attempts >= EHCI_STALL_RETRIES) {
            p->closing = 1;
            kprintf("[ehci] dev=%u ep=%02x giving up after %u clear-halt attempts\n",
                    p->dev_addr, p->addr, EHCI_STALL_RETRIES);
        }
    } else {
        p->stall_retry_ns = 0;
    }
}

/* Publish a completely initialized QH pointing at an inactive dummy before
 * it becomes reachable. Linux ehci-q.c qh_make/qh_update, ehci-sched.c
 * qh_link_periodic; EHCI 3.6 forbids Mult=0 even on an unarmed endpoint.
 * Only configure (thread) calls this routine, so the first PSS handshake may
 * wait. Rearms and IRQ recovery never use it or rewrite an active overlay. */
static int ep_publish(struct ehci *e, struct ehci_ep *p)
{
#ifdef EHCI_NEGCTL_ZERO_QH_LINKED
    p->qh->link = EHCI_END;
    ehci_ptree_link(&e->tree, &p->node);
#endif
    if (qh_device(p->qh, p->dev, USB_EP_NUM(p->addr), p->packet, 0, p->ps.smask, p->ps.cmask)) {
        return -1;
    }
    ehci_qtd_dummy(p->q[0]);
    ehci_qtd_dummy(p->q[1]);
    p->qh->next = p->q_dma[0];
    dma_wmb();
    int enabling = 0;
    for (;;) {
        {
            IO_GUARD(&e->gate);
            /* Linux v6.12 ehci-sched.c enable_periodic() cancels delayed disable
             * before polling PSS. Checking PSE before gate used to let an IRQ
             * clear it between the check and publication, reversing the command
             * while PSS was still 1 (EHCI 2.3.1/2.3.2, 4.6).
             *
             * Cancel and decide under the same gate as periodic_tick. A stop
             * already issued still needs its acknowledgement: retain the check
             * deadline and drop gate before waiting in configure's thread. IRQ
             * progress never waits. Recheck after reacquiring because another
             * thread may have enabled the schedule while this one was waiting. */
            if (e->failed) {
                return -1;
            }
            e->pse_off_deadline = 0;
            uint32_t command = rd(e, OP_CMD);
            if ((command & CMD_PSE) || !(rd(e, OP_STS) & STS_PSS)) {
#ifdef EHCI_NEGCTL_ZERO_QH_LINKED
                p->node.linked = 0;
#endif
                ehci_ptree_link(&e->tree, &p->node);
                p->state = EP_LINKED;
                e->intr_count++;
                e->pse_check_deadline = 0;
                if (!(command & CMD_PSE)) {
                    wr(e, OP_CMD, command | CMD_PSE);
                    e->pse_toggles++;
                    enabling = 1;
                }
                sched_measure(e);
                ep_report(e, p);
                break;
            }
        }
        if (wait_reg(e, OP_STS, STS_PSS, 0, EHCI_PSS_TIMEOUT_MS)) {
            failed(e, "periodic schedule disable was not acknowledged");
            return -1;
        }
    }
    if (enabling && wait_reg(e, OP_STS, STS_PSS, STS_PSS, EHCI_PSS_TIMEOUT_MS)) {
        failed(e, "periodic schedule did not start");
        return -1;
    }
    return 0;
}

/* IRQ/timer progress only: the old periodic_service stopped every endpoint
 * for each packet. Correction: only an errored QH is unlinked, its overlay is
 * refreshed after Linux ehci-timer.c UNLINK_INTR=1125 us, then relinked.
 * DISABLE_PERIODIC=10 ms and the PSS timeout are also timestamp transitions,
 * not delay()/wait_reg() calls beneath usb_hc_irq. Caller holds gate. */
static void periodic_tick(struct ehci *e)
{
    if (!e->up || e->failed) {
        return;
    }
    uint64_t now = time_mono_ns();
    for (unsigned i = 0; i < EHCI_ENDPOINTS; i++) {
        struct ehci_ep *p = &e->ep[i];
        if (!p->dev || p->type != USB_XFER_INT || p->closing) {
            continue;
        }
        if (p->state == EP_UNLINKING && now - p->unlink_ns >= EHCI_UNLINK_NS) {
            if (p->stall_retry_ns) {
                if (!p->stall_pending && now >= p->stall_retry_ns) {
                    if (p->stall_attempts >= EHCI_STALL_RETRIES) {
                        p->closing = 1;
                        kprintf("[ehci] dev=%u ep=%02x giving up after %u clear-halt attempts\n",
                                p->dev_addr, p->addr, EHCI_STALL_RETRIES);
                    } else {
                        p->stall_attempts++;
                        p->stall_pending = 1;
                        work_queue(&p->stall_work);
                    }
                }
                continue;
            }
            if (p->stall_pending) {
                continue;
            }
            /* Linux qh_update()/qh_refresh(): software owns the overlay only
             * after the unlink grace period. Preserve DTC=0's hardware toggle
             * on transaction errors; only clear_halt is allowed to reset it. */
            ehci_qtd_dummy(p->q[p->cur]);
            p->qh->current = 0;
            p->qh->next = p->q_dma[p->cur];
            p->qh->alt = EHCI_END;
            p->qh->token &= EHCI_TOGGLE;
            dma_wmb();
            ehci_ptree_link(&e->tree, &p->node);
            p->state = EP_LINKED;
            e->sched_diag.relinks++;
        }
        if (p->armed && !p->done && !p->reported_stalled && now - p->arm_ns >= EHCI_INPUT_WARN_NS) {
            /* EHCI 1.0 4.10.3: interrupt IN can NAK indefinitely with Active
             * still set. EHCI_STALLED is the existing log key, not a fault
             * classification. Observe once; never recover, unlink or rewrite
             * this live qTD merely because no input has been produced. */
            char status[64];
            dma_rmb();
            uint32_t overlay = p->qh->token;
            uint32_t frame = rd(e, OP_FRAME);
            ehci_ov_decode(overlay, status, sizeof(status));
            p->reported_stalled = 1;
            kprintf("EHCI_STALLED hc=%u dev=%u ep=%02x armed_ms=%lu ov=%02x(%s) frindex=%u previous=%u delta=%u usbsts=%08x not verified on X79-VG4 hardware\n",
                    e->hc.index, p->dev_addr, p->addr, (unsigned long)((now - p->arm_ns) / NS_PER_MS),
                    overlay & 255, status, frame, p->arm_frame, (frame - p->arm_frame) & 0x3fff,
                    rd(e, OP_STS));
        }
    }
    if (!e->intr_count && e->pse_off_deadline && now >= e->pse_off_deadline) {
        e->pse_off_deadline = 0;
        if (rd(e, OP_CMD) & CMD_PSE) {
            wr(e, OP_CMD, rd(e, OP_CMD) & ~CMD_PSE);
            e->pse_toggles++;
            e->sched_diag.pse_toggles = e->pse_toggles;
            e->pse_check_deadline = now + EHCI_PSS_TIMEOUT_MS * NS_PER_MS;
        }
    }
    if (e->pse_check_deadline) {
        if (!!(rd(e, OP_CMD) & CMD_PSE) == !!(rd(e, OP_STS) & STS_PSS)) {
            e->pse_check_deadline = 0;
        } else if (now >= e->pse_check_deadline) {
            /* IRQ-safe refusal retains all DMA; shutdown owns the full drain. */
            e->failed = 1;
            wr(e, OP_INTR, 0);
            wr(e, OP_CMD, rd(e, OP_CMD) & ~CMD_RUN);
        }
    }
    if (!e->reported_sched && e->reports >= 8) {
        e->reported_sched = 1;
        kprintf("EHCI_PERIODIC hc=%u eps=%u pse_toggles=%u rearms=%u unlinks=%u max_uframe_us=%u max_tt_us=%u not verified on X79-VG4 hardware\n",
                e->hc.index, e->intr_count, e->pse_toggles, e->sched_diag.rearms,
                e->sched_diag.unlinks, e->sched_diag.max_uframe_us, e->sched_diag.max_tt_frame_us);
    }
}

/* Linux qh_append_tds dummy swap: edit only an inactive qTD, publish the new
 * dummy and buffer pointers, dma_wmb(), then Active LAST (EHCI 4.10.2).
 * A pending recovery makes arm a harmless no-op until the next callback.
 * The host HC model's 200-report FIFO scenario measured max completion-to-
 * Active latency 9015 us with a 10 ms tick, 265 us with a 1 ms tick. These
 * are model-clock observations, not verified on X79-VG4 hardware. */
static int int_arm(struct usb_device *d,uint8_t addr)
{
    struct ehci *e=controller(d);
    if (!e) return -1;
    IO_GUARD(&e->gate);
    struct ehci_ep *p=endpoint(e,d,addr);
    if (!e->up || e->failed || !p || p->type!=USB_XFER_INT) return -1;
    /* Permanent STALL refusal is terminal, unlike the temporary unlink wait.
     * Checking UNLINKING first would claim success forever after giving up. */
    if (p->closing) {
        return -1;
    }
    if (p->armed || p->state == EP_UNLINKING) {
        return 0;
    }
    if (p->state != EP_LINKED) {
        return -1;
    }
    unsigned dummy = p->cur;
    unsigned successor = dummy ^ 1;
    uint32_t token;
    ehci_qtd_dummy(p->q[successor]);
#ifdef EHCI_NEGCTL_ACTIVATE_BEFORE_LINK
    EHCI_HW_STORE(&p->q[dummy]->token, EHCI_ACTIVE);
#endif
    if (ehci_qtd_fill_in(p->q[dummy], p->q_dma[successor], p->payload_dma, p->packet, &token)) {
        return -1;
    }
#ifdef EHCI_NEGCTL_TOGGLE_RESET_ON_REARM
    p->qh->token &= ~EHCI_TOGGLE;
#endif
    dma_wmb();
    EHCI_HW_STORE(&p->q[dummy]->token, token);
    p->armed = 1;
    p->td = p->q[dummy];
    p->td_dma = p->q_dma[dummy];
    p->rearms++;
    e->sched_diag.rearms++;
    if (!p->arm_ns) {
        p->arm_ns = time_mono_ns();
        p->arm_frame = rd(e, OP_FRAME);
    }
#ifdef EHCI_NEGCTL_REARM_STOPS_PSE
    wr(e, OP_CMD, rd(e, OP_CMD) & ~CMD_PSE);
#endif
    return 0;
}
/* Consume each finished qTD once and freeze diagnostic status. Rearming does
 * not reconstruct QH/toggle; transaction errors recover only this QH and a
 * real STALL schedules CLEAR_FEATURE on the worker thread. */
static int int_poll(struct usb_device *d,uint8_t addr,uint8_t **data)
{
    struct ehci *e=controller(d);
    if (!e || !data) return -1;
    IO_GUARD(&e->gate);
    struct ehci_ep *p=endpoint(e,d,addr);
    if (!e->up || !p || !p->armed) return -1;
    dma_rmb();
    uint32_t t=p->q[p->cur]->token;
    if (t&EHCI_ACTIVE) return -1;
    p->armed=0;
    p->last_token=t;
    /* copied here, under the gate: the panel must never read QH DMA memory
     * that controller teardown can free (Astra round 7) */
    p->last_qh_token=p->qh->token;
    int n=ehci_actual(p->packet,t);
    if (n < 0) {
        e->errors++;
        p->errs++;
        /* EHCI 3.5.3: Halted=6, XactErr=3, MMF=2, DBE=5, Babble=4. */
        p->err_halt += !!(t & EHCI_HALTED);
        p->err_xact += !!(t & (1u << 3));
        p->err_mmf += !!(t & (1u << 2));
        p->err_dbe += !!(t & (1u << 5));
        p->err_babble += !!(t & (1u << 4));
        /* EHCI 3.5.3: bare Halted with nonzero CERR is STALL. CERR=0
         * transaction errors are never cleared with a fabricated TT reset.
         * USB2 11.24.2.3 Clear_TT_Buffer excludes interrupt endpoints. */
        if ((t & EHCI_HALTED) && !(t & 0x3c)) {
            p->stalls++;
            p->stall_retry_ns = time_mono_ns() + EHCI_STALL_RETRY_NS;
        }
        ep_report(e, p);
        ep_recover(e, p);
        return -1;
    }
    p->done++;
    p->toggle=!!(p->qh->token&EHCI_TOGGLE);
    p->cur ^= 1;
    p->stall_attempts = 0;
    if (p->done == 1) {
        ep_report(e, p);
    }
    *data=p->payload; e->completions++;e->reports++;
    return n;
}
/* Admit before allocating the non-freeable arena, then initialize-before-link.
 * A failure changes neither PSE nor existing endpoints. This intentionally
 * does not roll back an earlier successfully configured sibling interface. */
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
        for (unsigned j=0;j<EHCI_ENDPOINTS;j++) {
            if (!e->ep[j].dev && !e->ep[j].stall_pending) {
                p = &e->ep[j];
                break;
            }
        }
        if (!p) return -1;
        *p=(struct ehci_ep){.addr=desc->addr,.type=type,.packet=packet};
        if (type == USB_XFER_BULK) {
            IO_GUARD(&e->gate);
            p->dev = d;
            continue;
        }
        unsigned think_ns = d->tt_hub ? 666u * (1u + d->tt_hub->hub_tt_think) : 0;
        /* Linux v6.12 hub.c hub_configure: wHubCharacteristics[6:5] encodes
         * think_time=666*(1+value) ns. Do not use the raw two-bit value as ns. */
        if (ehci_ps_make(&p->ps, d->speed, 1, packet, desc->interval, think_ns)) {
            memset(p, 0, sizeof(*p));
            return -1;
        }
#ifdef EHCI_NEGCTL_LEAK_ARENA
        p->mem=arena(e,4096);
#endif
        int admitted;
        {
            IO_GUARD(&e->gate);
            if (d->speed != USB_SPEED_HIGH && d->tt_hub) {
                p->tt = ehci_sched_tt(&e->sched, d->tt_hub->addr, d->tt_port, d->tt_multi);
            }
            admitted = ehci_sched_admit(&e->sched, p->tt, &p->ps);
        }
        if (admitted) {
            kprintf("[ehci] periodic bandwidth unavailable addr=%u ep=%x interval=%u\n",d->addr,desc->addr,desc->interval);
            e->sched_diag.refused++;
            {
                IO_GUARD(&e->gate);
                ehci_sched_release(&e->sched, p->tt, &p->ps);
            }
#ifdef EHCI_NEGCTL_NO_PERIODIC_RESTORE
            /* Existing named control now restores the old refusal defect,
             * rather than testing the removed stop/restart implementation. */
            command(e, CMD_PSE, 0);
#endif
            memset(p,0,sizeof(*p));
            return -1;
        }
#ifndef EHCI_NEGCTL_LEAK_ARENA
        p->mem = arena(e, 4096);
#endif
        if (!p->mem) {
            IO_GUARD(&e->gate);
            ehci_sched_release(&e->sched, p->tt, &p->ps);
            memset(p, 0, sizeof(*p));
            return -1;
        }
        /* EHCI 3.5/3.6: QH and qTDs require 32-byte alignment. Separate cache
         * lines at 0/128/192 and payload@256 fit any supported single packet. */
        p->qh = p->mem->cpu;
        p->qh_dma = (uint32_t)dma_addr_value(p->mem->dma);
        p->q[0] = (void *)((uint8_t *)p->mem->cpu + 128);
        p->q[1] = (void *)((uint8_t *)p->mem->cpu + 192);
        p->q_dma[0] = p->qh_dma + 128;
        p->q_dma[1] = p->qh_dma + 192;
        p->td = p->q[0];
        p->td_dma = p->q_dma[0];
        p->payload = (uint8_t *)p->mem->cpu + 256;
        p->payload_dma = p->qh_dma + 256;
        p->dev = d;
        p->dev_addr = d->addr;
        p->interval = desc->interval;
        p->owner = e;
        p->node = (struct ehci_pnode){.hw_next = &p->qh->link, .dma = p->qh_dma,
            .period = p->ps.period, .phase = p->ps.phase};
        work_item_init(&p->stall_work, stall_work, p);
        if (ep_publish(e, p)) {
            /* A failed enable may have prefetched this QH. Keep both endpoint
             * and arena quarantined; shutdown must prove HCHalted first. */
            return -1;
        }
    }
    return 0;
}
static int configure_hub(struct usb_device *d,int ports,int multi,int think)
{
    (void)multi; (void)think;
    return d && ports>0 && ports<=127 ? 0 : -1;
}
/* CLEAR_FEATURE has already succeeded at the core. Reset bulk software state
 * or an interrupt overlay only while that QH is safely outside the schedule. */
static int clear_halt(struct usb_device *d,uint8_t addr)
{
    struct ehci *e=controller(d);
    if (!e) return -1;
    IO_DOMAIN_GUARD(&e->owner);
    struct ehci_ep *p=endpoint(e,d,addr);
    if (!p || e->failed) return -1;
    if (p->type == USB_XFER_INT) {
        IO_GUARD(&e->gate);
        if (p->state != EP_UNLINKING || time_mono_ns() - p->unlink_ns < EHCI_UNLINK_NS) {
            return -1;
        }
        /* USB2 9.4.1 and Linux qh_update: device already accepted CLEAR_HALT;
         * the unlinked overlay is now safe to reset to DATA0. */
        p->qh->token &= ~EHCI_TOGGLE;
        p->toggle = 0;
        return 0;
    }
    if (p->type != USB_XFER_BULK) return -1;
    p->toggle=0; /* bulk QH is reconstructed for every transfer */
    return 0;
}
/* Thread-only close. First unlink all of this device's QHs under gate, then
 * wait outside it for Linux's 9-microframe grace period. Other devices keep
 * running throughout; the controller arena is retained until final halt. */
static void device_close(struct usb_device *d)
{
    struct ehci *e=controller(d);
    if (!e || !d->hcpriv) return;
    IO_DOMAIN_GUARD(&e->owner);
    if (e->dma.blocked) return; /* keep the hardware-visible schedule intact */
    int unlinked = 0;
    {
        IO_GUARD(&e->gate);
        for (unsigned i = 0; i < EHCI_ENDPOINTS; i++) {
            struct ehci_ep *p = &e->ep[i];
            if (p->dev != d || p->type != USB_XFER_INT) {
                continue;
            }
            ep_recover(e, p);
            p->closing = 1;
            p->armed = 0;
            unlinked = 1;
        }
    }
#ifndef EHCI_NEGCTL_UNLINK_NO_WAIT
    /* delay() accepts milliseconds; 2 ms rounds 1125 us upward and, unlike a
     * tick handshake, also works with a 10 ms PIT. Never hold gate here. */
    if (unlinked && delay(2)) {
        failed(e, "device close unlink grace did not elapse");
        return;
    }
#else
    (void)unlinked;
#endif
    IO_GUARD(&e->gate);
    for (unsigned i=0;i<EHCI_ENDPOINTS;i++) {
        struct ehci_ep *p=&e->ep[i];
        if (p->dev!=d) continue;
        if (p->type == USB_XFER_INT) {
            ehci_sched_release(&e->sched, p->tt, &p->ps);
            e->intr_count--;
#ifdef EHCI_NEGCTL_UNLINK_NO_WAIT
            p->qh->token = 0;
            dma_wmb();
#endif
        }
        /* Coherent endpoint memory remains in the controller ledger until
         * HCHalted at shutdown. This also makes failed-stop pointers stable. */
        if (p->stall_pending) {
            /* work.c has no cancellation primitive. Do not memset its next
             * pointer or reuse this slot until the queued worker observes the
             * tombstone. owner serializes an already running worker too. */
            p->dev = NULL;
        } else {
            memset(p,0,sizeof(*p));
        }
    }
    memset(d->hcpriv,0,sizeof(struct ehci_dev)); d->hcpriv=NULL;
    if (!e->intr_count) {
        e->pse_off_deadline = time_mono_ns() + EHCI_PSE_OFF_NS;
    }
    e->sched_diag.eps = e->intr_count;
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
    periodic_tick(e);
}
static void timer_tick(struct ktimer *t)
{
    struct ehci *e=t->arg;
    usb_hc_irq(&e->hc);
    uint64_t irqs=dev_irq_count(e->pci);
    if (!irqs && !e->reported_poll && e->intr_count) {
        e->reported_poll = 1;
        kprintf("[ehci] hc=%u no IRQ observed; periodic input polling active (not verified on X79-VG4 hardware)\n", e->hc.index);
    }
    if (!e->reported_irq && e->reports && irqs) {
        e->reported_irq=1;
        kprintf("EHCI_IRQ vector=%d delivered=%lu reports=%lu\n",e->pci->irq_vec,(unsigned long)irqs,e->reports);
    }
}
/* EHCI 2.3.3 USBINTR bits 0/1/2/4: qTD completion/error, port change and HSE.
 * Progress is independently timer-driven even if INTx never arrives. A 1 ms
 * requested period is rounded by the 100 Hz PIT to 10 ms on that clock; dummy
 * swapping therefore guarantees one tick, not an invented 1 kHz HID rate. */
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
    ehci_sched_init(&e->sched);
    ehci_ptree_init(&e->tree, e->frames);
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
    /* The core calls irq_enable only when a vector was obtained. A missing
     * vector must still install our input progress timer; enabling USBINTR
     * alone would leave a NAK-only keyboard dependent on an absent IRQ. */
    if (e->timer.heap_idx < 0) {
        irq_enable(&e->hc);
    }
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
