#include "cpuidle.h"
#include "hardware.h"

struct idle_policy { unsigned hint; int mwait; uint64_t monitor; };
static struct idle_policy policies[POWER_CPUS] __attribute__((aligned(64)));

void cpuidle_init(void)
{
    unsigned cpu = power_cpu();
    if (cpu >= POWER_CPUS) return;
    struct idle_policy *p = &policies[cpu];
    *p = (struct idle_policy){0};
    uint32_t r[4];
    power_cpuid(0, 0, r);
    if (r[0] < 6 || !power_intel()) return;
    power_cpuid(1, 0, r);
    if (!(r[2] & (1u << 3))) return;
    /* Correction: the original policy checked MWAIT's interrupt-break bit,
     * but that only helps when an interrupt is actually generated. Without
     * ARAT the local APIC timer can stop in deep C-states, silently stranding
     * an AP. There is no timer broadcast here; require CPUID.6:EAX[2] rather
     * than treating a <=200us exit latency as proof of a working wake source.
     * Conservatively use HLT even for shallow firmware hints without ARAT.
     * Intel SDM vol.3A, "APIC Timer" (Always Running APIC Timer):
     * https://cdrdv2.intel.com/v1/dl/getContent/671190#page=438
     * Host cases exercise ARAT present/absent and missing leaf 6; not verified
     * on X79-VG4 hardware. */
    power_cpuid(6, 0, r);
    if (!(r[0] & (1u << 2))) return;
    power_cpuid(5, 0, r);
    if ((r[2] & 3) != 3) return; /* extensions AND interrupt break while IF=0 */
    uint32_t substates = r[3];
    struct power_cstate states[POWER_STATES];
    unsigned count = power_get_cst(cpu, states), deepest = 0;
    if (count > POWER_STATES) return;
    for (unsigned i = 0; i < count; i++) {
        const struct power_cstate *s = &states[i];
        unsigned cstate = ((s->hint >> 4) & 15) + 1;
        /* CPUID encodes eight nibbles, C0..C7; ACPI Type is not the
         * hardware hint. See Linux v6.12 arch/x86/kernel/acpi/cstate.c:124.
         * https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/acpi/cstate.c */
        if (s->latency_us > 200 || s->hint > 255 || cstate > 7 ||
            (s->hint & 15) >= ((substates >> (cstate * 4)) & 15)) continue;
        if (s->type > deepest || (s->type == deepest && s->hint > p->hint)) {
            deepest = s->type; p->hint = s->hint; p->mwait = 1;
        }
    }
}

void cpuidle_enter(void)
{
    unsigned cpu = power_cpu();
    if (cpu < POWER_CPUS && policies[cpu].mwait)
        power_mwait(policies[cpu].hint, &policies[cpu].monitor);
    else power_halt();
}
