/* SMP bring-up + a parallel framebuffer present.
 *
 * Stage 1: detect CPUs (ACPI) + enable the BSP's LAPIC.
 * Stage 2: INIT-SIPI each AP into long mode on the shared kernel address space.
 * Stage 3: APs idle on hlt and wake via a work IPI (vector 240) to copy a
 *          disjoint band of the back buffer to the framebuffer. The full-screen
 *          present (the costly part of every frame) now runs on all cores.
 *          This is safe without locks: the source (back buffer) is read-only and
 *          each CPU writes a disjoint set of rows. No scheduler/kernel changes,
 *          so the rest of the system is exactly as before. */
#include <stdint.h>
#include <stddef.h>
#include "smp.h"
#include "acpi.h"
#include "lapic.h"
#include "ioapic.h"
#include "idt.h"
#include "pic.h"
#include "vmm.h"
#include "fb.h"
#include "e1000.h"
#include "kprintf.h"
#include "percpu.h"
#include "sched.h"
#include "spinlock.h"

void *memcpy(void *, const void *, size_t);
void *kmalloc(unsigned long);

extern uint8_t ap_tramp_start[], ap_tramp_end[];

#define TRAMP_PHYS 0x8000
#define AP_ARGS    ((volatile uint64_t *)0x8F00)   /* cr3 @0, stack @1, entry @2 */
#define AP_STACK   (64 * 1024)
#define MAXCPU     ACPI_MAX_CPUS

/* LAPIC periodic-timer reload for AP preemption. Uncalibrated (div-by-16); tuned
 * empirically under -smp 4 TCG so preemption is visible without thrashing. */
/* Periodic LAPIC-timer reload for AP preemption (div-by-16). Uncalibrated; tuned
 * empirically under -smp 4 TCG: ~100k gives a visible preemption tick (~tens of
 * Hz per AP) that spreads runnable threads across cores without thrashing. The
 * timer also wakes a parked AP from hlt to re-check g_sched_ready. */
#define LAPIC_AP_TIMER_COUNT 100000

/* APs park here until the BSP's sched_init() has built the global run queue
 * (wm_run runs after smp_init returns). */
static volatile int g_sched_ready = 0;
void smp_mark_sched_ready(void) { __atomic_store_n(&g_sched_ready, 1, __ATOMIC_SEQ_CST); }

static uint8_t  cpu_apicid[MAXCPU];     /* index -> APIC id; index 0 = BSP */
static volatile int g_online = 1;       /* CPUs online (incl. BSP) */
static volatile int ap_ack;             /* bringup handshake */
static volatile int g_via_apic;         /* device IRQs go through the I/O APIC */

int smp_irq_via_apic(void) { return g_via_apic; }

int smp_cpu_count(void) { return g_online; }

/* P0: the vector-240 present-IPI is retired (APs now run the scheduler, not a
 * present band). Kept as a no-op so the interrupts.c vector-240 path stays valid;
 * no IPI-240 is sent anymore. (P4 restores a scheduler-aware parallel present.) */
void smp_ipi_work(void) { }

/* First C code each AP runs: enable LAPIC, load the shared IDT + its own GDT/TSS,
 * arm a periodic preemption timer, then become a full scheduling core. */
static void ap_entry(void)
{
    lapic_init();                              /* maps MMIO if needed; lapic_id() now valid */
    idt_load();
    /* Resolve this AP's percpu slot by lapic_id. smp_init's SIPI loop already
     * registered g_cpus[slot].lapic_id (percpu_register_id) BEFORE starting us, so
     * this is correct and independent of g_online (which we haven't incremented).
     * (this_index() is unusable here: it bounds its scan by g_online.) */
    uint32_t my_id = lapic_id();
    int idx = -1;
    for (int i = 1; i < PERCPU_MAXCPU; i++)
        if (g_cpus[i].lapic_id == my_id) { idx = i; break; }
    if (idx <= 0 || idx >= PERCPU_MAXCPU) {    /* slot 0 = BSP; not found -> park idle */
        __atomic_add_fetch(&g_online, 1, __ATOMIC_SEQ_CST);
        ap_ack = 1;
        for (;;) __asm__ volatile ("sti; hlt");
    }
    percpu_ap_init(idx, lapic_id());           /* build + load this core's GDT/TSS */
    /* Arm the periodic LAPIC timer NOW (before parking). It both (a) wakes this AP
     * from its park `hlt` so it can re-check g_sched_ready -- nothing else sends
     * the AP an interrupt -- and (b) becomes the preemption tick once scheduling
     * starts. While parked, me->current is NULL: schedule() short-circuits on a
     * NULL current (guarded), so a timer tick during the park is a harmless no-op. */
    lapic_timer_init(32, LAPIC_AP_TIMER_COUNT);
    __atomic_add_fetch(&g_online, 1, __ATOMIC_SEQ_CST);
    kprintf("[smp] CPU %d apic_id=%d online\n", idx, (int)lapic_id());
    ap_ack = 1;

    /* Park until the BSP's sched_init() has built the global ring. The timer above
     * periodically wakes the hlt so this loop re-tests g_sched_ready. */
    while (!__atomic_load_n(&g_sched_ready, __ATOMIC_SEQ_CST))
        __asm__ volatile ("sti; hlt");

    thread_create_idle(idx);                   /* sets g_cpus[idx].idle + .current = this stack */
    spin_lock(&g_bkl);                         /* enter the kernel before first schedule() */
    this_cpu()->in_kernel = 1;
    sched_become_idle();                       /* this AP stack BECOMES the idle thread; never returns */
}

