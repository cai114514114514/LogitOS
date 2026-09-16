/* SPDX-License-Identifier: MIT */
/* Real files with faults confined to port.c: short transfers, interrupted
 * calls and failed cleanup must not be confused with successful ownership. */
#include "native.h"
#include "port.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum Fault {
    NORMAL,
    INTERRUPTED,
    READ_ERROR,
    WRITE_ERROR,
    ZERO_WRITE,
    CLOSE_ERROR,
    ALLOCATION_ERROR,
    GROW_ERROR,
    SECOND_ALLOCATION_ERROR,
    PIPE_ERROR
};
static enum Fault fault;
static int calls, closes;
static int allocations, pipe_calls;
static void *wrappers[8];
static int wrapper_count;

int probe_pipe(int descriptors[2])
{
    pipe_calls++;
    if (fault == PIPE_ERROR) {
        errno = EMFILE;
        return -1;
    }
    return pipe(descriptors);
}

ssize_t probe_read(int fd, void *data, size_t length)
{
    if (fault == INTERRUPTED && !calls++) {
        errno = EINTR;
        return -1;
    }
    if (fault == READ_ERROR) {
        errno = EIO;
        return -1;
    }
    return read(fd, data, length > 3 ? 3 : length);
}

ssize_t probe_write(int fd, const void *data, size_t length)
{
    if (fault == INTERRUPTED && !calls++) {
        errno = EINTR;
        return -1;
    }
    if (fault == WRITE_ERROR) {
        errno = EIO;
        return -1;
    }
    if (fault == ZERO_WRITE) {
        return 0;
    }
    return write(fd, data, length > 3 ? 3 : length);
}

int probe_close(int fd)
{
    closes++;
    int result = close(fd);
    if (fault == CLOSE_ERROR) {
        /* Model close consuming the fd before returning an error. Retrying
         * could close an unrelated reused fd, so release must call only once. */
        errno = EINTR;
        return -1;
    }
    return result;
}

void *probe_calloc(size_t count, size_t bytes)
{
    allocations++;
    if (fault == ALLOCATION_ERROR || (fault == SECOND_ALLOCATION_ERROR && allocations == 2)) {
        return NULL;
    }
    void *pointer = calloc(count, bytes);
    if (pointer) {
        assert(wrapper_count < (int)(sizeof wrappers / sizeof wrappers[0]));
        wrappers[wrapper_count++] = pointer;
    }
    return pointer;
}

void probe_free(void *pointer)
{
    /* Track only calloc-owned Port wrappers. Byte buffers also pass through
     * free here, but they have independent allocation and GC checks. */
    for (int i = 0; i < wrapper_count; i++) {
        if (wrappers[i] == pointer) {
            wrappers[i] = wrappers[--wrapper_count];
            break;
        }
    }
    free(pointer);
}

void *probe_realloc(void *pointer, size_t bytes)
{
    return fault == GROW_ERROR ? NULL : realloc(pointer, bytes);
}

static AtPort *acquire(const char *path, const char *mode)
{
    AtPort *port = NULL;
    assert(at_port_open(&port, path, strlen(path), mode, strlen(mode)) == 0);
    assert(port && at_port_live() == 1);
    return port;
}

static void release_once(AtPort *port, int status)
{
    int before = closes;
    AtPortCounters old, current;
    at_port_counters(&old);
    int descriptor = (int)at_port_fd(port);
    assert(at_port_close(port) == status);
    assert(at_port_closed(port) && at_port_fd(port) == -1);
    assert(fcntl(descriptor, F_GETFD) == -1 && errno == EBADF);
    assert(at_port_close(port) == 0);
    assert(at_port_release(port) == 0);
    assert(closes == before + 1 && at_port_live() == 0);
    at_port_counters(&current);
    assert(current.open == old.open - 1);
    assert(current.closed == old.closed + (status == 0));
    assert(current.close_errors == old.close_errors + (status != 0));
}

