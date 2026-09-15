/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stddef.h>
#include "netdev.h"
#include "e1000e_ring.h"
#include "pci.h"
#include "dma.h"
#include "net.h"
#include "ktime.h"
#include "kprintf.h"

/* Intel 82574 PCIe copper controller (8086:10d3/10f6), a distinct init path
 * from e1000.c's 8254x. Intel 82574 datasheet rev 3.3 sections 4.5/4.6 and
 * 10.2 describe the NVM/MDIO ownership, PCIe master drain and queue setup:
 * https://www.mouser.com/pdfdocs/82574datasheet.pdf (Intel-authored document).
 * Intel's Linux e1000e/82571.c and phy.c supply the BM PHY / PCIe errata steps.
 * QEMU's e1000e model is our executable register-level check; it cannot verify
 * a physical board's NVM, electrical link, ASPM or firmware interaction.
 *
 * One queue pair, one controller, 1500 MTU, legacy descriptors, INTx plus timer
 * polling. ICH/PCH/I217/I219 and igb controllers are intentionally unmatched:
 * their PHY/firmware contracts are different, even though Linux calls some
 * of those drivers "e1000e" too. No suspend, NVM writes, TSO or checksum offload.
 */
void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);
#define CTRL 0x0000
#define STATUS 0x0008
#define EECD 0x0010
#define CTRL_EXT 0x0018
#define MDIC 0x0020
#define ICR 0x00c0
#define ITR 0x00c4
#define IMS 0x00d0
#define IMC 0x00d8
#define IAM 0x00e0
#define RCTL 0x0100
#define TCTL 0x0400
#define TIPG 0x0410
#define EXTCNF_CTRL 0x0f00
#define PBA 0x1000
#define RX_BASE 0x2800
#define RXDCTL 0x2828
#define TX_BASE 0x3800
#define TXDCTL 0x3828
#define TARC 0x3840
#define RXCSUM 0x5000
#define RFCTL 0x5008
#define RAL 0x5400
#define RAH 0x5404
#define WUC 0x5800
#define MANC 0x5820
#define GCR 0x5b00
#define GCR2 0x5b64
#define POEMB 0x0f10
#define CTRL_MASTER_DISABLE (1u << 2)
#define STATUS_MASTER_ENABLE (1u << 19)
#define CTRL_RESET (1u << 26)
#define MDIO_OWN (1u << 5)
#define IRQ_WANTED ((1u << 7) | (1u << 2))

static volatile uint8_t *mmio;
static int ready, link_known;
static uint32_t last_link, rx_ok, rx_bad, tx_ok, tx_bad, report_count;
static uint64_t next_report;
static unsigned rx_head, tx_head, tx_tail, tx_pending;
static net_rx_cb rx_callback;
static struct dma_device e1k_dma;
static struct dma_buffer *rings, *rx_payload, *tx_payload;
static volatile struct e1k_rx_desc *rx_ring;
static volatile struct e1k_tx_desc *tx_ring;
static uint32_t rd(unsigned r) { return *(volatile uint32_t *)(mmio + r); }
static void wr(unsigned r, uint32_t v) { *(volatile uint32_t *)(mmio + r) = v; }
static void flush(void) { (void)rd(STATUS); }

/* The PIT tick does not advance with IF=0; probe and NET_GUARD run that way.
 * Use the calibrated free-running clock, with an iteration ceiling so a bad
 * platform clock cannot turn a missing device into an infinite boot hang. */
