/* SPDX-License-Identifier: MIT */
#include "native.h"
#include "system.h"

int at_system_call(int64_t *out, int64_t number, uint64_t a, uint64_t b, uint64_t c)
{
    *out = 0;
    /* Arbitrary syscall numbers bypass typed resource wrappers. Retain the
     * existing RAW requirement; the guest kernel additionally checks each
     * call's resource category and held path scope. Unsafe grants neither. */
    if (!at_caps_have(AS_CAP_RAW)) {
        return AT_E_PERMISSION;
    }
#if defined(__x86_64__) && !__STDC_HOSTED__
    __asm__ volatile("int $0x80" : "=a"(*out) : "a"(number), "D"(a), "S"(b), "d"(c) : "memory");
    return 0;
#else
    (void)number;
    (void)a;
    (void)b;
    (void)c;
    /* In particular, x86_64 Linux int 0x80 is a DIFFERENT syscall table.
     * Returning a fabricated kernel result would hide a platform error. */
    return AT_E_RUNTIME;
#endif
}
