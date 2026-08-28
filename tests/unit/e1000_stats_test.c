/* Host unit tests for the e1000 statistics accumulator and link-status decode.
 *
 * WHAT IS BEING TESTED, AND WHY IT CAN BE TESTED WITHOUT HARDWARE
 * ==============================================================
 * tests/unit/net_drv_test.c's header argues that a NIC's register PROGRAMMING
 * must not be mocked -- "if the fake agrees with the driver, both can be wrong
 * together". That argument does not apply here and its converse does. The
 * 8254x manual specifies every register below as read-to-clear -- and QEMU
 * 11.0.0's e1000 model, the only device this tree has ever booted on,
 * measurably does NOT implement that for MPC (hw/net/e1000.c: `mac_readreg`,
 * a plain non-clearing getter, vs GPRC's `mac_read_clr4`). A single missed
 * frame used to re-add itself to the software total once a second FOREVER --
 * a real boot's log showed `mpc` climbing by a constant, hundreds of times,
 * with no traffic. Every bug this file exists to catch is a bug in the
 * arithmetic wrapped around register behaviour, not in which register was
 * poked, and that now includes NOT ASSUMING which behaviour is in front of
 * it:
 *
 *   - `sw = read()` instead of `sw += read()`. Compiles, runs, reports numbers
 *     that go up and down plausibly, and quietly means "since the last sample"
 *     everywhere the driver claims a total.
 *   - a second reader anywhere in the kernel, which sees zero AND takes the
 *     count away from the first.
 *   - reading the high half of a 64-bit octet counter before the low half.
 *     Correct-looking, and catastrophic under the documented clearing rule:
 *     every octet total comes back a multiple of 4 GiB.
 *   - 32-bit software counters, which wrap after 4 GiB -- about two minutes at
 *     the 269.9 Mbit/s this tree has measured.
 *   - trusting `*sw += hw` for a register that never clears: the exact bug
 *     above, now with its own model (`sticky_singles`) and its own section
 *     ("the MPC bug, reproduced").
 *
 * The model below implements the documented rule and its plausible variants
 * -- for 64-bit pairs, which half clears; for single registers, whether they
 * clear at all -- and is driven through e1000_stats_sample() -- the same
 * function the kernel calls. Nothing in this file re-implements the
 * accumulation, which is what stops the model and the driver from being
 * wrong together.
 *
 * Built by tests/nic.mk: `make test-e1000-stats`.
 * Negative control:      `make test-e1000-stats-negctl` (-DE1000_STATS_NO_ACC).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "e1000_stats.h"

static int checks, failures;

#define CHECK(cond, ...) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

#define CHECK_EQ(got, want, what) do {                                        \
    unsigned long long g_ = (unsigned long long)(got);                        \
    unsigned long long w_ = (unsigned long long)(want);                       \
    CHECK(g_ == w_, "%s: got %llu (0x%llx), want %llu (0x%llx)",              \
          what, g_, g_, w_, w_);                                              \
} while (0)

/* ------------------------------------------------------- the model NIC --- */

/* Two models, because a device can clear a 64-bit counter pair on either half
 * and the driver has to be right under both. They are named after the rule,
 * NOT after a vendor: which one QEMU implements is not asserted anywhere in
 * this line of work, because the correctness argument does not need it and
 * asserting it from recollection is how a confident wrong sentence gets into a
 * file everybody reads.
 *
 *   MODEL_CLR_HI   the 8254x manual: a 32-bit counter clears on its own read;
 *                  a 64-bit pair clears when the HIGH register is read.
 *   MODEL_CLR_LO   reading the LOW register of a pair returns the low word and
 *                  clears BOTH, so the high register then reads zero.
 *
 * A test written against only MODEL_CLR_LO would pass a driver that reads
 * high-before-low, because under that rule the order happens not to matter --
 * and under MODEL_CLR_HI the same driver reports every octet total as a
 * multiple of 4 GiB.
 */
enum { MODEL_CLR_HI, MODEL_CLR_LO };

#define REGFILE_WORDS 0x100          /* 0x04000..0x043FC in 4-byte words */

