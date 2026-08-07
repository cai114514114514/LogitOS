#ifndef LOGIT_IDT_H
#define LOGIT_IDT_H

/* Build and load the 64-bit Interrupt Descriptor Table (vectors 0..47:
 * 32 CPU exceptions + 16 remapped IRQs). */
void idt_init(void);
void idt_load(void);     /* load the shared IDT on an application processor */

#endif /* LOGIT_IDT_H */
