#ifndef LOGIT_ACPI_H
#define LOGIT_ACPI_H
#include <stdint.h>

#define ACPI_MAX_CPUS 32

/* Parse ACPI tables (RSDP -> MADT). Returns the number of enabled CPUs, or -1. */
int      acpi_init(void);
int      acpi_cpu_count(void);
uint8_t  acpi_cpu_apic_id(int i);
uint32_t acpi_lapic_base(void);
uint32_t acpi_ioapic_addr(void);          /* IOAPIC MMIO base (0 if none) */
uint32_t acpi_ioapic_gsibase(void);
uint32_t acpi_gsi_for_irq(int isa_irq);   /* ISA IRQ -> GSI (handles overrides) */
uint16_t acpi_gsi_flags(int isa_irq);     /* MPS INTI flags for the override */

#endif
