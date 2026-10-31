/* Local APIC backend.  Firmware may hand the machine to us in either xAPIC
 * (MMIO) or x2APIC (MSR) mode.  Intel forbids a one-step x2APIC -> xAPIC
 * transition, and enabling x2APIC safely also requires system-wide interrupt
 * remapping, so an ordinary kernel preserves the handed-off mode. */
#include <stdint.h>
#include "lapic.h"
#include "apic_model.h"
#include "acpi.h"
#include "kprintf.h"
#ifndef LOGIT_LAPIC_HOST
#include "vmm.h"
#include "ktime.h"
#endif

#define IA32_APIC_BASE  0x01bu
#define APIC_BASE_EXTD  (1ull << 10)

#define LAPIC_ID        0x020
#define LAPIC_EOI       0x0B0
#define LAPIC_SVR       0x0F0
#define LAPIC_ICRLO     0x300
#define LAPIC_ICRHI     0x310
#define LAPIC_LVT_TMR   0x320
#define LAPIC_TMRINIT   0x380
#define LAPIC_TMRDIV    0x3E0
#define LAPIC_TMRCUR    0x390

#define X2APIC_MSR(reg) (0x800u + ((reg) >> 4))
#define X2APIC_ICR_MSR  0x830u
#ifndef LAPIC_IPI_WAIT_SPINS
#define LAPIC_IPI_WAIT_SPINS 100000000L
#endif
#ifndef LAPIC_DELAY_STALL_SPINS
#define LAPIC_DELAY_STALL_SPINS 100000000L
#endif

static volatile uint8_t *lapic;              /* xAPIC MMIO only */
static enum apic_access_mode system_mode;
static int announced;

static inline void lapic_relax(void)
{
#ifdef LOGIT_LAPIC_HOST
    __asm__ volatile ("" ::: "memory");
#else
    __asm__ volatile ("pause");
#endif
}

#ifdef LOGIT_LAPIC_HOST
void lapic_host_cpuid1(uint32_t *ecx, uint32_t *edx);
int lapic_host_rdmsr(uint32_t msr, uint64_t *value);
void lapic_host_wrmsr(uint32_t msr, uint64_t value);
volatile uint8_t *lapic_host_map(uint64_t phys);
uint32_t lapic_host_mmio_read(uint32_t reg);
void lapic_host_mmio_write(uint32_t reg, uint32_t value);
static void arch_cpuid1(uint32_t *ecx, uint32_t *edx)
{
    lapic_host_cpuid1(ecx, edx);
}
static int arch_rdmsr(uint32_t msr, uint64_t *value)
{
    return lapic_host_rdmsr(msr, value);
}
static void arch_wrmsr(uint32_t msr, uint64_t value)
{
    lapic_host_wrmsr(msr, value);
}
void lapic_host_reset(void)
{
    lapic = 0;
    system_mode = APIC_ACCESS_NONE;
    announced = 0;
}
#else
static void arch_cpuid1(uint32_t *ecx, uint32_t *edx)
{
    uint32_t a, b;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(*ecx), "=d"(*edx)
                              : "a"(1), "c"(0));
}
static int arch_rdmsr(uint32_t msr, uint64_t *value)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    *value = ((uint64_t)hi << 32) | lo;
    return 0;
}
static void arch_wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"((uint32_t)value),
                                  "d"((uint32_t)(value >> 32)) : "memory");
}
#endif

static uint32_t reg_read(uint32_t reg)
{
    if (system_mode == APIC_ACCESS_X2APIC) {
        uint64_t value = 0;
        (void)arch_rdmsr(X2APIC_MSR(reg), &value);
        return (uint32_t)value;
    }
#ifdef LOGIT_LAPIC_HOST
    return lapic_host_mmio_read(reg);
#else
    return *(volatile uint32_t *)(lapic + reg);
#endif
}

static void reg_write(uint32_t reg, uint32_t value)
{
    if (system_mode == APIC_ACCESS_X2APIC) {
        arch_wrmsr(X2APIC_MSR(reg), value);
        return;
    }
#ifdef LOGIT_LAPIC_HOST
    lapic_host_mmio_write(reg, value);
#else
    *(volatile uint32_t *)(lapic + reg) = value;
#endif
}

