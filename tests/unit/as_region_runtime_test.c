/* SPDX-License-Identifier: MIT */
#include "region.h"
#include "native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int fail_next;
static int allocations;
static int releases;

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(expression)) {                                                                       \
            printf("FAIL region line %d: %s\n", __LINE__, #expression);                            \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

/* Instrument only the Region modules in a private test build. Production
 * runtime builds use libc directly. This proves release even on Apple Silicon,
 * where LeakSanitizer cannot report leaks at process exit. */
void *test_region_allocate(size_t count, size_t bytes)
{
    if (fail_next) {
        fail_next = 0;
        return NULL;
    }
    void *memory = calloc(count, bytes);
    if (memory) {
        allocations++;
    }
    return memory;
}

void test_region_free(void *memory)
{
    if (memory) {
        releases++;
    }
    free(memory);
}

static int owner_lifecycle(void)
{
    AtRegion *owner = NULL;
    AtRegion *destination = NULL;
    unsigned char *address = NULL;
    int64_t value = 99;
    CHECK(at_region_new(&owner, -1) == AT_E_VALUE && !owner);
    CHECK(at_region_new(&owner, AT_BUFFER_LIMIT + 1) == AT_E_VALUE && !owner);
    CHECK(at_region_new(&owner, 0) == 0 && owner);
    CHECK(at_region_length(&value, owner) == 0 && value == 0);
    CHECK(at_region_read(&value, owner, 0) == AT_E_INDEX && value == 0);
    CHECK(at_region_write(owner, -1, 1) == AT_E_INDEX);
    CHECK(at_region_release(&owner) == 0 && !owner);
    CHECK(at_region_release(&owner) == 0);

    int64_t managed_before = at_gc_live_bytes();
    CHECK(at_region_new(&owner, 16) == 0);
    CHECK(at_gc_live_bytes() == managed_before);
    for (int index = 0; index < 16; index++) {
        CHECK(at_region_read(&value, owner, index) == 0 && value == 0);
    }
    CHECK(at_region_write(owner, -1, 255) == 0);
    CHECK(at_region_write(owner, 0, 19) == 0);
    CHECK(at_region_write(owner, 0, -1) == AT_E_VALUE);
    CHECK(at_region_write(owner, 0, 256) == AT_E_VALUE);
    CHECK(at_region_read(&value, owner, INT64_MIN) == AT_E_INDEX);
    CHECK(at_region_read(&value, owner, INT64_MAX) == AT_E_INDEX);
    CHECK(at_region_read(&value, owner, 16) == AT_E_INDEX);
    at_gc_collect();
    CHECK(at_region_read(&value, owner, 0) == 0 && value == 19);
    CHECK(at_region_read(&value, owner, -1) == 0 && value == 255);
    CHECK(at_region_address(&address, owner, -1, 0) == 0 && *address == 255);
    CHECK(at_region_address(&address, owner, 16, 0) == AT_E_INDEX && !address);
    CHECK(at_region_address(&address, owner, INT64_MIN, 1) == AT_E_INDEX && !address);

    CHECK(at_region_move(&owner, &owner) == AT_E_RUNTIME);
    CHECK(at_region_new(&owner, 8) == AT_E_RUNTIME);
    CHECK(at_region_new(&destination, 4) == 0);
    CHECK(at_region_move(&destination, &owner) == AT_E_RUNTIME);
    CHECK(at_region_length(&value, destination) == 0 && value == 4);
    CHECK(at_region_release(&destination) == 0);
    CHECK(at_region_move(&destination, &owner) == 0 && !owner && destination);
    CHECK(at_region_read(&value, owner, 0) == AT_E_RUNTIME && value == 0);
    CHECK(at_region_length(&value, owner) == AT_E_RUNTIME);
    CHECK(at_region_write(owner, 0, 8) == AT_E_RUNTIME);
    CHECK(at_region_address(&address, owner, 0, 1) == AT_E_RUNTIME && !address);
    CHECK(at_region_release(&owner) == 0);
    CHECK(at_region_read(&value, destination, 0) == 0 && value == 19);
    CHECK(at_region_release(&destination) == 0);
    CHECK(at_region_move(&destination, &owner) == AT_E_RUNTIME);
    return 0;
}

static int shared_borrows(void)
{
    unsigned char *address = NULL;
    AtRegion *owner = NULL;
    AtRegion *destination = NULL;
    AtRegionBorrow *first = NULL;
    AtRegionBorrow *second = NULL;
    AtRegionBorrow *refused = NULL;
    int64_t value = 0;
    CHECK(at_region_new(&owner, 16) == 0);
    CHECK(at_region_write(owner, 2, 42) == 0);
    CHECK(at_region_borrow(&first, owner, -1, 8, 0) == AT_E_INDEX && !first);
    CHECK(at_region_borrow(&first, owner, 9, 8, 0) == AT_E_INDEX && !first);
    CHECK(at_region_borrow(&first, owner, 0, 17, 0) == AT_E_INDEX && !first);
    CHECK(at_region_borrow(&first, owner, 0, INT64_MAX, 0) == AT_E_INDEX && !first);
    CHECK(at_region_borrow(&first, owner, 0, 1, 2) == AT_E_RUNTIME && !first);
    CHECK(at_region_borrow(&first, owner, 2, 10, 0) == 0);
    CHECK(at_region_borrow(&first, owner, 0, 1, 0) == AT_E_RUNTIME);
    CHECK(at_region_borrow(&second, owner, 0, 16, 0) == 0);
    CHECK(at_region_borrow_length(&value, first) == 0 && value == 8);
    CHECK(at_region_borrow_read(&value, first, 0) == 0 && value == 42);
    CHECK(at_region_borrow_read(&value, first, -8) == 0 && value == 42);
    CHECK(at_region_borrow_read(&value, first, 8) == AT_E_INDEX);
    CHECK(at_region_borrow_read(&value, first, -9) == AT_E_INDEX);
    CHECK(at_region_borrow_read(&value, first, INT64_MIN) == AT_E_INDEX);
    CHECK(at_region_borrow_write(first, 0, 31) == AT_E_RUNTIME);
    CHECK(at_region_read(&value, owner, 2) == 0 && value == 42);
    CHECK(at_region_write(owner, 2, 31) == AT_E_RUNTIME);
    CHECK(at_region_address(&address, owner, 2, 0) == 0 && *address == 42);
    CHECK(at_region_address(&address, owner, 2, 1) == AT_E_RUNTIME && !address);
    CHECK(at_region_borrow(&refused, owner, 0, 1, 1) == AT_E_RUNTIME && !refused);
    CHECK(at_region_move(&destination, &owner) == AT_E_RUNTIME && !destination && owner);
    CHECK(at_region_release(&owner) == AT_E_RUNTIME && owner);
    CHECK(at_region_borrow_release(&first) == 0 && !first);
    CHECK(at_region_borrow_release(&first) == 0);
    CHECK(at_region_write(owner, 2, 31) == AT_E_RUNTIME);
    CHECK(at_region_borrow_release(&second) == 0);
    CHECK(at_region_write(owner, 2, 31) == 0);
    CHECK(at_region_borrow(&first, owner, 16, 16, 0) == 0);
    CHECK(at_region_borrow_length(&value, first) == 0 && value == 0);
    CHECK(at_region_borrow_read(&value, first, 0) == AT_E_INDEX);
    CHECK(at_region_borrow_release(&first) == 0);
    CHECK(at_region_release(&owner) == 0);
    CHECK(at_region_borrow(&first, owner, 0, 0, 0) == AT_E_RUNTIME);
    return 0;
}

static int mutable_reborrows(void)
{
    unsigned char *address = NULL;
    AtRegion *owner = NULL;
    AtRegionBorrow *parent = NULL;
    AtRegionBorrow *first = NULL;
    AtRegionBorrow *second = NULL;
    AtRegionBorrow *child = NULL;
    AtRegionBorrow *refused = NULL;
    int64_t value = 0;
    CHECK(at_region_new(&owner, 16) == 0);
    CHECK(at_region_borrow(&parent, owner, 2, 14, 1) == 0);
    CHECK(at_region_borrow_write(parent, 1, 51) == 0);
    CHECK(at_region_read(&value, owner, 3) == AT_E_RUNTIME);
    CHECK(at_region_write(owner, 3, 77) == AT_E_RUNTIME);
    CHECK(at_region_address(&address, owner, 3, 0) == AT_E_RUNTIME && !address);
    CHECK(at_region_address(&address, owner, 3, 1) == AT_E_RUNTIME && !address);
    CHECK(at_region_borrow(&refused, owner, 0, 1, 0) == AT_E_RUNTIME && !refused);
    CHECK(at_region_borrow(&refused, owner, 0, 1, 1) == AT_E_RUNTIME && !refused);
    CHECK(at_region_reborrow(&first, parent, 1, 5, 0) == 0);
    CHECK(at_region_reborrow(&second, parent, 4, 8, 0) == 0);
    CHECK(at_region_borrow_read(&value, parent, 1) == AT_E_RUNTIME);
    CHECK(at_region_borrow_write(parent, 1, 77) == AT_E_RUNTIME);
    CHECK(at_region_borrow_release(&parent) == AT_E_RUNTIME && parent);
    CHECK(at_region_borrow_read(&value, first, 0) == 0 && value == 51);
    CHECK(at_region_reborrow(&refused, first, 0, 1, 1) == AT_E_RUNTIME && !refused);
    CHECK(at_region_reborrow(&child, first, 0, 1, 0) == 0);
    CHECK(at_region_borrow_release(&first) == AT_E_RUNTIME && first);
    CHECK(at_region_borrow_read(&value, first, 0) == 0 && value == 51);
    CHECK(at_region_borrow_read(&value, child, 0) == 0 && value == 51);
    CHECK(at_region_borrow_release(&child) == 0);
    CHECK(at_region_borrow_release(&first) == 0);
    CHECK(at_region_borrow_release(&second) == 0);
    CHECK(at_region_borrow_read(&value, parent, 1) == 0 && value == 51);

    CHECK(at_region_reborrow(&first, parent, 0, 2, 1) == 0);
    CHECK(at_region_reborrow(&refused, parent, 0, 1, 0) == AT_E_RUNTIME && !refused);
    CHECK(at_region_reborrow(&refused, parent, 0, 1, 1) == AT_E_RUNTIME && !refused);
    CHECK(at_region_borrow_write(first, -1, 92) == 0);
    CHECK(at_region_borrow_write(first, 0, 256) == AT_E_VALUE);
    CHECK(at_region_borrow_read(&value, parent, 1) == AT_E_RUNTIME);
    CHECK(at_region_borrow_release(&first) == 0);
    CHECK(at_region_borrow_read(&value, parent, 1) == 0 && value == 92);
    CHECK(at_region_reborrow(&first, parent, 12, 12, 0) == 0);
    CHECK(at_region_borrow_length(&value, first) == 0 && value == 0);
    CHECK(at_region_borrow_release(&first) == 0);
    CHECK(at_region_borrow_release(&parent) == 0);
    CHECK(at_region_read(&value, owner, 3) == 0 && value == 92);
    CHECK(at_region_release(&owner) == 0);
    CHECK(at_region_borrow_read(&value, parent, 0) == AT_E_RUNTIME);
    CHECK(at_region_borrow_length(&value, parent) == AT_E_RUNTIME);
    CHECK(at_region_borrow_write(parent, 0, 0) == AT_E_RUNTIME);
    return 0;
}

static int allocation_failures(void)
{
    AtRegion *owner = NULL;
    AtRegionBorrow *parent = NULL;
    AtRegionBorrow *child = NULL;
    int64_t value = 0;
    fail_next = 1;
    CHECK(at_region_new(&owner, 8) == AT_E_MEMORY && !owner);
    CHECK(at_region_new(&owner, 8) == 0);
    fail_next = 1;
    CHECK(at_region_borrow(&parent, owner, 0, 8, 0) == AT_E_MEMORY && !parent);
    CHECK(at_region_write(owner, 0, 18) == 0);
    fail_next = 1;
    CHECK(at_region_borrow(&parent, owner, 0, 8, 1) == AT_E_MEMORY && !parent);
    CHECK(at_region_read(&value, owner, 0) == 0 && value == 18);
    CHECK(at_region_borrow(&parent, owner, 0, 8, 1) == 0);
    fail_next = 1;
    CHECK(at_region_reborrow(&child, parent, 0, 8, 1) == AT_E_MEMORY && !child);
    CHECK(at_region_borrow_write(parent, 0, 27) == 0);
    fail_next = 1;
    CHECK(at_region_reborrow(&child, parent, 0, 8, 0) == AT_E_MEMORY && !child);
    CHECK(at_region_borrow_read(&value, parent, 0) == 0 && value == 27);
    CHECK(at_region_borrow_release(&parent) == 0);
    CHECK(at_region_release(&owner) == 0);
    return 0;
}

int main(int argc, char **argv)
{
    int tracked = argc == 2 && !strcmp(argv[1], "--tracked");
    if (owner_lifecycle() || shared_borrows() || mutable_reborrows()) {
        return 1;
    }
    if (tracked) {
        CHECK(allocations > 0);
        if (allocation_failures()) {
            return 1;
        }
        CHECK(allocations == releases);
    }
    printf("native region runtime ok: %d checks, tracked=%d\n", checks, tracked);
    return 0;
}
