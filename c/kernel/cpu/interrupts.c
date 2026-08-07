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
#include "tlb.h"
#include "lapic.h"
#include "smp.h"
#include "e1000.h"
#include "proc.h"
#include "ata.h"
#include "virtio.h"
#include "nvme.h"
#include "percpu.h"
#include "spinlock.h"
#include "mm.h"        /* mm_fault(): the page faults that are work, not errors */
#include "work.h"        /* M27: softirq_run_pending() on the kernel-exit path */
#include "kbench.h"      /* entry/exit accounting, off by default */

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
    uint64_t cr2 = 0; __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    struct cpu *me = this_cpu();
    vga_set_color(VGA_WHITE, VGA_RED);
    kprintf("\n  *** EXCEPTION: %s (vector %d) ***\n",
            exception_names[r->vector & 31], (int)r->vector);
    kprintf("  error=%x  rip=%p  rflags=%x cr2=%p cpu=%d cur=%p\n",
            (unsigned)r->error_code, (void *)r->rip, (unsigned)r->rflags,
            (void *)cr2, me->index, (void *)me->current);
    for (;;)
        __asm__ volatile ("cli; hlt");
}

void interrupt_handler(struct registers *r)
{
    if (r->vector == 255)          /* LAPIC spurious: no BKL, no EOI, just return */
        return;

    /* ENTRY/EXIT ACCOUNTING (kbench.h). What one trip into the kernel costs,
     * by class, measured on the real workload rather than a synthetic loop --
     * because a synthetic loop cannot reproduce four cores queueing on the BKL,
     * which is most of what a kernel entry costs here. Disabled by default: one
     * load of a global and a predicted-not-taken branch. Deliberately spans the
     * WHOLE handler including the BKL acquire and release, since from ring 3's
     * point of view that queueing IS the syscall. Not counted: entries that
     * never return here (proc_exit -> thread_exit), which is the right
     * direction to lose samples in -- a dying process is not a hot path. */
    uint64_t kb_t0 = g_kb_stat ? kb_rdtsc() : 0;

    /* BKL: acquire on every kernel entry (P0: at most one core in the kernel at a
     * time; ring 3 runs in parallel). `in_kernel` makes the deliberate `sti`
     * windows (execve, SYS_HTTP_GET) safe: a nested IRQ on a core that already
     * holds the BKL must NOT re-acquire (self-deadlock) and must NOT re-enter the
     * scheduler. */
    struct cpu *me = this_cpu();
    /* "Nested" = this core already holds the BKL. Key off the lock's true owner,
     * NOT a separate in_kernel flag: in_kernel has wide windows (it is set/cleared
     * a few instructions away from the actual lock op), and a timer IRQ landing in
     * one of those gaps used to read in_kernel=0 and re-acquire the BKL this core
     * already holds -> the ticket lock self-deadlocks (every core then spins on a
     * `serving` that never advances). g_bkl_owner is updated inside the lock under
     * IF=0, so it has no such gap. */
    int nested = (g_bkl_owner == me->index);
    /* M25 P1: BKL-free syscalls run WITHOUT the BKL (self-locked via fine-grained
     * locks), so multiple cores execute them in parallel. They are still safely
     * preemptible: while holding a fine-grained lock IF=0 (irqsave) blocks the
     * timer; between locks they hold nothing, so a timer preempt is as safe as
     * preempting ring 3. Only the syscall vector can be bkl-free; IRQs/faults
     * always take the BKL. */
    /* vectors 240 (TLB shootdown) / 241 (parallel-present band): MUST be BKL-free
     * -- the initiator may hold the BKL while waiting for this core to ack, so
     * taking the BKL here would deadlock. Each handler touches only its own
     * published work item + an ack word. */
    int bkl_free = ((r->vector == 128) && syscall_is_bkl_free((int)r->rax))
                   || r->vector == 240 || r->vector == 241;
    uint64_t bf = 0;
    /* BSP wall-clock tick MUST NOT wait for the BKL: the network stack's
     * timeouts/retransmits are driven by timer_ticks(), and a thread holding
     * the BKL in a blocking fetch may itself be waiting for those timeouts.
     * If the tick queued behind the BKL, ticks would freeze, the fetch would
     * never time out, and the BKL would never be released -> circular wait,
     * whole-machine freeze. timer_ticks is a single-writer (BSP timer IRQ
     * only) volatile counter and its readers are lock-free, so ticking here
     * is safe. */
    if (r->vector == 32 && me->index == 0) {
        timer_tick();
        /* M27: expire timed sleeps in the SAME pre-BKL window, for the same
         * reason. A thread parked with a deadline is made runnable here;
         * sched_timer_expire() takes only g_sched_lock and never the BKL, so a
         * deadline can never queue behind a BKL held by a thread that is itself
         * waiting for that deadline -- the identical circular wait that made
         * timer_tick() move ahead of the acquire. */
        sched_timer_expire();
    }
    if (!nested && !bkl_free) { bf = spin_lock_irqsave(&g_bkl); me->in_kernel = 1; }

    if (r->vector == 128) {        /* int 0x80 system call */
        syscall_dispatch(r);
        goto done;
    }
    if (r->vector == 240) {        /* M25 P2: TLB-shootdown IPI (BKL-free) */
        tlb_ipi();
        lapic_eoi();
        goto done;
    }
    if (r->vector == 241) {        /* M25 P4b: parallel-present band IPI (BKL-free) */
        smp_present_ipi();
        lapic_eoi();
        goto done;
    }
    if (r->vector == 65) {         /* e1000 NIC: drain RX into the stack */
        e1000_irq();
        lapic_eoi();
        goto done;
    }
    if (r->vector < 32) {
        /* A page fault may be WORK rather than an error: a copy-on-write page
         * being written, or an anonymous mapping touched for the first time.
         * mm_fault() claims those and returns non-zero, and we retry the
         * instruction; it declines everything else and returns 0, so the two
         * branches below are exactly what they were.
         *
         * This hook is why MM_COW_DEFAULT could not be 1 before now: with no
         * handler, the first write to a shared page after fork looks like a
         * protection violation and kills the process. The memory line wrote
         * the whole copy-on-write path against this call and deliberately did
         * not install it, because a concurrency line was reworking the BKL
         * discipline in this file and a SIMD line was considering an
         * FXSAVE->XSAVE migration in the trap prologue at the same time. Both
         * have landed, so it goes in here rather than being self-installed
         * behind their backs. */
        if (r->vector == 14 && mm_fault(mm_cr2(), r->error_code, r->rip))
            goto done;

        if (r->cs & 3) {            /* fault came from ring 3: kill the app, keep the kernel alive */
            uint64_t cr2 = 0; __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            kprintf("\n[fault] app exception: %s (vector %d) rip=%p err=%x cr2=%p rsp=%p -- terminating app\n",
                    exception_names[r->vector & 31], (int)r->vector,
                    (void *)r->rip, (unsigned)r->error_code, (void *)cr2, (void *)r->rsp);
            proc_exit(139);         /* -> thread_exit -> context_switch; DOES NOT RETURN.
                                     * The BKL is handed to the incoming thread; do NOT
                                     * fall through to done: (no double release). */
        }
        panic_exception(r);         /* kernel-mode fault is still fatal (loops, no return) */
    }

    {
        int irq = (int)r->vector - 32;
        int apic = smp_irq_via_apic();      /* EOI to the LAPIC once IRQs go via I/O APIC */

        if (irq == 0) {
            /* tick already done above (before the BKL acquire -- see comment) */
            if (apic) lapic_eoi(); else pic_eoi(0);
            /* Don't preempt mid block-I/O, and never re-enter the scheduler from
             * a NESTED IRQ (the sti window inside an in-progress kernel op). */
            if (!nested && !ata_busy() && !virtio_busy() && !nvme_busy())
                schedule();    /* preempt: round-robin to the next thread */
            goto done;
        }
        if (irq == 1)
            keyboard_handle();  /* PS/2 keyboard */
        else if (irq == 12)
            mouse_handle();     /* PS/2 mouse */

        if (apic) lapic_eoi(); else pic_eoi(irq);
    }

done:
    /* M27 deferred work runs HERE: the tail of the interrupt that raised it,
     * still holding the BKL, before the exit window below. This is the piece the
     * kernel never had -- a handler can now say "finish this later" instead of
     * doing everything inside the IRQ (e1000_irq drains the whole RX ring in
     * interrupt context) or leaving it for the WM loop to notice (net_poll).
     *
     * Conditions: only when THIS entry owns the BKL (a nested IRQ's outer frame
     * will run them, and a BKL-free vector must not touch shared state), and not
     * inside a device's non-preemptible poll window -- the same guard the timer
     * preempt uses, for the same reason. Softirq handlers must not sleep: there
     * is a live interrupt frame under them. */
    if (!nested && !bkl_free && !ata_busy() && !virtio_busy() && !nvme_busy())
        softirq_run_pending();

    /* schedule() (called above for timer preemption, or inside a blocking syscall)
     * may have switched this thread out and later RESUMED it on a DIFFERENT core,
     * so `me` (captured at entry) is now stale. Clear in_kernel on the core we
     * actually resumed on -- using the stale `me` would clobber another core's
     * in_kernel and leave THIS core's stuck at 1, so its next entry would read
     * nested=1 and skip the BKL acquire (two cores in the kernel at once). The BKL
     * itself is global, so releasing it here (on whatever core) is correct. */
    /* cli before the in_kernel=0 .. BKL-release window: a syscall body may have
     * left IF=1 (tty_read's sti;hlt, SYS_HTTP_GET, the virtio present poll), and a
     * nested IRQ landing in that gap would read nested=0 and re-acquire the BKL we
     * still hold -> self-deadlock. bf carries IF=0 (entry was via an int gate) so
     * irqrestore won't re-enable it here; the final iretq restores the caller's IF. */
    __asm__ volatile ("cli");
    if (!nested && !bkl_free) { this_cpu()->in_kernel = 0; spin_unlock_irqrestore(&g_bkl, bf); }

    if (__builtin_expect(kb_t0 != 0, 0)) {
        int cls = (r->vector == 128) ? KB_C_SYSCALL
                : (r->vector == 32)  ? KB_C_TIMER
                : (r->vector < 32)   ? KB_C_FAULT
                : (r->vector == 240 || r->vector == 241) ? KB_C_IPI
                : KB_C_IRQ;
        /* this_cpu() and not the entry-time `me`: schedule() may have resumed
         * this thread on a different core, and charging another core's counters
         * would put the cost where the work did not happen. */
        int idx = this_cpu()->index;
        if (idx >= 0 && idx < KB_MAXCPU) {
            g_kb[idx].n[cls]++;
            g_kb[idx].cyc[cls] += kb_rdtsc() - kb_t0;
        }
    }
}
