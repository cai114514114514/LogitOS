/* /bin/schedtest -- does the weighted scheduler actually divide the CPU in the
 * ratio it was asked for?
 *
 *   schedtest [MS]        runs the three cases below and prints a verdict
 *
 * WHY THIS EXISTS AS A PROGRAM AND NOT AS AN ASSERTION IN THE KERNEL: the
 * quantity under test is "how much work did each of two competing processes
 * get done", and only a ring-3 process can count its own work. A kernel-side
 * check would necessarily read the kernel's own accounting, i.e. it would ask
 * the scheduler whether the scheduler did what the scheduler recorded.
 *
 * SO THERE ARE TWO NUMBERS PER CHILD AND THEY ARE INDEPENDENT:
 *   work   an iteration count kept in ring 3 and touched by nothing else
 *   cpuns  getrusage(RUSAGE_THREAD), the kernel's own per-thread ns
 * They are maintained by different code in different rings and must agree. A
 * disagreement is a finding either way round: the same discipline the rmap's
 * "rmap_count == pmm_refcount" rests on, and virtio-balloon's requirement that
 * the guest and the host both report the same number of frames.
 *
 * THE PARENT SETS THE CHILDREN'S NICE, not the children themselves, for two
 * reasons. It removes the race (a child could start counting before it had
 * reniced itself, and that error is silent and always in the same direction),
 * and it makes this the same code path /bin/renice uses -- setpriority on
 * ANOTHER pid, which is the half of the kernel's permission rule that a
 * self-only test never reaches.
 *
 * WHAT IS DELIBERATELY NOT MEASURED HERE: latency, interactivity, and anything
 * about the desktop. This machine's own BKL profile (CLAUDE.md) says the
 * compositor holds the big lock for a whole frame, so a wakeup-latency number
 * from this program would be a number about that lock wearing a scheduler's
 * name. Share under contention is what the weight means and it is all that is
 * claimed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include "logit_abi.h"

static long sys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

/* The kernel's own 100 Hz counter. Used instead of clock_gettime() because the
 * quantity that matters here is "how many preemption points have gone by", and
 * this is literally that counter times ten -- a ns-looking clock would suggest
 * a resolution the deadline does not have. */
static unsigned long long ms_now(void)
{ return (unsigned long long)sys(SYS_MONOTONIC_MS, 0, 0, 0); }

/* The work unit. `volatile` on the sink is what stops -O2 from deleting the
 * whole loop: without it the multiply chain is dead and the "CPU-bound" child
 * spends its slice calling the clock, which would make every ratio 1:1 and
 * look exactly like a scheduler that ignores weight -- i.e. it would silently
 * reproduce this gate's own negative control. */
static volatile unsigned long g_sink;
#define INNER 4096u

static unsigned long long spin_until(unsigned long long deadline_ms)
{
    unsigned long long units = 0;
    while (ms_now() < deadline_ms) {
        unsigned long x = (unsigned long)units + 1u;
        for (unsigned i = 0; i < INNER; i++) x = x * 1103515245ul + 12345ul;
        g_sink = x;
        units++;
    }
    return units;
}

static unsigned long long cpu_ns_self(void)
{ long ns = sys(SYS_RUSAGE, RUCTL_GET_NS, 0, 0); return ns < 0 ? 0ull : (unsigned long long)ns; }

/* One child: wait for the common start instant, work to the common end
 * instant, report one line down the pipe with a single write(). One write
 * because both children share the pipe and a torn line would be indisplayable
 * as a parse error rather than as the scheduling result it is. */
static void child(int wfd, unsigned long long start_ms, unsigned long long end_ms)
{
    char line[160];
    while (ms_now() < start_ms) { }        /* busy, not yield: both children must
                                            * be RUNNABLE through the whole
                                            * window, including its start */
    unsigned long long c0 = cpu_ns_self();
    unsigned long long work = spin_until(end_ms);
    unsigned long long c1 = cpu_ns_self();
    errno = 0;
    int nice_read = getpriority(PRIO_PROCESS, 0);
    int n = snprintf(line, sizeof line, "R %d %llu %llu %d\n",
                     (int)getpid(), work, c1 - c0, nice_read);
    ssize_t ignored = write(wfd, line, (size_t)n);
    (void)ignored;
    _exit(0);
}

struct res { int pid; unsigned long long work, cpuns; int nice; };

static int parse(char *s, struct res *out, int n)
{
    int got = 0;
    while (*s && got < n) {
        char *e = strchr(s, '\n');
        if (e) *e = 0;
        if (s[0] == 'R' && s[1] == ' ') {
            char *p = s + 2;
            out[got].pid   = (int)strtol(p, &p, 10);
            out[got].work  = strtoull(p, &p, 10);
            out[got].cpuns = strtoull(p, &p, 10);
            out[got].nice  = (int)strtol(p, &p, 10);
            got++;
        }
        if (!e) break;
        s = e + 1;
    }
    return got;
}

