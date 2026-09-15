#include <stdint.h>
#include <stddef.h>
#include "netdev.h"
#include "netring.h"
#include "e1000_stats.h"
#include "pci.h"
#include "dma.h"
#include "vmm.h"
#include "net.h"
#include "pit.h"
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
#define ICR_LSC     0x04        /* link status change */
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
#define REG_ITR     E1000_REG_ITR       /* interrupt throttle; see e1000_stats.h */

/* 0 = no moderation, i.e. exactly the behaviour that shipped before this line.
 * Overridable from the build so the A/B is a rebuild and not an edit:
 *   make test-net-bench E1000_ITR=61      (tests/nic.mk turns it into -D)
 * The default is set from the measurement recorded at the write site below. */
#ifndef E1000_ITR_VALUE
#define E1000_ITR_VALUE 0
#endif

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

/* RING DEPTH, OVERRIDABLE ONLY AS AN INSTRUMENT (make ... E1000_RXDESC=8).
 *
 * The default is the shipped value and nothing below changes without the knob.
 * It exists because MPC -- the one drop counter QEMU's e1000 lets a guest read
 * -- is incremented in exactly one place in the model (e1000_receiver_overrun),
 * which fires only when a frame arrives and the ring has no room. On the
 * shipped 64-descriptor ring a 16 MiB SLIRP download produces ZERO overruns, so
 * the register stays 0 and a probe of it measures nothing. Shrinking the ring
 * is how the counter is made non-zero ON PURPOSE, so that its READ semantics
 * (sticky vs read-to-clear) can be observed at all. It is a knob for the probe
 * build, not a tuning parameter, and the default must never move. */
#ifndef RX_DESC
#define RX_DESC 64      /* deeper RX ring: absorb a full receive-window burst
                        * (~45 frames for 64 KiB) before the guest drains it */
#endif
#ifndef RX_REFILL
#define RX_REFILL 16    /* descriptors returned per RDT write -- see e1000_rx_drain */
#endif
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
static struct dma_buffer *rx_dma, *tx_dma;
static volatile struct rx_desc *rx_ring;    /* DMA rings: the NIC writes status/length */
static volatile struct tx_desc *tx_ring;
static uint8_t *rx_buf[RX_DESC];
static uint8_t *tx_buf[TX_DESC];
static uint32_t rx_cur, tx_cur;

static inline uint32_t reg_read(uint32_t off)  { return *(volatile uint32_t *)(mmio + off); }
static inline void reg_write(uint32_t off, uint32_t v) { *(volatile uint32_t *)(mmio + off) = v; }

/* ======================================================================== */
/* STATISTICS AND LINK STATE                                                */
/*                                                                          */
/* Before this block there was NO WAY, anywhere in this tree, to observe a   */
/* dropped packet or a link going down. CLAUDE.md quotes 269.9 Mbit/s as a   */
/* measured throughput with no denominator beside it; RNBC and MPC are the   */
/* two numbers that say whether that run was clean or was silently losing    */
/* frames, and every network measurement in this repo was uncalibrated       */
/* without them. The arithmetic lives in e1000_stats.h so a host test can    */
/* drive it (make test-e1000-stats); what is here is the device half.        */
/*                                                                          */
/* THE READ-TO-CLEAR TRAP, at the read: every register e1000_stats_sample()  */
/* touches is cleared BY the read. A second reader sees zero and takes the   */
/* count away from the first, so there is exactly one call to it in the      */
/* kernel -- stats_poll() below -- and every other consumer goes through     */
/* stats_get(), which does not touch the device.                             */
/* ======================================================================== */

static uint32_t e1000_rd(void *ctx, uint32_t off) { (void)ctx; return reg_read(off); }

static struct e1000_stats g_stats;      /* 64-bit accumulators; see e1000_stats.h */
static uint64_t g_stat_next_ms;         /* when the block may be sampled again    */
static uint64_t g_report_pkts;          /* rx+tx total at the last printed line   */
static uint64_t g_report_losses;        /* losses at the last printed line        */
static uint64_t g_report_loss_ms;       /* timer_ms() of the last printed line    */
static uint64_t g_sample_losses;        /* losses as of the PREVIOUS sample (every*/
                                         /* call, printed or not) -- see below     */