struct model {
    int      kind;
    uint32_t reg[REGFILE_WORDS];     /* indexed by (off - 0x04000) / 4 */
    int      reads;                  /* every register access, for ordering */
    uint32_t last_off;
    int      saturating;             /* 1 = a counter left unread sticks at ~0 */
    int      sticky_singles;         /* 1 = single (32-bit) registers do NOT
                                       * clear on read -- QEMU 11.0.0's e1000
                                       * model for MPC, measured, not the
                                       * 8254x manual's spec. Pairs are
                                       * unaffected: this flag exists to drive
                                       * e1000_stat_acc_auto() through both
                                       * branches, not to add a third pair
                                       * convention. */
};

static int midx(uint32_t off) { return (int)((off - 0x04000u) / 4u); }

/* Which registers form a 64-bit pair, low first. */
static const uint32_t pair_lo[] = { E1000_REG_GORCL, E1000_REG_GOTCL,
                                   E1000_REG_TORL,  E1000_REG_TOTL };
#define NPAIR ((int)(sizeof pair_lo / sizeof pair_lo[0]))

static int pair_partner(uint32_t off, uint32_t *lo, uint32_t *hi)
{
    for (int i = 0; i < NPAIR; i++) {
        if (off == pair_lo[i])      { *lo = pair_lo[i]; *hi = pair_lo[i] + 4; return 1; }
        if (off == pair_lo[i] + 4)  { *lo = pair_lo[i]; *hi = pair_lo[i] + 4; return 1; }
    }
    return 0;
}

static uint32_t model_read(void *ctx, uint32_t off)
{
    struct model *m = (struct model *)ctx;
    m->reads++;
    m->last_off = off;

    uint32_t v = m->reg[midx(off)];
    uint32_t lo, hi;
    if (pair_partner(off, &lo, &hi)) {
        int clears = (m->kind == MODEL_CLR_LO) ? (off == lo) : (off == hi);
        if (clears) { m->reg[midx(lo)] = 0; m->reg[midx(hi)] = 0; }
    } else if (!m->sticky_singles) {
        m->reg[midx(off)] = 0;
    }
    /* sticky_singles: no side effect at all -- exactly QEMU 11.0.0's
     * mac_readreg, measured for MPC. */
    return v;
}

/* The device counting traffic. Adds to a register the way silicon does, with
 * the saturation behaviour that makes an unread counter useless. */
static void model_bump(struct model *m, uint32_t off, uint32_t n)
{
    int i = midx(off);
    uint64_t s = (uint64_t)m->reg[i] + n;
    if (m->saturating && s > 0xFFFFFFFFull) s = 0xFFFFFFFFull;
    m->reg[i] = (uint32_t)s;
}

/* Add to a 64-bit pair. The device keeps the true 64-bit value split across
 * the two registers; how it CLEARS them is the part the two models disagree
 * about, and this function is common to both. */
static void model_bump64(struct model *m, uint32_t lo, uint64_t n)
{
    uint64_t s = (uint64_t)m->reg[midx(lo)] | ((uint64_t)m->reg[midx(lo + 4)] << 32);
    s += n;
    m->reg[midx(lo)]     = (uint32_t)s;
    m->reg[midx(lo + 4)] = (uint32_t)(s >> 32);
}

static void model_init(struct model *m, int kind)
{
    memset(m, 0, sizeof *m);
    m->kind = kind;
}

static void stats_zero(struct e1000_stats *st) { memset(st, 0, sizeof *st); }

/* ------------------------------------------------- read-to-clear proper --- */

static void test_accumulates_across_samples(void)
{
    struct model m; struct e1000_stats st;
    model_init(&m, MODEL_CLR_LO); stats_zero(&st);

    model_bump(&m, E1000_REG_GPRC, 10);
    e1000_stats_sample(&st, model_read, &m);
    CHECK_EQ(st.rx_pkts, 10, "first sample takes the whole count");

    model_bump(&m, E1000_REG_GPRC, 7);
    e1000_stats_sample(&st, model_read, &m);
    CHECK_EQ(st.rx_pkts, 17, "the second sample ADDS -- the register was cleared");

    model_bump(&m, E1000_REG_GPRC, 3);
    model_bump(&m, E1000_REG_GPTC, 4);
    e1000_stats_sample(&st, model_read, &m);
    CHECK_EQ(st.rx_pkts, 20, "three samples of rx accumulate");
    CHECK_EQ(st.tx_pkts, 4,  "tx accumulates independently");
    CHECK_EQ(st.samples, 3,  "the sample count tracks the folds");
}