/* Deviation of `got` from `want`, in per mille of `want`. Integer only: a
 * printed 1.99 that is really 1.994999 and a printed 1.99 that is really
 * 1.985 are different verdicts at a 1% bound, and %f would hide which. */
static long dev_permille(unsigned long long got_x1000, unsigned long long want_x1000)
{
    long d = (long)got_x1000 - (long)want_x1000;
    if (d < 0) d = -d;
    return want_x1000 ? (long)((d * 1000ull) / want_x1000) : 0;
}

/* THE TWO BOUNDS, derived rather than chosen -- and they are two because the
 * first measurement showed the two quantities do NOT deviate by the same
 * amount, which is a fact about the machine and not about the scheduler.
 *
 * (1) THE SHARE BOUND, on cpu_ns_a / cpu_ns_b against weight_a / weight_b.
 * This is what the scheduler actually decides. The preemption point is the
 * 100 Hz tick, so over a window of W ms each child receives an integer number
 * of 10 ms ticks, +-1 at each end of its run. At W = 4000 the lighter child of
 * a 2:1 pair gets about 133 ticks and the heavier about 267, so +-1 tick each
 * moves the ratio by (268/132)/(267/133) - 1 = 1.1%. The deadline is read from
 * that same 10 ms counter, so the two windows can differ by one tick: another
 * 0.25%. The weight table is EXACT at these nice values -- 1024, 512 and 2048
 * are powers of two, so the reciprocal the kernel precomputes has no rounding
 * at all, which is why the gate uses nice 0, 10 and -10 and not 3 and 7.
 * Total: about 1.4%. What that does not cover is the rest of the machine (the
 * compositor, kworker, the shell), whose ticks are not split evenly between
 * the two children and which nothing in the scheduler bounds -- so the gate is
 * 50 per mille. MEASURED on this machine, -smp 2 under TCG, 4000 ms:
 * 0/1000, 16/1000, 9/1000 for the 1:1, 2:1 and 4:1 cases.
 *
 * (2) THE WORK BOUND, on work_a / work_b against the same weight ratio. This
 * is the INDEPENDENT number -- an iteration count kept in ring 3, which no
 * kernel code touches -- and it is bounded far more loosely, at 250 per mille,
 * for a measured reason rather than a hedge: WORK PER NANOSECOND IS NOT EQUAL
 * BETWEEN THE TWO CHILDREN, and the equal-weight case measures exactly how
 * unequal in every run. Two effects, one real and one environmental:
 *
 *   - the heavier child runs in longer consecutive bursts and pays
 *     proportionally fewer context switches (and, under TCG, fewer
 *     translation-buffer disruptions) per unit of CPU it receives. Measured:
 *     6.020e-4 units/ns for the nice-0 child against 5.729e-4 for the nice-10
 *     one -- 5.1% more productive per ns, which accounts for that run's work
 *     ratio of 2.118 against a CPU ratio of 2.016 to within a thousandth.
 *   - host load. The SAME 1:1 case, where the feature under test contributes
 *     nothing whatsoever, read wdev = 18, 20 and 28 per mille on a quiet host
 *     and 126, 130 and 130 per mille an hour later with other builds running.
 *     Its sdev stayed at 1 or 2 per mille through all six.
 *
 * So the noise floor on (2) alone reaches 130 per mille with the scheduler
 * doing nothing, which is why a bound of 150 was tried and abandoned: it would
 * have failed a correct kernel on a busy afternoon. 250 leaves the control
 * failing by a factor of 1.7 to 2.9 (it returns ~1.13 against wants of 2.000
 * and 4.000, i.e. 437 and 719 per mille out), and every run prints the 1:1
 * case's own wdev so the floor is visible rather than assumed.
 *
 * Dropping (2) and keeping only (1) is the tempting simplification and it is
 * the wrong one: (1) is the kernel's own accounting, so a gate built on it
 * alone is the scheduler marking its own work. Both, with different bounds,
 * both printed. */
#define TOL_SHARE_PERMILLE  50
#define TOL_WORK_PERMILLE  250

