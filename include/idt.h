#ifndef AQUA_IDT_H
#define AQUA_IDT_H

/* Build and load the 64-bit Interrupt Descriptor Table (vectors 0..47:
 * 32 CPU exceptions + 16 remapped IRQs). */
void idt_init(void);

#endif /* AQUA_IDT_H */
