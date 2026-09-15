#ifndef LOGIT_APIC_MODEL_H
#define LOGIT_APIC_MODEL_H

#include <stddef.h>
#include <stdint.h>

/* Access mode selected from this CPU's CPUID.01H feature bits and
 * IA32_APIC_BASE.  Production deliberately follows the mode firmware handed
 * off; switching every CPU and the interrupt-remapping unit is a separate,
 * system-wide transition. */
enum apic_access_mode {
    APIC_ACCESS_NONE = 0,
    APIC_ACCESS_XAPIC,
    APIC_ACCESS_X2APIC,
};

enum apic_access_mode apic_model_access_mode(uint32_t cpuid1_ecx,
                                              uint32_t cpuid1_edx,
                                              uint64_t apic_base);

/* One enabled CPU record decoded from a MADT subtable.  Return 1 for an
 * enabled type-0/type-9 processor, 0 for a disabled or unrelated record, and
 * -1 for a malformed processor record. */
struct apic_madt_cpu {
    uint32_t apic_id;
    uint32_t acpi_uid;
    uint8_t  source_type;
};
int apic_model_madt_cpu(const uint8_t *entry, size_t available,
                        struct apic_madt_cpu *cpu);

/* Insert one decoded record into a bounded MADT CPU set.  The ACPI processor
 * UID and APIC ID jointly identify the processor: an exact type-0/type-9
 * repeat is a duplicate, while either field being reused for a different
 * partner is a firmware conflict and is rejected.  Return 1 when inserted,
 * 0 for an exact duplicate, and -1 for conflict/full/invalid input. */
int apic_model_add_madt_cpu(const struct apic_madt_cpu *cpu,
                            uint32_t *apic_ids, uint32_t *acpi_uids,
                            size_t *count, size_t capacity);

/* Compose a physical, no-shorthand ICR write.  In xAPIC mode `value` is the
 * two MMIO halves packed as ICRHI:ICRLO; in x2APIC mode it is MSR 0x830.
 * A destination that the selected mode cannot address is rejected. */
int apic_model_icr(enum apic_access_mode mode, uint32_t apic_id,
                   uint32_t low, uint64_t *value);

/* Legacy IOAPIC/MSI destination fields are only eight bits.  APIC ID 0xff is
 * reserved in that address space, so it and every wider ID require interrupt
 * remapping that LogitOS does not yet configure. */
int apic_model_external_dest8(uint32_t apic_id, uint8_t *encoded);

/* Vector 32 is the LAPIC timer on APs even while the BSP keeps legacy PIC
 * device routing. Other legacy vectors follow the BSP's PIC/IOAPIC policy. */
int apic_model_legacy_irq_uses_lapic(int cpu_index, int irq,
                                     int device_irqs_via_ioapic);

#endif
