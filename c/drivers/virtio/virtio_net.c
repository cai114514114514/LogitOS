#include <stdint.h>
#include <stddef.h>
#include "virtio.h"
#include "netdev.h"
#include "netring.h"
#include "pci.h"
#include "pmm.h"
#include "net.h"
#include "pit.h"
#include "kprintf.h"

/* virtio-net over the existing modern virtio-pci transport.
 *
 * This is the highest-value NIC in the tree per line of code: every hypervisor
 * that is not our particular QEMU line offers virtio-net, and a VM on real
 * hardware will present it rather than an emulated Intel card. The transport
 * (virtio.c) already parses the vendor-0x09 capability chain, negotiates
 * VIRTIO_F_VERSION_1 and sets up split virtqueues, so all that is left is the
 * two queues and the 12-byte packet header.
 *
 * What it does NOT reuse is virtio_request(): that is a synchronous, single-
 * outstanding, poll-until-done call, which is right for a disk and wrong for a
 * NIC. A NIC keeps a whole ring of receive buffers posted with the device at
 * all times and consumes completions whenever they appear. So this driver
 * drives the virtqueue structures directly -- they are public in virtio.h --
 * and virtio.c is untouched.
 */

void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);

#define VIRTIO_NET_F_MAC     (1u << 5)
#define VIRTIO_NET_F_STATUS  (1u << 16)

/* virtio-v1.1 sec 5.1.4, struct virtio_net_config: mac[6] then a little-endian
 * u16 `status` at byte offset 6 -- present in the device-specific config
 * region only when VIRTIO_NET_F_STATUS was ACCEPTED (same trap as the MAC
 * read below: a device that did not offer the feature has nothing meaningful
 * at this offset, so this is read only behind the same features_lo check). */
#define VIRTIO_NET_CFG_STATUS_OFF 6
#define VIRTIO_NET_S_LINK_UP      1u   /* bit 0 of the status field */

/* Queue 0 receives, queue 1 transmits. (With VIRTIO_NET_F_MQ, which we do not
 * negotiate, there would be more pairs and a control queue.) */
#define VQ_RX 0
#define VQ_TX 1

#define RX_BUFS  32
#define TX_BUFS  16
#define BUF_SIZE 2048        /* VNET_HDR_LEN + 1514 + slack, one per 4 KiB page */

/* VIRTQ_AVAIL_F_NO_INTERRUPT: "do not interrupt me when you consume these".
 * Set on the TX ring only -- transmit completions are reclaimed lazily on the
 * next send, so an interrupt per transmitted packet is pure overhead, and on a
 * busy connection it doubles the NIC interrupt rate for nothing. */
#define VIRTQ_AVAIL_F_NO_INTERRUPT 1

static struct virtio_dev vnet;
static struct virtq      rxq, txq;
static uint8_t          *rx_buf[RX_BUFS];
static uint8_t          *tx_buf[TX_BUFS];
static uint8_t           tx_free[TX_BUFS];
static uint16_t          tx_cur;
static int               ready;
static net_rx_cb         g_rxcb;
static int               have_status_cfg;  /* VIRTIO_NET_F_STATUS actually accepted */

/* ======================================================================== */
/* STATISTICS AND LINK STATE -- the same gap e1000.c had (see its own header  */
/* comment on the block of the same name), closed the same shape here. There  */
/* is no hardware read-to-clear accumulator to fold on this device the way    */
/* e1000's GPRC/GORCL/etc are: virtio-net's spec has no statistics feature    */
/* this transport negotiates, so these are SOFTWARE counters, incremented at  */
/* the one place in tx/rx each outcome is already decided.                    */
/* ======================================================================== */

static uint32_t g_rx_ok, g_tx_ok, g_tx_qfull;   /* software counters */

static int      g_link_known;   /* 0 until the first observation */
static int      g_link_up;      /* last reported state */
static uint64_t g_stat_next_ms;
static uint32_t g_report_pkts;
static uint32_t g_report_losses;

