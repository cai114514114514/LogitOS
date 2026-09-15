#ifndef LOGIT_USERCOPY_H
#define LOGIT_USERCOPY_H

#include <stdint.h>

/* Make `ptr[0..len)` usable by the kernel for read (or write) access in the
 * CURRENT address space, and say whether it is legitimately the process's.
 *
 * NOT a pure predicate: under copy-on-write a logically writable user page is
 * physically read-only, and an mmap'd page may not be mapped at all yet, so
 * this resolves both before returning 1 -- the caller may then read or write
 * the range with a plain memcpy. See the header comment in usercopy.c for why
 * that shape (and not a relaxed check) is the one that is not Dirty COW. */
int user_range_ok(const void *ptr, uint64_t len, int write);

/* The pure predicate: is this range mapped RIGHT NOW, with these permissions?
 * Changes nothing. For diagnostics and assertions, not for syscall entry. */
int user_range_mapped(const void *ptr, uint64_t len, int write);
int user_copy_from(void *dst, const void *src, uint64_t len);
int user_copy_to(void *dst, const void *src, uint64_t len);
int user_copy_string(char *dst, int max, const char *src);

/* Pin a naturally aligned 32-bit futex word before taking its bucket lock.
 * The CPU alias survives unmap; release after enqueue, before sleeping. */
int user_pin_word(const void *ptr, uint64_t *phys, const void **cpu);
void user_unpin_word(uint64_t phys);
#endif /* LOGIT_USERCOPY_H */
