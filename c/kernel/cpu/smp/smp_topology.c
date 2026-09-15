/* Pure hybrid-topology bookkeeping. CPUID decoding stays in cpu_platform.c;
 * this file owns only the SMP join, validation and aggregate view. */
#include "smp_topology.h"

static uint8_t normalized_class(uint8_t value)
{
#ifdef LOGIT_RAPTOR_SMP_NEGCTL_ALL_P
    (void)value;
    return SMP_CORE_PERFORMANCE;
#else
    return value == SMP_CORE_PERFORMANCE || value == SMP_CORE_EFFICIENCY
             ? value : SMP_CORE_UNKNOWN;
#endif
}

int smp_topology_store(struct smp_topology_record *records, size_t capacity,
                       size_t slot, const struct smp_topology_sample *sample)
{
    if (!records || !sample || slot >= capacity ||
        sample->madt_apic_id == UINT32_MAX)
        return -1;

#ifdef LOGIT_RAPTOR_SMP_NEGCTL_APIC8
    uint32_t madt_id = (uint8_t)sample->madt_apic_id;
#else
    uint32_t madt_id = sample->madt_apic_id;
#endif
    for (size_t i = 0; i < slot; i++)
        if (records[i].present && records[i].madt_apic_id == madt_id)
            return -2;

    struct smp_topology_record r = {0};
    r.present = 1;
    r.madt_apic_id = madt_id;
    r.cpuid_apic_id = sample->cpuid_apic_id;
    r.apic_mismatch = sample->valid && sample->cpuid_apic_id != sample->madt_apic_id;
    r.topology_valid = sample->valid && !r.apic_mismatch;
    r.hybrid = sample->hybrid;
    r.native_model_valid = sample->native_model_valid;
    r.package_id = r.topology_valid ? sample->package_id : 0;
    r.core_id = r.topology_valid ? sample->core_id : madt_id;
    r.thread_id = r.topology_valid ? sample->thread_id : 0;
    r.native_model_id = sample->native_model_id;
    r.core_class = r.topology_valid ? normalized_class(sample->core_class)
                                    : SMP_CORE_UNKNOWN;
    r.source = r.topology_valid ? sample->source : SMP_TOPOLOGY_NONE;
    records[slot] = r;
    return 0;
}

static int same_core(const struct smp_topology_record *a,
                     const struct smp_topology_record *b)
{
#ifdef LOGIT_RAPTOR_SMP_NEGCTL_ASSUME_SMT2
    return (a->madt_apic_id >> 1) == (b->madt_apic_id >> 1);
#else
    if (a->topology_valid != b->topology_valid) return 0;
    if (!a->topology_valid)
        return a->madt_apic_id == b->madt_apic_id;
    return a->package_id == b->package_id && a->core_id == b->core_id;
#endif
}

void smp_topology_summarize(const struct smp_topology_record *records,
                            size_t count, struct smp_topology_summary *s)
{
    if (!s) return;
    *s = (struct smp_topology_summary){0};
    if (!records) return;

    for (size_t i = 0; i < count; i++) {
        const struct smp_topology_record *r = &records[i];
        if (!r->present) continue;
        s->logical++;
        if (r->apic_mismatch) s->apic_mismatches++;
        if (r->core_class == SMP_CORE_PERFORMANCE)
            s->performance_logical++;
        else if (r->core_class == SMP_CORE_EFFICIENCY)
            s->efficiency_logical++;
        else
            s->unknown_logical++;

        int leader = 1;
        for (size_t j = 0; j < i; j++)
            if (records[j].present && same_core(&records[j], r)) {
                leader = 0;
                break;
            }
        if (!leader) continue;

        uint8_t core_class = r->core_class;
        for (size_t j = i + 1; j < count; j++) {
            if (!records[j].present || !same_core(r, &records[j])) continue;
            if (records[j].core_class != core_class) {
                core_class = SMP_CORE_UNKNOWN;
                s->class_conflicts++;
                break;
            }
        }
        s->cores++;
        if (core_class == SMP_CORE_PERFORMANCE)
            s->performance_cores++;
        else if (core_class == SMP_CORE_EFFICIENCY)
            s->efficiency_cores++;
        else
            s->unknown_cores++;
    }
}

const char *smp_core_class_name(uint8_t core_class)
{
    if (core_class == SMP_CORE_PERFORMANCE) return "performance";
    if (core_class == SMP_CORE_EFFICIENCY) return "efficiency";
    return "unknown";
}

const char *smp_topology_source_name(uint8_t source)
{
    if (source == SMP_TOPOLOGY_LEAF_1F) return "cpuid.1f";
    if (source == SMP_TOPOLOGY_LEAF_B) return "cpuid.0b";
    if (source == SMP_TOPOLOGY_LEGACY) return "legacy";
    return "none";
}
