/* Compile the TARGET address-conversion branch on the host. No privileged
 * helper is called: this catches changing mm_p2v to high aliases for old DMA
 * buffers, which MM_HOSTTEST's offset arena intentionally cannot observe. */
#include <stdio.h>
#include <stdint.h>
#include "mmhost.h"
int main(void)
{
    int failures = 0;
#define CHECK(x) do { if (!(x)) { puts("FAIL: " #x); failures++; } } while (0)
    CHECK((uintptr_t)mm_p2v(0x123456) == 0x123456);
    CHECK((uintptr_t)mm_p2v(PMM_LOW_LIMIT) == PHYSMAP_BASE + PMM_LOW_LIMIT);
    CHECK((uintptr_t)mm_p2v(0x100001234ull) == PHYSMAP_BASE + 0x100001234ull);
    CHECK(mm_p2v(PHYSMAP_SIZE) == NULL);
    CHECK((uintptr_t)mm_physmap_ptr(0x123456) == PHYSMAP_BASE + 0x123456);
    CHECK((uintptr_t)mm_physmap_ptr(0x100001234ull) == PHYSMAP_BASE + 0x100001234ull);
    CHECK(mm_physmap_ptr(PHYSMAP_SIZE) == NULL);
    CHECK(mm_v2p(mm_physmap_ptr(0x123456)) == 0x123456);
    CHECK(mm_v2p((void *)(uintptr_t)0x123456) == 0x123456);
    CHECK(mm_v2p(mm_p2v(0x100001234ull)) == 0x100001234ull);
    CHECK(mm_v2p((void *)(uintptr_t)PMM_LOW_LIMIT) == MM_PHYS_INVALID);
    CHECK(mm_v2p((void *)(uintptr_t)(PHYSMAP_BASE - 1)) == MM_PHYS_INVALID);
    CHECK(mm_v2p((void *)(uintptr_t)(PHYSMAP_BASE + PHYSMAP_SIZE)) == MM_PHYS_INVALID);
    printf("physmap aliases: %d failures\n", failures);
    return failures ? 1 : 0;
}
