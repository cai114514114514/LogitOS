/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stddef.h>
#include "netdev.h"
#include "pcnet_ring.h"
#include "dma.h"
#include "io.h"
#include "net.h"
#include "pit.h"
#include "kprintf.h"

/* AMD PCnet-PCI II Am79C970A (1022:2000), style-2 DMA and word I/O.
 * Register programming follows AMD 19436 and was cross-checked against the
 * actual QEMU model: https://github.com/qemu/qemu/blob/master/hw/net/pcnet.c
 * This adds a different controller, including its own ownership protocol;
 * it does not pretend that an AMD PCI ID can use an Intel descriptor ring.
 * One controller instance, 1500-byte MTU, no checksum/VLAN offload or suspend.
 * Same-family second cards are refused, as in the existing NIC drivers.
 */
void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);

#define R_RDP 0x10
#define R_RAP 0x12
#define R_RESET16 0x14
#define R_RESET32 0x18
#define R_BDP 0x16
#define C_INIT 0x0001
#define C_STRT 0x0002
#define C_STOP 0x0004
#define C_TDMD 0x0008
#define C_TXON 0x0010
#define C_RXON 0x0020
#define C_INEA 0x0040
#define C_IDON 0x0100
#define C_MERR 0x0800
#define C_CAUSES 0x7f00

static uint16_t io;
static int ready, interrupts_on, link_known, link_up;
static uint64_t next_report;
static uint32_t rx_ok, rx_bad, tx_ok, tx_bad, reported_packets;
static unsigned rx_head, tx_head, tx_tail, tx_pending;
static net_rx_cb rx_callback;
static struct dma_device pcnet_dma;
static struct dma_buffer *metadata, *rx_payload, *tx_payload;
static volatile struct pcnet_desc *rx_ring, *tx_ring;

/* RAP is shared by CSR and BCR. All runtime callers hold NET_GUARD, including
 * the interrupt handler; otherwise an IRQ between RAP and RDP can write the
 * STOP command into an unrelated register and leave the card DMAing forever. */
static uint16_t csr_read(unsigned reg)
{ outw(io + R_RAP, (uint16_t)reg); return inw(io + R_RDP); }
static void csr_write(unsigned reg, uint16_t value)
{ outw(io + R_RAP, (uint16_t)reg); outw(io + R_RDP, value); }
static uint16_t bcr_read(unsigned reg)
{ outw(io + R_RAP, (uint16_t)reg); return inw(io + R_BDP); }
static void bcr_write(unsigned reg, uint16_t value)
{ outw(io + R_RAP, (uint16_t)reg); outw(io + R_BDP, value); }

static void free_buffers(void)
{
    while (pcnet_dma.buffers)
        if (dma_free_coherent(pcnet_dma.buffers) != 0) break;
    metadata = rx_payload = tx_payload = NULL;
    rx_ring = tx_ring = NULL;
}

static int stop_dma(void)
{
    csr_write(0, C_STOP);
    for (unsigned i = 0; i < 100000; i++) {
        uint16_t state = csr_read(0);
        if (state != 0xffffu && (state & C_STOP)) return 0;
    }
    return -1;
}

static void report_link(void)
{
    /* BCR4 is configured for LNKSTE alone, so LEDOUT reports the link rather
     * than the OR of activity/error signals that other LED masks permit. */
    int up = !!(bcr_read(4) & 0x8000);
    if (!link_known || up != link_up) {
        link_known = 1; link_up = up;
        kprintf("[pcnet] link: %s\n", up ? "UP" : "DOWN");
    }
}

static void reap_tx(void)
{
    while (tx_pending) {
        volatile struct pcnet_desc *d = &tx_ring[tx_tail];
        uint16_t status = d->status;
        if (status & PCNET_OWN) break;
        dma_rmb();
        /* Count hardware completion, not the act of setting OWN. A queue
         * submission says nothing about whether the controller sent bytes. */
        if (status & PCNET_ERR) tx_bad++; else tx_ok++;
        tx_tail = pcnet_next(tx_tail);
        tx_pending--;
    }
}

static void ack_causes(void)
{
    uint16_t cause = csr_read(0);
    csr_write(0, (cause & C_CAUSES) | (interrupts_on ? C_INEA : 0));
    if (cause & C_MERR) {
        /* MERR can stop the DMA engines. Do not claim a running interface or
         * recycle its backing pages while the stop state is unconfirmed. */
        ready = 0;
        kprintf("[pcnet] DMA memory error, interface stopped\n");
    }
}

