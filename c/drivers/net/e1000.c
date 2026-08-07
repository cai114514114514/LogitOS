#include <stdint.h>
#include <stddef.h>
#include "netdev.h"
#include "netring.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "net.h"
#include "kprintf.h"

/* Intel 8254x (QEMU "e1000" = 82540EM). MMIO BAR0, legacy RX/TX descriptor
 * rings, IRQ-driven receive on RXT0 with net_poll() as the backstop.
 *
 * This used to BE the network layer -- eth.c called e1000_tx() by name. It is
 * now one entry in the netdev registry (see net_ids.inc for the match table);
 * everything below is static, and the only thing it exports is e1000_probe().
 */

void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);

/* --- e1000 register offsets (bytes from MMIO base) --- */
#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_ICR     0x00C0      /* interrupt cause read */
#define REG_IMS     0x00D0      /* interrupt mask set */
#define REG_IMC     0x00D8      /* interrupt mask clear */
#define ICR_RX      0xD0        /* RXT0 | RXO | RXDMT0 (all receive causes) */
#define ICR_RXT0    0x80        /* receive timer: one IRQ per received packet */
#define REG_RCTL    0x0100
#define REG_TCTL    0x0400
#define REG_TIPG    0x0410
#define REG_RDBAL   0x2800
#define REG_RDBAH   0x2804
#define REG_RDLEN   0x2808
#define REG_RDH     0x2810
#define REG_RDT     0x2818
#define REG_TDBAL   0x3800
#define REG_TDBAH   0x3804
#define REG_TDLEN   0x3808
#define REG_TDH     0x3810
#define REG_TDT     0x3818
#define REG_RAL0    0x5400
#define REG_RAH0    0x5404

#define CTRL_SLU    (1u << 6)   /* set link up */
#define CTRL_RST    (1u << 26)
#define CTRL_ASDE   (1u << 5)   /* auto speed detect enable */

#define RCTL_EN     (1u << 1)
#define RCTL_MPE    (1u << 4)   /* multicast promiscuous: accept ALL multicast.
                                 * IPv6 needs it. Router Advertisements arrive
                                 * on the all-nodes group and address-resolution
                                 * solicitations on our solicited-node group, so
                                 * with this bit clear the NIC silently drops
                                 * exactly the frames Neighbour Discovery and
                                 * SLAAC depend on -- v6 looks "implemented but
                                 * dead". The precise alternative is programming
                                 * the MTA hash for each joined group; that is
                                 * the NIC line's call, and this bit is the
                                 * minimum that makes IPv6 work today. */
#define RCTL_BAM    (1u << 15)  /* broadcast accept */
#define RCTL_SECRC  (1u << 26)  /* strip ethernet CRC */
#define RCTL_BSIZE_2048 0       /* (BSEX=0, SZ=00) */

#define TCTL_EN     (1u << 1)
#define TCTL_PSP    (1u << 3)   /* pad short packets */
#define TCTL_CT_SHIFT  4        /* collision threshold = 0x10 */
#define TCTL_COLD_SHIFT 12      /* collision distance = 0x40 (full duplex) */

#define RX_DESC 64      /* deeper RX ring: absorb a full receive-window burst
                        * (~45 frames for 64 KiB) before the guest drains it */
#define RX_REFILL 16    /* descriptors returned per RDT write -- see e1000_rx_drain */
#define TX_DESC 8
#define BUF_SIZE 2048

/* TX descriptor command bits */
#define TXD_CMD_EOP  (1u << 0)
#define TXD_CMD_IFCS (1u << 1)
#define TXD_CMD_RS   (1u << 3)
#define TXD_STA_DD   (1u << 0)

/* RX descriptor status bits */
#define RXD_STA_DD   (1u << 0)
#define RXD_STA_EOP  (1u << 1)

struct rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

static volatile uint8_t *mmio;
static net_rx_cb g_rxcb;                    /* RX handler for IRQ mode */

