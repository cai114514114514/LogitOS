#include <stdint.h>
#include <stddef.h>
#include "netdev.h"
#include "netring.h"
#include "pci.h"
#include "dma.h"
#include "vmm.h"
#include "net.h"
#include "ktime.h"
#include "kprintf.h"
#include "ktime.h"
#include "rtl8168.h"

/* Realtek RTL8169 / RTL8168 / RTL8111 gigabit -- the onboard NIC on a very
 * large share of consumer PCs built in the last twenty years.
 *
 * *** THIS DRIVER HAS NEVER BEEN RUN AGAINST A DEVICE. ***
 *
 * QEMU has no rtl8169 model at all (`qemu-system-x86_64 -device help` lists
 * exactly one Realtek part, rtl8139), and there is no real hardware in this
 * environment, so there is no way to boot it and watch a packet arrive. What IS
 * tested is the part that can be: every descriptor field accessor and index
 * computation below comes from netring.h and is checked against hand-computed
 * values in tests/unit/net_drv_test.c. The register programming is from the
 * datasheet and mirrors what Linux's r8169 does for the original 8169; treat it
 * as unverified until someone boots it on metal.
 * Correction 2026-10-31, retained beside the old claim above: a host register
 * model now executes probe, packet I/O, link events and removal. That checks
 * software ordering, not silicon behavior. Only MAC 40/42/44 are accepted;
 * the old generic startup is replaced with the G/GU/8411B programs in
 * rtl8168_hw.c and rtl8168_phy.c. not verified on X79-VG4 hardware.
 *
 * Unlike the 8139 this is a descriptor-ring design much closer to the e1000:
 * 16-byte descriptors, ownership in the top bit of opts1, and an explicit
 * end-of-ring marker instead of a modulo.
 */

#ifndef memset
void *memset(void *, int, size_t);
#endif
#ifndef memcpy
void *memcpy(void *, const void *, size_t);
#endif

#define R_IDR0     0x00
#define R_MAR0     0x08
#define R_TNPDS    0x20      /* transmit normal-priority descriptor start, 64-bit */
#define R_CR       0x37
#define R_TPPOLL   0x38
#define R_IMR      0x3C
#define R_ISR      0x3E
#define R_TCR      0x40
#define R_RCR      0x44
#define R_CFG9346  0x50
#define R_PHYSTATUS 0x6C
#define R_RMS      0xDA      /* max receive packet size */
#define R_CPCMD    0xE0      /* "C+" command register */
#define R_RDSAR    0xE4      /* receive descriptor start, 64-bit */
#define R_MTPS     0xEC      /* max transmit packet size, 128-byte units */

#define CR_TE      0x04
#define CR_RE      0x08
#define CR_RST     0x10

#define TPPOLL_NPQ 0x40      /* "there is work on the normal-priority TX ring" */

#define ISR_ROK    0x0001
#define ISR_RER    0x0002
#define ISR_TOK    0x0004
#define ISR_RDU    0x0010    /* receive descriptor unavailable */
#define ISR_FOVW   0x0040
#define ISR_LINK   0x0020

#define CFG9346_UNLOCK 0xC0
#define CFG9346_LOCK   0x00

/* PHYstatus (0x6C) bits, from the r8169 Linux driver's rtl_register_content
 * enum (drivers/net/ethernet/realtek/r8169_main.c). Sourced rather than
 * remembered, and worth flagging exactly BECAUSE it cannot be checked the way
 * MSR_LINKB above was: there is no QEMU rtl8169 model to boot this against
 * (see the file header), so this decode is unverified by execution the same
 * way the rest of this driver is -- it is a passive read+print at probe, not
 * a polled link-state machine. Correction 2026-10-31: LinkChg plus polling now
 * use these bits to publish NETIF_F_RUNNING through netdev_set_link(). The
 * decode is covered by the register model, still unverified on hardware. */
#define PHYSTATUS_LINK  0x02
#define PHYSTATUS_FDX   0x01
#define PHYSTATUS_1000M 0x10
#define PHYSTATUS_100M  0x08
#define PHYSTATUS_10M   0x04

#define RX_DESC   32
#define TX_DESC   16
#define BUF_SIZE  2048
#define ETH_MIN   60
#define FRAME_MAX 1518

struct rtl_desc {
    uint32_t opts1;      /* OWN | EOR | FS | LS | errors | length */
    uint32_t opts2;      /* VLAN / checksum offload -- unused here */
    uint64_t addr;       /* buffer physical address */
} __attribute__((packed));

