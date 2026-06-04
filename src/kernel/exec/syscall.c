#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "aqua_abi.h"
#include "serial.h"
#include "wm.h"
#include "sched.h"
#include "usercopy.h"
#include "proc.h"
#include "file.h"
#include "vfs.h"

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
    case SYS_EXIT:
        proc_exit((int)r->rdi);  /* zombie + close fds + mark window dead; never returns */
        return;
    case SYS_FORK:
        r->rax = (uint64_t)proc_fork(r);
        return;
    case SYS_GETPID: {
        struct proc *p = proc_current();
        r->rax = p ? (uint64_t)p->pid : (uint64_t)-1;
        return;
    }
    case SYS_WAITPID: {
        int status = 0;
        long rc = proc_waitpid((int)r->rdi, &status);
        if (rc >= 0 && r->rsi) user_copy_to((void *)r->rsi, &status, sizeof(int));
        r->rax = (uint64_t)rc;
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
