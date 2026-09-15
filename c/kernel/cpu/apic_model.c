/* Pure APIC/MADT rules shared by the hardware backend and hosted tests. */
#include "apic_model.h"

#define CPUID1_ECX_X2APIC (1u << 21)
#define CPUID1_EDX_MSR     (1u << 5)
#define CPUID1_EDX_APIC    (1u << 9)
#define APIC_BASE_EXTD     (1ull << 10)
#define APIC_BASE_ENABLE   (1ull << 11)

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

enum apic_access_mode apic_model_access_mode(uint32_t ecx, uint32_t edx,
                                              uint64_t apic_base)
{
    if (!(edx & CPUID1_EDX_MSR) || !(edx & CPUID1_EDX_APIC) ||
        !(apic_base & APIC_BASE_ENABLE))
        return APIC_ACCESS_NONE;
    if (apic_base & APIC_BASE_EXTD) {
#ifdef LOGIT_X2APIC_NEGCTL_MMIO_WHEN_EXTD
        (void)ecx;
        return APIC_ACCESS_XAPIC;
#else
        return (ecx & CPUID1_ECX_X2APIC) ? APIC_ACCESS_X2APIC
                                         : APIC_ACCESS_NONE;
#endif
    }
    return APIC_ACCESS_XAPIC;
}

int apic_model_madt_cpu(const uint8_t *p, size_t available,
                        struct apic_madt_cpu *cpu)
{
    if (!p || !cpu || available < 2 || p[1] < 2 || p[1] > available)
        return -1;
    if (p[0] == 0) {                         /* Processor Local APIC */
        if (p[1] < 8) return -1;
        if (!(le32(p + 4) & 1u)) return 0;   /* hotplug-only is not online */
        if (p[3] == 0xffu) return -1;        /* reserved xAPIC ID */
        *cpu = (struct apic_madt_cpu){
            .apic_id = p[3], .acpi_uid = p[2], .source_type = 0
        };
        return 1;
    }
    if (p[0] == 9) {                         /* Processor Local x2APIC */
#ifdef LOGIT_X2APIC_NEGCTL_IGNORE_TYPE9
        return 0;
#else
        if (p[1] < 16 || p[2] || p[3]) return -1;
        if (!(le32(p + 8) & 1u)) return 0;
        uint32_t id = le32(p + 4);
        if (id == UINT32_MAX) return -1;      /* architectural broadcast ID */
        *cpu = (struct apic_madt_cpu){
            .apic_id = id, .acpi_uid = le32(p + 12), .source_type = 9
        };
        return 1;
#endif
    }
    return 0;
}

int apic_model_add_madt_cpu(const struct apic_madt_cpu *cpu,
                            uint32_t *apic_ids, uint32_t *acpi_uids,
                            size_t *count, size_t capacity)
{
    if (!cpu || !apic_ids || !acpi_uids || !count || *count > capacity)
        return -1;
    for (size_t i = 0; i < *count; i++) {
        if (apic_ids[i] == cpu->apic_id && acpi_uids[i] == cpu->acpi_uid) {
#ifdef LOGIT_X2APIC_NEGCTL_DUPLICATE_CPU
            return 1;
#else
            return 0;
#endif
        }
        if (apic_ids[i] == cpu->apic_id || acpi_uids[i] == cpu->acpi_uid)
            return -1;
    }
    if (*count == capacity) return -1;
    apic_ids[*count] = cpu->apic_id;
    acpi_uids[*count] = cpu->acpi_uid;
    (*count)++;
    return 1;
}

int apic_model_icr(enum apic_access_mode mode, uint32_t apic_id,
                   uint32_t low, uint64_t *value)
{
    if (!value) return -1;
    if (mode == APIC_ACCESS_X2APIC) {
        if (apic_id == UINT32_MAX) return -1; /* unicast API, no broadcast */
#ifdef LOGIT_X2APIC_NEGCTL_TRUNCATE_ICR
        apic_id &= 0xffu;
#endif
        *value = ((uint64_t)apic_id << 32) | low;
        return 0;
    }
    if (mode == APIC_ACCESS_XAPIC) {
        if (apic_id >= 0xffu) return -1;
        *value = ((uint64_t)apic_id << 56) | low;
        return 0;
    }
    return -1;
}

int apic_model_external_dest8(uint32_t apic_id, uint8_t *encoded)
{
#ifdef LOGIT_X2APIC_NEGCTL_TRUNCATE_EXTERNAL
    if (encoded) *encoded = (uint8_t)apic_id;
    return encoded ? 0 : -1;
#else
    if (!encoded || apic_id >= 0xffu) return -1;
    *encoded = (uint8_t)apic_id;
    return 0;
#endif
}

int apic_model_legacy_irq_uses_lapic(int cpu_index, int irq,
                                     int device_irqs_via_ioapic)
{
#ifdef LOGIT_X2APIC_NEGCTL_AP_TIMER_PIC_EOI
    (void)cpu_index;
    (void)irq;
    return !!device_irqs_via_ioapic;
#else
    if (cpu_index > 0 && irq == 0) return 1;
    return !!device_irqs_via_ioapic;
#endif
}
