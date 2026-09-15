/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_MM_GUARD_H
#define LOGIT_MM_GUARD_H
#include <stdint.h>

/* Address-space operations may allocate, fault or sleep on I/O. A spinlock
 * around them would deadlock reclaim. These bounded, CR3-hashed sleeping
 * locks keep the PTE/VMA transaction intact; collisions only serialize two
 * spaces, never all kernel work. Reclaim may TRY several locks, never wait
 * for a second space while holding the first. A held CR3 also protects the
 * lifetime of pointers returned by vmm_pte(). */
struct mm_guard { unsigned slot; int held; };
struct mm_guard mm_guard_start(uint64_t cr3);
struct mm_guard mm_guard_try(uint64_t cr3);
void mm_guard_end(struct mm_guard *g);
/* Shared supervisor roots use a dedicated slot outside the AS hash. It may
 * sleep across page-table allocation. Allowed order: AS -> kernel owner;
 * never block on an AS while holding the kernel owner (reclaim only tries).
 * Before threads exist ownership uses the existing per-CPU boot fallback. */
struct mm_guard mm_kernel_guard_start(void);
struct mm_guard mm_kernel_guard_try(void);
#define MM_KERNEL_GUARD struct mm_guard _mmkg __attribute__((cleanup(mm_guard_end))) = mm_kernel_guard_start()

unsigned mm_guard_index(uint64_t cr3);
int mm_space_live(uint64_t cr3); /* caller holds its guard */
int mm_space_publish(uint64_t cr3);
void mm_space_retire(uint64_t cr3);
#define MM_GUARD(cr3) struct mm_guard _mmg __attribute__((cleanup(mm_guard_end))) = mm_guard_start(cr3)
#define MM_PAIR(a,b) \
    struct mm_guard _mmg0 __attribute__((cleanup(mm_guard_end))) = mm_guard_start(mm_guard_index(a)<=mm_guard_index(b)?(a):(b)); \
    struct mm_guard _mmg1 __attribute__((cleanup(mm_guard_end))) = mm_guard_start(mm_guard_index(a)<=mm_guard_index(b)?(b):(a))

/* Copy through owned RAM aliases, never through a checked-then-unmapped user
 * pointer. write=1 copies kernel -> user. The range must fit one user window. */
int vmm_copy_in_space(uint64_t cr3, void *kernel, uint64_t user_va, uint64_t len, int write);
int vmm_pin_user_page(uint64_t cr3, uint64_t va, int write, uint64_t *phys);
void vmm_flush_space(uint64_t cr3);
#endif
