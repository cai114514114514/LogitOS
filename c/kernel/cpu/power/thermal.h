#ifndef LOGIT_CPU_THERMAL_H
#define LOGIT_CPU_THERMAL_H
/* Sample locally on each initialized CPU; call the report on BSP after AP
 * samples have been published. RDMSR always describes the executing CPU.
 * Call samples with preemption disabled; do not sample a slot concurrently
 * with reporting. Reporting itself does not execute remote MSR reads. */
void cpu_power_sample(void);
void cpu_power_report(void);
#endif
