#include <stdint.h>
#include <stddef.h>
#include "gdt.h"

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

extern void gdt_flush(void *gdt_ptr);
extern void tss_flush(uint16_t selector);
void *memset(void *, int, size_t);          /* lib/string.c */

/* 0 null, 1 kcode, 2 kdata, 3 ucode, 4 udata, 5-6 TSS (system descriptor). */
static struct gdt_entry gdt[7];
static struct gdt_ptr   gdtr;
static struct tss       tss;

static uint8_t syscall_stack[16384] __attribute__((aligned(16)));

static void set_seg(int i, uint8_t access, uint8_t flags)
{
    gdt[i].limit_low   = 0;
    gdt[i].base_low    = 0;
    gdt[i].base_mid    = 0;
    gdt[i].access      = access;
    gdt[i].flags_limit = flags;
    gdt[i].base_high   = 0;
}

static void set_tss_desc(int i, uint64_t base, uint32_t limit)
{
    gdt[i].limit_low   = limit & 0xFFFF;
    gdt[i].base_low    = base & 0xFFFF;
    gdt[i].base_mid    = (base >> 16) & 0xFF;
    gdt[i].access      = 0x89;                       /* present, 64-bit TSS (avail) */
    gdt[i].flags_limit = (limit >> 16) & 0x0F;
    gdt[i].base_high   = (base >> 24) & 0xFF;

    uint32_t *hi = (uint32_t *)&gdt[i + 1];           /* upper 8 bytes of descriptor */
    hi[0] = (uint32_t)(base >> 32);
    hi[1] = 0;
}

void gdt_init(void)
{
    set_seg(0, 0x00, 0x00);                           /* null */
    set_seg(1, 0x9A, 0x20);                           /* kernel code (L=1) */
    set_seg(2, 0x92, 0x00);                           /* kernel data */
    set_seg(3, 0xFA, 0x20);                           /* user code (DPL3, L=1) */
    set_seg(4, 0xF2, 0x00);                           /* user data (DPL3) */

    memset(&tss, 0, sizeof tss);
    tss.rsp0 = (uint64_t)(syscall_stack + sizeof syscall_stack);
    tss.iomap_base = sizeof(struct tss);
    set_tss_desc(5, (uint64_t)&tss, sizeof(struct tss) - 1);

    gdtr.limit = sizeof gdt - 1;
    gdtr.base  = (uint64_t)&gdt;
    gdt_flush(&gdtr);
    tss_flush(SEL_TSS);
}

void tss_set_rsp0(uint64_t rsp0)
{
    tss.rsp0 = rsp0;
}
