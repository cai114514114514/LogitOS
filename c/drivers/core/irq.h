#ifndef LOGIT_DEV_IRQ_H
#define LOGIT_DEV_IRQ_H
/* Dynamic interrupt vectors for the device model.
 *
 * The kernel's static IDT (c/kernel/cpu/idt.c) only installs vectors 0..47 plus
 * a handful of hard-coded ones (65 = e1000, 128 = syscall, 240/241 = IPIs, 255
 * = spurious). A device model cannot hard-code a vector per driver, so this
 * module owns a pool of vectors 0x60..0x7F, their assembly stubs, and their IDT
 * gates -- which it installs by reading IDTR with `sidt`, so nothing outside
 * c/drivers/core has to change to add a device interrupt.
 *
 * Handlers run in interrupt context on whichever CPU the interrupt was routed
 * to (the BSP, in practice), with the BKL held, and EOI is sent for them. */

#include <stdint.h>
#include "driver.h"     /* irq_handler_t */

#define IRQ_VEC_BASE 0x60
#define IRQ_NVEC     32

/* Claim a vector and install `fn`. Returns the CPU vector (0x60..0x7F) or -1
 * when the pool is exhausted. Does NOT route anything -- the caller (MSI, MSI-X
 * or the I/O APIC path) programs the source to raise this vector. */
int      irq_alloc_vector(irq_handler_t fn, void *arg, const char *name);
void     irq_free_vector(int vec);

/* Number of times this vector has been delivered. The MSI boot test asserts on
 * this: a fallback path that has never actually run is not a fallback. */
uint64_t irq_vector_count(int vec);
const char *irq_vector_name(int vec);

/* Called from irq stubs. Public only because the stub needs the symbol. */
struct registers;
void irq_isr_entry(struct registers *r);

#endif /* LOGIT_DEV_IRQ_H */
