#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "logit_abi.h"
#include "serial.h"
#include "wm.h"
#include "sched.h"
#include "usercopy.h"
#include "proc.h"
#include "file.h"
#include "vfs.h"
#include "rtc.h"
#include "net.h"
#include "icmp.h"
#include "dns.h"
#include "sock.h"
#include "logit_pack.h"     /* generated: the port/flags unpack for SYS_SOCK_OPEN */
#include "img.h"
#include "kheap.h"
#include "percpu.h"
#include "kprintf.h"
#include "pit.h"
#include "ktime.h"
/* Path-qualified, and it has to be: mini-libc ships c/apps/libc/include/sys/
 * wait.h, that directory is in INCDIRS, and it sorts BEFORE c/kernel/core -- so
 * a bare #include "wait.h" from OUTSIDE c/kernel/core silently resolves to the
 * userland one, and sched_sleep_ms below becomes an undeclared function. (Files
 * in c/kernel/core get away with the bare form only because a quoted include
 * searches the including file's own directory first.) */
#include "kernel/core/wait.h"   /* M27 sched_sleep_ms: the kernel's ONE sleeper */
#include "snd.h"
#include "mm.h"          /* mm_syscall: SYS_MMAP / SYS_MUNMAP / SYS_MEMINFO */
#include "settings.h"    /* settings_syscall: SYS_SETTING_* */
#include "clipboard.h"   /* clip_syscall:   SYS_CLIP_SET / _GET / _INFO */
#include "notify.h"      /* notify_syscall: SYS_NOTIFY */
#include "kbench.h"      /* per-syscall accounting, off by default */

/* M25 P1: which syscalls run WITHOUT the Big Kernel Lock (interrupt_handler skips
 * the BKL for these; they self-lock via fine-grained locks). Only the kheap stress
 * for now -- the proof that concurrent, BKL-free kmalloc works. */
int syscall_is_bkl_free(int n) { return n == SYS_KHEAP_STRESS; }

/* BKL-FREE concurrent kmalloc/kfree stress (the P1 gate). Runs on N cores at once,
 * hammering g_kheap_lock under real contention. Each call stamps a per-call tag
 * (from `seed`, stable across thread migration) into every byte of a batch of
 * blocks, reads it back, then frees -- a freelist race that hands one block to two
 * callers makes their tags clash, caught as `bad`. VOLATILE byte access keeps the
 * fill/verify out of XMM, so the QEMU MTTCG-on-ARM FP artifact cannot false-flag. */
static long kheap_stress(long iters, int size, unsigned long seed)
{
    long bad = 0;
    for (long it = 0; it < iters; it++) {
        unsigned char *blk[8];
        unsigned char tag = (unsigned char)(seed * 131u + (unsigned long)it * 7u + 1u);
        for (int k = 0; k < 8; k++) {
            blk[k] = kmalloc((size_t)size);
            volatile unsigned char *v = blk[k];
            if (v) for (int j = 0; j < size; j++) v[j] = tag;
        }
        for (int k = 0; k < 8; k++) {
            volatile unsigned char *v = blk[k];
            if (!v) { bad++; continue; }
            for (int j = 0; j < size; j++) if (v[j] != tag) { bad++; break; }
        }
        for (int k = 0; k < 8; k++) if (blk[k]) kfree(blk[k]);
    }
    return bad;
}

static void syscall_do(struct registers *r);

/* SYS_PROCS / SYS_KILL live in proc.c (the table they read is static there);
 * these are prototyped here rather than in proc.h because that header belongs
 * to the process line, exactly as proc.c prototypes proc_fork_stats() locally.
 *
 * proc_kill_armed() is the gate for the deferred kill: a marked process runs
 * proc_exit() ON ITSELF at its next kernel entry, which is what makes killing
 * another process safe here (see the long comment above proc_kill()). Off, it
 * costs one load of a global and one never-taken branch per syscall. */
long proc_syscall(long num, long a, long b, long c);
int  proc_kill_armed(void);
void proc_kill_check(void);      /* does not return if THIS process is the victim */

/* The dispatcher proper is wrapped so the per-number accounting has exactly one
 * place to live, instead of being repeated at the ~60 `return`s below. When the
 * counters are disarmed this is a load of a global, a branch, and a tail call. */
