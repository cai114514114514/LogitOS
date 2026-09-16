/* SPDX-License-Identifier: MIT
 * Observe the real allocator's private count without exporting a diagnostic
 * accessor in shipped libc. Call only after all allocator threads join. */
/* The test supplies strong locking providers. Suppress the Mach-O fallback
 * asm definitions, which its LTO linker otherwise diagnoses as duplicates. */
#define LOGIT_WEAK_LOCAL___libc_lock 1
#define LOGIT_WEAK_LOCAL___libc_unlock 1
#include "../../c/apps/libc/src/malloc.c"
size_t sized_test_live_bytes(void) { return malloc_cur; }
void sized_test_flush(void)
{
#ifdef MALLOC_SMALL_CACHE
    small_flush();
#endif
}
unsigned sized_test_cached(void)
{
#ifdef MALLOC_SMALL_CACHE
    return small_total;
#else
    return 0;
#endif
}
size_t sized_test_limit(size_t limit)
{size_t old=arena_limit;arena_limit=(uint32_t)limit;return old;}
