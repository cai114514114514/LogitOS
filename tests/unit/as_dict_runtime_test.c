/* SPDX-License-Identifier: MIT */
#include "native.h"
#include <assert.h>
#include <stdio.h>

/* Force every key into one probe chain. Ordinary source fixtures cannot
 * reliably exercise hash collisions just by choosing a few integer keys. */
static uint64_t collision(const void *key)
{
    (void)key;
    return 0;
}

static int equal_integer(const void *left, const void *right)
{
    return *(const int64_t *)left == *(const int64_t *)right;
}

static void scan_pointer(void *slot)
{
    at_gc_mark(*(void **)slot);
}

static void exercise(void)
{
    void *frame = at_gc_frame();
    AtRoot dict_root;
    AtDict *dict = at_dict_new(8, 8, NULL, NULL, collision, equal_integer);
    assert(dict);
    at_gc_root(&dict_root, &dict, scan_pointer);
    for (int64_t key = 0; key < 400; key++) {
        int64_t value = key * 3;
        assert(at_dict_set(dict, &key, &value));
    }
    for (int64_t key = 0; key < 400; key += 2) {
        assert(at_dict_remove(dict, &key));
        assert(!at_dict_remove(dict, &key));
    }
    at_gc_collect();
    for (int64_t key = 1; key < 400; key += 2) {
        int64_t *value = at_dict_get(dict, &key);
        assert(value && *value == key * 3);
    }
    for (int64_t key = 0; key < 400; key += 2) {
        int64_t value = -key;
        assert(at_dict_set(dict, &key, &value));
    }
    assert(at_dict_len(dict) == 400);
    for (int64_t key = 0; key < 400; key++) {
        int64_t *value = at_dict_get(dict, &key);
        assert(value && *value == (key % 2 ? key * 3 : -key));
    }
    for (int64_t key = 0; key < 400; key++) {
        assert(at_dict_remove(dict, &key));
    }
    int64_t missing = -1;
    assert(!at_dict_get(dict, &missing));
    assert(at_dict_set(dict, &missing, &missing));
    assert(at_dict_len(dict) == 1);

    /* A zero-byte value still has an address and survives values() snapshots.
     * Padding it and copying a fake byte would read beyond its LLVM value. */
    AtRoot empty_root;
    AtDict *empty = at_dict_new(8, 0, NULL, NULL, collision, equal_integer);
    assert(empty);
    at_gc_root(&empty_root, &empty, scan_pointer);
    assert(at_dict_set(empty, &missing, &missing));
    assert(at_dict_get(empty, &missing));
    AtList *values = at_dict_values(empty);
    assert(values && at_list_len(values) == 1 && at_list_at(values, 0));
    assert(!at_list_at(values, 1));
    at_gc_restore(frame);
}

int main(void)
{
    exercise();
    at_gc_collect();
    assert(at_gc_live_bytes() == 0);
    puts("dictionary collision chains ok");
    return 0;
}
