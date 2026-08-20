#ifndef LOGIT_E1000_STATS_H
#define LOGIT_E1000_STATS_H

#include <stdint.h>

/* The e1000's statistics block and link status, as PURE COMPUTATION.
 *
 * Why this is a separate header and not just code in e1000.c: the one thing in
 * a statistics counter that can be wrong without anybody noticing is the
 * accumulation, and it is wrong in a way that still produces plausible small
 * numbers -- which this tree has been bitten by twice (see CLAUDE.md's units
 * bugs). Everything below is arithmetic over a register file passed in as a
 * callback, so tests/unit/e1000_stats_test.c drives THE SAME FUNCTION the
 * kernel drives against a model of a read-to-clear register, instead of a copy
 * of it. net_drv_test.c's header already argues why the register PROGRAMMING
 * must not be mocked; this is the other half, the part that must be.
 *
 * The rejected alternative was putting these in netring.h beside the ring
 * arithmetic. netring.h is shared by four drivers and holds nothing
 * device-specific; a block of 82540EM register offsets is exactly that.
 */

/* ---------------------------------------------------------------- STATUS -- */

#define E1000_STATUS_FD          (1u << 0)   /* full duplex */
#define E1000_STATUS_LU          (1u << 1)   /* link up */
#define E1000_STATUS_SPEED_SHIFT 6
#define E1000_STATUS_SPEED_MASK  0x3u

static inline int e1000_link_is_up(uint32_t status)
{
    return (status & E1000_STATUS_LU) != 0;
}

static inline int e1000_link_is_fd(uint32_t status)
{
    return (status & E1000_STATUS_FD) != 0;
}

/* 8254x STATUS bits 7:6: 00 = 10, 01 = 100, 10 = 1000, 11 = reserved. The
 * reserved encoding is reported as 1000 rather than 0 on purpose -- a card that
 * answers 11b is linked at SOMETHING, and printing "0 Mb/s" for a working link
 * would be a lie with the shape of a diagnosis. */
static inline uint32_t e1000_link_mbps(uint32_t status)
{
    switch ((status >> E1000_STATUS_SPEED_SHIFT) & E1000_STATUS_SPEED_MASK) {
    case 0:  return 10;
    case 1:  return 100;
    default: return 1000;
    }
}

/* The bits worth REPORTING -- link, duplex, speed -- and nothing else.
 *
 * This is what makes "once per transition, never per poll" true rather than
 * intended. STATUS also carries GIO_MASTER_ENABLE, TXOFF, the auto-speed-detect
 * value and a handful of reserved bits, several of which move on their own; a
 * driver that compared the whole register would print a link line every time
 * the bus went idle. Comparing a mask is not an optimisation, it is the
 * difference between a transition and a sample. */
#define E1000_STATUS_REPORTED \
    (E1000_STATUS_LU | E1000_STATUS_FD | \
     (E1000_STATUS_SPEED_MASK << E1000_STATUS_SPEED_SHIFT))

static inline int e1000_link_changed(uint32_t a, uint32_t b)
{
#ifdef E1000_LINK_NEGCTL
    /* NEGATIVE CONTROL: the plausible wrong implementation, which is what the
     * obvious `if (status != last)` gives. It is the PLAUSIBLE one and not the
     * absent one -- the link is still reported, still with the right speed and
     * duplex, still on every real transition. What changes is that it also
     * reports on transitions that are not link events at all. */
    return a != b;
#else
    return ((a ^ b) & E1000_STATUS_REPORTED) != 0;
#endif
}

