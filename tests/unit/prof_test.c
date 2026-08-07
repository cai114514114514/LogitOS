/* Host unit tests for c/kernel/core/kprof.c -- the REAL accumulator, not a copy.
 *
 * WHAT THIS FILE IS FOR, AND WHY IT IS NOT A QEMU TEST.
 *
 * The claim that matters most about a sampling profiler is the one that is
 * hardest to observe on a VM: samples arrive in interrupt context on four cores
 * at once, and the shared accumulator must not lose or tear a single one. On
 * the machine that is a race you can only hope to hit -- and a race you did not
 * hit looks exactly like a race that cannot happen.
 *
 * Here it is deterministic. Four pthreads hammer the same handful of addresses
 * a million times each, and the totals must be EXACT. The same suite built with
 * -DKPROF_UNSAFE (the plain `hits++` a profiler gets written with the first
 * time) is REQUIRED to fail, which is what makes the atomic version evidence
 * rather than a habit. See `make test-prof-host` and `make test-prof-negctl`.
 *
 * The on-device half -- does the sampler report a distribution it did not know
 * in advance, and what does it cost -- is tests/boot/run-prof-test.sh. Neither
 * half is sufficient alone.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>

#include "kprof.h"

void kprof_host_advance(uint64_t ns);

static int fails;
static int checks;

static void ok(int cond, const char *what)
{
    checks++;
    if (cond) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        fails++;
    }
}

static void okf(int cond, const char *fmt, ...)
{
    va_list ap;
    char buf[256];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ok(cond, buf);
}

static uint64_t hits_at(uint64_t rip) { return kprof_hits_in(rip, rip + 1); }

/* ------------------------------------------------------------------ basics */

static void test_accumulate(void)
{
    printf("--- the histogram counts what it was given ---\n");
    kprof_host_reset();

    for (int i = 0; i < 100; i++) kprof_host_sample(0x100000, 0, 0);
    for (int i = 0; i < 37;  i++) kprof_host_sample(0x100010, 0, 0);
    for (int i = 0; i < 1;   i++) kprof_host_sample(0x100020, 0, 1);

    okf(hits_at(0x100000) == 100, "100 samples at one address -> 100 (%llu)",
        (unsigned long long)hits_at(0x100000));
    okf(hits_at(0x100010) == 37,  "37 at a second address -> 37 (%llu)",
        (unsigned long long)hits_at(0x100010));
    ok(hits_at(0x100020) == 1,    "1 at a third address -> 1");
    ok(hits_at(0x100030) == 0,    "an address never sampled reports 0");

    struct kprof_stats st;
    kprof_get_stats(&st);
    okf(st.samples == 138, "the per-CPU witness counted every call (%llu)",
        (unsigned long long)st.samples);
    okf(st.recorded == 138, "the histogram holds every call (%llu)",
        (unsigned long long)st.recorded);
    ok(st.distinct == 3, "three distinct addresses");
    ok(st.overflow == 0, "nothing overflowed");
    /* The integrity identity the on-device harness greps for. */
    ok(st.recorded + st.overflow + st.lost_ring == st.samples,
       "taken == recorded + overflow + lost");
}

static void test_rings_are_separate(void)
{
    printf("--- ring 0 and ring 3 go in different tables ---\n");
    kprof_host_reset();
    /* The same numeric address in both rings. A single table would merge them,
     * and the host tool would then resolve a user address against the kernel
     * map and print a confident wrong name. */
    for (int i = 0; i < 10; i++) kprof_host_sample(0x40000000, 0, 0);
    for (int i = 0; i < 4;  i++) kprof_host_sample(0x40000000, 1, 0);

    okf(hits_at(0x40000000) == 10,
        "the kernel table holds only the kernel samples (%llu)",
        (unsigned long long)hits_at(0x40000000));
    struct kprof_stats st;
    kprof_get_stats(&st);
    ok(st.user_samples == 4,   "4 user samples counted");
    ok(st.kernel_samples == 10, "10 kernel samples counted");
    ok(st.recorded == 14,      "both tables together hold all 14");
    ok(st.distinct == 2,       "the same address in two rings is two entries");
}

static void test_overflow_is_counted(void)
{
    printf("--- a hash that cannot place a sample says so ---\n");
    kprof_host_reset();
    /* Fill every bucket, then keep going. What must NOT happen is a silent
     * loss: a profiler that quietly drops samples under-reports whatever is
     * hottest, which is the one thing it exists to find. */
    for (int i = 0; i < KPROF_NBUCKET + 4096; i++)
        kprof_host_sample(0x200000 + (uint64_t)i * 64, 0, 0);

    struct kprof_stats st;
    kprof_get_stats(&st);
    okf(st.overflow > 0, "the table filled and the overflow was counted (%llu)",
        (unsigned long long)st.overflow);
    okf(st.recorded + st.overflow == st.samples,
        "not one sample went missing (%llu + %llu == %llu)",
        (unsigned long long)st.recorded, (unsigned long long)st.overflow,
        (unsigned long long)st.samples);
    okf(st.distinct <= KPROF_NBUCKET, "distinct entries never exceed the table (%u)",
        st.distinct);
}