void smp_init(void)
{
    int n = acpi_init();
    lapic_init();
    percpu_register_id(0, lapic_id());     /* BSP's real lapic_id now available */
    if (n < 1) { kprintf("[smp] no CPUs via ACPI; uniprocessor\n"); return; }
    kprintf("[smp] %d CPU(s) detected, BSP apic_id=%d\n", n, (int)lapic_id());

    /* Switch device IRQs from the legacy PIC to the I/O APIC: route the ISA
     * lines (timer/keyboard/mouse) to the BSP and mask the PIC. EOI then goes
     * to the LAPIC (see interrupts.c). */
    ioapic_init();
    if (ioapic_present()) {
        uint8_t bspid = (uint8_t)lapic_id();
        ioapic_route_isa(0,  32, bspid);     /* PIT timer  -> vec 32 */
        ioapic_route_isa(1,  33, bspid);     /* keyboard   -> vec 33 */
        ioapic_route_isa(12, 44, bspid);     /* PS/2 mouse -> vec 44 */
        int nicg = e1000_irq_line();         /* e1000 NIC -> vec 65 */
        /* EDGE-triggered (not level): QEMU's TCG IOAPIC doesn't clear a level
         * RTE's remote-IRR on EOI (LAPIC broadcast or directed 0x40 both fail),
         * so a level PCI line re-fires forever after its first IRQ (~2M/s, 88%
         * CPU). Edge has no remote-IRR: each packet is one falling edge; e1000_irq
         * drains the whole ring per call and net_poll backstops any coalesced miss. */
        if (nicg > 0 && nicg < 24) ioapic_route((uint32_t)nicg, 65, bspid, 0, 1);
        pic_disable();
        g_via_apic = 1;
        kprintf("[ioapic] device IRQs routed via I/O APIC (NIC gsi=%d)\n", nicg);
    }

    memcpy((void *)TRAMP_PHYS, ap_tramp_start, (size_t)(ap_tramp_end - ap_tramp_start));
    AP_ARGS[0] = vmm_kernel_cr3();

    uint32_t bsp = lapic_id();
    cpu_apicid[0] = (uint8_t)bsp;
    for (int i = 0; i < n; i++) {
        uint8_t aid = acpi_cpu_apic_id(i);
        if (aid == bsp) continue;
        uint8_t *stk = kmalloc(AP_STACK);
        if (!stk) continue;
        cpu_apicid[g_online] = aid;        /* claim the next CPU index */
        if (g_online < PERCPU_MAXCPU)      /* register before SIPI so ap_entry finds its slot */
            percpu_register_id(g_online, aid);
        AP_ARGS[1] = (uint64_t)(stk + AP_STACK) & ~(uint64_t)0xF;
        AP_ARGS[2] = (uint64_t)ap_entry;
        ap_ack = 0;
        lapic_start_ap(aid, TRAMP_PHYS >> 12);
        for (volatile long w = 0; !ap_ack && w < 200000000L; w++) __asm__ volatile ("pause");
        if (!ap_ack) { cpu_apicid[g_online] = 0; kprintf("[smp] CPU apic_id=%d did not start\n", (int)aid); }
    }
    kprintf("[smp] %d/%d CPUs online\n", g_online, n);
    /* P0: present reverted to BSP-only (fb keeps g_par_present NULL). The APs are
     * now scheduling cores, not idle present-helpers. Parallel present returns in
     * P4 as a scheduler-aware job. */
}
