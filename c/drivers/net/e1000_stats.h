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
/* The 8254x manual specifies every register below as READ-TO-CLEAR: reading
 * one returns the count since the last read and zeroes it in the same access.
 *
 * THAT IS FALSE OF THE ONLY DEVICE THIS TREE HAS EVER BOOTED ON, MEASURED
 * rather than assumed -- see e1000_stat_acc_auto() below. QEMU 11.0.0's e1000
 * model (hw/net/e1000.c, fetched at the v11.0.0 tag, not recalled) clears
 * GPRC/GPTC and the high half of each 64-bit octet pair on read, exactly as
 * the manual says, and does NOT clear MPC -- ever, on any read, by anyone.
 * Trusting `*sw += hw` for a register that never clears re-adds whatever it
 * last held once every STAT_PERIOD_MS FOREVER: one real missed frame prints
 * as a "mpc" field that climbs by a constant with no traffic at all, for the
 * rest of the boot, and because e1000_stats_losses() (below) then changes on
 * every single sample as a side effect, it also floods stats_poll()'s report
 * line once a second for the rest of the boot -- CLAUDE.md rule 5's "an
 * instrument that floods the log it writes to has replaced the thing it was
 * measuring". Both held, unnoticed, since this driver's first boot.
 *
 * A handful of the registers below are a THIRD case, worse than either:
 * RNBC, GORCL/GORCH, GOTCL/GOTCH, RLEC, COLC, ECOL and LATECOL never reach a
 * real register at all on this device -- see the comment above `rx_no_buf`
 * in `struct e1000_stats`.
 *
 * What is still true everywhere, silicon or QEMU, clearing or not:
 *
 *   - THERE MAY BE EXACTLY ONE READER IN THE WHOLE KERNEL. A second one sees
 *     whatever the first left behind (zero, on a register that does clear)
 *     and, worse, steals the count from the first. e1000.c has that one
 *     reader (its stats_poll(), inside the RX drain and under net_lock) and
 *     every other consumer goes through its stats_get(), which does not touch
 *     the device at all.
 *   - the software counters must be 64-bit accumulators. `sw = read()` is the
 *     naive form and it is not merely imprecise: on a register that DOES
 *     clear it silently redefines the counter as "since the previous
 *     sample", and on one that does NOT clear it silently redefines the
 *     counter as "whatever the register currently holds" -- either way a
 *     total of zero and a link that has never dropped a frame print
 *     identically. That form is what -DE1000_STATS_NO_ACC compiles, and it is
 *     the negative control.
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
     * already break down by cause.
     *
     * THE ZERO IS NOW EXPLAINED, NOT JUST OBSERVED. QEMU 11.0.0's e1000 model
     * tags GORCL/GORCH/GOTCL/GOTCH MAC_ACCESS_FLAG_NEEDED in mac_reg_access[]
     * (hw/net/e1000.c:1235-1236) with no compat_flags bit that can ever
     * satisfy the read gate at :1296-1309 (`mac_reg_access[x] >> 2 == 0` for
     * every entry that carries only that flag, so `compat_flags & 0` is
     * always false) -- e1000_mmio_read returns a literal 0 for these four
     * registers UNCONDITIONALLY, on every guest, regardless of traffic. That
     * is a fact about the register, not about the network, and rx_good_bytes
     * / tx_good_bytes staying zero forever must not be printed as though it
     * were the latter. `rx_good_bytes_stuck` / `tx_good_bytes_stuck` below
     * are how the sampler tells the two apart: a good packet's octets ALWAYS
     * contribute to GORC by protocol definition, on any 8254x-compatible
     * device, so packets counted with their bytes never once counted cannot
     * be a legitimate zero -- unlike RNBC below, this correlation does not
     * depend on which emulator is running, so it is safe to detect live
     * rather than only assert from the QEMU source. */
    uint64_t rx_good_bytes; /* GORCL + GORCH */
    uint64_t tx_good_bytes; /* GOTCL + GOTCH */
    int rx_good_bytes_stuck; /* rx_pkts moved, rx_good_bytes never did: this
                               * register cannot be read on this device, full
                               * stop -- see the comment above. Sticky: once
                               * earned it is never cleared back to 0, because
                               * the evidence that earned it does not expire. */
    int tx_good_bytes_stuck;

    /* The denominator. CLAUDE.md quotes 269.9 Mbit/s with nothing beside it;
     * these are the two numbers that say whether that run was clean.
     * RNBC = the NIC had a frame and we had posted no descriptor for it.
     * MPC  = the on-chip FIFO overflowed, i.e. the host bus or the driver did
     *        not keep up. They are different failures with different fixes,
     *        which is why they are not summed into one "dropped".
     *
     * MEASURED, QEMU 11.0.0 (hw/net/e1000.c, fetched at the v11.0.0 tag, not
     * recalled): MPC is real and moves -- e1000_receiver_overrun() is the
     * model's only writer of it -- but is served by `mac_readreg`, a PLAIN,
     * NON-CLEARING getter, unlike the 8254x manual's read-to-clear spec. That
     * is exactly what e1000_stat_acc_auto() below exists to be right about
     * regardless (see its comment); rx_missed does not special-case MPC by
     * name, because a build constant naming ONE register as the exception is
     * CLAUDE.md's "one jar, two doors" waiting to disagree with itself the
     * day this driver runs on silicon where MPC genuinely does clear.
     *
     * RNBC does NOT get the same automatic correction, and that is a
     * decision, not an oversight: it is tagged MAC_ACCESS_FLAG_NEEDED in
     * mac_reg_access[] the same way GORC/GOTC are (:1232, gated the same
     * unsatisfiable way at :1296-1309), so e1000_mmio_read returns a literal
     * 0 for it UNCONDITIONALLY -- confirmed live, RNBC read 0 across 700
     * logged samples and 107 MB of traffic including runs where its sibling
     * MPC was demonstrably nonzero. The reason this is NOT auto-detected the
     * way rx_good_bytes_stuck is above: RNBC and MPC measure two genuinely
     * DIFFERENT physical events -- ring exhaustion vs on-chip FIFO overflow
     * -- that CAN disagree on real silicon (a card can overflow its FIFO
     * without the ring ever running dry, or the reverse). "MPC moved and
     * RNBC didn't" is proof of nothing there; it is only proof here, because
     * the QEMU source settles it. A caller that needs "did the ring run dry
     * on THIS emulator" cannot get that answer from rx_no_buf -- there is no
     * live signal that would tell it apart from a real, honest zero, so this
     * driver reports the register faithfully (0, always, on this device)
     * rather than fabricate a flag it cannot back with evidence. */
    uint64_t rx_no_buf;     /* RNBC  */
    uint64_t rx_missed;     /* MPC   */

    uint64_t crc_errs;      /* CRCERRS */
    uint64_t len_errs;      /* RLEC    */
    uint64_t colls;         /* COLC    */
    uint64_t excess_colls;  /* ECOL    */
    uint64_t late_colls;    /* LATECOL */

    /* Last raw value read from each SINGLE (32-bit) counter above, i.e. every
     * field on this list except the 64-bit octet pairs. e1000_stat_acc_auto()
     * needs these to tell "this register is sticky and unchanged" from "this
     * register is disabled and reads 0" across a sample boundary -- see its
     * comment. Persisted here rather than as file-local statics in e1000.c so
     * a host test can drive two independent `struct e1000_stats` instances
     * (test_second_reader_sees_zero already relies on exactly that). */
    uint32_t raw_rx_pkts, raw_tx_pkts;
    uint32_t raw_rx_no_buf, raw_rx_missed;
    uint32_t raw_crc_errs, raw_len_errs, raw_colls, raw_excess_colls, raw_late_colls;

    uint64_t samples;       /* how many times the block has been folded in */
};