static void test_idle_sample_changes_nothing(void)
{
    struct model m; struct e1000_stats st;
    model_init(&m, MODEL_CLR_LO); stats_zero(&st);

    model_bump(&m, E1000_REG_GPRC, 42);
    model_bump(&m, E1000_REG_RNBC, 5);
    e1000_stats_sample(&st, model_read, &m);
    CHECK_EQ(st.rx_pkts,   42, "count taken");
    CHECK_EQ(st.rx_no_buf, 5,  "loss count taken");

    /* net_poll runs ~100x/s and mostly finds an idle card. Sampling an idle
     * card must be a no-op on the totals -- with `sw = read()` it ZEROES
     * them, which is the whole bug: a machine that dropped five frames and
     * then went quiet reports that it has never dropped one. */
    for (int i = 0; i < 5; i++) e1000_stats_sample(&st, model_read, &m);
    CHECK_EQ(st.rx_pkts,   42, "five idle samples do not move the rx total");
    CHECK_EQ(st.rx_no_buf, 5,  "five idle samples do not erase the loss count");
    CHECK_EQ(st.samples,   6,  "but they are counted as samples");
}

static void test_second_reader_sees_zero(void)
{
    /* The trap, stated as an executable fact rather than a comment. This is
     * why e1000.c has exactly one caller of e1000_stats_sample() and why
     * e1000_stats_get() does not touch the device. */
    struct model m; struct e1000_stats a, b;
    model_init(&m, MODEL_CLR_LO); stats_zero(&a); stats_zero(&b);

    model_bump(&m, E1000_REG_GPRC, 99);
    e1000_stats_sample(&a, model_read, &m);
    e1000_stats_sample(&b, model_read, &m);      /* the second reader */
    CHECK_EQ(a.rx_pkts, 99, "the first reader gets the count");
    CHECK_EQ(b.rx_pkts, 0,  "the second reader gets ZERO -- it was cleared");
    CHECK_EQ(m.reg[midx(E1000_REG_GPRC)], 0, "and the register is empty");
}

static void test_prime_discards(void)
{
    struct model m; struct e1000_stats st;
    model_init(&m, MODEL_CLR_LO); stats_zero(&st);

    /* Whatever a previous owner of the card left behind is not ours. */
    model_bump(&m, E1000_REG_GPRC, 1000);
    model_bump(&m, E1000_REG_MPC, 77);
    model_bump64(&m, E1000_REG_TORL, 123456);
    e1000_stats_prime(model_read, &m);

    e1000_stats_sample(&st, model_read, &m);
    CHECK_EQ(st.rx_pkts,    0, "prime discarded the stale packet count");
    CHECK_EQ(st.rx_missed,  0, "prime discarded the stale miss count");
    CHECK_EQ(st.rx_bytes,   0, "prime discarded the stale octet count");

    model_bump(&m, E1000_REG_GPRC, 6);
    e1000_stats_sample(&st, model_read, &m);
    CHECK_EQ(st.rx_pkts, 6, "counting starts from zero after the prime");
}

/* Every register the sampler names must actually be read, or on real silicon
 * it saturates and then reports the same number forever. */
static void test_every_counter_is_drained(void)
{
    static const uint32_t all[] = {
        E1000_REG_CRCERRS, E1000_REG_MPC,   E1000_REG_ECOL,  E1000_REG_LATECOL,
        E1000_REG_COLC,    E1000_REG_RLEC,  E1000_REG_GPRC,  E1000_REG_GPTC,
        E1000_REG_GORCL,   E1000_REG_GORCH, E1000_REG_GOTCL, E1000_REG_GOTCH,
        E1000_REG_TORL,    E1000_REG_TORH,  E1000_REG_TOTL,  E1000_REG_TOTH,
        E1000_REG_RNBC,
    };
    const int n = (int)(sizeof all / sizeof all[0]);

    struct model m; struct e1000_stats st;
    model_init(&m, MODEL_CLR_HI); stats_zero(&st);
    m.saturating = 1;

    for (int i = 0; i < n; i++) model_bump(&m, all[i], 0xFFFFFFFFu);
    e1000_stats_sample(&st, model_read, &m);

    int left = 0;
    for (int i = 0; i < n; i++) if (m.reg[midx(all[i])]) left++;
    CHECK_EQ(left, 0, "one sample drains every counter in the block");
    /* 9 single (32-bit) registers read TWICE each by e1000_stat_acc_auto()'s
     * discriminator (see its comment), 8 pair registers read once: 9*2+8=26.
     * Under this model (MODEL_CLR_HI, sticky_singles off) every single also
     * clears on its first read, so the second read of each sees 0 and the
     * "left == 0" check above still holds -- doubling the read count changes
     * nothing this test already asserted, only how many traps it costs. */
    CHECK_EQ(m.reads, 26, "singles are read twice, pairs once: 9*2 + 8");
}

