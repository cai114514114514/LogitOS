#include <stdint.h>
#include <stddef.h>
#include "gdt.h"
#include "percpu.h"

/* The GDT/TSS now live per-CPU (struct cpu in percpu.{h,c}). gdt_init builds the
 * BSP's; AP GDTs are built in percpu_ap_init from smp.c. set_seg/set_tss_desc and
 * the per-core syscall stacks moved to percpu.c. */

void gdt_init(void)
{
    percpu_bsp_init();
}

void tss_set_rsp0(uint64_t rsp0)
{
    percpu_tss_set_rsp0(rsp0);
}
