/* thrtest -- the M30 threads gate. Prints THREAD_TEST_OK only if all of it holds.
 *
 * WHAT THIS HAS TO PROVE, and why each one is measured rather than asserted:
 *
 *  1. TWO THREADS, ONE ADDRESS SPACE, PROVABLY CONCURRENT -- BY WALL CLOCK.
 *     "No corruption" is not a concurrency proof: a build where pthread_create
 *     silently ran the function inline would pass every corruption check ever
 *     written. c/apps/coreutils/smptest.c set the standard for this machine --
 *     N workers finishing in well under N*T1 is a thing that cannot happen if
 *     they serialised -- and this is the same measurement for threads.
 *     The work is deliberately PURE RING-3 ARITHMETIC with no syscall in it,
 *     because two threads inside the kernel do NOT run in parallel here: the
 *     big kernel lock serialises them. Timing syscall throughput would measure
 *     the BKL and report it as a threading failure.
 *
 *  2. TLS IS ACTUALLY PER-THREAD. Two threads write the same `__thread`
 *     variable and each must read back its own value -- and the check is done
 *     AFTER both have written, with a barrier in between, so a build where all
 *     threads shared one copy fails rather than racing to look right.
 *
 *  3. A MUTEX THAT ACTUALLY EXCLUDES. Four threads increment a shared counter
 *     with a non-atomic read-modify-write under one mutex. The final value is
 *     exact if and only if the mutex worked; the read-modify-write is split by
 *     a deliberate yield so a broken mutex loses counts on every machine rather
 *     than only on a lucky one.
 *
 *  4. JOIN DELIVERS THE EXIT VALUE, DETACH FREES WITHOUT A JOIN, AND NEITHER
 *     LEAKS. The leak check is the kernel's own descriptor count across
 *     thousands of create/join cycles, read through SYS_THREAD_INFO -- because
 *     "it did not crash" is not a leak check, and a table that fills up is
 *     invisible until it does.
 *
 * NEGATIVE CONTROLS. Each of the four is built into this file behind a -D, and
 * tests/boot/run-thread-negctl.sh REQUIRES the build to FAIL:
 *     THR_NEGCTL_SERIAL   threads created and joined one at a time -> no speedup
 *     THR_NEGCTL_TLS      the per-thread value read from a shared global
 *     THR_NEGCTL_NOLOCK   the mutex removed from the counter
 *     THR_NEGCTL_LEAK     detach never called, so descriptors are never freed
 * An assertion nobody has watched fail is not a known-failing assertion.
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "logit_abi.h"

static long sys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

static long mono_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int fails;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

/* ---------------------------------------------------------------------------
 * 1. Concurrency, by wall clock.
 *
 * The load is integer-only and touches nothing shared, so the only thing that
 * can make four of them take as long as one is a scheduler that ran them one
 * after another. Volatile so -O2 cannot fold the loop away -- an optimiser that
 * deleted the work would produce a speedup ratio of "instant vs instant" and a
 * test that passes for the wrong reason.
 * ------------------------------------------------------------------------- */
#define SPIN_ROUNDS 2200000u

static volatile unsigned long spin_sink;

static void *burn(void *arg)
{
    unsigned long acc = (unsigned long)(long)arg;
    for (unsigned i = 0; i < SPIN_ROUNDS; i++)
        acc = acc * 6364136223846793005ul + 1442695040888963407ul;
    spin_sink += acc;
    return (void *)acc;
}

static long timed_batch(int n)
{
    pthread_t th[8];
    long t0 = mono_ms();
#ifdef THR_NEGCTL_SERIAL
    /* THE NEGATIVE CONTROL: create and join one at a time. Everything else about
     * the program is identical -- same threads, same work, same joins -- and the
     * speedup assertion must fail. */
    for (int i = 0; i < n; i++) {
        if (pthread_create(&th[0], 0, burn, (void *)(long)(i + 1)) != 0) return -1;
        pthread_join(th[0], 0);
    }
#else
    for (int i = 0; i < n; i++)
        if (pthread_create(&th[i], 0, burn, (void *)(long)(i + 1)) != 0) return -1;
    for (int i = 0; i < n; i++) pthread_join(th[i], 0);
#endif
    return mono_ms() - t0;
}

/* ---------------------------------------------------------------------------
 * 2. Thread-local storage.
 * ------------------------------------------------------------------------- */
static __thread unsigned long tls_value;

