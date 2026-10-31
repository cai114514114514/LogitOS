#ifndef LOGIT_CPU_POWER_FIRMWARE_H
#define LOGIT_CPU_POWER_FIRMWARE_H
/* Owner supplies the ACPI processor namespace path matched to this logical
 * CPU, never an enumeration-order guess. Call in serialized sleepable context
 * after uACPI namespace init, before that CPU's pstate_init/cpuidle_init.
 * Refresh/hotplug and _CST change notifications are deliberately unsupported;
 * quiesce the CPU before replacing its cached tables. */
int cpu_power_load_acpi(unsigned cpu, const char *processor_path);
#endif
