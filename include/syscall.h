#ifndef AQUA_SYSCALL_H
#define AQUA_SYSCALL_H

#include "interrupts.h"

/* System call numbers (Aqua ABI): rax = number, rdi/rsi/rdx = args. */
#define SYS_WRITE 1
#define SYS_EXIT  2

/* Dispatched from the int 0x80 handler. */
void syscall_dispatch(struct registers *regs);

#endif /* AQUA_SYSCALL_H */
