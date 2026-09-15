#include "tlb.h"
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
#include "smp_boot_model.h"
#include "smp_topology.h"
#include "acpi.h"
#include "lapic.h"
#include "cpu_platform.h"
#include "ioapic.h"
#include "idt.h"
#include "pic.h"
#include "vmm.h"
#include "fb.h"
#include "netdev.h"
#include "kprintf.h"
#include "percpu.h"
#include "sched.h"
#include "spinlock.h"
#include "prot.h"       /* cpu_prot_report: EFER/CR4 are per-core, so this is too */
#include "ktime.h"

void *memcpy(void *, const void *, size_t);
void *kmalloc(unsigned long);
void  kfree(void *);

extern uint8_t ap_tramp_start[], ap_tramp_end[];

#define TRAMP_PHYS 0x8000
#define AP_ARGS    ((volatile uint64_t *)0x8F00)   /* cr3, stack, entry, claimed slot */
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

static volatile int g_online = 1;       /* CPUs online (incl. BSP) */
static volatile int g_ap_state[MAXCPU]; /* fixed-slot bring-up handshakes */
static volatile int g_via_apic;         /* device IRQs go through the I/O APIC */
static struct smp_topology_record g_topology[MAXCPU];

#ifndef SMP_AP_WAIT_STALL_SPINS
#define SMP_AP_WAIT_STALL_SPINS 100000000L
#endif

static int wait_ap_terminal(int slot)
{
    uint64_t start = time_mono_raw_ns(), last = start;
    long stalled = 0;
    for (;;) {
        int state = __atomic_load_n(&g_ap_state[slot], __ATOMIC_ACQUIRE);
        if (state != SMP_AP_CLAIMED && state != SMP_AP_PUBLISHING) return state;
        uint64_t now = time_mono_raw_ns();
        if (now - start >= 1000000000ull) return state;
        if (now == last) {
            if (++stalled >= SMP_AP_WAIT_STALL_SPINS) return state;
        } else {
            last = now;
            stalled = 0;
        }
        __asm__ volatile ("pause");
    }
}

int smp_irq_via_apic(void) { return g_via_apic; }

int smp_cpu_count(void)
{
    return __atomic_load_n(&g_online, __ATOMIC_ACQUIRE);
}

int smp_cpu_topology_get(int index, struct smp_topology_record *out)
{
    int online = __atomic_load_n(&g_online, __ATOMIC_ACQUIRE);
    if (!out || index < 0 || index >= online || index >= MAXCPU ||
        !g_topology[index].present)
        return -1;
    *out = g_topology[index];
    return 0;
}

void smp_topology_get_summary(struct smp_topology_summary *out)
{
    int online = __atomic_load_n(&g_online, __ATOMIC_ACQUIRE);
    if (online < 0) online = 0;
    if (online > MAXCPU) online = MAXCPU;
    smp_topology_summarize(g_topology, (size_t)online, out);
}