#define STAT_PERIOD_MS     1000   /* same period and argument as e1000's */
#define REPORT_EVERY_PKTS  512    /* same argument as e1000's -- stay silent on
                                    * a DHCP/ARP-only boot; see e1000.c */

static void link_report(int up)
{
    kprintf("[virtio-net] link: %s\n", up ? "UP" : "DOWN");
}

/* Once per transition, mirroring e1000's link_check(). Without
 * VIRTIO_NET_F_STATUS accepted there is nothing to read -- report UP once,
 * unconditionally, which is what every version of this driver before this
 * change silently assumed by never checking at all. */
static void link_check(void)
{
    int up = 1;
    if (have_status_cfg) {
        uint16_t status = (uint16_t)vnet.device[VIRTIO_NET_CFG_STATUS_OFF] |
                           ((uint16_t)vnet.device[VIRTIO_NET_CFG_STATUS_OFF + 1] << 8);
        up = (status & VIRTIO_NET_S_LINK_UP) != 0;
    }
    if (!g_link_known) { g_link_known = 1; g_link_up = up; link_report(up); return; }
    if (up == g_link_up) return;
    g_link_up = up;
    link_report(up);
}

static void stats_report(void)
{
    kprintf("[virtio-net] stats: rx %u ok, tx %u ok / %u ring-full\n",
            g_rx_ok, g_tx_ok, g_tx_qfull);
}

/* Called from inside rx_poll, under net_lock -- same placement as e1000's and
 * rtl8139's stats_poll(). Unlike rtl8139's rx_poll, this driver already reads
 * an extra register every call (vnet.isr, to ack -- see the comment above that
 * read), so an additional access rate-limited to once/second is a much
 * smaller relative addition here than it was on the 8139. */
static void stats_poll(void)
{
    uint64_t now = timer_ms();
    if (now < g_stat_next_ms) return;
    g_stat_next_ms = now + STAT_PERIOD_MS;

    link_check();

    uint32_t pkts = g_rx_ok + g_tx_ok;
    if (g_tx_qfull != g_report_losses || pkts >= g_report_pkts + REPORT_EVERY_PKTS) {
        g_report_losses = g_tx_qfull;
        g_report_pkts = pkts;
        stats_report();
    }
}

static inline void barrier(void) { __asm__ volatile ("mfence" ::: "memory"); }

/* Publish one descriptor index on a queue's available ring. One descriptor per
 * buffer: virtio 1.0 devices must accept any descriptor layout, so the 12-byte
 * header and the frame live in the same buffer and no chaining is needed.
 * Descriptor index == buffer index, per queue, which is what makes a completion
 * (which reports only the head descriptor id) name its own buffer. */
static void vq_publish(struct virtq *vq, uint16_t d, const void *buf,
                       uint32_t len, int device_writes)
{
    vq->desc[d].addr  = (uint64_t)(uintptr_t)buf;   /* identity-mapped: phys == virt */
    vq->desc[d].len   = len;
    vq->desc[d].flags = (uint16_t)(device_writes ? VIRTQ_DESC_F_WRITE : 0);
    vq->desc[d].next  = 0;
    vq->avail->ring[vq->avail->idx % vq->size] = d;
    barrier();
    vq->avail->idx++;
}

/* Reap transmit completions so their buffers can be reused. */
static void tx_reclaim(void)
{
    barrier();
    while (vq_pending(txq.used->idx, txq.last_used)) {
        struct virtq_used_elem *e = &txq.used->ring[vq_slot(txq.last_used, txq.size)];
        if (e->id < TX_BUFS) tx_free[e->id] = 1;
        txq.last_used++;
    }
}