static uint64_t g_sample_loss_delta;    /* that sample's delta, i.e. the "shape"  */

static uint32_t g_link;                 /* last STATUS we reported                */
static int      g_link_known;           /* 0 until the first observation          */
static volatile uint32_t g_link_evt;    /* set by the ISR on ICR.LSC              */

static uint32_t g_irq;                  /* NIC interrupts taken (ITR's denominator) */
static uint32_t g_irq_reported;

static int g_goct_note_printed;         /* the "*" legend, printed at most once */

/* THE accessor. Deliberately does not sample: sampling is what clears the
 * device, and a getter that cleared would make every caller a thief. Two
 * consumers today, both in this file (the report line and the loss test); a
 * third outside the driver needs a slot on `struct netdev`, which this
 * workflow does not own -- said plainly rather than exported into nothing. */
static const struct e1000_stats *stats_get(void) { return &g_stats; }

/* SAMPLE PERIOD. The drain runs ~100x/s from net_poll, and thirteen MMIO reads
 * per drain is 1,300 traps into QEMU's device model every second on the
 * receive hot path, to answer a question nobody asks more than once a second.
 * The cost of the period is that a report is up to a second stale; the
 * alternative -- sampling only when somebody asks -- is not available, because
 * the registers are 32-bit and clear on read, so nobody asking for a while is
 * how a counter saturates and starts reporting the same number forever. */
#define STAT_PERIOD_MS 1000

/* Print every N packets of traffic, not every N milliseconds. Same argument
 * net.c's rx_report() makes for the same constant: a desktop boot moves a
 * couple of dozen frames (DHCP, ARP), so this stays silent unless something
 * actually moved bytes, which keeps it out of every other harness's serial
 * expectations. A LOSS prints immediately regardless -- one dropped frame on
 * an otherwise idle machine is the interesting event, and waiting for 512
 * packets that may never come is how it would be missed. */
#define REPORT_EVERY_PKTS 512

/* THE FLOOD, AND WHY "loss != g_report_losses" WAS NOT SAFE.
 *
 * The line above says "a loss prints immediately", and on real 8254x silicon
 * that is exactly right: MPC is R/clr, so a report of 32 followed by a report
 * of 0 IS 32 distinct events settling back to quiet. This tree's settle-phase
 * dossier (tests/boot/run-e1000-mpc-probe.sh, both a direct register re-read
 * and QEMU 11.0.0's own hw/net/e1000.c: `[CRCERRS ... MPC] = &mac_readreg`,
 * no clear) found that MPC is NOT read-to-clear on the one machine this tree
 * has ever booted on. A sticky register under e1000_stats.h's `*sw += hw` is
 * re-added to the software loss total once per STAT_PERIOD_MS forever, so
 * `loss != g_report_losses` is true on EVERY sample once a single overrun has
 * ever happened -- including 148 of 148 idle samples measured in the dossier,
 * with rx flat and irq (+0). "Print immediately on any change" degenerates
 * into "print forever", one busy-waited kprintf per second, unboundedly.
 *
 * THE FIX IS SHAPE, NOT SILENCE. What made the original line interesting was
 * never "the total changed" -- a monotone counter's total changes on every
 * sample once it is nonzero, log-worthy or not. What is interesting is a
 * change in RATE: the total starting to move at all (0 -> nonzero), or moving
 * at a different pace than it was a moment ago (a second, distinct burst on
 * top of a sticky reading, or a real drop rate that is climbing or easing).
 * A steady +32/sample forever is exactly the shape a sticky register with no
 * new events produces, and it is also the shape a print-suppression rule
 * should treat as "already said".
 *
 * The per-sample delta is tracked against the PREVIOUS SAMPLE, not the
 * previous PRINTED line -- deliberately. STAT_PERIOD_MS gates stats_poll() to
 * once a second regardless of whether the last call printed, so if the shape
 * comparison used the last-printed baseline, a quiet pulse 30 s later would
 * compare this second's +32 against a baseline 30 samples stale (+960) and
 * misread its own pulse as a new shape, printing every second again from
 * there. Comparing consecutive samples keeps the shape check correct no
 * matter how long the printing side has been suppressing. So: report
 * immediately when this sample's delta differs from the immediately
 * preceding sample's delta (which covers the first-ever loss, since the
 * baseline delta starts at 0); otherwise, print at most once every
 * REPORT_LOSS_QUIET_MS as a "still losing, same rate" pulse, so an
 * unattended serial log never goes silent about an ongoing problem -- it
 * just stops repeating the identical line every second.
 *
 * WHAT THIS NOW MISSES, stated rather than discovered later: two real,
 * distinct overrun bursts of the identical size arriving back to back will
 * be reported as one shape-unchanged event and merged into the periodic
 * pulse -- rule 3 (never stub to success) is not violated because the count
 * itself is never altered, only how often the same count is re-announced.
 * And on real silicon, where MPC genuinely clears, this policy is *stricter*
 * than the original: a train of many small distinct drops of the same size
 * (a real, if unlikely, failure mode) prints once per quiet window instead of
 * once per drop. Both are read from `g_stats`, which is untouched -- nothing
 * here changes what is COUNTED, only what is PRINTED. */
