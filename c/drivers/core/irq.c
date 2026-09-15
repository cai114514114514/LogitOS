/* 2026-09-10 concurrency correction: IRQ entry tracks CPU-local nesting only. A short registry lock pins active callbacks, and free drains those pins before caller-owned arguments can be released. */
/* Dynamic interrupt vectors (0x60..0x7F) for the device model.
 *
 * Two things here are worth explaining, because both look like they should have
 * been done somewhere else:
 *
 * 1. THE STUBS ARE HERE, NOT IN boot/isr.asm. isr.asm's `isr_common` is a
 *    file-local nasm label, not a `global`, so no other object can jump to it;
 *    and isr.asm only emits stubs for the vectors the kernel knew about at the
 *    time it was written. Rather than teach the boot assembly about a device
 *    model, this file emits its own 32 stubs with file-scope inline asm. They
 *    mirror isr_common exactly, including the FXSAVE/FXRSTOR pair -- the kernel
 *    is built with -msse2, so a C handler may clobber XMM of whatever it
 *    interrupted (M15).
 *
 * 2. THE IDT GATES ARE INSTALLED VIA `sidt`, NOT via idt.c. idt.c's setter is
 *    static and its table is private. Reading IDTR gives the same table every
 *    CPU has already loaded (there is one shared IDT), so writing a gate here
 *    is immediately live on every core with no cross-module plumbing.
 *
 * The stubs preserve CPU-local nesting depth. A short registry lock pins the
 * callback and its argument; device handlers run outside that lock. The pin
 * remains through EOI so another CPU cannot recycle a retiring vector while
 * this CPU is still completing its previous interrupt. */
#include <stdint.h>
#include <stddef.h>
#include "irq.h"
#include "interrupts.h"
#include "spinlock.h"
#include "io_lock.h"
#include "percpu.h"
#include "lapic.h"
#include "smp.h"
#include "pic.h"
#include "kprintf.h"

/* --------------------------------------------------------------- stubs -- */
/* 32 stubs, each padded to 16 bytes so stub N is at irq_stub_base + N*16.
 * (A symbol per stub would need .altmacro string pasting; fixed stride is
 * simpler and irq_init() asserts the stride at boot rather than trusting it.) */
#define IRQ_STUB_STRIDE 16

__asm__(
".text\n"
".balign 16\n"
"irq_stub_common:\n"
"    push %rax\n    push %rbx\n    push %rcx\n    push %rdx\n"
"    push %rsi\n    push %rdi\n    push %rbp\n"
"    push %r8\n    push %r9\n    push %r10\n    push %r11\n"
"    push %r12\n   push %r13\n   push %r14\n   push %r15\n"
"    mov %rsp, %rdi\n"
"    sub $528, %rsp\n"
"    and $-16, %rsp\n"
"    mov %rdi, (%rsp)\n"
"    fxsave 16(%rsp)\n"
"    cld\n"
"    call irq_isr_entry\n"
"    fxrstor 16(%rsp)\n"
"    mov (%rsp), %rsp\n"
"    pop %r15\n    pop %r14\n   pop %r13\n   pop %r12\n"
"    pop %r11\n    pop %r10\n   pop %r9\n    pop %r8\n"
"    pop %rbp\n    pop %rdi\n   pop %rsi\n   pop %rdx\n"
"    pop %rcx\n    pop %rbx\n   pop %rax\n"
"    add $16, %rsp\n"
"    iretq\n"
".balign 16\n"
".globl irq_stub_base\n"
"irq_stub_base:\n"
".set _ivec, 0x60\n"
".rept 32\n"
"    .balign 16\n"
"    pushq $0\n"                    /* dummy error code, uniform frame */
"    pushq $_ivec\n"
"    jmp irq_stub_common\n"
"    .set _ivec, _ivec+1\n"
".endr\n"
".balign 16\n"
".globl irq_stub_end\n"
"irq_stub_end:\n"
);

extern uint8_t irq_stub_base[], irq_stub_end[];

/* ----------------------------------------------------------- IDT gates -- */
struct idt_gate {
    uint16_t off_lo, sel;
    uint8_t  ist, type_attr;
    uint16_t off_mid;
    uint32_t off_hi, zero;
} __attribute__((packed));

struct idtr { uint16_t limit; uint64_t base; } __attribute__((packed));

static void install_gate(int vec, const void *handler)
{
    struct idtr p;
    __asm__ volatile ("sidt %0" : "=m"(p));
    if (p.limit < (vec + 1) * 16 - 1) return;

    uint16_t cs;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));

    struct idt_gate *g = (struct idt_gate *)(uintptr_t)p.base + vec;
    uint64_t a = (uint64_t)(uintptr_t)handler;
    g->off_lo    = (uint16_t)(a & 0xFFFF);
    g->sel       = cs;
    g->ist       = 0;
    g->off_mid   = (uint16_t)((a >> 16) & 0xFFFF);
    g->off_hi    = (uint32_t)(a >> 32);
    g->zero      = 0;
    __asm__ volatile ("" ::: "memory");
    g->type_attr = 0x8E;                    /* present, DPL0, 64-bit int gate */
}

/* ------------------------------------------------------------- vectors -- */
struct irq_slot {
    irq_handler_t fn;
    void         *arg;
    const char   *name;
    volatile uint64_t count;
    unsigned active;
    int retiring;
};

