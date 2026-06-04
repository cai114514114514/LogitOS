#ifndef AQUA_ACPI_H
#define AQUA_ACPI_H
#include <stdint.h>

#define ACPI_MAX_CPUS 32

/* Parse ACPI tables (RSDP -> MADT). Returns the number of enabled CPUs, or -1. */
int      acpi_init(void);
int      acpi_cpu_count(void);
uint8_t  acpi_cpu_apic_id(int i);
uint32_t acpi_lapic_base(void);

#endif