static void capture_topology(int slot, uint32_t apic_id)
{
    struct cpu_platform_core_info core = {0};
    int decoded = cpu_platform_current_core(&core);
    uint8_t source = core.topology.leaf_1f ? SMP_TOPOLOGY_LEAF_1F :
                     core.topology.leaf_b ? SMP_TOPOLOGY_LEAF_B :
                     core.topology.valid ? SMP_TOPOLOGY_LEGACY : SMP_TOPOLOGY_NONE;
    struct smp_topology_sample sample = {
        .valid = decoded == 0 && core.valid && core.topology.valid,
        .hybrid = core.hybrid,
        .native_model_valid = core.native_model_valid,
        .madt_apic_id = apic_id,
        .cpuid_apic_id = core.topology.x2apic_id,
        .package_id = core.topology.package_id,
        .core_id = core.topology.core_id,
        .thread_id = core.topology.thread_id,
        .native_model_id = core.native_model_id,
        .core_class = (uint8_t)core.type,
        .source = source,
    };
    int stored = smp_topology_store(g_topology, MAXCPU, (size_t)slot, &sample);
    if (stored != 0) {
        kprintf("[smp] CPU %d apic_id=%u topology record rejected rc=%d\n",
                slot, (unsigned)apic_id, stored);
        return;
    }
    const struct smp_topology_record *r = &g_topology[slot];
    if (r->apic_mismatch) {
        kprintf("[smp] CPU %d MADT/LAPIC apic_id=%u != CPUID apic_id=%u; topology isolated\n",
                slot, (unsigned)r->madt_apic_id, (unsigned)r->cpuid_apic_id);
        return;
    }
    kprintf("[smp] CPU %d apic_id=%u topology=%s pkg=%u core=%u thread=%u type=%s native-valid=%d native=%x hybrid=%d\n",
            slot, (unsigned)r->madt_apic_id,
            smp_topology_source_name(r->source), (unsigned)r->package_id,
            (unsigned)r->core_id, (unsigned)r->thread_id,
            smp_core_class_name(r->core_class),
            r->native_model_valid,
            r->native_model_valid ? (unsigned)r->native_model_id : 0u,
            r->hybrid);
}

static void report_topology(void)
{
    struct smp_topology_summary s;
    smp_topology_get_summary(&s);
    kprintf("[smp] topology logical=%u cores=%u p=%u/%u e=%u/%u unknown=%u/%u mismatch=%u conflict=%u\n",
            (unsigned)s.logical, (unsigned)s.cores,
            (unsigned)s.performance_logical, (unsigned)s.performance_cores,
            (unsigned)s.efficiency_logical, (unsigned)s.efficiency_cores,
            (unsigned)s.unknown_logical, (unsigned)s.unknown_cores,
            (unsigned)s.apic_mismatches, (unsigned)s.class_conflicts);
    /* Scheduling correctness is keyed only by the dense online slot. P-core,
     * E-core and unknown records all receive an idle thread and timer. Topology
     * is currently diagnostic; capacity-aware placement is future policy. */
    kprintf("[smp] scheduler topology policy=uniform-online-cpus\n");
}

#ifdef LOGIT_RAPTOR_SMP_QEMU_TEST
static volatile uint32_t g_test_ipi_mask;
static volatile uint32_t g_test_timer_mask;
static volatile uint32_t g_test_expected_mask;
static volatile int g_test_timer_reported;

static uint32_t online_mask(int n)
{
    return n >= 32 ? UINT32_MAX : n > 0 ? ((1u << n) - 1u) : 0;
}

void smp_test_ipi_interrupt(void)
{
    int index = this_cpu()->index;
    if (index >= 0 && index < 32)
        __atomic_fetch_or(&g_test_ipi_mask, 1u << index, __ATOMIC_RELEASE);
}

void smp_test_timer_interrupt(void)
{
    int index = this_cpu()->index;
    if (index < 0 || index >= 32) return;
    uint32_t seen = __atomic_fetch_or(&g_test_timer_mask, 1u << index,
                                     __ATOMIC_ACQ_REL) | (1u << index);
    uint32_t expected = __atomic_load_n(&g_test_expected_mask, __ATOMIC_ACQUIRE);
    if (expected && (seen & expected) == expected &&
        __atomic_exchange_n(&g_test_timer_reported, 1, __ATOMIC_ACQ_REL) == 0)
        kprintf("[raptor-smp] timer vector32 all-online mask=%x\n", expected);
}