static int delay_us(unsigned us)
{
    uint64_t end = time_mono_ns() + (uint64_t)us * NS_PER_US;
    for (unsigned i = 0; i < 10000000; i++) {
        if (time_mono_ns() >= end) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}
static int wait_bit(unsigned reg, uint32_t mask, int set, unsigned timeout_us)
{
    for (unsigned t = 0; t < timeout_us; t += 10) {
        if (!!(rd(reg) & mask) == set) return 0;
        if (delay_us(10)) break;
    }
    return -1;
}
static void mdio_release(void) { wr(EXTCNF_CTRL, rd(EXTCNF_CTRL) & ~MDIO_OWN); flush(); }
static int mdio_acquire(void)
{
    for (unsigned i = 0; i < 100; i++) {
        wr(EXTCNF_CTRL, rd(EXTCNF_CTRL) | MDIO_OWN);
        if (rd(EXTCNF_CTRL) & MDIO_OWN) return 0;
        if (delay_us(1000)) break;
    }
    mdio_release();
    return -1;
}
static int phy_io(unsigned reg, uint16_t *value, int write)
{
    wr(MDIC, (reg << 16) | (1u << 21) | (write ? (1u << 26) | *value : (1u << 27)));
    if (wait_bit(MDIC, E1K_MDIC_READY, 1, 5000)) return -1;
    uint32_t result = rd(MDIC);
    if (!e1k_mdic_valid(result, reg)) return -1;
    if (!write) *value = (uint16_t)result;
    return 0;
}
static int phy_read(unsigned reg, uint16_t *v) { return phy_io(reg, v, 0); }
static int phy_write(unsigned reg, uint16_t v) { return phy_io(reg, &v, 1); }
static int phy_commit(void)
{
    uint16_t ctrl;
    if (phy_read(0, &ctrl) || phy_write(0, (ctrl & ~0x0c00u) | 0x8000u)) return -1;
    for (unsigned i = 0; i < 500; i++) {
        if (delay_us(1000) || phy_read(0, &ctrl)) return -1;
        if (!(ctrl & 0x8000)) return 0;
    }
    return -1;
}

static int phy_setup(void)
{
    uint16_t id1, id2, control, adv, gig;
    if (mdio_acquire()) return -1;
    int result = -1;
    if (phy_write(22, 0) || phy_read(2, &id1) || phy_read(3, &id2)) goto out;
    uint32_t id = ((uint32_t)id1 << 16) | id2;
    if ((id & ~15u) != 0x01410cb0u) {
        kprintf("[e1000e] unsupported PHY %x\n", id); goto out;
    }
    /* BM PHY: bit 11 means downshift, not CRS-on-TX as on older Marvell
     * PHYs. The R2 erratum requires disabling, committing, then re-enabling
     * it. These steps also preserve board-specific fields loaded from NVM. */
    if (phy_read(16, &control)) goto out;
    control = (control & ~0x0802u) | 0x0060u; /* auto MDI-X, polarity correction */
    if (phy_write(16, control) || phy_commit()) goto out;
    if (phy_write(16, control | 0x0800u)) goto out;
    if ((id & 15u) == 1 && (phy_write(29, 3) || phy_write(30, 0))) goto out;
    if (phy_commit() || phy_read(4, &adv) || phy_read(9, &gig)) goto out;
    /* Do not advertise PAUSE while MAC flow control is disabled. 1000-half
     * is unsupported; 10/100 half/full and 1000 full use PHY autonegotiation. */
    adv = (adv & ~0x0fe0u) | 0x01e1u;
    gig = (gig & ~0x1300u) | 0x0200u; /* preserve port type; auto master/slave */
    if (phy_write(4, adv) || phy_write(9, gig) || phy_read(0, &control)) goto out;
    if (phy_write(0, (control & ~0xcc00u) | 0x1200u)) goto out;
    kprintf("[e1000e] PHY %x: auto MDI-X, autoneg 10/100/1000\n", id);
    result = 0;
out:
    mdio_release();
    return result;
}

static int master_stop(void)
{
    wr(IMC, UINT32_MAX); wr(RCTL, 0); wr(TCTL, rd(TCTL) & ~2u);
    wr(CTRL, rd(CTRL) | CTRL_MASTER_DISABLE); flush();
    /* CTRL.RST alone is insufficient on PCIe: pending bus transactions must
     * drain first or a subsequent DMA completion can target freed memory. */
    return wait_bit(STATUS, STATUS_MASTER_ENABLE, 0, 100000);
}
static void free_buffers(void)
{
    while (e1k_dma.buffers)
        if (dma_free_coherent(e1k_dma.buffers)) break;
    rings = rx_payload = tx_payload = NULL;
    rx_ring = NULL; tx_ring = NULL;
}
static void reap_tx(void)
{
    while (tx_pending) {
        uint8_t status = tx_ring[tx_tail].status;
        if (!(status & 1)) break;
        dma_rmb();
        if (status & 0x0e) tx_bad++; else tx_ok++;
        tx_tail = (tx_tail + 1) & (E1K_TX_COUNT - 1); tx_pending--;
    }
}
static void report_link(void)
{
    uint32_t status = rd(STATUS) & 0xc3u;
    if (link_known && last_link == status) return;
    last_link = status; link_known = 1;
    if (status & 2)
        kprintf("[e1000e] link: UP %u Mb/s %s duplex\n", (status & 0x80) ? 1000u :
                (status & 0x40) ? 100u : 10u, (status & 1) ? "full" : "half");
    else kprintf("[e1000e] link: DOWN\n");
    /* Collision distance is negotiated, not blindly set for gigabit. This
     * matters when the old PC is plugged into a 10/100 half-duplex switch. */
    wr(TCTL, (rd(TCTL) & ~(0x3ffu << 12)) | (((status & 1) ? 63u : 511u) << 12));
}
static int e1k_rx_poll(net_rx_cb cb)
{
    NET_GUARD;
    if (!ready) return 0;
    uint32_t cause = rd(ICR); /* clear even when IRQ routing is unavailable */
    reap_tx();
    int n = 0, consumed = 0;
    unsigned tail = 0;
    for (unsigned budget = 0; budget < E1K_RX_COUNT; budget++) {
        volatile struct e1k_rx_desc *d = &rx_ring[rx_head];
        uint8_t status = d->status;
        if (!(status & 1)) break;
        dma_rmb();
        if (e1k_rx_valid(status, d->errors, d->length)) {
            cb((uint8_t *)rx_payload->cpu + rx_head * E1K_BUFFER, d->length);
            rx_ok++; n++;
        } else rx_bad++;
        d->status = 0; tail = rx_head; consumed++;
        rx_head = (rx_head + 1) & (E1K_RX_COUNT - 1);
    }
    if (consumed) { dma_wmb(); wr(RX_BASE + 0x18, tail); }
    uint64_t now = time_mono_ms();
    if ((cause & 4) || now >= next_report) {
        next_report = now + 1000; report_link();
        if (rx_ok + tx_ok >= report_count + 64) {
            report_count = rx_ok + tx_ok;
            kprintf("[e1000e] completed: rx=%u tx=%u rx_bad=%u tx_bad=%u\n", rx_ok, tx_ok, rx_bad, tx_bad);
        }
    }
    return n;
}
static int e1k_tx(const void *frame, uint16_t len)
{
    NET_GUARD;
    if (!ready || !frame || len < 14 || len > E1K_MAX_FRAME) return -1;
    reap_tx();
    /* Keep one slot empty: Intel head == tail denotes EMPTY, not a full
     * queue. Filling all 32 slots makes a burst silently disappear. */
    if (tx_pending >= E1K_TX_COUNT - 1) return -1;
    volatile struct e1k_tx_desc *d = &tx_ring[tx_head];
    memcpy((uint8_t *)tx_payload->cpu + tx_head * E1K_BUFFER, frame, len);
    d->length = len; d->cso = d->css = 0; d->special = 0;
    d->cmd = 0x0b; /* EOP, insert FCS, report status; legacy descriptor */
    d->status = 0; dma_wmb();
    tx_head = (tx_head + 1) & (E1K_TX_COUNT - 1); tx_pending++;
    wr(TX_BASE + 0x18, tx_head);
    return 0;
}
static void e1k_irq_on(net_rx_cb cb)
{
    NET_GUARD;
    if (!ready) return;
    rx_callback = cb; (void)rd(ICR); wr(IMS, IRQ_WANTED); flush();
}
static void e1k_irq(void)
{
    NET_GUARD;
    if (!ready) return;
    if (rd(ICR) && rx_callback) net_rx_schedule();
}
static struct netdev e1k_dev = {
    .name = "e1000e", .irq_line = -1, .tx = e1k_tx, .rx_poll = e1k_rx_poll,
    .irq_enable = e1k_irq_on, .irq = e1k_irq,
};

int e1000e_probe(struct device *dev)
{
    if (mmio || e1k_dma.blocked || !time_ready()) return -1;
    if (!(dev->res[0].flags & DEV_RES_MEM) || !dev->res[0].start ||
        dev->res[0].size < 0x20000) return -1;
    /* Clear and read back firmware bus mastering while the function is still
     * in its inherited power state.  Bringing D3hot back to D0 first could
     * revive firmware-owned rings during the mandatory 10 ms settle window. */
    if (dev_enable_checked(dev, 0) != 0) {
        kprintf("[e1000e] PCI Command decode rejected\n"); return -1;
    }
    /* Return a firmware-suspended NIC to D0 before touching BAR registers;
     * writing back PMCSR.PME_Status=1 would accidentally acknowledge it. */
    uint8_t pm = pci_cap_find(dev->bus, dev->slot, dev->func, 1);
    if (pm) {
        uint16_t state = pci_cfg_read16(dev->bus, dev->slot, dev->func, pm + 4);
        if (state & 3) {
            pci_cfg_write16(dev->bus, dev->slot, dev->func, pm + 4, state & 0x7ffcu);
            if (delay_us(10000)) return -1;
        }
    }
    uint64_t base = dev_bar_map(dev, 0);
    if (!base) return -1;
    mmio = (volatile uint8_t *)(uintptr_t)base;
    if (master_stop() || delay_us(10000) || mdio_acquire()) goto fail;
    wr(CTRL, rd(CTRL) | CTRL_RESET); flush();
    mdio_release();
    if (wait_bit(CTRL, CTRL_RESET, 0, 100000) ||
        wait_bit(EECD, 1u << 9, 1, 100000) || delay_us(25000)) goto fail;
    wr(IMC, UINT32_MAX); (void)rd(ICR);
    if (rd(MANC) & (1u << 18)) {
        kprintf("[e1000e] firmware blocks PHY reset; refusing ownership\n"); goto fail;
    }
    /* Explicit INTx mode even after a warm firmware handoff enabled MSI.
     * The kernel's common NIC IRQ currently uses the GSI routing contract. */
    uint8_t msi = pci_cap_find(dev->bus, dev->slot, dev->func, PCI_CAP_MSI);
    uint8_t msix = pci_cap_find(dev->bus, dev->slot, dev->func, PCI_CAP_MSIX);
    if (msi) pci_cfg_write16(dev->bus, dev->slot, dev->func, msi + 2,
                            pci_cfg_read16(dev->bus, dev->slot, dev->func, msi + 2) & ~1u);
    if (msix) pci_cfg_write16(dev->bus, dev->slot, dev->func, msix + 2,
                             pci_cfg_read16(dev->bus, dev->slot, dev->func, msix + 2) & ~(1u << 15));
    pci_cfg_write16(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND,
                    pci_cfg_read16(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND) & ~PCI_CMD_INTX_DIS);
    wr(IAM, 0); wr(ITR, 0);
    wr(CTRL_EXT, (rd(CTRL_EXT) & ~(1u << 23)) | (1u << 22) | (1u << 28));
    wr(GCR, rd(GCR) | (1u << 22) | (1u << 27));
    wr(GCR2, rd(GCR2) | 1u); /* PCIe completion/ASPM erratum workaround */
    wr(POEMB, rd(POEMB) & ~6u); /* disable D0/non-D0 low-power link-up */
    wr(WUC, 0);
    wr(CTRL, (rd(CTRL) & ~((1u << 5) | (1u << 7) | (1u << 11) | (1u << 12) |
             (1u << 27) | (1u << 28) | (1u << 29) | CTRL_MASTER_DISABLE)) | (1u << 6));
    wr(0x28, 0); wr(0x2c, 0); wr(0x30, 0); /* no negotiated PAUSE */
    if (phy_setup()) { kprintf("[e1000e] PHY initialization failed\n"); goto fail; }
    uint32_t ral = rd(RAL), rah = rd(RAH);
    for (unsigned i = 0; i < 4; i++) e1k_dev.mac[i] = (uint8_t)(ral >> (i * 8));
    e1k_dev.mac[4] = rah; e1k_dev.mac[5] = rah >> 8;
    if ((ral & 1) || !(ral | (rah & 0xffffu))) goto fail;
    wr(RAH, (rah & 0xffffu) | (1u << 31));
    for (unsigned i = 1; i < 16; i++) { wr(RAL + 8 * i, 0); wr(RAH + 8 * i, 0); }
    for (unsigned i = 0; i < 128; i++) wr(0x5200 + 4 * i, 0);
    dma_device_init(&e1k_dma, "e1000e", DMA_MASK_64);
    rings = dma_alloc_coherent(&e1k_dma, 4096, 4096, 0);
    rx_payload = dma_alloc_coherent(&e1k_dma, E1K_RX_COUNT * E1K_BUFFER, 4096, 0);
    tx_payload = dma_alloc_coherent(&e1k_dma, E1K_TX_COUNT * E1K_BUFFER, 4096, 0);
    if (!rings || !rx_payload || !tx_payload) goto fail;
    memset(rings->cpu, 0, 4096);
    rx_ring = rings->cpu;
    tx_ring = (volatile struct e1k_tx_desc *)((uint8_t *)rings->cpu + 2048);
    for (unsigned i = 0; i < E1K_RX_COUNT; i++) rx_ring[i].addr = dma_addr_value(rx_payload->dma) + i * E1K_BUFFER;
    for (unsigned i = 0; i < E1K_TX_COUNT; i++) {
        tx_ring[i].addr = dma_addr_value(tx_payload->dma) + i * E1K_BUFFER;
        tx_ring[i].status = 1;
    }
    uint64_t rx = dma_addr_value(rings->dma), tx = rx + 2048;
    wr(RX_BASE, (uint32_t)rx); wr(RX_BASE + 4, rx >> 32);
    wr(RX_BASE + 8, E1K_RX_COUNT * 16); wr(RX_BASE + 0x10, 0); wr(RX_BASE + 0x18, E1K_RX_COUNT - 1);
    wr(TX_BASE, (uint32_t)tx); wr(TX_BASE + 4, tx >> 32);
    wr(TX_BASE + 8, E1K_TX_COUNT * 16); wr(TX_BASE + 0x10, 0); wr(TX_BASE + 0x18, 0);
    wr(PBA, 20); /* 20 KiB RX, remaining 20 KiB TX; no jumbo frames */
    wr(RFCTL, 0); wr(RXCSUM, 0); /* explicit legacy RX format, software checksums */
    wr(RXDCTL, 1u << 16);
    wr(TXDCTL, (rd(TXDCTL) & ~(0x3fu << 16)) | 0x01410000u);
    wr(TARC, (rd(TARC) & ~(0xfu << 27)) | (1u << 26));
    wr(TIPG, 8u | (2u << 10) | (10u << 20));
    wr(RX_BASE + 0x20, 0); wr(RX_BASE + 0x2c, 0); /* RX delay timers */
    if (dev_enable_checked(dev, 1) != 0) {
        kprintf("[e1000e] PCI bus-master enable rejected\n"); goto fail;
    }
    for (struct dma_buffer *b = e1k_dma.buffers; b; b = b->next) dma_buffer_submit(b);
    dma_wmb();
    wr(TCTL, 2u | 8u | (15u << 4) | (63u << 12));
    wr(RCTL, 2u | (1u << 4) | (1u << 15) | (1u << 26));
    wr(RX_BASE + 0x18, E1K_RX_COUNT - 1); flush();
    rx_head = tx_head = tx_tail = tx_pending = 0;
    rx_ok = rx_bad = tx_ok = tx_bad = report_count = 0;
    rx_callback = NULL; next_report = 0; link_known = 0;
    e1k_dev.irq_line = dev->irq_line; ready = 1;
    kprintf("[e1000e] up: Intel 82574 PCIe mmio=%p rx=%u tx=%u DMA64\n", (void *)(uintptr_t)base, E1K_RX_COUNT, E1K_TX_COUNT);
    report_link(); dev_set_drvdata(dev, &e1k_dev);
    return 0;
fail:
    if (master_stop()) {
        dma_device_quarantine(&e1k_dma);
        kprintf("[e1000e] PCIe master stop unconfirmed: DMA quarantined\n"); return -1;
    }
    dma_device_quiesced(&e1k_dma); free_buffers(); mmio = NULL;
    return -1;
}

void e1000e_remove(struct device *dev)
{
    NET_GUARD;
    if (!mmio) return;
    ready = 0; rx_callback = NULL;
    if (master_stop()) {
        dma_device_quarantine(&e1k_dma);
        kprintf("[e1000e] remove PCIe master stop unconfirmed: DMA quarantined\n"); return;
    }
    wr(CTRL_EXT, rd(CTRL_EXT) & ~(1u << 28));
    dma_device_quiesced(&e1k_dma); free_buffers();
    dev_set_drvdata(dev, NULL); mmio = NULL;
}
