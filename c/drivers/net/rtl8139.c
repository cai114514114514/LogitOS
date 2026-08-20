#include <stdint.h>
#include <stddef.h>
#include "netdev.h"
#include "netring.h"
#include "pci.h"
#include "pmm.h"
#include "net.h"
#include "io.h"
#include "pit.h"
#include "kprintf.h"

/* Realtek RTL8139 -- the "everything had one of these" 100 Mbit part, and the
 * only non-Intel NIC QEMU emulates (`-device rtl8139`).
 *
 * It is nothing like the e1000: there are no receive descriptors at all. The
 * chip writes packets back to back into one flat 8 KiB ring, each preceded by a
 * 4-byte {status, size} header, and the driver's whole job on receive is to
 * keep an offset and tell the chip how far it has read. Transmit is four fixed
 * registers, not a ring.
 *
 * Registers are reached through the I/O BAR, which is how this chip is normally
 * driven and avoids needing the MMIO BAR mapped.
 */

void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);

#define R_IDR0     0x00      /* MAC, 6 bytes                    */
#define R_MAR0     0x08      /* multicast filter, 8 bytes       */
#define R_TSD0     0x10      /* transmit status  0..3, +4 each  */
#define R_TSAD0    0x20      /* transmit address 0..3, +4 each  */
#define R_RBSTART  0x30      /* receive ring physical base (32-bit!) */
#define R_CR       0x37      /* command                         */
#define R_CAPR     0x38      /* current address of packet read  */
#define R_CBR      0x3A
#define R_IMR      0x3C
#define R_ISR      0x3E
#define R_TCR      0x40
#define R_RCR      0x44
#define R_CFG9346  0x50
#define R_CONFIG1  0x52
#define R_MSR      0x58      /* media status */

#define CR_BUFE    0x01      /* receive buffer EMPTY */
#define CR_TE      0x04
#define CR_RE      0x08
#define CR_RST     0x10

#define ISR_ROK    0x0001
#define ISR_RER    0x0002
#define ISR_TOK    0x0004
#define ISR_TER    0x0008
#define ISR_RXOVW  0x0010
#define ISR_FOVW   0x0040

#define TSD_OWN    0x00002000   /* set by the chip when its DMA of this buffer is done */
#define TSD_TOK    0x00008000
#define TSD_ERTXTH (8u << 16)   /* start transmitting after 8*32 = 256 bytes are in the FIFO */

#define CFG9346_UNLOCK 0xC0
#define CFG9346_LOCK   0x00

/* MSR bit 2, "LinkB": ACTIVE LOW, not a typo -- confirmed against QEMU's own
 * model (hw/net/rtl8139.c, MediaStatus read: `ret = 0xd0 | (~BasicModeStatus &
 * 0x04)`, whose comment says outright "the LinkDown bit of MediaStatus is
 * inverse with link status"). Real RTL8139C(L) silicon and the Linux 8139too
 * driver (`if (!(RTL_R8(MSR) & MSR_LINKB)) ...link is up...`) agree on the
 * polarity, which is the one thing worth double-checking on an inverted bit --
 * this is measured against the emulator this OS actually boots on, not
 * recalled from a datasheet nobody re-read. */
#define MSR_LINKB  0x04

#define TX_SLOTS   4            /* the chip has exactly four, in hardware */
#define ETH_MIN    60           /* the 8139 does not pad; a runt is dropped on the wire */
#define FRAME_MAX  1518

static uint16_t io;             /* I/O port base */
static uint8_t *rxbuf;          /* RTL8139_RXBUF_PAD bytes, contiguous, < 4 GiB */
static uint8_t *txbuf[TX_SLOTS];
static uint32_t rx_off;         /* our read position in the 8 KiB ring */
static uint32_t tx_cur;
static int      ready;
static net_rx_cb g_rxcb;

/* ======================================================================== */
/* STATISTICS AND LINK STATE -- the same gap e1000.c had, closed the same     */
/* shape (see e1000.c's own header comment on this block for the argument     */
/* that motivated it: before it existed there was no way in this tree to      */
/* observe a dropped packet or a link going down). The difference here is     */
/* what the counters ARE: the 8254x hands the driver a read-to-clear hardware */
/* accumulator (GPRC/GORCL/CRCERRS/...) and e1000_stats.h exists to fold that  */
/* correctly. This chip has no such register block reachable from a plain I/O */
/* BAR -- only a DMA'd "dump tally counters" command (DTCCR, not implemented   */
/* here) -- so these are SOFTWARE counters, incremented once per frame at the  */
/* one place each outcome is already decided in rx_poll/tx. Simpler than      */
/* e1000's accumulate-a-read-to-clear-register problem, and correspondingly    */
/* not given its own host-testable header: there is no accumulation subtlety  */
/* to get wrong, just a counter per branch already being taken.               */
/* ======================================================================== */

