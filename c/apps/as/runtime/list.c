/* SPDX-License-Identifier: MIT */
/* Typed contiguous list storage. The object identity survives buffer growth. */
#include "native.h"
#include <string.h>

static void scan_list(void *value)
{
    AtList *list = value;
    at_gc_mark(list->data);
    if (list->element_scan) {
        for (int64_t i = 0; i < list->count; i++) {
            list->element_scan(list->data + i * list->stride);
        }
    }
}

AtList *at_list_new(int64_t stride, AtScan element_scan)
{
    AtList *list = at_gc_allocate(sizeof(*list), scan_list);
    if (list) {
        list->stride = stride;
        list->element_scan = element_scan;
    }
    return list;
}

int64_t at_list_len(AtList *list)
{
    return list->count;
}

void *at_list_at(AtList *list, int64_t index)
{
    if (index < 0) {
        index += list->count;
    }
    if (index < 0 || index >= list->count) {
        return NULL;
    }
    return list->data + index * list->stride;
}

int at_list_append(AtList *list, const void *value)
{
    if (list->stride == 0) {
        /* Empty structs have identity-free, zero-byte values. A real sentinel
         * distinguishes a valid element address from an out-of-range NULL. */
        if (list->count == INT64_MAX) {
            return 0;
        }
        if (!list->data) {
            list->data = at_gc_allocate(1, NULL);
            if (!list->data) {
                return 0;
            }
        }
        list->count++;
        return 1;
    }
    if (list->count == list->capacity) {
        if (list->capacity > INT64_MAX / 2) {
            return 0;
        }
        int64_t capacity = list->capacity ? list->capacity * 2 : 4;
        if (list->stride <= 0 || capacity > INT64_MAX / list->stride) {
            return 0;
        }
        unsigned char *data = at_gc_allocate((size_t)(capacity * list->stride), NULL);
        if (!data) {
            return 0;
        }
        if (list->count) {
            memcpy(data, list->data, (size_t)(list->count * list->stride));
        }
        list->data = data;
        list->capacity = capacity;
    }
    memcpy(list->data + list->count * list->stride, value, (size_t)list->stride);
    list->count++;
    return 1;
}