static int pcnet_rx_poll(net_rx_cb cb)
{
    NET_GUARD;
    if (!ready) return 0;
    /* Acknowledge before draining. net_poll also runs from its timer, so an
     * RX cause cannot remain asserted forever on the kernel's edge GSI path. */
    ack_causes();
    if (!ready) return 0;
    reap_tx();
    int delivered = 0;
    for (unsigned budget = 0; budget < PCNET_RING_COUNT; budget++) {
        volatile struct pcnet_desc *d = &rx_ring[rx_head];
        uint16_t status = d->status;
        if (status & PCNET_OWN) break;
        dma_rmb();
        unsigned len = pcnet_rx_length(status, d->misc);
        if (len) {
            cb((uint8_t *)rx_payload->cpu + rx_head * PCNET_BUF_SIZE, (uint16_t)len);
            rx_ok++; delivered++;
        } else rx_bad++;
        d->misc = 0; d->reserved = 0;
        d->bcnt = pcnet_bcnt(PCNET_BUF_SIZE);
        dma_wmb();
        d->status = PCNET_OWN;   /* publish last: the NIC may DMA immediately */
        rx_head = pcnet_next(rx_head);
    }
    uint64_t now = timer_ms();
    if (now >= next_report) {
        next_report = now + 1000;
        report_link();
        if (rx_ok + tx_ok >= reported_packets + 64) {
            reported_packets = rx_ok + tx_ok;
            kprintf("[pcnet] completed: rx=%u tx=%u rx_bad=%u tx_bad=%u\n",
                    rx_ok, tx_ok, rx_bad, tx_bad);
        }
    }
    return delivered;
}

static int pcnet_tx(const void *frame, uint16_t len)
{
    NET_GUARD;
    if (!ready || !frame || len < 14 || len > PCNET_FRAME_MAX) return -1;
    reap_tx();
    if (tx_pending == PCNET_RING_COUNT) return -1;
    volatile struct pcnet_desc *d = &tx_ring[tx_head];
    if (d->status & PCNET_OWN) return -1;
    unsigned bytes = len < 60 ? 60 : len;
    uint8_t *payload = (uint8_t *)tx_payload->cpu + tx_head * PCNET_BUF_SIZE;
    if (bytes > len) memset(payload, 0, bytes);
    memcpy(payload, frame, len);
    d->bcnt = pcnet_bcnt(bytes); d->misc = 0; d->reserved = 0;
    dma_wmb();
    d->status = PCNET_OWN | PCNET_STP | PCNET_ENP;
    dma_wmb();
    tx_head = pcnet_next(tx_head); tx_pending++;
    csr_write(0, C_TDMD | (interrupts_on ? C_INEA : 0));
    return 0;
}

static void pcnet_irq_enable(net_rx_cb cb)
{
    NET_GUARD;
    if (!ready) return;
    rx_callback = cb; interrupts_on = 1;
    csr_write(0, C_CAUSES | C_INEA);
}
static void pcnet_irq(void)
{
    NET_GUARD;
    if (!ready) return;
    uint16_t causes = csr_read(0) & C_CAUSES;
    if (!causes) return; /* shared IRQ: do not schedule another card's work */
    ack_causes();
    if (ready && rx_callback) net_rx_schedule();
}
static struct netdev pcnet_dev = {
    .name = "pcnet", .irq_line = -1,
    .tx = pcnet_tx, .rx_poll = pcnet_rx_poll,
    .irq_enable = pcnet_irq_enable, .irq = pcnet_irq,
};