/* NOT static, and that is the whole point of the variable.
 *
 * This is the one that proves the INITIAL IMAGE reaches a new thread -- i.e.
 * that .tdata is copied into the block and not merely zeroed, which .tbss alone
 * could never distinguish. It was `static` at first, and clang -O2 deleted it:
 * a static __thread object that no visible code writes has the same value in
 * every thread by construction, so every read folds to the constant and the
 * variable never reaches .tdata at all. The check then passed against a compile
 * time constant and measured nothing.
 *
 * External linkage takes that proof away from the compiler (there is no LTO
 * here, so another TU might write it), and main() does write it once below so
 * the assumption cannot come back by another route. */
__thread unsigned long tls_initialised = 0xABCDEF01ul;
static unsigned long tls_shared_impostor;

static pthread_mutex_t bar_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  bar_c = PTHREAD_COND_INITIALIZER;
static int             bar_count, bar_gen;

/* A real barrier over the mutex+condvar pair, so this doubles as the condition
 * variable's own test: if cond_wait lost a wakeup, every thread would hang here
 * and the harness would time out rather than print a wrong answer. */
static void barrier(int n)
{
    pthread_mutex_lock(&bar_m);
    int gen = bar_gen;
    if (++bar_count == n) { bar_count = 0; bar_gen++; pthread_cond_broadcast(&bar_c); }
    else while (gen == bar_gen) pthread_cond_wait(&bar_c, &bar_m);
    pthread_mutex_unlock(&bar_m);
}

#define NTLS 4
static unsigned long tls_seen[NTLS];
static unsigned long tls_init_seen[NTLS];

static void *tls_worker(void *arg)
{
    int idx = (int)(long)arg;
    unsigned long mine = 0x1000ul + (unsigned long)idx;

    tls_init_seen[idx] = tls_initialised;    /* before anything overwrites it */
    tls_value = mine;
    tls_shared_impostor = mine;

    /* EVERY thread writes BEFORE any thread reads. Without this the test would
     * pass on a broken build whenever the threads happened not to overlap. */
    barrier(NTLS);

#ifdef THR_NEGCTL_TLS
    tls_seen[idx] = tls_shared_impostor;     /* the control: one shared copy */
#else
    tls_seen[idx] = tls_value;
#endif
    return 0;
}

/* ---------------------------------------------------------------------------
 * 3. Mutual exclusion under contention.
 * ------------------------------------------------------------------------- */
#define NLOCK   4
#define NBUMP   6000
static pthread_mutex_t counter_m = PTHREAD_MUTEX_INITIALIZER;
static volatile long   counter;

