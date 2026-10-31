#include "pstate.h"
#include "hardware.h"
#include "kprintf.h"

struct ratio_policy {
    struct power_pss states[POWER_STATES];
    unsigned count, minimum, maximum, turbo, current;
    unsigned pending;
    uint64_t changed_ns;
    int enabled, changed;
};
static struct ratio_policy policies[POWER_CPUS];
static void apply_ratio(struct ratio_policy *p, unsigned ratio);

int pstate_init(void)
{
    unsigned cpu = power_cpu();
    if (cpu >= POWER_CPUS) return -19;
    struct ratio_policy *p = &policies[cpu];
    *p = (struct ratio_policy){0};
    uint32_t r[4]; power_cpuid(1, 0, r);
    if (!power_legacy_intel() || !(r[2] & (1u << 7))) {
        kprintf("[cpu] EIST unavailable on this CPU\n");
        return -19;
    }
    uint64_t misc = power_rdmsr(0x1a0);
    if (!(misc & (1ull << 16))) {
        kprintf("[cpu] EIST disabled in IA32_MISC_ENABLE\n");
        return -19;
    }
    uint64_t platform = power_rdmsr(0xce);
    p->minimum = (platform >> 40) & 255;
    p->maximum = (platform >> 8) & 255;
    if (!p->minimum || p->minimum > p->maximum) return -19;
    p->count = power_get_pss(cpu, p->states);
    if (p->count > POWER_STATES) return -19;
    if (!p->count) {
        kprintf("[cpu] pss missing, derived from MSR_PLATFORM_INFO (%u-%u)\n", p->minimum, p->maximum);
    } else {
        unsigned minimum = 256, maximum = 0;
        for (unsigned i = 0; i < p->count; i++) {
            unsigned ratio = p->states[i].ratio;
            /* _PSS Control is authoritative but only the EIST ratio field is
             * accepted here; legacy voltage-control and non-FFH _PCT require
             * a different driver. Malformed firmware is not a license to guess. */
            if (ratio < p->minimum || ratio > 255 ||
                p->states[i].control != (ratio << 8)) return -19;
            if (ratio < minimum) minimum = ratio;
            if (ratio > maximum) maximum = ratio;
        }
        p->minimum = minimum; p->maximum = maximum;
    }
    uint32_t features[4]; power_cpuid(6, 0, features);
    p->turbo = p->maximum;
    if (!p->count && (features[0] & 2) && !(misc & (1ull << 38))) {
        unsigned turbo = power_rdmsr(0x1ad) & 255;
        if (turbo >= p->maximum) p->turbo = turbo;
    }
    p->enabled = 1;
    /* No load governor is wired yet (pstate_tick has no scheduler metric),
     * so run at the highest ratio this CPU allows: turbo where firmware
     * leaves it enabled, the maximum non-turbo ratio otherwise. */
#ifndef PSTATE_NEGCTL_KEEP_FIRMWARE_RATIO
    apply_ratio(p, p->turbo);
    p->changed = 0;   /* a one-off boot write does not open the 10 ms rate-limit window */
    kprintf("[cpu] cpu%u ratio %u (range %u-%u, turbo %u)\n", cpu, p->current, p->minimum,
            p->maximum, p->turbo);
#endif
    return 0;
}

/* The rate-limit check, WRMSR and timestamp form one critical section: a
 * timer-path pstate_tick() landing between a local setter's check and its
 * write would otherwise write twice inside 10 ms and overwrite the
 * timestamp (gpt-6-astra, T7 review). Interrupts off, local CPU only. */
static void apply_ratio_locked(struct ratio_policy *p, unsigned ratio);
static void apply_ratio(struct ratio_policy *p, unsigned ratio)
{
    uint64_t flags = power_irq_save();
    apply_ratio_locked(p, ratio);
    power_irq_restore(flags);
}

static void apply_ratio_locked(struct ratio_policy *p, unsigned ratio)
{
    if (!p->enabled) return;
    uint64_t now = power_now();
    if (p->changed && now - p->changed_ns < 10000000ull) return;
    if (ratio < p->minimum) ratio = p->minimum;
    if (ratio > p->turbo) ratio = p->turbo;
    if (p->count) {
        /* Round down within the actual firmware table, never synthesize a
         * ratio between two _PSS states. An undershoot selects its minimum. */
        unsigned selected = p->minimum;
        for (unsigned i = 0; i < p->count; i++)
            if (p->states[i].ratio <= ratio && p->states[i].ratio > selected)
                selected = p->states[i].ratio;
        ratio = selected;
    }
    if (p->changed && p->current == ratio) return;
    /* Preserve non-ratio bits. Intel SDM vol.4 IA32_PERF_CTL [15:8]; firmware
     * EIST enable is checked again because SMM may change it after init. */
    if (!(power_rdmsr(0x1a0) & (1ull << 16))) { p->enabled = 0; return; }
    uint64_t control = power_rdmsr(0x199);
    power_wrmsr(0x199, (control & ~0xff00ull) | ((uint64_t)ratio << 8));
    p->current = ratio; p->changed_ns = now; p->changed = 1;
}

void pstate_set_ratio(unsigned cpu, unsigned ratio)
{
    if (cpu >= POWER_CPUS) return;
    if (cpu == power_cpu()) apply_ratio(&policies[cpu], ratio);
    else __atomic_store_n(&policies[cpu].pending, ratio ? ratio : 1, __ATOMIC_RELEASE);
}

void pstate_tick(unsigned load)
{
    unsigned cpu = power_cpu();
    if (cpu >= POWER_CPUS) return;
    struct ratio_policy *p = &policies[cpu];
    if (!p->enabled) return;
    /* Leave a pending request queued until the 10ms boundary, instead of
     * consuming it and silently dropping it when apply_ratio rate-limits. */
    if (p->changed && power_now() - p->changed_ns < 10000000ull) return;
    unsigned ratio = __atomic_exchange_n(&p->pending, 0, __ATOMIC_ACQ_REL);
    if (ratio) apply_ratio(p, ratio);
    else if (load >= 80) apply_ratio(p, p->turbo);
    else if (load <= 20) apply_ratio(p, p->minimum);
}