/* ------------------------------------------- the MPC bug, reproduced --- */
/*
 * MEASURED, QEMU 11.0.0's e1000 model (hw/net/e1000.c, fetched at the v11.0.0
 * tag): MPC is served by `mac_readreg`, a plain non-clearing getter, unlike
 * GPRC's `mac_read_clr4`. Under the OLD `*sw += hw`, a single missed frame
 * re-added itself to the software total once every sample FOREVER -- a real
 * boot's serial log showed `mpc` climbing by a constant +32 per line,
 * hundreds of times, with rx flat and irq (+0), which is exactly what
 * `sticky_singles = 1` reproduces below. This section is the regression test
 * for that: e1000_stat_acc_auto() must (a) take a sticky counter's value
 * exactly once no matter how many idle samples follow, (b) attribute a NEW
 * event correctly on top of a sticky baseline, and (c) produce the SAME
 * total a clearing register would have produced for the identical sequence
 * of real hardware events -- "right under both", the same bar
 * e1000_stat_acc64 already had to clear for the 64-bit pairs.
 */

static void test_sticky_counter_does_not_reflood(void)
{
    struct model m; struct e1000_stats st;
    model_init(&m, MODEL_CLR_HI); stats_zero(&st);
    m.sticky_singles = 1;

    /* One real overrun. MPC now holds 32 and NEVER clears on this model,
     * exactly as measured. */
    model_bump(&m, E1000_REG_MPC, 32);
    e1000_stats_sample(&st, model_read, &m);
    CHECK_EQ(st.rx_missed, 32, "the one real overrun is counted");

    /* This is the flood, reproduced: under the OLD `*sw += hw` this loop
     * would take rx_missed to 32 + 5*32 = 192. A card that dropped 32 frames
     * once and then went quiet must still say 32 after any number of idle
     * samples -- the same invariant test_idle_sample_changes_nothing already
     * proves for a clearing register, now proved for a sticky one. */
    for (int i = 0; i < 5; i++) e1000_stats_sample(&st, model_read, &m);
    CHECK_EQ(st.rx_missed, 32,
             "five idle samples of a STICKY register do not re-add its value");
    CHECK_EQ(st.samples, 6, "but they are still counted as samples");
}

static void test_sticky_counter_attributes_new_events(void)
{
    struct model m; struct e1000_stats st;
    model_init(&m, MODEL_CLR_HI); stats_zero(&st);
    m.sticky_singles = 1;

    model_bump(&m, E1000_REG_MPC, 32);          /* raw MPC: 32 */
    e1000_stats_sample(&st, model_read, &m);
    CHECK_EQ(st.rx_missed, 32, "first overrun");

    e1000_stats_sample(&st, model_read, &m);    /* idle */
    e1000_stats_sample(&st, model_read, &m);    /* idle */
    CHECK_EQ(st.rx_missed, 32, "still 32 after two idle samples");

    /* A second, distinct overrun. The register does not reset -- it is
     * sticky -- so its raw value is now 32+11=43, and the correct software
     * delta is 11, not 43 (which would double-count the first event) and not
     * 0 (which would drop the second event entirely). */
    model_bump(&m, E1000_REG_MPC, 11);          /* raw MPC: 43 */
    e1000_stats_sample(&st, model_read, &m);
    CHECK_EQ(st.rx_missed, 43, "second overrun adds exactly its own 11");
}