/* How the sampler reaches the register file. In the kernel this is one MMIO
 * load; in the host test it is a model that implements read-to-clear. */
typedef uint32_t (*e1000_rd32)(void *ctx, uint32_t off);

/* SINGLE (32-bit) COUNTER, ACCUMULATED WITHOUT ASSUMING HOW IT CLEARS.
 *
 * The naive form, `*sw += hw`, is correct only if the register cleared itself
 * on the read that produced `hw` -- true of GPRC/GPTC on this device, false
 * of MPC (see the block comment above `struct e1000_stats`'s `rx_no_buf`
 * field, and the block comment at the top of this file). A build constant
 * naming MPC as the exception would fix today's bug and reintroduce it the
 * day this driver runs on real silicon, where MPC genuinely IS read-to-clear
 * -- CLAUDE.md's "one jar, two doors" with a `#define` standing in for the
 * door that should be a live read. So this does not choose a behaviour for a
 * named register; it is handed TWO back-to-back reads of the SAME register,
 * taken with nothing else touching the device in between (e1000_stats_sample
 * below does exactly that for every field this function drives), and derives
 * which behaviour it is looking at from them:
 *
 *   hw2 == 0 and hw1 != 0   the read cleared it. hw1 IS the exact count since
 *                           the previous sample -- there is no earlier state
 *                           to consult, and none is used.
 *   otherwise               nothing cleared between hw1 and hw2 (the common
 *                           case is hw1 == hw2). The register is a
 *                           free-running total, so the delta since the LAST
 *                           SAMPLE is against `*last_raw`, the raw value this
 *                           function itself saved then -- not against zero,
 *                           which is what made MPC re-add itself forever. A
 *                           drop below `*last_raw` means the counter itself
 *                           was reset underneath us (a device reset, not a
 *                           read), so the fresh value is already the count
 *                           since that reset and is taken whole.
 *
 * `*last_raw` is per-register state in `struct e1000_stats`, not a file-local
 * static, for the same reason `struct e1000_stats` itself is a parameter
 * rather than a global: test_second_reader_sees_zero already proves a driver
 * bug by running two independent samplers against one register file, and that
 * stops being possible the moment any of this state lives outside the struct
 * the caller owns.
 *
 * This is the SAME TECHNIQUE `e1000_stat_acc64` above already uses to survive
 * not knowing which half of a 64-bit pair clears -- a delta taken against
 * state this function saved last time, rather than an assumption about the
 * hardware's contract -- just carried inside one register instead of across
 * the two halves of a pair, and settled by direct observation (the two reads)
 * instead of by an argument that has to work under either convention. */