#define REPORT_LOSS_QUIET_MS 30000

/* WHAT GETS PRINTED, DECIDED UNDER THE LOCK AND PRINTED OUTSIDE IT.
 *
 * The split is not tidiness. serial_putc() busy-waits on the UART's
 * transmitter-holding-empty bit, one `inb` per poll per character, and the
 * stats line is ~200 characters; doing that inside net_lock() means a
 * multi-millisecond window with interrupts off on the RECEIVE HOT PATH, once a
 * second, to print a diagnostic. net.c's rx_report() already prints outside the
 * drain for the same reason -- this file is the one that would have introduced
 * the cost that file avoided.
 *
 * The snapshot is on the drain's STACK rather than a module-level "pending"
 * struct, so a second core inside stats_poll() cannot rewrite the numbers
 * between the decision to print and the print. A torn log line is a small harm
 * and it is exactly the kind that gets quoted later as a measurement. */
struct e1000_pending {
    int      link;              /* print a link line */
    uint32_t link_status;
    int      stats;             /* print a stats line */
    int      goct_note;         /* print the "*" legend once -- see stats_poll */
    struct e1000_stats s;       /* the snapshot to print */
    uint32_t irq, dirq;
#ifdef E1000_REG_PROBE
    int      probe;             /* print a [e1000-probe] line -- see below */
    uint32_t p[16];
#endif
};