#ifdef LOGIT_X2APIC_NEGCTL_SHORT_DELAY
#define INIT_TO_SIPI_NS 200000ull
#else
#define INIT_TO_SIPI_NS (10ull * 1000000ull)
#endif
#define SIPI_TO_SIPI_NS (200ull * 1000ull)

/* Intel's MP initialization intervals are wall-time minima, not instruction
 * counts.  PAUSE duration differs across Sandy/Ivy Bridge-EP and TCG, so the
 * production path uses the already-calibrated monotonic clock. */
static int ap_delay(uint64_t delay_ns)
{
#ifdef LOGIT_LAPIC_HOST
    extern int lapic_host_delay_ns(uint64_t ns);
    return lapic_host_delay_ns(delay_ns);
#else
    uint64_t start = time_mono_raw_ns();
    if (!start) {
        kprintf("[lapic] AP startup timing unavailable; refusing SIPI\n");
        return -1;
    }
    uint64_t last = start;
    long stalled = 0;
    while (last - start < delay_ns) {
        uint64_t now = time_mono_raw_ns();
        if (now == last) {
            if (++stalled >= LAPIC_DELAY_STALL_SPINS) {
                kprintf("[lapic] monotonic clock stalled during AP startup\n");
                return -1;
            }
        } else {
            last = now;
            stalled = 0;
        }
        lapic_relax();
    }
    return 0;
#endif
}

int lapic_init(void)
{
    uint32_t ecx = 0, edx = 0;
    uint64_t apic_base = 0;
    arch_cpuid1(&ecx, &edx);
    /* RDMSR itself would #UD/#GP without CPUID.01H:EDX.MSR.  The pure mode
     * decoder checks the same bit, but it cannot protect the read. */
    if (!(edx & (1u << 5)) || arch_rdmsr(IA32_APIC_BASE, &apic_base) != 0) {
        kprintf("[lapic] unavailable: IA32_APIC_BASE cannot be read\n");
        return -1;
    }

#ifdef LOGIT_X2APIC_QEMU_TEST
    /* Test-only transition used to execute the MSR backend under QEMU.  It is
     * deliberately absent from ordinary images: Intel requires interrupt
     * remapping before an OS enables x2APIC for device delivery. */
    if (!(apic_base & APIC_BASE_EXTD) && (ecx & (1u << 21))) {
        arch_wrmsr(IA32_APIC_BASE, apic_base | APIC_BASE_EXTD);
        (void)arch_rdmsr(IA32_APIC_BASE, &apic_base);
    }
#endif

    enum apic_access_mode local = apic_model_access_mode(ecx, edx, apic_base);
    if (local == APIC_ACCESS_NONE) {
        kprintf("[lapic] unavailable: disabled or inconsistent APIC mode\n");
        return -1;
    }
    if (system_mode != APIC_ACCESS_NONE && system_mode != local) {
        kprintf("[lapic] refusing mixed per-CPU APIC modes system=%s local=%s\n",
                lapic_mode_name(), local == APIC_ACCESS_X2APIC ? "x2apic" : "xapic");
        return -1;
    }
    system_mode = local;

    if (local == APIC_ACCESS_XAPIC && !lapic) {
        uint64_t base = apic_base & 0x000ffffffffff000ull;
        if (!base) return -1;
#ifdef LOGIT_LAPIC_HOST
        lapic = lapic_host_map(base);
        if (!lapic) return -1;
#else
        vmm_map_page(base, base, VMM_WRITABLE | VMM_NOCACHE);
        lapic = (volatile uint8_t *)(uintptr_t)base;
#endif
    }
    reg_write(LAPIC_SVR, 0x100 | 0xFF); /* software enable, spurious vec FF */
    reg_write(LAPIC_EOI, 0);            /* x2APIC EOI permits only zero */
    if (!announced) {
        announced = 1;
        kprintf("[lapic] mode=%s id=%u source=IA32_APIC_BASE%s\n",
                lapic_mode_name(), (unsigned)lapic_id(),
#ifdef LOGIT_X2APIC_QEMU_TEST
                " TEST-ONLY-force"
#else
                ""
#endif
        );
    }
    return 0;
}