/* Coherent memory is CPU-mapped independently of the device address. All
 * buffers are prepared before DMA is enabled; failed preparation can therefore
 * release them without assuming a controller has stopped. Successful rings
 * remain device-owned for the driver's lifetime. */
static struct dma_device nic_dma;
static void dma_discard_unpublished(void)
{
    while (nic_dma.buffers) {
        if (dma_free_coherent(nic_dma.buffers) != 0) break;
    }
}
static void dma_publish_buffers(void)
{
    for (struct dma_buffer *b = nic_dma.buffers; b; b = b->next)
        dma_buffer_submit(b);
    dma_wmb();
}
static struct dma_buffer *rx_dma, *tx_dma, *tx_payload[TX_DESC];
static volatile uint8_t  *mmio;
static volatile struct rtl_desc *rxd, *txd;
static uint8_t *rxbuf[RX_DESC];
static uint8_t *txbuf[TX_DESC];
static uint32_t rx_cur, tx_cur;
static int ready;
static net_rx_cb g_rxcb;
static struct device *nic_owner;
static struct netdev rtl_dev;
static int link_known, link_up, link_published;
static uint64_t link_changes;

static inline uint8_t  m8 (uint32_t o)             { return *(volatile uint8_t  *)(mmio + o); }
static inline void     w8 (uint32_t o, uint8_t v)  { *(volatile uint8_t  *)(mmio + o) = v; }
static inline void     w16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(mmio + o) = v; }
static inline uint16_t r16(uint32_t o)             { return *(volatile uint16_t *)(mmio + o); }
static inline uint32_t r32(uint32_t o)             { return *(volatile uint32_t *)(mmio + o); }
static inline void     w32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(mmio + o) = v; }
/* Linux v6.12 r8169_main.c, rtl_set_rx_tx_desc_registers writes high first. The old
 * low-first helper was inherited from 8169; keep neither a truncated address
 * nor that order when buffers really live above 4 GiB. */
static inline void     w64(uint32_t o, uint64_t v) { w32(o + 4, (uint32_t)(v >> 32)); w32(o, (uint32_t)v); }
static inline void barrier(void) { __asm__ volatile ("mfence" ::: "memory"); }

static void rtl_delay(unsigned usec)
{
    uint64_t start = time_mono_ns();
    /* Clock-based spacing, bounded even if an early boot clock is frozen.
     * This is not a calibrated spin count or a host timing measurement. */
    for (unsigned i = 0; i < 1000000; i++) {
        if (time_mono_ns() - start >= (uint64_t)usec * 1000) break;
        __asm__ volatile ("pause");
    }
}
static const struct rtl_io hw_io = {m8, r32, w8, w16, w32, rtl_delay};

static int rtl_reset(void)
{
    /* Linux v6.12 r8169_main.c, rtl_hw_reset: 100 polls spaced by 100 us.
     * The old million unspaced reads had a machine-dependent deadline. */
    w8(R_CR, CR_RST);
    for (unsigned i = 0; i < 100; i++) {
        rtl_delay(100);
        if (!(m8(R_CR) & CR_RST)) return 0;
    }
    return -1;
}

static void rtl_link_update(void)
{
    int up = !!(m8(R_PHYSTATUS) & PHYSTATUS_LINK);
    if (!link_known || up != link_up) {
        if (link_known) link_changes++;
        link_known = 1;
        link_up = up;
        link_published = 0;
        kprintf("[rtl8169] link %s changes=%llu (not verified on X79-VG4 hardware)\n",
                up ? "up" : "down", (unsigned long long)link_changes);
    }
    /* probe precedes netif_register. Retry publication on poll even when the
     * physical state did not change, otherwise an initially unplugged card
     * keeps the network layer's optimistic RUNNING flag forever. */
    if (!link_published && netdev_set_link(&rtl_dev, up) == 0) link_published = 1;
}