static volatile struct rx_desc *rx_ring;    /* DMA rings: the NIC writes status/length */
static volatile struct tx_desc *tx_ring;
static uint8_t *rx_buf[RX_DESC];
static uint8_t *tx_buf[TX_DESC];
static uint32_t rx_cur, tx_cur;

static inline uint32_t reg_read(uint32_t off)  { return *(volatile uint32_t *)(mmio + off); }
static inline void reg_write(uint32_t off, uint32_t v) { *(volatile uint32_t *)(mmio + off) = v; }

static int rx_init(void)
{
    /* One contiguous frame holds the descriptor ring (RX_DESC*16 = 512 B). */
    uint64_t ring = pmm_alloc();
    if (!ring) return -1;
    rx_ring = (struct rx_desc *)ring;
    memset((void *)rx_ring, 0, RX_DESC * sizeof(struct rx_desc));
    for (int i = 0; i < RX_DESC; i++) {
        rx_buf[i] = (uint8_t *)pmm_alloc();      /* 4 KiB frame, holds a 2 KiB buffer */
        if (!rx_buf[i]) {
            for (int j = 0; j < i; j++) pmm_free((uint64_t)rx_buf[j]);
            pmm_free(ring);
            return -1;
        }
        rx_ring[i].addr = (uint64_t)rx_buf[i];   /* identity-mapped: phys == virt */
        rx_ring[i].status = 0;
    }
    reg_write(REG_RDBAL, (uint32_t)(ring & 0xFFFFFFFF));
    reg_write(REG_RDBAH, (uint32_t)(ring >> 32));
    reg_write(REG_RDLEN, RX_DESC * sizeof(struct rx_desc));
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, RX_DESC - 1);
    rx_cur = 0;
    reg_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_MPE | RCTL_SECRC | RCTL_BSIZE_2048);
    return 0;
}

static int tx_init(void)
{
    uint64_t ring = pmm_alloc();
    if (!ring) return -1;
    tx_ring = (struct tx_desc *)ring;
    memset((void *)tx_ring, 0, TX_DESC * sizeof(struct tx_desc));
    for (int i = 0; i < TX_DESC; i++) {
        tx_buf[i] = (uint8_t *)pmm_alloc();
        if (!tx_buf[i]) {
            for (int j = 0; j < i; j++) pmm_free((uint64_t)tx_buf[j]);
            pmm_free(ring);
            return -1;
        }
        tx_ring[i].addr = (uint64_t)tx_buf[i];
        tx_ring[i].status = TXD_STA_DD;          /* mark free */
    }
    reg_write(REG_TDBAL, (uint32_t)(ring & 0xFFFFFFFF));
    reg_write(REG_TDBAH, (uint32_t)(ring >> 32));
    reg_write(REG_TDLEN, TX_DESC * sizeof(struct tx_desc));
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    tx_cur = 0;
    reg_write(REG_TIPG, 10 | (8 << 10) | (6 << 20));
    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP | (0x10 << TCTL_CT_SHIFT) | (0x40 << TCTL_COLD_SHIFT));
    return 0;
}

/* Contract: callers must hold net_lock (IF=0) -- the TX ring and tx_cur are not
 * otherwise serialized. All current paths (eth/ip/tcp/udp/icmp send) do. */
static int e1000_tx_frame(const void *frame, uint16_t len)
{
    if (!mmio || len > BUF_SIZE) return -1;
    uint32_t i = tx_cur;
    /* Wait for this descriptor to be free (its previous send done). */
    int spins = 0;
    while (!(tx_ring[i].status & TXD_STA_DD)) {
        if (++spins > 1000000) return -1;
    }
    memcpy(tx_buf[i], frame, len);
    tx_ring[i].length = len;
    tx_ring[i].cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    tx_ring[i].status = 0;
    tx_cur = ring_next(i, TX_DESC);
    reg_write(REG_TDT, tx_cur);
    return 0;
}