/* Virtual-wire mode for the boot CPU (Intel MP spec 3.6.2.2; what Linux sets
 * up on the BSP until the I/O APIC takes over): LINT0 delivers the 8259's
 * output as ExtINT, LINT1 is NMI, and the task priority admits every vector.
 * QEMU's firmware leaves LINT0 like this; the X79-VG4's AMI UEFI does not,
 * and its first boots took no PIT interrupt at all ("simd selftest ... 0
 * ticks", 2026-10-31). Not verified on X79-VG4 hardware. */
#define LAPIC_TPR_REG   0x080
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
void lapic_virtual_wire(void)
{
    if (lapic_init() != 0) return;
    uint32_t l0 = reg_read(LAPIC_LVT_LINT0), l1 = reg_read(LAPIC_LVT_LINT1), tpr = reg_read(LAPIC_TPR_REG);
#ifndef LAPIC_NEGCTL_NO_VIRTUAL_WIRE
    reg_write(LAPIC_TPR_REG, 0);
    reg_write(LAPIC_LVT_LINT0, 0x700);       /* ExtINT, edge, unmasked */
    reg_write(LAPIC_LVT_LINT1, 0x400);       /* NMI */
#endif
    kprintf("[lapic] virtual wire: LINT0 %x -> %x, LINT1 %x -> %x, TPR %x -> %x\n",
            l0, reg_read(LAPIC_LVT_LINT0), l1, reg_read(LAPIC_LVT_LINT1), tpr, reg_read(LAPIC_TPR_REG));
}

uint32_t lapic_id(void)
{
    if (!lapic_ready()) return UINT32_MAX;
    uint32_t id = reg_read(LAPIC_ID);
    return system_mode == APIC_ACCESS_X2APIC ? id : id >> 24;
}

void lapic_eoi(void)
{
    if (lapic_ready()) reg_write(LAPIC_EOI, 0);
}

int lapic_ready(void)
{
    return system_mode == APIC_ACCESS_X2APIC ||
           (system_mode == APIC_ACCESS_XAPIC && lapic != 0);
}

int lapic_x2apic_active(void) { return system_mode == APIC_ACCESS_X2APIC; }

const char *lapic_mode_name(void)
{
    return system_mode == APIC_ACCESS_X2APIC ? "x2apic" :
           system_mode == APIC_ACCESS_XAPIC ? "xapic" : "none";
}

int lapic_ipi_destination_supported(uint32_t apic_id)
{
    uint64_t value;
    return lapic_ready() &&
           apic_model_icr(system_mode, apic_id, 0x000040f0u, &value) == 0;
}

/* Bounded wait on xAPIC delivery status.  x2APIC removes the bit: the one
 * WRMSR to ICR is the complete architectural dispatch operation. */
static int ipi_wait(void)
{
    if (system_mode != APIC_ACCESS_XAPIC) return 0;
    for (volatile long s = 0;
         s < LAPIC_IPI_WAIT_SPINS && (reg_read(LAPIC_ICRLO) & (1u << 12)); s++)
        lapic_relax();
    if (reg_read(LAPIC_ICRLO) & (1u << 12)) {
#ifdef LOGIT_X2APIC_NEGCTL_IGNORE_BUSY
        return 0;
#else
        kprintf("[lapic] ICR delivery-status timeout\n");
        return -1;
#endif
    }
    return 0;
}

static int send_icr(uint32_t apic_id, uint32_t low)
{
    uint64_t value;
    if (!lapic_ready() || apic_model_icr(system_mode, apic_id, low, &value) != 0) {
        kprintf("[lapic] refusing IPI destination apic_id=%u mode=%s\n",
                (unsigned)apic_id, lapic_mode_name());
        return -1;
    }
    if (system_mode == APIC_ACCESS_X2APIC) {
        arch_wrmsr(X2APIC_ICR_MSR, value);
    } else {
        if (ipi_wait() != 0) return -1;
        reg_write(LAPIC_ICRHI, (uint32_t)(value >> 32));
        reg_write(LAPIC_ICRLO, (uint32_t)value);
        if (ipi_wait() != 0) return -1;
    }
    return 0;
}

