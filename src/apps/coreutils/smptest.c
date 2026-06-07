/* smptest -- M25 P0 SMP concurrency + no-corruption proof.
 *
 * Each worker child runs a long, deterministic integer-checksum compute over a
 * PRIVATE array, interleaved with on-stack scribble passes. The checksum is
 * reproducible, so the parent knows it must be identical across all children: any
 * mismatch means another core corrupted this child's memory (a mutual-exclusion
 * bug). Each child also samples SYS_CPU_INDEX during the loop and reports the set
 * of cores it ran on.
 *
 * The parent runs TWO batches and compares wall-clock:
 *   - batch of 1 child  -> T1 (single-core baseline)
 *   - batch of N children -> TN
 * If the N children ran truly in PARALLEL, TN ~= T1 (they share the wall window);
 * if they ran sequentially on one core, TN ~= N*T1. We require TN < 1.6*T1, a
 * speedup only achievable if >=2 cores ran ring-3 simultaneously.
 *
 * Asserts: (a) every checksum == reference (no corruption), (b) the N-batch ran
 * on >=2 distinct cores AND TN < 1.6*T1 (genuine concurrency). Prints SMP_TEST_OK.
 */
#include "clib.h"

#define NCHILD_MAX 8
#define ARRN       512          /* private compute array (per child address space) */
#define ITERS      480000000L /* ~4 s of pure ring-3 compute per child (TCG): the timing
                                * baseline uses second-resolution RTC, so T1 must clear 2 s */
#define SAMPLE     10000000L    /* sample the running core index every SAMPLE iters */

/* Deterministic checksum: identical inputs -> identical output every run, so the
 * parent can compare children against each other. Pure integer in the hot loop
 * (only the rare SYS_CPU_INDEX sample touches the BKL) -> real parallel compute.
 * `seen_mask` collects the distinct core indices this child observed. A countdown
 * (not a modulo) gates the sample so the hot loop stays cheap under TCG. */
static unsigned long compute(unsigned *seen_mask)
{
    static unsigned long a[ARRN];          /* private to this forked address space */
    for (int i = 0; i < ARRN; i++) a[i] = (unsigned long)(i * 2654435761u + 1u);
    unsigned long sum = 0;
    long next_sample = 0;
    for (long it = 0; it < ITERS; it++) {
        int i = (int)(it & (ARRN - 1));
        a[i] = a[i] * 1099511628211UL + (unsigned long)it;   /* scribble (alloc-ish churn) */
        sum ^= a[i] + (sum << 7) + (sum >> 3);
        if (it == next_sample) {
            next_sample += SAMPLE;
            int c = sys_cpu_index();
            if (c >= 0 && c < 16) *seen_mask |= (1u << c);
        }
    }
    return sum;
}

/* Crude wall-clock seconds from the RTC fields (monotonic enough over the test's
 * window; day rollover is irrelevant here). */
static long now_secs(void)
{
    struct aqua_time t; get_time(&t);
    return ((long)t.hour * 60 + t.minute) * 60 + t.second;
}

/* Run a batch of `n` worker children. Returns the batch wall time (seconds);
 * sets *cks_ref to the common checksum, *ok (1 = all checksums matched),
 * *seen_all (OR of the cores all children observed), *got (children reaped). */
static long run_batch(int n, unsigned long *cks_ref, int *ok, unsigned *seen_all, int *got)
{
    int fds[2];
    if (sys_pipe(fds) < 0) { outs("smptest: pipe failed\n"); *ok = 0; *got = 0; return 0; }

    long t0 = now_secs();
    int pids[NCHILD_MAX];
    for (int i = 0; i < n; i++) {
        int pid = sys_fork();
        if (pid < 0) { outs("smptest: fork failed\n"); *ok = 0; *got = 0; return 0; }
        if (pid == 0) {
            sys_close(fds[0]);
            unsigned seen = 0;
            unsigned long cks = compute(&seen);
            /* line: "R <seen_hex> <cksum_hex>\n" */
            char line[64]; int k = 0;
            line[k++] = 'R'; line[k++] = ' ';
            for (int s = 12; s >= 0; s -= 4) { int d=(int)((seen>>s)&0xF); line[k++]=(char)(d<10?'0'+d:'a'+d-10); }
            line[k++] = ' ';
            for (int s = 60; s >= 0; s -= 4) { int d=(int)((cks>>s)&0xF); line[k++]=(char)(d<10?'0'+d:'a'+d-10); }
            line[k++] = '\n';
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

    int g = 0, set = 0, okc = 1; unsigned long ref = 0; unsigned smask = 0;
    const char *p = buf;
    while (*p) {
        if (p[0] == 'R' && p[1] == ' ') {
            const char *q = p + 2;
            unsigned seen = 0; while (*q && *q != ' ') {
                int d = (*q>='a') ? (*q-'a'+10) : (*q-'0'); seen = (seen<<4) | (unsigned)d; q++;
            }
            q++;
            unsigned long cks = 0; for (int kk = 0; kk < 16 && *q && *q != '\n'; kk++, q++) {
                int d = (*q>='a') ? (*q-'a'+10) : (*q-'0'); cks = (cks<<4) | (unsigned)d;
            }
            if (!set) { ref = cks; set = 1; } else if (cks != ref) okc = 0;
            smask |= seen;
            g++;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    *cks_ref = ref; *ok = okc; *seen_all = smask; *got = g;
    return wall;
}

int main(void)
{
    int ncpu = 4;   /* matches the -smp 4 test rig */

    /* Baseline: one child alone -> T1. */
    unsigned long cks1; int ok1, got1; unsigned seen1;
    long T1 = run_batch(1, &cks1, &ok1, &seen1, &got1);

    /* Parallel batch: N children -> TN. */
    unsigned long cksN; int okN, gotN; unsigned seenN;
    long TN = run_batch(ncpu, &cksN, &okN, &seenN, &gotN);

    int distinct = 0; for (int i = 0; i < 16; i++) if (seenN & (1u << i)) distinct++;

    outs("smptest: T1="); outn(T1); outs("s TN="); outn(TN);
    outs("s children="); outn(gotN); outs(" distinct_cpus="); outn(distinct);
    outs(" cksum="); { char h[17]; int n=0; for (int s=60;s>=0;s-=4){int d=(int)((cksN>>s)&0xF);h[n++]=(char)(d<10?'0'+d:'a'+d-10);} h[n]=0; outs(h); }
    outs("\n");

    if (got1 != 1 || gotN != ncpu) { outs("SMP_TEST_FAIL: missing children\n"); return 1; }
    if (!ok1 || !okN)              { outs("SMP_TEST_FAIL: checksum mismatch (corruption)\n"); return 1; }
    if (cks1 != cksN)              { outs("SMP_TEST_FAIL: checksum differs across batches (corruption)\n"); return 1; }
    if (distinct < 2)              { outs("SMP_TEST_FAIL: children ran on <2 cores (no parallelism)\n"); return 1; }
    /* Genuine concurrency: N children finished in well under N*T1. Require
     * TN < 1.6*T1 (i.e. 5*TN < 8*T1). Need a meaningful baseline (>=2s). */
    if (T1 < 2)                    { outs("SMP_TEST_FAIL: baseline too short to time\n"); return 1; }
    if (!(TN * 5 < T1 * 8))        { outs("SMP_TEST_FAIL: no wall-clock speedup (ran sequentially?)\n"); return 1; }

    outs("SMP_TEST_OK\n");
    return 0;
}
