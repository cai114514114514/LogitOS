#ifndef LOGIT_POWER_ACPI_NATIVE_H
#define LOGIT_POWER_ACPI_NATIVE_H
/* Call in sleepable context after sched_init (which starts kworker), clocks,
 * ACPI table discovery, allocator and IOAPIC setup. Confirms actual deferred
 * worker execution before making AML kernel services available. */
int power_acpi_native_start(void);
#endif