static int rtl_rx_poll(net_rx_cb cb)
{
    NET_GUARD;
    if (!ready) return 0;
    rtl_link_update();
    uint64_t f = net_lock();
    /* No ack-in-the-drain here either, and deliberately. e1000 and virtio-net
     * have one (see e1000_rx_drain for what it fixes and what it is worth);
     * rtl8139 does not, because adding it there took a card that completed 9 of
     * 9 large transfers to 0 of 9. QEMU has no rtl8169 device model, so this
     * driver cannot be booted here at all -- which means adding it would be
     * reasoned symmetry with the one sibling that was measured to be harmed by
     * it, on a card nobody can run. That is not a trade worth making blind.
     * Expect the same tick-latency tail rtl8139 has, on real hardware, until
     * someone can put an actual 8169 in front of it. */
    int n = 0, budget = RX_DESC;      /* bounded: also runs in the ISR */
    while (budget-- > 0) {
        uint32_t o1 = rxd[rx_cur].opts1;
        if (rtl8169_own(o1)) break;   /* the chip still owns this one: nothing new */
        dma_rmb();                   /* OWN relinquishes DMA payload to the CPU */
        uint16_t len = rtl8169_len(o1);
        /* The length INCLUDES the 4-byte CRC, as on the 8139. */
        if (rtl8169_rx_ok(o1) && len > 4 + 14 && len <= BUF_SIZE)
            cb(rxbuf[rx_cur], (uint16_t)(len - 4));
        barrier();
        rxd[rx_cur].opts1 = rtl8169_rx_opts1(BUF_SIZE, rx_cur == RX_DESC - 1);
        rx_cur = ring_next(rx_cur, RX_DESC);
        n++;
    }
    net_unlock(f);
    return n;
}

/* Contract: callers hold net_lock (IF=0). */
static int rtl_tx(const void *frame, uint16_t len)
{
    if (!ready || len == 0 || len > FRAME_MAX) return -1;
    uint32_t i = tx_cur;
    int spins = 0;
    while (rtl8169_own(txd[i].opts1)) {           /* previous send still queued */
        if (++spins > 1000000) return -1;
    }
    uint16_t n = len;
    if (n < ETH_MIN) { memset(txbuf[i], 0, ETH_MIN); n = ETH_MIN; }
    memcpy(txbuf[i], frame, len);
    txd[i].addr  = dma_addr_value(tx_payload[i]->dma);
    txd[i].opts2 = 0;
    barrier();                                    /* buffer + address visible before OWN */
    txd[i].opts1 = rtl8169_tx_opts1(n, i == TX_DESC - 1);
    barrier();
    tx_cur = ring_next(i, TX_DESC);
    w8(R_TPPOLL, TPPOLL_NPQ);                     /* kick the normal-priority queue */
    return 0;
}


static void rtl_irq_on(net_rx_cb cb)
{
    if (!ready) return;
    g_rxcb = cb;
    w16(R_IMR, 0);
    /* The interrupt route itself is netdev_irq_route()'s: rtl_dev sets
     * msi_ok, so the device model tries MSI first and falls back to INTx,
     * and only a routed device gets here. */
    w16(R_ISR, 0xFFFF);                           /* write-1-to-clear */
    w16(R_IMR, ISR_ROK | ISR_RER | ISR_RDU | ISR_FOVW | ISR_LINK);
}

/* Interrupts taken, and a storm guard. The X79-VG4's first boot (2026-10-31)
 * stopped right after this driver bound its 8168 and enabled the interrupt:
 * a level-triggered INTx this code cannot acknowledge on that chip re-enters
 * forever and the boot thread never runs again. More than 50,000 in one
 * second masks the device's interrupts and leaves it polled. */
uint64_t rtl8169_irq_count;
static uint64_t storm_window_ns, storm_count;
static int storm_masked;

static void rtl_isr(void)
{
    NET_GUARD;
    if (!ready) return;
    rtl8169_irq_count++;
    uint64_t now = time_mono_ns();
    if (now - storm_window_ns > 1000000000ull) { storm_window_ns = now; storm_count = 0; }
    if (++storm_count > 50000 && !storm_masked) {
        w16(R_IMR, 0);
        storm_masked = 1;
        kprintf("[rtl8169] interrupt storm (%lu in 1 s, ISR=%x): device interrupts masked, polling\n",
                (unsigned long)storm_count, r16(R_ISR));
        return;
    }
    uint16_t isr = r16(R_ISR);
    if (!isr || isr == UINT16_MAX) return;       /* another device owns the shared IRQ */
    w16(R_ISR, isr);                              /* ack before draining */
    if (isr & ISR_LINK) rtl_link_update();
    /* Ack here, drain on SOFTIRQ_NET -- see c/net/core/net.c. */
    if (g_rxcb) net_rx_schedule();
}

static struct netdev rtl_dev = {
    .name = "rtl8169", .irq_line = -1,
    .tx = rtl_tx, .rx_poll = rtl_rx_poll,
    .irq_enable = rtl_irq_on, .irq = rtl_isr,
    .msi_ok = 1,                     /* MSI path covered by tests/hw/rtl */
};

/* First memory BAR. pci_dev->bar0 is no use here: on this family BAR0 is the
 * I/O window and the registers we want are behind BAR1 or BAR2. The device
 * model already sized and classified every BAR at enumeration, including the
 * 64-bit pairing, so this is a lookup rather than a config-space walk. */