/* ====================================================================== */
/* TEMPORARY PROBE -- off unless built with -DE1000_REG_PROBE             */
/* (make ... E1000_PROBE=1). THIS IS AN INSTRUMENT, NOT A FEATURE.        */
/*                                                                        */
/* Everything in e1000_stats.h rests on ONE assumption, stated at the top  */
/* of this block: "every register e1000_stats_sample() touches is cleared  */
/* BY the read". That is what the 8254x manual specifies for silicon. It   */
/* has never been measured on the device model this tree actually runs,    */
/* and e1000_stats.h says so out loud about the 64-bit pairs ("WHICH ONE   */
/* QEMU IMPLEMENTS IS DELIBERATELY NOT ASSERTED HERE").                    */
/*                                                                        */
/* So: read each register TWICE IN A ROW with nothing in between. A        */
/* read-to-clear register answers <n> then 0. A sticky one answers <n>     */
/* twice -- and a sticky one under `*sw += hw` is added to the software    */
/* total once per sample period FOREVER, which is a counter that climbs by */
/* a constant with no traffic at all.                                      */
/*                                                                        */
/* THE CONTROL IS THE FIRST LINE OF IT. GPRC is read-to-clear on this part */
/* and is the register whose totals this tree has always believed. If      */
/* gprc b != 0 while gprc a != 0, the probe cannot tell the two behaviours */
/* apart -- it is reading a stale cache line, or the MMIO window is wrong, */
/* or reg_read got optimised -- and NOTHING BELOW IT MEANS ANYTHING. A     */
/* probe whose discriminating power is not visible in its own output is    */
/* the thing CLAUDE.md rule 5 is about.                                    */
/*                                                                        */
/* A PROBE BUILD'S STATS LINE IS NOT A MEASUREMENT. These reads happen     */
/* before e1000_stats_sample() and, for the registers that DO clear, they  */
/* take the count away from it -- exactly the "second reader steals from   */
/* the first" failure the block above forbids. That is why this is a build */
/* knob and why the output carries its own prefix.                         */
/* ====================================================================== */
#ifdef E1000_REG_PROBE
static void e1000_probe_read(struct e1000_pending *p)
{
    /* THE CONTROL. A register the manual and QEMU both make read-to-clear. */
    p->p[0]  = reg_read(E1000_REG_GPRC);     /* gprc a */
    p->p[1]  = reg_read(E1000_REG_GPRC);     /* gprc b -- must be 0 if a != 0 */
    /* The two drop counters, which are the whole question. */
    p->p[2]  = reg_read(E1000_REG_MPC);      /* mpc a  */
    p->p[3]  = reg_read(E1000_REG_MPC);      /* mpc b  */
    p->p[4]  = reg_read(E1000_REG_RNBC);     /* rnbc a */
    p->p[5]  = reg_read(E1000_REG_RNBC);     /* rnbc b */
    /* The 64-bit pairs. low, high, low again: under CLEAR-ON-HIGH the third
     * read is 0 and low-then-high is exact; under CLEAR-ON-LOW the second is
     * 0. e1000_stats.h names both conventions and refuses to guess which. */
    p->p[6]  = reg_read(E1000_REG_TORL);
    p->p[7]  = reg_read(E1000_REG_TORH);
    p->p[8]  = reg_read(E1000_REG_TORL);     /* torl again */
    p->p[9]  = reg_read(E1000_REG_GORCL);
    p->p[10] = reg_read(E1000_REG_GORCH);
    p->p[11] = reg_read(E1000_REG_GORCL);    /* gorcl again */
    p->p[12] = reg_read(E1000_REG_GOTCL);
    p->p[13] = reg_read(E1000_REG_GOTCH);
    /* Two more of the "always zero" family, to say whether they are zero
     * because nothing happened or zero because the model does not have them. */
    p->p[14] = reg_read(E1000_REG_CRCERRS);
    p->p[15] = reg_read(E1000_REG_COLC);
    p->probe = 1;
}
#endif

static void report_flush(const struct e1000_pending *p)
{
    if (p->link) {
        if (e1000_link_is_up(p->link_status))
            kprintf("[e1000] link: UP %u Mb/s %s duplex\n",
                    e1000_link_mbps(p->link_status),
                    e1000_link_is_fd(p->link_status) ? "full" : "half");
        else
            kprintf("[e1000] link: DOWN\n");
    }
    if (p->stats) {
        const struct e1000_stats *s = &p->s;
        /* "0" and "unreadable on this device" print identically as a bare
         * number, and CLAUDE.md rule 5 is exactly about a zero that reads as
         * a fact when it is really a hole in the instrument. goct rx/tx carry
         * a "*" the moment rx_good_bytes_stuck/tx_good_bytes_stuck goes true
         * (see the comment on those fields in e1000_stats.h) so this line
         * stops claiming "0 good bytes" for a register QEMU never lets the
         * guest read. rnbc has no such marker: unlike GORC/GOTC, RNBC and MPC
         * measure genuinely different events that CAN disagree on real
         * silicon, so there is no safe live signal for "this rnbc is not to
         * be trusted" -- see the struct comment above rx_no_buf. */
        kprintf("[e1000] stats: rx %llu pkt / %llu B, tx %llu pkt / %llu B; "
                "drop rnbc %llu mpc %llu; err crc %llu rlec %llu; "
                "col %llu ecol %llu late %llu; goct rx %llu%s tx %llu%s; "
                "irq %u (+%u)\n",
                (unsigned long long)s->rx_pkts,   (unsigned long long)s->rx_bytes,
                (unsigned long long)s->tx_pkts,   (unsigned long long)s->tx_bytes,
                (unsigned long long)s->rx_no_buf, (unsigned long long)s->rx_missed,
                (unsigned long long)s->crc_errs,  (unsigned long long)s->len_errs,
                (unsigned long long)s->colls,     (unsigned long long)s->excess_colls,
                (unsigned long long)s->late_colls,
                (unsigned long long)s->rx_good_bytes, s->rx_good_bytes_stuck ? "*" : "",
                (unsigned long long)s->tx_good_bytes, s->tx_good_bytes_stuck ? "*" : "",
                p->irq, p->dirq);
        if (p->goct_note)
            kprintf("[e1000] goct*: this register cannot be read on this "
                    "device (MAC_ACCESS_FLAG_NEEDED, hw/net/e1000.c QEMU "
                    "11.0.0) -- 0 does NOT mean zero good bytes\n");
    }
#ifdef E1000_REG_PROBE
    if (p->probe) {
        kprintf("[e1000-probe] gprc a=%u b=%u | mpc a=%u b=%u | rnbc a=%u b=%u | "
                "torl=%u torh=%u torl2=%u | gorcl=%u gorch=%u gorcl2=%u | "
                "gotcl=%u gotch=%u | crcerrs=%u colc=%u\n",
                p->p[0], p->p[1], p->p[2], p->p[3], p->p[4], p->p[5],
                p->p[6], p->p[7], p->p[8], p->p[9], p->p[10], p->p[11],
                p->p[12], p->p[13], p->p[14], p->p[15]);
    }
#endif
}