static inline void e1000_stat_acc_auto(uint64_t *sw, uint32_t *last_raw,
                                        uint32_t hw1, uint32_t hw2)
{
#ifdef E1000_STATS_NO_ACC
    *sw = hw1;                    /* NEGATIVE CONTROL: the naive read */
    *last_raw = hw2;
#else
    if (hw1 != 0 && hw2 == 0) {
        *sw += hw1;                /* cleared: hw1 is the whole delta */
        *last_raw = 0;
    } else {
        uint32_t now = hw2;        /* did not clear: a free-running total */
        *sw += (now >= *last_raw) ? (now - *last_raw) : now;
        *last_raw = now;
    }
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
 * whole block exists to make impossible.
 *
 * Every SINGLE (32-bit) counter is now read TWICE, back to back, so
 * e1000_stat_acc_auto() can tell a register that just cleared from one that
 * never does -- see its comment. That doubles the trap count for those nine
 * registers (18 instead of 9); at this function's one-second call rate
 * (STAT_PERIOD_MS in e1000.c) that is nine extra MMIO reads a second, not a
 * cost worth a second code path for. GPRC/GPTC go through the same double
 * read as MPC/RNBC/etc for the same reason ONE mechanism is used throughout
 * this function rather than "clearing registers use the old accumulate,
 * MPC uses the new one": a per-register special case is exactly the thing
 * that was wrong here before. */
static inline void e1000_stats_sample(struct e1000_stats *st, e1000_rd32 rd, void *ctx)
{
    uint32_t lo, hi, a, b;

    a = rd(ctx, E1000_REG_GPRC); b = rd(ctx, E1000_REG_GPRC);
    e1000_stat_acc_auto(&st->rx_pkts, &st->raw_rx_pkts, a, b);
    a = rd(ctx, E1000_REG_GPTC); b = rd(ctx, E1000_REG_GPTC);
    e1000_stat_acc_auto(&st->tx_pkts, &st->raw_tx_pkts, a, b);

    lo = rd(ctx, E1000_REG_TORL);  hi = rd(ctx, E1000_REG_TORH);
    e1000_stat_acc64(&st->rx_bytes, lo, hi);
    lo = rd(ctx, E1000_REG_TOTL);  hi = rd(ctx, E1000_REG_TOTH);
    e1000_stat_acc64(&st->tx_bytes, lo, hi);
    lo = rd(ctx, E1000_REG_GORCL); hi = rd(ctx, E1000_REG_GORCH);
    e1000_stat_acc64(&st->rx_good_bytes, lo, hi);
    lo = rd(ctx, E1000_REG_GOTCL); hi = rd(ctx, E1000_REG_GOTCH);
    e1000_stat_acc64(&st->tx_good_bytes, lo, hi);

    a = rd(ctx, E1000_REG_RNBC); b = rd(ctx, E1000_REG_RNBC);
    e1000_stat_acc_auto(&st->rx_no_buf, &st->raw_rx_no_buf, a, b);
    a = rd(ctx, E1000_REG_MPC); b = rd(ctx, E1000_REG_MPC);
    e1000_stat_acc_auto(&st->rx_missed, &st->raw_rx_missed, a, b);
    a = rd(ctx, E1000_REG_CRCERRS); b = rd(ctx, E1000_REG_CRCERRS);
    e1000_stat_acc_auto(&st->crc_errs, &st->raw_crc_errs, a, b);
    a = rd(ctx, E1000_REG_RLEC); b = rd(ctx, E1000_REG_RLEC);
    e1000_stat_acc_auto(&st->len_errs, &st->raw_len_errs, a, b);
    a = rd(ctx, E1000_REG_COLC); b = rd(ctx, E1000_REG_COLC);
    e1000_stat_acc_auto(&st->colls, &st->raw_colls, a, b);
    a = rd(ctx, E1000_REG_ECOL); b = rd(ctx, E1000_REG_ECOL);
    e1000_stat_acc_auto(&st->excess_colls, &st->raw_excess_colls, a, b);
    a = rd(ctx, E1000_REG_LATECOL); b = rd(ctx, E1000_REG_LATECOL);
    e1000_stat_acc_auto(&st->late_colls, &st->raw_late_colls, a, b);

    /* GOOD-OCTET LIVENESS -- see the comment above `rx_good_bytes_stuck` in
     * `struct e1000_stats`. Once true, stays true: the evidence that earned
     * it (packets counted, their bytes never counted) does not un-happen. */
    if (st->rx_pkts && !st->rx_good_bytes) st->rx_good_bytes_stuck = 1;
    if (st->tx_pkts && !st->tx_good_bytes) st->tx_good_bytes_stuck = 1;

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
    junk.rx_good_bytes_stuck = junk.tx_good_bytes_stuck = 0;
    junk.rx_no_buf = junk.rx_missed = junk.crc_errs = junk.len_errs = 0;
    junk.colls = junk.excess_colls = junk.late_colls = junk.samples = 0;
    junk.raw_rx_pkts = junk.raw_tx_pkts = 0;
    junk.raw_rx_no_buf = junk.raw_rx_missed = 0;
    junk.raw_crc_errs = junk.raw_len_errs = junk.raw_colls = 0;
    junk.raw_excess_colls = junk.raw_late_colls = 0;
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
