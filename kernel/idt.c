#include <stdint.h>
#include "idt.h"

#define IDT_ENTRIES 48
#define KERNEL_CS   0x08          /* 64-bit code selector from the boot GDT */
#define GATE_INT64  0x8E          /* present, DPL0, 64-bit interrupt gate */

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idtp;

/* Filled in by boot/isr.asm: address of each vector's stub. */
extern void *isr_stub_table[];

static void idt_set(int vec, void *handler)
{
    uint64_t addr = (uint64_t)handler;
    idt[vec].offset_low  = addr & 0xFFFF;
    idt[vec].selector    = KERNEL_CS;
    idt[vec].ist         = 0;
    idt[vec].type_attr   = GATE_INT64;
    idt[vec].offset_mid  = (addr >> 16) & 0xFFFF;
    idt[vec].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt[vec].zero        = 0;
}

void idt_init(void)
{
    for (int i = 0; i < IDT_ENTRIES; i++)
        idt_set(i, isr_stub_table[i]);

    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint64_t)&idt;
    __asm__ volatile ("lidt %0" : : "m"(idtp));
}
