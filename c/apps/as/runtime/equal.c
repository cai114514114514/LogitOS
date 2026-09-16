/* SPDX-License-Identifier: MIT */
#include "native.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const AtNativeType *type;
    const unsigned char *left;
    const unsigned char *right;
    int64_t index;
} Comparison;

typedef struct {
    Comparison *items;
    size_t count;
    size_t capacity;
    Comparison local[32];
} Comparisons;

static int push(Comparisons *work, Comparison comparison)
{
    if (work->count == work->capacity) {
        if (work->capacity > SIZE_MAX / (2 * sizeof(Comparison))) {
            return 0;
        }
        size_t capacity = work->capacity * 2;
        Comparison *items = malloc(capacity * sizeof *items);
        if (!items) {
            return 0;
        }
        memcpy(items, work->items, work->count * sizeof *items);
        if (work->items != work->local) {
            free(work->items);
        }
        work->items = items;
        work->capacity = capacity;
    }
    work->items[work->count++] = comparison;
    return 1;
}

static int scalar_equal(const AtNativeType *type, const void *left, const void *right)
{
    if (type->kind == AT_NATIVE_NONE) {
        return 1;
    }
    if (type->kind == AT_NATIVE_TEXT) {
        AtNativeText a, b;
        memcpy(&a, left, sizeof a);
        memcpy(&b, right, sizeof b);
        return a.length == b.length && (!a.length || !memcmp(a.data, b.data, (size_t)a.length));
    }
    if (type->kind == AT_NATIVE_BYTES) {
        const AtBytes *a;
        const AtBytes *b;
        memcpy(&a, left, sizeof a);
        memcpy(&b, right, sizeof b);
        return at_bytes_equal(a, b);
    }
    if (type->kind == AT_NATIVE_FLOAT) {
        if (type->bits == 32) {
            float a, b;
            memcpy(&a, left, sizeof a);
            memcpy(&b, right, sizeof b);
            return a == b;
        }
        double a, b;
        memcpy(&a, left, sizeof a);
        memcpy(&b, right, sizeof b);
        return a == b;
    }
    if (type->kind == AT_NATIVE_BOOL) {
        return (*(const unsigned char *)left & 1) == (*(const unsigned char *)right & 1);
    }
    /* Integers have no padding. List/Dict are identity pointers; Callable
     * includes both code and environment. Composite values are walked below,
     * rather than comparing their uninitialized padding bytes. */
    return !memcmp(left, right, (size_t)type->bytes);
}

static int compare(Comparisons *work)
{
    while (work->count) {
        Comparison *current = &work->items[work->count - 1];
        const AtNativeType *type = current->type;
        if (type->kind == AT_NATIVE_OPTIONAL) {
            int left = *current->left & 1, right = *current->right & 1;
            if (left != right) {
                return 0;
            }
            if (!left) {
                work->count--;
                continue;
            }
            int64_t offset = type->fields[0].offset;
            *current =
                (Comparison){type->element, current->left + offset, current->right + offset, 0};
            continue;
        }
        if (type->kind == AT_NATIVE_ANY) {
            AtAny *left, *right;
            memcpy(&left, current->left, sizeof left);
            memcpy(&right, current->right, sizeof right);
            if (at_any_type(left) != at_any_type(right)) {
                return 0;
            }
            *current = (Comparison){at_any_type(left), at_any_value(left), at_any_value(right), 0};
            continue;
        }
        if (type->kind == AT_NATIVE_STRUCT || type->kind == AT_NATIVE_EXCEPTION) {
            if (current->index == type->count) {
                work->count--;
                continue;
            }
            const AtNativeField *field = &type->fields[current->index++];
            Comparison next = {field->type, current->left + field->offset,
                               current->right + field->offset, 0};
            if (!push(work, next)) {
                return -1;
            }
            continue;
        }
        if (type->kind == AT_NATIVE_ARRAY || type->kind == AT_NATIVE_SLICE) {
            const unsigned char *left = current->left, *right = current->right;
            int64_t count = type->count;
            if (type->kind == AT_NATIVE_SLICE) {
                struct {
                    const unsigned char *data;
                    int64_t count;
                } a, b;

                memcpy(&a, left, sizeof a);
                memcpy(&b, right, sizeof b);
                if (a.count != b.count) {
                    return 0;
                }
                left = a.data;
                right = b.data;
                count = a.count;
            }
            if (current->index == count) {
                work->count--;
                continue;
            }
            int64_t offset = current->index++ * type->element->bytes;
            Comparison next = {type->element, left + offset, right + offset, 0};
            if (!push(work, next)) {
                return -1;
            }
            continue;
        }
        if (!scalar_equal(type, current->left, current->right)) {
            return 0;
        }
        work->count--;
    }
    return 1;
}

int at_any_equal(AtAny *left, AtAny *right)
{
    if (at_any_type(left) != at_any_type(right)) {
        return 0;
    }
    /* A deep chain of boxed value records must not consume the C call stack.
     * The explicit work stack uses malloc, never a GC safepoint. Managed
     * collections compare identity, so cyclic List/Dict graphs terminate. */
    Comparisons work = {0};
    work.items = work.local;
    work.capacity = sizeof work.local / sizeof *work.local;
    push(&work, (Comparison){at_any_type(left), at_any_value(left), at_any_value(right), 0});
    int equal = compare(&work);
    if (work.items != work.local) {
        free(work.items);
    }
    return equal;
}
