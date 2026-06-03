#ifndef AQUA_SYSCALL_H
#define AQUA_SYSCALL_H

#include "interrupts.h"

/* Dispatched from the int 0x80 handler. (Syscall numbers live in aqua_abi.h.) */
void syscall_dispatch(struct registers *regs);

#endif /* AQUA_SYSCALL_H */
