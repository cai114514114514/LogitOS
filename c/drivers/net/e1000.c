#include <stdint.h>
#include <stddef.h>
#include "e1000.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "net.h"
#include "kprintf.h"

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
#define RCTL_BAM    (1u << 15)  /* broadcast accept */
#define RCTL_SECRC  (1u << 26)  /* strip ethernet CRC */
#define RCTL_BSIZE_2048 0       /* (BSEX=0, SZ=00) */

#define TCTL_EN     (1u << 1)
#define TCTL_PSP    (1u << 3)   /* pad short packets */
#define TCTL_CT_SHIFT  4        /* collision threshold = 0x10 */
#define TCTL_COLD_SHIFT 12      /* collision distance = 0x40 (full duplex) */

#define RX_DESC 64      /* deeper RX ring: absorb a full receive-window burst
                        * (~45 frames for 64 KiB) before the guest drains it */
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
static uint8_t mac[6];
static int g_irq = -1;                                  /* PCI IRQ line (GSI) */
static void (*g_rxcb)(const uint8_t *frame, uint16_t len);   /* RX handler for IRQ mode */

static volatile struct rx_desc *rx_ring;    /* DMA rings: the NIC writes status/length */
static volatile struct tx_desc *tx_ring;
static uint8_t *rx_buf[RX_DESC];
static uint8_t *tx_buf[TX_DESC];
static int rx_cur, tx_cur;

static inline uint32_t reg_read(uint32_t off)  { return *(volatile uint32_t *)(mmio + off); }
static inline void reg_write(uint32_t off, uint32_t v) { *(volatile uint32_t *)(mmio + off) = v; }

static void delay(void) { for (volatile int i = 0; i < 1000000; i++) ; }

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
    reg_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);
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

int e1000_init(void)
{
    struct pci_dev dev;
    if (pci_find(E1000_VENDOR, E1000_DEVICE, &dev) != 0) {
        kprintf("[net] no e1000 NIC found\n");
        return -1;
    }
    /* Map the NIC's MMIO BAR (128 KiB) uncached, identity. */
    vmm_map_range(dev.bar0, dev.bar0, 0x20000, VMM_WRITABLE | VMM_NOCACHE);
    mmio = (volatile uint8_t *)(uint64_t)dev.bar0;
    g_irq = dev.irq_line;                        /* PCI IRQ line -> GSI for the I/O APIC */

    /* Reset, then bring the link up. CTRL.RST self-clears when reset completes;
     * poll for it instead of a blind delay. */
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_RST);
    for (int i = 0; i < 1000; i++) {
        if (!(reg_read(REG_CTRL) & CTRL_RST)) break;
        for (volatile int d = 0; d < 10000; d++) ;
    }
    reg_write(REG_CTRL, (reg_read(REG_CTRL) | CTRL_SLU | CTRL_ASDE));
    reg_write(REG_IMC, 0xFFFFFFFF);              /* mask all NIC interrupts (we poll) */
    reg_read(REG_ICR);                           /* clear pending causes */

    uint32_t ral = reg_read(REG_RAL0), rah = reg_read(REG_RAH0);
    mac[0] = ral & 0xFF; mac[1] = (ral >> 8) & 0xFF;
    mac[2] = (ral >> 16) & 0xFF; mac[3] = (ral >> 24) & 0xFF;
    mac[4] = rah & 0xFF; mac[5] = (rah >> 8) & 0xFF;

    /* Program receive-address filter 0 with our MAC and the Address-Valid bit
     * (RAH bit 31). Without AV the NIC drops unicast frames (only BAM broadcasts
     * pass), so ARP/ICMP replies addressed to us would never be received. */
    reg_write(REG_RAL0, (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) |
              ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24));
    reg_write(REG_RAH0, (uint32_t)mac[4] | ((uint32_t)mac[5] << 8) | (1u << 31));

    if (rx_init() != 0 || tx_init() != 0) {
        kprintf("[net] e1000: descriptor ring allocation failed\n");
        return -1;
    }

    /* QEMU only re-offers a packet that arrived while RX was disabled when the
     * guest pokes the NIC; re-write RDT so any queued frame is flushed to us. */
    reg_write(REG_RDT, RX_DESC - 1);

    kprintf("[net] e1000 up: MAC %x:%x:%x:%x:%x:%x  mmio=%p\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (void *)(uint64_t)dev.bar0);
    return 0;
}

const uint8_t *e1000_mac(void) { return mac; }

/* Contract: callers must hold net_lock (IF=0) -- the TX ring and tx_cur are not
 * otherwise serialized. All current paths (eth/ip/tcp/udp/icmp send) do. */
int e1000_tx(const void *frame, uint16_t len)
{
    if (!mmio || len > BUF_SIZE) return -1;
    int i = tx_cur;
    /* Wait for this descriptor to be free (its previous send done). */
    int spins = 0;
    while (!(tx_ring[i].status & TXD_STA_DD)) {
        if (++spins > 1000000) return -1;
    }
    memcpy(tx_buf[i], frame, len);
    tx_ring[i].length = len;
    tx_ring[i].cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    tx_ring[i].status = 0;
    tx_cur = (i + 1) % TX_DESC;
    reg_write(REG_TDT, tx_cur);
    return 0;
}

int e1000_rx_poll(void (*cb)(const uint8_t *frame, uint16_t len))
{
    if (!mmio) return 0;
    uint64_t f = net_lock();                     /* exclude the RX IRQ + mainline tcp_recv */
    int n = 0;
    /* Bounded drain: each iteration hands the buffer straight back to the NIC
     * (RDT write), so under a sustained RX flood the NIC re-posts DD as fast as
     * we clear it -- an unbounded loop here never exits, and when the caller is
     * the NIC IRQ (vector 65, IF=0, BKL held) that hard-freezes the machine.
     * One ring's worth per call is guaranteed progress; the rest is picked up
     * by the next RXT0 IRQ or the net_poll backstop. */
    int budget = RX_DESC;
    while (budget-- > 0 && (rx_ring[rx_cur].status & RXD_STA_DD)) {
        uint16_t len = rx_ring[rx_cur].length;
        /* length/errors/EOP come from the NIC: only hand the stack frames that
         * fit our 2 KiB buffer and completed in a single descriptor (no jumbo). */
        if (len > 0 && len <= BUF_SIZE && !rx_ring[rx_cur].errors &&
            (rx_ring[rx_cur].status & RXD_STA_EOP))
            cb(rx_buf[rx_cur], len);
        rx_ring[rx_cur].status = 0;
        reg_write(REG_RDT, rx_cur);              /* hand the buffer back to the NIC */
        rx_cur = (rx_cur + 1) % RX_DESC;
        n++;
    }
    net_unlock(f);
    return n;
}

/* IRQ-driven RX: register the receive handler and unmask the NIC's RX causes.
 * Polling (net_poll) stays as a backstop, so this only adds lower latency. */
void e1000_irq_enable(void (*cb)(const uint8_t *frame, uint16_t len))
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

int e1000_irq_line(void) { return g_irq; }

/* Called from the NIC IRQ handler: ack causes + drain RX into the stack. */
void e1000_irq(void)
{
    if (!mmio) return;
    reg_read(REG_ICR);                           /* read-to-clear the causes */
    if (g_rxcb) e1000_rx_poll(g_rxcb);
}
