/* SPDX-License-Identifier: MIT */
#include "native.h"
#include <string.h>

/* Open addressing keeps expected lookup cost independent of element count.
 * Each bucket is [hash, padded native key, padded native value]. Hash 0 is
 * empty, 1 is deleted; live hashes are normalized away from these sentinels.
 * No universal Value representation is used for either keys or values. */
struct AtDict {
    int64_t count;
    int64_t used;
    int64_t capacity;
    int64_t key_bytes;
    int64_t value_bytes;
    int64_t value_offset;
    int64_t stride;
    AtScan key_scan;
    AtScan value_scan;
    AtHash hash;
    AtEqual equal;
    unsigned char *data;
};

static uint64_t bucket_hash(const unsigned char *bucket)
{
    uint64_t hash;
    memcpy(&hash, bucket, sizeof hash);
    return hash;
}

static void store_hash(unsigned char *bucket, uint64_t hash)
{
    memcpy(bucket, &hash, sizeof hash);
}

static void scan_dict(void *pointer)
{
    AtDict *dict = pointer;
    at_gc_mark(dict->data);
    for (int64_t index = 0; index < dict->capacity; index++) {
        unsigned char *bucket = dict->data + index * dict->stride;
        if (bucket_hash(bucket) < 2) {
            continue;
        }
        if (dict->key_scan) {
            dict->key_scan(bucket + sizeof(uint64_t));
        }
        if (dict->value_scan) {
            dict->value_scan(bucket + dict->value_offset);
        }
    }
}

AtDict *at_dict_new(int64_t key_bytes, int64_t value_bytes, AtScan key_scan, AtScan value_scan,
                    AtHash hash, AtEqual equal)
{
    if (key_bytes < 0 || value_bytes < 0 || key_bytes > INT64_MAX - 15 ||
        value_bytes > INT64_MAX - 7 || !hash || !equal) {
        return NULL;
    }
    /* Present language values require at most eight-byte alignment. Round
     * each region independently so a one-byte key cannot misalign an f64. */
    int64_t value_offset = 8 + ((key_bytes + 7) & ~INT64_C(7));
    int64_t padded_value = (value_bytes + 7) & ~INT64_C(7);
    if (value_offset > INT64_MAX - padded_value) {
        return NULL;
    }
    AtDict *dict = at_gc_allocate(sizeof(*dict), scan_dict);
    if (dict) {
        dict->key_bytes = key_bytes;
        dict->value_bytes = value_bytes;
        dict->value_offset = value_offset;
        dict->stride = value_offset + padded_value;
        dict->key_scan = key_scan;
        dict->value_scan = value_scan;
        dict->hash = hash;
        dict->equal = equal;
    }
    return dict;
}

