/* Signals, tested on the machine -- because delivery is a ring-3 mechanism and
 * there is no way to test it anywhere else.
 *
 * A host test can check the disposition table and the default actions; it
 * cannot check that the kernel built a frame on a real user stack, entered a
 * real handler at ring 3, and put every register back. So this is a program,
 * it runs on LogitOS, and it prints SIGTEST_OK / SIGTEST_FAIL on the serial
 * console for tests/boot/run-signal-test.sh to read.
 *
 * THE TEST THAT JUSTIFIES THE WHOLE FILE is fpu_across_handler(). Everything
 * else here would pass on a kernel that delivers signals correctly and does not
 * save the FPU/SSE state -- every handler runs, kill works, the shell works.
 * What breaks on such a kernel is arithmetic, silently, and only when a signal
 * happens to land between a value being computed and being used. So that test
 * loads known bit patterns into xmm0-xmm7, raises a signal from inside the same
 * asm block, and compares the registers on the other side. `make
 * test-signal-negctl` builds the kernel with -DSIGNAL_NO_FPU and REQUIRES this
 * program to fail.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <sys/wait.h>
#include "logit_abi.h"

static int g_fail, g_pass;

static void ck(int cond, const char *what)
{
    if (cond) { g_pass++; return; }
    printf("FAIL: %s\n", what);
    g_fail++;
}

static void cki(int cond, const char *what, long got, long want)
{
    if (cond) { g_pass++; return; }
    printf("FAIL: %s (got %ld, want %ld)\n", what, got, want);
    g_fail++;
}

static long sigq(long what)
{
    long r;
    __asm__ volatile ("int $0x80" : "=a"(r)
                      : "a"((long)SYS_SIGQUERY), "D"(what), "S"(0L), "d"(0L) : "memory");
    return r;
}

/* ------------------------------------------------------------------ 1. it runs */
static volatile sig_atomic_t g_got, g_count;
static void simple(int s) { g_got = s; g_count++; }

static void basic_delivery(void)
{
    g_got = 0; g_count = 0;
    ck(signal(SIGUSR1, simple) != SIG_ERR, "signal(SIGUSR1) installs");
    ck(raise(SIGUSR1) == 0, "raise(SIGUSR1) returns 0");
    cki(g_got == SIGUSR1, "handler ran with the right number", g_got, SIGUSR1);
    cki(g_count == 1, "handler ran exactly once", g_count, 1);

    /* And control came BACK. A handler that runs but never returns correctly
     * looks identical to one that works, right up to the next statement. */
    g_got = 0;
    ck(signal(SIGUSR2, simple) != SIG_ERR, "signal(SIGUSR2) installs");
    raise(SIGUSR2);
    raise(SIGUSR2);
    cki(g_count == 3, "three handler runs, three returns", g_count, 3);

    ck(signal(SIGKILL, simple) == SIG_ERR && errno == EINVAL, "SIGKILL is uncatchable");
    ck(signal(SIGSTOP, simple) == SIG_ERR && errno == EINVAL, "SIGSTOP is uncatchable");
}