static int one_case(int niceA, int niceB, unsigned long long ms)
{
    int fds[2];
    if (pipe(fds) != 0) { printf("SCHED-FAIL pipe\n"); return 0; }

    unsigned long long t0 = ms_now();
    unsigned long long start = t0 + 600;      /* room for both forks + both
                                               * setpriority calls, in ticks */
    unsigned long long end   = start + ms;

    int pa = fork();
    if (pa == 0) { close(fds[0]); child(fds[1], start, end); }
    int pb = fork();
    if (pb == 0) { close(fds[0]); child(fds[1], start, end); }
    close(fds[1]);
    if (pa <= 0 || pb <= 0) { printf("SCHED-FAIL fork\n"); return 0; }

    int perr = 0;
    if (setpriority(PRIO_PROCESS, pa, niceA) != 0) perr = 1;
    if (setpriority(PRIO_PROCESS, pb, niceB) != 0) perr = 1;

    /* The weights the PICK LOOP will use, read from the kernel rather than
     * recomputed from the nice values here. If the table in sched.c is wrong,
     * a harness that re-derived it would agree with the bug. */
    int wa = (int)sys(SYS_SCHED, SCHEDCTL_GET_WEIGHT, pa, 0);
    int wb = (int)sys(SYS_SCHED, SCHEDCTL_GET_WEIGHT, pb, 0);

    char buf[512];
    size_t used = 0;
    for (;;) {
        ssize_t r = read(fds[0], buf + used, sizeof buf - 1 - used);
        if (r <= 0) break;
        used += (size_t)r;
        if (used >= sizeof buf - 1) break;
    }
    buf[used] = 0;
    close(fds[0]);
    int st;
    waitpid(pa, &st, 0);
    waitpid(pb, &st, 0);

    struct res r[2];
    int got = parse(buf, r, 2);
    if (got != 2) { printf("SCHED-FAIL case n=%d/%d: got %d results\n", niceA, niceB, got); return 0; }

    struct res *ra = (r[0].pid == pa) ? &r[0] : &r[1];
    struct res *rb = (r[0].pid == pa) ? &r[1] : &r[0];

    if (wa <= 0 || wb <= 0 || !rb->work || !rb->cpuns) {
        printf("SCHED-FAIL case n=%d/%d: w=%d/%d work=%llu/%llu\n",
               niceA, niceB, wa, wb, ra->work, rb->work);
        return 0;
    }

    unsigned long long want  = ((unsigned long long)wa * 1000ull) / (unsigned long long)wb;
    unsigned long long meas  = (ra->work  * 1000ull) / rb->work;
    unsigned long long cpu   = (ra->cpuns * 1000ull) / rb->cpuns;
    long dv = dev_permille(meas, want);   /* work  vs weight -- the independent one */
    long dc = dev_permille(cpu,  want);   /* share vs weight -- what was decided */

    int ok = 1;
    if (perr)                       ok = 0;
    if (ra->nice != niceA || rb->nice != niceB) ok = 0;
    if (dv > TOL_WORK_PERMILLE)     ok = 0;
    if (dc > TOL_SHARE_PERMILLE)    ok = 0;

    printf("SCHED-CASE nice=%d/%d weight=%d/%d want=%llu.%03llu work=%llu.%03llu "
           "wdev=%ld/1000 cpu=%llu.%03llu sdev=%ld/1000 niceback=%d/%d %s\n",
           niceA, niceB, wa, wb,
           want / 1000, want % 1000, meas / 1000, meas % 1000, dv,
           cpu / 1000, cpu % 1000, dc,
           ra->nice, rb->nice, ok ? "PASS" : "FAIL");
    printf("SCHED-RAW  a: pid=%d work=%llu cpuns=%llu   b: pid=%d work=%llu cpuns=%llu\n",
           ra->pid, ra->work, ra->cpuns, rb->pid, rb->work, rb->cpuns);
    return ok;
}

int main(int argc, char **argv)
{
    unsigned long long ms = (argc > 1) ? strtoull(argv[1], 0, 10) : 4000;
    int pass = 0, total = 0;

    /* Case 1 is the control INSIDE the positive test: equal nice must give
     * 1:1. It is what separates "the weighting works" from "these two numbers
     * always come out different", and it is the one case that must keep
     * passing under -DSCHED_IGNORE_WEIGHT. */
    total++; pass += one_case(0,   0,  ms);
    total++; pass += one_case(0,  10,  ms);   /* exactly 2:1 by construction */
    total++; pass += one_case(-10,10,  ms);   /* exactly 4:1 by construction */

    /* The permission rule, checked here because it shares the syscall and
     * nothing else in the tree exercises the refusal. Root is allowed to lower
     * the number, so this only proves the plumbing when run as root -- which
     * every process on this machine currently is, and that is stated rather
     * than dressed up as a passing security test. */
    errno = 0;
    if (getpriority(PRIO_PGRP, 0) == -1 && errno == EINVAL) { pass++; }
    else printf("SCHED-FAIL PRIO_PGRP was not refused\n");
    total++;

    errno = 0;
    if (setpriority(PRIO_PROCESS, 999999, 5) == -1 && errno == ESRCH) { pass++; }
    else printf("SCHED-FAIL setpriority on a dead pid was not ESRCH\n");
    total++;

    /* nice() is relative and returns the new value; run last because it moves
     * this process's own priority and nothing after it depends on that. */
    {
        int before = getpriority(PRIO_PROCESS, 0);
        int after  = nice(3);
        if (after == before + 3) pass++;
        else printf("SCHED-FAIL nice(3): %d -> %d\n", before, after);
        total++;
        setpriority(PRIO_PROCESS, 0, before);
    }

    printf("SCHED-RESULT %d/%d\n", pass, total);
    return pass == total ? 0 : 1;
}