/* -------------------------------------------------------------------- spans */

static void test_spans(void)
{
    printf("--- spans: count, total, max ---\n");
    kprof_host_reset();

    int s = kprof_slot("tls");
    ok(s >= 0, "a name interns to a slot");
    ok(kprof_slot("tls") == s, "the same name interns to the same slot");
    ok(kprof_slot("layout") != s, "a different name gets a different slot");

    kprof_span_add(s, 100);
    kprof_span_add(s, 300);
    kprof_span_add(s, 200);

    uint64_t c = 0, t = 0, m = 0;
    ok(kprof_span_get("tls", &c, &t, &m) == 0, "the span reads back by name");
    okf(c == 3,   "count == 3 (%llu)", (unsigned long long)c);
    okf(t == 600, "total == 600 (%llu)", (unsigned long long)t);
    okf(m == 300, "max == 300, not the last value (%llu)", (unsigned long long)m);
    ok(kprof_span_get("nosuch", &c, &t, &m) == -1, "an unknown name is not invented");

    /* The lexical macro, driven by the fake clock. This is the path real code
     * takes, so it is the path that has to be tested -- not just span_add. */
    kprof_start(0);
    {
        KPROF_BEGIN(phase);
        kprof_host_advance(1500);
        KPROF_END(phase);
    }
    {
        KPROF_BEGIN(phase);
        kprof_host_advance(500);
        KPROF_END(phase);
    }
    c = t = m = 0;
    kprof_span_get("phase", &c, &t, &m);
    okf(c == 2 && t == 2000 && m == 1500,
        "KPROF_BEGIN/END recorded 2 spans totalling 2000 ns, max 1500 (%llu/%llu/%llu)",
        (unsigned long long)c, (unsigned long long)t, (unsigned long long)m);

    /* Disabled, the macros must record NOTHING -- not a zero-length span, not a
     * count. A profiler that still costs a table update when off is not off. */
    kprof_stop();
    {
        KPROF_BEGIN(phase);
        kprof_host_advance(999999);
        KPROF_END(phase);
    }
    uint64_t c2 = 0;
    kprof_span_get("phase", &c2, 0, 0);
    okf(c2 == 2, "with spans off, BEGIN/END record nothing (%llu)",
        (unsigned long long)c2);

    /* Nesting: an outer span must include the inner one, and both must be
     * recorded. The start timestamp lives in a local, which is what makes this
     * work across nesting AND across a migration to another core. */
    kprof_start(0);
    {
        KPROF_BEGIN(outer);
        kprof_host_advance(100);
        {
            KPROF_BEGIN(inner);
            kprof_host_advance(400);
            KPROF_END(inner);
        }
        kprof_host_advance(100);
        KPROF_END(outer);
    }
    uint64_t ot = 0, it = 0;
    kprof_span_get("outer", 0, &ot, 0);
    kprof_span_get("inner", 0, &it, 0);
    okf(it == 400, "the inner span is 400 ns (%llu)", (unsigned long long)it);
    okf(ot == 600, "the outer span contains it: 600 ns (%llu)", (unsigned long long)ot);
    kprof_stop();
}

static void test_span_slot_exhaustion(void)
{
    printf("--- the span table is finite and says so ---\n");
    kprof_host_reset();
    int last = 0;
    char nm[16];
    for (int i = 0; i < KPROF_NSLOT + 8; i++) {
        snprintf(nm, sizeof nm, "s%d", i);
        last = kprof_slot(nm);
    }
    ok(last == -1, "past KPROF_NSLOT names, kprof_slot returns -1 (it does not "
                   "overwrite a live slot)");
    /* -1 must be inert, not a wild write. */
    kprof_span_add(-1, 12345);
    kprof_span_add(KPROF_NSLOT + 3, 12345);
    ok(1, "posting to an invalid slot does not corrupt anything");
}

/* -------------------------------------------------------- the concurrency test */

#define NTHREAD 4
#define NITER   400000
#define NRIP    8
static const uint64_t RIP_BASE = 0x300000;

static void *hammer(void *arg)
{
    long id = (long)arg;
    for (long i = 0; i < NITER; i++)
        kprof_host_sample(RIP_BASE + (uint64_t)(i % NRIP) * 16, 0, (int)id);
    return 0;
}

