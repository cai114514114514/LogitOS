#ifndef AQUA_GDT_H
#define AQUA_GDT_H

#include <stdint.h>

/* Segment selectors (index << 3 | RPL). */
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UCODE (0x18 | 3)
#define SEL_UDATA (0x20 | 3)
#define SEL_TSS   0x28

/* Build the GDT (null, kernel code/data, user code/data, TSS) and load it
 * along with the task register. */
void gdt_init(void);

/* Set the kernel stack the CPU switches to on a ring 3 -> ring 0 trap. */
void tss_set_rsp0(uint64_t rsp0);

#endif /* AQUA_GDT_H */
