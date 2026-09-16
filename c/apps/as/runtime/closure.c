/* SPDX-License-Identifier: MIT */
#include "native.h"
#include <string.h>

typedef struct {
    int64_t count;
    void *cells[];
} AtEnvironment;

static void scan_environment(void *pointer)
{
    AtEnvironment *environment = pointer;
    for (int64_t index = 0; index < environment->count; index++) {
        at_gc_mark(environment->cells[index]);
    }
}

void *at_closure_new(int64_t count, void *const *cells)
{
    if (count < 0 || (uint64_t)count > (SIZE_MAX - sizeof(AtEnvironment)) / sizeof(void *)) {
        return NULL;
    }
    size_t bytes = sizeof(AtEnvironment) + (size_t)count * sizeof(void *);
    AtEnvironment *environment = at_gc_allocate(bytes, scan_environment);
    if (environment) {
        environment->count = count;
        if (count) {
            memcpy(environment->cells, cells, (size_t)count * sizeof(void *));
        }
    }
    return environment;
}

void *at_closure_cell(void *pointer, int64_t index)
{
    AtEnvironment *environment = pointer;
    if (!environment || index < 0 || index >= environment->count) {
        return NULL;
    }
    return environment->cells[index];
}