int pcnet_probe(struct device *dev)
{
    if (ready || pcnet_dma.blocked) return -1;
    io = 0;
    for (unsigned i = 0; i < DEV_NRES; i++)
        if ((dev->res[i].flags & DEV_RES_IO) && dev->res[i].size >= 0x20 &&
            dev->res[i].start && dev->res[i].start <= 0xffe0) {
            io = (uint16_t)dev->res[i].start; break;
        }
    if (!io) return -1;
    if (dev_enable_checked(dev, 0) != 0) {
        kprintf("[pcnet] PCI Command decode rejected\n"); return -1;
    }
    /* Firmware may have left DWIO selected. Reading both reset aliases gets
     * back to word I/O; the inactive alias is ignored by this controller. */
    (void)inl(io + R_RESET32);
    (void)inw(io + R_RESET16);
    if (!(csr_read(0) & C_STOP)) {
        kprintf("[pcnet] reset did not acknowledge STOP\n"); return -1;
    }
    /* 1022:2000 is also used by later PCnet/FAST parts with an external MII
     * PHY. Their descriptor format agrees but their link setup does not.
     * The chip version prevents claiming those boards on PCI ID alone. */
    uint32_t chip = (uint32_t)csr_read(88) | ((uint32_t)csr_read(89) << 16);
    if ((chip & 0xfffu) != 3u || ((chip >> 12) & 0xffffu) != 0x2621u) {
        kprintf("[pcnet] unsupported silicon %x: requires separate PHY setup\n", chip);
        return -1;
    }
    bcr_write(20, 2);
    if ((bcr_read(20) & 0xff) != 2) return -1;
    bcr_write(2, bcr_read(2) | 2); /* auto-select 10BASE-T/AUI */
    bcr_write(4, 0x0040);         /* report link only, not activity */
    for (unsigned i = 0; i < 6; i++) pcnet_dev.mac[i] = inb(io + i);
    if ((pcnet_dev.mac[0] & 1) ||
        !(pcnet_dev.mac[0] | pcnet_dev.mac[1] | pcnet_dev.mac[2] |
          pcnet_dev.mac[3] | pcnet_dev.mac[4] | pcnet_dev.mac[5])) return -1;

    dma_device_init(&pcnet_dma, "pcnet", DMA_MASK_32);
    metadata = dma_alloc_coherent(&pcnet_dma, 4096, 4096, 0);
    rx_payload = dma_alloc_coherent(&pcnet_dma, PCNET_RING_COUNT * PCNET_BUF_SIZE, 4096, 0);
    tx_payload = dma_alloc_coherent(&pcnet_dma, PCNET_RING_COUNT * PCNET_BUF_SIZE, 4096, 0);
    if (!metadata || !rx_payload || !tx_payload) goto allocation_fail;
    memset(metadata->cpu, 0, 4096);
    struct pcnet_init_block *init = metadata->cpu;
    rx_ring = (volatile struct pcnet_desc *)((uint8_t *)metadata->cpu + 0x100);
    tx_ring = (volatile struct pcnet_desc *)((uint8_t *)metadata->cpu + 0x400);
    for (unsigned i = 0; i < PCNET_RING_COUNT; i++) {
        rx_ring[i].addr = (uint32_t)dma_addr_value(rx_payload->dma) + i * PCNET_BUF_SIZE;
        rx_ring[i].bcnt = pcnet_bcnt(PCNET_BUF_SIZE);
        rx_ring[i].status = PCNET_OWN;
        tx_ring[i].addr = (uint32_t)dma_addr_value(tx_payload->dma) + i * PCNET_BUF_SIZE;
        tx_ring[i].bcnt = pcnet_bcnt(PCNET_BUF_SIZE);
    }
    init->rlen = init->tlen = PCNET_RING_LOG2 << 4;
    memcpy(init->mac, pcnet_dev.mac, 6);
    init->multicast[0] = init->multicast[1] = UINT32_MAX; /* IPv6 ND multicast */
    init->rx_ring = (uint32_t)dma_addr_value(metadata->dma) + 0x100;
    init->tx_ring = (uint32_t)dma_addr_value(metadata->dma) + 0x400;
    if (dev_enable_checked(dev, 1) != 0) {
        kprintf("[pcnet] PCI bus-master enable rejected\n");
        goto allocation_fail;
    }
    for (struct dma_buffer *b = pcnet_dma.buffers; b; b = b->next)
        dma_buffer_submit(b);
    dma_wmb();
    uint32_t addr = (uint32_t)dma_addr_value(metadata->dma);
    csr_write(1, addr & 0xffff); csr_write(2, addr >> 16);
    /* Mask IDON/TX/BABL interrupts. RX, missed RX and memory errors wake the
     * network softirq. TX completions are reaped in polling and before send. */
    csr_write(3, 0x4300);
    csr_write(0, C_INIT);
    unsigned wait;
    for (wait = 0; wait < 1000000; wait++)
        if (csr_read(0) & C_IDON) break;
    if (wait == 1000000) {
        kprintf("[pcnet] initialization timeout\n"); goto published_fail;
    }
    csr_write(0, C_IDON | C_STRT);
    if ((csr_read(0) & (C_RXON | C_TXON)) != (C_RXON | C_TXON)) goto published_fail;
    rx_head = tx_head = tx_tail = tx_pending = 0;
    rx_ok = rx_bad = tx_ok = tx_bad = reported_packets = 0;
    interrupts_on = link_known = 0; next_report = 0; rx_callback = NULL;
    pcnet_dev.irq_line = dev->irq_line;
    ready = 1;
    kprintf("[pcnet] up: io=%x style=2 rx=%u tx=%u DMA32\n", io,
            PCNET_RING_COUNT, PCNET_RING_COUNT);
    report_link();
    dev_set_drvdata(dev, &pcnet_dev);
    return 0;
published_fail:
    if (stop_dma()) {
        dma_device_quarantine(&pcnet_dma);
        kprintf("[pcnet] STOP unconfirmed: DMA quarantined\n"); return -1;
    }
    dma_device_quiesced(&pcnet_dma);
allocation_fail:
    free_buffers();
    return -1;
}

void pcnet_remove(struct device *dev)
{
    NET_GUARD;
    if (!io) return;
    ready = interrupts_on = 0; rx_callback = NULL;
    if (stop_dma()) {
        dma_device_quarantine(&pcnet_dma);
        kprintf("[pcnet] remove STOP unconfirmed: DMA quarantined\n"); return;
    }
    dma_device_quiesced(&pcnet_dma);
    free_buffers();
    dev_set_drvdata(dev, NULL);
    io = 0;
}
