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
 * The stubs land in interrupt context, take the BKL the same way
 * interrupts.c does (including the nested-entry check that keeps a `sti` window
 * inside an in-progress kernel operation from self-deadlocking the ticket
 * lock), run the handler, and EOI to the LAPIC. */
#include <stdint.h>
#include <stddef.h>
#include "irq.h"
#include "interrupts.h"
#include "spinlock.h"
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
};

static struct irq_slot g_slot[IRQ_NVEC];
static spinlock_t      g_irq_lock = SPINLOCK_INIT;
static int             g_stubs_ok = -1;    /* -1 = not checked yet */

static int stubs_ready(void)
{
    if (g_stubs_ok < 0)
        g_stubs_ok = ((irq_stub_end - irq_stub_base) == IRQ_NVEC * IRQ_STUB_STRIDE);
    return g_stubs_ok;
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
        if (g_slot[i].fn) continue;
        g_slot[i].fn = fn; g_slot[i].arg = arg;
        g_slot[i].name = name ? name : "?";
        g_slot[i].count = 0;
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
    g_slot[i].fn = NULL; g_slot[i].arg = NULL; g_slot[i].name = NULL;
    spin_unlock_irqrestore(&g_irq_lock, fl);
}

uint64_t irq_vector_count(int vec)
{
    int i = vec - IRQ_VEC_BASE;
    return (i >= 0 && i < IRQ_NVEC) ? g_slot[i].count : 0;
}

const char *irq_vector_name(int vec)
{
    int i = vec - IRQ_VEC_BASE;
    return (i >= 0 && i < IRQ_NVEC && g_slot[i].name) ? g_slot[i].name : "-";
}

/* ------------------------------------------------------------- dispatch -- */
void irq_isr_entry(struct registers *r)
{
    struct cpu *me = this_cpu();
    /* Same nested-entry rule as interrupts.c: key off the BKL's true owner, not
     * a separate flag, or a nested IRQ re-acquires a lock this core holds. */
    int nested = (g_bkl_owner == me->index);
    uint64_t bf = 0;
    if (!nested) { bf = spin_lock_irqsave(&g_bkl); me->in_kernel = 1; }

    int i = (int)r->vector - IRQ_VEC_BASE;
    if (i >= 0 && i < IRQ_NVEC) {
        g_slot[i].count++;
        irq_handler_t fn = g_slot[i].fn;
        if (fn) fn(g_slot[i].arg);
    }

    /* EOI to the LAPIC whenever there is one. Keying off smp_irq_via_apic()
     * would be wrong for MSI: an MSI is delivered straight to the LAPIC even on
     * a machine whose *line* interrupts still go through the 8259. */
    if (lapic_ready()) lapic_eoi();
    else               pic_eoi((int)r->vector - 32);

    __asm__ volatile ("cli");
    if (!nested) { this_cpu()->in_kernel = 0; spin_unlock_irqrestore(&g_bkl, bf); }
}
