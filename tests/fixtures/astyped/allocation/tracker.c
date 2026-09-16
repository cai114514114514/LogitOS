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
    assert(allocations > 0 && allocations == releases);
    puts("native allocation balance ok");
}

void *test_allocate(size_t count, size_t bytes)
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

void test_release(void *memory)
{
    assert(memory);
    releases++;
    free(memory);
}
