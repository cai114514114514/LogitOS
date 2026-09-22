/* polltest -- the on-device half of the poll() gate. Prints POLL_TEST_OK only
 * if every line below holds on the real machine.
 *
 * WHAT THIS HALF IS FOR, and what it is NOT for. The lost-wakeup race is proved
 * on the host (tests/unit/poll_test.c), where the event can be injected at the
 * exact instruction that matters and the failure is deterministic in both
 * directions. Trying to provoke it here would be hoping for a race inside a
 * window a few hundred instructions wide. What can only be proved HERE is that
 * the real backends answer correctly: that the pipe wait queue poll() registers
 * on is the same one a blocking read() parks on, that the console's readiness
 * comes from the ring the timer fills, that an eventfd written by another
 * thread wakes a poll in a different thread, and that a timerfd is advanced by
 * the interrupt rather than by anything this program does.
 *
 * IT ALSO MEASURES THE CEILING, which is the number the ABI's poll block cites
 * and which had never been measured on this machine:
 *   - descriptors per process, by opening pipes until one fails;
 *   - threads per process, by creating them until one fails.
 * Both are printed with the name of what failed first, because "about thirteen"
 * and "VMA_MAXAREA, not the thread table" are claims that were carried in
 * include/abi/logit_abi.h without a run behind them.
 *
 * NEGATIVE CONTROL: -DPOLLTEST_NEGCTL_NOPOLL replaces every poll() with a
 * sleep of the same timeout and a claim that nothing was ready -- which is
 * EXACTLY what mini-libc's poll() did before SYS_POLL existed, quoted from its
 * own header. Every readiness assertion below must fail against it. The point
 * is that the old behaviour was not absurd; it was a reasonable answer from a
 * kernel that could not answer, and a gate that cannot tell it from the real
 * thing is not measuring poll().
 */

#include <poll.h>
#include <sys/select.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include "logit_abi.h"

static long sys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

static uint64_t now_ms(void) { return (uint64_t)sys(SYS_MONOTONIC_MS, 0, 0, 0); }

static int g_pass, g_fail;
static void check(int ok, const char *what)
{
    if (ok) g_pass++;
    else  { g_fail++; printf("POLL_FAIL %s\n", what); }
}

#ifdef POLLTEST_NEGCTL_NOPOLL
/* The pre-SYS_POLL implementation, in one function and quoted from the header
 * it used to carry: a regular file is ready, everything else is never ready,
 * and a timeout is a sleep followed by "nothing became ready that we could
 * detect". */
static int xpoll(struct pollfd *fds, nfds_t n, int tmo)
{
    for (nfds_t i = 0; i < n; i++) fds[i].revents = 0;
    if (tmo > 0) usleep((unsigned)tmo * 1000u);
    return 0;
}
#else
#define xpoll poll
#endif

/* --- helper threads ----------------------------------------------------- */

struct wr { int fd; int ms; int close_after; };

static void *wr_fn(void *arg)
{
    struct wr *w = (struct wr *)arg;
    usleep((unsigned)w->ms * 1000u);
    if (w->close_after) close(w->fd);
    else                { char c = 'x'; write(w->fd, &c, 1); }
    return NULL;
}

static void *evwr_fn(void *arg)
{
    int fd = *(int *)arg;
    usleep(150000);
    eventfd_write(fd, 1);
    return NULL;
}

/* ========================================================================== */

