#include <stdint.h>
#include "pic.h"
#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

void pic_remap(void)
{
    /* ICW1: begin init, expect ICW4 */
    outb(PIC1_CMD, 0x11);
    outb(PIC2_CMD, 0x11);
    /* ICW2: vector offsets (master -> 32, slave -> 40) */
    outb(PIC1_DATA, 0x20);
    outb(PIC2_DATA, 0x28);
    /* ICW3: master has a slave on IRQ2; tell slave its cascade identity */
    outb(PIC1_DATA, 0x04);
    outb(PIC2_DATA, 0x02);
    /* ICW4: 8086 mode */
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);

    /* Masks: unmask IRQ0 (timer) and IRQ1 (keyboard); mask the rest. */
    outb(PIC1_DATA, 0xFC);
    outb(PIC2_DATA, 0xFF);
}

void pic_eoi(int irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}
