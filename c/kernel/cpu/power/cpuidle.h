#ifndef LOGIT_CPUIDLE_H
#define LOGIT_CPUIDLE_H
/* Init after firmware table publication, on each CPU with preemption off.
 * Enter requires IF=0 and returns IF=0, including its HLT fallback. */
void cpuidle_init(void);
void cpuidle_enter(void);
#endif