static void *bumper(void *arg)
{
    (void)arg;
    for (int i = 0; i < NBUMP; i++) {
#ifndef THR_NEGCTL_NOLOCK
        pthread_mutex_lock(&counter_m);
#endif
        /* Split read-modify-write with a yield in the middle. A missing mutex
         * then loses counts on ANY machine, not just on one where the timing
         * happens to interleave -- which is the difference between a negative
         * control that fails and one that fails sometimes. */
        long v = counter;
        sched_yield();
        counter = v + 1;
#ifndef THR_NEGCTL_NOLOCK
        pthread_mutex_unlock(&counter_m);
#endif
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * 4. Return values, detach, and the leak check.
 * ------------------------------------------------------------------------- */
static void *ret_worker(void *arg) { return (void *)((long)arg * 7 + 3); }

static volatile int detached_done;
static void *detached_worker(void *arg)
{
    (void)arg;
    __atomic_add_fetch(&detached_done, 1, __ATOMIC_SEQ_CST);
    return 0;
}

int main(void)
{
    printf("thrtest: start\n");

    /* --- 1. concurrency, by wall clock ---------------------------------- */
    long t1 = timed_batch(1);
    long t4 = timed_batch(4);
    printf("thrtest: T1=%ldms T4=%ldms sink=%lu\n", t1, t4, spin_sink);
    check(t1 > 150, "baseline long enough to time (>150ms)");
    /* Four threads on four cores. Require T4 < 2*T1 -- a wide margin against
     * TCG jitter, and still impossible if they serialised (that is T4 ~= 4*T1). */
    check(t1 > 0 && t4 < 2 * t1, "4 threads finished in <2x the time of 1 (genuine parallelism)");

    /* --- 2. TLS ---------------------------------------------------------- */
    {
        pthread_t th[NTLS];
        for (int i = 0; i < NTLS; i++)
            if (pthread_create(&th[i], 0, tls_worker, (void *)(long)i) != 0) {
                printf("THREAD_TEST_FAIL: pthread_create (tls)\n"); return 1;
            }
        for (int i = 0; i < NTLS; i++) pthread_join(th[i], 0);

        int distinct = 1, initialised = 1;
        for (int i = 0; i < NTLS; i++) {
            if (tls_seen[i] != 0x1000ul + (unsigned long)i) distinct = 0;
            if (tls_init_seen[i] != 0xABCDEF01ul) initialised = 0;
        }
        printf("thrtest: tls seen = %lu %lu %lu %lu\n",
               tls_seen[0], tls_seen[1], tls_seen[2], tls_seen[3]);
        check(distinct, "__thread is per-thread (each thread read back its own value)");
        /* Writing it here is what stops the compiler folding the reads above
         * back into a constant; see the declaration. */
        tls_initialised = 0;
        check(initialised, "__thread initialisers reached every thread (.tdata copied)");
        check(tls_value == 0, "the main thread's own copy was untouched by any of them");
    }

    /* --- 3. mutual exclusion --------------------------------------------- */
    {
        pthread_t th[NLOCK];
        counter = 0;
        for (int i = 0; i < NLOCK; i++)
            if (pthread_create(&th[i], 0, bumper, 0) != 0) {
                printf("THREAD_TEST_FAIL: pthread_create (mutex)\n"); return 1;
            }
        for (int i = 0; i < NLOCK; i++) pthread_join(th[i], 0);
        printf("thrtest: counter=%ld want=%d\n", counter, NLOCK * NBUMP);
        check(counter == (long)NLOCK * NBUMP, "mutex excluded (counter is exact)");
    }

    /* --- 4a. join delivers the exit value --------------------------------- */
    {
        int ok = 1;
        for (long i = 1; i <= 8; i++) {
            pthread_t t; void *r = 0;
            if (pthread_create(&t, 0, ret_worker, (void *)i) != 0) { ok = 0; break; }
            if (pthread_join(t, &r) != 0) { ok = 0; break; }
            if ((long)r != i * 7 + 3) { ok = 0; break; }
        }
        check(ok, "join returned each thread's exit value");
    }

    /* --- 4b. detach frees without a join ---------------------------------- */
    {
        long before = sys(SYS_THREAD_INFO, THRINFO_SLOTS, 0, 0);
        detached_done = 0;
        int made = 0;
        for (int i = 0; i < 16; i++) {
            pthread_t t;
            if (pthread_create(&t, 0, detached_worker, 0) != 0) break;
#ifndef THR_NEGCTL_LEAK
            pthread_detach(t);
#endif
            made++;
        }
        /* Wait for them to finish, then let the last exits settle. */
        for (int i = 0; i < 500 && __atomic_load_n(&detached_done, __ATOMIC_SEQ_CST) < made; i++)
            usleep(10000);
        usleep(200000);
        long after = sys(SYS_THREAD_INFO, THRINFO_SLOTS, 0, 0);
        printf("thrtest: detach slots %ld -> %ld (%d threads, %d ran)\n",
               before, after, made, detached_done);
        check(made == 16 && detached_done == 16, "16 detached threads all ran");
        check(after <= before, "detached threads freed their descriptors with no join");
    }

    /* --- 4c. the leak check ----------------------------------------------- */
    {
        long slots0 = sys(SYS_THREAD_INFO, THRINFO_SLOTS, 0, 0);
        const int CYCLES = 2000;
        int ok = 1;
        for (int i = 0; i < CYCLES; i++) {
            pthread_t t; void *r = 0;
            if (pthread_create(&t, 0, ret_worker, (void *)(long)i) != 0) { ok = 0; break; }
            if (pthread_join(t, &r) != 0) { ok = 0; break; }
        }
        long slots1 = sys(SYS_THREAD_INFO, THRINFO_SLOTS, 0, 0);
        long created = sys(SYS_THREAD_INFO, THRINFO_CREATED, 0, 0);
        long reaped  = sys(SYS_THREAD_INFO, THRINFO_REAPED, 0, 0);
        printf("thrtest: %d create/join cycles, slots %ld -> %ld, created=%ld reaped=%ld\n",
               CYCLES, slots0, slots1, created, reaped);
        check(ok, "2000 create/join cycles all succeeded");
        /* The real assertion. A one-descriptor-per-thread leak would have needed
         * 2000 slots out of a table of 128, so this cannot pass by luck: without
         * the free it stops at cycle ~120 with THR_E_FULL, which is what the
         * `ok` check above catches, and this one proves the table came back
         * DOWN rather than merely never filling. */
        check(slots1 <= slots0, "the descriptor table returned to its starting size");
    }

    /* --- what is refused, verified as refused ----------------------------- */
    {
        check(pthread_cancel(pthread_self()) != 0, "pthread_cancel is refused, not faked");
        check(pthread_kill(pthread_self(), 9) != 0, "pthread_kill is refused, not faked");
    }

    if (fails) { printf("THREAD_TEST_FAIL: %d checks failed\n", fails); return 1; }
    printf("THREAD_TEST_OK\n");
    return 0;
}
