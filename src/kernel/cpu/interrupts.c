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

void wm_app_exit(void);   /* wm.c: mark the current app dead (window reaped) */

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
    if (r->vector < 32) {
        if (r->cs & 3) {            /* fault came from ring 3: kill the app, keep the kernel alive */
            uint64_t cr2 = 0; __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            kprintf("\n[fault] app exception: %s (vector %d) rip=%p err=%x cr2=%p rsp=%p -- terminating app\n",
                    exception_names[r->vector & 31], (int)r->vector,
                    (void *)r->rip, (unsigned)r->error_code, (void *)cr2, (void *)r->rsp);
            wm_app_exit();          /* mark the app dead so the WM reaps its window + frees the slot */
            thread_exit();          /* marks thread+app dead, frees its stack, switches away */
        }
        panic_exception(r);         /* kernel-mode fault is still fatal */
        return;
    }

    int irq = (int)r->vector - 32;

    if (irq == 0) {
        timer_tick();
        pic_eoi(0);        /* EOI before we possibly switch stacks */
        schedule();        /* preempt: round-robin to the next thread */
        return;
    }
    if (irq == 1)
        keyboard_handle();  /* PS/2 keyboard */
    else if (irq == 12)
        mouse_handle();     /* PS/2 mouse */

    pic_eoi(irq);
}
