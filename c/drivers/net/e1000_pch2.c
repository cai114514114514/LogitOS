/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stddef.h>
#include "e1000_pch2.h"
#include "netdev.h"
#include "e1000e_ring.h"
#include "pci.h"
#include "dma.h"
#include "ktime.h"
#include "net.h"
#include "kprintf.h"
#ifndef memset
void *memset(void *, int, size_t);
#endif
#ifndef memcpy
void *memcpy(void *, const void *, size_t);
#endif

/* Intel 82579LM/V + PCH2 MAC, not the discrete 82574 initialization path.
 * Register facts: Intel 82579 datasheet rev 2.1, chapters 10 and 12:
 * https://www.mouser.com/pdfdocs/82579datasheetvol21.pdf
 * Silicon sequencing/errata cross-check: Intel's Linux v6.12 e1000e
 * ich8lan.c (pch2lan reset, lv workarounds, copper setup) and phy.c (HV MDIO).
 * Only the common legacy descriptor layout is shared with e1000e_ring.h.
 *
 * Deliberately narrow bring-up: one PCH MAC, D0, hardware-autoloaded NVM,
 * accessible 82579 PHY, no managed firmware, 1500 MTU, coherent DMA32,
 * INTx through netdev_irq_route plus polling. FW_VALID is NOT an ownership
 * denial: it selects a ME coexistence path we have not implemented. Both LM
 * and V can have management firmware. Report that policy refusal separately
 * from a denied SWFLAG or PHY-reset permission. No flash access/writes,
 * NVM repair, LANPHYPC power-cycle recovery, EEE, K1, WoL, suspend or MSI.
 * Host MMIO tests exercise this production TU; no 82579 QEMU model or physical
 * NIC was available. A model pass must not be called a working physical link.
 */
#define P_CTRL 0x0000u
#define P_STATUS 0x0008u
#define P_EXT 0x0018u
#define P_MDIC 0x0020u
#define P_FEXT 0x0028u
#define P_KMRN 0x0034u
#define P_FEXT3 0x003cu
#define P_ICR 0x00c0u
#define P_ITR 0x00c4u
#define P_IMS 0x00d0u
#define P_IMC 0x00d8u
#define P_IAM 0x00e0u
#define P_RCTL 0x0100u
#define P_TCTL 0x0400u
#define P_TIPG 0x0410u
#define P_EXCNF 0x0f00u
#define P_EXSIZE 0x0f08u
#define P_PHYCTRL 0x0f10u
#define P_PBA 0x1000u
#define P_RX 0x2800u
#define P_RXDCTL 0x2828u
#define P_TX 0x3800u
#define P_TXDCTL 0x3828u
#define P_TARC 0x3840u
#define P_RXCSUM 0x5000u
#define P_RFCTL 0x5008u
#define P_RAL 0x5400u
#define P_RAH 0x5404u
#define P_WUC 0x5800u
#define P_FWSM 0x5b54u
#define P_GCR 0x5b00u
#define P_MASTER_OFF (1u << 2)
#define P_MASTER_ON (1u << 19)
#define P_RESET (1u << 26)
#define P_PHY_RESET (1u << 31)
#define P_SWFLAG (1u << 5)
#define P_GATE (1u << 7)
#define P_FW_VALID (1u << 15)
#define P_PHY_PERMIT (1u << 6)
#define P_DRV_LOAD (1u << 28)
#define P_LAN_DONE (1u << 9)
#define P_IRQS ((1u << 7) | (1u << 2) | (1u << 5))

static struct {
    struct device *pci;
    volatile uint8_t *mmio;
    struct dma_device dma;
    struct dma_buffer *rings, *rxbuf, *txbuf;
    volatile struct e1k_rx_desc *rx;
    volatile struct e1k_tx_desc *tx;
    unsigned rxhead, txhead, txtail, pending;
    unsigned rx_good, rx_bad, tx_good, tx_bad;
    uint32_t link;
    int online, sw_owned, claimed, poisoned, link_seen, reset_unconfirmed;
    net_rx_cb callback;
} pch;

