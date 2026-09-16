/* SPDX-License-Identifier: MIT */
#include "allocation.h"
#include "capability.h"
#include "exception.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static int fail_next;
static int allocated;
static int released;

/* Only allocation.c is redirected to these hooks. Counts therefore observe
 * real manual ownership, independently of capability/GC runtime allocations. */
void *test_allocate(size_t count, size_t bytes)
{
    if (fail_next) {
        fail_next = 0;
        return NULL;
    }
    void *result = calloc(count, bytes);
    if (result) {
        allocated++;
    }
    return result;
}

void test_release(void *memory)
{
    assert(memory);
    released++;
    free(memory);
}

int main(void)
{
    uint64_t first = 99;
    uint64_t middle = 0;
    uint64_t last = 0;
    at_caps_set(AS_CAP_RAW, NULL);
    assert(at_manual_alloc(&first, 0) == AT_E_VALUE && first == 0);
    assert(at_manual_alloc(&first, -1) == AT_E_VALUE && first == 0);
    assert(allocated == 0);

    fail_next = 1;
    assert(at_manual_alloc(&first, 16) == AT_E_MEMORY && first == 0);
    assert(allocated == 0 && released == 0);
    assert(at_manual_alloc(&first, 16) == 0);
    assert(at_manual_alloc(&middle, 32) == 0);
    assert(at_manual_alloc(&last, 48) == 0);
    unsigned char *bytes = (unsigned char *)(uintptr_t)middle;
    for (int index = 0; index < 32; index++) {
        assert(bytes[index] == 0);
    }
    bytes[31] = 253;

    at_caps_set(0, NULL);
    uint64_t denied = 99;
    assert(at_manual_alloc(&denied, 8) == AT_E_PERMISSION && denied == 0);
    assert(at_manual_dealloc(middle) == AT_E_PERMISSION);
    assert(released == 0 && bytes[31] == 253);
    at_caps_set(AS_CAP_RAW, NULL);

    assert(at_manual_dealloc(middle + 1) == AT_E_VALUE);
    assert(at_manual_dealloc(0) == AT_E_VALUE);
    /* Removal from the middle must keep both remaining registry links live. */
    assert(at_manual_dealloc(middle) == 0);
    assert(at_manual_dealloc(middle) == AT_E_VALUE);
    assert(at_manual_dealloc(first) == 0);
    assert(at_manual_dealloc(last) == 0);
    assert(allocated == 3 && released == 3);

    for (int iteration = 0; iteration < 128; iteration++) {
        assert(at_manual_alloc(&first, 8) == 0);
        assert(at_manual_dealloc(first) == 0);
    }
    assert(allocated == released);
    puts("native allocation runtime ok");
    return 0;
}
