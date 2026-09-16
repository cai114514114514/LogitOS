/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

static int registered;
static size_t allocations;
static size_t releases;

static void verify_balance(void)
{
    /* A successful program that never ran a Region scope must fail too. This
     * observes actual allocator calls, not a counter maintained by cleanup. */
    assert(allocations > 0 && allocations == releases);
    puts("native region balance ok");
}

void *test_region_allocate(size_t count, size_t bytes)
{
    if (!registered) {
        assert(atexit(verify_balance) == 0);
        registered = 1;
    }
    void *memory = calloc(count, bytes);
    if (memory) {
        allocations++;
    }
    return memory;
}

void test_region_free(void *memory)
{
    assert(memory);
    releases++;
    free(memory);
}
