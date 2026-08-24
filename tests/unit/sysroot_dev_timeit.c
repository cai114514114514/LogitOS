/* sysroot_dev_timeit.c -- a stopwatch for the device: `timeit N prog args...`
 * runs the program N times through fork+execv+waitpid and prints each run's
 * wall time and the min/median/max, in milliseconds with three decimals.
 *
 * WHY NOT `uptime -s` AROUND THE COMMAND. The only clock ring 3 can read is
 * the 100 Hz tick (CLOCK_MONOTONIC, 10 ms granularity), and under KVM the
 * command being timed -- `tcc -E` over seven headers -- takes a few tens of
 * milliseconds, which the tick cannot resolve. So the stopwatch is the TSC,
 * read with rdtsc (tcc assembles it: x86_64-asm.h DEF_ASM_OP0), calibrated
 * against that tick at start.
 *
 * WHY fork+execv AND NOT system(). system() is fork + execv("/bin/sh","-c",
 * cmd) -- and /bin/sh has NO -c: it ignores its arguments and starts an
 * INTERACTIVE shell on the inherited fds (measured on the first run of this
 * harness, 2026-08-21: the nested shell printed its banner and a prompt, ate
 * the harness's next command, and the parent sat in waitpid forever). That
 * is a real mini-libc/sh mismatch, reported to the libc line; until it is
 * fixed nothing on this machine can use system() for a program that exits.
 *
 * CALIBRATION TAKES THE MAX of five 300 ms windows: under KVM the tick is
 * delivered in catch-up bursts when the host preempts the vCPU, and a burst
 * inside a window makes the guest clock cover MORE wall time than the TSC
 * did, biasing that window's MHz LOW (first measurement: 3191/3354/3417 MHz
 * across three runs against the kernel's own pit-calibrated 3417.916). A
 * burst can only subtract, so the max is the estimate; every window is
 * printed so the spread is on the record.
 *
 * WHAT IS INSIDE THE INTERVAL: fork + execv + the program + waitpid. The
 * harness times /bin/true the same way first and reports it as the floor;
 * nothing is subtracted here (a tool that silently subtracted a baseline
 * would hide a baseline that changed).
 *
 * Compiled ON THE DEVICE by /bin/tcc against the sysroot, like crcwalk. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

static unsigned long long rdtsc(void)
{
    unsigned lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

/* One calibration window: TSC ticks per ms over >= win_ms of the tick clock,
 * starting on a tick edge so the window is an exact number of ticks. */
static double window(double win_ms)
{
    double t0 = now_ms(), t;
    unsigned long long c0, c1;
    while ((t = now_ms()) == t0) ;
    t0 = t; c0 = rdtsc();
    while ((t = now_ms()) - t0 < win_ms) ;
    c1 = rdtsc();
    return (double)(c1 - c0) / (t - t0);
}

static double calibrate(void)
{
    double best = 0.0, v;
    int i;
    printf("TIMEIT calib MHz:");
    for (i = 0; i < 5; i++) {
        v = window(300.0);
        printf(" %.1f", v / 1000.0);
        if (v > best) best = v;
    }
    printf(" -> using max %.3f MHz\n", best / 1000.0);
    return best;
}

static int cmpd(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    static double runs[64];
    static char cmd[1024];
    double per_ms;
    int n, i, st, pid;
    if (argc < 3) { printf("usage: timeit N prog args...\n"); return 2; }
    n = atoi(argv[1]);
    if (n < 1 || n > 64) { printf("timeit: N must be 1..64\n"); return 2; }
    cmd[0] = 0;
    for (i = 2; i < argc; i++) {
        if (strlen(cmd) + strlen(argv[i]) + 2 > sizeof cmd) { printf("timeit: command too long\n"); return 2; }
        if (i > 2) strcat(cmd, " ");
        strcat(cmd, argv[i]);
    }
    per_ms = calibrate();
    for (i = 0; i < n; i++) {
        unsigned long long c0 = rdtsc(), c1;
        pid = fork();
        if (pid < 0) { printf("TIMEIT-ERR: fork failed\n"); return 1; }
        if (pid == 0) {
            execv(argv[2], argv + 2);
            _Exit(127);
        }
        st = -1;
        if (waitpid(pid, &st, 0) < 0) { printf("TIMEIT-ERR: waitpid failed\n"); return 1; }
        c1 = rdtsc();
        runs[i] = (double)(c1 - c0) / per_ms;
        printf("TIMEIT run %d: %.3f ms status=%d\n", i + 1, runs[i], st);
        if (st != 0) { printf("TIMEIT-ERR: %s exited with status %d\n", argv[2], st); return 1; }
    }
    qsort(runs, (size_t)n, sizeof runs[0], cmpd);
    printf("TIMEIT-RESULT n=%d min=%.3f med=%.3f max=%.3f ms cmd=%s\n",
           n, runs[0], runs[n / 2], runs[n - 1], cmd);
    return 0;
}