/* Contract, same as every other NIC here: callers hold net_lock (IF=0). */
static int vnet_tx(const void *frame, uint16_t len)
{
    if (!ready || len == 0 || (uint32_t)len + VNET_HDR_LEN > BUF_SIZE) return -1;
    tx_reclaim();
    if (!tx_free[tx_cur]) {
        /* The ring is full and the device has not caught up. Poll briefly --
         * bounded, because this can run inside the NIC IRQ with the BKL held,
         * and an unbounded wait there is a machine freeze, not a slow send. */
        for (int spins = 0; spins < 200000 && !tx_free[tx_cur]; spins++)
            tx_reclaim();
        if (!tx_free[tx_cur]) { g_tx_qfull++; return -1; }
    }
    uint16_t d = tx_cur;
    memset(tx_buf[d], 0, VNET_HDR_LEN);        /* no checksum offload, no GSO */
    memcpy(tx_buf[d] + VNET_HDR_LEN, frame, len);
    tx_free[d] = 0;
    tx_cur = (uint16_t)ring_next(d, TX_BUFS);
    vq_publish(&txq, d, tx_buf[d], VNET_HDR_LEN + len, 0);
    barrier();
    *txq.notify = VQ_TX;
    g_tx_ok++;
    return 0;
}

static int vnet_rx_poll(net_rx_cb cb)
{
    if (!ready) return 0;
    uint64_t f = net_lock();                   /* exclude the RX IRQ + mainline tcp_recv */
    /* Ack here as well as in the ISR, for the reason written out in full in
     * e1000_rx_drain(): reading the ISR status byte is what deasserts the
     * device's INTx line, that line is routed EDGE-triggered (smp.c), and a
     * line left asserted produces no further edges -- so if the only reader is
     * an ISR that can no longer run, the interrupt is dead for the boot.
     *
     * This is not theoretical for virtio-net; the driver comparison measured
     * it. Over 1920 segments each, with only e1000 fixed:
     *     e1000       rx->ack median 0.69 ms  p99 1.67   max  2.07
     *     virtio-net  rx->ack median 1.08 ms  p99 9.88   max 10.06
     *     rtl8139     rx->ack median 1.72 ms  p99 9.94   max 10.52
     * A 10 ms tail on a 100 Hz kernel is a receive path that only runs when the
     * timer says so, and it appeared on exactly the two cards that had not had
     * this line added. */
    if (vnet.isr) (void)*vnet.isr;
    int n = 0, reposted = 0;
    /* Bounded for the same reason as e1000: each iteration hands the buffer
     * straight back, so a sustained flood makes an unbounded loop non-
     * terminating, and in the ISR that is a hard freeze. */
    int budget = RX_BUFS;
    barrier();
    while (budget-- > 0 && vq_pending(rxq.used->idx, rxq.last_used)) {
        struct virtq_used_elem *e = &rxq.used->ring[vq_slot(rxq.last_used, rxq.size)];
        uint32_t id = e->id, got = e->len;
        rxq.last_used++;
        if (id < RX_BUFS) {
            /* `got` counts what the DEVICE wrote, header included. Anything at
             * or below the header length carries no frame. */
            if (got > VNET_HDR_LEN && got <= BUF_SIZE) {
                cb(rx_buf[id] + VNET_HDR_LEN, (uint16_t)(got - VNET_HDR_LEN));
                g_rx_ok++;
            }
            vq_publish(&rxq, (uint16_t)id, rx_buf[id], BUF_SIZE, 1);
            reposted++;
        }
        n++;
    }
    if (reposted) { barrier(); *rxq.notify = VQ_RX; }
    stats_poll();
    net_unlock(f);
    return n;
}

static void vnet_irq_on(net_rx_cb cb) { g_rxcb = cb; }

static void vnet_isr(void)
{
    if (!ready) return;
    /* Read-to-clear the ISR status byte: until this read the device holds its
     * interrupt asserted and will not raise a new one. */
    if (vnet.isr) (void)*vnet.isr;
    /* Ack here, drain on SOFTIRQ_NET -- see c/net/core/net.c. */
    if (g_rxcb) net_rx_schedule();
}

static struct netdev vnet_dev = {
    .name = "virtio-net", .irq_line = -1,
    .tx = vnet_tx, .rx_poll = vnet_rx_poll,
    .irq_enable = vnet_irq_on, .irq = vnet_isr,
};