/* Once per TRANSITION, never per poll. e1000_link_changed() compares only
 * LU/FD/SPEED because STATUS also carries GIO_MASTER_ENABLE, TXOFF and the
 * auto-speed-detect value, several of which move on their own -- comparing the
 * whole register prints a link line every time the bus goes idle, which in a
 * log grepped for "link" is indistinguishable from a correct implementation.
 * Pinned host-side: six polls across one unplug/replug print two lines. */
static void link_check(struct e1000_pending *p)
{
    uint32_t status = reg_read(REG_STATUS);
    if (g_link_known && !e1000_link_changed(g_link, status)) return;
    g_link_known = 1;
    g_link = status;
    p->link = 1;
    p->link_status = status;
}

/* Called from inside the drain, under net_lock. Rate-limited; the link half
 * additionally runs immediately when the ISR saw ICR.LSC, so an unplug is
 * reported within a drain (~10 ms) rather than within a sample period. */
static void stats_poll(struct e1000_pending *p)
{
    if (g_link_evt) { g_link_evt = 0; link_check(p); }

    uint64_t now = timer_ms();
    if (now < g_stat_next_ms) return;
    g_stat_next_ms = now + STAT_PERIOD_MS;

#ifdef E1000_REG_PROBE
    e1000_probe_read(p);            /* BEFORE the sample, and it steals from it */
#endif
    e1000_stats_sample(&g_stats, e1000_rd, 0);
    link_check(p);                       /* the backstop: LSC can be masked or lost */

    const struct e1000_stats *s = stats_get();
    uint64_t pkts = s->rx_pkts + s->tx_pkts, loss = e1000_stats_losses(s);
    /* This SAMPLE's delta, against the previous SAMPLE -- see the comment on
     * REPORT_LOSS_QUIET_MS for why this is not measured against the last
     * PRINTED baseline. Both operands are monotone, so this never borrows. */
    uint64_t sample_delta = loss - g_sample_losses;

    /* new_shape covers 0 -> nonzero (g_sample_loss_delta starts at 0, so the
     * first loss is always a shape change) and any later change of PACE,
     * sticky or not. quiet_pulse is the once-per-window reminder for a loss
     * that keeps recurring at an unchanged pace since it was last shown, so a
     * real, ongoing problem never goes fully silent between prints. */
    int new_shape   = loss != g_sample_losses && sample_delta != g_sample_loss_delta;
    int quiet_pulse = loss != g_report_losses &&
                       now - g_report_loss_ms >= REPORT_LOSS_QUIET_MS;

    /* Advance the sample baseline EVERY call, print or not -- this is what
     * makes the shape comparison immune to how long the print side has been
     * suppressing (see the comment above). */
    g_sample_losses = loss;
    g_sample_loss_delta = sample_delta;

    if (new_shape || quiet_pulse || pkts >= g_report_pkts + REPORT_EVERY_PKTS) {
        g_report_losses = loss;
        g_report_pkts = pkts;
        g_report_loss_ms = now;
        p->stats = 1;
        p->s = *s;
        p->irq = g_irq;
        p->dirq = g_irq - g_irq_reported;
        g_irq_reported = g_irq;
        if (!g_goct_note_printed &&
            (s->rx_good_bytes_stuck || s->tx_good_bytes_stuck)) {
            g_goct_note_printed = 1;
            p->goct_note = 1;
        }
    }
}

