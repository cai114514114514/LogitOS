/* Hosted proof for a 14700KF-shaped 8P/12E, 28-thread topology. The CPUID
 * decoder has its own fixtures; this gate covers the MADT-to-SMP join and the
 * fact that E-cores must not be collapsed by a package-wide SMT=2 assumption. */
#include <stdint.h>
#include <stdio.h>

#include "apic_model.h"
#include "smp_boot_model.h"
#include "smp_topology.h"

static int checks, failures;
#define CHECK(c, m) do { checks++; if (!(c)) { failures++; printf("FAIL: %s\n", m); } } while (0)
#define TEST_UNUSED __attribute__((unused))

#define RAPTOR_LOGICAL 28
#define RAPTOR_P_LOGICAL 16
#define RAPTOR_E_LOGICAL 12

static int publish_pause_state, cancel_during_publish;
static enum smp_ap_boot_state publish_cancel_result;

void smp_boot_host_publish_pause(volatile int *state)
{
    publish_pause_state = __atomic_load_n(state, __ATOMIC_ACQUIRE);
    if (cancel_during_publish)
        publish_cancel_result = smp_bsp_cancel_unpublished(state);
}

static const uint32_t p_apic[RAPTOR_P_LOGICAL] = {
    0, 1, 4, 5, 8, 9, 12, 13, 16, 17, 20, 21, 24, 25, 28, 29
};

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void type0(uint8_t e[8], uint8_t uid, uint8_t id)
{
    for (int i = 0; i < 8; i++) e[i] = 0;
    e[0] = 0; e[1] = 8; e[2] = uid; e[3] = id; e[4] = 1;
}

static void type9(uint8_t e[16], uint32_t uid, uint32_t id)
{
    for (int i = 0; i < 16; i++) e[i] = 0;
    e[0] = 9; e[1] = 16;
    put32(e + 4, id);
    put32(e + 8, 1);
    put32(e + 12, uid);
}

static int make_madt_set(uint32_t ids[RAPTOR_LOGICAL],
                         uint32_t uids[RAPTOR_LOGICAL])
{
    size_t count = 0;
    for (int i = 0; i < RAPTOR_LOGICAL; i++) {
        struct apic_madt_cpu cpu;
        int decoded;
        if (i < RAPTOR_P_LOGICAL) {
            uint8_t e[8];
            type0(e, (uint8_t)(i + 1), (uint8_t)p_apic[i]);
            decoded = apic_model_madt_cpu(e, sizeof e, &cpu);
        } else {
            uint8_t e[16];
            /* Wide, consecutive x2APIC IDs make both hazards observable:
             * truncating to eight bits collides with the BSP, while assuming
             * every logical pair is SMT collapses twelve real E-cores to six. */
            type9(e, (uint32_t)(i + 1), 0x100u + (uint32_t)(i - RAPTOR_P_LOGICAL));
            decoded = apic_model_madt_cpu(e, sizeof e, &cpu);
        }
        if (decoded != 1 ||
            apic_model_add_madt_cpu(&cpu, ids, uids, &count, RAPTOR_LOGICAL) != 1)
            return -1;
    }
    return count == RAPTOR_LOGICAL ? 0 : -1;
}

static int make_topology(struct smp_topology_record records[RAPTOR_LOGICAL])
{
    uint32_t ids[RAPTOR_LOGICAL] = {0}, uids[RAPTOR_LOGICAL] = {0};
    if (make_madt_set(ids, uids) != 0) return -1;
    for (int i = 0; i < RAPTOR_LOGICAL; i++) {
        int efficiency = i >= RAPTOR_P_LOGICAL;
        int eidx = i - RAPTOR_P_LOGICAL;
        struct smp_topology_sample sample = {
            .valid = 1,
            .hybrid = 1,
            .native_model_valid = 1,
            .madt_apic_id = ids[i],
            .cpuid_apic_id = ids[i],
            .package_id = 0,
            .core_id = efficiency ? (uint32_t)(8 + eidx) : (uint32_t)(i / 2),
            .thread_id = efficiency ? 0u : (uint32_t)(i & 1),
            .native_model_id = efficiency ? 0x20u : 0x40u,
            .core_class = efficiency ? SMP_CORE_EFFICIENCY : SMP_CORE_PERFORMANCE,
            .source = SMP_TOPOLOGY_LEAF_1F,
        };
        if (smp_topology_store(records, RAPTOR_LOGICAL, (size_t)i, &sample) != 0)
            return -1;
    }
    return 0;
}

