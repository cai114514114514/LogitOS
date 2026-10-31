#ifndef LOGIT_CPU_POWER_HARDWARE_H
#define LOGIT_CPU_POWER_HARDWARE_H
#include <stdint.h>
#include "../smp/percpu.h"

/* Derive the storage ceiling from the scheduler's CPU slots; an independent
 * literal would silently disagree with a later topology/cap change. */
#define POWER_CPUS PERCPU_MAXCPU
#define POWER_STATES 32
struct power_pss { unsigned ratio; uint32_t control; };
struct power_cstate { unsigned type, latency_us, hint; };

/* This is the privileged-instruction boundary, not a second policy model.
 * Host gates replace only these operations. Not verified on X79-VG4 hardware. */
void power_cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]);
uint64_t power_rdmsr(uint32_t reg);
void power_wrmsr(uint32_t reg, uint64_t value);
uint64_t power_read_cr(unsigned reg);
void power_write_cr(unsigned reg, uint64_t value);
uint64_t power_irq_save(void);
void power_irq_restore(uint64_t flags);
void power_wbinvd(void);
unsigned power_cpu(void);
uint64_t power_now(void);
void power_halt(void);
void power_mwait(unsigned hint, const void *address);
void power_mce_putc(char c); /* bounded, lock-free COM1 emergency sink */
unsigned power_get_pss(unsigned cpu, struct power_pss *out);
unsigned power_get_cst(unsigned cpu, struct power_cstate *out);

static inline int power_intel(void)
{
    uint32_t r[4]; power_cpuid(0, 0, r);
    return r[1] == 0x756e6547 && r[3] == 0x49656e69 && r[2] == 0x6c65746e;
}

/* Model-specific MSRs have no universal CPUID bit. Deliberately refuse other
 * models rather than let a valid CPUID.MSR turn an unsupported RDMSR into #GP.
 * Intel SDM vol.4, family 06_2DH / 06_3EH register tables:
 * https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html */
static inline int power_legacy_intel(void)
{
    uint32_t r[4];
    if (!power_intel()) return 0;
    power_cpuid(1, 0, r);
    unsigned family = (r[0] >> 8) & 15;
    unsigned model = ((r[0] >> 4) & 15) | ((r[0] >> 12) & 0xf0);
    return family == 6 && (model == 0x2d || model == 0x3e) && (r[3] & (1u << 5));
}
#endif