static int rx_init(void)
{
    /* One contiguous frame holds the descriptor ring (RX_DESC*16 = 512 B). */
    rx_dma = dma_alloc_coherent(&nic_dma, 4096, 4096, 0);
    if (!rx_dma) return -1;
    uint64_t ring = dma_addr_value(rx_dma->dma);
    rx_ring = rx_dma->cpu;
    for (int i = 0; i < RX_DESC; i++) {
        struct dma_buffer *b = dma_alloc_coherent(&nic_dma, 4096, 4096, 0);
        if (!b) return -1;
        rx_buf[i] = b->cpu;
        rx_ring[i].addr = dma_addr_value(b->dma);
        rx_ring[i].status = 0;
    }
    reg_write(REG_RDBAL, (uint32_t)(ring & 0xFFFFFFFF));
    reg_write(REG_RDBAH, (uint32_t)(ring >> 32));
    reg_write(REG_RDLEN, RX_DESC * sizeof(struct rx_desc));
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, RX_DESC - 1);
    rx_cur = 0;

    return 0;
}

static int tx_init(void)
{
    tx_dma = dma_alloc_coherent(&nic_dma, 4096, 4096, 0);
    if (!tx_dma) return -1;
    uint64_t ring = dma_addr_value(tx_dma->dma);
    tx_ring = tx_dma->cpu;
    for (int i = 0; i < TX_DESC; i++) {
        struct dma_buffer *b = dma_alloc_coherent(&nic_dma, 4096, 4096, 0);
        if (!b) return -1;
        tx_buf[i] = b->cpu;
        tx_ring[i].addr = dma_addr_value(b->dma);
        tx_ring[i].status = TXD_STA_DD;
    }
    reg_write(REG_TDBAL, (uint32_t)(ring & 0xFFFFFFFF));
    reg_write(REG_TDBAH, (uint32_t)(ring >> 32));
    reg_write(REG_TDLEN, TX_DESC * sizeof(struct tx_desc));
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    tx_cur = 0;
    reg_write(REG_TIPG, 10 | (8 << 10) | (6 << 20));

    return 0;
}

/* Contract: callers must hold net_lock (IF=0) -- the TX ring and tx_cur are not
 * otherwise serialized. All current paths (eth/ip/tcp/udp/icmp send) do. */
static int e1000_tx_frame(const void *frame, uint16_t len)
{
    if (!mmio || nic_dma.blocked || len > BUF_SIZE) return -1;
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
    NET_GUARD;
    if (!mmio || nic_dma.blocked) return 0;
    struct e1000_pending rep; rep.link = rep.stats = 0; rep.goct_note = 0; rep.link_status = 0; rep.irq = rep.dirq = 0;
#ifdef E1000_REG_PROBE
    rep.probe = 0;
#endif
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
     * edge, rather than being silently folded into the cause we just cleared.
     *
     * The value is no longer discarded: this read CONSUMES ICR.LSC as well, so
     * a link change noticed here would otherwise be destroyed by the very
     * mechanism that keeps receive alive. */
    if (reg_read(REG_ICR) & ICR_LSC) g_link_evt = 1;
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
    /* Inside the lock, and after the drain rather than before it: the counters
     * are ordinary memory shared with nothing else, but the REGISTERS are
     * read-to-clear, and net_lock is what makes "exactly one reader" true when
     * the WM thread's net_poll and an app thread's blocking fetch both reach
     * this function. After, so the frames this call delivered are already in
     * the NIC's counters when the sample takes them. */
    stats_poll(&rep);
    net_unlock(f);
    report_flush(&rep);                          /* kprintf with the lock RELEASED */
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
     * storms when idle. (NOT RXO either, for the same re-assert reason.)
     *
     * LSC is safe to add and does not join that argument: it is raised on a
     * TRANSITION, not on a level, so it cannot re-assert the instant ICR is
     * read -- there is nothing to re-assert until somebody moves a cable. It is
     * what makes "is the cable in?" answerable at all promptly; the once-a-
     * second STATUS read in stats_poll() is the backstop for a machine where
     * the interrupt never arrives. */
    reg_write(REG_IMS, ICR_RXT0 | ICR_LSC);      /* receive + link change */
}