/* -------------------------------------------------- 2. THE ONE THAT MATTERS */
static void xmm_clobber(int s)
{
    (void)s;
    /* Deliberately trash every register the frame is supposed to protect. A
     * real handler does this incidentally the moment it touches a double; doing
     * it explicitly means the test does not depend on what the compiler decided
     * to emit for some arithmetic. */
    unsigned long junk = 0xBAADF00DDEADBEEFul;
    __asm__ volatile (
        "movq %0, %%xmm0\n\t" "movq %0, %%xmm1\n\t"
        "movq %0, %%xmm2\n\t" "movq %0, %%xmm3\n\t"
        "movq %0, %%xmm4\n\t" "movq %0, %%xmm5\n\t"
        "movq %0, %%xmm6\n\t" "movq %0, %%xmm7\n\t"
        :: "r"(junk)
        : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
}

static void fpu_across_handler(void)
{
    static unsigned long in[8] = {
        0x0101010101010101ul, 0x0202020202020202ul, 0x0303030303030303ul,
        0x0404040404040404ul, 0x0505050505050505ul, 0x0606060606060606ul,
        0x0707070707070707ul, 0x0808080808080808ul,
    };
    static unsigned long out[8];
    struct sigaction sa;
    sa.sa_handler = xmm_clobber;
    sa.sa_mask = 0;
    sa.sa_flags = 0;
    sa.sa_restorer = 0;
    ck(sigaction(SIGUSR1, &sa, 0) == 0, "sigaction for the FPU test");

    long saved_before = sigq(SIGQ_FPUSAVED);
    int pid = getpid();

    /* The signal is raised from INSIDE the block, between the loads and the
     * stores, so the handler runs with the test's own values live in xmm0-7.
     * That is the only arrangement that tests what the frame is for: a raise()
     * on either side of ordinary C would let the compiler spill and reload,
     * and the test would pass on a kernel that saves nothing.
     *
     * SYS_KILL with LOGIT_KILL_SIGNAL is the syscall; delivery happens at its
     * return, i.e. at the iretq that lands on the next instruction here. */
    __asm__ volatile (
        "movq  0(%[i]), %%xmm0\n\t"  "movq  8(%[i]), %%xmm1\n\t"
        "movq 16(%[i]), %%xmm2\n\t"  "movq 24(%[i]), %%xmm3\n\t"
        "movq 32(%[i]), %%xmm4\n\t"  "movq 40(%[i]), %%xmm5\n\t"
        "movq 48(%[i]), %%xmm6\n\t"  "movq 56(%[i]), %%xmm7\n\t"
        "int $0x80\n\t"
        "movq %%xmm0,  0(%[o])\n\t"  "movq %%xmm1,  8(%[o])\n\t"
        "movq %%xmm2, 16(%[o])\n\t"  "movq %%xmm3, 24(%[o])\n\t"
        "movq %%xmm4, 32(%[o])\n\t"  "movq %%xmm5, 40(%[o])\n\t"
        "movq %%xmm6, 48(%[o])\n\t"  "movq %%xmm7, 56(%[o])\n\t"
        :
        : [i] "r"(in), [o] "r"(out),
          "a"((long)SYS_KILL), "D"((long)pid), "S"((long)SIGUSR1),
          "d"((long)LOGIT_KILL_SIGNAL)
        : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");

    int bad = 0;
    for (int i = 0; i < 8; i++) if (out[i] != in[i]) bad++;
    if (bad) {
        printf("FAIL: %d of 8 XMM registers were corrupted by the handler\n", bad);
        for (int i = 0; i < 8; i++)
            if (out[i] != in[i])
                printf("      xmm%d: %lx -> %lx\n", i, in[i], out[i]);
        printf("      This is what a signal frame with no FPU/SSE state looks like.\n");
        g_fail++;
    } else {
        g_pass++;
    }

    /* The kernel's own view, so the test can also say WHICH kernel it ran on
     * rather than only that the numbers came out wrong. */
    long saved_after = sigq(SIGQ_FPUSAVED);
    cki(saved_after > saved_before, "the kernel saved an FPU area for this frame",
        saved_after - saved_before, 1);
}

/* ------------------------------------------------- 3. a fault becomes a signal */
static jmp_buf g_segv_jmp;
static volatile sig_atomic_t g_segv_seen;

static void segv_handler(int s)
{
    g_segv_seen = s;
    longjmp(g_segv_jmp, 1);
}

static void fault_to_signal(void)
{
    struct sigaction sa;
    sa.sa_handler = segv_handler;
    sa.sa_mask = 0;
    sa.sa_flags = 0;
    sa.sa_restorer = 0;
    ck(sigaction(SIGSEGV, &sa, 0) == 0, "sigaction(SIGSEGV) installs");

    g_segv_seen = 0;
    if (setjmp(g_segv_jmp) == 0) {
        volatile int *p = (volatile int *)0x10;    /* not mapped, not page 0's guard */
        *p = 1;
        ck(0, "the store to an unmapped address should have faulted");
    }
    cki(g_segv_seen == SIGSEGV, "a ring-3 page fault arrived as SIGSEGV",
        g_segv_seen, SIGSEGV);

    /* longjmp out of a handler leaves SIGSEGV blocked -- the kernel added it to
     * the mask on the way in and only sigreturn takes it out again, and this
     * path never reached sigreturn. Unblocking is the program's job, and saying
     * so here is the point: it is the documented cost of escaping a handler. */
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGSEGV);
    ck(sigprocmask(SIG_UNBLOCK, &s, 0) == 0, "unblock SIGSEGV after the longjmp");

    sigset_t pend;
    ck(sigpending(&pend) == 0 && pend == 0, "nothing left pending");

    signal(SIGSEGV, SIG_DFL);
}

/* ------------------------- 4. no handler = the machine behaves as it always did */
static void fault_without_handler(void)
{
    int pid = fork();
    if (pid == 0) {
        volatile int *p = (volatile int *)0x10;
        *p = 1;
        _exit(0);                       /* not reached */
    }
    ck(pid > 0, "fork for the unhandled-fault child");
    int st = 0;
    int r = waitpid(pid, &st, 0);
    cki(r == pid, "reaped the faulting child", r, pid);
    /* 139 is what interrupts.c has always used for a ring-3 fault, and the
     * point of checking it is that this line did NOT change it: a process with
     * no SIGSEGV handler dies exactly as before, and the desktop survives. */
    cki(st == 139, "an unhandled fault still exits 139", st, 139);
}

/* --------------------------------- 5. killing a process that never syscalls */
static void kill_a_compute_loop(void)
{
    int pid = fork();
    if (pid == 0) {
        /* No syscalls at all. This is the case proc_kill()'s own comment says
         * its mark cannot reach; delivery on the timer's return to ring 3 can.
         * If SIGTERM never arrives, this child spins until the harness times
         * out -- which is exactly the failure the test is looking for. */
        volatile unsigned long x = 0;
        for (;;) x++;
    }
    ck(pid > 0, "fork for the compute-loop child");

    /* Give it a moment to actually be running in ring 3 rather than still in
     * the kernel finishing its fork. */
    for (volatile int i = 0; i < 2000000; i++) { }

    ck(kill(pid, SIGTERM) == 0, "kill(child, SIGTERM)");
    int st = 0;
    int r = waitpid(pid, &st, 0);
    cki(r == pid, "reaped the SIGTERM'd child", r, pid);
    cki(st == 128 + SIGTERM, "SIGTERM's default action terminated it", st, 128 + SIGTERM);
}

/* ------------------------------------------------------------- 6. SIGCHLD */
static volatile sig_atomic_t g_chld;
static void chld_handler(int s) { (void)s; g_chld++; }

static void sigchld_arrives(void)
{
    g_chld = 0;
    ck(signal(SIGCHLD, chld_handler) != SIG_ERR, "signal(SIGCHLD) installs");
    int pid = fork();
    if (pid == 0) _exit(7);
    ck(pid > 0, "fork for the SIGCHLD child");
    int st = 0;
    waitpid(pid, &st, 0);
    cki(st == 7, "child's exit status", st, 7);
    cki(g_chld >= 1, "SIGCHLD was delivered", g_chld, 1);
    signal(SIGCHLD, SIG_DFL);
}

/* -------------------------------------------------------------- 7. SIGPIPE */
static volatile sig_atomic_t g_pipe;
static void pipe_handler(int s) { (void)s; g_pipe++; }

static void sigpipe_on_write(void)
{
    int fds[2];
    g_pipe = 0;
    ck(signal(SIGPIPE, pipe_handler) != SIG_ERR, "signal(SIGPIPE) installs");
    ck(pipe(fds) == 0, "pipe()");
    close(fds[0]);                       /* no readers left */
    long n = write(fds[1], "x", 1);
    ck(n < 0, "write to a pipe with no reader fails");
    cki(g_pipe >= 1, "SIGPIPE was delivered", g_pipe, 1);
    close(fds[1]);
    signal(SIGPIPE, SIG_IGN);
}

/* --------------------------------------------------- 8. blocking and pending */
static void mask_and_pending(void)
{
    sigset_t block, pend, old;
    g_got = 0;
    ck(signal(SIGUSR2, simple) != SIG_ERR, "signal(SIGUSR2) for the mask test");
    sigemptyset(&block);
    sigaddset(&block, SIGUSR2);
    ck(sigprocmask(SIG_BLOCK, &block, &old) == 0, "block SIGUSR2");
    raise(SIGUSR2);
    cki(g_got == 0, "a blocked signal is NOT delivered", g_got, 0);
    ck(sigpending(&pend) == 0, "sigpending()");
    ck((pend & (1UL << SIGUSR2)) != 0, "it is reported pending");
    ck(sigprocmask(SIG_SETMASK, &old, 0) == 0, "unblock SIGUSR2");
    cki(g_got == SIGUSR2, "unblocking delivers it", g_got, SIGUSR2);
}

/* ------------------------------------------------- 9. alarm, and EINTR at last */
static volatile sig_atomic_t g_alrm;
static void alrm_handler(int s) { (void)s; g_alrm++; }

static void alarm_and_eintr(void)
{
    int fds[2];
    struct sigaction sa;
    sa.sa_handler = alrm_handler;
    sa.sa_mask = 0;
    sa.sa_flags = 0;                     /* NO SA_RESTART: we want the EINTR */
    sa.sa_restorer = 0;
    ck(sigaction(SIGALRM, &sa, 0) == 0, "sigaction(SIGALRM) installs");
    ck(pipe(fds) == 0, "pipe() for the EINTR test");

    g_alrm = 0;
    ck(alarm(1) == 0, "alarm(1) with no previous alarm");
    char b;
    errno = 0;
    long n = read(fds[0], &b, 1);        /* nothing will ever be written */
    cki(n < 0, "the blocking read returned an error", n, -1);
    cki(errno == EINTR, "and the error is EINTR", errno, EINTR);
    cki(g_alrm == 1, "the SIGALRM handler ran", g_alrm, 1);
    close(fds[0]); close(fds[1]);
    alarm(0);
}

/* ------------------------------------------------------------ 10. SA_RESTART */
static void sa_restart_restarts(void)
{
    int fds[2];
    struct sigaction sa;
    sa.sa_handler = alrm_handler;
    sa.sa_mask = 0;
    sa.sa_flags = SA_RESTART;
    sa.sa_restorer = 0;
    ck(sigaction(SIGALRM, &sa, 0) == 0, "sigaction(SIGALRM, SA_RESTART)");
    ck(pipe(fds) == 0, "pipe() for the SA_RESTART test");

    int pid = fork();
    if (pid == 0) {
        close(fds[0]);
        /* Long enough that the parent is certainly parked in read() and the
         * alarm has certainly fired first. No sleep() dependency: a compute
         * loop is the one timing primitive that needs nothing. */
        for (volatile long i = 0; i < 120000000L; i++) { }
        write(fds[1], "R", 1);
        _exit(0);
    }
    close(fds[1]);
    g_alrm = 0;
    alarm(1);
    char b = 0;
    errno = 0;
    long n = read(fds[0], &b, 1);
    cki(n == 1, "SA_RESTART: the read completed instead of failing", n, 1);
    cki(b == 'R', "and it returned the byte the child wrote", b, 'R');
    cki(g_alrm >= 1, "the handler still ran", g_alrm, 1);
    alarm(0);
    close(fds[0]);
    int st = 0; waitpid(pid, &st, 0);
    signal(SIGALRM, SIG_DFL);
}

int main(void)
{
    long d0 = sigq(SIGQ_DELIVERED), r0 = sigq(SIGQ_RETURNED);

    printf("sigtest: start (delivered=%ld returned=%ld)\n", d0, r0);

    basic_delivery();
    fpu_across_handler();
    fault_to_signal();
    fault_without_handler();
    kill_a_compute_loop();
    sigchld_arrives();
    sigpipe_on_write();
    mask_and_pending();
    alarm_and_eintr();
    sa_restart_restarts();

    long d1 = sigq(SIGQ_DELIVERED), r1 = sigq(SIGQ_RETURNED);
    /* Every frame pushed must have been unwound EXCEPT the one the SIGSEGV
     * handler longjmp'd out of. A larger gap means a frame was pushed twice or
     * a sigreturn silently failed, which nothing else here would notice. */
    cki(d1 - r1 == (d0 - r0) + 1, "one outstanding frame (the longjmp), no more",
        d1 - r1, (d0 - r0) + 1);
    printf("sigtest: delivered=%ld returned=%ld fpusaved=%ld defaulted=%ld dropped=%ld\n",
           d1, r1, sigq(SIGQ_FPUSAVED), sigq(SIGQ_DEFAULTED), sigq(SIGQ_DROPPED));

    if (g_fail) { printf("SIGTEST_FAIL %d/%d checks failed\n", g_fail, g_fail + g_pass); return 1; }
    printf("SIGTEST_OK %d checks passed\n", g_pass);
    return 0;
}