/* INIT-SIPI-SIPI: standard AP wake sequence. `vec` = trampoline page number. */
int lapic_start_ap(uint32_t apic_id, uint8_t vec)
{
    if (send_icr(apic_id, 0x00004500) != 0) return -1;
    if (ap_delay(INIT_TO_SIPI_NS) != 0) return -1;
    for (int i = 0; i < 2; i++) {
        if (send_icr(apic_id, 0x00004600 | vec) != 0) return -1;
        if (ap_delay(SIPI_TO_SIPI_NS) != 0) return -1;
    }
    return 0;
}

/* Fixed IPI to one CPU (e.g. a TLB shootdown or present-band request). */
int lapic_send_ipi(uint32_t apic_id, uint8_t vec)
{
    if (vec < 0x10 || vec == 0xff) return -1;
    return send_icr(apic_id, 0x00004000 | vec);
}

/* The LAPIC timer's rate after divide-by-16, measured once on the BSP
 * against the calibrated clock. The AP preemption counts used to be fixed
 * numbers picked under QEMU, whose APIC timer runs at 1 GHz: 1.6-5.4 ms
 * there, but 16-54 ms on the X79-VG4's 100 MHz bus clock, so every
 * cross-thread handoff waited tens of milliseconds (2026-10-31). */
static uint64_t g_timer_hz;

#ifndef LOGIT_LAPIC_HOST
static inline uint64_t tsc_now(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}
#endif

/* Measure the divided timer while rejecting SMI-delayed register endpoints.
 * A single old 10 ms sample could double the result when an endpoint stalled;
 * plausibility bounds alone do not detect that error. Model evidence only,
 * not verified on X79-VG4 hardware. */