static TEST_UNUSED void apic_width_focus(void)
{
    struct smp_topology_record r[2] = {0};
    struct smp_topology_sample a = {
        .valid = 1, .madt_apic_id = 0, .cpuid_apic_id = 0,
        .core_class = SMP_CORE_PERFORMANCE, .source = SMP_TOPOLOGY_LEAF_1F,
    };
    struct smp_topology_sample b = {
        .valid = 1, .madt_apic_id = 0x100, .cpuid_apic_id = 0x100,
        .core_id = 8, .core_class = SMP_CORE_EFFICIENCY,
        .source = SMP_TOPOLOGY_LEAF_1F,
    };
    int first = smp_topology_store(r, 2, 0, &a);
    int second = smp_topology_store(r, 2, 1, &b);
    CHECK(first == 0 && second == 0 && r[1].madt_apic_id == 0x100,
          "wide MADT x2APIC IDs remain distinct in SMP slots");
}

static TEST_UNUSED void hybrid_core_focus(void)
{
    struct smp_topology_record records[RAPTOR_LOGICAL] = {0};
    struct smp_topology_summary s;
    int ok = make_topology(records);
    smp_topology_summarize(records, RAPTOR_LOGICAL, &s);
    CHECK(ok == 0 && s.cores == 20,
          "8 SMT P-cores plus 12 single-thread E-cores report 20 physical cores");
}

static TEST_UNUSED void core_type_focus(void)
{
    struct smp_topology_record records[RAPTOR_LOGICAL] = {0};
    struct smp_topology_summary s;
    int ok = make_topology(records);
    smp_topology_summarize(records, RAPTOR_LOGICAL, &s);
    CHECK(ok == 0 && s.performance_logical == 16 && s.performance_cores == 8 &&
          s.efficiency_logical == 12 && s.efficiency_cores == 12,
          "CPUID.1A core classes survive per-CPU recording and aggregation");
}

static TEST_UNUSED void publishing_timeout_focus(void)
{
    volatile int state = SMP_AP_PUBLISHING, online = 1;
    int committed = smp_bsp_commit_online(&state, &online, 1);
    enum smp_ap_boot_state cancelled = smp_bsp_cancel_unpublished(&state);
    CHECK(committed != 0 && online == 1 &&
          cancelled == SMP_AP_REJECTED && state == SMP_AP_REJECTED,
          "PUBLISHING timeout cannot enter the dense online set");
}

static void boot_state_positive(void)
{
    volatile int state = SMP_AP_CLAIMED, online = 1;
    publish_pause_state = -1;
    cancel_during_publish = 0;
    CHECK(smp_ap_publish_online(&state) == 0 &&
          state == SMP_AP_ONLINE && online == 1 &&
          publish_pause_state == SMP_AP_PUBLISHING,
          "AP publishes ONLINE only after traversing PUBLISHING and never owns the count");
    CHECK(smp_bsp_commit_online(&state, &online, 1) == 0 && online == 2 &&
          smp_bsp_commit_online(&state, &online, 1) != 0 && online == 2,
          "BSP admits a published AP once at the exact dense frontier");

    volatile int stalled = SMP_AP_PUBLISHING, stalled_online = 1;
    CHECK(smp_bsp_commit_online(&stalled, &stalled_online, 1) != 0 &&
          smp_bsp_cancel_unpublished(&stalled) == SMP_AP_REJECTED &&
          stalled == SMP_AP_REJECTED && stalled_online == 1,
          "BSP cancels a stalled publisher without exposing its slot");

    volatile int late = SMP_AP_CLAIMED;
    cancel_during_publish = 1;
    publish_cancel_result = SMP_AP_EMPTY;
    CHECK(smp_ap_publish_online(&late) != 0 &&
          publish_pause_state == SMP_AP_PUBLISHING &&
          publish_cancel_result == SMP_AP_REJECTED && late == SMP_AP_REJECTED,
          "late AP publish CAS cannot overwrite timeout cancellation");
    cancel_during_publish = 0;
}