uint64_t at_hash_u64(uint64_t value)
{
    /* Unsigned multiplication intentionally mixes all bits modulo 2^64. */
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

uint64_t at_hash_text(const char *text, int64_t length)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (int64_t index = 0; index < length; index++) {
        hash ^= (unsigned char)text[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t key_hash(AtDict *dict, const void *key)
{
    uint64_t hash = dict->hash(key);
    return hash < 2 ? hash + 2 : hash;
}

static unsigned char *find_bucket(AtDict *dict, const void *key, uint64_t hash)
{
    if (!dict->capacity) {
        return NULL;
    }
    uint64_t mask = (uint64_t)dict->capacity - 1;
    uint64_t index = hash & mask;
    unsigned char *deleted = NULL;
    for (int64_t probe = 0; probe < dict->capacity; probe++) {
        unsigned char *bucket = dict->data + index * (uint64_t)dict->stride;
        uint64_t stored = bucket_hash(bucket);
        if (!stored) {
            return deleted ? deleted : bucket;
        }
        if (stored == 1) {
            if (!deleted) {
                deleted = bucket;
            }
        } else if (stored == hash && dict->equal(bucket + sizeof(uint64_t), key)) {
            return bucket;
        }
        index = (index + 1) & mask;
    }
    return deleted;
}

static int resize(AtDict *dict, int64_t capacity)
{
    if (capacity <= 0 || capacity > INT64_MAX / dict->stride) {
        return 0;
    }
    unsigned char *data = at_gc_allocate((size_t)(capacity * dict->stride), NULL);
    if (!data) {
        return 0;
    }
    /* Nothing below allocates. Publish only after every live bucket is copied;
     * the old table remains reachable through dict at the allocation safepoint. */
    uint64_t mask = (uint64_t)capacity - 1;
    for (int64_t old = 0; old < dict->capacity; old++) {
        unsigned char *bucket = dict->data + old * dict->stride;
        uint64_t hash = bucket_hash(bucket);
        if (hash < 2) {
            continue;
        }
        uint64_t index = hash & mask;
        while (bucket_hash(data + index * (uint64_t)dict->stride)) {
            index = (index + 1) & mask;
        }
        memcpy(data + index * (uint64_t)dict->stride, bucket, (size_t)dict->stride);
    }
    dict->data = data;
    dict->capacity = capacity;
    dict->used = dict->count;
    return 1;
}

int64_t at_dict_len(AtDict *dict)
{
    return dict->count;
}

void *at_dict_get(AtDict *dict, const void *key)
{
    unsigned char *bucket = find_bucket(dict, key, key_hash(dict, key));
    return bucket && bucket_hash(bucket) >= 2 ? bucket + dict->value_offset : NULL;
}

int at_dict_set(AtDict *dict, const void *key, const void *value)
{
    uint64_t hash = key_hash(dict, key);
    unsigned char *bucket = find_bucket(dict, key, hash);
    if (bucket && bucket_hash(bucket) >= 2) {
        memcpy(bucket + dict->value_offset, value, (size_t)dict->value_bytes);
        return 1;
    }
    if (!dict->capacity || dict->used >= dict->capacity - dict->capacity / 4) {
        if (dict->capacity > INT64_MAX / 2) {
            return 0;
        }
        int64_t capacity = !dict->capacity ? 8 : dict->capacity;
        if (dict->count >= capacity / 2) {
            capacity *= 2;
        }
        if (!resize(dict, capacity)) {
            return 0;
        }
        bucket = find_bucket(dict, key, hash);
    }
    if (!bucket) {
        return 0;
    }
    dict->used += bucket_hash(bucket) == 0;
    memcpy(bucket + sizeof(uint64_t), key, (size_t)dict->key_bytes);
    memcpy(bucket + dict->value_offset, value, (size_t)dict->value_bytes);
    store_hash(bucket, hash);
    dict->count++;
    return 1;
}

int at_dict_remove(AtDict *dict, const void *key)
{
    unsigned char *bucket = find_bucket(dict, key, key_hash(dict, key));
    if (!bucket || bucket_hash(bucket) < 2) {
        return 0;
    }
    memset(bucket + sizeof(uint64_t), 0, (size_t)dict->stride - sizeof(uint64_t));
    store_hash(bucket, 1);
    dict->count--;
    return 1;
}

static void scan_list_slot(void *slot)
{
    at_gc_mark(*(AtList **)slot);
}

static AtList *snapshot(AtDict *dict, int keys)
{
    int64_t bytes = keys ? dict->key_bytes : dict->value_bytes;
    AtScan scan = keys ? dict->key_scan : dict->value_scan;
    AtList *list = at_list_new(bytes, scan);
    if (!list) {
        return NULL;
    }
    void *frame = at_gc_frame();
    AtRoot root;
    at_gc_root(&root, &list, scan_list_slot);
    /* Iteration, keys() and values() use independent snapshots, as the old
     * dictionary did. Mutating a dictionary cannot invalidate its key cursor. */
    for (int64_t index = 0; index < dict->capacity; index++) {
        unsigned char *bucket = dict->data + index * dict->stride;
        if (bucket_hash(bucket) >= 2) {
            void *value = bucket + (keys ? sizeof(uint64_t) : (size_t)dict->value_offset);
            if (!at_list_append(list, value)) {
                at_gc_restore(frame);
                return NULL;
            }
        }
    }
    at_gc_restore(frame);
    return list;
}

AtList *at_dict_keys(AtDict *dict)
{
    return snapshot(dict, 1);
}

AtList *at_dict_values(AtDict *dict)
{
    return snapshot(dict, 0);
}

int at_dict_visit(AtDict *dict, AtDictVisit visit, void *context)
{
    for (int64_t index = 0; index < dict->capacity; index++) {
        unsigned char *bucket = dict->data + index * dict->stride;
        if (bucket_hash(bucket) >= 2 &&
            !visit(context, bucket + sizeof(uint64_t), bucket + dict->value_offset)) {
            return 0;
        }
    }
    return 1;
}