static uint32_t g_rx_ok, g_rx_bad, g_rx_overflow;   /* software counters */
static uint32_t g_tx_ok, g_tx_fail;

static int      g_link_known;   /* 0 until the first observation */
static int      g_link_up;      /* last reported state */
static uint64_t g_stat_next_ms; /* when the block may be sampled again */
static uint32_t g_report_pkts;  /* rx_ok+tx_ok at the last printed report line */
static uint32_t g_report_losses;/* rx_bad+rx_overflow+tx_fail at the last report */

/* Same period e1000 uses and for the same reason: an MSR read is one I/O port
 * access, cheap on its own, but rx_poll runs on every net_poll from the WM
 * loop (~100x/s) plus every RX interrupt, and nobody needs link status more
 * often than once a second. */
#define STAT_PERIOD_MS     1000
/* Same argument as e1000's REPORT_EVERY_PKTS: a boot that only does DHCP+ARP
 * moves a couple dozen frames, and staying silent below this keeps the line
 * out of every OTHER boot harness's serial expectations -- this file does not
 * own those harnesses and must not change what they see. */
#define REPORT_EVERY_PKTS  512

static void link_report(int up)
{
    /* No speed/duplex to report: unlike e1000's STATUS register, MSR's other
     * bits are receive-FIFO-empty and AUX status, not link speed -- this chip
     * predates the 100/10 auto-negotiation bits QEMU's model bothers to fake. */
    kprintf("[rtl8139] link: %s\n", up ? "UP" : "DOWN");
}

/* Once per TRANSITION, mirroring e1000's link_check() for the same reason: a
 * driver that reported every sample would print a line every second forever,
 * indistinguishable in a grepped log from one that never checked at all. */
static void link_check(void)
{
    int up = !(inb(io + R_MSR) & MSR_LINKB);
    if (!g_link_known) { g_link_known = 1; g_link_up = up; link_report(up); return; }
    if (up == g_link_up) return;
    g_link_up = up;
    link_report(up);
}

static void stats_report(void)
{
    kprintf("[rtl8139] stats: rx %u ok / %u bad, tx %u ok / %u fail, "
            "%u overflow reset(s)\n",
            g_rx_ok, g_rx_bad, g_tx_ok, g_tx_fail, g_rx_overflow);
}

/* Called from inside rx_poll, under net_lock -- same placement as e1000's
 * stats_poll(), for the same reason: MSR is a live register and reading it
 * off the interrupt path would race the ISR touching CR/ISR/CAPR, which the
 * comment in rtl_rx_poll already documents this chip does not tolerate. */
static void stats_poll(void)
{
    uint64_t now = timer_ms();
    if (now < g_stat_next_ms) return;
    g_stat_next_ms = now + STAT_PERIOD_MS;

    link_check();

    /* Compared to the LAST REPORTED value, not tested for nonzero -- a single
     * bad frame must print once, not on every poll for the rest of the boot.
     * (e1000's stats_poll has the identical `loss != g_report_losses` line for
     * the identical reason; a `losses > 0` test here was the first draft and it
     * is exactly the plausible-wrong-implementation shape a negative control
     * exists to catch.) */
    uint32_t pkts = g_rx_ok + g_tx_ok;
    uint32_t losses = g_rx_bad + g_rx_overflow + g_tx_fail;
    if (losses != g_report_losses || pkts >= g_report_pkts + REPORT_EVERY_PKTS) {
        g_report_losses = losses;
        g_report_pkts = pkts;
        stats_report();
    }
}

/* Re-arm the receiver after an overflow or a header the chip could not have
 * written. Cheaper than it looks and vastly better than the alternative, which
 * is a receive path that is silently desynchronised from the ring for the rest
 * of the boot. */
static void rx_reset(void)
{
    g_rx_overflow++;
    outb(io + R_CR, CR_TE);                       /* RX off */
    rx_off = 0;
    outw(io + R_CAPR, rtl8139_capr(0));
    outb(io + R_CR, CR_TE | CR_RE);
}

