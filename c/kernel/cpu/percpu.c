#include <stdint.h>
#include "percpu.h"

extern void gdt_flush(void *gdt_ptr);
extern void tss_flush(uint16_t selector);
void *memset(void *, int, unsigned long);          /* lib/string.c */

struct cpu g_cpus[PERCPU_MAXCPU];

/* One ring-0 syscall stack per core (the TSS rsp0 the CPU loads on a ring3->0
 * trap). Was a single static in gdt.c; now per-CPU. */
static uint8_t syscall_stack[PERCPU_MAXCPU][16384] __attribute__((aligned(16)));

/* Find the caller's struct cpu. We identify the running core by its loaded GDT
 * base: each core ran `lgdt &g_cpus[i].gdtr` in build_gdt(), so `sgdt` returns a
 * GDTR whose base == (uint64_t)&g_cpus[i].gdt -- unique per core and readable in
 * ANY context (no LAPIC MMIO, which proved unreliable to read from the int-0x80
 * syscall path under TCG). Before any per-core GDT is loaded, fall back to BSP.
 *
 * THE INDEX IS ARITHMETIC, NOT A SEARCH. Measured (kbench): the whole function
 * cost 19.5 ns and the `sgdt` inside it cost 3.1 ns -- five sixths of it was a
 * linear scan of up to PERCPU_MAXCPU entries comparing addresses, to recover a
 * number that the address already encodes. The entries are elements of one
 * array, so the index is (base - &g_cpus[0].gdt) / sizeof(struct cpu), and the
 * equality check that the search was doing is kept as a verification of that
 * division rather than as the way the answer is found: an sgdt that returns
 * anything other than one of our per-core GDTs (the AP trampoline's, a boot GDT,
 * a corrupted GDTR) still lands on core 0 exactly as before, instead of
 * indexing g_cpus out of bounds.
 *
 * This runs at least four times on every kernel entry, on every core -- from
 * interrupt_handler, from the BKL's owner tracking, and from the exit path -- so
 * it is one of the very few functions in this kernel whose constant factor is
 * multiplied by every syscall the machine makes. */
struct cpu *this_cpu(void)
{
    struct gdt_ptr gp;
    __asm__ volatile ("sgdt %0" : "=m"(gp));
    uint64_t off = gp.base - (uint64_t)&g_cpus[0].gdt;
    uint64_t i = off / sizeof(struct cpu);
    if (i < PERCPU_MAXCPU && (uint64_t)&g_cpus[i].gdt == gp.base)
        return &g_cpus[i];
    return &g_cpus[0];
}

static void set_seg(struct gdt_entry *g, uint8_t access, uint8_t flags)
{
    g->limit_low   = 0;
    g->base_low    = 0;
    g->base_mid    = 0;
    g->access      = access;
    g->flags_limit = flags;
    g->base_high   = 0;
}

static void set_tss_desc(struct gdt_entry *gdt, int i, uint64_t base, uint32_t limit)
{
    gdt[i].limit_low   = limit & 0xFFFF;
    gdt[i].base_low    = base & 0xFFFF;
    gdt[i].base_mid    = (base >> 16) & 0xFF;
    gdt[i].access      = 0x89;                       /* present, 64-bit TSS (avail) */
    gdt[i].flags_limit = (limit >> 16) & 0x0F;
    gdt[i].base_high   = (base >> 24) & 0xFF;

    uint32_t *hi = (uint32_t *)&gdt[i + 1];          /* upper 8 bytes of descriptor */
    hi[0] = (uint32_t)(base >> 32);
    hi[1] = 0;
}

/* Build this core's GDT + TSS (using its own syscall stack) and load both.
 * Segment slots are byte-identical across cores, so no CS far-jump is needed
 * (the AP trampoline GDT's kcode slot matches). */
static void build_gdt(struct cpu *c)
{
    set_seg(&c->gdt[0], 0x00, 0x00);                 /* null */
    set_seg(&c->gdt[1], 0x9A, 0x20);                 /* kernel code (L=1) */
    set_seg(&c->gdt[2], 0x92, 0x00);                 /* kernel data */
    set_seg(&c->gdt[3], 0xFA, 0x20);                 /* user code (DPL3, L=1) */
    set_seg(&c->gdt[4], 0xF2, 0x00);                 /* user data (DPL3) */

    memset(&c->tss, 0, sizeof c->tss);
    c->tss.rsp0 = (uint64_t)(syscall_stack[c->index] + sizeof syscall_stack[c->index]);
    c->tss.iomap_base = sizeof(struct tss);
    set_tss_desc(c->gdt, 5, (uint64_t)&c->tss, sizeof(struct tss) - 1);

    c->gdtr.limit = sizeof c->gdt - 1;
    c->gdtr.base  = (uint64_t)&c->gdt;
    gdt_flush(&c->gdtr);
    tss_flush(SEL_TSS);
}

void percpu_bsp_init(void)
{
    g_cpus[0].index = 0;
    g_cpus[0].lapic_id = 0;          /* real id filled by percpu_register_id in smp_init */
    build_gdt(&g_cpus[0]);
}

void percpu_ap_init(int index, uint32_t lapic_id)
{
    g_cpus[index].index = index;
    g_cpus[index].lapic_id = lapic_id;
    build_gdt(&g_cpus[index]);
}

void percpu_register_id(int index, uint32_t lapic_id)
{
    g_cpus[index].lapic_id = lapic_id;
}

void percpu_tss_set_rsp0(uint64_t rsp0)
{
    this_cpu()->tss.rsp0 = rsp0;
}

/* kheap.c's per-core magazines ask which core they are on through this, rather
 * than including percpu.h -- that file is host-compiled by make test-kheap. */
int kheap_cpu_index(void);
int kheap_cpu_index(void) { return this_cpu()->index; }