/* Only the two MMIO primitives are replaced by the host apparatus. All
 * ownership, deadlines, PHY page selection, descriptor and cleanup decisions
 * remain the same functions linked into the kernel. */
#ifdef E1000_PCH2_HOST
uint32_t pch_model_read(unsigned r);
void pch_model_write(unsigned r, uint32_t v);
static uint32_t pr(unsigned r) { return pch_model_read(r); }
static void pw(unsigned r, uint32_t v) { pch_model_write(r,v); }
#else
static uint32_t pr(unsigned r) { return *(volatile uint32_t *)(pch.mmio+r); }
static void pw(unsigned r,uint32_t v) { *(volatile uint32_t *)(pch.mmio+r)=v; }
#endif
static void pflush(void) { (void)pr(P_STATUS); }
static int pdelay(unsigned us)
{
    uint64_t start=time_mono_ns();
    for(unsigned i=0;i<10000000;i++) {
        if(time_mono_ns()-start >= (uint64_t)us*NS_PER_US) return 0;
        __asm__ volatile("" ::: "memory");
    }
    return -1; /* a stopped clock is failure, never a fabricated delay */
}
static int pwait(unsigned r,uint32_t mask,uint32_t expected,unsigned us)
{
    uint64_t start=time_mono_ns();
    for(unsigned i=0;i<10000000;i++) {
        uint32_t v=pr(r);
        if(v==UINT32_MAX) return -1;
        if((v&mask)==expected) return 0;
        if(time_mono_ns()-start >= (uint64_t)us*NS_PER_US) break;
    }
    return -1;
}
static int pwrite(unsigned r,uint32_t v,uint32_t mask)
{ pw(r,v); return pwait(r,mask,v&mask,1000); }
static uint16_t pcfg(unsigned off)
{ return pci_cfg_read16(pch.pci->bus,pch.pci->slot,pch.pci->func,off); }
static void pcfgwrite(unsigned off,uint16_t v)
{ pci_cfg_write16(pch.pci->bus,pch.pci->slot,pch.pci->func,off,v); }
static int pci_isolate(void)
{
    uint16_t c=pcfg(PCI_CFG_COMMAND);
    if(c==UINT16_MAX) return -1;
    pcfgwrite(PCI_CFG_COMMAND,(c&~PCI_CMD_MASTER)|PCI_CMD_INTX_DIS);
    c=pcfg(PCI_CFG_COMMAND);
    return c!=UINT16_MAX && !(c&PCI_CMD_MASTER) && (c&PCI_CMD_INTX_DIS) ? 0:-1;
}
static int firmware_policy(void)
{
    uint32_t fw=pr(P_FWSM);
    if(fw==UINT32_MAX) return -1;
#ifndef PCH2_NEGCTL_IGNORE_FW
    if(fw&P_FW_VALID) {
        kprintf("[e1000-pch2] decline: managed-firmware path not implemented FWSM=%x\n",fw);
        return -1;
    }
#endif
    if(pwait(P_FWSM,P_PHY_PERMIT,P_PHY_PERMIT,300000)) {
        kprintf("[e1000-pch2] decline: firmware denies PHY reset FWSM=%x\n",pr(P_FWSM));
        return -1;
    }
    return 0;
}
static int sw_acquire(void)
{
    if(pch.sw_owned) return -1;
    /* Waiting for an existing flag is essential. Setting an already-set flag
     * then seeing 1 does not transfer ownership from another software agent. */
#ifndef PCH2_NEGCTL_STEAL_SWFLAG
    if(pwait(P_EXCNF,P_SWFLAG,0,100000)) return -1;
#endif
    pw(P_EXCNF,pr(P_EXCNF)|P_SWFLAG);
    if(pwait(P_EXCNF,P_SWFLAG,P_SWFLAG,100000)) return -1;
    pch.sw_owned=1;
    return 0;
}
static int sw_release(void)
{
    if(!pch.sw_owned) return 0;
    if(pwrite(P_EXCNF,pr(P_EXCNF)&~P_SWFLAG,P_SWFLAG)) return -1;
    pch.sw_owned=0;
    return 0;
}
static int mdio(unsigned addr,unsigned reg,uint16_t *v,int write)
{
    if(!pch.sw_owned || addr>31 || reg>31) return -1;
    pw(P_MDIC,(addr<<21)|(reg<<16)|(write?(1u<<26)|*v:1u<<27));
    if(pwait(P_MDIC,1u<<28,1u<<28,10000)) return -1;
    uint32_t got=pr(P_MDIC);
    if((got&(1u<<30)) || ((got>>16)&31)!=reg || ((got>>21)&31)!=addr) return -1;
    if(!write) *v=(uint16_t)got;
    /* PCH2 needs a gap even after READY, unlike discrete 82574. */
    return pdelay(100);
}
static int phy(unsigned page,unsigned reg,uint16_t *v,int write)
{
    if(reg>30 || (page && (page<768 || page>=800))) return -1;
    unsigned addr=page>=768?1:2;
#ifdef PCH2_NEGCTL_WRONG_PHY_ADDR
    addr=1;
#endif
    if(reg>15) {
        /* HV page 768 aliases page zero at address 1. The page selector itself
         * is ALWAYS address 1/register 31, also for address 2's page zero. */
        uint16_t select=(page==768?0:page)<<5;
        if(mdio(1,31,&select,1)) return -1;
    }
    return mdio(addr,reg,v,write);
}
static int phy_update(unsigned page,unsigned reg,uint16_t clear,uint16_t set)
{
    uint16_t v,got;
    if(phy(page,reg,&v,0)) return -1;
    v=(v&~clear)|set;
    return phy(page,reg,&v,1) || phy(page,reg,&got,0) || ((got^v)&(clear|set)) ? -1:0;
}
static int emi_write(uint16_t addr,uint16_t value)
{
    uint16_t got;
    return phy(0,16,&addr,1) || phy(0,17,&value,1) || phy(0,17,&got,0) || got!=value ? -1:0;
}
static int kmrn(unsigned reg,uint16_t *value,int write)
{
    pw(P_KMRN,(reg<<16)|(write?*value:1u<<21));
    pflush();
    if(pdelay(2)) return -1;
    if(!write) *value=pr(P_KMRN);
    return 0;
}
static int phy_setup(void)
{
    uint16_t id1,id2,v,check;
    if(sw_acquire()) return -1;
    int rc=-1;
    if(phy(0,2,&id1,0)||phy(0,3,&id2,0)) goto out;
    if((((uint32_t)id1<<16)|id2)>>4 != 0x0154009u) {
        kprintf("[e1000-pch2] decline: unsupported PHY %x\n",((uint32_t)id1<<16)|id2); goto out;
    }
    /* Intel LV post-reset settings. EEE and K1 remain disabled at every link
     * speed, avoiding a link-change-time MDIO transaction in the IRQ path. */
    if(phy_update(769,16,0,0x0400) ||
       emi_write(0x084f,0x0034) || emi_write(0x2411,0x0005) ||
       emi_write(0x4805,0x1387) || emi_write(0x040e,0) ||
       phy_update(772,20,0x6000,0) || phy_update(770,17,0x4000,0) ||
       phy_update(769,17,0x0010,0)) goto out; /* clear host wakeup */
    /* Respect the board's global GbE-disable strap; do not turn a deliberately
     * 100-Mb/s OEM setup into a fabricated gigabit capability. */
    if(phy_update(768,25,0x0044,(pr(P_PHYCTRL)&0x40)?0x40:0) ||
       phy_update(0,22,0,0x8c00) || phy_update(0,18,0x0600,0x0400)) goto out;
    v=0xffff; if(kmrn(4,&v,1)||kmrn(4,&check,0)||check!=v) goto out;
    if(kmrn(9,&v,0)) goto out;
    v|=0x3f;
    if(kmrn(9,&v,1)||kmrn(9,&check,0)||check!=v) goto out;
    if(phy_update(0,4,0x0fe0,0x01e1) ||
       phy_update(0,9,0x1300,(pr(P_PHYCTRL)&0x40)?0:0x0200)) goto out;
    if(phy(0,0,&v,0)) goto out;
    v=(v&~0xcc00u)|0x1200u; /* power up, no loopback/isolate, autoneg restart */
    if(phy(0,0,&v,1)||phy(0,0,&check,0)||(check&0x5c00)!=0x1000) goto out;
    rc=0;
out:
    if(sw_release()) rc=-1;
    return rc;
}
static int master_stop(void)
{
    pw(P_IMC,UINT32_MAX);
    int mask_failed=pwait(P_IMS,UINT32_MAX,0,1000);
    (void)pr(P_ICR); /* clear latched causes even when RX/TX are already idle */
    if(pwrite(P_RCTL,pr(P_RCTL)&~2u,2) || pwrite(P_TCTL,pr(P_TCTL)&~2u,2)) return -1;
    pw(P_CTRL,pr(P_CTRL)|P_MASTER_OFF); pflush();
#ifndef PCH2_NEGCTL_NO_MASTER_DRAIN
    if(pwait(P_STATUS,P_MASTER_ON,0,100000)) return -1;
#endif
    return pci_isolate() || mask_failed ? -1:0;
}
static void free_dma(void)
{
    while(pch.dma.buffers) if(dma_free_coherent(pch.dma.buffers)) break;
    pch.rings=pch.rxbuf=pch.txbuf=NULL;
    pch.rx=NULL; pch.tx=NULL;
}
static void dispose(void)
{
    pch.online=0; pch.callback=NULL;
    if(pch.reset_unconfirmed) {
        /* Do not read-modify-write CTRL while RST might still be asserted:
         * that can restart reset, followed by the forbidden early flush.
         * Config space is the only safe isolation path in this state. */
        (void)pci_isolate();dma_device_quarantine(&pch.dma);pch.poisoned=1;
        kprintf("[e1000-pch2] reset unconfirmed: config-isolated, no further MMIO\n");
        return;
    }
    if(master_stop() || sw_release()) {
        (void)pci_isolate();
        dma_device_quarantine(&pch.dma); pch.poisoned=1;
        kprintf("[e1000-pch2] stop/ownership unconfirmed: quarantined, rebind refused\n");
        return;
    }
    if(pch.claimed && pwrite(P_EXT,pr(P_EXT)&~P_DRV_LOAD,P_DRV_LOAD)) {
        dma_device_quarantine(&pch.dma); pch.poisoned=1; return;
    }
    pch.claimed=0;
    dma_device_quiesced(&pch.dma); free_dma();
    pch.mmio=NULL; pch.pci=NULL;
}
static void reap(void)
{
    while(pch.pending) {
        uint8_t s=pch.tx[pch.txtail].status;
        if(!(s&1)) break;
        dma_rmb();
        if(s&0x0e) pch.tx_bad++; else pch.tx_good++;
        pch.txtail=(pch.txtail+1)&(E1K_TX_COUNT-1); pch.pending--;
    }
}
static void link_report(void)
{
    uint32_t status=pr(P_STATUS)&0xc3u;
    if(pch.link_seen && pch.link==status) return;
    pch.link=status; pch.link_seen=1;
    kprintf("[e1000-pch2] link %s %u Mbps %s duplex\n",status&2?"UP":"DOWN",
            status&2?(status&0x80?1000:status&0x40?100:10):0,status&1?"full":"half");
    pw(P_TCTL,(pr(P_TCTL)&~(0x3ffu<<12))|((status&1?63u:511u)<<12));
}
static int pch_tx(const void *frame,uint16_t len)
{
    NET_GUARD;
    if(!pch.online || !frame || len<14 || len>E1K_MAX_FRAME || !(pr(P_STATUS)&2)) return -1;
    reap();
    if(pch.pending>=E1K_TX_COUNT-1) return -1;
    volatile struct e1k_tx_desc *d=&pch.tx[pch.txhead];
    memcpy((uint8_t *)pch.txbuf->cpu+pch.txhead*E1K_BUFFER,frame,len);
    d->length=len; d->cso=d->css=0; d->special=0; d->cmd=0x0b; d->status=0;
    dma_wmb();
    pch.txhead=(pch.txhead+1)&(E1K_TX_COUNT-1); pch.pending++;
    pw(P_TX+0x18,pch.txhead);
    return 0; /* queued, never misreported as a hardware completion */
}
static int pch_poll(net_rx_cb cb)
{
    NET_GUARD;
    if(!pch.online || !cb) return 0;
    uint32_t fw=pr(P_FWSM);
    if(fw==UINT32_MAX || (fw&P_FW_VALID)) {
        /* A late firmware-mode change is not an invitation to reset the PHY.
         * Stop only the host DMA path; no reset/MDIO and retain on failure. */
        pch.online=0; pch.callback=NULL; pch.poisoned=1;
        if(master_stop()) { (void)pci_isolate(); dma_device_quarantine(&pch.dma); }
        else dma_device_quiesced(&pch.dma);
        /* The device and netif are still bound. Keep their identity and DMA
         * handles for the later remove callback; never silently rebind here.
         * PHY_PERMIT is only reset permission, not run-time MDIO ownership. */
        kprintf("[e1000-pch2] firmware mode changed: interface silenced, retained until remove\n");
        return 0;
    }
    uint32_t cause=pr(P_ICR);
    (void)cause; /* ICR clears on a real asserted interrupt, or with IMS=0 */
    reap(); link_report();
    int n=0; unsigned consumed=0,tail=0;
    for(unsigned budget=0;budget<E1K_RX_COUNT;budget++) {
        volatile struct e1k_rx_desc *d=&pch.rx[pch.rxhead];
        uint8_t status=d->status;
        if(!(status&1)) break;
        dma_rmb();
#ifdef PCH2_NEGCTL_ACCEPT_RX_ERROR
        int valid=(status&3)==3 && d->length>=14 && d->length<=E1K_MAX_FRAME;
#else
        int valid=e1k_rx_valid(status,d->errors,d->length);
#endif
        if(valid) { cb((uint8_t *)pch.rxbuf->cpu+pch.rxhead*E1K_BUFFER,d->length); n++; pch.rx_good++; }
        else pch.rx_bad++;
        d->status=0; tail=pch.rxhead; consumed++;
        pch.rxhead=(pch.rxhead+1)&(E1K_RX_COUNT-1);
    }
    if(consumed) { dma_wmb(); pw(P_RX+0x18,tail); }
    return n;
}
static void pch_irq_enable(net_rx_cb cb)
{
    NET_GUARD;
    if(!pch.online || !cb) return;
    pch.callback=cb; (void)pr(P_ICR); pw(P_IMS,P_IRQS); pflush();
}
static void pch_irq(void)
{
    NET_GUARD;
    if(!pch.mmio || pch.reset_unconfirmed) return;
#ifdef PCH2_NEGCTL_OFFLINE_NO_ACK
    if(!pch.online) return;
#endif
    uint32_t cause=pr(P_ICR);
    if(!cause || cause==UINT32_MAX) return; /* another member owns shared INTx */
    /* A silenced but still-bound member may have an already-latched cause,
     * or IMC may have failed. Drain it until common INTx retirement completes;
     * suppress deferred receive, not acknowledgement, while offline. */
    if(pch.online && (cause&P_IRQS) && pch.callback) net_rx_schedule();
}
static struct netdev pch_net={.name="e1000-pch2",.irq_line=-1,.tx=pch_tx,.rx_poll=pch_poll,
                            .irq_enable=pch_irq_enable,.irq=pch_irq};

