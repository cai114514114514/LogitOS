/* SPDX-License-Identifier: GPL-3.0-or-later
 * An atom's address is its identity; the open-addressed index only stores that
 * address. Resizing the index must never move the bytes a DOM node retained.
 * A folded hash lives in the atom itself: case-insensitive lookup neither
 * allocates a second atom nor introduces a self-reference to break at teardown.
 */
#include "atom.h"
#include <stdlib.h>
#include <string.h>

static struct {
    lwc_string **slots;
    size_t capacity;
    size_t count;
    unsigned char lock;
} pool;

static void enter(void)
{
    while (__atomic_test_and_set(&pool.lock, __ATOMIC_ACQUIRE)) { }
}

static void leave(void)
{
    __atomic_clear(&pool.lock, __ATOMIC_RELEASE);
}

uint32_t atom_hash(const char *bytes, size_t length, bool folded)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)bytes[i];
        hash = (hash ^ (folded ? atom_fold(byte) : byte)) * 16777619u;
    }
    /* Mix the upper bits into the low bits used by power-of-two table sizes. */
    hash ^= hash >> 16;
    hash *= 0x85ebca6bu;
    return hash ^ (hash >> 13);
}

static size_t locate(const char *bytes, size_t length, uint32_t hash)
{
    size_t slot = hash & (pool.capacity - 1);
    while (pool.slots[slot]) {
        lwc_string *atom = pool.slots[slot];
        if (atom->exact_hash == hash && atom->length == length &&
            (length == 0 || memcmp(atom->bytes, bytes, length) == 0)) break;
        slot = (slot + 1) & (pool.capacity - 1);
    }
    return slot;
}

static bool expand(void)
{
    size_t capacity = pool.capacity ? pool.capacity * 2 : 128;
    if (capacity < pool.capacity || capacity > SIZE_MAX / sizeof(*pool.slots))
        return false;
    lwc_string **slots = calloc(capacity, sizeof(*slots));
    if (!slots) return false;
    for (size_t i = 0; i < pool.capacity; i++) {
        lwc_string *atom = pool.slots[i];
        if (!atom) continue;
        size_t slot = atom->exact_hash & (capacity - 1);
        while (slots[slot]) slot = (slot + 1) & (capacity - 1);
        slots[slot] = atom;
    }
    free(pool.slots);
    pool.slots = slots;
    pool.capacity = capacity;
    return true;
}

lwc_error lwc_intern_string(const char *bytes, size_t length, lwc_string **result)
{
    if (!result || (!bytes && length)) return lwc_error_range;
    *result = NULL;
    if (length > SIZE_MAX - sizeof(lwc_string) - 1) return lwc_error_oom;
    uint32_t hash = atom_hash(bytes, length, false);
    enter();
    if (!pool.capacity && !expand()) { leave(); return lwc_error_oom; }
    size_t slot = locate(bytes, length, hash);
    lwc_string *atom = pool.slots[slot];
    if (atom) {
        if (atom->references == UINT32_MAX) { leave(); return lwc_error_oom; }
        atom->references++;
    } else {
        if (pool.count >= pool.capacity - pool.capacity / 4) {
            if (!expand()) { leave(); return lwc_error_oom; }
            slot = locate(bytes, length, hash);
        }
        atom = malloc(sizeof(*atom) + length + 1);
        if (!atom) { leave(); return lwc_error_oom; }
        atom->length = length;
        atom->references = 1;
        atom->exact_hash = hash;
        atom->folded_hash = atom_hash(bytes, length, true);
        if (length) memcpy(atom->bytes, bytes, length);
        atom->bytes[length] = 0;
        pool.slots[slot] = atom;
        pool.count++;
    }
    *result = atom;
    leave();
    return lwc_error_ok;
}

lwc_string *lwc_string_ref(lwc_string *atom)
{
    if (!atom) return NULL;
    enter();
    if (atom->references == UINT32_MAX) { leave(); return NULL; }
    atom->references++;
    leave();
    return atom;
}

void lwc_string_unref(lwc_string *atom)
{
    if (!atom) return;
    enter();
    if (--atom->references) { leave(); return; }
    size_t mask = pool.capacity - 1;
    size_t hole = locate(atom->bytes, atom->length, atom->exact_hash);
    /* Shift only entries whose probe path crosses the hole. Simply clearing a
     * slot silently hides the rest of a collision cluster from future lookups. */
    for (size_t scan = (hole + 1) & mask; pool.slots[scan]; scan = (scan + 1) & mask) {
        size_t home = pool.slots[scan]->exact_hash & mask;
        if (((hole - home) & mask) < ((scan - home) & mask)) {
            pool.slots[hole] = pool.slots[scan];
            hole = scan;
        }
    }
    pool.slots[hole] = NULL;
    pool.count--;
    if (!pool.count) {
        free(pool.slots);
        pool.slots = NULL;
        pool.capacity = 0;
    }
    leave();
    free(atom);
}

void lwc_iterate_strings(lwc_iteration_callback_fn callback, void *context)
{
    if (!callback) return;
    enter();
    size_t count = pool.count;
    if (!count || count > SIZE_MAX / sizeof(lwc_string *)) { leave(); return; }
    lwc_string **snapshot = malloc(count * sizeof(*snapshot));
    if (!snapshot) { leave(); return; }
    size_t used = 0;
    for (size_t i = 0; i < pool.capacity; i++) {
        lwc_string *atom = pool.slots[i];
        if (!atom) continue;
        if (atom->references == UINT32_MAX) {
            while (used) snapshot[--used]->references--;
            free(snapshot);
            leave();
            return;
        }
        atom->references++;
        snapshot[used++] = atom;
    }
    leave();
    /* Callbacks may intern strings themselves. A retained snapshot avoids both
     * executing foreign code under our lock and traversing a resized index. */
    for (size_t i = 0; i < used; i++) {
        callback(snapshot[i], context);
        lwc_string_unref(snapshot[i]);
    }
    free(snapshot);
}