/* ------------------------------------------------------ statistics block -- */
/* All of these are READ-TO-CLEAR. Reading one returns the count since the last
 * read and zeroes it in the same access, so:
 *
 *   - THERE MAY BE EXACTLY ONE READER IN THE WHOLE KERNEL. A second one sees
 *     zero and, worse, steals the count from the first. e1000.c has that one
 *     reader (its stats_poll(), inside the RX drain and under net_lock) and
 *     every other consumer goes through its stats_get(), which does not touch
 *     the device at all.
 *   - the software counters must be 64-bit accumulators. `sw = read()` is the
 *     naive form and it is not merely imprecise: it silently redefines every
 *     counter as "since the previous sample", so a total of zero and a link
 *     that has never dropped a frame print identically. That form is what
 *     -DE1000_STATS_NO_ACC compiles, and it is the negative control.
 */

#define E1000_REG_CRCERRS 0x04000   /* CRC error count                        */
#define E1000_REG_MPC     0x04010   /* missed packets: RX FIFO overran        */
#define E1000_REG_ECOL    0x04018   /* excessive collisions                   */
#define E1000_REG_LATECOL 0x04020   /* late collisions                        */
#define E1000_REG_COLC    0x04028   /* collision count                        */
#define E1000_REG_RLEC    0x04040   /* receive length error                   */
#define E1000_REG_GPRC    0x04074   /* good packets received                  */
#define E1000_REG_GPTC    0x04080   /* good packets transmitted               */
#define E1000_REG_GORCL   0x04088   /* good octets received, low              */
#define E1000_REG_GORCH   0x0408C   /* good octets received, high             */
#define E1000_REG_GOTCL   0x04090   /* good octets transmitted, low           */
#define E1000_REG_GOTCH   0x04094   /* good octets transmitted, high          */
#define E1000_REG_RNBC    0x040A0   /* receive no buffers: the ring ran dry   */
#define E1000_REG_TORL    0x040C0   /* total octets received, low             */
#define E1000_REG_TORH    0x040C4   /* total octets received, high            */
#define E1000_REG_TOTL    0x040C8   /* total octets transmitted, low          */
#define E1000_REG_TOTH    0x040CC   /* total octets transmitted, high         */

struct e1000_stats {
    uint64_t rx_pkts;       /* GPRC  */
    uint64_t tx_pkts;       /* GPTC  */

    /* TOTAL octets, from TOR/TOT -- destination address through CRC inclusive,
     * errored frames included. NOT the good-octet registers, and the reason is
     * a measurement rather than a preference.
     *
     * MEASURED (DEVICE, 2026-08-20, QEMU e1000 = 82540EM, three 128 KiB HTTP
     * fetches). A diagnostic build printed the raw words after traffic:
     *
     *     [probe] GORC 0/0 GOTC 0/0 TOR 2010/0 TOT 912/0 TPR 5 TPT 5
     *
     * GORCL/GORCH and GOTCL/GOTCH are zero at every sample while packets and
     * total octets both count. The first stats line this driver ever printed
     * read `rx 651 pkt / 0 B` -- a byte counter structurally stuck at zero,
     * which is precisely the plausible-looking hole this whole block exists to
     * make impossible, and it would have shipped as "the network moved no
     * bytes" in every log.
     *
     * The rejected alternative was to keep GORC and describe the zero in a
     * comment. A number nobody can use is not improved by an explanation of
     * why it is useless, and CLAUDE.md's own scoreboard section is about
     * exactly this: an instrument that cannot report is not an instrument. */
    uint64_t rx_bytes;      /* TORL + TORH */
    uint64_t tx_bytes;      /* TOTL + TOTH */

    /* The good-octet registers, kept and REPORTED rather than dropped. Two
     * reasons, and neither is sentiment: on real silicon they do count and an
     * unread read-to-clear counter saturates, and printing them is what makes
     * "this emulator does not implement them" a visible fact in the log
     * instead of a thing somebody has to rediscover. Their difference from
     * rx_bytes/tx_bytes is the errored traffic, which the error counters below
     * already break down by cause. */
    uint64_t rx_good_bytes; /* GORCL + GORCH */
    uint64_t tx_good_bytes; /* GOTCL + GOTCH */