static struct irq_slot g_slot[IRQ_NVEC];
static spinlock_t      g_irq_lock = SPINLOCK_INIT;
static int             g_stubs_ok = -1;    /* -1 = not checked yet */

static int stubs_ready(void)
{
    int ready = __atomic_load_n(&g_stubs_ok, __ATOMIC_ACQUIRE);
    if (ready < 0) {
        ready = ((irq_stub_end - irq_stub_base) == IRQ_NVEC * IRQ_STUB_STRIDE);
        __atomic_store_n(&g_stubs_ok, ready, __ATOMIC_RELEASE);
    }
    return ready;
}

int irq_alloc_vector(irq_handler_t fn, void *arg, const char *name)
{
    if (!fn) return -1;
    if (!stubs_ready()) {
        kprintf("[irq] stub stride mismatch (%d bytes for %d stubs) -- no dynamic vectors\n",
                (int)(irq_stub_end - irq_stub_base), IRQ_NVEC);
        return -1;
    }
    uint64_t fl = spin_lock_irqsave(&g_irq_lock);
    int got = -1;
    for (int i = 0; i < IRQ_NVEC; i++) {
        if (g_slot[i].fn || g_slot[i].retiring) continue;
        g_slot[i].fn = fn; g_slot[i].arg = arg;
        g_slot[i].name = name ? name : "?";
        __atomic_store_n(&g_slot[i].count, 0, __ATOMIC_RELAXED);
        got = i;
        break;
    }
    spin_unlock_irqrestore(&g_irq_lock, fl);
    if (got < 0) return -1;

    install_gate(IRQ_VEC_BASE + got, irq_stub_base + got * IRQ_STUB_STRIDE);
    return IRQ_VEC_BASE + got;
}

void irq_free_vector(int vec)
{
    int i = vec - IRQ_VEC_BASE;
    if (i < 0 || i >= IRQ_NVEC) return;
    uint64_t fl = spin_lock_irqsave(&g_irq_lock);
    /* Retiring slots cannot be reused until the last old callback returns.
     * Clearing the function alone would leave a dispatcher using freed arg. */
    if (g_slot[i].retiring) {
        spin_unlock_irqrestore(&g_irq_lock, fl);
        while (__atomic_load_n(&g_slot[i].active, __ATOMIC_ACQUIRE)) io_relax();
        return; /* Only the first retirement may clear/publish this slot. */
    }
    g_slot[i].retiring = 1;
    g_slot[i].fn = NULL;
    spin_unlock_irqrestore(&g_irq_lock, fl);
    while (__atomic_load_n(&g_slot[i].active, __ATOMIC_ACQUIRE)) io_relax();
    fl = spin_lock_irqsave(&g_irq_lock);
    g_slot[i].arg = NULL; g_slot[i].name = NULL; g_slot[i].retiring = 0;
    spin_unlock_irqrestore(&g_irq_lock, fl);
}

uint64_t irq_vector_count(int vec)
{
    int i = vec - IRQ_VEC_BASE;
    return (i >= 0 && i < IRQ_NVEC) ? __atomic_load_n(&g_slot[i].count, __ATOMIC_RELAXED) : 0;
}

const char *irq_vector_name(int vec)
{
    uint64_t fl = spin_lock_irqsave(&g_irq_lock);
    int i = vec - IRQ_VEC_BASE;
    const char *name = (i >= 0 && i < IRQ_NVEC && g_slot[i].name) ? g_slot[i].name : "-";
    spin_unlock_irqrestore(&g_irq_lock, fl);
    return name;
}

/* ------------------------------------------------------------- dispatch -- */
void irq_isr_entry(struct registers *r)
{
    struct cpu *me = this_cpu();
    /* Entry depth is CPU-local context tracking, never mutual exclusion. */
    int depth = me->in_kernel;
    me->in_kernel = depth + 1;

    int i = (int)r->vector - IRQ_VEC_BASE;
    int borrowed = 0;
    if (i >= 0 && i < IRQ_NVEC) {
        uint64_t f = spin_lock_irqsave(&g_irq_lock);
        irq_handler_t fn = g_slot[i].fn;
        void *arg = g_slot[i].arg;
        if (fn) {
            __atomic_fetch_add(&g_slot[i].active, 1, __ATOMIC_RELAXED);
            borrowed = 1;
        }
        __atomic_fetch_add(&g_slot[i].count, 1, __ATOMIC_RELAXED);
        spin_unlock_irqrestore(&g_irq_lock, f);
        /* A callback may acquire device locks or wake tasks. Keep the registry
         * lock out of that graph; the active reference pins its argument. */
        if (fn) fn(arg);
    }

    /* EOI to the LAPIC whenever there is one. Keying off smp_irq_via_apic()
     * would be wrong for MSI: an MSI is delivered straight to the LAPIC even on
     * a machine whose *line* interrupts still go through the 8259. */
    if (lapic_ready()) lapic_eoi();
    else               pic_eoi((int)r->vector - 32);

    __asm__ volatile ("cli");
    this_cpu()->in_kernel = depth;
    /* Keep the vector pinned through EOI: a level INTx owner's final removal
     * must not recycle it while this CPU still completes the old interrupt. */
    if (borrowed) __atomic_fetch_sub(&g_slot[i].active, 1, __ATOMIC_RELEASE);
}
