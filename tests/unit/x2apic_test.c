/* Production APIC/MADT/SMP boot rules with only CPUID, MSR, MMIO and delay
 * leaves emulated. The same c/kernel/cpu sources are linked into the kernel. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "apic_model.h"
#include "lapic.h"
#include "smp_boot_model.h"

static int checks, failures;
#define CHECK(c, m) do { checks++; if (!(c)) { failures++; printf("FAIL: %s\n", m); } } while (0)
#define TEST_UNUSED __attribute__((unused))

#define CPUID_X2APIC (1u << 21)
#define CPUID_MSR    (1u << 5)
#define CPUID_APIC   (1u << 9)
#define APIC_ENABLE  (1ull << 11)
#define APIC_EXTD    (1ull << 10)
#define IA32_APIC_BASE 0x1bu

struct msr_write { uint32_t msr; uint64_t value; };
static uint32_t host_ecx, host_edx;
static uint64_t host_msrs[0x900];
static struct msr_write host_writes[64];
static int host_nwrite, host_maps, host_fail_read;
static uint32_t host_mmio[0x400 / 4];
static int host_sticky_icr;
static uint64_t host_delays[8];
static int host_ndelay, host_delay_fail;
static int publish_pause_state, cancel_during_publish;
static enum smp_ap_boot_state publish_cancel_result;

void kprintf(const char *fmt, ...) { (void)fmt; }

void lapic_host_cpuid1(uint32_t *ecx, uint32_t *edx)
{
    *ecx = host_ecx;
    *edx = host_edx;
}

int lapic_host_rdmsr(uint32_t msr, uint64_t *value)
{
    if (host_fail_read || msr >= 0x900) return -1;
    *value = host_msrs[msr];
    return 0;
}

void lapic_host_wrmsr(uint32_t msr, uint64_t value)
{
    if (host_nwrite < (int)(sizeof host_writes / sizeof host_writes[0]))
        host_writes[host_nwrite++] = (struct msr_write){ msr, value };
    if (msr < 0x900) host_msrs[msr] = value;
}

volatile uint8_t *lapic_host_map(uint64_t phys)
{
    (void)phys;
    host_maps++;
    return (volatile uint8_t *)host_mmio;
}

uint32_t lapic_host_mmio_read(uint32_t reg)
{
    uint32_t value = host_mmio[reg / 4];
    if (reg == 0x300 && host_sticky_icr) value |= 1u << 12;
    return value;
}

void lapic_host_mmio_write(uint32_t reg, uint32_t value)
{
    host_mmio[reg / 4] = value;
}

int lapic_host_delay_ns(uint64_t ns)
{
    if (host_ndelay < (int)(sizeof host_delays / sizeof host_delays[0]))
        host_delays[host_ndelay++] = ns;
    return host_delay_fail ? -1 : 0;
}

void smp_boot_host_publish_pause(volatile int *state)
{
    publish_pause_state = __atomic_load_n(state, __ATOMIC_ACQUIRE);
    if (cancel_during_publish)
        publish_cancel_result = smp_bsp_cancel_unpublished(state);
}

static TEST_UNUSED void host_reset(uint64_t apic_base, uint32_t apic_id)
{
    memset(host_msrs, 0, sizeof host_msrs);
    memset(host_writes, 0, sizeof host_writes);
    memset(host_mmio, 0, sizeof host_mmio);
    memset(host_delays, 0, sizeof host_delays);
    host_ecx = CPUID_X2APIC;
    host_edx = CPUID_MSR | CPUID_APIC;
    host_msrs[IA32_APIC_BASE] = apic_base;
    host_msrs[0x802] = apic_id;
    host_mmio[0x20 / 4] = apic_id << 24;
    host_nwrite = host_maps = host_fail_read = host_sticky_icr = 0;
    host_ndelay = host_delay_fail = 0;
    publish_pause_state = -1;
    cancel_during_publish = 0;
    publish_cancel_result = SMP_AP_EMPTY;
    lapic_host_reset();
}

static TEST_UNUSED void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static TEST_UNUSED void type9(uint8_t e[16], uint32_t id, uint32_t flags, uint32_t uid)
{
    memset(e, 0, 16);
    e[0] = 9;
    e[1] = 16;
    put32(e + 4, id);
    put32(e + 8, flags);
    put32(e + 12, uid);
}

static TEST_UNUSED void test_type9_focus(void)
{
    uint8_t e[16];
    struct apic_madt_cpu cpu = {0};
    type9(e, 0x12345678u, 1, 0x89abcdefu);
    CHECK(apic_model_madt_cpu(e, sizeof e, &cpu) == 1 &&
          cpu.apic_id == 0x12345678u && cpu.acpi_uid == 0x89abcdefu &&
          cpu.source_type == 9,
          "MADT type9 preserves its full APIC ID and ACPI UID");
}

static TEST_UNUSED void test_madt(void)
{
    struct apic_madt_cpu cpu = {0};
    uint8_t t0[8] = {0, 8, 7, 0x2a, 1, 0, 0, 0};
    CHECK(apic_model_madt_cpu(t0, sizeof t0, &cpu) == 1 &&
          cpu.apic_id == 0x2a && cpu.acpi_uid == 7 && cpu.source_type == 0,
          "MADT type0 remains compatible");
    t0[4] = 2;
    CHECK(apic_model_madt_cpu(t0, sizeof t0, &cpu) == 0,
          "type0 online-capable-only CPU is not booted without hotplug");
    t0[3] = 0xff; t0[4] = 1;
    CHECK(apic_model_madt_cpu(t0, sizeof t0, &cpu) == -1,
          "reserved type0 APIC ID 255 is rejected");

    uint8_t t9[16];
    type9(t9, 0x12345678u, 1, 0x89abcdefu);
    CHECK(apic_model_madt_cpu(t9, sizeof t9, &cpu) == 1 &&
          cpu.apic_id == 0x12345678u && cpu.acpi_uid == 0x89abcdefu,
          "MADT type9 preserves 32-bit fields");
    put32(t9 + 8, 2);
    CHECK(apic_model_madt_cpu(t9, sizeof t9, &cpu) == 0,
          "type9 online-capable bit alone is not treated as enabled");
    put32(t9 + 8, 3);
    CHECK(apic_model_madt_cpu(t9, sizeof t9, &cpu) == 1,
          "type9 enabled bit remains authoritative when both flags are set");
    t9[2] = 1;
    CHECK(apic_model_madt_cpu(t9, sizeof t9, &cpu) == -1,
          "type9 reserved bytes are validated");

    uint32_t ids[4] = {0}, uids[4] = {0};
    size_t count = 0;
    struct apic_madt_cpu a = {.apic_id=3, .acpi_uid=9, .source_type=0};
    struct apic_madt_cpu exact = {.apic_id=3, .acpi_uid=9, .source_type=9};
    struct apic_madt_cpu id_conflict = {.apic_id=3, .acpi_uid=10, .source_type=9};
    struct apic_madt_cpu uid_conflict = {.apic_id=4, .acpi_uid=9, .source_type=9};
    CHECK(apic_model_add_madt_cpu(&a, ids, uids, &count, 4) == 1 && count == 1,
          "first MADT CPU record is inserted");
    CHECK(apic_model_add_madt_cpu(&exact, ids, uids, &count, 4) == 0 && count == 1,
          "matching type0/type9 records do not start a CPU twice");
    CHECK(apic_model_add_madt_cpu(&id_conflict, ids, uids, &count, 4) == -1 && count == 1,
          "one APIC ID cannot name two ACPI UIDs");
    CHECK(apic_model_add_madt_cpu(&uid_conflict, ids, uids, &count, 4) == -1 && count == 1,
          "one ACPI UID cannot name two APIC IDs");
}

static TEST_UNUSED void test_duplicate_focus(void)
{
    uint32_t ids[2] = {0}, uids[2] = {0};
    size_t count = 0;
    struct apic_madt_cpu a = {.apic_id=7, .acpi_uid=11, .source_type=0};
    struct apic_madt_cpu b = {.apic_id=7, .acpi_uid=11, .source_type=9};
    (void)apic_model_add_madt_cpu(&a, ids, uids, &count, 2);
    CHECK(apic_model_add_madt_cpu(&b, ids, uids, &count, 2) == 0,
          "matching type0/type9 records do not start a CPU twice");
}

static TEST_UNUSED void test_mode_focus(void)
{
    CHECK(apic_model_access_mode(CPUID_X2APIC, CPUID_MSR | CPUID_APIC,
                                 APIC_ENABLE | APIC_EXTD) == APIC_ACCESS_X2APIC,
          "firmware-active x2APIC selects the MSR backend");
}

static TEST_UNUSED void test_modes(void)
{
    CHECK(apic_model_access_mode(CPUID_X2APIC, CPUID_MSR | CPUID_APIC,
                                 APIC_ENABLE) == APIC_ACCESS_XAPIC,
          "enabled xAPIC selects MMIO backend");
    CHECK(apic_model_access_mode(CPUID_X2APIC, CPUID_MSR | CPUID_APIC,
                                 APIC_ENABLE | APIC_EXTD) == APIC_ACCESS_X2APIC,
          "enabled x2APIC selects MSR backend");
    CHECK(apic_model_access_mode(0, CPUID_MSR | CPUID_APIC,
                                 APIC_ENABLE | APIC_EXTD) == APIC_ACCESS_NONE,
          "inconsistent x2APIC state is rejected");
    CHECK(apic_model_access_mode(CPUID_X2APIC, CPUID_APIC, APIC_ENABLE) == APIC_ACCESS_NONE,
          "APIC mode without MSR capability is rejected before RDMSR");
}

static TEST_UNUSED void test_icr_focus(void)
{
    uint64_t value = 0;
    CHECK(apic_model_icr(APIC_ACCESS_X2APIC, 0x12345678u, 0x40f1u, &value) == 0 &&
          value == 0x12345678000040f1ull,
          "x2APIC ICR keeps all 32 destination bits");
}

static TEST_UNUSED void test_external_focus(void)
{
    uint8_t encoded = 0;
    CHECK(apic_model_external_dest8(0x12345678u, &encoded) == -1,
          "legacy external routing rejects an unrepresentable APIC ID");
}

static TEST_UNUSED void test_icr_rules(void)
{
    uint64_t value = 0;
    uint8_t encoded = 0;
    CHECK(apic_model_icr(APIC_ACCESS_X2APIC, 0x12345678u, 0x40f1u, &value) == 0 &&
          value == 0x12345678000040f1ull,
          "x2APIC ICR preserves all destination bits");
    CHECK(apic_model_icr(APIC_ACCESS_XAPIC, 0xfeu, 0x4042u, &value) == 0 &&
          value == 0xfe00000000004042ull,
          "xAPIC ICR retains 8-bit physical destination");
    CHECK(apic_model_icr(APIC_ACCESS_XAPIC, 0xffu, 0x4042u, &value) == -1,
          "xAPIC unicast rejects reserved destination 255");
    CHECK(apic_model_external_dest8(0xfe, &encoded) == 0 && encoded == 0xfe,
          "legacy external destination 254 is representable");
    CHECK(apic_model_external_dest8(0xff, &encoded) == -1 &&
          apic_model_external_dest8(0x12345678u, &encoded) == -1,
          "legacy external routing rejects reserved and wide IDs");
}

static TEST_UNUSED int wrote(uint32_t msr, uint64_t value)
{
    for (int i = 0; i < host_nwrite; i++)
        if (host_writes[i].msr == msr && host_writes[i].value == value) return 1;
    return 0;
}

static TEST_UNUSED void test_x2_backend(void)
{
    const uint32_t id = 0x12345678u;
    host_reset(APIC_ENABLE | APIC_EXTD | 0xfee00000u, id);
    CHECK(lapic_init() == 0 && lapic_x2apic_active() && host_maps == 0,
          "firmware-active x2APIC initializes without LAPIC MMIO");
    CHECK(lapic_id() == id, "x2APIC ID MSR is not truncated");
    CHECK(wrote(0x80f, 0x1ff) && wrote(0x80b, 0),
          "x2APIC SVR and EOI use their architectural MSRs");
    host_nwrite = 0;
    CHECK(lapic_send_ipi(id, 0xf1) == 0 &&
          wrote(0x830, ((uint64_t)id << 32) | 0x40f1u),
          "fixed x2APIC IPI is one full-width ICR WRMSR");

    host_nwrite = host_ndelay = 0;
    CHECK(lapic_start_ap(id, 8) == 0 && host_nwrite == 3 &&
          host_writes[0].msr == 0x830 &&
          host_writes[0].value == (((uint64_t)id << 32) | 0x4500u) &&
          host_writes[1].value == (((uint64_t)id << 32) | 0x4608u) &&
          host_writes[2].value == (((uint64_t)id << 32) | 0x4608u),
          "x2APIC INIT-SIPI-SIPI uses full destination ID");
    CHECK(host_ndelay == 3 && host_delays[0] == 10000000ull &&
          host_delays[1] == 200000ull && host_delays[2] == 200000ull,
          "AP startup obeys 10ms INIT-to-SIPI and 200us SIPI spacing");
}

static TEST_UNUSED void test_delay_focus(void)
{
    host_reset(APIC_ENABLE | APIC_EXTD | 0xfee00000u, 4);
    (void)lapic_init();
    host_nwrite = host_ndelay = 0;
    (void)lapic_start_ap(4, 8);
    CHECK(host_ndelay == 3 && host_delays[0] == 10000000ull &&
          host_delays[1] == 200000ull,
          "AP startup obeys the Intel INIT/SIPI wall-time minima");
}

static TEST_UNUSED void test_busy_focus(void)
{
    host_reset(APIC_ENABLE | 0xfee00000u, 3);
    (void)lapic_init();
    host_sticky_icr = 1;
    CHECK(lapic_send_ipi(2, 0x42) == -1,
          "stuck xAPIC delivery status aborts the IPI");
}

static TEST_UNUSED void test_xapic_backend(void)
{
    host_reset(APIC_ENABLE | 0xfee00000u, 3);
    CHECK(lapic_init() == 0 && !lapic_x2apic_active() && host_maps == 1 &&
          lapic_id() == 3,
          "type0-era xAPIC MMIO remains operational");
    CHECK(lapic_send_ipi(0x22, 0x42) == 0 &&
          host_mmio[0x310 / 4] == 0x22000000u &&
          host_mmio[0x300 / 4] == 0x4042u,
          "xAPIC fixed IPI writes high destination then low command");
    CHECK(lapic_send_ipi(0x1234, 0x42) == -1,
          "xAPIC refuses a destination it cannot encode");
    host_mmio[0x300 / 4] = 0;
    host_sticky_icr = 1;
    CHECK(lapic_send_ipi(2, 0x42) == -1,
          "stuck xAPIC delivery status is propagated");
}

static TEST_UNUSED void test_publish_focus(void)
{
    volatile int state = SMP_AP_PUBLISHING, online = 1;
    CHECK(smp_bsp_commit_online(&state, &online, 1) != 0 && online == 1,
          "PUBLISHING AP is excluded from the dense online set");
}

static TEST_UNUSED void test_stack_focus(void)
{
    CHECK(!smp_ap_stack_may_free(SMP_AP_CLAIMED, 1),
          "a timed-out AP stack is quarantined after startup was sent");
}

static TEST_UNUSED void test_smp_boot(void)
{
    volatile int state = SMP_AP_CLAIMED, online = 1;
    publish_pause_state = -1;
    cancel_during_publish = 0;
    CHECK(smp_ap_publish_online(&state) == 0 &&
          state == SMP_AP_ONLINE && online == 1 &&
          publish_pause_state == SMP_AP_PUBLISHING,
          "fixed-slot AP release-publishes ONLINE without changing the dense count");
    CHECK(smp_bsp_commit_online(&state, &online, 1) == 0 && online == 2 &&
          smp_bsp_commit_online(&state, &online, 1) != 0 && online == 2,
          "BSP commits an ONLINE slot exactly once at the dense frontier");

    volatile int stalled = SMP_AP_PUBLISHING, stalled_online = 1;
    CHECK(smp_bsp_commit_online(&stalled, &stalled_online, 1) != 0 &&
          smp_bsp_cancel_unpublished(&stalled) == SMP_AP_REJECTED &&
          stalled == SMP_AP_REJECTED && stalled_online == 1,
          "PUBLISHING timeout is cancelled without inflating the dense count");

    volatile int late = SMP_AP_CLAIMED;
    cancel_during_publish = 1;
    publish_cancel_result = SMP_AP_EMPTY;
    CHECK(smp_ap_publish_online(&late) != 0 &&
          publish_pause_state == SMP_AP_PUBLISHING &&
          publish_cancel_result == SMP_AP_REJECTED && late == SMP_AP_REJECTED,
          "late AP cannot overwrite BSP cancellation after a PUBLISHING timeout");
    cancel_during_publish = 0;
    CHECK(!smp_ap_stack_may_free(SMP_AP_CLAIMED, 1) &&
          !smp_ap_stack_may_free(SMP_AP_REJECTED, 1),
          "any emitted startup command quarantines a late AP stack");
    CHECK(smp_ap_stack_may_free(SMP_AP_REJECTED, 0),
          "unpublished stack can be reclaimed before startup is sent");
}

static TEST_UNUSED void test_ap_timer_eoi_focus(void)
{
    CHECK(!apic_model_legacy_irq_uses_lapic(0, 0, 0) &&
          apic_model_legacy_irq_uses_lapic(1, 0, 0),
          "wide-BSP PIC fallback still EOIs an AP LAPIC timer locally");
}

static TEST_UNUSED void test_eoi_policy(void)
{
    CHECK(!apic_model_legacy_irq_uses_lapic(0, 0, 0),
          "BSP PIT uses PIC EOI when external routes are retained");
    CHECK(apic_model_legacy_irq_uses_lapic(1, 0, 0),
          "AP vector 32 always uses LAPIC EOI even with PIC retained");
    CHECK(apic_model_legacy_irq_uses_lapic(0, 1, 1),
          "BSP external IOAPIC route uses LAPIC EOI");
}

int main(void)
{
#if defined(LOGIT_X2APIC_NEGCTL_IGNORE_TYPE9)
    test_type9_focus();
#elif defined(LOGIT_X2APIC_NEGCTL_DUPLICATE_CPU)
    test_duplicate_focus();
#elif defined(LOGIT_X2APIC_NEGCTL_MMIO_WHEN_EXTD)
    test_mode_focus();
#elif defined(LOGIT_X2APIC_NEGCTL_TRUNCATE_ICR)
    test_icr_focus();
#elif defined(LOGIT_X2APIC_NEGCTL_TRUNCATE_EXTERNAL)
    test_external_focus();
#elif defined(LOGIT_X2APIC_NEGCTL_SHORT_DELAY)
    test_delay_focus();
#elif defined(LOGIT_X2APIC_NEGCTL_IGNORE_BUSY)
    test_busy_focus();
#elif defined(LOGIT_X2APIC_NEGCTL_COMMIT_PUBLISHING)
    test_publish_focus();
#elif defined(LOGIT_X2APIC_NEGCTL_FREE_TIMEOUT)
    test_stack_focus();
#elif defined(LOGIT_X2APIC_NEGCTL_AP_TIMER_PIC_EOI)
    test_ap_timer_eoi_focus();
#else
    test_madt();
    test_modes();
    test_icr_rules();
    test_x2_backend();
    test_xapic_backend();
    test_smp_boot();
    test_eoi_policy();
#endif
    printf("X2APIC_HOST: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
