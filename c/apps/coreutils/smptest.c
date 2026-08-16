/* smptest -- M25 SMP concurrency proof (P0 scheduler + P1 BKL-free kheap).
 *
 * Each worker child hammers the kernel heap via SYS_KHEAP_STRESS, a BKL-FREE
 * syscall (M25 P1): it runs WITHOUT the Big Kernel Lock, so N children alloc/free
 * on N cores AT THE SAME TIME, exercising kheap's own lock (g_kheap_lock) under
 * real contention. The syscall stamps a per-call tag (from a per-child seed) into
 * every byte of each block and verifies it before freeing -- a freelist race that
 * hands one block to two cores makes the tags clash, returned as a nonzero
 * corruption count. So:
 *   - every child's count == 0  => concurrent kmalloc/kfree did not corrupt.
 *   - the N-batch ran on >=2 cores AND TN < 1.6*T1  => genuine BKL-free parallelism
 *     (if the syscall still took the BKL, the N children would serialize: TN~=N*T1).
 * The stress uses volatile byte access (no XMM) so the QEMU MTTCG-on-Apple-Silicon
 * FP artifact cannot false-flag it. Prints SMP_TEST_OK on success.
 */
#include "clib.h"

#define NCHILD_MAX 8
#define KS_SIZE    512          /* bytes per block (the per-byte fill/verify is the
                                 * concurrent, un-locked work that yields the speedup) */
#define KS_ITERS   8000L         /* alloc-batches per chunk */
#define KS_CHUNKS  48           /* chunks per child; cpu index sampled between chunks
                                 * so a migrating child observes every core it ran on.
                                 * Sized so T1 >= ~5s: with the M25 P4 per-CPU
                                 * scheduler the baseline child gets a clean core and
                                 * 16 chunks finished in ~1s -- below the test's own
                                 * >=2s floor, making the 1s-granularity RTC clock
                                 * read as +/-50% noise on the TN/T1 ratio. */

/* Run the BKL-free heap stress, returning the total corruption count (want 0) and
 * OR-ing every core this child observed into *seen_mask. */
static long compute(unsigned long seed, unsigned *seen_mask)
{
    long bad = 0;
    for (int chunk = 0; chunk < KS_CHUNKS; chunk++) {
        bad += sys_kheap_stress(KS_ITERS, KS_SIZE, seed);
        int c = sys_cpu_index();
        if (c >= 0 && c < 16) *seen_mask |= (1u << c);
    }
    return bad;
}

/* Crude wall-clock seconds from the RTC fields (monotonic enough over the test's
 * window; day rollover is irrelevant here). */
static long now_secs(void)
{
    struct logit_time t; get_time(&t);
    return ((long)t.hour * 60 + t.minute) * 60 + t.second;
}

/* Run a batch of `n` worker children. Returns the batch wall time (seconds);
 * sets *ok (1 = every child reported 0 corruption), *seen_all (OR of cores the
 * children observed), *got (children reaped). */
