#include <stdint.h>
#include <stddef.h>
#include "gdt.h"
#include "percpu.h"
#include "prot.h"

/* The GDT/TSS now live per-CPU (struct cpu in percpu.{h,c}). gdt_init builds the
 * BSP's; AP GDTs are built in percpu_ap_init from smp.c. set_seg/set_tss_desc and
 * the per-core syscall stacks moved to percpu.c. */

void gdt_init(void)
{
    percpu_bsp_init();
    /* Report the BSP's ring-3 protection bits here rather than from kernel_main:
     * gdt_init is the point at which ring 3 becomes describable at all (this is
     * where the user code/data selectors and the TSS come from), serial is
     * already up, and this is a file this line owns. The bits themselves were
     * set far earlier, in c/boot/long.asm -- see prot.h for why they cannot
     * wait until C. */
    cpu_prot_report("bsp");
}

void tss_set_rsp0(uint64_t rsp0)
{
    percpu_tss_set_rsp0(rsp0);
}
