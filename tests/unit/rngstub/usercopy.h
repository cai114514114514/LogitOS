#ifndef LOGIT_USERCOPY_STUB_H
#define LOGIT_USERCOPY_STUB_H

/* Host stub for c/kernel/exec/usercopy.h, so tests/unit/rng_test.c can build
 * c/kernel/core/rng.c without the memory manager.
 *
 * rng.c pulled this in when it grew SYS_GETRANDOM: the syscall bounds-checks
 * the caller's buffer before generating into it. The DRBG itself -- which is
 * all rng_test.c exercises -- does not touch either function, so the stubs are
 * deliberately trivial and deliberately PERMISSIVE: a stub that refused would
 * silently turn a real regression in the generator into a passing test that
 * never ran. The syscall's own bounds behaviour is asserted where it can be
 * asserted for real, on the machine, by tests/boot/run-entropy-test.sh (the
 * ENT_BADPTR case). */
#include <stdint.h>
#include <string.h>

static inline int user_range_ok(const void *ptr, uint64_t len, int write)
{ (void)len; (void)write; return ptr != 0; }

static inline int user_copy_to(void *dst, const void *src, uint64_t len)
{ memcpy(dst, src, (size_t)len); return 0; }

static inline int user_copy_from(void *dst, const void *src, uint64_t len)
{ memcpy(dst, src, (size_t)len); return 0; }

#endif
