#ifndef AQUA_INTERRUPTS_H
#define AQUA_INTERRUPTS_H

#include <stdint.h>

/* CPU + manually pushed register state at the point an interrupt was taken.
 * Field order MUST match the push sequence in boot/isr.asm. */
struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;        /* pushed by stubs (+ CPU for some) */
    uint64_t rip, cs, rflags, rsp, ss;  /* pushed by the CPU */
};

/* Common C dispatcher, called from the assembly stub. */
void interrupt_handler(struct registers *regs);

#endif /* AQUA_INTERRUPTS_H */
