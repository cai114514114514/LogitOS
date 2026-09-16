/* SPDX-License-Identifier: MIT */
/* Fault injection at the OS boundary: each open must have exactly one close,
 * short transfers must finish, and failure must not publish a partial result. */
#include "native.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

enum {
    NORMAL,
    INTERRUPTED,
    READ_ERROR,
    WRITE_ERROR,
    CLOSE_ERROR,
    ALLOCATION_ERROR,
    ZERO_WRITE
};

static int mode;
static int active;
static int closes;
static int calls;
static size_t position;
static unsigned char received[32];
static const unsigned char contents[] = {0, 255, 128, 1, 2, 3, 4, 5, 6};

int probe_open(const char *path, int flags, ...)
{
    (void)path;
    (void)flags;
    assert(!active);
    active = 1;
    position = 0;
    calls = 0;
    return 42;
}

int probe_openat(int directory, const char *path, int flags, ...)
{
    (void)directory;
    return probe_open(path, flags);
}

ssize_t probe_read(int descriptor, void *data, size_t length)
{
    assert(descriptor == 42 && active && length >= 3);
    if (mode == INTERRUPTED && !calls++) {
        errno = EINTR;
        return -1;
    }
    if (mode == READ_ERROR && position >= 3) {
        errno = EIO;
        return -1;
    }
    size_t remaining = sizeof contents - position;
    size_t count = remaining < 3 ? remaining : 3;
    memcpy(data, contents + position, count);
    position += count;
    return (ssize_t)count;
}

ssize_t probe_write(int descriptor, const void *data, size_t length)
{
    assert(descriptor == 42 && active);
    if (mode == ZERO_WRITE) {
        return 0;
    }
    if (mode == INTERRUPTED && !calls++) {
        errno = EINTR;
        return -1;
    }
    if (mode == WRITE_ERROR && position >= 3) {
        errno = EIO;
        return -1;
    }
    size_t count = length < 3 ? length : 3;
    assert(position + count <= sizeof received);
    memcpy(received + position, data, count);
    position += count;
    return (ssize_t)count;
}

int probe_close(int descriptor)
{
    assert(descriptor == 42 && active);
    active = 0;
    closes++;
    if (mode == CLOSE_ERROR) {
        errno = EIO;
        return -1;
    }
    return 0;
}

void *probe_realloc(void *pointer, size_t bytes)
{
    return mode == ALLOCATION_ERROR ? NULL : realloc(pointer, bytes);
}

int main(void)
{
    at_caps_init();
    for (mode = NORMAL; mode <= ZERO_WRITE; mode++) {
        int before = closes;
        AtBytes *result = NULL;
        int status = at_file_read(&result, "/data", 5);
        assert(!active && closes == before + 1);
        if (mode == READ_ERROR || mode == CLOSE_ERROR || mode == ALLOCATION_ERROR) {
            assert(status == (mode == ALLOCATION_ERROR ? AT_E_MEMORY : AT_E_IO));
            assert(result == NULL);
        } else {
            assert(status == 0 && result->length == sizeof contents);
            assert(!memcmp(result->data, contents, sizeof contents));
        }
        AtBytes *data = at_bytes_copy(contents, sizeof contents);
        before = closes;
        int64_t written = -1;
        status = at_file_write(&written, "/data", 5, data);
        assert(!active && closes == before + 1);
        if (mode == WRITE_ERROR || mode == CLOSE_ERROR || mode == ZERO_WRITE) {
            assert(status == AT_E_IO && written == 0);
        } else {
            assert(status == 0 && written == sizeof contents);
            assert(position == sizeof contents);
            assert(!memcmp(received, contents, sizeof contents));
        }
    }
    at_gc_collect();
    assert(at_gc_live_bytes() == 0);
    return 0;
}
