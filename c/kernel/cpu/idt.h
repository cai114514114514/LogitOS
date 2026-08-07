#ifndef LOGIT_IDT_H
#define LOGIT_IDT_H

/* Build and load the 64-bit Interrupt Descriptor Table (vectors 0..47:
 * 32 CPU exceptions + 16 remapped IRQs). */
void idt_init(void);
void idt_load(void);     /* load the shared IDT on an application processor */

/* Install a ring-0 (DPL 0) interrupt gate for a vector idt_init does not claim.
 * `handler` is a raw entry point: the CPU frame is on the stack and it must
 * return with iretq. See the comment on the definition for why this exists. */
void idt_install_gate(int vec, void *handler);

#endif /* LOGIT_IDT_H */
