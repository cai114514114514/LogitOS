#ifndef LOGIT_SMP_TOPOLOGY_H
#define LOGIT_SMP_TOPOLOGY_H

#include <stddef.h>
#include <stdint.h>

/* CPUID.1A core types used by Intel hybrid parts. Keep the architectural
 * values: diagnostics can then be compared directly with a CPUID dump. */
enum smp_core_class {
    SMP_CORE_UNKNOWN     = 0x00,
    SMP_CORE_EFFICIENCY  = 0x20,
    SMP_CORE_PERFORMANCE = 0x40,
};

enum smp_topology_source {
    SMP_TOPOLOGY_NONE    = 0,
    SMP_TOPOLOGY_LEGACY  = 1,
    SMP_TOPOLOGY_LEAF_B  = 0x0b,
    SMP_TOPOLOGY_LEAF_1F = 0x1f,
};

/* One logical CPU's local CPUID view joined to the APIC ID by which MADT and
 * the LAPIC identify it. `valid` describes the CPUID topology. A mismatch does
 * not make the CPU unusable: the SMP layer keeps it online, records a loud
 * mismatch, and treats it as an unknown, single-thread core rather than merging
 * it with a possibly unrelated logical processor. */
struct smp_topology_sample {
    int valid;
    int hybrid;
    int native_model_valid;
    uint32_t madt_apic_id;
    uint32_t cpuid_apic_id;
    uint32_t package_id;
    uint32_t core_id;
    uint32_t thread_id;
    uint32_t native_model_id;
    uint8_t core_class;
    uint8_t source;
};

struct smp_topology_record {
    int present;
    int topology_valid;
    int apic_mismatch;
    int hybrid;
    int native_model_valid;
    uint32_t madt_apic_id;
    uint32_t cpuid_apic_id;
    uint32_t package_id;
    uint32_t core_id;
    uint32_t thread_id;
    uint32_t native_model_id;
    uint8_t core_class;
    uint8_t source;
};

struct smp_topology_summary {
    uint32_t logical;
    uint32_t cores;
    uint32_t performance_logical;
    uint32_t performance_cores;
    uint32_t efficiency_logical;
    uint32_t efficiency_cores;
    uint32_t unknown_logical;
    uint32_t unknown_cores;
    uint32_t apic_mismatches;
    uint32_t class_conflicts;
};

/* Store `slot` after rejecting duplicate MADT APIC IDs in earlier slots.
 * Returns 0, -1 for malformed/bounds, or -2 for a duplicate APIC ID. */
int smp_topology_store(struct smp_topology_record *records, size_t capacity,
                       size_t slot, const struct smp_topology_sample *sample);

void smp_topology_summarize(const struct smp_topology_record *records,
                            size_t count, struct smp_topology_summary *summary);

const char *smp_core_class_name(uint8_t core_class);
const char *smp_topology_source_name(uint8_t source);

#endif
