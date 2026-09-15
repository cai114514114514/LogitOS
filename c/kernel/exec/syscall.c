#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "logit_abi.h"
#include "../../../include/abi/agent_policy.h"
#include "serial.h"
#include "wm.h"
#include "fb.h"
#include "sched.h"
#include "usercopy.h"
#include "proc.h"
#include "file.h"
/* Path-qualified for the SAME reason as wait.h below, and this one was found by
 * the build rather than by reading: there are THREE pty.h in this tree --
 * c/apps/libc/include/pty.h, include/abi/pty.h and the kernel's own. The bare
 * form worked only while pty.h sat in this file's own directory; the moment it
 * moved to exec/fd/ the sorted INCDIRS handed over the mini-libc one and every
 * SYS_PTY_* became an undeclared identifier. Colocation was hiding a collision,
 * not preventing one. */
#include "kernel/exec/fd/pty.h"
#include "vfs.h"
#include "rtc.h"
#include "net.h"
#include "icmp.h"
#include "dns.h"
#include "sock.h"
#include "lsock.h"       /* SERVER sockets: SYS_SOCKET .. SYS_SOCKSTAT */
#include "logit_pack.h"     /* generated: the port/flags unpack for SYS_SOCK_OPEN */
#include "img.h"
#include "kheap.h"
#include "percpu.h"
#include "smp.h"
#include "kprintf.h"
#include "pit.h"
#include "ktime.h"
/* Path-qualified, and it has to be: mini-libc ships c/apps/libc/include/sys/
 * wait.h, that directory is in INCDIRS, and it sorts BEFORE c/kernel/core -- so
 * a bare #include "wait.h" from OUTSIDE c/kernel/core silently resolves to the
 * userland one, and sched_sleep_ms below becomes an undeclared function. (Files
 * in c/kernel/core get away with the bare form only because a quoted include
 * searches the including file's own directory first.) */
#include "kernel/sync/wait.h"   /* M27 sched_sleep_ms: the kernel's ONE sleeper */
#include "snd.h"
#include "mm.h"          /* mm_syscall: SYS_MMAP / SYS_MMAP_FILE / SYS_MPROTECT / SYS_MUNMAP / SYS_MEMINFO */
#include "settings.h"    /* settings_syscall: SYS_SETTING_* */
#include "clipboard.h"   /* clip_syscall:   SYS_CLIP_SET / _GET / _INFO */
#include "notify.h"      /* notify_syscall: SYS_NOTIFY */
#include "kbench.h"      /* per-syscall accounting, off by default */
#include "kpoll.h"       /* poll_syscall: SYS_POLL / SYS_EVENTFD / SYS_TIMERFD.
                          * NOT "poll.h" -- see kpoll.h's own first paragraph,
                          * and the wait.h note twenty lines up for the same
                          * INCDIRS trap in its earlier form. */
#include "uthread.h"     /* M30: SYS_THREAD_* / SYS_SET_TLS / SYS_FUTEX */
#include "oom.h"
#include "mmguard.h"
#include "ksignal.h"     /* signals: SYS_SIGACTION..SYS_SIGQUERY, execve reset */
#include "ptrace.h"      /* SYS_PTRACE: attach, registers, peek/poke */
#include "meta.h"        /* meta_syscall: SYS_STAT / SYS_GETDENTS / SYS_CHMOD ... */
#include "../../../include/abi/fs_ref.h"
#include "vfs_cred.h"    /* id_syscall:   SYS_GETUID .. SYS_GETSESSION (150-159) */
#include "power.h"       /* kernel_poweroff / kernel_reboot: SYS_POWEROFF / SYS_REBOOT */
/* mod_syscall: SYS_MODULE_LOAD/_UNLOAD/_LIST/_SYM (182-185). Bare include is
 * safe here -- `module.h` is a unique basename under c/ and include/ (checked),
 * unlike the wait.h case documented above. */
#include "module.h"
#include "../../../include/weaksym.h"   /* the weak declarations below are an ELF idiom */

/* M25 P1: which syscalls run WITHOUT the Big Kernel Lock (interrupt_handler skips
 * the BKL for these; they self-lock via fine-grained locks). Only the kheap stress
 * for now -- the proof that concurrent, BKL-free kmalloc works. */
/* BKL removal: every syscall uses object-specific synchronization. */

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

static void syscall_do(struct registers *r, const void *user_fxarea);

/* ONE COPY-IN AND ONE RESOLUTION for the two AF_UNIX calls that take a path
 * (SYS_BIND with an AF_UNIX address, and SYS_CONNECT).
 *
 * Three things have to happen to a `struct logit_sockaddr_un`, and all three
 * are easy to leave out of a second copy of them:
 *   1. THE PATH IS NUL-TERMINATED HERE. A user program is free to hand over
 *      108 bytes with no terminator, and every reader below it is a C string
 *      function.
 *   2. IT IS RESOLVED against the process cwd, so a relative name works exactly
 *      as it does for open() and chdir(). c/net/core/unix.c never sees a
 *      relative path and does not have to know what a cwd is.
 *   3. IT IS RESOLVED INTO 256 BYTES, not into 128. proc_resolve TRUNCATES
 *      silently when its output buffer is too small, and a truncated path is
 *      not a shorter name -- it is a name that collides with a different one.
 *      Resolving into a buffer that cannot overflow and letting unix.c refuse
 *      what does not fit keeps that rule in ONE place.
 *
 * `connecting` picks which of the two calls to make: the difference between
 * bind and connect here is genuinely that one line. */
/* Keep user addresses out of wait queues and devices: a sibling can unmap
 * them while the operation sleeps. Reads use a bounded private buffer and
 * copy out after completion; short reads/writes keep their usual ABI meaning. */
#define SYSCALL_IO_MAX (64 * 1024)
static void syscall_buf_free(unsigned char **p) { if (*p) kfree(*p); }
#define SYSCALL_BUF(name, n) unsigned char *name \
    __attribute__((cleanup(syscall_buf_free))) = kmalloc((size_t)((n) ? (n) : 1))

int kernel_img_decode(const uint8_t *data, int len, struct image *out)
{
    /* JPEG uses shared scratch. Only decoders serialize here; unrelated
     * syscalls, the compositor, and I/O continue on other cores. */
    static struct mutex decode_lock = MUTEX_INIT;
    mutex_lock(&decode_lock);
    fb_graphics_lock(); /* SVG shares gfx_raster scratch with the compositor. */
    int rc = img_decode(data, len, out);
    fb_graphics_unlock();
    mutex_unlock(&decode_lock);
    return rc;
}

static long unix_addr_call(struct proc *p, struct file *f, const void *uaddr,
                           int connecting)
{
    struct logit_sockaddr_un ua;
    if (user_copy_from(&ua, uaddr, sizeof ua) < 0) return LSK_E_ARG;
    ua.path[sizeof ua.path - 1] = 0;                       /* (1) */
    /* An empty path is Linux's abstract namespace, which this kernel does not
     * implement. Passed through UNRESOLVED so that unix.c refuses it by name:
     * proc_resolve("") returns the cwd, which is a real path, and the caller
     * would silently bind something it never asked for. */
    if (!ua.path[0])
        return connecting ? lsock_connect_unix(f, "") : lsock_bind_unix(f, "");
    char abs[256];                                         /* (3) */
    proc_resolve(p, ua.path, abs, sizeof abs);             /* (2) */
    return connecting ? lsock_connect_unix(f, abs) : lsock_bind_unix(f, abs);
}

/* SYS_PROCS / SYS_KILL live in proc.c (the table they read is static there);
 * these are prototyped here rather than in proc.h because that header belongs
 * to the process line, exactly as proc.c prototypes proc_fork_stats() locally.
 *
 * proc_kill_armed() is the gate for the deferred kill: a marked process runs
 * proc_exit() ON ITSELF at its next kernel entry, which is what makes killing
 * another process safe here (see the long comment above proc_kill()). Off, it
 * costs one load of a global and one never-taken branch per syscall. */
long proc_syscall(long num, long a, long b, long c);
/* The SYS_GETRANDOM back end, weak -- see the case for why. Prototyped here
 * rather than by including c/kernel/core/rng.h so that the weakness is stated
 * at the point that depends on it. */
long rng_syscall(long ubuf, long len, long flags) LOGIT_WEAK;
LOGIT_WEAK_STUB(rng_syscall);
int  proc_kill_armed(void);
void proc_kill_check(void);      /* does not return if THIS process is the victim */