    /* The denominator. CLAUDE.md quotes 269.9 Mbit/s with nothing beside it;
     * these are the two numbers that say whether that run was clean.
     * RNBC = the NIC had a frame and we had posted no descriptor for it.
     * MPC  = the on-chip FIFO overflowed, i.e. the host bus or the driver did
     *        not keep up. They are different failures with different fixes,
     *        which is why they are not summed into one "dropped". */
    uint64_t rx_no_buf;     /* RNBC  */
    uint64_t rx_missed;     /* MPC   */

    uint64_t crc_errs;      /* CRCERRS */
    uint64_t len_errs;      /* RLEC    */
    uint64_t colls;         /* COLC    */
    uint64_t excess_colls;  /* ECOL    */
    uint64_t late_colls;    /* LATECOL */

    uint64_t samples;       /* how many times the block has been folded in */
};

/* How the sampler reaches the register file. In the kernel this is one MMIO
 * load; in the host test it is a model that implements read-to-clear. */
typedef uint32_t (*e1000_rd32)(void *ctx, uint32_t off);

static inline void e1000_stat_acc(uint64_t *sw, uint32_t hw)
{
#ifdef E1000_STATS_NO_ACC
    *sw = hw;                    /* NEGATIVE CONTROL: the naive read */
#else
    *sw += hw;
#endif
}

/* A 64-bit octet counter lives in two registers and the READ ORDER IS PART OF
 * THE CONTRACT -- low first, high second.
 *
 * A device can clear the pair on either half, and this driver has to be right
 * under both, because it can only ever be MEASURED under one of them:
 *
 *   CLEAR-ON-HIGH   what the 8254x manual specifies: the low register reads
 *                   without clearing, and reading the HIGH register returns the
 *                   high word and clears the whole pair.
 *   CLEAR-ON-LOW    reading the LOW register returns the low word and clears
 *                   the pair, so the high register then reads zero.
 *
 * Low-then-high is exact under CLEAR-ON-HIGH, and under CLEAR-ON-LOW it is
 * exact for any value below 4 GiB per sample and under-reports above it. At
 * this driver's one-second sample period, 4 GiB per sample is 34 Gbit/s -- two
 * orders of magnitude above the 269.9 Mbit/s this tree has ever measured.
 *
 * High-then-low is the ordering that looks equally reasonable in a diff and it
 * is CATASTROPHIC under CLEAR-ON-HIGH: the high read clears the pair, the low
 * read returns zero, and every octet total comes back a multiple of 4 GiB. So
 * the order is not a preference, and both models are in the host test.
 *
 * WHICH ONE QEMU IMPLEMENTS IS DELIBERATELY NOT ASSERTED HERE. It was, from
 * recollection of hw/net/e1000.c, and recollection is not a source this tree
 * accepts. The argument above does not need the answer, so it does not claim
 * one -- and a reader who needs it has two named conventions to go and check
 * rather than one confident sentence to trust. */
static inline void e1000_stat_acc64(uint64_t *sw, uint32_t lo, uint32_t hi)
{
#ifdef E1000_STATS_NO_ACC
    *sw = (uint64_t)lo | ((uint64_t)hi << 32);
#else
    *sw += (uint64_t)lo | ((uint64_t)hi << 32);
#endif
}

/* Fold one read of the whole block into `st`. THE SINGLE READER.
 *
 * Every register named here is drained on every call even though a caller may
 * only care about two of them: leaving one unread on real silicon lets it
 * saturate at 0xFFFFFFFF, and a saturated counter reports the same number
 * forever, which reads as "nothing is happening" -- the failure mode this
 * whole block exists to make impossible. */