/* Called from the NIC IRQ handler. Ack the device and hand the drain to
 * SOFTIRQ_NET -- see the receive-path comment in c/net/core/net.c for why the
 * protocol work no longer happens here, and for the one case (a NIC interrupt
 * nested inside a kernel sti window) where net_rx_schedule drains inline. */
static void e1000_isr(void)
{
    NET_GUARD;
    if (!mmio) return;
    uint32_t icr = reg_read(REG_ICR);            /* read-to-clear the causes */
    if (!icr || icr == UINT32_MAX) return;       /* shared INTx, or removed PCI function */
    g_irq++;
    /* NOT reported here. kprintf from the NIC vector runs with IF=0 and the BKL
     * held, and would put a VGA scroll inside an interrupt; the flag is picked
     * up by the next drain, which the softirq raised below is about to run. */
    if (icr & ICR_LSC) g_link_evt = 1;
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
    if (dev_enable_checked(dev, 0) != 0) {
        kprintf("[e1000] PCI Command decode rejected\n"); return -1;
    }
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

    if (reg_read(REG_CTRL) & CTRL_RST) {
        kprintf("[e1000] reset timeout\n");
        return -1;
    }

    /* Program receive-address filter 0 with our MAC and the Address-Valid bit
     * (RAH bit 31). Without AV the NIC drops unicast frames (only BAM broadcasts
     * pass), so ARP/ICMP replies addressed to us would never be received. */
    reg_write(REG_RAL0, (uint32_t)e1000_dev.mac[0] | ((uint32_t)e1000_dev.mac[1] << 8) |
              ((uint32_t)e1000_dev.mac[2] << 16) | ((uint32_t)e1000_dev.mac[3] << 24));
    reg_write(REG_RAH0, (uint32_t)e1000_dev.mac[4] | ((uint32_t)e1000_dev.mac[5] << 8) | (1u << 31));

    dma_device_init(&nic_dma, "e1000", DMA_MASK_64);
    if (rx_init() != 0 || tx_init() != 0) {
        dma_discard_unpublished();
        rx_ring = NULL; tx_ring = NULL;
        kprintf("[e1000] descriptor ring allocation failed\n");
        mmio = NULL;
        return -1;
    }

    if (dev_enable_checked(dev, 1) != 0) {
        kprintf("[e1000] PCI bus-master enable rejected\n");
        dma_discard_unpublished();
        rx_ring = NULL; tx_ring = NULL; mmio = NULL;
        return -1;
    }
    dma_publish_buffers();
    reg_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_MPE | RCTL_SECRC | RCTL_BSIZE_2048);
    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP | (0x10 << TCTL_CT_SHIFT) | (0x40 << TCTL_COLD_SHIFT));

    /* QEMU only re-offers a packet that arrived while RX was disabled when the
     * guest pokes the NIC; re-write RDT so any queued frame is flushed to us. */
    reg_write(REG_RDT, RX_DESC - 1);

    /* INTERRUPT MODERATION -- IMPLEMENTED, MEASURED, AND LEFT OFF.
     *
     * The premise for doing this was that every received frame is one
     * interrupt that takes the BKL, and CLAUDE.md's profile puts
     * interrupt_handler+0xc2 at 32% of BKL-held time on four cores. On THIS
     * machine that premise is false, and the driver's own counters are what
     * say so.
     *
     * MEASURED (DEVICE, 2026-08-20, QEMU e1000, -smp 4 TCG, three 917,504-byte
     * HTTP fetches, from the `irq (+N)` field of the stats line beside the
     * packet count in the same line):
     *
     *     ITR off    1,941 packets received, 44 NIC interrupts total
     *     ITR = 61   1,940 packets received,  8 NIC interrupts total
     *
     * ONE INTERRUPT PER 44 RECEIVED FRAMES, before any moderation. The reason
     * is in tests/boot/run-net-rx-test.sh's header: e1000_rx_drain() reads ICR
     * at its top and net_poll() reaches that drain ~100x/s, so a poll landing
     * between the card asserting and the CPU taking the interrupt CONSUMES the
     * cause and does the work itself. The 100 Hz poll is already the
     * moderator, and it is a far coarser one than any ITR value.
     *
     * The run-to-run spread makes the comparison unquotable anyway: three
     * ITR-off boots of the SAME build reported 12, 44 and 40 total interrupts.
     * A 44 -> 8 difference sits inside that, and the number that matters --
     * 1,941 packets against 44 interrupts -- says there is nothing left to
     * moderate.
     *
     * So the register write stays behind a knob that defaults to 0. It is not
     * dead code and it is not speculative: on hardware where the interrupt
     * actually fires per frame (real silicon, or any build whose poll backstop
     * is removed) this is the one line that bounds the rate, and `make
     * E1000_ITR=<n>` turns it on for a paired measurement without an edit.
     * Landing it ON, on the strength of a difference this measurement cannot
     * resolve, would be the design statement this tree has twice had
     * contradicted by its own numbers.
     *
     * The unit is not portable and e1000_stats.h says so: 256 ns per count in
     * the 8254x manual, and an emulator may scale it differently -- which is
     * why nothing here converts to microseconds and why the value above is
     * quoted as a raw register value beside the interrupt count it produced.
     */