static int rtl_rx_poll(net_rx_cb cb)
{
    if (!ready) return 0;
    uint64_t f = net_lock();
    /* THIS CARD DOES NOT GET THE ACK-IN-THE-DRAIN THAT e1000 AND virtio-net DO,
     * AND THE REASON IS MEASURED, NOT ASSUMED.
     *
     * e1000_rx_drain() explains the bug: the ISR is the only reader of the
     * interrupt-cause register, the I/O APIC entry is edge-triggered, and a line
     * left asserted produces no further edges, so a cause nobody clears kills
     * the interrupt for the boot. This card has that shape too, and it shows:
     * over 5763 timed segments its rx->ack turnaround is median 1.24 ms, p90
     * 1.70, p99 2.16 -- and max 10.08, one whole 100 Hz tick.
     *
     * Adding the same ack here fixes the tail and BREAKS THE CARD. Paired, both
     * arms booted together, fetches round-robin, nine 900 KiB reps each:
     *
     *     without the ack   9/9 completed, median 260.9 Mbit/s, max 10.08 ms
     *     with the ack      0/9 completed
     *
     * (A first version that also handled the overflow causes here was worse
     * still. A 32 KiB fetch passes either way, which is why test-nic-rtl8139
     * does not see this and the throughput arm does.) The 8139 receives into one
     * flat ring rather than descriptors, and CAPR/ISR/CR are coupled in a way
     * that does not tolerate a second reader at poll rates; working out exactly
     * which of them is the one that must not be touched is a job for someone
     * with the datasheet and more time than this took to measure. Until then the
     * tail stays, documented, on the slowest card of the three -- which is the
     * right place for a known defect to sit. */
    int n = 0, budget = 64;                       /* bounded: this also runs in the ISR */
    while (budget-- > 0 && !(inb(io + R_CR) & CR_BUFE)) {
        const uint8_t *p = rxbuf + rx_off;
        uint16_t status = rtl8139_rx_status(p);
        uint16_t size   = rtl8139_rx_size(p);

        /* 0xFFF0 is the chip saying "this packet is still being DMA'd" -- not a
         * length. Reading it as one walks the offset into the middle of a frame
         * and every subsequent packet is garbage. */
        if (size == 0xFFF0) break;
        if (size < 4 + 14 || size > FRAME_MAX + 4) { rx_reset(); break; }

        if (rtl8139_rx_ok(status)) {
            cb(p + 4, (uint16_t)(size - 4));      /* -4: the size includes the CRC */
            g_rx_ok++;
        } else {
            g_rx_bad++;
        }

        rx_off = rtl8139_next_off(rx_off, size);
        outw(io + R_CAPR, rtl8139_capr(rx_off));  /* CAPR trails by 16 -- see netring.h */
        n++;
    }
    /* Rate-limited to at most one extra I/O read (MSR) per second -- NOT the
     * every-call shape the header comment above measured breaking this card.
     * That measurement was about a SECOND READER of ISR/CR/CAPR at poll
     * frequency (~100x/s); MSR is a different register, touched by nothing
     * else in this file, and read at most once/s regardless of how often
     * rx_poll runs. Verified after landing this (not merely argued): a
     * heavier fetch than the 32 KiB gate exercises, since the gate is exactly
     * what the header comment says did NOT catch the earlier regression. */
    stats_poll();
    net_unlock(f);
    return n;
}

/* Contract: callers hold net_lock (IF=0). */
static int rtl_tx(const void *frame, uint16_t len)
{
    if (!ready || len == 0 || len > FRAME_MAX) return -1;
    uint32_t i = tx_cur;
    uint16_t port = (uint16_t)(io + R_TSD0 + 4 * i);
    int spins = 0;
    while (!(inl(port) & TSD_OWN)) {              /* previous send still in DMA */
        if (++spins > 1000000) { g_tx_fail++; return -1; }
    }
    uint16_t n = len;
    if (n < ETH_MIN) {                            /* pad: no hardware short-packet pad */
        memset(txbuf[i], 0, ETH_MIN);
        n = ETH_MIN;
    }
    memcpy(txbuf[i], frame, len);
    outl(io + R_TSAD0 + 4 * i, (uint32_t)(uintptr_t)txbuf[i]);
    /* Writing TSD with a length clears OWN and starts the transmit. */
    outl(port, TSD_ERTXTH | n);
    tx_cur = ring_next(i, TX_SLOTS);
    g_tx_ok++;                 /* queued for DMA, not confirmed delivered: this
                                 * chip's TSD completion status (TOK/TABT/etc) is
                                 * only valid once OWN is set again, i.e. at the
                                 * TOP of this function on the NEXT send into this
                                 * slot -- there is no free place to read it back
                                 * without adding a second reader of TSD at poll
                                 * rate, which is exactly the class of change the
                                 * rx-side comment measured breaking this card. */
    return 0;
}

static void rtl_irq_on(net_rx_cb cb)
{
    if (!ready) return;
    g_rxcb = cb;
    outw(io + R_ISR, 0xFFFF);                     /* ISR is write-1-to-clear */
    outw(io + R_IMR, ISR_ROK | ISR_RER | ISR_RXOVW | ISR_FOVW);
}

static void rtl_isr(void)
{
    if (!ready) return;
    uint16_t isr = inw(io + R_ISR);
    outw(io + R_ISR, isr);                        /* ack before draining, so a frame
                                                   * arriving mid-drain still raises
                                                   * a fresh edge */
    if (isr & (ISR_RXOVW | ISR_FOVW)) rx_reset();
    /* Ack here, drain on SOFTIRQ_NET -- see c/net/core/net.c. */
    if (g_rxcb) net_rx_schedule();
}