static void free_bufs(void)
{
    for (int i = 0; i < RX_BUFS; i++) if (rx_buf[i]) { pmm_free((uint64_t)(uintptr_t)rx_buf[i]); rx_buf[i] = NULL; }
    for (int i = 0; i < TX_BUFS; i++) if (tx_buf[i]) { pmm_free((uint64_t)(uintptr_t)tx_buf[i]); tx_buf[i] = NULL; }
}

int virtio_net_probe(struct device *dev)
{
    if (ready) return -1;                        /* one NIC bound at a time */
    dev_enable(dev, 1);                          /* memory decode + bus master (DMA) */
    /* virtio.c owns the transport and locates the device itself by ID; hand it
     * the ID the match table matched, so a transitional (0x1000) and a modern
     * (0x1041) device both work without this driver caring which it got. */
    if (virtio_init(dev->device, &vnet, VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS) != 0)
        return -1;
    if (virtio_queue_setup(&vnet, VQ_RX, &rxq) != 0 ||
        virtio_queue_setup(&vnet, VQ_TX, &txq) != 0) {
        kprintf("[virtio-net] queue setup failed\n");
        return -1;
    }
    if (rxq.size < RX_BUFS || txq.size < TX_BUFS) {
        kprintf("[virtio-net] queues too small (rx=%d tx=%d)\n", rxq.size, txq.size);
        return -1;
    }

    for (int i = 0; i < RX_BUFS; i++) {
        rx_buf[i] = (uint8_t *)pmm_alloc();      /* identity-mapped: phys == virt */
        if (!rx_buf[i]) { free_bufs(); return -1; }
    }
    for (int i = 0; i < TX_BUFS; i++) {
        tx_buf[i] = (uint8_t *)pmm_alloc();
        if (!tx_buf[i]) { free_bufs(); return -1; }
        tx_free[i] = 1;
    }
    tx_cur = 0;
    txq.avail->flags = VIRTQ_AVAIL_F_NO_INTERRUPT;

    /* The MAC. Bit 5 must have been ACCEPTED, not merely asked for: reading the
     * device config for a feature the device did not offer returns whatever
     * happens to be there, and a NIC that quietly adopts a garbage MAC gets no
     * ARP replies and looks like a dead link. */
    if ((vnet.features_lo & VIRTIO_NET_F_MAC) && vnet.device) {
        for (int i = 0; i < 6; i++) vnet_dev.mac[i] = vnet.device[i];
    } else {
        /* Locally-administered, unicast: the low bit of byte 0 clear (unicast),
         * bit 1 set (locally administered). */
        static const uint8_t fallback[6] = { 0x02, 0x4C, 0x4F, 0x47, 0x49, 0x54 };
        memcpy(vnet_dev.mac, fallback, 6);
        kprintf("[virtio-net] device offers no MAC; using a local one\n");
    }
    /* Same ACCEPTED-not-merely-ASKED-for rule as the MAC just above: read the
     * status field only when the device actually offered VIRTIO_NET_F_STATUS,
     * or link_check() would decode bytes of an unrelated field (or, on a
     * device with a shorter config than ours, bytes past it). */
    have_status_cfg = (vnet.features_lo & VIRTIO_NET_F_STATUS) && vnet.device;
    vnet_dev.irq_line = dev->irq_line;

    /* Post the whole receive ring BEFORE DRIVER_OK. The device may not touch a
     * queue until DRIVER_OK, so this cannot race; doing it after leaves a window
     * where the link is live with nothing posted and the first frames are lost.
     * We never enable MSI-X, so the device falls back to legacy INTx on the PCI
     * line -- which is the line smp.c routes to vector 65. */
    for (int i = 0; i < RX_BUFS; i++)
        vq_publish(&rxq, (uint16_t)i, rx_buf[i], BUF_SIZE, 1);

    virtio_driver_ok(&vnet);
    ready = 1;
    barrier();
    *rxq.notify = VQ_RX;

    kprintf("[virtio-net] up: %d rx / %d tx buffers, queues %d/%d, feat=%x\n",
            RX_BUFS, TX_BUFS, rxq.size, txq.size, vnet.features_lo);
    dev_set_drvdata(dev, &vnet_dev);
    return 0;
}
