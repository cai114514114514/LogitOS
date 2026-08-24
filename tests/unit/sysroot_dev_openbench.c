/* sysroot_dev_openbench.c -- what ONE open() costs on this VFS, measured on
 * the device, for the two outcomes a compiler's -I probing produces: a path
 * that does not exist (every failed probe) and one that does (the header it
 * finally finds), open+close. Compiled ON THE DEVICE by /bin/tcc against
 * the sysroot, like crcwalk.
 *
 * WHY SELF-SCALING. CLOCK_MONOTONIC here is the 100 Hz tick (c/apps/libc/
 * include/time.h: granularity 10 ms), so a fixed iteration count would be
 * either a few ticks under KVM (useless) or a minute under TCG. Each case
 * runs until at least ONE SECOND has elapsed and reports the iterations it
 * managed, so the quantisation error is <= 1% in both accelerators. Not
 * rdtsc: tcc has no __builtin_ia32_rdtsc and the number wanted is wall
 * time, which is what the tick measures.
 *
 * The expected outcome of every open is ASSERTED (a bench whose "miss" path
 * silently hit would report the wrong cost with a straight face). */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static int bench(const char *label, const char *path, int expect_ok)
{
    long n = 0, okc = 0, failc = 0;
    double t0 = now_ms(), t1;
    int i;
    do {
        for (i = 0; i < 100; i++) {
            int fd = open(path, O_RDONLY);
            if (fd >= 0) { close(fd); okc++; } else failc++;
        }
        n += 100;
        t1 = now_ms();
    } while (t1 - t0 < 1000.0);
    printf("OPENBENCH %-9s %6ld opens in %5.0f ms = %7.1f us each  (ok=%ld fail=%ld) %s\n",
           label, n, t1 - t0, (t1 - t0) * 1000.0 / (double)n, okc, failc, path);
    if (expect_ok ? (failc != 0) : (okc != 0)) {
        printf("OPENBENCH-ERR %s: expected every open to %s\n", label, expect_ok ? "succeed" : "fail");
        return 1;
    }
    return 0;
}

int main(void)
{
    int bad = 0;
    /* The misses a -I probe makes: a name that is not in the first include
     * directory, then not in the second; and a miss at the root for the cost
     * of the walk itself. */
    bad |= bench("miss-inc",  "/usr/include/no_such_header.h", 0);
    bad |= bench("miss-tinc", "/usr/lib/tcc/include/no_such_header.h", 0);
    bad |= bench("miss-root", "/no_such_file", 0);
    bad |= bench("hit-stdio", "/usr/include/stdio.h", 1);
    bad |= bench("hit-deep",  "/usr/lib/tcc/include/stddef.h", 1);
    printf("OPENBENCH-DONE %s\n", bad ? "with errors" : "ok");
    return bad;
}