static int e1000_rx_drain(net_rx_cb cb)
{
    if (!mmio) return 0;
    uint64_t f = net_lock();                     /* exclude the RX IRQ + mainline tcp_recv */
    /* ACK FIRST, THEN DRAIN -- and ack HERE, not only in the ISR.
     *
     * Reading ICR is what DEASSERTS the card's INTx line, and smp.c routes the
     * NIC's I/O APIC entry EDGE-triggered on purpose (a level RTE's remote-IRR
     * is never cleared by QEMU's TCG IOAPIC, which storms). An edge-triggered
     * line that is already asserted produces no further edges: once nobody
     * clears ICR, the NIC interrupt is dead for the rest of the boot.
     *
     * That is exactly what was happening. net_init() unmasks RXT0 before
     * smp_init() routes the line; DHCP arrives in that window and asserts INTx;
     * the routing then goes in behind an already-low line. Measured, with the
     * ISR as the only reader of ICR: `[net] rx path: frames 520 irq 0` -- the
     * card raised ZERO interrupts across a whole boot and a 900 KiB fetch, and
     * every frame came in on the net_poll backstop. The card looked
     * interrupt-driven and was not.
     *
     * Putting the ack in the drain fixes it for good, because the drain is the
     * one thing that always runs: the net_poll backstop reaches it even when no
     * interrupt can. Acking BEFORE consuming descriptors is also what makes a
     * frame that arrives mid-drain deassert-then-reassert and raise a fresh
     * edge, rather than being silently folded into the cause we just cleared. */
    reg_read(REG_ICR);
    int n = 0;
    /* Bounded drain: the buffers are handed back to the NIC as we go, so under a
     * sustained RX flood the NIC re-posts DD as fast as we clear it -- an
     * unbounded loop here never exits, and when the caller is the NIC IRQ
     * (vector 65, IF=0, BKL held) that hard-freezes the machine. One ring's
     * worth per call is guaranteed progress; the rest is picked up by the next
     * RXT0 IRQ or the net_poll backstop. */
    int budget = RX_DESC;
    uint32_t tail = 0;
    int owed = 0;                                /* descriptors consumed since the last RDT */
    while (budget-- > 0 && (rx_ring[rx_cur].status & RXD_STA_DD)) {
        uint16_t len = rx_ring[rx_cur].length;
        /* length/errors/EOP come from the NIC: only hand the stack frames that
         * fit our 2 KiB buffer and completed in a single descriptor (no jumbo). */
        if (len > 0 && len <= BUF_SIZE && !rx_ring[rx_cur].errors &&
            (rx_ring[rx_cur].status & RXD_STA_EOP))
            cb(rx_buf[rx_cur], len);
        rx_ring[rx_cur].status = 0;
        tail = rx_cur;
        rx_cur = ring_next(rx_cur, RX_DESC);
        n++;
        /* The receive tail is a DOORBELL, not a per-descriptor obligation: RDT
         * says "everything up to here is yours again", so one write hands back
         * a whole batch. It used to be written once per frame, and under
         * emulation that is the single most expensive thing in the drain --
         * every store traps into QEMU's e1000 model, which re-runs its RX-queue
         * flush. Batching by RX_REFILL keeps the NIC supplied (a quarter of the
         * ring is always in flight) at a sixteenth of the register traffic. */
        if (++owed >= RX_REFILL) { reg_write(REG_RDT, tail); owed = 0; }
    }
    if (owed) reg_write(REG_RDT, tail);
    net_unlock(f);
    return n;
}

/* IRQ-driven RX: register the receive handler and unmask the NIC's RX causes.
 * Polling (net_poll) stays as a backstop, so this only adds lower latency. */
static void e1000_irq_on(net_rx_cb cb)
{
    if (!mmio) return;
    g_rxcb = cb;
    reg_read(REG_ICR);                           /* clear stale causes */
    /* Unmask ONLY RXT0 (one IRQ per received packet). NOT RXDMT0 (RX-ring-low):
     * with RDLEN's default min-threshold the ring sits below it after any RX
     * burst, so RXDMT0 re-asserts the instant e1000_irq() reads ICR -> ~2M IRQ/s
     * at the NIC vector, ~88% CPU forever, defeating every hlt. RXT0 self-clears
     * on the ICR read and only re-fires on the next real packet, so it never
     * storms when idle. (NOT RXO either, for the same re-assert reason.) */
    reg_write(REG_IMS, ICR_RXT0);                /* unmask receive interrupts */
}