static void pipe_acquisition(void)
{
    AtPort *reader = NULL, *writer = NULL;
    at_caps_set(AS_CAP_FS_READ | AS_CAP_FS_WRITE, NULL);
    int before = pipe_calls;
    assert(at_port_pipe(&reader, &writer) == AT_E_PERMISSION);
    assert(!reader && !writer && pipe_calls == before);
    at_caps_init();

    enum Fault failures[] = {ALLOCATION_ERROR, SECOND_ALLOCATION_ERROR, PIPE_ERROR};

    for (unsigned i = 0; i < sizeof failures / sizeof failures[0]; i++) {
        fault = failures[i];
        allocations = 0;
        before = pipe_calls;
        assert(at_port_pipe(&reader, &writer) == (fault == PIPE_ERROR ? AT_E_IO : AT_E_MEMORY));
        assert(!reader && !writer && at_port_live() == 0);
        assert(wrapper_count == 0);
        assert(pipe_calls == before + (fault == PIPE_ERROR));
    }

    fault = NORMAL;
    assert(at_port_pipe(&reader, &writer) == 0);
    assert(reader && writer && at_port_live() == 2);
    int read_fd = (int)at_port_fd(reader);
    int write_fd = (int)at_port_fd(writer);
    int64_t written = 0;
    assert(at_port_write(&written, writer, "pipe", 4) == 0 && written == 4);
    assert(at_port_write(&written, reader, "x", 1) == AT_E_IO);
    AtBytes *bytes = NULL;
    assert(at_port_read(&bytes, writer, 1, 1) == AT_E_IO);
    assert(at_port_release(writer) == 0);
    assert(at_port_live() == 1);
    assert(fcntl(write_fd, F_GETFD) == -1 && errno == EBADF);
    assert(at_port_readall(&bytes, reader) == 0);
    assert(bytes->length == 4 && !memcmp(bytes->data, "pipe", 4));
    assert(at_port_release(reader) == 0 && at_port_live() == 0);
    assert(fcntl(read_fd, F_GETFD) == -1 && errno == EBADF);
    assert(wrapper_count == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    char path[4096], missing[4096];
    snprintf(path, sizeof path, "%s/runtime-port", argv[1]);
    snprintf(missing, sizeof missing, "%s/never-created", argv[1]);
    at_caps_init();
    const char *contents = "first\r\n\n中文\nlast";
    AtBytes *bytes = at_bytes_copy(contents, strlen(contents));
    int64_t written;
    assert(at_file_write(&written, path, strlen(path), bytes) == 0);

    for (fault = NORMAL; fault <= GROW_ERROR; fault++) {
        AtPort *port = NULL;
        calls = 0;
        if (fault == ALLOCATION_ERROR) {
            assert(at_port_open(&port, path, strlen(path), "r", 1) == AT_E_MEMORY);
            assert(!port && at_port_live() == 0);
            continue;
        }
        port = acquire(path, "r");
        AtBytes *result = NULL;
        int status = at_port_readall(&result, port);
        if (fault == READ_ERROR || fault == GROW_ERROR) {
            assert(status == (fault == GROW_ERROR ? AT_E_MEMORY : AT_E_IO));
            assert(result == NULL);
        } else {
            assert(status == 0 && result->length == (int64_t)strlen(contents));
            assert(!memcmp(result->data, contents, strlen(contents)));
        }
        release_once(port, fault == CLOSE_ERROR ? AT_E_IO : 0);
        calls = 0;
        port = acquire(missing, "w");
        status = at_port_write(&written, port, contents, strlen(contents));
        if (fault == WRITE_ERROR || fault == ZERO_WRITE) {
            assert(status == AT_E_IO && written == 0);
        } else {
            assert(status == 0 && written == (int64_t)strlen(contents));
            AtBytes *actual = NULL;
            assert(at_file_read(&actual, missing, strlen(missing)) == 0);
            assert(actual->length == written && !memcmp(actual->data, contents, written));
        }
        release_once(port, fault == CLOSE_ERROR ? AT_E_IO : 0);
    }

    fault = NORMAL;
    AtPort *port = acquire(path, "r");
    AtBytes *data = NULL;
    assert(at_port_read(&data, port, 0, 0) == AT_E_VALUE);
    assert(at_port_read(&data, port, AT_BUFFER_LIMIT + 1, 0) == AT_E_VALUE);
    assert(at_port_read(&data, port, 5, 1) == 0);
    assert(data->length == 5 && !memcmp(data->data, "first", 5));
    AtNativeText line;
    int32_t present;
    assert(at_port_line(&line, &present, port) == 0 && present && line.length == 0);
    assert(at_port_line(&line, &present, port) == 0 && present && line.length == 0);
    assert(at_port_line(&line, &present, port) == 0 && present && line.length == 6);
    assert(at_port_line(&line, &present, port) == 0 && present && line.length == 4);
    assert(at_port_line(&line, &present, port) == 0 && !present);
    assert(at_port_read(&data, port, 1, 1) == AT_E_IO && !data);
    assert(at_port_read(&data, port, 1, 0) == 0 && !data);
    assert(at_port_write(&written, port, "x", 1) == AT_E_IO);
    release_once(port, 0);

    /* Permission requirements come from modes, including BOTH bits for rw.
     * This exercises the shared real acquisition helper through the Port API. */
    at_caps_set(AS_CAP_FS_READ, NULL);
    assert(at_port_open(&port, path, strlen(path), "rw", 2) == AT_E_PERMISSION && !port);
    port = acquire(path, "r");
    release_once(port, 0);
    at_caps_set(AS_CAP_FS_WRITE, NULL);
    assert(at_port_open(&port, path, strlen(path), "r", 1) == AT_E_PERMISSION && !port);
    at_caps_init();
    assert(at_port_open(&port, path, strlen(path), "invalid", 7) == AT_E_VALUE && !port);
    unlink(missing);
    assert(at_port_open(&port, missing, strlen(missing), "r+b", 3) == AT_E_IO && !port);
    assert(access(missing, F_OK) != 0);
    port = acquire(missing, "rw");
    release_once(port, 0);

    bytes = at_bytes_copy("\xff\n", 2);
    assert(at_file_write(&written, path, strlen(path), bytes) == 0);
    port = acquire(path, "r");
    assert(at_port_line(&line, &present, port) == AT_E_CONVERSION && !present);
    release_once(port, 0);
    assert(at_port_live() == 0);
    pipe_acquisition();
    at_gc_collect();
    assert(at_gc_live_bytes() == 0);
    return 0;
}