static long run_batch(int n, int *ok, unsigned *seen_all, int *got)
{
    int fds[2];
    if (sys_pipe(fds) < 0) { outs("smptest: pipe failed\n"); *ok = 0; *got = 0; return 0; }

    long t0 = now_secs();
    int pids[NCHILD_MAX];
    for (int i = 0; i < n; i++) {
        int pid = sys_fork();
        if (pid < 0) {                              /* reap already-forked children, don't leak fds */
            outs("smptest: fork failed\n");
            sys_close(fds[1]);                      /* parent's write end: no longer needed */
            for (int k = 0; k < i; k++) sys_waitpid(pids[k], 0);
            sys_close(fds[0]);
            *ok = 0; *got = 0; return 0;
        }
        if (pid == 0) {
            sys_close(fds[0]);
            unsigned seen = 0;
            long bad = compute((unsigned long)(i + 1), &seen);   /* unique per-child seed */
            /* Report "R <seen_hex> <bad_hex>\n". Encode through a VOLATILE pointer:
             * clang -msse2 would otherwise auto-vectorize the 16-digit loop into XMM,
             * and QEMU's SSE emulation on an Arm host miscompiles that shuffle (it
             * wrote 0x00 instead of the ascii digits). Volatile = plain byte stores. */
            static const char HEX[16] = "0123456789abcdef";
            char line[64]; volatile char *lp = line; int k = 0;
            lp[k++] = 'R'; lp[k++] = ' ';
            for (int s = 12; s >= 0; s -= 4) lp[k++] = HEX[(seen >> s) & 0xF];
            lp[k++] = ' ';
            for (int s = 60; s >= 0; s -= 4) lp[k++] = HEX[((unsigned long)bad >> s) & 0xF];
            lp[k++] = '\n';
            sys_write(fds[1], line, k);
            sys_close(fds[1]);
            app_exit(0);
        }
        pids[i] = pid;
    }
    sys_close(fds[1]);   /* parent closes write end so reads hit EOF after children */

    char buf[1024]; int total = 0; int r;
    while (total < (int)sizeof(buf) - 1 && (r = sys_read(fds[0], buf + total, sizeof(buf) - 1 - total)) > 0)
        total += r;
    buf[total] = 0;
    sys_close(fds[0]);

    for (int i = 0; i < n; i++) { int st; sys_waitpid(pids[i], &st); }
    long t1 = now_secs();
    long wall = t1 - t0; if (wall < 0) wall += 86400;

    int g = 0, okc = 1; unsigned smask = 0;
    const char *p = buf;
    while (*p) {
        if (p[0] == 'R' && p[1] == ' ') {
            const char *q = p + 2;
            unsigned seen = 0; while (*q && *q != ' ') {
                int d = (*q>='a') ? (*q-'a'+10) : (*q-'0'); seen = (seen<<4) | (unsigned)d; q++;
            }
            q++;
            unsigned long bad = 0; for (int kk = 0; kk < 16 && *q && *q != '\n'; kk++, q++) {
                int d = (*q>='a') ? (*q-'a'+10) : (*q-'0'); bad = (bad<<4) | (unsigned)d;
            }
            if (bad != 0) okc = 0;
            smask |= seen;
            g++;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    *ok = okc; *seen_all = smask; *got = g;
    return wall;
}

int main(void)
{
    int ncpu = 4;   /* matches the -smp 4 test rig */

    /* Baseline: one child alone -> T1. */
    int ok1, got1; unsigned seen1;
    long T1 = run_batch(1, &ok1, &seen1, &got1);

    /* Parallel batch: N children -> TN. */
    int okN, gotN; unsigned seenN;
    long TN = run_batch(ncpu, &okN, &seenN, &gotN);

    int distinct = 0; for (int i = 0; i < 16; i++) if (seenN & (1u << i)) distinct++;

    outs("smptest: T1="); outn(T1); outs("s TN="); outn(TN);
    outs("s children="); outn(gotN); outs(" distinct_cpus="); outn(distinct);
    outs(" corruption="); outn(okN ? 0 : 1);
    outs("\n");

    if (got1 != 1 || gotN != ncpu) { outs("SMP_TEST_FAIL: missing children\n"); return 1; }
    if (!ok1 || !okN)              { outs("SMP_TEST_FAIL: concurrent kmalloc corruption\n"); return 1; }
    if (distinct < 2)              { outs("SMP_TEST_FAIL: children ran on <2 cores (no parallelism)\n"); return 1; }
    /* Genuine BKL-free concurrency: N children finished in well under N*T1. Require
     * TN < 1.6*T1 (i.e. 5*TN < 8*T1). Need a meaningful baseline (>=2s). */
    if (T1 < 2)                    { outs("SMP_TEST_FAIL: baseline too short to time\n"); return 1; }
    /* The old text guessed "kmalloc still serialized by the BKL?" and the guess
     * was wrong by a factor of 834. Measured with tests/boot/run-smp-lockprobe.sh,
     * which samples every lock's ticket counter across exactly this workload:
     *
     *     kheap_lock   267 -> 30,720,350     (+30.7 MILLION)
     *     g_bkl      6,473 ->     43,309     (+36,836)
     *     pmm_lock   2,740 ->      3,044     (+304)
     *
     * SYS_KHEAP_STRESS is the ONE entry on syscall_is_bkl_free()'s allow-list,
     * so the BKL was never what serialised this. It is kheap_lock -- one global
     * lock around the allocator -- and four cores spinning on it cost more than
     * doing the work serially (T1=5s, TN=41s). Making this gate pass needs a
     * per-core front end (magazines, a per-CPU free list), not anything to do
     * with the big kernel lock. */
    if (!(TN * 5 < T1 * 8))        { outs("SMP_TEST_FAIL: no wall-clock speedup -- kheap_lock serialises kmalloc (see the note above)\n"); return 1; }

    outs("SMP_TEST_OK\n");
    return 0;
}
