#ifndef AQUA_PIC_H
#define AQUA_PIC_H

/* 8259 PIC: remap IRQs 0..15 to interrupt vectors 32..47 and unmask the
 * timer (IRQ0) and keyboard (IRQ1). */
void pic_remap(void);

/* Signal end-of-interrupt for the given IRQ line (0..15). */
void pic_eoi(int irq);

/* Mask all PIC lines (when IRQs are routed through the I/O APIC instead). */
void pic_disable(void);

#endif /* AQUA_PIC_H */
