#include <stdint.h>
#include <stddef.h>
#include "usercopy.h"
#include "sched.h"
#include "vmm.h"

void *memcpy(void *, const void *, size_t);

int user_range_ok(const void *ptr, uint64_t len, int write)
{
    return vmm_user_range_ok(sched_current_cr3(), ptr, len, write);
}

int user_copy_from(void *dst, const void *src, uint64_t len)
{
    if (!dst) return -1;
    if (len == 0) return 0;
    if (!user_range_ok(src, len, 0)) return -1;
    memcpy(dst, src, (size_t)len);
    return 0;
}

int user_copy_to(void *dst, const void *src, uint64_t len)
{
    if (!src) return -1;
    if (len == 0) return 0;
    if (!user_range_ok(dst, len, 1)) return -1;
    memcpy(dst, src, (size_t)len);
    return 0;
}

int user_copy_string(char *dst, int max, const char *src)
{
    if (!dst || max <= 0) return -1;
    for (int i = 0; i < max; i++) {
        if (!user_range_ok(src + i, 1, 0)) return -1;
        dst[i] = src[i];
        if (dst[i] == 0) return i;
    }
    dst[max - 1] = 0;
    return -1;
}
