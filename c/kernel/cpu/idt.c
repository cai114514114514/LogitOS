#include <stdint.h>
#include "idt.h"

#define IDT_ENTRIES 256
#define KERNEL_CS   0x08          /* 64-bit code selector from the boot GDT */
#define GATE_INT64  0x8E          /* present, DPL0, 64-bit interrupt gate */
#define GATE_USER   0xEE          /* present, DPL3, 64-bit interrupt gate (syscall) */
#define SYSCALL_VEC 128

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

/* Filled in by boot/isr.asm. */
extern void *isr_stub_table[];
extern void isr128(void);
extern void isr65(void);
extern void isr240(void);
extern void isr241(void);
extern void isr255(void);

static void idt_set(int vec, void *handler, uint8_t type)
{
    uint64_t addr = (uint64_t)handler;
    idt[vec].offset_low  = addr & 0xFFFF;
    idt[vec].selector    = KERNEL_CS;
    idt[vec].ist         = 0;
    idt[vec].type_attr   = type;
    idt[vec].offset_mid  = (addr >> 16) & 0xFFFF;
    idt[vec].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt[vec].zero        = 0;
}

void idt_init(void)
{
    for (int i = 0; i < 48; i++)
        idt_set(i, isr_stub_table[i], GATE_INT64);

    /* int 0x80 must be reachable from ring 3, so DPL 3. */
    idt_set(SYSCALL_VEC, (void *)isr128, GATE_USER);
    idt_set(65,  (void *)isr65,  GATE_INT64);     /* e1000 NIC */
    idt_set(240, (void *)isr240, GATE_INT64);     /* TLB-shootdown IPI */
    idt_set(241, (void *)isr241, GATE_INT64);     /* parallel-present band IPI */
    idt_set(255, (void *)isr255, GATE_INT64);     /* LAPIC spurious */

    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint64_t)&idt;
    __asm__ volatile ("lidt %0" : : "m"(idtp));
}

/* Load the (shared) IDT on an application processor. */
void idt_load(void)
{
    __asm__ volatile ("lidt %0" : : "m"(idtp));
}
