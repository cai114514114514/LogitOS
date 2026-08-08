#ifndef LOGIT_INTERRUPTS_H
#define LOGIT_INTERRUPTS_H

#include <stdint.h>

/* CPU + manually pushed register state at the point an interrupt was taken.
 * Field order MUST match the push sequence in boot/isr.asm. */
struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;        /* pushed by stubs (+ CPU for some) */
    uint64_t rip, cs, rflags, rsp, ss;  /* pushed by the CPU */
};

/* Common C dispatcher, called from the assembly stub.
 *
 * `fxarea` is the 512-byte FXSAVE area boot/isr.asm filled on entry and will
 * FXRSTOR on the way out. It is passed rather than left implicit because a
 * signal frame must carry the interrupted program's FP/SSE state, and by the
 * time delivery runs kernel C has already clobbered the live registers -- so
 * the saved copy is the only correct source. See c/kernel/exec/ksigframe.c. */
void interrupt_handler(struct registers *regs, void *fxarea);

#endif /* LOGIT_INTERRUPTS_H */