void syscall_dispatch(struct registers *r)
{
    /* A process marked by SYS_KILL dies here, on its own stack, before it gets
     * to make the call. This is the ONLY point at which a killed process is
     * torn down, so the teardown is the ordinary proc_exit() one.
     *
     * NOT on a BKL-free syscall. interrupt_handler skips the big kernel lock
     * for those (syscall_is_bkl_free), and proc_exit() -> file_close() ->
     * thread_exit() is written for a caller that holds it -- exactly as every
     * other exit path in the tree does. Dying here would be the one place that
     * ran the teardown without it. The mark is durable, so the victim simply
     * dies at its next ordinary syscall instead; nothing is lost but a few
     * microseconds, and only for a process calling the BKL-free stress call. */
    if (__builtin_expect(proc_kill_armed(), 0) && !syscall_is_bkl_free((int)r->rax))
        proc_kill_check();

    if (__builtin_expect(!g_kb_stat, 1)) { syscall_do(r); return; }
    uint64_t n = r->rax, t0 = kb_rdtsc();
    syscall_do(r);
    /* r->rax is the RESULT by now, so the number has to be the one saved above.
     * execve rewrites the whole frame; it is still the right number to charge. */
    if (n < KB_NSYS) { g_kb_sys_n[n]++; g_kb_sys_cyc[n] += kb_rdtsc() - t0; }
}