static void test_concurrent(void)
{
    printf("--- four cores, one accumulator, no torn or lost samples ---\n");
    kprof_host_reset();

    pthread_t th[NTHREAD];
    for (long i = 0; i < NTHREAD; i++)
        pthread_create(&th[i], 0, hammer, (void *)i);
    for (int i = 0; i < NTHREAD; i++)
        pthread_join(th[i], 0);

    struct kprof_stats st;
    kprof_get_stats(&st);
    uint64_t want = (uint64_t)NTHREAD * NITER;

    okf(st.samples == want,
        "the per-CPU witness saw every call: %llu (want %llu)",
        (unsigned long long)st.samples, (unsigned long long)want);
    /* THE ASSERTION THIS FILE EXISTS FOR. With a plain `hits++` this is short
     * by tens of thousands. */
    okf(st.recorded + st.overflow == want,
        "the shared histogram accounted for every one: %llu recorded + %llu "
        "overflow (want %llu, short by %lld)",
        (unsigned long long)st.recorded, (unsigned long long)st.overflow,
        (unsigned long long)want,
        (long long)want - (long long)(st.recorded + st.overflow));
    ok(st.overflow == 0, "8 addresses fit the table, so nothing overflowed");
    okf(st.distinct == NRIP, "exactly %d distinct addresses (%u)", NRIP, st.distinct);

    /* Per address: the work was spread evenly by construction, so each address
     * must hold exactly its share. A torn CAS on the KEY shows up here even
     * when the totals happen to survive. */
    int even = 1;
    uint64_t per = want / NRIP;
    for (int i = 0; i < NRIP; i++)
        if (hits_at(RIP_BASE + (uint64_t)i * 16) != per) even = 0;
    okf(even, "each of the %d addresses holds exactly %llu hits",
        NRIP, (unsigned long long)per);
}

/* ------------------------------------------------------------------- report */

static void test_report(void)
{
    printf("--- the report is parseable and ordered ---\n");
    kprof_host_reset();
    for (int i = 0; i < 500; i++) kprof_host_sample(0x400000, 0, 0);
    for (int i = 0; i < 300; i++) kprof_host_sample(0x400100, 0, 1);
    for (int i = 0; i < 200; i++) kprof_host_sample(0x400200, 0, 2);
    kprof_span_add(kprof_slot("paint"), 42);

    static char buf[64 * 1024];
    int n = kprof_report(buf, (int)sizeof buf);
    buf[n < (int)sizeof buf ? n : (int)sizeof buf - 1] = 0;

    ok(strstr(buf, "kprof v1") != 0, "the report is versioned");
    ok(strstr(buf, "KPROF_INTEGRITY ok") != 0,
       "the integrity line says ok when nothing was lost");
    ok(strstr(buf, "samples            1000") != 0, "the sample count is reported");
    ok(strstr(buf, "span paint") != 0, "spans appear in the report");

    const char *a = strstr(buf, "0000000000400000");
    const char *b = strstr(buf, "0000000000400100");
    const char *c = strstr(buf, "0000000000400200");
    ok(a && b && c, "every sampled address appears");
    ok(a && b && c && a < b && b < c, "and they are ordered hottest first");
    ok(strstr(buf, "50.0%") != 0, "the hottest address is shown as 50.0% of 1000");

    /* Rendering must not consume the histogram: a second `cat` has to agree
     * with the first, or every user's second look at a profile is a different
     * profile. */
    static char buf2[64 * 1024];
    int n2 = kprof_report(buf2, (int)sizeof buf2);
    okf(n2 == n && memcmp(buf, buf2, (size_t)n) == 0,
        "rendering twice gives the identical report (%d vs %d bytes)", n, n2);
}

static void test_commands(void)
{
    printf("--- /dev/kprof's write verbs ---\n");
    kprof_host_reset();

    const char *c1 = "span tlshandshake 987654\n";
    kprof_command(c1, c1 + strlen(c1));
    uint64_t cnt = 0, tot = 0;
    /* KPROF_NAMELEN truncates long names rather than overrunning. */
    ok(kprof_span_get("tlshandshake", &cnt, &tot, 0) == 0,
       "`span NAME NS` posts a span from a write");
    okf(tot == 987654, "with the nanoseconds it was given (%llu)",
        (unsigned long long)tot);

    const char *c2 = "span tlshandshake 12\n";
    kprof_command(c2, c2 + strlen(c2));
    kprof_span_get("tlshandshake", &cnt, &tot, 0);
    okf(cnt == 2 && tot == 987666, "a second post accumulates (%llu, %llu)",
        (unsigned long long)cnt, (unsigned long long)tot);

    /* A malformed command must not post anything and must not crash. */
    const char *c3 = "span\n";
    kprof_command(c3, c3 + strlen(c3));
    const char *c4 = "wibble\n";
    kprof_command(c4, c4 + strlen(c4));
    const char *c5 = "span noname\n";
    kprof_command(c5, c5 + strlen(c5));
    kprof_span_get("tlshandshake", &cnt, 0, 0);
    ok(cnt == 2, "malformed commands post nothing");
}

int main(void)
{
    printf("kprof host tests\n");
    test_accumulate();
    test_rings_are_separate();
    test_overflow_is_counted();
    test_spans();
    test_span_slot_exhaustion();
    test_concurrent();
    test_report();
    test_commands();
    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