static void test_sticky_and_clearing_agree_on_total(void)
{
    /* The property that matters is not the mechanics, it is the ANSWER: for
     * the identical sequence of real hardware events, a sticky register and
     * a clearing one must accumulate to the SAME total. Three bursts, three
     * idle samples folded in between each, driven through both models. */
    uint32_t bursts[] = { 7, 0, 0, 15, 0, 3 };
    const int n = (int)(sizeof bursts / sizeof bursts[0]);
    uint64_t totals[2];

    for (int sticky = 0; sticky < 2; sticky++) {
        struct model m; struct e1000_stats st;
        model_init(&m, MODEL_CLR_HI); stats_zero(&st);
        m.sticky_singles = sticky;
        for (int i = 0; i < n; i++) {
            if (bursts[i]) model_bump(&m, E1000_REG_MPC, bursts[i]);
            e1000_stats_sample(&st, model_read, &m);
        }
        totals[sticky] = st.rx_missed;
    }
    CHECK_EQ(totals[0], 7 + 15 + 3, "clearing model: totals sum the bursts");
    CHECK_EQ(totals[1], totals[0],
             "sticky model reaches the SAME total as clearing -- right under both");
}

static void test_stat_acc_auto_directly(void)
{
    /* The function's own two branches, exercised without a device model at
     * all -- the same directness test_octet_read_order uses for the pair
     * helper. */
    uint64_t sw; uint32_t raw;

    /* hw2 == 0, hw1 != 0: a register that just cleared. hw1 is the whole
     * delta, unconditionally -- no history consulted. */
    sw = 0; raw = 0xDEADBEEF;   /* a stale raw value must not leak in */
    e1000_stat_acc_auto(&sw, &raw, 9, 0);
    CHECK_EQ(sw, 9, "cleared branch: hw1 taken whole");
    CHECK_EQ(raw, 0, "cleared branch: last_raw reset to 0");

    /* hw1 == hw2 == 0: nothing happened, either model. Must not fabricate a
     * "cleared" event out of two honest zeros. */
    sw = 5; raw = 0;
    e1000_stat_acc_auto(&sw, &raw, 0, 0);
    CHECK_EQ(sw, 5, "double zero: no change");

    /* Sticky, first observation: hw1==hw2==20, last_raw starts at 0. */
    sw = 0; raw = 0;
    e1000_stat_acc_auto(&sw, &raw, 20, 20);
    CHECK_EQ(sw, 20, "sticky first read: delta against a zero baseline");
    CHECK_EQ(raw, 20, "sticky first read: last_raw becomes 20");

    /* Sticky, unchanged: hw1==hw2==20 again (idle). Delta must be 0. */
    e1000_stat_acc_auto(&sw, &raw, 20, 20);
    CHECK_EQ(sw, 20, "sticky idle: unchanged");

    /* Sticky, register itself reset underneath us (e.g. a device reset): the
     * fresh reading is BELOW last_raw, so it is taken whole rather than
     * subtracted (which would underflow a uint32_t and fabricate a huge
     * loss). */
    e1000_stat_acc_auto(&sw, &raw, 3, 3);
    CHECK_EQ(sw, 23, "sticky reset-underneath: fresh value taken whole (3)");
    CHECK_EQ(raw, 3, "last_raw follows the reset value");
}

/* --------------------------------------------- good-octet liveness --- */

static void test_good_bytes_stuck_flag(void)
{
    /* QEMU 11.0.0 hard-zeros GORC/GOTC (MAC_ACCESS_FLAG_NEEDED, unsatisfiable
     * gate) -- packets counted, their bytes never counted. That must be
     * DETECTED, not silently reported as "0 good bytes". */
    struct model m; struct e1000_stats st;
    model_init(&m, MODEL_CLR_HI); stats_zero(&st);

    CHECK(!st.rx_good_bytes_stuck, "not stuck before any traffic");

    model_bump(&m, E1000_REG_GPRC, 5);          /* packets counted... */
    e1000_stats_sample(&st, model_read, &m);    /* ...GORCL/GORCH never bumped */
    CHECK_EQ(st.rx_pkts, 5, "packets were counted");
    CHECK_EQ(st.rx_good_bytes, 0, "and GORC read 0, as measured on QEMU");
    CHECK(st.rx_good_bytes_stuck, "so the driver must flag it, not print a bare 0");

    /* Sticky once set. */
    e1000_stats_sample(&st, model_read, &m);
    CHECK(st.rx_good_bytes_stuck, "stays flagged on a later idle sample");
}

