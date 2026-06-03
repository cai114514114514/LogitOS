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

    /* Masks: unmask IRQ0 (timer), IRQ1 (keyboard), IRQ2 (cascade) on the
     * master, and IRQ12 (PS/2 mouse) on the slave; mask everything else. */
    outb(PIC1_DATA, 0xF8);      /* 1111 1000 -> IRQ0,1,2 enabled */
    outb(PIC2_DATA, 0xEF);      /* 1110 1111 -> IRQ12 enabled    */
}

void pic_eoi(int irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}