/* ===========================================================================
 * M28: THE CATEGORY GATE (docs/superpowers/specs/2026-08-14-m28-capabilities.md
 * D6). "The category bit (CAP_FS at all, CAP_NET at all) at syscall_dispatch...
 * one place, one table lookup." This is that table.
 *
 * WHAT IT DOES NOT DO, and why not here. This checks only WHICH SYSCALL is
 * being made, never the PATH argument's value -- a syscall that names a path
 * still runs if the caller holds CAP_FS, regardless of what the path is. The
 * path-PREFIX restriction (struct proc's fs_prefix) is deliberately absent
 * from this function: it belongs after symlink resolution, where the FINAL
 * path is known, and this point is not that point -- proc_resolve() (proc.c)
 * is lexical only, so a prefix check on ITS output would look correct, pass
 * every obvious test, and be silently bypassable by a symlink whose real
 * target escapes the prefix (and SYS_SYMLINK stores its target completely
 * unresolved by design, so a scoped process could plant that escape itself).
 * That check has to live in c/fs/vfs.c's resolve(), a file this line does not
 * own -- see `not_done` for the exact change it still needs.
 *
 * CLASSIFICATION RULE: a syscall is CAP_FS/CAP_NET if it ACQUIRES fs/network
 * access BY NAME (a path string, a host string, a URL) -- open, read_file,
 * mkdir, rename, stat, symlink, http_get, sock_open, bind, connect-shaped
 * calls, and so on. A syscall that only operates on an FD ALREADY HELD
 * (read/write/close/lseek/dup/dup2/setnb/fsync/fstat) is deliberately absent:
 * containment lives at ACQUISITION (D10's whole point, even though fd
 * inheritance across SYS_CAP_SPAWN is itself out of this file's scope -- see
 * `not_done`), and gating a bare fd operation by name would ALSO wrongly
 * block pipe and tty I/O, which were never fs or network access in the first
 * place and have no path or host to have been gated on. */
static int syscall_cap_class(int num)
{
    switch (num) {
    /* ---- CAP_FS: creates, destroys, renames, links, or reveals something
     * BY PATH. See the classification rule above for what is deliberately
     * NOT here (fd-only operations). */
    case SYS_CREATE_FILE: case SYS_READ_FILE: case SYS_WRITE_FILE: case SYS_DELETE_FILE:
    case SYS_MKDIR: case SYS_DIR_COUNT: case SYS_DIR_NAME:
    case SYS_FILE_COUNT: case SYS_FILE_NAME:
    case SYS_OPEN: case SYS_CHDIR: case SYS_RENAME: case SYS_OPEN_PATH:
    case SYS_IMG_DECODE:
    case SYS_FSREF: case SYS_STAT: case SYS_LSTAT: case SYS_GETDENTS: case SYS_CHMOD:
    case SYS_SYMLINK: case SYS_READLINK: case SYS_LINK: case SYS_CHOWN:
        return CAP_FS;

    /* ---- CAP_NET: touches the network in any shape -- info/ping/DNS, HTTP,
     * client sockets (SYS_SOCK_*), or the server-socket family
     * (bind/listen/accept/... and the datagram calls). */
    case SYS_NET_INFO: case SYS_NET_PING: case SYS_NET_PING_RTT:
    case SYS_NET_DNS: case SYS_NET_DNS_RESULT:
    case SYS_HTTP_GET: case SYS_HTTP_STATUS: case SYS_HTTP_BODY:
    case SYS_RES_FETCH:
    case SYS_SOCK_OPEN: case SYS_SOCK_POLL: case SYS_SOCK_SEND:
    case SYS_SOCK_RECV: case SYS_SOCK_ALPN: case SYS_SOCK_CLOSE:
    /* SYS_CONNECT is here with the rest of the family even though its ONLY
     * working case today is AF_UNIX, which touches a path and not the network.
     * The class is per NUMBER and this number is polymorphic, so it is filed
     * with its siblings and the discrepancy is written down rather than
     * silently resolved one way: a call that can be either CAP_FS or CAP_NET
     * depending on its argument is a question for the capability line, not
     * something to decide in passing here. SYS_SOCKETPAIR is deliberately NOT
     * listed -- it names no path and reaches no network, so it is CAP_NONE like
     * SYS_PIPE, which is exactly what it is a two-way version of. */
    case SYS_CONNECT:
    case SYS_SOCKET: case SYS_BIND: case SYS_LISTEN: case SYS_ACCEPT:
    case SYS_GETSOCKNAME: case SYS_SETSOCKOPT: case SYS_SHUTDOWN:
    case SYS_RECVFROM: case SYS_SENDTO: case SYS_SOCKSTAT:
        return CAP_NET;

    /* ---- Considered and left CAP_NONE (0) -- an explicit decision recorded
     * here, not an omission found later. ----------------------------------
     *
     * SYS_KILL (96) is "one number with two unrelated privilege shapes
     * selected by a flag bit in its third argument" (LOGIT_KILL_SIGNAL --
     * see logit_abi.h): the historical destroy-by-pid shape and POSIX
     * kill(2). NEITHER touches a path or the network, so both classify the
     * same way regardless of the flag -- this function does not need to look
     * at the flag to know that, which is exactly why it takes only `num`.
     * proc_kill()'s own structural refusal (no parent, no window) is that
     * call's real protection and is unrelated to this gate.
     *
     * SYS_PROCS, the clipboard (SYS_CLIP_*), SYS_NOTIFY, and SYS_SETTING_*
     * are named EXPLICITLY OUT OF SCOPE by the spec (section 8): a
     * capability-confined script can enumerate the process table, exfiltrate
     * through the clipboard regardless of CAP_FS, and change persistent
     * settings, and all three stay true after M28. Gating them here would be
     * this file quietly doing more than the spec asked and disagreeing with
     * it about what M28 covers.
     *
     * SYS_FORK / SYS_EXECVE / SYS_CAP_SPAWN / SYS_CAP_QUERY / SYS_WAITPID:
     * process lifecycle is not a category this gate covers at all -- SYS_FORK
     * and SYS_EXECVE are safe by construction (proc.c/exec.c: they copy or
     * leave `caps` alone, never widen it) and SYS_CAP_SPAWN carries its OWN
     * ceiling check inside proc_cap_spawn(), which is stricter than a single
     * bitmap test because it also has to check fs_prefix. Gating the spawn
     * call ITSELF by CAP_FS/CAP_NET would conflate "may this process spawn a
     * child" (not a thing M28 defines) with "may this process touch files/
     * network" (a different, already-covered question). */
    /*
     * SYS_POLL / SYS_EVENTFD / SYS_TIMERFD: waiting is not a category either.
     * SYS_POLL reaches nothing the caller does not already hold -- an fd it
     * cannot open cannot be in the array, and an fd it can open it can already
     * read() with no gate here. SYS_EVENTFD and SYS_TIMERFD create a
     * descriptor out of nothing: no path, no host, no device, exactly like
     * SYS_PIPE two entries up in spirit, which is also ungated. Classifying
     * them CAP_FS because the word "descriptor" appears would gate a counter
     * and a clock behind the filesystem grant, which is the kind of
     * over-reach the block above refuses for the clipboard. */
    default:
        return 0;
    }
}

/* The dispatcher proper is wrapped so the per-number accounting has exactly one
 * place to live, instead of being repeated at the ~60 `return`s below. When the
 * counters are disarmed this is a load of a global, a branch, and a tail call. */
void syscall_entry_checks(void)
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
     * microseconds, and only for a process calling the BKL-free stress call.
     * Correction (BKL removal): every syscall now checks the mark; teardown
     * uses object lifetime rules and needs no global kernel lock. */
    if (__builtin_expect(proc_kill_armed(), 0))
        proc_kill_check();

    /* M30: and a thread whose PROCESS is exiting dies here too, for exactly the
     * reasons above. One thread called exit() (or was killed); its siblings are
     * marked and each ends itself at its own next kernel entry, on its own
     * stack, rather than being torn down from the exiting thread's context.
     * Same gate discipline: one load of a global and a never-taken branch on a
     * machine where nothing is exiting. */
    if (__builtin_expect(uthread_exit_armed(), 0))
        uthread_exit_check();

    /* M30: make this core's TLB current before the call runs.
     *
     * A thread stack that was just unmapped can have its address handed
     * straight back by the next mmap, and a core still holding the old
     * translation would write through it into a FREED FRAME without faulting --
     * the write silently misses the new mapping, and the next reader takes an
     * ordinary fault and gets a zero page. That is not hypothetical; it is what
     * the threads gate caught, twice, and the second time from the writer's
     * side rather than the reader's.
     *
     * Step two of that sequence is a syscall, and there is no way to reach a
     * recycled mapping without one -- so this is not an approximation of the
     * right place, it is the right place. Costs one relaxed load and a
     * not-taken branch on a machine where nothing is being unmapped. */
    sched_tlb_gen_check();
}

