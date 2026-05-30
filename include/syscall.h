#ifndef AQUA_SYSCALL_H
#define AQUA_SYSCALL_H

#include "interrupts.h"

/* System call numbers (Aqua ABI): rax = number, rdi/rsi/rdx = args. */
#define SYS_WRITE 1
#define SYS_EXIT  2

/* Dispatched from the int 0x80 handler. */
void syscall_dispatch(struct registers *regs);

/* Everything userland has written via SYS_WRITE (NUL-terminated), for the
 * desktop's Console window to display. */
const char *syscall_console(void);

#endif /* AQUA_SYSCALL_H */
