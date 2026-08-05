#ifndef LOGIT_GDT_H
#define LOGIT_GDT_H

#include <stdint.h>

/* Segment selectors (index << 3 | RPL). */
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UCODE (0x18 | 3)
#define SEL_UDATA (0x20 | 3)
#define SEL_TSS   0x28

/* GDT/TSS layout (exported so percpu.h can embed a per-CPU TSS + GDT). */
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

/* Build the GDT (null, kernel code/data, user code/data, TSS) and load it
 * along with the task register. */
void gdt_init(void);

/* Set the kernel stack the CPU switches to on a ring 3 -> ring 0 trap. */
void tss_set_rsp0(uint64_t rsp0);

#endif /* LOGIT_GDT_H */
