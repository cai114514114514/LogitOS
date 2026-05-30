#include <stdint.h>
#include "syscall.h"
#include "aqua_abi.h"
#include "serial.h"
#include "wm.h"
#include "sched.h"

void syscall_dispatch(struct registers *r)
{
    switch (r->rax) {
    case SYS_WRITE: {
        const char *buf = (const char *)r->rsi;     /* user pointer, mapped */
        long len = (long)r->rdx;
        for (long i = 0; i < len; i++)
            serial_putc(buf[i]);
        r->rax = (uint64_t)len;
        return;
    }
    case SYS_EXIT:
        wm_app_exit();          /* mark the app dead so the WM reaps its window */
        thread_exit();          /* remove this thread; does not return */
        return;
    default:
        /* GUI + misc system calls are handled by the window manager, which
         * resolves the calling app via the scheduler's current thread. */
        r->rax = (uint64_t)wm_gui_syscall((long)r->rax, (long)r->rdi,
                                          (long)r->rsi, (long)r->rdx);
        return;
    }
}