uint64_t lapic_timer_calibrate(void)
{
    if (g_timer_hz) {
        return g_timer_hz;
    }
#ifdef LOGIT_LAPIC_HOST
    /* Substitute only the clock leaf; the register sequence and estimator are
     * the same code as the kernel, unlike the former host-only return zero. */
    extern uint64_t lapic_host_tsc_now(void);
    extern uint64_t lapic_host_tsc_hz(void);
    uint64_t (*read_tsc)(void) = lapic_host_tsc_now;
    uint64_t tsc_hz = lapic_host_tsc_hz();
#define TIMER_FENCE() __asm__ volatile ("" ::: "memory")
#else
    uint64_t (*read_tsc)(void) = tsc_now;
    uint64_t tsc_hz = time_source_hz(TIMESRC_TSC);
    /* Intel SDM Vol. 2, RDTSC: LFENCE orders earlier loads and prevents later
     * instructions executing before the timestamp. Fence both sides so the
     * APIC access stays inside its bracket even on an out-of-order core. */
#define TIMER_FENCE() __asm__ volatile ("lfence" ::: "memory")
#endif
    /* A 100 Hz PIT makes a nominal 10 ms interval 0-10 ms (round 7).
     * Only a calibrated TSC can establish this measurement's time scale. */
    if (!tsc_hz) {
        kprintf("[lapic] timer unmeasured (no calibrated TSC): AP ticks keep the fixed counts\n");
        return 0;
    }

    /* Intel SDM Vol. 3A section 10.5.4, APIC Timer: divisor encoding 0x3 is
     * divide-by-16; LVT bit 16 masks delivery, bit 17 clear selects one-shot.
     * Vector 0xFF is the existing spurious vector; masking prevents delivery. */
    reg_write(LAPIC_TMRDIV, 0x3);
    reg_write(LAPIC_LVT_TMR, 0xffu | (1u << 16));
    uint64_t samples[5];
    int valid = 0;
    int attempts = 0;
    /* Bring-up policy: 10 ms observation, at most 10 us uncertainty per
     * endpoint (0.2% combined), five valid samples and at most 15 attempts.
     * Three clean samples are the minimum for a useful median. */
    const uint64_t window = tsc_hz / 100;
    const uint64_t max_bracket = tsc_hz / 100000;
    int wanted_samples = 5;
    int minimum_samples = 3;
#ifdef LAPIC_NEGCTL_UNBRACKETED
    wanted_samples = 1;
    minimum_samples = 1;
#endif
    while (valid < wanted_samples && attempts < 15) {
        attempts++;
        TIMER_FENCE();
        uint64_t start_before = read_tsc();
        TIMER_FENCE();
        /* Full 32-bit one-shot span, SDM 10.5.4; no wrap is accepted below. */
        reg_write(LAPIC_TMRINIT, 0xffffffffu);
        TIMER_FENCE();
        uint64_t start_after = read_tsc();
        TIMER_FENCE();

        uint64_t polled;
        unsigned long spins = 0;
        /* Retain the existing 400 M poll ceiling: a stalled TSC must not
         * strand BSP startup. A short interval is rejected, never estimated. */
        do {
            polled = read_tsc();
            lapic_relax();
        } while (polled - start_after < window && ++spins < 400000000ul);

        TIMER_FENCE();
        uint64_t end_before = read_tsc();
        TIMER_FENCE();
        uint32_t current = reg_read(LAPIC_TMRCUR);
        TIMER_FENCE();
        uint64_t end_after = read_tsc();
        TIMER_FENCE();
        /* Stop even rejected samples; leaving a countdown running obscures
         * fallback behavior when APs retain their existing fixed counts. */
        reg_write(LAPIC_TMRINIT, 0);

#ifndef LAPIC_NEGCTL_UNBRACKETED
        if (start_after - start_before > max_bracket ||
            end_after - end_before > max_bracket) {
            continue;
        }
#else
        (void)max_bracket;
#endif
        /* Midpoints bound the access uncertainty. The negative control uses
         * the old inside timestamps, which exclude endpoint SMI time while
         * still including those LAPIC decrements. */
        uint64_t start = start_before + (start_after - start_before) / 2;
        uint64_t end = end_before + (end_after - end_before) / 2;
#ifdef LAPIC_NEGCTL_UNBRACKETED
        start = start_after;
        end = end_before;
#endif
        uint64_t elapsed = end - start;
        uint64_t ticks = 0xffffffffull - current;
        if (polled - start_after < window || !current || !ticks ||
            elapsed < window) {
            continue;
        }
        /* Compute directly against TSC ticks rather than truncating to ns.
         * Split the product to avoid overflow for implausibly long stalls. */
        uint64_t whole = ticks / elapsed;
        uint64_t remainder = ticks % elapsed;
        if (whole > UINT64_MAX / tsc_hz ||
            (remainder && tsc_hz > UINT64_MAX / remainder)) {
            continue;
        }
        uint64_t fraction = remainder * tsc_hz / elapsed;
        uint64_t hz = whole * tsc_hz;
        if (hz > UINT64_MAX - fraction) {
            continue;
        }
        hz += fraction;
        /* Preserve the previous divided-rate sanity range (1 MHz-2 GHz).
         * It is a guard against broken clocks, not proof of a clean sample. */
        if (hz < 1000000ull || hz > 2000000000ull) {
            continue;
        }
        int position = valid;
        while (position > 0 && samples[position - 1] > hz) {
            samples[position] = samples[position - 1];
            position--;
        }
        samples[position] = hz;
        valid++;
    }
#undef TIMER_FENCE
    if (valid < minimum_samples) {
        kprintf("[lapic] timer calibration disturbed (%d/%d samples): AP ticks keep the fixed counts\n",
                valid, attempts);
        return 0;
    }
    g_timer_hz = samples[valid / 2];
    kprintf("[lapic] timer %lu kHz after divide-by-16 (measured against the TSC)\n",
            (unsigned long)(g_timer_hz / 1000));
    return g_timer_hz;
}

uint64_t lapic_timer_hz(void) { return g_timer_hz; }

uint32_t lapic_timer_count_ns(uint64_t ns)
{
    if (!g_timer_hz) return 0;
    uint64_t c = g_timer_hz * ns / 1000000000ull;
    return c < 1 ? 1 : c > 0xffffffffull ? 0xffffffffu : (uint32_t)c;
}

/* Periodic LAPIC timer -> `vec` (reload is rough and not calibrated). */
void lapic_timer_init(uint8_t vec, uint32_t count)
{
    reg_write(LAPIC_TMRDIV, 0x3);               /* divide by 16 */
    reg_write(LAPIC_LVT_TMR, vec | (1u << 17)); /* periodic */
    reg_write(LAPIC_TMRINIT, count);
}