/* Called from the NIC IRQ handler. Ack the device and hand the drain to
 * SOFTIRQ_NET -- see the receive-path comment in c/net/core/net.c for why the
 * protocol work no longer happens here, and for the one case (a NIC interrupt
 * nested inside a kernel sti window) where net_rx_schedule drains inline. */
static void e1000_isr(void)
{
    if (!mmio) return;
    reg_read(REG_ICR);                           /* read-to-clear the causes */
    if (g_rxcb) net_rx_schedule();
}

static struct netdev e1000_dev = {
    .name = "e1000", .irq_line = -1,
    .tx = e1000_tx_frame, .rx_poll = e1000_rx_drain,
    .irq_enable = e1000_irq_on, .irq = e1000_isr,
};

int e1000_probe(struct device *dev)
{
    if (mmio) return -1;                          /* one NIC bound at a time */
    dev_enable(dev, 1);                           /* memory decode + bus master (DMA) */
    /* BAR0 is the register window. dev_bar_map maps exactly the size the BAR
     * decodes, identity + uncached -- the old code mapped a hardcoded 128 KiB,
     * which is right for a 82540EM and a guess anywhere else. */
    uint64_t base = dev_bar_map(dev, 0);
    if (!base) { kprintf("[e1000] no MMIO BAR\n"); return -1; }
    mmio = (volatile uint8_t *)(uintptr_t)base;
    e1000_dev.irq_line = dev->irq_line;           /* PCI IRQ line -> GSI for the I/O APIC */

    /* Reset, then bring the link up. CTRL.RST self-clears when reset completes;
     * poll for it instead of a blind delay. */
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_RST);
    for (int i = 0; i < 1000; i++) {
        if (!(reg_read(REG_CTRL) & CTRL_RST)) break;
        for (volatile int d = 0; d < 10000; d++) ;
    }
    reg_write(REG_CTRL, (reg_read(REG_CTRL) | CTRL_SLU | CTRL_ASDE));
    reg_write(REG_IMC, 0xFFFFFFFF);              /* mask all NIC interrupts for now */
    reg_read(REG_ICR);                           /* clear pending causes */

    uint32_t ral = reg_read(REG_RAL0), rah = reg_read(REG_RAH0);
    e1000_dev.mac[0] = ral & 0xFF;        e1000_dev.mac[1] = (ral >> 8) & 0xFF;
    e1000_dev.mac[2] = (ral >> 16) & 0xFF; e1000_dev.mac[3] = (ral >> 24) & 0xFF;
    e1000_dev.mac[4] = rah & 0xFF;        e1000_dev.mac[5] = (rah >> 8) & 0xFF;

    /* Program receive-address filter 0 with our MAC and the Address-Valid bit
     * (RAH bit 31). Without AV the NIC drops unicast frames (only BAM broadcasts
     * pass), so ARP/ICMP replies addressed to us would never be received. */
    reg_write(REG_RAL0, (uint32_t)e1000_dev.mac[0] | ((uint32_t)e1000_dev.mac[1] << 8) |
              ((uint32_t)e1000_dev.mac[2] << 16) | ((uint32_t)e1000_dev.mac[3] << 24));
    reg_write(REG_RAH0, (uint32_t)e1000_dev.mac[4] | ((uint32_t)e1000_dev.mac[5] << 8) | (1u << 31));

    if (rx_init() != 0 || tx_init() != 0) {
        kprintf("[e1000] descriptor ring allocation failed\n");
        mmio = NULL;
        return -1;
    }

    /* QEMU only re-offers a packet that arrived while RX was disabled when the
     * guest pokes the NIC; re-write RDT so any queued frame is flushed to us. */
    reg_write(REG_RDT, RX_DESC - 1);

    kprintf("[e1000] up: mmio=%p\n", (void *)(uintptr_t)base);
    dev_set_drvdata(dev, &e1000_dev);
    return 0;
}