static TEST_UNUSED void positive(void)
{
    uint32_t ids[RAPTOR_LOGICAL] = {0}, uids[RAPTOR_LOGICAL] = {0};
    CHECK(make_madt_set(ids, uids) == 0,
          "MADT type0/type9 fixture yields all 28 enabled logical CPUs");
    CHECK(ids[0] == 0 && ids[15] == 29 && ids[16] == 0x100 && ids[27] == 0x10b,
          "MADT order retains sparse and full-width APIC IDs");

    size_t count = RAPTOR_LOGICAL;
    struct apic_madt_cpu exact = {
        .apic_id = ids[7], .acpi_uid = uids[7], .source_type = 9,
    };
    CHECK(apic_model_add_madt_cpu(&exact, ids, uids, &count, RAPTOR_LOGICAL) == 0 &&
          count == RAPTOR_LOGICAL,
          "matching type0/type9 firmware records do not double-start one CPU");

    struct smp_topology_record records[RAPTOR_LOGICAL] = {0};
    CHECK(make_topology(records) == 0,
          "all 28 MADT CPUs accept their own local CPUID topology sample");
    CHECK(records[0].source == SMP_TOPOLOGY_LEAF_1F &&
          records[0].native_model_valid && records[0].native_model_id == 0x40 &&
          records[16].source == SMP_TOPOLOGY_LEAF_1F &&
          records[16].native_model_valid && records[16].native_model_id == 0x20,
          "per-CPU records preserve CPUID.1F source and CPUID.1A native model");
    struct smp_topology_summary s;
    smp_topology_summarize(records, RAPTOR_LOGICAL, &s);
    CHECK(s.logical == 28 && s.cores == 20,
          "14700KF-shaped topology aggregates to 28 logical and 20 physical CPUs");
    CHECK(s.performance_logical == 16 && s.performance_cores == 8,
          "eight Hyper-Threaded P-cores remain eight cores and sixteen logical CPUs");
    CHECK(s.efficiency_logical == 12 && s.efficiency_cores == 12,
          "twelve E-cores remain twelve distinct single-thread cores");
    CHECK(s.unknown_logical == 0 && s.unknown_cores == 0 &&
          s.apic_mismatches == 0 && s.class_conflicts == 0,
          "valid hybrid fixture has no silent topology degradation");

    struct smp_topology_record mismatch[1] = {0};
    struct smp_topology_sample bad = {
        .valid = 1, .madt_apic_id = 300, .cpuid_apic_id = 301,
        .package_id = 0, .core_id = 2, .thread_id = 1,
        .core_class = SMP_CORE_PERFORMANCE, .source = SMP_TOPOLOGY_LEAF_1F,
    };
    CHECK(smp_topology_store(mismatch, 1, 0, &bad) == 0 &&
          mismatch[0].present && !mismatch[0].topology_valid &&
          mismatch[0].apic_mismatch && mismatch[0].core_class == SMP_CORE_UNKNOWN,
          "CPUID/MADT APIC mismatch stays online but degrades to a unique unknown core");
    smp_topology_summarize(mismatch, 1, &s);
    CHECK(s.logical == 1 && s.cores == 1 && s.unknown_logical == 1 &&
          s.unknown_cores == 1 && s.apic_mismatches == 1,
          "mismatched APIC identity is visible in the topology summary");

    boot_state_positive();
}

int main(void)
{
#if defined(LOGIT_RAPTOR_SMP_NEGCTL_APIC8)
    apic_width_focus();
#elif defined(LOGIT_RAPTOR_SMP_NEGCTL_ASSUME_SMT2)
    hybrid_core_focus();
#elif defined(LOGIT_RAPTOR_SMP_NEGCTL_ALL_P)
    core_type_focus();
#elif defined(LOGIT_RAPTOR_SMP_NEGCTL_COMMIT_PUBLISHING)
    publishing_timeout_focus();
#else
    positive();
#endif
    printf("RAPTOR_SMP_HOST: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