static void run_test_ipi_probe(void)
{
    int online = __atomic_load_n(&g_online, __ATOMIC_ACQUIRE);
    uint32_t expected = online_mask(online);
    __atomic_store_n(&g_test_expected_mask, expected, __ATOMIC_RELEASE);
    /* The BSP runs this probe before normal interrupt delivery is established,
     * so it cannot honestly self-IPI here.  Seed only its sender bit and state
     * the AP-only proof in the marker consumed by the guest harness. */
    __atomic_store_n(&g_test_ipi_mask, 1u, __ATOMIC_RELEASE);
    int sends_ok = 1;
    for (int i = 1; i < online; i++)
        if (lapic_send_ipi(g_cpus[i].lapic_id, 242) != 0) sends_ok = 0;

    uint64_t start = time_mono_raw_ns();
    long stalls = 0;
    while ((__atomic_load_n(&g_test_ipi_mask, __ATOMIC_ACQUIRE) & expected) != expected &&
           stalls++ < 100000000L) {
        uint64_t now = time_mono_raw_ns();
        if (start && now - start >= 1000000000ull) break;
        __asm__ volatile ("pause");
    }
    uint32_t seen = __atomic_load_n(&g_test_ipi_mask, __ATOMIC_ACQUIRE);
    if (sends_ok && (seen & expected) == expected)
        kprintf("[raptor-smp] fixed IPI APs %d/%d observed; BSP sender\n",
                online - 1, online - 1);
    else
        kprintf("[raptor-smp] fixed IPI FAILED seen=%x expected=%x sends=%d\n",
                seen, expected, sends_ok);
}
#endif

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
    int pending = 0;
    if (__atomic_compare_exchange_n(&g_band_ack[i], &pending, 2, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        fb_copy_rect(g_band[i].x, g_band[i].y, g_band[i].w, g_band[i].h);
        __atomic_store_n(&g_band_ack[i], 1, __ATOMIC_SEQ_CST);
    }
}