int main(void)
{
    int p[2];
    struct pollfd pf[3];
    int rc;
    uint64_t t0, dt;
    pthread_t th;

    printf("polltest start\n");

    /* --- 1-2: a probe on an empty pipe is not ready and does not sleep ---- */
    if (pipe(p) != 0) { printf("POLL_FAIL pipe()\n"); return 1; }
    pf[0].fd = p[0]; pf[0].events = POLLIN; pf[0].revents = 0;
    t0 = now_ms();
    rc = xpoll(pf, 1, 0);
    dt = now_ms() - t0;
    check(rc == 0, "empty-pipe probe returns 0");
    check(dt < 60, "empty-pipe probe does not sleep");

    /* --- 3-4: with a byte in it, the SAME pipe is readable ---------------- */
    { char c = 'a'; write(p[1], &c, 1); }
    pf[0].revents = 0;
    rc = xpoll(pf, 1, 0);
    check(rc == 1, "a pipe with a byte in it is ready");
    check(pf[0].revents == POLLIN, "and reports POLLIN");
    { char c; read(p[0], &c, 1); }

    /* --- 5-6: the timeout --------------------------------------------------
     * 300 ms against a 10 ms tick. The lower bound is deliberately slack --
     * this is a timeout, not a clock test, and the thing being checked is that
     * it waited at all rather than returning instantly. */
    pf[0].revents = 0;
    t0 = now_ms();
    rc = xpoll(pf, 1, 300);
    dt = now_ms() - t0;
    check(rc == 0, "a poll that times out returns 0");
    check(dt >= 250, "and it waited");

    /* --- 7-9: BLOCKED, then woken by a write from another thread ----------
     * The whole point: an infinite poll on a pipe, which the old libc refused
     * with ENOSYS because it had nothing to block on. */
    struct wr w1 = { p[1], 150, 0 };
    pthread_create(&th, NULL, wr_fn, &w1);
    pf[0].revents = 0;
    t0 = now_ms();
    rc = xpoll(pf, 1, 5000);
    dt = now_ms() - t0;
    pthread_join(th, NULL);
    check(rc == 1, "an infinite-ish poll is woken by a write on the pipe");
    check(pf[0].revents & POLLIN, "and reports POLLIN");
    check(dt < 1000, "and it woke on the write, not on the timeout");
    { char c; read(p[0], &c, 1); }

    /* --- 10-11: POLLHUP when the last writer closes ----------------------- */
    struct wr w2 = { p[1], 150, 1 };
    pthread_create(&th, NULL, wr_fn, &w2);
    pf[0].revents = 0;
    rc = xpoll(pf, 1, 5000);
    pthread_join(th, NULL);
    check(rc == 1, "a blocked poll is woken by the writer closing");
    check(pf[0].revents & POLLHUP, "and reports POLLHUP even though only POLLIN was asked");
    close(p[0]);

    /* --- 12-13: POLLNVAL on a closed fd, with an INFINITE timeout ---------
     * The case that hangs forever if this is wrong, which is why the timeout is
     * -1 and not a number. */
    pf[0].fd = p[0];          /* just closed */
    pf[0].events = POLLIN; pf[0].revents = 0;
    t0 = now_ms();
    rc = xpoll(pf, 1, -1);
    dt = now_ms() - t0;
    check(rc == 1 && (pf[0].revents & POLLNVAL), "a closed fd polls as POLLNVAL");
    check(dt < 500, "and an infinite poll over it returns instead of hanging");

    /* --- 14: a NEGATIVE fd is skipped, not refused ------------------------ */
    if (pipe(p) != 0) { printf("POLL_FAIL pipe() 2\n"); return 1; }
    pf[0].fd = -1;    pf[0].events = POLLIN; pf[0].revents = 0;
    pf[1].fd = p[0];  pf[1].events = POLLIN; pf[1].revents = 0;
    { char c = 'z'; write(p[1], &c, 1); }
    rc = xpoll(pf, 2, 0);
    check(rc == 1 && pf[0].revents == 0 && (pf[1].revents & POLLIN),
          "a negative fd is skipped and the real one still answers");
    { char c; read(p[0], &c, 1); }

    /* --- 15-16: THE CONSOLE (F_TTY) ---------------------------------------
     * The only backend whose readiness comes from an interrupt handler rather
     * than from another thread in this process, and the only one whose queue
     * lives outside file.c (ksig_tty_waitq, beside the tick that fills the
     * ring). Untested, it is a branch that runs on every poll of fd 0.
     *
     * fd 1 must be writable: a tty write goes straight at the UART and cannot
     * block, so LPOLLOUT here is a fact and not an estimate. fd 0 must NOT be
     * readable: the harness typed one command line, the shell consumed it, and
     * nothing else is sent for another minute. A 200 ms wait is well inside
     * that and comfortably over the 10 ms tick the console's readiness is
     * quantised to. */
    pf[0].fd = 1; pf[0].events = POLLOUT; pf[0].revents = 0;
    rc = xpoll(pf, 1, 0);
    check(rc == 1 && (pf[0].revents & POLLOUT), "the console is writable");

    pf[0].fd = 0; pf[0].events = POLLIN; pf[0].revents = 0;
    rc = xpoll(pf, 1, 200);
    check(rc == 0, "the console with nothing typed at it is not readable");

    /* --- 17-18: select() over the same pipe ------------------------------- */
    {
        fd_set r;
        struct timeval tv = { 0, 200000 };
        FD_ZERO(&r); FD_SET(p[0], &r);
#ifdef POLLTEST_NEGCTL_NOPOLL
        usleep(200000); rc = 0; FD_ZERO(&r);
#else
        rc = select(p[0] + 1, &r, NULL, NULL, &tv);
#endif
        check(rc == 0, "select on an empty pipe times out at 0");

        { char c = 'q'; write(p[1], &c, 1); }
        FD_ZERO(&r); FD_SET(p[0], &r);
        tv.tv_sec = 0; tv.tv_usec = 200000;
#ifdef POLLTEST_NEGCTL_NOPOLL
        usleep(200000); rc = 0; FD_ZERO(&r);
#else
        rc = select(p[0] + 1, &r, NULL, NULL, &tv);
#endif
        check(rc == 1 && FD_ISSET(p[0], &r), "select sees the byte that arrived");
        { char c; read(p[0], &c, 1); }
    }

    /* --- 17-19: eventfd. A poll in THIS thread, a write in another. ------- */
    {
        int ev = eventfd(0, 0);
        check(ev >= 0, "eventfd() returns a descriptor");
        if (ev >= 0) {
            pthread_create(&th, NULL, evwr_fn, &ev);
            pf[0].fd = ev; pf[0].events = POLLIN; pf[0].revents = 0;
            t0 = now_ms();
            rc = xpoll(pf, 1, 5000);
            dt = now_ms() - t0;
            pthread_join(th, NULL);
            check(rc == 1 && (pf[0].revents & POLLIN) && dt < 1000,
                  "an eventfd written by another thread wakes a blocked poll");
            eventfd_t v = 0;
            check(eventfd_read(ev, &v) == 0 && v == 1,
                  "and the counter read back is exactly what was written");
            close(ev);
        }
    }

    /* --- 20-22: timerfd. Nothing in this program advances it; the 100 Hz
     * interrupt does, which is the property being checked. ---------------- */
    {
        int tf = timerfd_create(CLOCK_MONOTONIC, 0);
        check(tf >= 0, "timerfd_create() returns a descriptor");
        if (tf >= 0) {
            struct itimerspec its;
            memset(&its, 0, sizeof its);
            its.it_value.tv_nsec = 200 * 1000000L;      /* 200 ms, one shot */
            check(timerfd_settime(tf, 0, &its, NULL) == 0, "timerfd_settime arms it");
            pf[0].fd = tf; pf[0].events = POLLIN; pf[0].revents = 0;
            t0 = now_ms();
            rc = xpoll(pf, 1, 5000);
            dt = now_ms() - t0;
            unsigned long long n = 0;
            long got = (rc == 1) ? read(tf, &n, sizeof n) : -1;
            check(rc == 1 && (pf[0].revents & POLLIN) && dt >= 150 && dt < 2000 &&
                  got == (long)sizeof n && n >= 1,
                  "a timerfd fires from the timer interrupt and poll sees it");
            close(tf);
        }
    }

    /* ======================================================================
     * THE CEILING. Two numbers the ABI quotes and nobody had run.
     * ==================================================================== */
    {
        /* Descriptors. Open pipe ends until one fails, then give them back.
         * Counted in ENDS, because that is what a poll set holds -- a server
         * with N connections holds N descriptors, not N/2. The two pipe ends
         * this program is still holding from the checks above are counted, so
         * the total is what the PROCESS held, not what this loop added. */
        int fds[64], nf = 0, err = 0;
        int held_before = 5;      /* 0,1,2 from the shell + p[0],p[1] above */
        for (;;) {
            int q[2];
            if (nf + 2 > 64) break;
            if (pipe(q) != 0) { err = errno; break; }
            fds[nf++] = q[0];
            fds[nf++] = q[1];
        }
        printf("POLL_CEILING fds: %d held at once (%d already open + %d new ends);\n",
               held_before + nf, held_before, nf);
        printf("POLL_CEILING      the next pipe() failed with errno %d. NFD in\n", err);
        printf("POLL_CEILING      c/kernel/exec/proc.h is what binds a poll() server.\n");
        for (int i = 0; i < nf; i++) close(fds[i]);
    }
    {
        /* Threads. Create sleepers until one fails. The number to compare it
         * against is LOGIT_THREADS_MAX = 64 (the thread table) -- if the answer
         * is well under that, the thread table is NOT what binds, which is what
         * include/abi/logit_abi.h claims and had never run. */
        pthread_t t[64];
        int nt = 0, terr = 0;
        for (; nt < 64; nt++) {
            terr = pthread_create(&t[nt], NULL, (void *(*)(void *))usleep,
                                  (void *)(long)400000);
            if (terr != 0) break;
        }
        printf("POLL_CEILING threads: %d created, then pthread_create failed (rc %d).\n",
               nt, terr);
        printf("POLL_CEILING      LOGIT_THREADS_MAX is %d, so the thread table is %s.\n",
               LOGIT_THREADS_MAX, nt >= LOGIT_THREADS_MAX ? "what binds" : "NOT what binds");
        for (int i = 0; i < nt; i++) pthread_join(t[i], NULL);
    }

    printf("polltest: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0) printf("POLL_TEST_OK\n");
    else             printf("POLL_TEST_FAILED\n");
    return g_fail ? 1 : 0;
}
