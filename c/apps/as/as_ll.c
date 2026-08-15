#include "as.h"

/* Low-level indirection bridge. peek/poke are plain machine-word memory access
 * (work on host and Logit alike). The syscall path is the asm bridge to Logit's
 * int 0x80 ABI -- compiled only for the freestanding x86_64 target; on hosted
 * builds (arm64 macOS, x86_64 Linux) it stubs to -1 so the portable core still
 * builds & tests. The __STDC_HOSTED__ guard matters on x86_64 Linux, where a
 * real int 0x80 would trap into the i386 compat syscall table (-ENOSYS).
 *
 * DELIBERATELY NO CAP_RAW CHECK IN THIS FILE (M28 spec D4/D5). The three
 * functions below are the ONLY place that would look like the "real"
 * enforcement point -- they are the actual dereference / the actual trap --
 * but putting the gate here is wrong for a reason specific to this file: every
 * one of these functions is REAL on the host build too (that is what makes
 * peek/poke/syscall testable without QEMU at all). A script denied CAP_RAW
 * that still reached as_ll_peek would SIGSEGV the host test binary instead of
 * failing with a clean, catchable message -- the crash would look like a bug
 * in the interpreter, not a security check doing its job. The gate lives one
 * layer up, in as_native.c's peek_w/poke_w/ptr_w/n_mem2str/n_mem2cstr/
 * n_syscall (each calls require_raw() before touching anything below), where
 * a denial can still return a Value and unwind through as_native_fail. If you
 * are looking at this file wondering where the capability check is: it is not
 * missing, it is one file up, on purpose. Do not add one here -- a second
 * check here would be redundant on the path that already denies.
 *
 * as_native.c is not literally the only caller: vm.c's op_INDEX_GET/
 * op_INDEX_SET also call as_ll_peek/as_ll_poke directly, for `p[i]` on an
 * already-constructed ObjPtr (pre-M28, the A3 typed-pointer feature). That is
 * safe with NO check at the index site because as_ptr_new (object.c) has
 * exactly two callers, both in as_native.c -- ptr_w (i8ptr..i64ptr) and
 * n_alloc -- and both require_raw() before constructing one. Capabilities
 * here consume at ACQUISITION, not at use (the same rule as_port.c states for
 * its own five acquisition points): there is no ObjPtr a script can hold
 * without having already cleared CAP_RAW to make it, so indexing one needs no
 * second gate. That invariant lives in as_native.c/object.c, not here -- if a
 * future change gives ObjPtr a second constructor (a port, a `.la` tag, a
 * deserializer), THAT constructor must call require_raw or this paragraph
 * becomes wrong and op_INDEX_GET/SET becomes the fourth, ungated caller. */

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
    return -1;            /* host: Logit syscalls unavailable */
#endif
}
