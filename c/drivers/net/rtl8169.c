#include <stdint.h>
#include <stddef.h>
#include "netdev.h"
#include "netring.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "net.h"
#include "kprintf.h"

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
 *
 * Unlike the 8139 this is a descriptor-ring design much closer to the e1000:
 * 16-byte descriptors, ownership in the top bit of opts1, and an explicit
 * end-of-ring marker instead of a modulo.
 */

void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);

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

#define CFG9346_UNLOCK 0xC0
#define CFG9346_LOCK   0x00

/* PHYstatus (0x6C) bits, from the r8169 Linux driver's rtl_register_content
 * enum (drivers/net/ethernet/realtek/r8169_main.c). Sourced rather than
 * remembered, and worth flagging exactly BECAUSE it cannot be checked the way
 * MSR_LINKB above was: there is no QEMU rtl8169 model to boot this against
 * (see the file header), so this decode is unverified by execution the same
 * way the rest of this driver is -- it is a passive read+print at probe, not
 * a polled link-state machine, precisely so that a wrong bit here costs one
 * misleading log line and nothing else. */
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

static volatile uint8_t  *mmio;
static volatile struct rtl_desc *rxd, *txd;
static uint8_t *rxbuf[RX_DESC];
static uint8_t *txbuf[TX_DESC];
static uint32_t rx_cur, tx_cur;
static int ready;
static net_rx_cb g_rxcb;

static inline uint8_t  m8 (uint32_t o)             { return *(volatile uint8_t  *)(mmio + o); }
static inline void     w8 (uint32_t o, uint8_t v)  { *(volatile uint8_t  *)(mmio + o) = v; }
static inline void     w16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(mmio + o) = v; }
static inline uint16_t r16(uint32_t o)             { return *(volatile uint16_t *)(mmio + o); }
static inline void     w32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(mmio + o) = v; }
static inline void     w64(uint32_t o, uint64_t v) { w32(o, (uint32_t)v); w32(o + 4, (uint32_t)(v >> 32)); }
static inline void barrier(void) { __asm__ volatile ("mfence" ::: "memory"); }

static int rtl_rx_poll(net_rx_cb cb)
{
    if (!ready) return 0;
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
    txd[i].addr  = (uint64_t)(uintptr_t)txbuf[i];
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
    w16(R_ISR, 0xFFFF);                           /* write-1-to-clear */
    w16(R_IMR, ISR_ROK | ISR_RER | ISR_RDU | ISR_FOVW);
}

static void rtl_isr(void)
{
    if (!ready) return;
    uint16_t isr = r16(R_ISR);
    w16(R_ISR, isr);                              /* ack before draining */
    /* Ack here, drain on SOFTIRQ_NET -- see c/net/core/net.c. */
    if (g_rxcb) net_rx_schedule();
}

static struct netdev rtl_dev = {
    .name = "rtl8169", .irq_line = -1,
    .tx = rtl_tx, .rx_poll = rtl_rx_poll,
    .irq_enable = rtl_irq_on, .irq = rtl_isr,
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

int rtl8169_probe(struct device *dev)
{
    if (ready) return -1;                          /* one NIC bound at a time */
    dev_enable(dev, 1);                            /* memory decode + bus master (DMA) */
    uint64_t base = map_mmio_bar(dev);
    if (!base) { kprintf("[rtl8169] no memory BAR\n"); return -1; }
    mmio = (volatile uint8_t *)(uintptr_t)base;

    uint64_t rr = pmm_alloc(), tr = pmm_alloc();
    if (!rr || !tr) { kprintf("[rtl8169] descriptor ring alloc failed\n"); return -1; }
    /* The descriptor rings must be 256-byte aligned; a 4 KiB frame is. */
    rxd = (volatile struct rtl_desc *)(uintptr_t)rr;
    txd = (volatile struct rtl_desc *)(uintptr_t)tr;
    memset((void *)rxd, 0, RX_DESC * sizeof(struct rtl_desc));
    memset((void *)txd, 0, TX_DESC * sizeof(struct rtl_desc));

    for (int i = 0; i < RX_DESC; i++) {
        uint64_t b = pmm_alloc();
        if (!b) { kprintf("[rtl8169] rx buffer alloc failed\n"); return -1; }
        rxbuf[i] = (uint8_t *)(uintptr_t)b;
        rxd[i].addr  = b;                          /* identity-mapped: phys == virt */
        rxd[i].opts2 = 0;
        rxd[i].opts1 = rtl8169_rx_opts1(BUF_SIZE, i == RX_DESC - 1);
    }
    for (int i = 0; i < TX_DESC; i++) {
        uint64_t b = pmm_alloc();
        if (!b) { kprintf("[rtl8169] tx buffer alloc failed\n"); return -1; }
        txbuf[i] = (uint8_t *)(uintptr_t)b;
        txd[i].addr  = b;
        txd[i].opts1 = (i == TX_DESC - 1) ? RTL8169_EOR : 0;   /* OWN clear = ours */
    }

    w8(R_CR, CR_RST);
    int spins = 0;
    while (m8(R_CR) & CR_RST) {
        if (++spins > 1000000) { kprintf("[rtl8169] reset timeout\n"); return -1; }
    }

    for (int i = 0; i < 6; i++) rtl_dev.mac[i] = m8(R_IDR0 + (uint32_t)i);
    rtl_dev.irq_line = dev->irq_line;

    w8(R_CFG9346, CFG9346_UNLOCK);
    w16(R_CPCMD, 0);                               /* no VLAN / checksum offload */
    w8(R_MTPS, 0x3B);                              /* 59 * 128 B max transmit */
    w16(R_RMS, BUF_SIZE);
    w32(R_TCR, 0x03000700u);                       /* normal IFG, max DMA burst */
    /* Accept physical-match + multicast + broadcast, max DMA burst, no receive
     * FIFO threshold. Not promiscuous, not runts, not errored frames. */
    w32(R_RCR, 0x0Eu | (7u << 8) | (7u << 13));
    w32(R_MAR0, 0xFFFFFFFFu);
    w32(R_MAR0 + 4, 0xFFFFFFFFu);
    w8(R_CR, CR_TE | CR_RE);
    /* Descriptor bases go in AFTER TE/RE per the 8169 bring-up order. */
    w64(R_TNPDS, tr);
    w64(R_RDSAR, rr);
    w16(R_ISR, 0xFFFF);
    w16(R_IMR, 0);                                 /* polled until irq_enable */
    w8(R_CFG9346, CFG9346_LOCK);

    rx_cur = tx_cur = 0;
    ready = 1;
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
}
