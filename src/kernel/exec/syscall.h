#ifndef AQUA_SYSCALL_H
#define AQUA_SYSCALL_H

#include "interrupts.h"

/* Dispatched from the int 0x80 handler. (Syscall numbers live in aqua_abi.h.) */
void syscall_dispatch(struct registers *regs);

/* M25 P1: 1 if this syscall number runs WITHOUT the Big Kernel Lock (self-locked
 * via fine-grained locks). interrupt_handler uses this to skip the BKL acquire. */
int syscall_is_bkl_free(int n);

#endif /* AQUA_SYSCALL_H */
