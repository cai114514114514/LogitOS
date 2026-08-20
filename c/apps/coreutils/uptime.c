#include "clib.h"

/* uptime -- how long this machine has been up, read out of /proc/uptime.
 *
 *   $ uptime
 *   up 0:04:12
 *
 * ONE NUMBER AND NO LOAD AVERAGE. Linux's uptime(1) prints a load average and
 * a user count; this machine has neither -- there is no run-queue length
 * accumulator in c/kernel/sched and no login session table to count. Printing
 * "load average: 0.00" would be three invented numbers in the place a reader
 * looks for real ones. /proc/uptime has one column for the same reason (see
 * r_uptime in c/fs/procfs.c): the kernel does not sum idle time either.
 *
 * -s prints the raw seconds, for a script that wants to subtract two of them.
 *
 * -d IS AN INSTRUMENT, and it is the only way to ask this question from ring 3
 * on the real machine. Everything else in /proc can be checked by reading a
 * file twice -- but two `cat`s are two OPENS, so they would differ even if
 * every /proc file were rendered at open() and cached for the life of the
 * descriptor. -d holds ONE descriptor open across a wait and then reads it,
 * beside a fresh open taken at the same instant:
 *
 *   $ uptime -d
 *   held=6.31 fresh=6.31 delta=0.00 LIVE
 *
 * `held` is the fd opened three seconds earlier. If it reads three seconds
 * behind `fresh`, the answer was computed at open() and the word is STALE.
 * See c/fs/procfs.h point 4 and the `live` field in c/kernel/exec/file.h. */

static void two(long v) { outc((char)('0' + (v / 10) % 10)); outc((char)('0' + v % 10)); }

/* Read a whole /proc/uptime and return it in HUNDREDTHS, or -1. */
static long read_cs(int fd)
{
    char b[64];
    int n = sys_read(fd, b, sizeof b - 1);
    if (n <= 0) return -1;
    b[n] = 0;
    long v = 0; int i = 0;
    if (b[i] < '0' || b[i] > '9') return -1;
    while (b[i] >= '0' && b[i] <= '9') v = v * 10 + (b[i++] - '0');
    v *= 100;
    if (b[i] == '.' && b[i + 1] >= '0' && b[i + 1] <= '9' && b[i + 2] >= '0' && b[i + 2] <= '9')
        v += (b[i + 1] - '0') * 10 + (b[i + 2] - '0');
    return v;
}

static void put_cs(long cs) { outn(cs / 100); outc('.'); two(cs % 100); }

static int delayed(void)
{
    int held = sys_open("/proc/uptime", O_RDONLY);
    if (held < 0) { errs("uptime: cannot open /proc/uptime\n"); return 1; }

    /* Three seconds, so a stale answer is three seconds wrong and cannot be
     * mistaken for scheduling jitter. The wait is here and not in a shell
     * `sleep` because the descriptor has to stay open across it, and a shell
     * cannot hold one for a program that is not running yet. */
    unsigned long long t0 = monotonic_ms();
    while (monotonic_ms() - t0 < 3000) sys_yield();

    long a = read_cs(held);
    sys_close(held);

    int fresh = sys_open("/proc/uptime", O_RDONLY);
    long b = fresh >= 0 ? read_cs(fresh) : -1;
    if (fresh >= 0) sys_close(fresh);

    if (a < 0 || b < 0) { errs("uptime: -d could not read both\n"); return 1; }
    long d = b - a; if (d < 0) d = -d;

    outs("held="); put_cs(a);
    outs(" fresh="); put_cs(b);
    outs(" delta="); put_cs(d);
    /* 100 hundredths = one second. The gap being measured is three; the noise
     * is a scheduler slice. Anything in between is neither and should be
     * looked at rather than rounded away, which is why the numbers print. */
    outs(d < 100 ? " LIVE\n" : " STALE\n");
    return d < 100 ? 0 : 1;
}

int main(int argc, char **argv)
{
    for (int k = 1; k < argc; k++)
        if (c_streq(argv[k], "-d")) return delayed();

    char buf[64];
    int fd = sys_open("/proc/uptime", O_RDONLY);
    if (fd < 0) { errs("uptime: cannot open /proc/uptime\n"); return 1; }
    int n = sys_read(fd, buf, sizeof buf - 1);
    sys_close(fd);
    if (n <= 0) { errs("uptime: /proc/uptime read failed\n"); return 1; }
    buf[n] = 0;

    long secs = 0;
    int i = 0;
    if (buf[i] < '0' || buf[i] > '9') { errs("uptime: /proc/uptime is malformed\n"); return 1; }
    while (buf[i] >= '0' && buf[i] <= '9') secs = secs * 10 + (buf[i++] - '0');

    for (int k = 1; k < argc; k++)
        if (c_streq(argv[k], "-s")) { outn(secs); outc('\n'); return 0; }

    outs("up ");
    outn(secs / 3600);
    outc(':'); two((secs / 60) % 60);
    outc(':'); two(secs % 60);
    outc('\n');
    return 0;
}
