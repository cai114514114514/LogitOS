#include <stdint.h>
#include "syscall.h"
#include "aqua_abi.h"
#include "serial.h"
#include "wm.h"
#include "sched.h"
#include "usercopy.h"
#include "proc.h"

void syscall_dispatch(struct registers *r)
{
    switch (r->rax) {
    case SYS_WRITE: {
        const char *buf = (const char *)r->rsi;     /* user pointer, mapped */
        long len = (long)r->rdx;
        if (len < 0 || !user_range_ok(buf, (uint64_t)len, 0)) {
            r->rax = (uint64_t)-1;
            return;
        }
        for (long i = 0; i < len; i++)               /* fd routing added in P2; fd 1/2 -> serial */
            serial_putc(buf[i]);
        r->rax = (uint64_t)len;
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