void syscall_dispatch(struct registers *r, const void *user_fxarea)
{
    syscall_entry_checks();

    /* M28: the capability category gate -- see syscall_cap_class() above for
     * the table and the reasoning. One field read on the current process plus
     * one table lookup and one branch.
     *
     * NO PROC, NO BENEFIT OF THE DOUBT. proc_current() == NULL means a
     * kernel thread reached int 0x80's dispatcher, which is not a shape any
     * legitimate ring-3 caller produces -- every real syscall arrives from a
     * thread whose ->data is a struct proc (see uthread.h). If a classified
     * (CAP_FS/CAP_NET) syscall number ever shows up with no proc to check a
     * grant against, the safe reading is "there is nothing to prove this is
     * allowed", not "there is nothing to prove this is forbidden" -- so it is
     * REFUSED, matching D9's stated default (docs/superpowers/specs/
     * 2026-08-14-m28-capabilities.md): "the default is DENY... a test that
     * forgets to [grant] fails closed." An UNCLASSIFIED syscall (need == 0)
     * still runs with no proc either way, same as it always has -- this gate
     * only ever NARROWS what an ordinary NULL-proc call could already do
     * (every classified handler already re-checks proc_current() itself and
     * refuses on NULL, so this changes no OBSERVABLE behaviour today; it only
     * removes a fail-OPEN path that a future classified syscall could
     * otherwise inherit by accident).
     *
     * Historical gate rationale (the kill gates are now unconditional):
     * DELIBERATELY NOT GUARDED BY `!syscall_is_bkl_free((int)r->rax)`, unlike
     * the two kill-check gates just above. That guard exists there so a KILL
     * MARK does not tear down a process while it is mid-flight through the
     * ONE syscall that runs without the BKL (proc_exit()'s teardown assumes
     * the lock is held). Capability refusal does not tear anything down --
     * it is a read of one field and an early return -- so it has no such
     * hazard to guard against. Copying that guard here anyway would not
     * change SYS_KHEAP_STRESS's outcome (syscall_cap_class() already answers
     * 0 for it, so it is never gated either way), but it WOULD read as a
     * second, unexplained reason that call is exempt from capability
     * enforcement, which is exactly the kind of copy-paste this spec warns
     * against (D6).
     *
     * SYS_SIGRETURN never reaches this line at all: c/kernel/cpu/
     * interrupts.c intercepts it ONE LINE BEFORE syscall_dispatch() is even
     * called (restoring a signal frame needs the register set this function
     * is about to iretq and the FXSAVE area isr.asm will FXRSTOR, neither of
     * which a syscall body can reach). Fine today -- SIGRETURN is not FS or
     * NET shaped anyway -- but structurally, nothing added beside this gate
     * will ever see it either, and that is worth knowing before assuming a
     * gate here covers every syscall NUMBER that exists. */
    {
        int need = syscall_cap_class((int)r->rax);
        if (need) {
            struct proc *cap_p = proc_current();
            if (!cap_p || !(cap_p->caps & (unsigned long)need)) { r->rax = (uint64_t)-1; return; }
        }
    }

    if (__builtin_expect(!kb_stat_enabled(), 1)) { syscall_do(r, user_fxarea); return; }
    uint64_t n = r->rax, t0 = kb_rdtsc();
    syscall_do(r, user_fxarea);
    /* r->rax is the RESULT by now, so the number has to be the one saved above.
     * execve rewrites the whole frame; it is still the right number to charge. */
    if (n < KB_NSYS) {
        /* A blocking syscall can migrate. Freeze only this final four-scalar
         * update, then choose the CPU we actually exit on; no shared LOCK RMW. */
        uint64_t flags;
        __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
        kb_sys_record((unsigned)this_cpu()->index, n, kb_rdtsc() - t0);
        if (flags & 0x200) __asm__ volatile ("sti" ::: "memory");
    }
}