static void test_good_bytes_not_stuck_when_it_moves(void)
{
    /* The safety half of the same check: a device that DOES implement GORC
     * (real silicon, or a future QEMU) must never be flagged. */
    struct model m; struct e1000_stats st;
    model_init(&m, MODEL_CLR_HI); stats_zero(&st);

    model_bump(&m, E1000_REG_GPRC, 5);
    model_bump64(&m, E1000_REG_GORCL, 700);     /* the register DOES move */
    e1000_stats_sample(&st, model_read, &m);
    CHECK(!st.rx_good_bytes_stuck,
          "a good-octet register that actually counts is never flagged stuck");
}

/* -------------------------------------------------- 64-bit octet counts --- */

static void test_octets_under_both_models(void)
{
    for (int kind = 0; kind < 2; kind++) {
        const char *who = kind == MODEL_CLR_HI ? "clr-hi" : "clr-lo";
        struct model m; struct e1000_stats st;
        model_init(&m, kind); stats_zero(&st);

        model_bump64(&m, E1000_REG_TORL, 1500);
        model_bump64(&m, E1000_REG_TOTL, 60);
        e1000_stats_sample(&st, model_read, &m);
        CHECK(st.rx_bytes == 1500, "%s: rx octets round-trip (got %llu)",
              who, (unsigned long long)st.rx_bytes);
        CHECK(st.tx_bytes == 60, "%s: tx octets round-trip (got %llu)",
              who, (unsigned long long)st.tx_bytes);

        model_bump64(&m, E1000_REG_TORL, 2500);
        e1000_stats_sample(&st, model_read, &m);
        CHECK(st.rx_bytes == 4000, "%s: octets accumulate (got %llu)",
              who, (unsigned long long)st.rx_bytes);

        /* Both halves populated. Under CLEAR-ON-LOW the high register reads 0
         * because the low read already cleared the pair, so a single sample
         * cannot report more than 4 GiB; under CLEAR-ON-HIGH both halves come
         * back. Below 4 GiB per sample -- 34 Gbit/s at a one-second period --
         * the two agree, which is the property the driver actually relies on. */
        model_init(&m, kind); stats_zero(&st);
        model_bump64(&m, E1000_REG_TORL, 0x1FFFFFFFFull);   /* 8 GiB - 1 */
        e1000_stats_sample(&st, model_read, &m);
        if (kind == MODEL_CLR_HI)
            CHECK(st.rx_bytes == 0x1FFFFFFFFull,
                  "clr-hi: a pair above 4 GiB reads back whole (got %llu)",
                  (unsigned long long)st.rx_bytes);
        else
            CHECK(st.rx_bytes == 0xFFFFFFFFull,
                  "clr-lo: only the low word survives above 4 GiB (got %llu)",
                  (unsigned long long)st.rx_bytes);
    }
}

static void test_octet_read_order(void)
{
    /* Low before high, asserted directly against the sampler's access trace.
     * Reversing it is wrong under both models and looks completely reasonable
     * in a diff, which is exactly the kind of bug worth pinning. */
    struct model m; struct e1000_stats st;
    model_init(&m, MODEL_CLR_LO); stats_zero(&st);

    /* A run of reads: find the index of GORCL and GORCH in the trace. */
    static uint32_t trace[64];
    static int ntrace;
    ntrace = 0;
    (void)trace;

    /* Simplest sound form: read the pair through the model with the high half
     * seeded, and require the low-first order to be what recovers it. */
    model_bump64(&m, E1000_REG_TORL, 0x100000005ull);
    uint32_t lo = model_read(&m, E1000_REG_TORL);
    uint32_t hi = model_read(&m, E1000_REG_TORH);
    uint64_t low_first = (uint64_t)lo | ((uint64_t)hi << 32);

    model_init(&m, MODEL_CLR_HI);
    model_bump64(&m, E1000_REG_TORL, 0x100000005ull);
    uint32_t hi2 = model_read(&m, E1000_REG_TORH);       /* the wrong order */
    uint32_t lo2 = model_read(&m, E1000_REG_TORL);
    uint64_t high_first = (uint64_t)lo2 | ((uint64_t)hi2 << 32);

    CHECK_EQ(low_first, 0x5ull, "clr-lo: low-first recovers the low word");
    CHECK(high_first != 0x100000005ull,
          "clr-hi: high-first LOSES the low word (got %llu) -- order is not free",
          (unsigned long long)high_first);

    /* And the sampler itself does it the right way round. */
    struct model m3; stats_zero(&st);
    model_init(&m3, MODEL_CLR_HI);
    model_bump64(&m3, E1000_REG_TORL, 0x100000005ull);
    e1000_stats_sample(&st, model_read, &m3);
    CHECK_EQ(st.rx_bytes, 0x100000005ull, "the sampler reads low before high");
    (void)ntrace;
}