static uint64_t map_mmio_bar(struct device *dev)
{
    for (int b = 0; b < DEV_NRES; b++) {
        if (!(dev->res[b].flags & DEV_RES_MEM)) continue;
        uint64_t base = dev_bar_map(dev, b);
        if (base) return base;
    }
    return 0;
}

/* The old unconditional 64-bit mask also covered conventional PCI 8169 and
 * unknown revisions. PCIe 8168C and later known MACs support 64-bit descriptors
 * without CPlusCmd.PCIDAC; that bit is for older PCI DAC, which we do not enable.
 * Hardware XID facts and the MAC >= 18 boundary are checked against Linux's
 * rtl_chip_infos / rtl_init_one in drivers/net/ethernet/realtek/r8169_main.c:
 * https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/realtek/r8169_main.c
 * This is capability selection, not a claim of device validation or complete
 * variant-specific PHY setup. New/unknown IDs deliberately stay below 4 GiB. */
static uint64_t rtl_dma_mask(const struct device *dev, uint32_t txconfig)
{
    if (dev->vendor != 0x10ec || !dev->cap_pcie ||
        (dev->device != 0x8168 && dev->device != 0x8161)) return DMA_MASK_32;
    /* Correction: capability selection now derives from the same identification
     * whitelist as binding, rather than a second, independently drifting jar. */
    return rtl_identify(txconfig) != RTL_MAC_UNKNOWN ? DMA_MASK_64 : DMA_MASK_32;
}

