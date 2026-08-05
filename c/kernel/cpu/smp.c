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
void  kfree(void *);

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

/* M25 P4b: parallel framebuffer present, restored on vector 241 (240 now belongs
 * to the TLB shootdown). The presenting core splits a tall rect's rows into one
 * band per online core, publishes the bands, IPIs the other cores, copies its own
 * band, and waits for acks WITH A TIMEOUT: a core that can't service the IPI
 * promptly (IF=0 spinning on a lock, or parked in early bring-up) is covered by
 * the presenter copying that band itself -- the copy is idempotent (same source
 * rows to the same destination rows), so a late ack arriving mid-fallback is
 * harmless. The handler is BKL-free (the presenter usually HOLDS the BKL while
 * waiting, so a handler that took it would deadlock) and touches only its
 * published band + the ack word. */
static struct { int x, y, w, h; } g_band[PERCPU_MAXCPU];
static volatile int g_band_ack[PERCPU_MAXCPU];

void smp_present_ipi(void)               /* vector-241 handler body (interrupts.c) */
{
    int i = this_cpu()->index;
    if (!__atomic_load_n(&g_band_ack[i], __ATOMIC_SEQ_CST)) {
        fb_copy_rect(g_band[i].x, g_band[i].y, g_band[i].w, g_band[i].h);
        __atomic_store_n(&g_band_ack[i], 1, __ATOMIC_SEQ_CST);
    }
}

static void smp_present_par(int x, int y, int w, int h)
{
    int n = g_online;
    if (n > PERCPU_MAXCPU) n = PERCPU_MAXCPU;
    int self = this_cpu()->index;
    if (n <= 1) { fb_copy_rect(x, y, w, h); return; }

    /* Contention gate: if anyone is queued on the BKL, present solo. The queued
     * cores spin with IF=0 (irqsave) and cannot service the band IPI until the
     * presenter -- who HOLDS the BKL -- releases it: every parallel attempt would
     * ride the full ack timeout while keeping the BKL, starving the whole system
     * (observed: 3 cores BKL-queued, boot-to-shell fine but smptest crawling).
     * Parallel present thus engages exactly when it helps: big composites while
     * the other cores are idle or in ring 3. */
    unsigned int t = __atomic_load_n(&g_bkl.ticket,  __ATOMIC_SEQ_CST);
    unsigned int s = __atomic_load_n(&g_bkl.serving, __ATOMIC_SEQ_CST);
    if (t - s > 1) { fb_copy_rect(x, y, w, h); return; }

    /* Row bands, top to bottom; the presenter takes band 0 (no IPI to self).
     * Presents are serialized by the BKL, so the band table has one writer. */
    int per = h / n, yy = y;
    int band_of[PERCPU_MAXCPU]; int nb = 0;
    for (int i = 0; i < n; i++) {
        int bh = (i == n - 1) ? (y + h - yy) : per;
        int core = (i == 0) ? self : (i <= self ? i - 1 : i);   /* others fill remaining slots */
        g_band[core].x = x; g_band[core].y = yy; g_band[core].w = w; g_band[core].h = bh;
        g_band_ack[core] = (i == 0);     /* self band needs no ack */
        if (i > 0) band_of[nb++] = core;
        yy += bh;
    }
    __sync_synchronize();
    for (int i = 0; i < nb; i++)
        lapic_send_ipi((uint8_t)g_cpus[band_of[i]].lapic_id, 241);

    fb_copy_rect(g_band[self].x, g_band[self].y, g_band[self].w, g_band[self].h);

    /* Bounded wait, then idempotent fallback for any band still un-acked. */
    for (volatile long spin = 0; spin < 500000L; spin++) {
        int done = 1;
        for (int i = 0; i < nb; i++)
            if (!g_band_ack[band_of[i]]) { done = 0; break; }
        if (done) return;
        __asm__ volatile ("pause");
    }
    for (int i = 0; i < nb; i++) {
        int c = band_of[i];
        if (!__atomic_exchange_n(&g_band_ack[c], 1, __ATOMIC_SEQ_CST))
            fb_copy_rect(g_band[c].x, g_band[c].y, g_band[c].w, g_band[c].h);
    }
}

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
        ap_ack = 1;                            /* do NOT g_online++ here: this CPU has no
                                                * percpu slot / g_cpus[] entry, and counting
                                                * it would overstate smp_cpu_count(). */
        for (;;) __asm__ volatile ("sti; hlt");
    }
    percpu_ap_init(idx, lapic_id());           /* build + load this core's GDT/TSS */
    /* Arm the periodic LAPIC timer NOW (before parking). It both (a) wakes this AP
     * from its park `hlt` so it can re-check g_sched_ready -- nothing else sends
     * the AP an interrupt -- and (b) becomes the preemption tick once scheduling
     * starts. While parked, me->current is NULL: schedule() short-circuits on a
     * NULL current (guarded), so a timer tick during the park is a harmless no-op.
     * The period is STAGGERED per core (+12.5% per index): identical periods armed
     * at near-identical times keep every core's preemption tick in phase forever,
     * so the cores hit the shared scheduler lock in lock-step (a convoy) and
     * sample each other's state at correlated instants (discovered during the
     * M25 P4 per-CPU-runqueue experiment; see the P4 spec doc). Drifted phases
     * decorrelate both. */
    lapic_timer_init(32, LAPIC_AP_TIMER_COUNT + (uint32_t)idx * (LAPIC_AP_TIMER_COUNT / 8));
    __atomic_add_fetch(&g_online, 1, __ATOMIC_SEQ_CST);
    kprintf("[smp] CPU %d apic_id=%d online\n", idx, (int)lapic_id());
    ap_ack = 1;

    /* Park until the BSP's sched_init() has built the global ring. The timer above
     * periodically wakes the hlt so this loop re-tests g_sched_ready. */
    while (!__atomic_load_n(&g_sched_ready, __ATOMIC_SEQ_CST))
        __asm__ volatile ("sti; hlt");

    thread_create_idle(idx);                   /* sets g_cpus[idx].idle + .current = this stack */
    /* We arrive with IF=1 (the park loop's `sti; hlt`) and the LAPIC timer armed:
     * a timer IRQ landing between taking the BKL ticket and g_bkl_owner=me would
     * try to re-acquire the BKL this core holds -> self-deadlock. cli first, per
     * spinlock.c's "bare re-acquire sites cli around themselves" rule. */
    __asm__ volatile ("cli");
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
        if (g_online >= PERCPU_MAXCPU) break;  /* no percpu slot / g_cpus[] entry beyond this */
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
        if (!ap_ack) { kfree(stk); cpu_apicid[g_online] = 0; kprintf("[smp] CPU apic_id=%d did not start\n", (int)aid); }
    }
    kprintf("[smp] %d/%d CPUs online\n", g_online, n);
    if (g_online > 1) {
        fb_set_present_par(smp_present_par);   /* M25 P4b: band-parallel present */
        kprintf("[smp] parallel present on %d cores (IPI 241)\n", g_online);
    }
}
