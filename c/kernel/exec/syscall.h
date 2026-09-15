#ifndef LOGIT_SYSCALL_H
#define LOGIT_SYSCALL_H

#include "interrupts.h"

/* Dispatched from the int 0x80 handler. (Syscall numbers live in logit_abi.h.) */
void syscall_dispatch(struct registers *regs, const void *user_fxarea);
/* SIGRETURN bypasses dispatch but must still observe exit and TLB epochs. */
void syscall_entry_checks(void);

/* M25 P1: 1 if this syscall number runs WITHOUT the Big Kernel Lock (self-locked
 * via fine-grained locks). interrupt_handler uses this to skip the BKL acquire.
 * Correction: that allow-list API has been removed. Every syscall uses its
 * object locks; the following decoder wrapper owns shared codec/gfx scratch. */
struct image;
int kernel_img_decode(const unsigned char *data, int len, struct image *out);

#endif /* LOGIT_SYSCALL_H */
