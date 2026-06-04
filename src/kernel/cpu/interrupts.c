#include <stdint.h>
#include "interrupts.h"
#include "kprintf.h"
#include "vga.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "mouse.h"
#include "sched.h"
#include "syscall.h"
#include "lapic.h"
#include "smp.h"
#include "e1000.h"
#include "proc.h"
#include "ata.h"
#include "virtio.h"

static const char *const exception_names[32] = {
    "divide-by-zero", "debug", "NMI", "breakpoint",
    "overflow", "bound range", "invalid opcode", "device not available",
    "double fault", "coprocessor overrun", "invalid TSS", "segment not present",
    "stack-segment fault", "general protection", "page fault", "reserved",
    "x87 FP", "alignment check", "machine check", "SIMD FP",
    "virtualization", "control protection", "?", "?",
    "?", "?", "?", "?", "hypervisor", "VMM comm", "security", "?",
};

static void panic_exception(struct registers *r)
{
    vga_set_color(VGA_WHITE, VGA_RED);
    kprintf("\n  *** EXCEPTION: %s (vector %d) ***\n",
            exception_names[r->vector & 31], (int)r->vector);
    kprintf("  error=%x  rip=%p  rflags=%x\n",
            (unsigned)r->error_code, (void *)r->rip, (unsigned)r->rflags);
    for (;;)
        __asm__ volatile ("cli; hlt");
}

void interrupt_handler(struct registers *r)
{
    if (r->vector == 128) {        /* int 0x80 system call */
        syscall_dispatch(r);
        return;
    }
    if (r->vector == 240) {        /* SMP work IPI: do this CPU's framebuffer band */
        smp_ipi_work();
        lapic_eoi();
        return;
    }
    if (r->vector == 65) {         /* e1000 NIC: drain RX into the stack */
        e1000_irq();
        lapic_eoi();
        return;
    }
    if (r->vector == 255)          /* LAPIC spurious: no EOI, just return */
        return;
    if (r->vector < 32) {
        if (r->cs & 3) {            /* fault came from ring 3: kill the app, keep the kernel alive */
            uint64_t cr2 = 0; __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            kprintf("\n[fault] app exception: %s (vector %d) rip=%p err=%x cr2=%p rsp=%p -- terminating app\n",
                    exception_names[r->vector & 31], (int)r->vector,
                    (void *)r->rip, (unsigned)r->error_code, (void *)cr2, (void *)r->rsp);
            proc_exit(139);         /* zombie + close fds + window dead; frees stack, switches away */
        }
        panic_exception(r);         /* kernel-mode fault is still fatal */
        return;
    }

    int irq = (int)r->vector - 32;
    int apic = smp_irq_via_apic();      /* EOI to the LAPIC once IRQs go via I/O APIC */

    if (irq == 0) {
        timer_tick();
        if (apic) lapic_eoi(); else pic_eoi(0);   /* EOI before we possibly switch stacks */
        if (!ata_busy() && !virtio_busy())  /* don't preempt mid block-I/O transfer */
            schedule();    /* preempt: round-robin to the next thread */
        return;
    }
    if (irq == 1)
        keyboard_handle();  /* PS/2 keyboard */
    else if (irq == 12)
        mouse_handle();     /* PS/2 mouse */

    if (apic) lapic_eoi(); else pic_eoi(irq);
}