static inline void e1000_stats_sample(struct e1000_stats *st, e1000_rd32 rd, void *ctx)
{
    uint32_t lo, hi;

    e1000_stat_acc(&st->rx_pkts,      rd(ctx, E1000_REG_GPRC));
    e1000_stat_acc(&st->tx_pkts,      rd(ctx, E1000_REG_GPTC));

    lo = rd(ctx, E1000_REG_TORL);  hi = rd(ctx, E1000_REG_TORH);
    e1000_stat_acc64(&st->rx_bytes, lo, hi);
    lo = rd(ctx, E1000_REG_TOTL);  hi = rd(ctx, E1000_REG_TOTH);
    e1000_stat_acc64(&st->tx_bytes, lo, hi);
    lo = rd(ctx, E1000_REG_GORCL); hi = rd(ctx, E1000_REG_GORCH);
    e1000_stat_acc64(&st->rx_good_bytes, lo, hi);
    lo = rd(ctx, E1000_REG_GOTCL); hi = rd(ctx, E1000_REG_GOTCH);
    e1000_stat_acc64(&st->tx_good_bytes, lo, hi);

    e1000_stat_acc(&st->rx_no_buf,    rd(ctx, E1000_REG_RNBC));
    e1000_stat_acc(&st->rx_missed,    rd(ctx, E1000_REG_MPC));
    e1000_stat_acc(&st->crc_errs,     rd(ctx, E1000_REG_CRCERRS));
    e1000_stat_acc(&st->len_errs,     rd(ctx, E1000_REG_RLEC));
    e1000_stat_acc(&st->colls,        rd(ctx, E1000_REG_COLC));
    e1000_stat_acc(&st->excess_colls, rd(ctx, E1000_REG_ECOL));
    e1000_stat_acc(&st->late_colls,   rd(ctx, E1000_REG_LATECOL));

    st->samples++;
}

/* Discard whatever the block holds without counting it -- used once, straight
 * after the device reset, so that anything a previous owner of the card left
 * behind is not attributed to us. Deliberately expressed as a sample into a
 * throwaway rather than its own list of offsets: a second list is a second
 * thing to forget to update, and "prime clears exactly what sample reads" is
 * then true by construction instead of by review. */
static inline void e1000_stats_prime(e1000_rd32 rd, void *ctx)
{
    struct e1000_stats junk;
    junk.rx_pkts = junk.tx_pkts = junk.rx_bytes = junk.tx_bytes = 0;
    junk.rx_good_bytes = junk.tx_good_bytes = 0;
    junk.rx_no_buf = junk.rx_missed = junk.crc_errs = junk.len_errs = 0;
    junk.colls = junk.excess_colls = junk.late_colls = junk.samples = 0;
    e1000_stats_sample(&junk, rd, ctx);
    (void)junk;
}

/* Anything that says the link is losing frames. Kept separate from the byte and
 * packet totals because a report is worth printing when THIS moves even if no
 * traffic threshold has been crossed -- a single dropped frame during an
 * otherwise idle boot is the interesting event. */
static inline uint64_t e1000_stats_losses(const struct e1000_stats *st)
{
    return st->rx_no_buf + st->rx_missed + st->crc_errs + st->len_errs +
           st->excess_colls + st->late_colls;
}

/* ------------------------------------------------- interrupt moderation -- */

#define E1000_REG_ITR 0x000C4   /* interrupt throttling rate, 16 bits */

/* ITR is a MINIMUM INTER-INTERRUPT INTERVAL. Nothing here converts from
 * microseconds, and that is the point: the unit is 256 ns per count in the
 * 8254x manual, and an emulator is free to scale it differently. A helper that
 * took microseconds would have to pick one and would then be quietly wrong on
 * the other, on a value whose whole purpose is to be tuned by measurement.
 *
 * THE EMULATOR'S SCALE IS NOT ASSERTED FROM MEMORY. The driver writes a raw
 * value and the report line says what that value bought on this machine; if a
 * number in microseconds is ever wanted, measure the interrupt rate at two
 * settings and divide. e1000_itr_ns_hw() is the one conversion with a written
 * source behind it. */
static inline uint32_t e1000_itr_ns_hw(uint32_t itr)   { return itr * 256u; }

#endif /* LOGIT_E1000_STATS_H */