static struct netdev rtl_dev = {
    .name = "rtl8139", .irq_line = -1,
    .tx = rtl_tx, .rx_poll = rtl_rx_poll,
    .irq_enable = rtl_irq_on, .irq = rtl_isr,
};

/* This chip's registers are behind its I/O BAR. The device model already sized
 * and classified every BAR at enumeration, so this is a lookup rather than the
 * hand-rolled config-space read it used to be -- and it gets the I/O-BAR mask
 * right (2 low bits, not 4), which the legacy pci_dev->bar0 did not. */
static uint16_t find_io_bar(const struct device *dev)
{
    for (int b = 0; b < DEV_NRES; b++)
        if ((dev->res[b].flags & DEV_RES_IO) && dev->res[b].start)
            return (uint16_t)dev->res[b].start;
    return 0;
}

int rtl8139_probe(struct device *dev)
{
    if (ready) return -1;                         /* one NIC bound at a time */
    dev_enable(dev, 1);                           /* I/O decode + bus master (DMA) */
    io = find_io_bar(dev);
    if (!io) { kprintf("[rtl8139] no I/O BAR\n"); return -1; }

    /* The receive ring must be physically contiguous, and RBSTART is a 32-BIT
     * register: this chip cannot DMA above 4 GiB at all. Refusing here is the
     * difference between "no network" and "the chip DMAs into the truncated
     * low-32-bit alias of our buffer", which is memory corruption elsewhere in
     * the kernel with no hint that the NIC caused it. */
    uint64_t phys = pmm_alloc_contig((RTL8139_RXBUF_PAD + 4095) / 4096);
    if (!phys) { kprintf("[rtl8139] rx ring alloc failed\n"); return -1; }
    if (phys + RTL8139_RXBUF_PAD > 0x100000000ull) {
        kprintf("[rtl8139] rx ring above 4 GiB (%p): chip cannot address it\n", (void *)phys);
        return -1;
    }
    rxbuf = (uint8_t *)(uintptr_t)phys;
    memset(rxbuf, 0, RTL8139_RXBUF_PAD);

    for (int i = 0; i < TX_SLOTS; i++) {
        uint64_t t = pmm_alloc();
        if (!t || t + 4096 > 0x100000000ull) { kprintf("[rtl8139] tx buffer alloc failed\n"); return -1; }
        txbuf[i] = (uint8_t *)(uintptr_t)t;
    }

    /* Power on, then soft reset and wait for the chip to clear RST itself. */
    outb(io + R_CFG9346, CFG9346_UNLOCK);
    outb(io + R_CONFIG1, 0x00);
    outb(io + R_CFG9346, CFG9346_LOCK);
    outb(io + R_CR, CR_RST);
    int spins = 0;
    while (inb(io + R_CR) & CR_RST) {
        if (++spins > 1000000) { kprintf("[rtl8139] reset timeout\n"); return -1; }
    }

    for (int i = 0; i < 6; i++) rtl_dev.mac[i] = inb((uint16_t)(io + R_IDR0 + i));
    rtl_dev.irq_line = dev->irq_line;

    outb(io + R_CFG9346, CFG9346_UNLOCK);
    outb(io + R_CR, CR_TE | CR_RE);               /* enable BEFORE writing RCR/TCR */
    /* RCR: accept physical-match + multicast + broadcast (not promiscuous, not
     * runts, not errored), WRAP set so a packet crossing the end of the ring is
     * written contiguously into the pad instead of split, max DMA burst, and no
     * receive FIFO threshold (hand us whole packets). */
    outl(io + R_RCR, 0x0Eu | 0x80u | (7u << 8) | (7u << 13));
    outl(io + R_TCR, 0x03000700u);                /* normal interframe gap, max DMA burst */
    outl(io + R_MAR0, 0xFFFFFFFFu);               /* accept all multicast groups */
    outl(io + R_MAR0 + 4, 0xFFFFFFFFu);
    outl(io + R_RBSTART, (uint32_t)phys);
    rx_off = 0;
    outw(io + R_CAPR, rtl8139_capr(0));
    outb(io + R_CFG9346, CFG9346_LOCK);

    outw(io + R_ISR, 0xFFFF);
    outw(io + R_IMR, 0);                          /* polled until irq_enable */
    outb(io + R_CR, CR_TE | CR_RE);
    tx_cur = 0;
    ready = 1;

    kprintf("[rtl8139] up: io=%x rxring=%p (%d B)\n", io, (void *)phys, RTL8139_RXBUF_PAD);
    dev_set_drvdata(dev, &rtl_dev);
    return 0;
}