static void syscall_do(struct registers *r, const void *user_fxarea)
{
    struct proc *agent_p = proc_current();
    if (agent_p && agent_p->agent.mode == AEX_ACT_WORKER &&
        !aex_agent_syscall_allowed(r->rax, r->rdi)) { r->rax = (uint64_t)-1; return; }
    switch (r->rax) {
    case SYS_PTY_OPEN: case SYS_PTY_CTL:
        r->rax = (uint64_t)pty_syscall(r->rax,r->rdi,r->rsi,r->rdx);return;
    case SYS_AGENT_SPAWN:
        r->rax = (uint64_t)proc_agent_spawn(r); return;
    case SYS_AGENT_SELF: {
        struct aex_agent_identity id;
        r->rax=agent_p && r->rsi==sizeof id && proc_agent_identity(agent_p->pid,&id) &&
            user_copy_to((void *)r->rdi,&id,sizeof id)==0 ? 0 : (uint64_t)-1;
        return;
    }
    case SYS_AGENT_PEER: {
        struct file *f=proc_fd_acquire(agent_p,(int)r->rdi);
        struct aex_agent_identity id;
        int ok=f && r->rdx==sizeof id && lsock_agent_peer(f,&id)==0;
        if (f) file_close(f);
        r->rax=ok && user_copy_to((void *)r->rsi,&id,sizeof id)==0 ? 0 : (uint64_t)-1;
        return;
    }
    case SYS_WRITE: {
        const unsigned char *buf = (const void *)r->rsi;
        long len = (long)r->rdx, done = 0;
        int fd = (int)r->rdi;
        if (len < 0 || !user_range_ok(buf, (uint64_t)len, 0)) { r->rax = (uint64_t)-1; return; }
        struct proc *p = proc_current();
        struct file *f FILE_REF = p ? proc_fd_acquire(p, fd) : NULL;
        if (!f && fd != 1 && fd != 2) { r->rax = (uint64_t)-1; return; }
        long cap = len < SYSCALL_IO_MAX ? len : SYSCALL_IO_MAX;
        SYSCALL_BUF(tmp, cap);
        if (!tmp) { r->rax = (uint64_t)-1; return; }
        while (done < len) {
            long n = len - done < cap ? len - done : cap;
            if (user_copy_from(tmp, buf + done, (uint64_t)n) < 0) { if (!done) done = -1; break; }
            long got = n;
            if (f) got = file_write(f, tmp, n);
            else for (long i = 0; i < n; i++) serial_putc(tmp[i]);
            if (got <= 0) { if (!done) done = got; break; }
            done += got; if (got < n) break;
        }
        r->rax = (uint64_t)done; return;
    }
    case SYS_READ: {
        long len = (long)r->rdx;
        if (len < 0) { r->rax = (uint64_t)-1; return; }
        struct proc *p = proc_current();
        struct file *f FILE_REF = p ? proc_fd_acquire(p, (int)r->rdi) : NULL;
        if (!f) { r->rax = (uint64_t)-1; return; }
        if (len > SYSCALL_IO_MAX) len = SYSCALL_IO_MAX;
        if (!user_range_ok((void *)r->rsi, (uint64_t)len, 1)) { r->rax = (uint64_t)-1; return; }
        SYSCALL_BUF(tmp, len);
        if (!tmp) { r->rax = (uint64_t)-1; return; }
        long got = file_read(f, tmp, len);
        if (got > 0 && user_copy_to((void *)r->rsi, tmp, (uint64_t)got) < 0) got = -1;
        r->rax = (uint64_t)got; return;
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
    case SYS_CLOSE:
        r->rax = (uint64_t)(long)proc_fd_close(proc_current(), (int)r->rdi);
        return;
    case SYS_LSEEK: {
        struct proc *p = proc_current();
        struct file *f FILE_REF = p ? proc_fd_acquire(p, (int)r->rdi) : NULL;
        if (!f) { r->rax = (uint64_t)-1; return; }
        r->rax = (uint64_t)file_lseek(f, (long)r->rsi, (int)r->rdx);
        return;
    }
    /* SYS_FTRUNCATE: fd-only, like its neighbours here, so the M28 capability
     * gate classifies it CAP_NONE -- the path was checked at SYS_OPEN. The
     * contract, the Step-0 measurement that motivated it, and the two
     * rejected alternatives (rename-replace, pwrite) are the SYS_FTRUNCATE
     * block in include/abi/logit_abi.h; the body and its refusal set are
     * file_truncate() in file.c. */
    case SYS_FTRUNCATE: {
        struct proc *p = proc_current();
        struct file *f FILE_REF = p ? proc_fd_acquire(p, (int)r->rdi) : NULL;
        if (!f) { r->rax = (uint64_t)-1; return; }
        r->rax = (uint64_t)file_truncate(f, (long)r->rsi);
        return;
    }
    case SYS_DUP: {
        struct proc *p = proc_current();
        struct file *f FILE_REF = p ? proc_fd_acquire(p, (int)r->rdi) : NULL;
        if (!f) { r->rax = (uint64_t)-1; return; }
        file_dup(f);
        int fd = proc_fd_alloc(p, f);
        if (fd < 0) { file_close(f); r->rax = (uint64_t)-1; return; }
        r->rax = (uint64_t)fd;
        return;
    }
    case SYS_SETNB: {
        struct proc *p = proc_current();
        struct file *f FILE_REF = p ? proc_fd_acquire(p, (int)r->rdi) : NULL;
        if (!f) { r->rax = (uint64_t)-1; return; }
        __atomic_fetch_or(&f->flags, O_NONBLOCK, __ATOMIC_RELAXED);
        r->rax = 0;
        return;
    }
    case SYS_FSYNC: {
        struct proc *p = proc_current();
        struct file *f FILE_REF = p ? proc_fd_acquire(p, (int)r->rdi) : NULL;
        if (!f) { r->rax = (uint64_t)-1; return; }
        r->rax = (uint64_t)file_fsync(f);
        return;
    }
    case SYS_DUP2:
        r->rax = (uint64_t)(long)proc_fd_dup2(proc_current(), (int)r->rdi, (int)r->rsi);
        return;
    case SYS_GETCWD: {
        struct proc *p = proc_current(); char *buf = (char *)r->rdi; int max = (int)r->rsi;
        if (!p || max <= 0 || !user_range_ok(buf, (uint64_t)max, 1)) { r->rax = (uint64_t)-1; return; }
        char cwd[sizeof p->cwd];
        uint64_t cf = spin_lock_irqsave(&p->fd_lock);
        int i = 0; for (; i < max - 1 && i < (int)sizeof cwd - 1 && p->cwd[i]; i++) cwd[i] = p->cwd[i]; cwd[i] = 0;
        spin_unlock_irqrestore(&p->fd_lock, cf);
        r->rax = user_copy_to(buf, cwd, (uint64_t)i + 1) < 0 ? (uint64_t)-1 : (uint64_t)i;
        return;
    }
    case SYS_CHDIR: {
        char path[128];
        if (user_copy_string(path, sizeof path, (const char *)r->rdi) < 0) { r->rax = (uint64_t)-1; return; }
        struct proc *p = proc_current(); if (!p) { r->rax = (uint64_t)-1; return; }
        char abs[128]; proc_resolve(p, path, abs, sizeof abs);
        if (vfs_count(abs) < 0) { r->rax = (uint64_t)-1; return; }   /* not a directory */
        uint64_t cf = spin_lock_irqsave(&p->fd_lock);
        int i = 0; for (; i < (int)sizeof(p->cwd) - 1 && abs[i]; i++) p->cwd[i] = abs[i]; p->cwd[i] = 0;
        spin_unlock_irqrestore(&p->fd_lock, cf);
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
        SYSCALL_BUF(tmp, max);
        if (!tmp) { r->rax = (uint64_t)-1; return; }
        long got = vfs_read(abs, tmp, max);
        if (got > 0 && user_copy_to((void *)r->rsi, tmp, (uint64_t)got) < 0) got = -1;
        r->rax = (uint64_t)got;
        return;
    }
    case SYS_CREATE_FILE:
    case SYS_WRITE_FILE: {
        int exclusive=r->rax==SYS_CREATE_FILE;
        if(exclusive&&r->rdx>AEX_AGENT_DOCUMENT_MAX){r->rax=(uint64_t)-1;return;}
        char path[128], abs[128]; int size = (int)r->rdx;
        struct proc *p = proc_current();
        if (!p || size < 0 || user_copy_string(path, sizeof path, (const char *)r->rdi) < 0) { r->rax = (uint64_t)-1; return; }
        if (size > 0 && !user_range_ok((const void *)r->rsi, (uint64_t)size, 0)) { r->rax = (uint64_t)-1; return; }
        proc_resolve(p, path, abs, sizeof abs);
        SYSCALL_BUF(tmp, size);
        if (!tmp || user_copy_from(tmp, (const void *)r->rsi, (uint64_t)size) < 0) { r->rax = (uint64_t)-1; return; }
        r->rax = (uint64_t)(exclusive ? vfs_create_file(abs,tmp,size) : vfs_write(abs, tmp, size));
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
        { const char *nm = vfs_ent_name(abs, i); char out[64];
          int j = 0; for (; j < 63 && nm && nm[j]; j++) out[j] = nm[j]; out[j] = 0;
          if (user_copy_to((void *)r->rdx, out, (uint64_t)j + 1) < 0) { r->rax = (uint64_t)-1; return; } }
        r->rax = (uint64_t)(vfs_ent_is_dir(abs, i) ? -2 : vfs_ent_size(abs, i));
        return;
    }
    case SYS_FILE_COUNT:
        r->rax = (uint64_t)vfs_count("/");
        return;
    case SYS_FILE_NAME: {
        int i = (int)r->rdi; int max = (int)r->rdx;
        if (i < 0 || i >= vfs_count("/") || max <= 0 || !user_range_ok((void *)r->rsi, (uint64_t)max, 1)) { r->rax = (uint64_t)-1; return; }
        { const char *nm = vfs_ent_name("/", i); char out[256];
          int j = 0; for (; j < max - 1 && j < 255 && nm && nm[j]; j++) out[j] = nm[j]; out[j] = 0;
          if (user_copy_to((void *)r->rsi, out, (uint64_t)j + 1) < 0) { r->rax = (uint64_t)-1; return; } }
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
         * wants the same answer. Historical assumption: BKL held (int 0x80 is
         * not in syscall_is_bkl_free), so no lock of its own.
         * Correction: cache_lock now protects the RTC snapshot and timestamp. */
        if (!user_range_ok((void *)r->rdi, sizeof(struct rtc_time), 1)) { r->rax = (uint64_t)-1; return; }
        static struct rtc_time cached;
        static spinlock_t cache_lock = SPINLOCK_INIT;
        uint64_t cf = spin_lock_irqsave(&cache_lock);
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
        struct rtc_time result = cached;
        spin_unlock_irqrestore(&cache_lock, cf);
        if (user_copy_to((void *)r->rdi, &result, sizeof result) < 0) { r->rax = (uint64_t)-1; return; }
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
        if (user_copy_to((void *)r->rsi, &ts, sizeof ts) < 0) { r->rax = (uint64_t)-1; return; }
        r->rax = 0;
        return;
    }
    case SYS_NANOSLEEP: {
        struct logit_timespec req;
        if (!user_range_ok((const void *)r->rdi, sizeof req, 0)) { r->rax = (uint64_t)-1; return; }
        if (user_copy_from(&req, (const void *)r->rdi, sizeof req) < 0) { r->rax = (uint64_t)-1; return; }
        if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000L) {
            r->rax = (uint64_t)-1; return;
        }
        uint64_t want = (uint64_t)req.tv_sec * NS_PER_SEC + (uint64_t)req.tv_nsec;
        if (want > 3600ull * NS_PER_SEC) want = 3600ull * NS_PER_SEC;   /* one hour cap */
        uint64_t deadline = time_mono_ns() + want;
        uint64_t tick_ns  = timer_ns_per_tick();
        /* NOT a second blocking mechanism -- deliberately. sched_sleep_ms() in
         * c/kernel/sync/wait.c is the kernel's sleeper: it UNLINKS the thread
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
            /* sched_poll_wait(), NOT schedule(), and this one hung the machine.
             *
             * schedule() does not touch the BKL unless it actually switches,
             * and with every other thread blocked it finds nothing to switch
             * to -- so this tail spun at ~3.5 million iterations a second
             * HOLDING THE GLOBAL LOCK, waiting for time_mono_ns() to reach the
             * deadline.
             *
             * Time cannot reach it. timer_tick() runs on the BSP only
             * (interrupts.c gates it on me->index == 0), and the BSP was in
             * spin_lock_irqsave(&g_bkl) -- WITH INTERRUPTS OFF -- waiting for
             * the lock this spin is holding. So the clock stops, the deadline
             * never arrives, and the lock is never released: a circular wait
             * between a spin on time and the lock that lets time advance.
             * Every core wedged, no panic, nothing in the log.
             *
             * It is multi-core only, which is why it hid: on one core the
             * spinner IS the BSP, timer_tick() runs ahead of the BKL acquire
             * and is not gated by , so time keeps moving.
             *
             * bkl_hlt_wait drops the BKL, halts until the next interrupt, and
             * retakes it -- the primitive that already exists for exactly this
             * (see its comment in sched.c). Reproducer: make test-smp-fork-storm. */
            else                   sched_poll_wait();
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
        if (user_copy_to((void *)r->rdi, &ci, sizeof ci) < 0) { r->rax = (uint64_t)-1; return; }
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
        r->rax = (uint64_t)proc_fork(r, user_fxarea);
        return;
    case SYS_EXECVE: {
        /* M30: REFUSED from a multi-threaded process, rather than done wrong.
         * POSIX says the other threads must vanish; making that safe means
         * stopping threads that may be anywhere -- parked on a futex, mid-write
         * in another syscall -- and this kernel has no mechanism for that (see
         * the long comment above proc_kill(): a thread is only ever torn down
         * BY ITSELF, at a kernel entry). Replacing the address space while a
         * sibling still runs in it is not a race, it is a certainty.
         *
         * The alternative shape -- mark, then spin here until the siblings
         * notice -- deadlocks against a sibling that never enters the kernel,
         * which is precisely the case proc_kill() already documents as open.
         * So it is a refusal, and it is a loud one. Nothing in the tree does
         * this today: /bin/sh forks single-threaded and execs in the child. */
        struct proc *ep = proc_current();
        if (!uthread_exec_begin()) {
            kprintf("[execve] pid %d: refused, %d threads live\n",
                    ep ? ep->pid : -1, ep ? uthread_proc_live(ep->pid) : 0);
            r->rax = (uint64_t)(long)THR_E_INVAL;
            return;
        }
        long rc = proc_execve(r);
        uthread_exec_end();
        /* On success the user image is gone and with it whatever the TLS
         * pointer used to name. Leaving IA32_FS_BASE loaded would hand the new
         * program a %fs pointing into memory that is no longer its own -- and
         * because the descriptor would still agree with the hardware, no later
         * context switch would ever correct it. */
        /* Correction: proc_execve installs ei.tls_tp (zero for an image with
         * no TLS). Clearing it again here erased the newly relocated PIE TLS,
         * making its first %fs:0 load fault at address zero. */
        if (rc == 0) {
            /* Signals across exec, and the distinction is POSIX's rather than a
             * shortcut: a CAUGHT signal goes back to SIG_DFL, because the
             * handler's address is in an image that no longer exists and
             * keeping it would be a jump into whatever the new program put
             * there; an IGNORED signal STAYS ignored, which is what lets a
             * shell start a child with SIGINT already ignored. The blocked mask
             * is inherited unchanged. `ep` is still the same struct proc --
             * execve replaces the address space, not the process. */
            if (ep) ksig_proc_exec(ep->pid);
        }
        else r->rax = (uint64_t)rc;
        return;
    }
    case SYS_PIPE: {
        struct proc *p = proc_current();
        int *ufds = (int *)r->rdi, fds[2];
        if (!p || !user_range_ok(ufds, sizeof fds, 1)) { r->rax = (uint64_t)-1; return; }
        struct file *rf = NULL, *wf = NULL;
        if (file_pipe(&rf, &wf) < 0) { r->rax = (uint64_t)-1; return; }
        struct file *hold_a FILE_REF = rf, *hold_b FILE_REF = wf;
        file_dup(rf); file_dup(wf); /* retain identity through copyout rollback */
        if (proc_fd_pair(p, rf, wf, fds) < 0) {
            file_close(rf); file_close(wf); r->rax = (uint64_t)-1; return;
        }
        if (user_copy_to(ufds, fds, sizeof fds) < 0) {
            proc_fd_close_if(p, fds[0], hold_a); proc_fd_close_if(p, fds[1], hold_b); r->rax = (uint64_t)-1; return;
        }
        r->rax = 0; return;
    }
    case SYS_GETPID: {
        struct proc *p = proc_current();
        r->rax = p ? (uint64_t)p->pid : (uint64_t)-1;
        return;
    }
    case SYS_CPU_INDEX:
        /* index of the core running this syscall (kernel entry depth only). SMP proof: a
         * child that observes a different index than another ran on another core. */
        r->rax = (uint64_t)(long)this_cpu()->index;
        return;
    case SYS_CPU_COUNT:
        /* Runtime state, after AP bring-up. Keeping this separate from CPU_INDEX
         * preserves that call's zero-argument ABI and prevents libc from
         * pretending every machine has four CPUs. */
        r->rax = (uint64_t)(unsigned)smp_cpu_count();
        return;
    case SYS_KHEAP_STRESS: {   /* concurrent kmalloc stress; all syscalls are BKL-free */
        long iters = (long)r->rdi; int size = (int)r->rsi; unsigned long seed = (unsigned long)r->rdx;
        if (size < 8 || size > 1024 || iters < 0) { r->rax = (uint64_t)-1; return; }
        r->rax = (uint64_t)kheap_stress(iters, size, seed);
        return;
    }
    case SYS_WAITPID: {
        /* rdx is WNOHANG (or 0), per logit_abi.h's own doc comment on
         * SYS_WAITPID -- it names `opts` as part of the ABI already. Before
         * this it was read nowhere, so a caller asking not to block (M31's
         * comment on SIG_E_INTR in proc.c already notes /bin/sh works around
         * exactly this) blocked anyway, quietly. proc_waitpid() now refuses
         * (SIG_E_NOSYS) any bit it does not implement instead of ignoring it. */
        int status = 0;
        long rc = proc_waitpid((int)r->rdi, &status, (int)r->rdx);
        if (rc >= 0 && r->rsi && user_copy_to((void *)r->rsi, &status, sizeof(int)) < 0) rc = -1;
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
        struct logit_netinfo info = {0};
        struct net_config cfg = net_config_snapshot();
        info.ip = cfg.ip; info.mask = cfg.mask; info.gw = cfg.gw;
        for (int i = 0; i < 6; i++) info.mac[i] = cfg.mac[i];
        if (user_copy_to(ni, &info, sizeof info) < 0) { r->rax = (uint64_t)-1; return; }
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
     * and the real work happens in net_poll() on the WM thread.
     * Correction: the BKL is gone. Socket calls pass private kernel buffers
     * to the socket owner; HTTP uses its own sleeping session lock. */
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
        if (len > SYSCALL_IO_MAX) len = SYSCALL_IO_MAX;
        SYSCALL_BUF(tmp, len);
        if (!tmp || user_copy_from(tmp, (const void *)r->rsi, (uint64_t)len) < 0) { r->rax = (uint64_t)-1; return; }
        r->rax = (uint64_t)(long)sock_send((int)r->rdi, tmp, len, p->pid);
        return;
    }
    case SYS_SOCK_RECV: {
        struct proc *p = proc_current();
        int max = (int)r->rdx;
        if (!p || max <= 0 || !user_range_ok((void *)r->rsi, (uint64_t)max, 1))
            { r->rax = (uint64_t)(long)SOCK_E_ARG; return; }
        if (max > SYSCALL_IO_MAX) max = SYSCALL_IO_MAX;
        SYSCALL_BUF(tmp, max);
        if (!tmp) { r->rax = (uint64_t)-1; return; }
        long got = sock_recv((int)r->rdi, (void *)tmp, max, p->pid);
        if (got > 0 && user_copy_to((void *)r->rsi, tmp, (uint64_t)got) < 0) got = -1;
        r->rax = (uint64_t)got;
        return;
    }
    case SYS_SOCK_ALPN: {
        struct proc *p = proc_current();
        int max = (int)r->rdx;
        if (!p || max <= 0 || !user_range_ok((void *)r->rsi, (uint64_t)max, 1))
            { r->rax = (uint64_t)(long)SOCK_E_ARG; return; }
        if (max > SYSCALL_IO_MAX) max = SYSCALL_IO_MAX;
        SYSCALL_BUF(tmp, max);
        if (!tmp) { r->rax = (uint64_t)-1; return; }
        long got = sock_alpn((int)r->rdi, (void *)tmp, max, p->pid);
        if (got >= 0 && user_copy_to((void *)r->rsi, tmp, (uint64_t)got + 1) < 0) got = -1;
        r->rax = (uint64_t)got;
        return;
    }
    case SYS_SOCK_CLOSE: {
        struct proc *p = proc_current();
        if (!p) { r->rax = (uint64_t)(long)SOCK_E_ARG; return; }
        r->rax = (uint64_t)(long)sock_close((int)r->rdi, p->pid);
        return;
    }

    /* --- SERVER SOCKETS (140-149) -------------------------------------------
     * The other direction: connections this machine ANSWERS. See the long note
     * above SYS_SOCKET in include/abi/logit_abi.h for the blocking model and
     * for the net_poll constraint that shapes it.
     *
     * These sit here rather than in wm_gui_syscall for the same reason the
     * SYS_SOCK_* block above gives -- a server is a process-level service and
     * /bin/httpd has no window.
     *
     * ONE THING TO KNOW ABOUT THE BLOCKING ONES. int 0x80 is an interrupt gate,
     * so a syscall body runs with IF=0. SYS_ACCEPT and a blocking read on a
     * socket fd do NOT re-enable interrupts the way SYS_HTTP_GET does, and they
     * do not need to: they PARK on a wait queue rather than spinning on a
     * timeout loop, and sched_block_self_unlock switches away to a thread whose
     * own saved flags have IF set. The trap SYS_HTTP_GET documents is a trap
     * for code that waits by looping; this code waits by not running. */
    case SYS_SOCKET: {
        struct proc *p = proc_current();
        if (!p) { r->rax = (uint64_t)(long)LSK_E_ARG; return; }
        int err = 0;
        struct file *f = lsock_create((int)r->rdi, (int)r->rsi, (int)r->rdx,
                                      p->pid, &err);
        if (!f) { r->rax = (uint64_t)(long)err; return; }
        int fd = proc_fd_alloc(p, f);
        if (fd < 0) { file_close(f); r->rax = (uint64_t)(long)LSK_E_FULL; return; }
        r->rax = (uint64_t)(long)fd;
        return;
    }
    /* AF_UNIX arrives here too, and the two families have DIFFERENT address
     * structs (8 bytes of number vs 110 bytes of path). The family is read
     * first, from the two bytes both shapes share at offset 0, and the rest of
     * the copy-in is sized from it -- copying `sizeof(struct logit_sockaddr_un)`
     * unconditionally would fault an AF_INET caller who legitimately passed an
     * 8-byte struct at the end of a page. */
    case SYS_BIND: {
        struct proc *p = proc_current();
        struct file *f FILE_REF = p ? proc_fd_acquire(p, (int)r->rdi) : NULL;
        unsigned short fam;
        if (!f || !user_range_ok((const void *)r->rsi, sizeof fam, 0))
            { r->rax = (uint64_t)(long)LSK_E_ARG; return; }
        if (user_copy_from(&fam, (const void *)r->rsi, sizeof fam) < 0) { r->rax = (uint64_t)(long)LSK_E_ARG; return; }
        if (fam == LOGIT_AF_UNIX) {
            r->rax = (uint64_t)(long)unix_addr_call(p, f, (const void *)r->rsi, 0);
            return;
        }
        struct logit_sockaddr a;
        if (!user_range_ok((const void *)r->rsi, sizeof a, 0))
            { r->rax = (uint64_t)(long)LSK_E_ARG; return; }
        if (user_copy_from(&a, (const void *)r->rsi, sizeof a) < 0) { r->rax = (uint64_t)(long)LSK_E_ARG; return; }   /* struct assignment: no
                                     * libc memcpy is declared in this TU */
        r->rax = (uint64_t)(long)lsock_bind(f, &a);
        return;
    }
    /* SYS_CONNECT is AF_UNIX ONLY -- see the block above it in logit_abi.h. An
     * AF_INET address is refused by name here rather than being handed to a
     * lsock_connect() that does not exist. */
    case SYS_CONNECT: {
        struct proc *p = proc_current();
        struct file *f FILE_REF = p ? proc_fd_acquire(p, (int)r->rdi) : NULL;
        unsigned short fam;
        if (!f || !user_range_ok((const void *)r->rsi, sizeof fam, 0))
            { r->rax = (uint64_t)(long)LSK_E_ARG; return; }
        if (user_copy_from(&fam, (const void *)r->rsi, sizeof fam) < 0) { r->rax = (uint64_t)(long)LSK_E_ARG; return; }
        /* 2026-09-11 correction to the historical comment above: IPv4
         * blocking connect now uses an owned ordinary socket descriptor. */
        if(fam==LOGIT_AF_INET){struct logit_sockaddr a;
            if(r->rdx<sizeof a){r->rax=(uint64_t)(long)LSK_E_ARG;return;}
            if(user_copy_from(&a,(void *)r->rsi,sizeof a)<0){r->rax=(uint64_t)(long)LSK_E_ARG;return;}
            r->rax=(uint64_t)(long)lsock_connect_inet(f,&a);return;}
        if (fam != LOGIT_AF_UNIX) { r->rax = (uint64_t)(long)LSK_E_ARG; return; }
        r->rax = (uint64_t)(long)unix_addr_call(p, f, (const void *)r->rsi, 1);
        return;
    }
    case SYS_SOCKETPAIR: {
        struct proc *p = proc_current();
        int *sv = (int *)r->rdx;
        if (!p || !user_range_ok(sv, 2 * sizeof(int), 1))
            { r->rax = (uint64_t)(long)LSK_E_ARG; return; }
        struct file *fa = NULL, *fb = NULL;
        int err = 0;
        if (lsock_socketpair((int)r->rdi, (int)r->rsi, 0, p->pid, &fa, &fb, &err) != 0)
            { r->rax = (uint64_t)(long)err; return; }
        /* Both fds are installed BEFORE anything is copied out, and if the
         * second slot cannot be had the first is given back -- a caller whose
         * sv[] holds one live descriptor and one -1 has leaked a socket it was
         * never told about. */
        int fds[2];
        struct file *hold_a FILE_REF = fa, *hold_b FILE_REF = fb;
        file_dup(fa); file_dup(fb); /* retain identity through copyout rollback */
        if (proc_fd_pair(p, fa, fb, fds) < 0) {
            file_close(fa); file_close(fb); r->rax = (uint64_t)(long)LSK_E_FULL; return;
        }
        if (user_copy_to(sv, fds, sizeof fds) < 0) {
            proc_fd_close_if(p, fds[0], hold_a); proc_fd_close_if(p, fds[1], hold_b); r->rax = (uint64_t)(long)LSK_E_ARG; return;
        }
        r->rax = 0; return;
    }

    case SYS_LISTEN: {
        struct proc *p = proc_current();
        struct file *f FILE_REF = p ? proc_fd_acquire(p, (int)r->rdi) : NULL;
        if (!f) { r->rax = (uint64_t)(long)LSK_E_ARG; return; }
        r->rax = (uint64_t)(long)lsock_listen(f, (int)r->rsi);
        return;
    }
    case SYS_ACCEPT: {
        struct proc *p = proc_current();
        struct file *f FILE_REF = p ? proc_fd_acquire(p, (int)r->rdi) : NULL;
        if (!f) { r->rax = (uint64_t)(long)LSK_E_ARG; return; }
        struct logit_sockaddr peer = {0}; /* unnamed AF_UNIX peer has no address */
        int want_peer = r->rsi != 0;
        if (want_peer && !user_range_ok((void *)r->rsi, sizeof peer, 1))
            { r->rax = (uint64_t)(long)LSK_E_ARG; return; }
        int err = 0;
        struct file *cf = lsock_accept(f, want_peer ? &peer : NULL,
                                       (unsigned)r->rdx, &err);
        if (!cf) { r->rax = (uint64_t)(long)err; return; }
        /* Install the fd BEFORE copying out: a failed copy-out with the fd
         * already allocated is a leaked connection, and the fd table is the
         * thing the caller cannot clean up itself. */
        struct file *hold_cf FILE_REF = cf;
        file_dup(cf);
        int fd = proc_fd_alloc(p, cf);
        if (fd < 0) { file_close(cf); r->rax = (uint64_t)(long)LSK_E_FULL; return; }
        if (want_peer && user_copy_to((void *)r->rsi, &peer, sizeof peer) < 0) { proc_fd_close_if(p, fd, hold_cf); r->rax = (uint64_t)(long)LSK_E_ARG; return; }
        r->rax = (uint64_t)(long)fd;
        return;
    }
    case SYS_GETSOCKNAME: {
        struct proc *p = proc_current();
        struct file *f FILE_REF = p ? proc_fd_acquire(p, (int)r->rdi) : NULL;
        if (!f) { r->rax = (uint64_t)(long)LSK_E_ARG; return; }
        /* Dispatched on the SOCKET, not on the buffer, because the buffer is
         * write-only here and has nothing in it to read a family out of. An
         * AF_UNIX socket answers with a path; every other kind refuses the path
         * form and falls through to the address one. */
        struct logit_sockaddr_un un;
        if (lsock_getsockname_unix(f, un.path, sizeof un.path) == 0) {
            if (!user_range_ok((void *)r->rsi, sizeof un, 1))
                { r->rax = (uint64_t)(long)LSK_E_ARG; return; }
            un.family = LOGIT_AF_UNIX;
            if (user_copy_to((void *)r->rsi, &un, sizeof un) < 0) { r->rax = (uint64_t)(long)LSK_E_ARG; return; }
            r->rax = 0;
            return;
        }
        struct logit_sockaddr a;
        if (!user_range_ok((void *)r->rsi, sizeof a, 1))
            { r->rax = (uint64_t)(long)LSK_E_ARG; return; }
        int rc = lsock_getsockname(f, &a);
        if (rc == 0 && user_copy_to((void *)r->rsi, &a, sizeof a) < 0) rc = LSK_E_ARG;
        r->rax = (uint64_t)(long)rc;
        return;
    }
    case SYS_SETSOCKOPT: {
        struct proc *p = proc_current();
        struct file *f FILE_REF = p ? proc_fd_acquire(p, (int)r->rdi) : NULL;
        if (!f) { r->rax = (uint64_t)(long)LSK_E_ARG; return; }
        int level = (int)((r->rsi >> 16) & 0xFFFF);
        int opt   = (int)(r->rsi & 0xFFFF);
        r->rax = (uint64_t)(long)lsock_setsockopt(f, level, opt, (long)r->rdx);
        return;
    }
    case SYS_SHUTDOWN: {
        struct proc *p = proc_current();
        struct file *f FILE_REF = p ? proc_fd_acquire(p, (int)r->rdi) : NULL;
        if (!f) { r->rax = (uint64_t)(long)LSK_E_ARG; return; }
        r->rax = (uint64_t)(long)lsock_shutdown(f, (int)r->rsi);
        return;
    }
    case SYS_RECVFROM:
    case SYS_SENDTO: {
        struct proc *p = proc_current();
        struct file *f FILE_REF = p ? proc_fd_acquire(p, (int)r->rdi) : NULL;
        struct logit_dgram d;
        /* recvfrom writes the sender back into the caller's struct; sendto only
         * reads it. Checking for write access on both would refuse a sendto
         * from a read-only mapping, which is legal. */
        if (!f || !user_range_ok((void *)r->rsi, sizeof d, r->rax == SYS_RECVFROM))
            { r->rax = (uint64_t)(long)LSK_E_ARG; return; }
        if (user_copy_from(&d, (const void *)r->rsi, sizeof d) < 0) { r->rax = (uint64_t)(long)LSK_E_ARG; return; }
        if (d.len < 0 || d.len > SYSCALL_IO_MAX || d.flags != 0)
            { r->rax = (uint64_t)(long)LSK_E_ARG; return; }
        int writing = (r->rax == SYS_RECVFROM);
        if (d.len > 0 && !user_range_ok(d.buf, (uint64_t)d.len, writing))
            { r->rax = (uint64_t)(long)LSK_E_ARG; return; }
        /* struct logit_dgram carries the address flattened (gen_abi.py lays out
         * scalars only); lsock takes it as a struct logit_sockaddr. Same three
         * fields, converted here so exactly one of the two shapes is public. */
        struct logit_sockaddr sa;
        sa.family = d.family; sa.port = d.port; sa.addr = d.addr;
        SYSCALL_BUF(tmp, d.len);
        if (!tmp) { r->rax = (uint64_t)(long)LSK_E_ARG; return; }
        long rc;
        if (r->rax == SYS_RECVFROM) {
            rc = lsock_recvfrom(f, tmp, d.len, &sa);
            if (rc > 0 && user_copy_to(d.buf, tmp, (uint64_t)rc) < 0) rc = LSK_E_ARG;
            if (rc > 0) {                                       /* the sender */
                d.family = sa.family; d.port = sa.port; d.addr = sa.addr;
                if (user_copy_to((void *)r->rsi, &d, sizeof d) < 0) rc = LSK_E_ARG;
            }
        } else {
            if (user_copy_from(tmp, d.buf, (uint64_t)d.len) < 0) rc = LSK_E_ARG;
            else rc = lsock_sendto(f, tmp, d.len, &sa);
        }
        r->rax = (uint64_t)rc;
        return;
    }
    case SYS_SOCKSTAT:
        r->rax = (uint64_t)lsock_stat((int)r->rdi);
        return;

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
        if (n <= 0 || kernel_img_decode(file, n, &im) != 0) { kfree(file); r->rax = (uint64_t)-1; return; }
        kfree(file);
        long need = (long)im.w * im.h * 4;
        if (need <= 0 || need > req.max) { img_free(&im); r->rax = (uint64_t)-1; return; }
        int copy_rc = user_copy_to(req.rgba, im.rgba, (uint64_t)need);
        req.w = im.w; req.h = im.h;
        if (copy_rc == 0) copy_rc = user_copy_to((void *)r->rdi, &req, sizeof req);
        if (copy_rc < 0) { img_free(&im); r->rax = (uint64_t)-1; return; }
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
    /* Capture (mic / line-in), added with the HDA capture work. Same
     * forwarding reason as the playback six above: which argument is a user
     * buffer is an audio fact, not a dispatcher fact. */
    case SYS_SND_CAP_OPEN:
    case SYS_SND_CAP_READ:
    case SYS_SND_CAP_AVAIL:
    case SYS_SND_CAP_CLOSE:
    case SYS_SND_CAP_STATE:
        r->rax = (uint64_t)snd_syscall((long)r->rax, (long)r->rdi,
                                       (long)r->rsi, (long)r->rdx);
        return;

    /* SYS_PTRACE: one process reading another. Handled here rather than
     * falling through to wm_gui_syscall's default for the reason SYS_RUSAGE
     * gives about itself -- a debugger has no window. Three scalars, so the
     * dispatch needs nothing beyond the label; every rule about who may is in
     * c/kernel/exec/ptrace.c. */
    case SYS_PTRACE:
        r->rax = (uint64_t)ptrace_syscall((long)r->rdi, (long)r->rsi,
                                          (long)r->rdx);
        return;

    case SYS_MMAP:
    case SYS_MPROTECT:   /* (addr, len, prot); three scalars, so it needs nothing
                          * here beyond the label -- see logit_abi.h for why it
                          * is its own number and not a flag on SYS_MMAP */
    case SYS_MMAP_FILE:  /* file-backed mmap; one argument, a struct pointer in
                          * rdi -- see the case in mmsys.c for the whole story */
        /* THESE THREE FELL THROUGH INTO SYS_RUSAGE AND WERE ANSWERED BY
         * sched_rusage_syscall(), 2026-08-20, from f3b2a97f6 -- a commit about
         * a PTE-bit collision that inserted the SYS_RUSAGE label into the
         * middle of this group and took its `mm_syscall` forwarding with it.
         * SYS_MUNMAP and SYS_MEMINFO below kept theirs, so munmap and meminfo
         * worked and mmap did not, which is why nothing looked obviously
         * broken at boot.
         *
         * WHAT IT COST, because it is worth knowing how quiet this was: every
         * mini-libc program on the machine. c/apps/libc/src/malloc.c's
         * arena_map() is `return (void *)arena_sys(92, ...)` with no check for
         * a negative return -- vma_reserve() reports failure as 0, so NULL was
         * the only failure the caller was written for -- and rusage's -1 became
         * the heap base. /bin/as then faulted on its FIRST malloc, storing to
         * 0xffffffffffffffff, before printing a single character. On the serial
         * log that is one [fault] line with an address in it and nothing else.
         *
         * Found by the core dump this line of work adds: the dump named the
         * faulting instruction (`mov %edx,(%rax)` in malloc_nl), said rax was
         * -1, and listed the process's whole region table with NO anonymous
         * mmap area in it -- which is three facts pointing at one call, from a
         * file, in one step. That is the entire argument for having them.
         *
         * The shape restored here is the one c/kernel/mm/mmsys.c:52-59 writes
         * out verbatim as the dispatch it expects. */
        r->rax = (uint64_t)mm_syscall((long)r->rax, (long)r->rdi,
                                      (long)r->rsi, (long)r->rdx);
        return;

    /* SYS_RUSAGE: per-thread CPU time and RLIMIT_CPU. Handled here rather
     * than falling to wm_gui_syscall's default, for the same reason
     * SYS_THREAD_* is -- a CLI process with no window still has a CPU-time
     * budget. Recovered from stash@{0}; see branch rescue-1845. */
    case SYS_RUSAGE:
        if(r->rdi==RUCTL_GET_RSS_FRAMES){
            struct proc *p=proc_current();if(!p){r->rax=(uint64_t)-1;return;}
            /* Measure the caller's live address space under its existing
             * owner. Never inspect another process by a recyclable PID. */
            uint64_t space=p->cr3;MM_GUARD(space);
            r->rax=oom_rss_frames(space);return;
        }
        r->rax = (uint64_t)sched_rusage_syscall((long)r->rdi, (long)r->rsi,
                                                (long)r->rdx);
        return;

    /* SYS_SCHED: nice/weight. Handled beside SYS_RUSAGE and for the identical
     * reason -- a CLI process with no window still has a priority, so it must
     * not fall through to wm_gui_syscall's default. Three scalars, so the
     * dispatch needs nothing beyond the label; the state, the permission rule
     * and the clamp all live in c/kernel/sched/sched.c. */
    case SYS_SCHED:
        r->rax = (uint64_t)sched_prio_syscall((long)r->rdi, (long)r->rsi,
                                              (long)r->rdx);
        return;

    case SYS_MUNMAP:
    case SYS_MEMINFO:
    /* Shared memory (176-179). Four more labels on the group that already
     * forwards here: every one of them is three scalars in rdi/rsi/rdx, so the
     * dispatch line needed nothing beyond the labels themselves. The bodies and
     * the argument validation are in c/kernel/mm/mmsys.c, which is where that
     * file argues they belong. */
    case SYS_SHM_OPEN:
    case SYS_SHM_MAP:
    case SYS_SHM_UNLINK:
    case SYS_SHM_CLOSE:
        r->rax = (uint64_t)mm_syscall((long)r->rax, (long)r->rdi,
                                      (long)r->rsi, (long)r->rdx);
        return;

    /* M30 threads. Forwarded whole to c/kernel/sched/uthread.c for the reason
     * mm_syscall() and proc_syscall() give: which argument is a user pointer
     * and what it means are facts about threads, and they belong beside the
     * table that knows them. Handled here rather than in wm_gui_syscall for the
     * same reason the socket calls are: a thread is a process-level service and
     * a CLI program has no window. */
    case SYS_THREAD_CREATE:
    case SYS_THREAD_EXIT:
    case SYS_THREAD_JOIN:
    case SYS_THREAD_DETACH:
    case SYS_THREAD_SELF:
    case SYS_SET_TLS:
    case SYS_FUTEX:
    case SYS_THREAD_INFO:
        r->rax = (uint64_t)uthread_syscall((long)r->rax, (long)r->rdi,
                                           (long)r->rsi, (long)r->rdx);
        return;

    /* Waiting on several descriptors at once. Forwarded whole to
     * c/kernel/exec/fd/kpollsys.c for the reason mm_syscall() and uthread_syscall()
     * are: which argument is a user pointer and what it means are facts about
     * this subsystem. Proc-level and not GUI -- a server has no window, and the
     * shell's own descriptors are exactly what this is for. */
    case SYS_POLL:
    case SYS_EVENTFD:
    case SYS_TIMERFD:
        r->rax = (uint64_t)poll_syscall((long)r->rax, (long)r->rdi,
                                        (long)r->rsi, (long)r->rdx);
        return;

    case SYS_PROCS:
    case SYS_KILL:
    case SYS_CAP_QUERY:   /* M28: read-only introspection of the caller's own grant */
        r->rax = (uint64_t)proc_syscall((long)r->rax, (long)r->rdi,
                                        (long)r->rsi, (long)r->rdx);
        return;

    /* M28: fork+load+execve a capability-BOUNDED child in one call. The whole
     * body -- copying path/argv/the request struct, the ceiling check, the
     * image load, the new proc -- lives in exec.c beside proc_execve() and
     * proc_spawn(), which already own every piece of machinery this needs
     * (setup_cli_stack, copy_uvec, the ELF loader). See proc_cap_spawn()
     * there for the full contract and why this is a fresh number rather than
     * SYS_SPAWN (63) revived. */
    case SYS_CAP_SPAWN:
        r->rax = (uint64_t)proc_cap_spawn(r);
        return;

    /* Signals (130-136). Forwarded whole to c/kernel/exec/signal/ksignal.c for the
     * reason mm_syscall() and uthread_syscall() give: which argument is a user
     * pointer and what it means are facts about signals, and they belong beside
     * the table that knows them. Proc-level and not GUI: /bin/sh, the coreutils
     * and every .as script have every right to a SIGINT handler and no window
     * for the WM to resolve them through.
     *
     * SYS_SIGRETURN is NOT in this list, and cannot be. Restoring a frame means
     * writing the register set that is about to be iretq'd and the FXSAVE area
     * c/boot/isr.asm will FXRSTOR; a syscall body reaches neither, so it is
     * intercepted in interrupt_handler ahead of this dispatcher. Reaching
     * ksignal.c's SYS_SIGRETURN case at all means somebody called it by hand,
     * and it answers SIG_E_ARG. */
    case SYS_SIGACTION:
    case SYS_SIGPROCMASK:
    case SYS_SIGRETURN:
    case SYS_SIGPENDING:
    case SYS_ALARM:
    case SYS_SIGSUSPEND:
    case SYS_SIGQUERY:
        r->rax = (uint64_t)ksig_syscall((long)r->rax, (long)r->rdi,
                                        (long)r->rsi, (long)r->rdx);
        return;

    /* M31 file metadata (120-129). Forwarded whole to c/kernel/exec/load/meta.c for
     * the reason mm_syscall() gives: which argument is a user pointer and what
     * it means are facts about this subsystem. Proc-level, not GUI: a CLI
     * program is the main caller and has no window. */
    case SYS_FSREF:
    case SYS_STAT:
    case SYS_LSTAT:
    case SYS_FSTAT:
    case SYS_GETDENTS:
    case SYS_CHMOD:
    case SYS_UMASK:
    case SYS_SYMLINK:
    case SYS_READLINK:
    case SYS_LINK:
    case SYS_CHOWN:
        r->rax = (uint64_t)meta_syscall((long)r->rax, (long)r->rdi,
                                        (long)r->rsi, (long)r->rdx);
        return;

    /* M32 identity (150-159). Forwarded to c/fs/vfs_cred.c, which is where the
     * credential table lives -- same rule as the block above. This is the half
     * of the permission model that was missing: the VFS has checked a uid on
     * every path walk for a long time, and until these landed nothing could
     * set that uid to anything but 0. */
    case SYS_GETUID:
    case SYS_GETEUID:
    case SYS_GETGID:
    case SYS_GETEGID:
    case SYS_SETUID:
    case SYS_SETGID:
    case SYS_GETGROUPS:
    case SYS_SETGROUPS:
    case SYS_GETSESSION:
        r->rax = (uint64_t)id_syscall((long)r->rax, (long)r->rdi,
                                      (long)r->rsi, (long)r->rdx);
        return;

    /* SYS_SETSESSION is the same forward, WRAPPED -- and this is the only
     * place in the kernel where the two halves of "who is using this machine"
     * meet, so the reasoning belongs here rather than in a header.
     *
     * The settings store used to be one file, /etc/settings.conf, root:root
     * 0600, written through vfs_write() with the CALLING process's credential.
     * The moment /bin/login started dropping the desktop to a real uid, a user
     * toggling dark mode was refused and their choice was gone at the next
     * boot. The fix is a per-user store (c/kernel/core/settings.h), and the
     * only thing it needs is to be told, once, whose it is.
     *
     * WHY IT IS TWO CALLS AROUND ONE. The user's home is looked up in
     * /etc/passwd, which is root:root 0600 -- and this syscall is the instant
     * the caller stops being root. So the lookup happens BEFORE (prepare) and
     * the switch happens AFTER (adopt), which also means the user's own
     * settings file is read with the user's own credential, as it should be.
     * Doing both before would read their file as root; doing both after would
     * find /etc/passwd unreadable and silently give every user the defaults.
     *
     * WHY HERE AND NOT IN /bin/login. Because the greeter authenticates too
     * (c/apps/gui/greeter.c), and so will the next thing that does. A hook a
     * userland program has to remember to call is a hook that will be missing
     * from one of them, and the symptom -- "settings do not persist, but only
     * when you log in through the other door" -- is nearly unfindable.
     *
     * A FAILURE HERE NEVER FAILS THE LOGIN. No row for that uid, or an
     * unreadable store, means the user gets the system settings read-only:
     * a worse desktop, not a refused one. */
    case SYS_SETSESSION: {
        settings_session_lock();
        int prepared = (settings_prepare_user((unsigned)r->rdi) == 0);
        long rc = id_syscall((long)r->rax, (long)r->rdi,
                             (long)r->rsi, (long)r->rdx);
        if (rc == 0 && prepared) settings_adopt_user();
        else                     settings_discard_user();
        settings_session_unlock();
        r->rax = (uint64_t)rc;
        return;
    }

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

    /* Entropy. Routed here rather than through wm_gui_syscall for the same
     * reason the clipboard is: a CLI process -- /bin/sh, an .as script, the
     * package verifier -- has every right to ask the kernel for random bytes
     * and has no window for the WM to resolve it through. The number and the
     * GRND_* flags were arbitrated in include/abi/logit_abi.h; the back end is
     * c/kernel/core/rng.c, which is also what TLS has always used. */
    case SYS_GETRANDOM:
        /* The NUMBER and the flags are published by this line in
         * include/abi/logit_abi.h -- the crypto line asked for them and they
         * were arbitrated here. The BACK END is theirs: rng_syscall() in
         * c/kernel/core/rng.c, over the SHA-256 Hash_DRBG that has been in the
         * kernel since the TLS work and that ring 3 could not reach.
         *
         * WEAK, for the reason proc.c's sock_close_owner is weak: the two
         * halves are owned by different lines and land in different commits,
         * and a dispatcher that hard-depends on a back end it does not own
         * turns "their file is mid-edit" into "the kernel does not link". A
         * build without it answers -1, which is what an unimplemented syscall
         * answered before the number existed. */
        r->rax = LOGIT_HAVE(rng_syscall)
               ? (uint64_t)rng_syscall((long)r->rdi, (long)r->rsi, (long)r->rdx)
               : (uint64_t)-1;
        return;

    /* Stopping the machine. ROOT ONLY, refused with ID_E_PERM exactly as
     * SYS_SETUID is (see id_syscall() above) -- checked HERE, inline, rather
     * than forwarded to vfs_cred.c, because there is nothing else these two
     * numbers share with the identity block: no shared state, no shared
     * struct vcred plumbing beyond the one uid read. UNCLASSIFIED by
     * syscall_cap_class() above on purpose -- see the doc comment on
     * SYS_POWEROFF in logit_abi.h for why a CAP_POWER bit was considered and
     * deliberately not invented.
     *
     * Neither kernel_poweroff() nor kernel_reboot() returns on the path that
     * works, so there is no rax to set there -- reaching a `return` after
     * either call is dead code the compiler already knows is dead (both are
     * declared noreturn in power.h), not a case this dispatcher forgot to
     * finish.
     * Correction: successful hardware shutdown/reset still cannot return, but
     * drain/sync/hardware refusal now restores service and returns an error.
     * Preserve that result instead of falling through to another syscall. */
    case SYS_POWEROFF: {
        struct vcred me;
        vfs_cred_current(&me);
        if (me.uid != 0) { r->rax = (uint64_t)ID_E_PERM; return; }
        r->rax = (uint64_t)kernel_poweroff();
        return;
    }
    case SYS_REBOOT: {
        struct vcred me;
        vfs_cred_current(&me);
        if (me.uid != 0) { r->rax = (uint64_t)ID_E_PERM; return; }
        r->rax = (uint64_t)kernel_reboot();
        return;
    }

    /* Loadable kernel modules (c/kernel/module/). The uid check is NOT here
     * with the two above it: mod_syscall does it per operation, because
     * SYS_MODULE_LIST is deliberately not root-only and a check at this level
     * could not express that. mod_syscall must NOT go on
     * syscall_is_bkl_free()'s allow-list -- it calls kmalloc, the VFS and
     * driver_register, and the last of those mutates a global list that has no
     * lock of its own because it was written for boot-time use. */
    case SYS_MODULE_LOAD:
    case SYS_MODULE_UNLOAD:
    case SYS_MODULE_LIST:
    case SYS_MODULE_SYM:
        r->rax = (uint64_t)mod_syscall((long)r->rax, (long)r->rdi,
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
