#include "as.h"

/* Low-level indirection bridge. peek/poke are plain machine-word memory access
 * (work on host and Aether alike). The syscall path is the asm bridge to Aether's
 * int 0x80 ABI -- compiled only for the freestanding x86_64 target; on hosted
 * builds (arm64 macOS, x86_64 Linux) it stubs to -1 so the portable core still
 * builds & tests. The __STDC_HOSTED__ guard matters on x86_64 Linux, where a
 * real int 0x80 would trap into the i386 compat syscall table (-ENOSYS). */

uint64_t as_ll_peek(uint64_t addr, int width)
{
    uintptr_t a = (uintptr_t)addr;
    switch (width) {
    case 1: return *(volatile uint8_t  *)a;
    case 2: return *(volatile uint16_t *)a;
    case 4: return *(volatile uint32_t *)a;
    default: return *(volatile uint64_t *)a;
    }
}

void as_ll_poke(uint64_t addr, int width, uint64_t val)
{
    uintptr_t a = (uintptr_t)addr;
    switch (width) {
    case 1: *(volatile uint8_t  *)a = (uint8_t)val; break;
    case 2: *(volatile uint16_t *)a = (uint16_t)val; break;
    case 4: *(volatile uint32_t *)a = (uint32_t)val; break;
    default: *(volatile uint64_t *)a = val; break;
    }
}

long as_ll_syscall(long n, long a, long b, long c)
{
#if defined(__x86_64__) && !defined(__APPLE__) && !__STDC_HOSTED__
    long r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
    return r;
#else
    (void)n; (void)a; (void)b; (void)c;
    return -1;            /* host: Aether syscalls unavailable */
#endif
}