int e1000_pch2_probe(struct device *dev)
{
    if(!dev || dev->bus_type!=DEV_BUS_PCI || dev->vendor!=0x8086 ||
       (dev->device!=0x1502 && dev->device!=0x1503) || dev->class_code!=2 ||
       dev->subclass!=0 || pch.pci || pch.poisoned || !time_ready()) return -1;
    pch.pci=dev;
    /* Disable host bus mastering BEFORE the D3->D0 transition. Firmware may
     * have left descriptors pointing into its own reclaimed boot memory. */
    if(pci_isolate()) goto untouched;
    uint8_t pm=pci_cap_find(dev->bus,dev->slot,dev->func,1);
    if(!pm) { kprintf("[e1000-pch2] decline: missing PCI PM capability\n"); goto untouched; }
    uint16_t pmcsr=pcfg(pm+4);
    if(pmcsr==UINT16_MAX) goto untouched;
    if(pmcsr&3) {
        pcfgwrite(pm+4,pmcsr&0x7ffcu); /* PME status is W1C, preserve it */
        if(pdelay(10000) || (pcfg(pm+4)&3)) goto untouched;
    }
    if(!(dev->res[0].flags&DEV_RES_MEM) || dev->res[0].size<0x20000) goto untouched;
    pcfgwrite(PCI_CFG_COMMAND,pcfg(PCI_CFG_COMMAND)|PCI_CMD_MEM);
    if(!(pcfg(PCI_CFG_COMMAND)&PCI_CMD_MEM)) goto untouched;
    uint64_t base=dev_bar_map(dev,0);
    if(!base) goto untouched;
    pch.mmio=(volatile uint8_t *)(uintptr_t)base;
    if(firmware_policy()) goto untouched;
    if((pr(P_FEXT)&(1u<<27)) || (pr(P_EXSIZE)&0x00ff0000u) || !(pr(P_EXCNF)&8)) {
        kprintf("[e1000-pch2] decline: board requires unsupported software NVM/LCD configuration\n"); goto untouched;
    }
    if(pr(P_EXT)&P_DRV_LOAD) {
        kprintf("[e1000-pch2] decline: another driver owns DRV_LOAD\n"); goto untouched;
    }
    if(sw_acquire()) {
        kprintf("[e1000-pch2] decline: SWFLAG unavailable\n"); goto untouched;
    }
    if(master_stop() || pdelay(10000)) goto failed;
    /* Allocate the packet buffer BEFORE reset: changing PBA afterward does
     * not reset its internal pointers. 18 KiB RX is sufficient for our MTU;
     * use hardware's reported TX remainder instead of assuming a total FIFO
     * size from another MAC. Datasheet requires TX allocation above 4 KiB. */
    if(pwrite(P_PBA,18,31) || ((pr(P_PBA)>>16)&31)<5 ||
       pwrite(P_EXCNF,pr(P_EXCNF)|P_GATE,P_GATE)) goto failed;
    pch.reset_unconfirmed=1;
    pw(P_CTRL,pr(P_CTRL)|P_RESET|P_PHY_RESET);
    pch.sw_owned=0; /* reset consumes SWFLAG; never clear someone else's new flag */
    /* A read/flush immediately after this write can hang PCH. Intel requires
     * the full 20 ms quiet window BEFORE examining reset completion. */
    if(pdelay(20000) || pwait(P_CTRL,P_RESET,0,100000)) goto failed;
    pch.reset_unconfirmed=0;
    if(pwrite(P_FEXT3,(pr(P_FEXT3)&~0x0c000000u)|0x08000000u,0x0c000000u) ||
       pwait(P_STATUS,P_LAN_DONE,P_LAN_DONE,150000) || firmware_policy()) goto failed;
    pw(P_STATUS,pr(P_STATUS)&~(P_LAN_DONE|(1u<<10)));
    if(pdelay(10000) || pwrite(P_EXCNF,pr(P_EXCNF)&~P_GATE,P_GATE) || pdelay(10000)) goto failed;
    pw(P_IMC,UINT32_MAX); (void)pr(P_ICR);
    if(pwrite(P_EXT,(pr(P_EXT)&~((1u<<27)|(1u<<24)))|P_DRV_LOAD|(1u<<22)|(1u<<17),P_DRV_LOAD)) goto failed;
    pch.claimed=1;
    if(phy_setup()) { kprintf("[e1000-pch2] decline: PHY/MDIO configuration failed\n"); goto failed; }
    uint32_t lo=pr(P_RAL),hi=pr(P_RAH);
    if(!(hi&(1u<<31)) || (lo&1) || !(lo|(hi&0xffffu))) {
        kprintf("[e1000-pch2] decline: no valid autoloaded unicast MAC\n"); goto failed;
    }
    for(unsigned i=0;i<4;i++) pch_net.mac[i]=lo>>(8*i);
    pch_net.mac[4]=hi; pch_net.mac[5]=hi>>8;
    /* No invented address and no flash fallback. RAR0 survives autoload and
     * SHRA0..3 are the only additional PCH2 addresses, protected by SWFLAG. */
    if(sw_acquire()) goto failed;
    for(unsigned i=0;i<4;i++) {
        if(pwrite(0x5438+8*i,0,UINT32_MAX)||pwrite(0x543c+8*i,0,UINT32_MAX)) goto failed;
    }
    if(sw_release()) goto failed;
    for(unsigned i=0;i<32;i++) pw(0x5200+4*i,0);
    uint8_t msi=pci_cap_find(dev->bus,dev->slot,dev->func,PCI_CAP_MSI);
    uint8_t msix=pci_cap_find(dev->bus,dev->slot,dev->func,PCI_CAP_MSIX);
    if(msi) { pcfgwrite(msi+2,pcfg(msi+2)&~1u); if(pcfg(msi+2)&1) goto failed; }
    if(msix) { pcfgwrite(msix+2,pcfg(msix+2)&~(1u<<15)); if(pcfg(msix+2)&(1u<<15)) goto failed; }
    pw(P_IAM,0); pw(P_ITR,0); pw(P_WUC,0); pw(P_RXCSUM,0); pw(P_RFCTL,0xc0);
    pw(P_GCR,pr(P_GCR)&~0x3fu); /* snooped DMA; no relaxed-ordering requests */
    pw(0x3004,pr(0x3004)|0x00050000u); /* Intel PCH band-gap bias setting */
    pw(P_TARC,pr(P_TARC)|(1u<<23)|(1u<<24)|(1u<<26)|(1u<<27));
    pw(P_TIPG,8u|(8u<<10)|(7u<<20));
    pw(P_RXDCTL,1u<<16); pw(P_TXDCTL,(1u<<22)|(1u<<16)|31u);
    pw(P_RX+0x20,0); pw(P_RX+0x2c,0); pw(P_TX+0x20,0); pw(P_TX+0x2c,0);
    dma_device_init(&pch.dma,"e1000-pch2",DMA_MASK_32);
    pch.rings=dma_alloc_coherent(&pch.dma,4096,4096,0);
    pch.rxbuf=dma_alloc_coherent(&pch.dma,E1K_RX_COUNT*E1K_BUFFER,4096,0);
    pch.txbuf=dma_alloc_coherent(&pch.dma,E1K_TX_COUNT*E1K_BUFFER,4096,0);
    if(!pch.rings || !pch.rxbuf || !pch.txbuf) goto failed;
    memset(pch.rings->cpu,0,4096);
    pch.rx=pch.rings->cpu; pch.tx=(void *)((uint8_t *)pch.rings->cpu+2048);
    for(unsigned i=0;i<E1K_RX_COUNT;i++) pch.rx[i].addr=dma_addr_value(pch.rxbuf->dma)+i*E1K_BUFFER;
    for(unsigned i=0;i<E1K_TX_COUNT;i++) { pch.tx[i].addr=dma_addr_value(pch.txbuf->dma)+i*E1K_BUFFER; pch.tx[i].status=1; }
    for(struct dma_buffer *b=pch.dma.buffers;b;b=b->next) if(!dma_buffer_submit(b)) goto failed;
    uint64_t rx=dma_addr_value(pch.rings->dma),tx=rx+2048;
    /* Program and read back bases before bus mastering. A truncated DMA
     * address must fail registration, never masquerade as a linked NIC. */
    if(pwrite(P_RX,(uint32_t)rx,UINT32_MAX)||pwrite(P_RX+4,rx>>32,UINT32_MAX)||
       pwrite(P_TX,(uint32_t)tx,UINT32_MAX)||pwrite(P_TX+4,tx>>32,UINT32_MAX)||
       pwrite(P_RX+8,E1K_RX_COUNT*16,0xfff80)||pwrite(P_TX+8,E1K_TX_COUNT*16,0xfff80)) goto failed;
    pw(P_RX+0x10,0); pw(P_RX+0x18,E1K_RX_COUNT-1); pw(P_TX+0x10,0); pw(P_TX+0x18,0);
    pch.rxhead=pch.txhead=pch.txtail=pch.pending=0;
    pch.rx_good=pch.rx_bad=pch.tx_good=pch.tx_bad=0; pch.link_seen=0;
    dma_wmb();
    /* netdev_irq_route owns INTx admission; retain INTX_DIS and IMC=all until
     * the common route has a live callback. A missing route remains polled. */
    pcfgwrite(PCI_CFG_COMMAND,pcfg(PCI_CFG_COMMAND)|PCI_CMD_MASTER);
    uint16_t final_command=pcfg(PCI_CFG_COMMAND);
    if(final_command==UINT16_MAX ||
       (final_command&(PCI_CMD_MASTER|PCI_CMD_INTX_DIS))!=(PCI_CMD_MASTER|PCI_CMD_INTX_DIS)) goto failed;
    uint32_t ctrl=pr(P_CTRL);
    ctrl&=~(P_MASTER_OFF|P_PHY_RESET|(1u<<11)|(1u<<12)|(1u<<27)|(1u<<28)|(1u<<30));
    if(pwrite(P_CTRL,ctrl|(1u<<6),P_MASTER_OFF|(1u<<6)) ||
       pwrite(P_TCTL,2u|8u|(15u<<4)|(63u<<12),2) ||
       pwrite(P_RCTL,2u|(1u<<4)|(1u<<15)|(1u<<26),2)) goto failed;
    pch_net.irq_line=dev->irq_line; pch.online=1;
    dev_set_drvdata(dev,&pch_net); link_report();
    kprintf("[e1000-pch2] initialized %x: DMA32 rx=%u tx=%u non-managed autoload path; physical validation pending\n",
            dev->device,E1K_RX_COUNT,E1K_TX_COUNT);
    return 0;
failed:
    dispose();
    dev_set_drvdata(dev,NULL);
    return -1;
untouched:
    (void)pci_isolate(); /* never reset a PHY for which we did not gain ownership */
    pch.pci=NULL; pch.mmio=NULL;
    return -1;
}
void e1000_pch2_remove(struct device *dev)
{
    /* Device model already drained the INTx member. NET_GUARD also excludes
     * outstanding polling/transmit callers before buffers can be reclaimed. */
    NET_GUARD;
    if(pch.pci!=dev) return;
    if(dev->irq_mode!=DEV_IRQ_NONE) {
        /* Common release can retain a route if masking failed. A still-live
         * callback must never outlive freed descriptors, even if a caller
         * violates the normal dev_unbind ordering. */
        pch.online=0; pch.callback=NULL; pch.poisoned=1;
        pw(P_IMC,UINT32_MAX); (void)pci_isolate();
        dma_device_quarantine(&pch.dma);
        return;
    }
    dispose(); dev_set_drvdata(dev,NULL);
}