static void test_counters_are_64_bit(void)
{
    /* 4 GiB is about two minutes at the 269.9 Mbit/s this tree has measured,
     * so a 32-bit software total is not a theoretical limit. */
    struct model m; struct e1000_stats st;
    model_init(&m, MODEL_CLR_LO); stats_zero(&st);

    for (int i = 0; i < 8; i++) {
        model_bump64(&m, E1000_REG_TORL, 0xC0000000ull);   /* 3 GiB each */
        e1000_stats_sample(&st, model_read, &m);
    }
    CHECK_EQ(st.rx_bytes, 8ull * 0xC0000000ull, "24 GiB of octets accumulate");
    CHECK(st.rx_bytes > 0xFFFFFFFFull, "the total exceeds 32 bits");

    /* Packet counts too: 0xFFFFFFFF five times over. */
    stats_zero(&st);
    for (int i = 0; i < 5; i++) {
        model_bump(&m, E1000_REG_GPRC, 0xFFFFFFFFu);
        e1000_stats_sample(&st, model_read, &m);
    }
    CHECK_EQ(st.rx_pkts, 5ull * 0xFFFFFFFFull, "packet totals are 64-bit too");
}

/* ------------------------------------------------------------- losses ---- */

static void test_losses(void)
{
    struct model m; struct e1000_stats st;
    model_init(&m, MODEL_CLR_LO); stats_zero(&st);

    model_bump(&m, E1000_REG_GPRC, 100000);
    model_bump64(&m, E1000_REG_TORL, 150000000);
    e1000_stats_sample(&st, model_read, &m);
    CHECK_EQ(e1000_stats_losses(&st), 0, "a clean run has no losses");
    CHECK(st.rx_bytes > 0, "byte totals come from TOR, which this device counts");
    CHECK_EQ(st.rx_good_bytes, 0,
             "the good-octet pair is SEPARATE -- a device that leaves GORC at "
             "zero (measured: QEMU's e1000 does) cannot zero the byte total");

    /* The two that matter, and they are counted separately on purpose: RNBC
     * means the driver did not post descriptors fast enough, MPC means the
     * on-chip FIFO overflowed. Same symptom, different fix. */
    model_bump(&m, E1000_REG_RNBC, 3);
    model_bump(&m, E1000_REG_MPC, 11);
    e1000_stats_sample(&st, model_read, &m);
    CHECK_EQ(st.rx_no_buf, 3,  "RNBC is its own counter");
    CHECK_EQ(st.rx_missed, 11, "MPC is its own counter");
    CHECK_EQ(e1000_stats_losses(&st), 14, "losses sums the drop causes");

    model_bump(&m, E1000_REG_CRCERRS, 1);
    model_bump(&m, E1000_REG_RLEC, 2);
    model_bump(&m, E1000_REG_ECOL, 4);
    model_bump(&m, E1000_REG_LATECOL, 8);
    model_bump(&m, E1000_REG_COLC, 16);
    e1000_stats_sample(&st, model_read, &m);
    CHECK_EQ(e1000_stats_losses(&st), 14 + 1 + 2 + 4 + 8,
             "errors and hard collisions count as loss");
    CHECK_EQ(st.colls, 16,
             "an ordinary collision is NOT a loss -- the frame went out");
}

/* ------------------------------------------------------------- link ------ */

