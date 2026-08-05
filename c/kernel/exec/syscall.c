#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "aether_abi.h"
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
#include "img.h"
#include "kheap.h"
#include "percpu.h"
#include "kprintf.h"

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

void syscall_dispatch(struct registers *r)
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
        if (!user_range_ok((void *)r->rdi, sizeof(struct rtc_time), 1)) { r->rax = (uint64_t)-1; return; }
        struct rtc_time t; rtc_now(&t);
        user_copy_to((void *)r->rdi, &t, sizeof t);
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
        struct aether_netinfo *ni = (struct aether_netinfo *)r->rdi;
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

    case SYS_IMG_DECODE: {
        /* Decode an image file (PNG/GIF) in-kernel and hand the RGBA back to the
         * caller's buffer -- so the Preview app needs no codec/libc of its own. */
        struct aether_imgreq req;
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

    default:
        /* GUI + misc system calls are handled by the window manager, which
         * resolves the calling app via the scheduler's current thread. */
        r->rax = (uint64_t)wm_gui_syscall((long)r->rax, (long)r->rdi,
                                          (long)r->rsi, (long)r->rdx);
        return;
    }
}