static void smp_present_par(int x, int y, int w, int h)
{
    int n = __atomic_load_n(&g_online, __ATOMIC_ACQUIRE);
    if (n > PERCPU_MAXCPU) n = PERCPU_MAXCPU;
    int self = this_cpu()->index;
    if (n <= 1) { fb_copy_rect(x, y, w, h); return; }

    /* Correction (2026-09-10): no BKL to inspect. Framebuffer ownership
     * serialises presenters; per-band CAS below transfers work exactly once
     * and distinguishes an executing copy from a completed one. */
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
        (void)lapic_send_ipi(g_cpus[band_of[i]].lapic_id, 241);

    fb_copy_rect(g_band[self].x, g_band[self].y, g_band[self].w, g_band[self].h);

    /* Bounded wait, then idempotent fallback for any band still un-acked. */
    for (volatile long spin = 0; spin < 500000L; spin++) {
        int done = 1;
        for (int i = 0; i < nb; i++)
            if (__atomic_load_n(&g_band_ack[band_of[i]], __ATOMIC_ACQUIRE) != 1) { done = 0; break; }
        if (done) return;
        __asm__ volatile ("pause");
    }
    for (int i = 0; i < nb; i++) {
        int c = band_of[i];
        int pending = 0;
        if (__atomic_compare_exchange_n(&g_band_ack[c], &pending, 2, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            fb_copy_rect(g_band[c].x, g_band[c].y, g_band[c].w, g_band[c].h);
            __atomic_store_n(&g_band_ack[c], 1, __ATOMIC_RELEASE);
        } else {
            /* A RUNNING reader still owns the backdrop. Do not recycle its
             * band table or framebuffer until it publishes completion. */
            while (__atomic_load_n(&g_band_ack[c], __ATOMIC_ACQUIRE) != 1) {
                tlb_service();
                __asm__ volatile ("pause");
            }
        }
    }
}

/* First C code each AP runs: enable LAPIC, load the shared IDT + its own GDT/TSS,
 * arm a periodic preemption timer, then become a full scheduling core. */
static void ap_entry(void)
{
    int idx = (int)AP_ARGS[3];
    if (idx <= 0 || idx >= PERCPU_MAXCPU ||
        __atomic_load_n(&g_ap_state[idx], __ATOMIC_ACQUIRE) != SMP_AP_CLAIMED) {
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (lapic_init() != 0) {                   /* handed-off APIC mode must match BSP */
        __atomic_store_n(&g_ap_state[idx], SMP_AP_REJECTED, __ATOMIC_RELEASE);
        for (;;) __asm__ volatile ("cli; hlt");
    }
    idt_load();
    /* The BSP put the exact claimed slot in the trampoline mailbox before the
     * SIPI and does not start another AP until this slot reaches a terminal
     * state.  Never derive ownership from the concurrently changing online
     * count: a late AP would otherwise clear or publish somebody else's slot. */
    uint32_t my_id = lapic_id();
    if (g_cpus[idx].lapic_id != my_id) {
        __atomic_store_n(&g_ap_state[idx], SMP_AP_REJECTED, __ATOMIC_RELEASE);
        for (;;) __asm__ volatile ("cli; hlt");
    }
    percpu_ap_init(idx, lapic_id());           /* build + load this core's GDT/TSS */
    capture_topology(idx, my_id);               /* CPUID.1A/1F are per logical CPU */
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
    /* EFER and CR4 are PER-CORE. An AP whose trampoline missed the NXE/SMEP
     * writes would run ring-3 code with no NX and no SMEP while the BSP's boot
     * line claimed both were on, and nothing would ever say so. Reported per
     * core so the claim is made once per core that has to honour it. */
    cpu_prot_report("ap");
    /* ONLINE release-publishes every AP-local write above.  The AP never
     * changes g_online: the BSP owns that dense prefix and commits this slot
     * only after an acquire observation of ONLINE. */
    if (smp_ap_publish_online(&g_ap_state[idx]) != 0)
        for (;;) __asm__ volatile ("cli; hlt");
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
    this_cpu()->in_kernel = 1;
    sched_become_idle();                       /* this AP stack BECOMES the idle thread; never returns */
}

void smp_init(void)
{
    int n = acpi_init();
    if (lapic_init() != 0) {
        kprintf("[smp] no usable local APIC; uniprocessor\n");
        return;
    }
    percpu_register_id(0, lapic_id());     /* BSP's real lapic_id now available */
    capture_topology(0, lapic_id());        /* sample the BSP on the BSP */
    if (n < 1) { kprintf("[smp] no CPUs via ACPI; uniprocessor\n"); return; }
    kprintf("[smp] %d CPU(s) detected, BSP apic_id=%u\n", n, (unsigned)lapic_id());

    /* Switch device IRQs from the legacy PIC to the I/O APIC: route the ISA
     * lines (timer/keyboard/mouse) to the BSP and mask the PIC. EOI then goes
     * to the LAPIC (see interrupts.c). */
    int ioapic_status = ioapic_init();
    if (ioapic_status == IOAPIC_ROUTE_UNSAFE) {
        kprintf("[ioapic] initial mask state unsafe; stopping before PIC fallback\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (ioapic_present()) {
        uint32_t bspid = lapic_id();
        int route = ioapic_route_legacy_set(bspid);
        if (route == IOAPIC_ROUTE_OK) {
            pic_disable();
            g_via_apic = 1;
            kprintf("[ioapic] device IRQs routed via I/O APIC\n");
        } else if (route == IOAPIC_ROUTE_UNSAFE) {
            kprintf("[ioapic] unsafe partial legacy route; stopping before PIC fallback\n");
            for (;;) __asm__ volatile ("cli; hlt");
        } else {
            kprintf("[ioapic] refusing BSP device routes apic_id=%u; PIC retained\n",
                    (unsigned)bspid);
        }
    }
    /* NICs were probed before LAPIC setup. Register them now, on the sole BSP,
     * with the same shared INTx lifecycle USB/storage use. The removed direct
     * vector-65 route overwrote any earlier/later device sharing its GSI. */
    (void)netdev_irq_route();

    memcpy((void *)TRAMP_PHYS, ap_tramp_start, (size_t)(ap_tramp_end - ap_tramp_start));
    AP_ARGS[0] = vmm_kernel_cr3();

    uint32_t bsp = lapic_id();
    g_ap_state[0] = SMP_AP_ONLINE;
    for (int i = 0; i < n; i++) {
        if (g_online >= PERCPU_MAXCPU) break;  /* no percpu slot / g_cpus[] entry beyond this */
        uint32_t aid = acpi_cpu_apic_id(i);
        if (aid == UINT32_MAX) continue;
        if (aid == bsp) continue;
        if (!lapic_ipi_destination_supported(aid)) {
            kprintf("[smp] CPU apic_id=%u not addressable in %s mode\n",
                    (unsigned)aid, lapic_mode_name());
            continue;
        }
        int slot = __atomic_load_n(&g_online, __ATOMIC_ACQUIRE);
        if (slot <= 0 || slot >= PERCPU_MAXCPU) break;
        uint8_t *stk = kmalloc(AP_STACK);
        if (!stk) continue;
        percpu_register_id(slot, aid);          /* publish ID before claimed state */
        AP_ARGS[1] = (uint64_t)(stk + AP_STACK) & ~(uint64_t)0xF;
        AP_ARGS[2] = (uint64_t)ap_entry;
        AP_ARGS[3] = (uint64_t)slot;
        __atomic_store_n(&g_ap_state[slot], SMP_AP_CLAIMED, __ATOMIC_RELEASE);
        int start_rc = lapic_start_ap(aid, TRAMP_PHYS >> 12);
        int observed = start_rc == 0 ? wait_ap_terminal(slot) :
                       __atomic_load_n(&g_ap_state[slot], __ATOMIC_ACQUIRE);
        int state = observed;
        if (state != SMP_AP_ONLINE) {
            /* A timeout or a failed startup call is only a snapshot: the AP
             * may have completed after it.  CAS cancellation decides the race.
             * If ONLINE won, continue with normal validation and admission;
             * if REJECTED won, a late AP's final publish CAS must fail. */
            state = smp_bsp_cancel_unpublished(&g_ap_state[slot]);
        }
        if (state != SMP_AP_ONLINE) {
            if (smp_ap_stack_may_free((enum smp_ap_boot_state)state, 1))
                kfree(stk);
            if (start_rc != 0)
                kprintf("[smp] CPU apic_id=%u startup failed; stack quarantined\n",
                        (unsigned)aid);
            else if (observed == SMP_AP_REJECTED)
                kprintf("[smp] CPU apic_id=%u rejected initialization or APIC identity; stack quarantined\n",
                        (unsigned)aid);
            else
                kprintf("[smp] CPU apic_id=%u late/timeout cancelled; stack quarantined\n",
                        (unsigned)aid);
            /* AP_ARGS and the claimed slot remain immutable.  Starting another
             * AP here could let the late CPU consume a different stack/slot. */
            break;
        }
        if (g_cpus[slot].lapic_id != aid) {
            kprintf("[smp] fatal published CPU slot=%d changed APIC identity %u/%u\n",
                    slot, (unsigned)g_cpus[slot].lapic_id, (unsigned)aid);
            for (;;) __asm__ volatile ("cli; hlt");
        }
        if (smp_bsp_commit_online(&g_ap_state[slot], &g_online, slot) != 0) {
            kprintf("[smp] fatal cannot commit published CPU slot=%d count=%d\n",
                    slot, __atomic_load_n(&g_online, __ATOMIC_ACQUIRE));
            for (;;) __asm__ volatile ("cli; hlt");
        }
        kprintf("[smp] CPU %d apic_id=%u online\n", slot, (unsigned)aid);
    }
    kprintf("[smp] %d/%d CPUs online\n", g_online, n);
    report_topology();
#ifdef LOGIT_RAPTOR_SMP_QEMU_TEST
    run_test_ipi_probe();
#endif
    if (g_online > 1) {
        fb_set_present_par(smp_present_par);   /* M25 P4b: band-parallel present */
        kprintf("[smp] parallel present on %d cores (IPI 241)\n", g_online);
    }
}