#if E1000_ITR_VALUE
    reg_write(REG_ITR, E1000_ITR_VALUE);
#endif

    /* Discard whatever the statistics block holds before counting anything: a
     * reset clears it on this part, but a warm handoff from a previous owner
     * (firmware, kexec, a re-probe) does not, and attributing their packets to
     * us is exactly the kind of plausible small number that survives review. */
    e1000_stats_prime(e1000_rd, 0);
    g_stat_next_ms = timer_ms() + STAT_PERIOD_MS;

    /* One line at bring-up, so "is the cable in?" has an answer from boot
     * rather than from the first transition. It is also the control for the
     * transition reporting: a harness that unplugs the cable needs to know the
     * link was up first, and "no line at all" and "down" read the same. */
    struct e1000_pending rep; rep.link = rep.stats = 0; rep.goct_note = 0; rep.link_status = 0; rep.irq = rep.dirq = 0;
#ifdef E1000_REG_PROBE
    rep.probe = 0;
#endif
    link_check(&rep);
    report_flush(&rep);

    kprintf("[e1000] up: mmio=%p\n", (void *)(uintptr_t)base);
    dev_set_drvdata(dev, &e1000_dev);
    return 0;
}

/* The existing device-model removal hook stops DMA before releasing backing
 * pages. A controller that cannot acknowledge reset keeps every allocation. */
void e1000_remove(struct device *dev)
{
    NET_GUARD;
    (void)dev;
    if (!mmio) return;
    reg_write(REG_IMC, 0xffffffffu);
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_RST);
    for (unsigned i = 0; i < 1000000; i++) {
        if (!(reg_read(REG_CTRL) & CTRL_RST)) goto stopped;
    }
    dma_device_quarantine(&nic_dma);
    kprintf("[e1000] reset unconfirmed: DMA quarantined\n");
    return;
stopped:
    dma_device_quiesced(&nic_dma);
    dma_discard_unpublished(); /* now quiesced, including previously owned pages */
    mmio = NULL;
    rx_ring = NULL; tx_ring = NULL;
}
