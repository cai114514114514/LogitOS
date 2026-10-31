#include "thermal.h"
#include "hardware.h"
#include "kprintf.h"

struct power_sample {
    unsigned package, ratio, minimum, maximum, turbo, microcode;
    int core, package_temp, core_valid, package_valid, throttled;
    int valid;
};
static struct power_sample samples[POWER_CPUS];

static int dts_temperature(uint64_t status, unsigned target)
{ return (int)target - (int)((status >> 16) & 127); }

void cpu_power_sample(void)
{
    unsigned cpu = power_cpu();
    if (cpu >= POWER_CPUS) return;
    struct power_sample sample = {0};
    if (!power_legacy_intel()) return;
    uint32_t r[4], max[4];
    power_cpuid(0, 0, max);
    if (max[0] >= 0xb) {
        /* The core topology level supplies the package ID shift; leaf B's
         * SMT level alone would label every core as a separate package. */
        for (unsigned sub = 0; sub < 8; sub++) {
            power_cpuid(0xb, sub, r);
            if (!r[1]) break;
            if (((r[2] >> 8) & 255) == 2) {
                sample.package = r[3] >> (r[0] & 31); break;
            }
        }
    }
    uint64_t platform = power_rdmsr(0xce);
    sample.minimum = (platform >> 40) & 255;
    sample.maximum = (platform >> 8) & 255;
    sample.ratio = (power_rdmsr(0x198) >> 8) & 255;
    power_cpuid(6, 0, r);
    if (r[0] & 2) sample.turbo = power_rdmsr(0x1ad) & 255;
    if (r[0] & 1) {
        unsigned target = (power_rdmsr(0x1a2) >> 16) & 255;
        uint64_t core = power_rdmsr(0x19c);
        sample.core_valid = target && (core & (1ull << 31));
        sample.core = dts_temperature(core, target);
        /* Status bits, not sticky log bits: thermal/prochot/current/power
         * limit. A past event must not be reported as currently throttling.
         * Intel SDM vol.4 IA32_THERM_STATUS, family 06_3EH. */
        sample.throttled = (core & ((1u << 0) | (1u << 2) | (1u << 10) | (1u << 12))) != 0;
        if (r[0] & (1u << 6)) {
            uint64_t package = power_rdmsr(0x1b1);
            /* Correction: this used to require bit 31 like IA32_THERM_STATUS.
             * IA32_PACKAGE_THERM_STATUS has no Reading Valid bit; bit 31 is
             * reserved, so that test silently hid real package temperatures.
             * CPUID.6:EAX[6] above establishes support; a nonzero target is
             * still required to translate the digital readout to Celsius.
             * Intel SDM vol.3B 16.9, figure 16-33:
             * https://cdrdv2-public.intel.com/858456/253669-088-sdm-vol-3b.pdf#page=47
             * Model regression: target 100, delta 50, bit 31 clear -> 50C.
             * Not verified on X79-VG4 hardware. */
            sample.package_valid = target != 0;
            sample.package_temp = dts_temperature(package, target);
        }
    }
    /* BIOS_SIGN_ID is refreshed by CPUID.1 after writing zero, per SDM vol.3
     * microcode update signature procedure. Reading a stale latch alone can
     * silently report the previous microcode. No update is loaded here. */
    power_wrmsr(0x8b, 0);
    power_cpuid(1, 0, r);
    sample.microcode = power_rdmsr(0x8b) >> 32;
    sample.valid = 1;
    samples[cpu] = sample;
}

void cpu_power_report(void)
{
    for (unsigned cpu = 0; cpu < POWER_CPUS; cpu++) {
        const struct power_sample *s = &samples[cpu];
        if (!s->valid) continue;
        int first = 1;
        for (unsigned i = 0; i < cpu; i++)
            if (samples[i].valid && samples[i].package == s->package) first = 0;
        if (first) {
            kprintf("[cpu] package %u ratio %u non-turbo %u min %u turbo %u microcode %08x\n",
                    s->package, s->ratio, s->maximum, s->minimum, s->turbo, s->microcode);
            if (s->package_valid) kprintf("[cpu] package %u package %dC\n", s->package, s->package_temp);
            else kprintf("[cpu] package %u temperature unavailable\n", s->package);
        }
        if (s->core_valid)
            kprintf("[cpu] cpu %u core %dC throttling %s\n", cpu, s->core, s->throttled ? "yes" : "no");
        else kprintf("[cpu] cpu %u core unavailable\n", cpu);
    }
    unsigned cpu = power_cpu();
    if (cpu >= POWER_CPUS || !samples[cpu].valid)
        kprintf("[cpu] power MSRs unavailable for this CPU; no temperatures inferred\n");
}