int rtl8169_probe(struct device *dev)
{
    if (ready) return -1;                                             /* one NIC bound at a time */
    /* A clean earlier ->remove leaves the DMA device quiesced (blocked). A
     * reboot whose every reset returned rebinds through here (power_abort),
     * so reopen it -- dma_device_resume refuses if anything is still
     * quarantined after an unconfirmed stop. */
    if (nic_dma.blocked && dma_device_resume(&nic_dma) != 0) return -1;
    if (dev_enable_checked(dev, 0) != 0) {
        kprintf("[rtl8169] PCI Command decode rejected\n"); return -1;
    }
    uint64_t base = map_mmio_bar(dev);
    if (!base) { kprintf("[rtl8169] no memory BAR\n"); return -1; }
    mmio = (volatile uint8_t *)(uintptr_t)base;

    uint32_t txconfig = r32(R_TCR);
    enum rtl_mac_ver version = rtl_identify(txconfig);
    if (version == RTL_MAC_UNKNOWN) {
        kprintf("[rtl8169] unknown XID %03x; refusing bind\n", (txconfig >> 20) & 0xfcf);
        goto alloc_fail;
    }
    w16(R_IMR, 0);
    w16(R_ISR, 0xffff);

    dma_device_init(&nic_dma, "rtl8169", rtl_dma_mask(dev, r32(R_TCR)));
    rx_dma = dma_alloc_coherent(&nic_dma, 4096, 4096, 0);
    tx_dma = dma_alloc_coherent(&nic_dma, 4096, 4096, 0);
    if (!rx_dma || !tx_dma) goto alloc_fail;
    uint64_t rr = dma_addr_value(rx_dma->dma), tr = dma_addr_value(tx_dma->dma);
    rxd = rx_dma->cpu;
    txd = tx_dma->cpu;
    for (int i = 0; i < RX_DESC; i++) {
        struct dma_buffer *b = dma_alloc_coherent(&nic_dma, 4096, 4096, 0);
        if (!b) goto alloc_fail;
        rxbuf[i] = b->cpu;
        rxd[i].addr = dma_addr_value(b->dma);
        rxd[i].opts1 = rtl8169_rx_opts1(BUF_SIZE, i == RX_DESC - 1);
    }
    for (int i = 0; i < TX_DESC; i++) {
        struct dma_buffer *b = dma_alloc_coherent(&nic_dma, 4096, 4096, 0);
        if (!b) goto alloc_fail;
        tx_payload[i] = b;
        txbuf[i] = b->cpu;
        txd[i].addr = dma_addr_value(b->dma);
        txd[i].opts1 = (i == TX_DESC - 1) ? RTL8169_EOR : 0;
    }

    /* Linux v6.12 rtl_hw_init_8168g / rtl_init_one exits firmware/OOB BEFORE
     * reset. Keeping this in the post-reset startup looks equivalent on a
     * cold model but can leave a Windows warm boot stuck in reset. */
    if (rtl8168_prepare(&hw_io) != 0) {
        kprintf("[rtl8169] OOB FIFO timeout\n"); goto alloc_fail;
    }
    if (rtl_reset() != 0) { kprintf("[rtl8169] reset timeout\n"); goto alloc_fail; }

    for (int i = 0; i < 6; i++) rtl_dev.mac[i] = m8(R_IDR0 + (uint32_t)i);
    rtl_dev.irq_line = dev->irq_line;

    w8(R_CFG9346, CFG9346_UNLOCK);
    kprintf("rtl: no firmware patch (rtl8168g-2.fw not loaded)\n");
    if (rtl8168_start(version, &hw_io, dev->func) != 0) {
        kprintf("[rtl8169] MAC/PHY initialization timeout\n");
        goto alloc_fail;
    }
    if (dev_enable_checked(dev, 1) != 0) {
        kprintf("[rtl8169] PCI bus-master enable rejected\n"); goto alloc_fail;
    }
    dma_publish_buffers();
    w16(R_CPCMD, 0);                               /* no VLAN / checksum offload */
    w16(R_RMS, BUF_SIZE);
    w32(R_TCR, 0x03000780u); /* IFG, DMA burst, G dynamic FIFO */
    /* Accept physical-match + multicast + broadcast, max DMA burst, no receive
     * FIFO threshold. Not promiscuous, not runts, not errored frames. */
    w32(R_RCR, 0x0Eu | (7u << 8) | (1u << 11) | (3u << 14));
    w32(R_MAR0, 0xFFFFFFFFu);
    w32(R_MAR0 + 4, 0xFFFFFFFFu);
    /* Correction: for G, descriptor addresses must precede TE/RE. The old
     * AFTER claim applied an unverified 8169 sequence to every revision. */
    w64(R_TNPDS, tr);
    w64(R_RDSAR, rr);
    w16(R_ISR, 0xFFFF);
    w16(R_IMR, 0);                                 /* polled until irq_enable */
    w8(R_CFG9346, CFG9346_LOCK);
    (void)r32(R_RCR); /* flush posted PCI writes before the engines start */
    w8(R_CR, CR_TE | CR_RE);

    rx_cur = tx_cur = 0;
    ready = 1;
    nic_owner = dev;
    link_known = link_published = 0;
    link_changes = 0;
    g_rxcb = NULL;
    rtl_link_update();
    {
        uint8_t phy = m8(R_PHYSTATUS);
        const char *mbps = (phy & PHYSTATUS_1000M) ? "1000" :
                            (phy & PHYSTATUS_100M)  ? "100"  :
                            (phy & PHYSTATUS_10M)   ? "10"   : "?";
        kprintf("[rtl8169] up (UNVERIFIED on device): mmio=%p phy=%x "
                "(decode, unverified: link=%s %sMb/s %s duplex)\n",
                (void *)(uintptr_t)base, phy,
                (phy & PHYSTATUS_LINK) ? "up" : "down", mbps,
                (phy & PHYSTATUS_FDX) ? "full" : "half");
    }
    dev_set_drvdata(dev, &rtl_dev);
    return 0;
alloc_fail:
    (void)dev_disable_checked(dev);
    dma_discard_unpublished();
    rxd = txd = NULL;
    mmio = NULL;
    return -1;
}

/* The existing device-model removal hook stops DMA before releasing backing
 * pages. A controller that cannot acknowledge reset keeps every allocation.
 * This is NOT yet system-shutdown evidence: power.c currently never calls a
 * NIC stop/device-remove hook. The shared power entry must reach this path
 * before firmware; tests/hw/rtl's poweroff/reboot fixtures expose that gap. */
void rtl8169_remove(struct device *dev)
{
    NET_GUARD;
    if (!mmio) return;
    ready = 0;
    w16(R_IMR, 0);
    int stopped = rtl_reset() == 0;
    /* A reset acknowledgement alone does not revoke PCI bus mastering. Even
     * a stuck reset must have MASTER cleared; retain DMA memory if either
     * readback is unconfirmed. Firmware must never inherit live DMA rings. */
    int disabled = dev_disable_checked(dev);
    if (!stopped || disabled != 0) {
        dma_device_quarantine(&nic_dma);
        kprintf("[rtl8169] stop unconfirmed: DMA quarantined\n");
        return;
    }
    dma_device_quiesced(&nic_dma);
    dma_discard_unpublished(); /* now quiesced, including previously owned pages */
    mmio = NULL;
    rxd = txd = NULL;
    nic_owner = NULL;
    g_rxcb = NULL;
    dev_set_drvdata(dev, NULL);
}