static void syscall_do(struct registers *r)
{
    switch (r->rax) {
    case SYS_WRITE: {
        const char *buf = (const char *)r->rsi;     /* user pointer, mapped */
        long len = (long)r->rdx;
        int  fd  = (int)r->rdi;
        if (len < 0 || !user_range_ok(buf, (uint64_t)len, 0)) { r->rax = (uint64_t)-1; return; }
        struct proc *p = proc_current();
        struct file *f = p ? proc_fd_get(p, fd) : NULL;
        if (f) { r->rax = (uint64_t)file_write(f, buf, len); return; }
        if (fd == 1 || fd == 2) {                    /* default console: serial (GUI apps, init) */
            for (long i = 0; i < len; i++) serial_putc(buf[i]);
            r->rax = (uint64_t)len;
            return;
        }
        r->rax = (uint64_t)-1;
        return;
    }
    case SYS_READ: {
        char *buf = (char *)r->rsi;
        long len = (long)r->rdx;
        if (len < 0 || !user_range_ok(buf, (uint64_t)len, 1)) { r->rax = (uint64_t)-1; return; }
        struct proc *p = proc_current();
        struct file *f = p ? proc_fd_get(p, (int)r->rdi) : NULL;
        if (!f) { r->rax = (uint64_t)-1; return; }
        r->rax = (uint64_t)file_read(f, buf, len);
        return;
    }
    case SYS_OPEN: {
        char path[128];
        if (user_copy_string(path, sizeof path, (const char *)r->rdi) < 0) { r->rax = (uint64_t)-1; return; }
        struct proc *p = proc_current();
        if (!p) { r->rax = (uint64_t)-1; return; }
        char abs[128]; proc_resolve(p, path, abs, sizeof abs);
        struct file *f = file_open_vfs(abs, (int)r->rsi);
        if (!f) { r->rax = (uint64_t)-1; return; }
        int fd = proc_fd_alloc(p, f);
        if (fd < 0) { file_close(f); r->rax = (uint64_t)-1; return; }
        r->rax = (uint64_t)fd;
        return;
    }
    case SYS_CLOSE: {
        struct proc *p = proc_current(); int fd = (int)r->rdi;
        if (!p || fd < 0 || fd >= NFD || !p->fd[fd]) { r->rax = (uint64_t)-1; return; }
        file_close(p->fd[fd]); p->fd[fd] = NULL; r->rax = 0;
        return;
    }
    case SYS_LSEEK: {
        struct proc *p = proc_current();
        struct file *f = p ? proc_fd_get(p, (int)r->rdi) : NULL;
        if (!f) { r->rax = (uint64_t)-1; return; }
        r->rax = (uint64_t)file_lseek(f, (long)r->rsi, (int)r->rdx);
        return;
    }
    case SYS_DUP: {
        struct proc *p = proc_current();
        struct file *f = p ? proc_fd_get(p, (int)r->rdi) : NULL;
        if (!f) { r->rax = (uint64_t)-1; return; }
        file_dup(f);
        int fd = proc_fd_alloc(p, f);
        if (fd < 0) { file_close(f); r->rax = (uint64_t)-1; return; }
        r->rax = (uint64_t)fd;
        return;
    }
    case SYS_SETNB: {
        struct proc *p = proc_current();
        struct file *f = p ? proc_fd_get(p, (int)r->rdi) : NULL;
        if (!f) { r->rax = (uint64_t)-1; return; }
        f->flags |= O_NONBLOCK;
        r->rax = 0;
        return;
    }
    case SYS_FSYNC: {
        struct proc *p = proc_current();
        struct file *f = p ? proc_fd_get(p, (int)r->rdi) : NULL;
        if (!f) { r->rax = (uint64_t)-1; return; }
        r->rax = (uint64_t)file_fsync(f);
        return;
    }
    case SYS_DUP2: {
        struct proc *p = proc_current(); int old = (int)r->rdi, nw = (int)r->rsi;
        struct file *f = p ? proc_fd_get(p, old) : NULL;
        if (!f || nw < 0 || nw >= NFD) { r->rax = (uint64_t)-1; return; }
        if (old != nw) {
            if (p->fd[nw]) file_close(p->fd[nw]);
            file_dup(f); p->fd[nw] = f;
        }
        r->rax = (uint64_t)nw;
        return;
    }
    case SYS_GETCWD: {
        struct proc *p = proc_current(); char *buf = (char *)r->rdi; int max = (int)r->rsi;
        if (!p || max <= 0 || !user_range_ok(buf, (uint64_t)max, 1)) { r->rax = (uint64_t)-1; return; }
        int i = 0; for (; i < max - 1 && p->cwd[i]; i++) buf[i] = p->cwd[i]; buf[i] = 0;
        r->rax = (uint64_t)i;
        return;
    }
    case SYS_CHDIR: {
        char path[128];
        if (user_copy_string(path, sizeof path, (const char *)r->rdi) < 0) { r->rax = (uint64_t)-1; return; }
        struct proc *p = proc_current(); if (!p) { r->rax = (uint64_t)-1; return; }
        char abs[128]; proc_resolve(p, path, abs, sizeof abs);
        if (vfs_count(abs) < 0) { r->rax = (uint64_t)-1; return; }   /* not a directory */
        int i = 0; for (; i < (int)sizeof(p->cwd) - 1 && abs[i]; i++) p->cwd[i] = abs[i]; p->cwd[i] = 0;
        r->rax = 0;
        return;
    }
    /* --- filesystem + info syscalls: proc-level (work for CLI processes too,
     *     which have no window). Paths resolve against the process cwd. --- */
    case SYS_READ_FILE: {
        char name[128], abs[128]; int max = (int)r->rdx;
        struct proc *p = proc_current();
        if (!p || max < 0 || user_copy_string(name, sizeof name, (const char *)r->rdi) < 0) { r->rax = (uint64_t)-1; return; }
        if (max > 0 && !user_range_ok((void *)r->rsi, (uint64_t)max, 1)) { r->rax = (uint64_t)-1; return; }
        proc_resolve(p, name, abs, sizeof abs);
        r->rax = (uint64_t)vfs_read(abs, (void *)r->rsi, max);
        return;
    }
    case SYS_WRITE_FILE: {
        char path[128], abs[128]; int size = (int)r->rdx;
        struct proc *p = proc_current();
        if (!p || size < 0 || user_copy_string(path, sizeof path, (const char *)r->rdi) < 0) { r->rax = (uint64_t)-1; return; }
        if (size > 0 && !user_range_ok((const void *)r->rsi, (uint64_t)size, 0)) { r->rax = (uint64_t)-1; return; }
        proc_resolve(p, path, abs, sizeof abs);
        r->rax = (uint64_t)vfs_write(abs, (const void *)r->rsi, size);
        return;
    }
    case SYS_DELETE_FILE: {
        char path[128], abs[128]; struct proc *p = proc_current();
        if (!p || user_copy_string(path, sizeof path, (const char *)r->rdi) < 0) { r->rax = (uint64_t)-1; return; }
        proc_resolve(p, path, abs, sizeof abs);
        r->rax = (uint64_t)vfs_delete(abs);
        return;
    }
    case SYS_MKDIR: {
        char path[128], abs[128]; struct proc *p = proc_current();
        if (!p || user_copy_string(path, sizeof path, (const char *)r->rdi) < 0) { r->rax = (uint64_t)-1; return; }
        proc_resolve(p, path, abs, sizeof abs);
        r->rax = (uint64_t)vfs_mkdir(abs);
        return;
    }
    case SYS_DIR_COUNT: {
        char path[128], abs[128]; struct proc *p = proc_current();
        if (!p || user_copy_string(path, sizeof path, (const char *)r->rdi) < 0) { r->rax = (uint64_t)-1; return; }
        proc_resolve(p, path, abs, sizeof abs);
        r->rax = (uint64_t)vfs_count(abs);
        return;
    }
    case SYS_DIR_NAME: {
        char dir[128], abs[128]; int i = (int)r->rsi; struct proc *p = proc_current();
        if (!p || user_copy_string(dir, sizeof dir, (const char *)r->rdi) < 0) { r->rax = (uint64_t)-1; return; }
        if (!user_range_ok((void *)r->rdx, 64, 1)) { r->rax = (uint64_t)-1; return; }
        proc_resolve(p, dir, abs, sizeof abs);
        if (i < 0 || i >= vfs_count(abs)) { r->rax = (uint64_t)-1; return; }
        { const char *nm = vfs_ent_name(abs, i); char *out = (char *)r->rdx;
          int j = 0; for (; j < 63 && nm && nm[j]; j++) out[j] = nm[j]; out[j] = 0; }
        r->rax = (uint64_t)(vfs_ent_is_dir(abs, i) ? -2 : vfs_ent_size(abs, i));
        return;
    }
    case SYS_FILE_COUNT:
        r->rax = (uint64_t)vfs_count("/");
        return;
    case SYS_FILE_NAME: {
        int i = (int)r->rdi; int max = (int)r->rdx;
        if (i < 0 || i >= vfs_count("/") || max <= 0 || !user_range_ok((void *)r->rsi, (uint64_t)max, 1)) { r->rax = (uint64_t)-1; return; }
        { const char *nm = vfs_ent_name("/", i); char *out = (char *)r->rsi;
          int j = 0; for (; j < max - 1 && nm && nm[j]; j++) out[j] = nm[j]; out[j] = 0; }
        r->rax = (uint64_t)vfs_ent_size("/", i);
        return;
    }
    case SYS_GET_TIME: {
        /* MEASURED: 402,175 of these in a 14-second boot -- 16% of every
         * syscall the machine made -- at 5.4 us each, because rtc_now() walks
         * the CMOS index/data ports with a double-read agreement loop. 2.18
         * SECONDS of CPU, all of it under the big kernel lock, so it is also
         * 2.18 seconds that every other core spent waiting.
         *
         * The caller is a clock face polling for the time it should display.
         * The RTC's answer changes once a second. Serving it from a cache that
         * is at most 100 ms old is therefore not an approximation of the old
         * behaviour -- it is the same value, ten times finer than the source's
         * own resolution, for 1/40000 of the port traffic.
         *
         * timer_ticks() and not time_mono_ns() as the cache clock: the tick is
         * 15 ns to read and the ns clock is 73 ns (kbench), and a 100 ms window
         * has no use for nanoseconds. Ten ticks = 100 ms.
         *
         * Statics, not per-process: this is one hardware clock and every reader
         * wants the same answer. Reached only with the BKL held (int 0x80 is
         * not in syscall_is_bkl_free), so no lock of its own. */
        if (!user_range_ok((void *)r->rdi, sizeof(struct rtc_time), 1)) { r->rax = (uint64_t)-1; return; }
        static struct rtc_time cached;
#ifdef KBENCH_NEGCTL
        rtc_now(&cached);                          /* the old behaviour: the CMOS, every time */
#else
        static uint64_t cached_at;                 /* timer_ticks(); 0 = never read */
        uint64_t now = timer_ticks();
        if (!cached_at || now - cached_at >= 10) {
            rtc_now(&cached);
            cached_at = now ? now : 1;             /* 0 means "never", so never store 0 */
        }
#endif
        user_copy_to((void *)r->rdi, &cached, sizeof cached);
        r->rax = 0;
        return;
    }
    case SYS_MONOTONIC_MS:
        /* Handled here beside SYS_GET_TIME, not in wm_gui_syscall: a clock is
         * not a GUI service. /bin/as, vidcheck and every coreutil are windowless
         * processes, and the wm route answers -1 for those.
         *
         * No user_range_ok: the answer is the return value, so there is no user
         * pointer to validate -- and no failure mode either. 10 ms granular; see
         * the SYS_MONOTONIC_MS comment in logit_abi.h. */
        r->rax = timer_ms();
        return;

    /* ---- M28 time subsystem ---------------------------------------------
     * Beside SYS_MONOTONIC_MS and for the same reason: a clock is not a GUI
     * service, and /bin/as, the coreutils and vidcheck are windowless. */
    case SYS_CLOCK_GETTIME: {
        struct logit_timespec ts;
        int64_t s = 0, ns = 0;
        if (!user_range_ok((void *)r->rsi, sizeof ts, 1)) { r->rax = (uint64_t)-1; return; }
        if (time_clock_gettime((int)r->rdi, &s, &ns) < 0) { r->rax = (uint64_t)-1; return; }
        ts.tv_sec = (long)s; ts.tv_nsec = (long)ns;
        user_copy_to((void *)r->rsi, &ts, sizeof ts);
        r->rax = 0;
        return;
    }
    case SYS_NANOSLEEP: {
        struct logit_timespec req;
        if (!user_range_ok((const void *)r->rdi, sizeof req, 0)) { r->rax = (uint64_t)-1; return; }
        user_copy_from(&req, (const void *)r->rdi, sizeof req);
        if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000L) {
            r->rax = (uint64_t)-1; return;
        }
        uint64_t want = (uint64_t)req.tv_sec * NS_PER_SEC + (uint64_t)req.tv_nsec;
        if (want > 3600ull * NS_PER_SEC) want = 3600ull * NS_PER_SEC;   /* one hour cap */
        uint64_t deadline = time_mono_ns() + want;
        uint64_t tick_ns  = timer_ns_per_tick();
        /* NOT a second blocking mechanism -- deliberately. sched_sleep_ms() in
         * c/kernel/core/wait.c is the kernel's sleeper: it UNLINKS the thread
         * from the run ring, so a sleeper consumes no scheduler time at all, and
         * its deadline is expired from the timer IRQ ahead of the BKL acquire.
         * Building a rival here would give the kernel two sleepers, which is one
         * more than any kernel should have.
         *
         * The two halves, and why there are two:
         *   - MORE than two ticks left: park. Aim one tick short of the deadline
         *     so the park cannot overshoot it -- its deadline is in ticks and
         *     rounds up, and overshooting is the failure mode a nanosecond
         *     nanosleep exists to avoid.
         *   - the LAST tick: yield in a loop against the ns clock. Parking here
         *     would round a nanosleep(1ms) up to a whole 10 ms, which is exactly
         *     the imprecision this subsystem was built to remove. Bounded by one
         *     tick of yielding, no matter how long the total sleep.
         *
         * When wait.c grows a nanosecond-deadline park, the whole loop collapses
         * into one call to it and the arithmetic above does not change. */
        for (;;) {
            uint64_t now = time_mono_ns();
            if (now >= deadline) break;
            uint64_t rem = deadline - now;
            if (rem > 2 * tick_ns) sched_sleep_ms((unsigned)((rem - tick_ns) / NS_PER_MS));
            else                   schedule();
        }
        if (r->rsi && user_range_ok((void *)r->rsi, sizeof req, 1)) {
            struct logit_timespec rem = { 0, 0 };
            user_copy_to((void *)r->rsi, &rem, sizeof rem);
        }
        r->rax = 0;
        return;
    }
    case SYS_CLOCK_INFO: {
        struct logit_clockinfo ci;
        int set = (int)r->rsi;
        if (!user_range_ok((void *)r->rdi, sizeof ci, 1)) { r->rax = (uint64_t)-1; return; }
        if (set >= 0 && time_set_source(set) < 0) { r->rax = (uint64_t)-1; return; }
        int src = time_get_source();
        ci.source   = src;
        ci.nsources = TIMESRC_N;
        ci.hz       = time_source_hz(src);
        ci.res_ns   = time_source_res_ns(src);
        ci.mono_ns  = time_mono_ns();
        ci.real_ns  = time_real_ns();
        ci.reads    = time_mono_reads();
        ci.backsteps = time_mono_backsteps();
        ci.backstep_max_ns = time_mono_backstep_max_ns();
        ci.timers_queued = (unsigned long long)ktimer_queued();
        ci.timers_fired  = ktimer_fired();
        ci.cores_seen    = time_cores_seen();
        { const char *n = time_source_name(src); int i = 0;
          for (; i < (int)sizeof ci.name - 1 && n[i]; i++) ci.name[i] = n[i];
          for (; i < (int)sizeof ci.name; i++) ci.name[i] = 0; }
        user_copy_to((void *)r->rdi, &ci, sizeof ci);
        r->rax = 0;
        return;
    }
    case SYS_EXIT:
        proc_exit((int)r->rdi);  /* zombie + close fds + mark window dead; never returns */
        return;
    case SYS_YIELD:
        /* Handled HERE, not in wm_gui_syscall: a CLI process (sleep, an .as
         * script's poll loop) has no window, and the wm route returned -1
         * WITHOUT yielding -- `sleep` busy-burned its whole core. */
        schedule();
        r->rax = 0;
        return;
    case SYS_FORK:
        r->rax = (uint64_t)proc_fork(r);
        return;
    case SYS_EXECVE:
        r->rax = (uint64_t)proc_execve(r);   /* on success, rewrites r and "returns" into the new program */
        return;
    case SYS_PIPE: {
        struct proc *p = proc_current();
        int *ufds = (int *)r->rdi;
        if (!p || !user_range_ok(ufds, sizeof(int) * 2, 1)) { r->rax = (uint64_t)-1; return; }
        struct file *rf = 0, *wf = 0;
        if (file_pipe(&rf, &wf) < 0) { kprintf("[pipe] file_pipe failed (file table/kheap)\n"); r->rax = (uint64_t)-1; return; }
        int rfd = proc_fd_alloc(p, rf);
        int wfd = proc_fd_alloc(p, wf);
        if (rfd < 0 || wfd < 0) {
            /* Unhook any installed fd BEFORE closing: file_close drops the last
             * ref, the slot becomes reusable, and a dangling p->fd[] entry would
             * later close the slot's NEW owner (proc_exit/SYS_CLOSE). */
            kprintf("[pipe] fd table full (pid %d)\n", p->pid);
            if (rfd >= 0) p->fd[rfd] = NULL;
            if (wfd >= 0) p->fd[wfd] = NULL;
            file_close(rf); file_close(wf); r->rax = (uint64_t)-1; return;
        }
        int fds[2] = { rfd, wfd };
        user_copy_to(ufds, fds, sizeof fds);
        r->rax = 0;
        return;
    }
    case SYS_GETPID: {
        struct proc *p = proc_current();
        r->rax = p ? (uint64_t)p->pid : (uint64_t)-1;
        return;
    }
    case SYS_CPU_INDEX:
        /* index of the core running this syscall (under the BKL). SMP proof: a
         * child that observes a different index than another ran on another core. */
        r->rax = (uint64_t)(long)this_cpu()->index;
        return;
    case SYS_KHEAP_STRESS: {   /* BKL-FREE (see syscall_is_bkl_free): concurrent kmalloc stress */
        long iters = (long)r->rdi; int size = (int)r->rsi; unsigned long seed = (unsigned long)r->rdx;
        if (size < 8 || size > 1024 || iters < 0) { r->rax = (uint64_t)-1; return; }
        r->rax = (uint64_t)kheap_stress(iters, size, seed);
        return;
    }
    case SYS_WAITPID: {
        int status = 0;
        long rc = proc_waitpid((int)r->rdi, &status);
        if (rc >= 0 && r->rsi) user_copy_to((void *)r->rsi, &status, sizeof(int));
        r->rax = (uint64_t)rc;
        return;
    }
    /* Networking: handled here (not in wm_gui_syscall) so CLI processes -- e.g.
     * the `net` coreutil run from the Terminal's shell, which has no GUI window --
     * can use them too. All non-blocking (start + poll); net_poll is pumped by the
     * WM loop while the caller yields. */
    case SYS_NET_INFO: {
        if (!net_up()) { r->rax = 0; return; }
        struct logit_netinfo *ni = (struct logit_netinfo *)r->rdi;
        if (!user_range_ok(ni, sizeof *ni, 1)) { r->rax = (uint64_t)-1; return; }
        ni->ip = net_cfg.ip; ni->mask = net_cfg.mask; ni->gw = net_cfg.gw;
        for (int i = 0; i < 6; i++) ni->mac[i] = net_cfg.mac[i];
        r->rax = 1; return;
    }
    case SYS_NET_PING:
        r->rax = (uint64_t)(long)(net_up() ? icmp_ping((uint32_t)r->rdi) : -1);
        return;
    case SYS_NET_PING_RTT: {
        int t = icmp_last_rtt();                    /* ticks (10 ms) -> ms */
        r->rax = (uint64_t)(long)(t < 0 ? -1 : t * 10);
        return;
    }
    case SYS_NET_DNS: {
        if (!net_up()) { r->rax = (uint64_t)-1; return; }
        char name[256];                             /* DNS names are <= 253 bytes */
        if (user_copy_string(name, sizeof name, (const char *)r->rdi) < 0) { r->rax = (uint64_t)-1; return; }
        dns_start(name);
        r->rax = 0; return;
    }
    case SYS_NET_DNS_RESULT:
        r->rax = (uint64_t)(long)(int)dns_result();
        return;

    /* M27 non-blocking sockets. Handled here rather than in wm_gui_syscall for
     * the same reason as SYS_NET_*: they are a process-level service, not a
     * window one, and /bin/socktest (and any future CLI HTTP client) has no
     * window. Every one of these returns without waiting -- which is the whole
     * change. The blocking SYS_HTTP_GET runs with the BKL held for the length of
     * a fetch, which is exactly why the desktop froze; these hold it for a memcpy
     * and the real work happens in net_poll() on the WM thread. */
    case SYS_SOCK_OPEN: {
        char host[SOCK_HOST_MAX];
        struct proc *p = proc_current();
        if (!p) { r->rax = (uint64_t)(long)SOCK_E_ARG; return; }
        if (user_copy_string(host, sizeof host, (const char *)r->rdi) < 0)
            { r->rax = (uint64_t)(long)SOCK_E_ARG; return; }
        int port  = LOGIT_SOCK_OPEN_B_PORT(r->rsi);
        int flags = LOGIT_SOCK_OPEN_B_FLAGS(r->rsi);
        r->rax = (uint64_t)(long)sock_open(host, port, flags, p->pid);
        return;
    }
    case SYS_SOCK_POLL: {
        struct proc *p = proc_current();
        if (!p) { r->rax = (uint64_t)(long)SOCK_E_ARG; return; }
        r->rax = (uint64_t)(long)sock_poll_bits((int)r->rdi, p->pid);
        return;
    }
    case SYS_SOCK_SEND: {
        struct proc *p = proc_current();
        int len = (int)r->rdx;
        if (!p || len < 0 || (len > 0 && !user_range_ok((const void *)r->rsi, (uint64_t)len, 0)))
            { r->rax = (uint64_t)(long)SOCK_E_ARG; return; }
        r->rax = (uint64_t)(long)sock_send((int)r->rdi, (const void *)r->rsi, len, p->pid);
        return;
    }
    case SYS_SOCK_RECV: {
        struct proc *p = proc_current();
        int max = (int)r->rdx;
        if (!p || max <= 0 || !user_range_ok((void *)r->rsi, (uint64_t)max, 1))
            { r->rax = (uint64_t)(long)SOCK_E_ARG; return; }
        r->rax = (uint64_t)(long)sock_recv((int)r->rdi, (void *)r->rsi, max, p->pid);
        return;
    }
    case SYS_SOCK_ALPN: {
        struct proc *p = proc_current();
        int max = (int)r->rdx;
        if (!p || max <= 0 || !user_range_ok((void *)r->rsi, (uint64_t)max, 1))
            { r->rax = (uint64_t)(long)SOCK_E_ARG; return; }
        r->rax = (uint64_t)(long)sock_alpn((int)r->rdi, (char *)r->rsi, max, p->pid);
        return;
    }
    case SYS_SOCK_CLOSE: {
        struct proc *p = proc_current();
        if (!p) { r->rax = (uint64_t)(long)SOCK_E_ARG; return; }
        r->rax = (uint64_t)(long)sock_close((int)r->rdi, p->pid);
        return;
    }

    case SYS_IMG_DECODE: {
        /* Decode an image file (PNG/GIF) in-kernel and hand the RGBA back to the
         * caller's buffer -- so the Preview app needs no codec/libc of its own. */
        struct logit_imgreq req;
        struct proc *p = proc_current();
        if (!p || user_copy_from(&req, (const void *)r->rdi, sizeof req) < 0) { r->rax = (uint64_t)-1; return; }
        char path[128], abs[128];
        if (user_copy_string(path, sizeof path, req.path) < 0) { r->rax = (uint64_t)-1; return; }
        if (req.max <= 0 || !user_range_ok(req.rgba, (uint64_t)req.max, 1)) { r->rax = (uint64_t)-1; return; }
        proc_resolve(p, path, abs, sizeof abs);
        int sz = vfs_size(abs);
        if (sz <= 0) { r->rax = (uint64_t)-1; return; }
        uint8_t *file = (uint8_t *)kmalloc((unsigned)sz);
        if (!file) { r->rax = (uint64_t)-1; return; }
        int n = vfs_read(abs, file, sz);
        struct image im;
        if (n <= 0 || img_decode(file, n, &im) != 0) { kfree(file); r->rax = (uint64_t)-1; return; }
        kfree(file);
        long need = (long)im.w * im.h * 4;
        if (need <= 0 || need > req.max) { img_free(&im); r->rax = (uint64_t)-1; return; }
        user_copy_to(req.rgba, im.rgba, (uint64_t)need);
        req.w = im.w; req.h = im.h;
        user_copy_to((void *)r->rdi, &req, sizeof req);     /* return dimensions */
        img_free(&im);
        r->rax = 0;
        return;
    }
    case SYS_RENAME: {
        char o[128], n[128], ao[128], an[128];
        struct proc *p = proc_current();
        if (!p || user_copy_string(o, sizeof o, (const char *)r->rdi) < 0) { r->rax = (uint64_t)-1; return; }
        if (user_copy_string(n, sizeof n, (const char *)r->rsi) < 0) { r->rax = (uint64_t)-1; return; }
        proc_resolve(p, o, ao, sizeof ao);
        proc_resolve(p, n, an, sizeof an);
        r->rax = (uint64_t)vfs_rename(ao, an);
        return;
    }

    /* M29 audio. Forwarded whole to c/kernel/audio/snd.c rather than expanded
     * into six cases here: which argument is a user buffer and how long it is
     * are audio facts, and they belong beside the code that knows them. Handled
     * here and not in wm_gui_syscall because a decoder is a CLI process with no
     * window -- the same reason the socket calls are here. */
    case SYS_SND_INFO:
    case SYS_SND_OPEN:
    case SYS_SND_WRITE:
    case SYS_SND_AVAIL:
    case SYS_SND_CLOSE:
    case SYS_SND_STATE:
        r->rax = (uint64_t)snd_syscall((long)r->rax, (long)r->rdi,
                                       (long)r->rsi, (long)r->rdx);
        return;

    case SYS_MMAP:
    case SYS_MUNMAP:
    case SYS_MEMINFO:
        r->rax = (uint64_t)mm_syscall((long)r->rax, (long)r->rdi,
                                      (long)r->rsi, (long)r->rdx);
        return;

    /* Settings. Handled here and not in wm_gui_syscall because a CLI process
     * -- a shell script, an .as program, a future `defaults` coreutil -- has
     * every right to read and write the machine's configuration and has no
     * window for the WM to resolve it through. */
    case SYS_SETTING_GET:
    case SYS_SETTING_SET:
    case SYS_SETTING_ENUM:
    case SYS_SETTING_CTL:
        r->rax = (uint64_t)settings_syscall((long)r->rax, (long)r->rdi,
                                            (long)r->rsi, (long)r->rdx);
        return;

    case SYS_PROCS:
    case SYS_KILL:
        r->rax = (uint64_t)proc_syscall((long)r->rax, (long)r->rdi,
                                        (long)r->rsi, (long)r->rdx);
        return;

    /* The clipboard and notifications are kernel services, not window-manager
     * ones, so they are routed here rather than falling through to
     * wm_gui_syscall: a CLI program with no window must be able to copy, paste
     * and notify, and `default:` would hand it to a back end that resolves the
     * caller through the WM's app table and finds nothing.
     *
     * The pid is looked up HERE and passed in, so c/kernel/gui/clipboard.c has
     * no dependency on the process table and can be compiled on the host by
     * tests/unit/clipboard_test.c. It is informational (CLIP_Q_OWNER); nothing
     * about the clipboard's lifetime depends on it. */
    case SYS_CLIP_SET:
    case SYS_CLIP_GET:
    case SYS_CLIP_INFO: {
        struct proc *p = proc_current();
        r->rax = (uint64_t)clip_syscall((long)r->rax, (long)r->rdi, (long)r->rsi,
                                        (long)r->rdx, p ? p->pid : 0);
        return;
    }

    case SYS_NOTIFY:
        r->rax = (uint64_t)notify_syscall((long)r->rax, (long)r->rdi,
                                          (long)r->rsi, (long)r->rdx);
        return;

    default:
        /* GUI + misc system calls are handled by the window manager, which
         * resolves the calling app via the scheduler's current thread. */
        r->rax = (uint64_t)wm_gui_syscall((long)r->rax, (long)r->rdi,
                                          (long)r->rsi, (long)r->rdx);
        return;
    }
}
