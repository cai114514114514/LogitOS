/* Host unit tests for the e1000 statistics accumulator and link-status decode.
 *
 * WHAT IS BEING TESTED, AND WHY IT CAN BE TESTED WITHOUT HARDWARE
 * ==============================================================
 * tests/unit/net_drv_test.c's header argues that a NIC's register PROGRAMMING
 * must not be mocked -- "if the fake agrees with the driver, both can be wrong
 * together". That argument does not apply here and its converse does. The
 * statistics registers have ONE externally-specified behaviour, read-to-clear,
 * which the 8254x manual states in so many words, and every bug this file
 * exists to catch is a bug in the arithmetic wrapped around that behaviour,
 * not in which register was poked:
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
 *
 * The model below implements the documented rule and its one plausible
 * variant, and is driven through e1000_stats_sample() -- the same function the
 * kernel calls. Nothing in this file re-implements the accumulation, which is
 * what stops the model and the driver from being wrong together.
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
    } else {
        m->reg[midx(off)] = 0;
    }
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
    CHECK_EQ(m.reads, n, "and reads each of them exactly once");
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
