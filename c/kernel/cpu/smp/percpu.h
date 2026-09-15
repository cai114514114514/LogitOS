#ifndef LOGIT_PERCPU_H
#define LOGIT_PERCPU_H

#include <stdint.h>
#include "gdt.h"        /* struct tss, struct gdt_entry, struct gdt_ptr */

/* Max logical CPUs the scheduler tracks. ACPI already enumerates 32; keep the
 * per-CPU arrays at the same ceiling so a 12C/24T LGA2011 system is not
 * silently truncated after CPU7. The test-only branch proves that the 8-CPU
 * regression is visible to the high-core boot gate. */
#ifdef LOGIT_CPU_CAP_NEGCTL
#define PERCPU_MAXCPU 8
#else
#define PERCPU_MAXCPU 32
#endif

struct thread;     /* fwd: sched.c */

/* Per-CPU state. `current` and `tss` were single globals; they are now per-core
 * so two cores can run different threads with their own ring-0 stacks. */
struct cpu {
    int       index;          /* 0 = BSP */
    uint32_t  lapic_id;
    struct thread *current;   /* the thread running on THIS core */
    struct thread *idle;      /* this core's idle thread (hlt loop) */
    struct tss tss;           /* this core's TSS -> its own ring-0 rsp0 */
    struct gdt_entry gdt[7];  /* this core's GDT (null,kcode,kdata,ucode,udata,TSS) */
    struct gdt_ptr   gdtr;
    int       in_kernel;      /* CPU-local entry depth; NOT a cross-CPU lock */
    uint64_t  exit_discard;   /* per-cpu scratch for thread_exit's context_switch */
};

extern struct cpu g_cpus[PERCPU_MAXCPU];

struct cpu *this_cpu(void);                          /* find caller's struct cpu by LAPIC id */
void percpu_bsp_init(void);                          /* index 0: build+load BSP GDT/TSS */
void percpu_ap_init(int index, uint32_t lapic_id);   /* AP: build+load its GDT/TSS */
void percpu_register_id(int index, uint32_t lapic_id);
void percpu_tss_set_rsp0(uint64_t rsp0);             /* this_cpu()->tss.rsp0 */

#endif /* LOGIT_PERCPU_H */