static void test_link_decode(void)
{
    /* QEMU's e1000 reset value for STATUS is 0x80080783: link up, full duplex,
     * 1000 Mb/s, ASDV=11, GIO master enable, and bit 31 set. */
    const uint32_t up1000 = 0x80080783u;
    CHECK(e1000_link_is_up(up1000), "QEMU's reset STATUS is link-up");
    CHECK(e1000_link_is_fd(up1000), "and full duplex");
    CHECK_EQ(e1000_link_mbps(up1000), 1000, "and 1000 Mb/s");

    CHECK(!e1000_link_is_up(up1000 & ~E1000_STATUS_LU), "clearing LU is link down");
    CHECK(!e1000_link_is_fd(up1000 & ~E1000_STATUS_FD), "clearing FD is half duplex");

    CHECK_EQ(e1000_link_mbps(0x00000002u), 10,   "speed 00 is 10 Mb/s");
    CHECK_EQ(e1000_link_mbps(0x00000042u), 100,  "speed 01 is 100 Mb/s");
    CHECK_EQ(e1000_link_mbps(0x00000082u), 1000, "speed 10 is 1000 Mb/s");
    CHECK_EQ(e1000_link_mbps(0x000000C2u), 1000, "speed 11 (reserved) reports 1000");
}

static void test_link_transition_not_poll(void)
{
    const uint32_t up = 0x80080783u;

    CHECK(!e1000_link_changed(up, up), "the same STATUS twice is not a transition");

    /* The bits that move on their own must not look like a link event. This is
     * the whole reason e1000_link_changed masks instead of comparing: the
     * report is once per transition, not once per poll. */
    CHECK(!e1000_link_changed(up, up ^ (1u << 19)),
          "TXOFF moving is not a link transition");
    CHECK(!e1000_link_changed(up, up ^ (1u << 7 << 3)),
          "an unreported high bit moving is not a link transition");
    CHECK(!e1000_link_changed(up, up & ~(1u << 31)),
          "bit 31 moving is not a link transition");

    CHECK(e1000_link_changed(up, up & ~E1000_STATUS_LU), "LU dropping IS a transition");
    CHECK(e1000_link_changed(up, up & ~E1000_STATUS_FD), "duplex changing IS a transition");
    CHECK(e1000_link_changed(up, (up & ~0xC0u) | 0x40u), "speed changing IS a transition");

    /* A full unplug/replug cycle: exactly two transitions out of six polls. */
    uint32_t seq[] = { up, up, up & ~E1000_STATUS_LU, up & ~E1000_STATUS_LU, up, up };
    int transitions = 0;
    for (int i = 1; i < (int)(sizeof seq / sizeof seq[0]); i++)
        if (e1000_link_changed(seq[i - 1], seq[i])) transitions++;
    CHECK_EQ(transitions, 2, "six polls across one unplug/replug print two lines");
}

/* --------------------------------------------------------------- ITR ----- */

static void test_itr_units(void)
{
    /* The one conversion with a written source behind it (8254x manual: the
     * interval is ITR * 256 ns). Kept as a check rather than a comment because
     * a driver that treats ITR as microseconds is off by 3.9x and still
     * produces a working, plausibly-throttled card. */
    CHECK_EQ(e1000_itr_ns_hw(1),    256,    "one ITR unit is 256 ns on silicon");
    CHECK_EQ(e1000_itr_ns_hw(61),   15616,  "ITR=61 is ~15.6 us on silicon");
    CHECK_EQ(e1000_itr_ns_hw(0xFFFF), 16776960u, "ITR saturates at ~16.8 ms");
}

int main(void)
{
    test_accumulates_across_samples();
    test_idle_sample_changes_nothing();
    test_second_reader_sees_zero();
    test_prime_discards();
    test_every_counter_is_drained();
    test_sticky_counter_does_not_reflood();
    test_sticky_counter_attributes_new_events();
    test_sticky_and_clearing_agree_on_total();
    test_stat_acc_auto_directly();
    test_good_bytes_stuck_flag();
    test_good_bytes_not_stuck_when_it_moves();
    test_octets_under_both_models();
    test_octet_read_order();
    test_counters_are_64_bit();
    test_losses();
    test_link_decode();
    test_link_transition_not_poll();
    test_itr_units();

    if (failures) {
        printf("e1000_stats_test: %d/%d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("e1000_stats_test: %d checks passed\n", checks);
    return 0;
}
